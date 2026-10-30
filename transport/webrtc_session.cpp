#include "webrtc_session.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <variant>
#include <vector>

#include <rtc/rtc.hpp>

namespace launcher::transport {
namespace {

using Json = nlohmann::json;

Json field(const Json &object, const char *name)
{
    const auto found = object.find(name);
    return found == object.end() ? Json() : *found;
}

bool integer(const Json &value, std::uint64_t minimum, std::uint64_t maximum)
{
    if (!value.is_number_integer())
        return false;
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        return number >= minimum && number <= maximum;
    }
    const auto number = value.get<std::int64_t>();
    return number >= 0 && static_cast<std::uint64_t>(number) >= minimum
                       && static_cast<std::uint64_t>(number) <= maximum;
}

bool identifier(const Json &value)
{
    if (!value.is_string())
        return false;
    const auto &text = value.get_ref<const std::string &>();
    return !text.empty() && text.size() <= 128
        && std::all_of(text.begin(), text.end(), [](unsigned char character) {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9') || character == '_'
                || character == '.' || character == '-';
        });
}

bool digest(const Json &value)
{
    if (!value.is_string())
        return false;
    const auto &text = value.get_ref<const std::string &>();
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

Json parse_control(const std::string &message)
{
    try {
        return Json::parse(message, [](int depth, Json::parse_event_t, Json &) {
            if (depth > 64)
                throw ProtocolError("Malformed file control message.");
            return true;
        });
    } catch (const std::exception &) {
        throw ProtocolError("Malformed file control message.");
    }
}

void append_u64(std::vector<std::uint8_t> &payload, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        payload.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::vector<std::uint8_t> bytes(const std::string &text)
{
    return {text.begin(), text.end()};
}

}

std::string host_only_sdp(const nlohmann::json &description)
{
    if (!description.is_string())
        throw ProtocolError("Invalid or oversized SDP.");
    const auto &sdp = description.get_ref<const std::string &>();
    if (sdp.empty() || sdp.size() > kMaxSdpSize)
        throw ProtocolError("Invalid or oversized SDP.");
    std::vector<std::string> media;
    std::size_t candidates = 0;
    std::istringstream lines(sdp);
    std::string line;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            continue;
        line.erase(0, first);
        if (line.compare(0, 2, "m=") == 0) {
            std::istringstream tokens(line);
            std::string kind;
            tokens >> kind;
            media.push_back(kind);
        }
        if (line.compare(0, 12, "a=candidate:") == 0) {
            std::istringstream tokens(line);
            std::vector<std::string> fields;
            std::string token;
            while (tokens >> token)
                fields.push_back(std::move(token));
            if (fields.size() < 8 || fields[6] != "typ" || fields[7] != "host")
                throw ProtocolError("Only local host ICE candidates are supported.");
            ++candidates;
        }
    }
    if (media != std::vector<std::string>{"m=application"} || candidates == 0)
        throw ProtocolError("A complete local DataChannel offer is required.");
    return sdp;
}

struct Session::Impl : std::enable_shared_from_this<Impl> {
    struct Incoming {
        std::uint64_t approval_id;
        std::string file_id;
        std::string name;
        std::uint64_t size;
        std::size_t chunk_size;
        std::string digest;
        std::uint64_t received = 0;
        std::uint64_t chunks = 0;
        bool awaiting_approval = true;
        Clock::time_point approval_deadline{};
    };

    Storage &storage;
    Signal send_signal;
    Dispatch dispatch;
    Now now;
    std::recursive_mutex state_mutex;
    std::mutex queue_mutex;
    std::deque<rtc::message_variant> queue;
    bool drain_pending = false;
    std::atomic<bool> overflowing{false};
    std::atomic<bool> is_closed{false};
    std::atomic<bool> is_connected{false};
    std::atomic<bool> has_channel{false};
    std::atomic<bool> accepting_binary{false};
    std::string session_id;
    bool paired = false;
    bool answer_sent = false;
    bool discarding = false;
    std::uint64_t next_approval_id = 1;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> channel;
    std::optional<Incoming> incoming;

    Impl(Storage &storage_value, Signal signal_value, Dispatch dispatch_value, Now now_value)
        : storage(storage_value), send_signal(std::move(signal_value)), dispatch(std::move(dispatch_value)),
          now(std::move(now_value))
    {
        if (!dispatch)
            dispatch = [](std::function<void()> operation) { operation(); };
        if (!now)
            now = [] { return Clock::now(); };
    }

    void post(std::function<void(Impl &)> operation)
    {
        const std::weak_ptr<Impl> weak = weak_from_this();
        dispatch([weak, operation = std::move(operation)] {
            const auto self = weak.lock();
            if (!self)
                return;
            std::lock_guard<std::recursive_mutex> lock(self->state_mutex);
            if (self->is_closed)
                return;
            try {
                operation(*self);
            } catch (const std::exception &error) {
                std::fprintf(stderr, "File-sharing transport operation failed: %s\n", error.what());
                self->close_locked();
            }
        });
    }

    void signal(const char *kind, Json payload)
    {
        if (!session_id.empty())
            send_signal({{"type", kind}, {"sessionId", session_id},
                         {"protocolVersion", kProtocolVersion}, {"payload", std::move(payload)}});
    }

    void signal_error(const char *kind, const std::string &message, Json details = Json::object())
    {
        details["code"] = kind;
        details["message"] = message;
        signal("session.error", std::move(details));
    }

    void status(const std::string &message)
    {
        storage.request('S', bytes(message.substr(0, 1024)));
    }

    void handle_signal(const Json &envelope)
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        if (is_closed)
            return;
        if (!envelope.is_object())
            throw ProtocolError("A signaling object is required.");
        const auto version = field(envelope, "protocolVersion");
        if (!integer(version, kProtocolVersion, kProtocolVersion))
            throw ProtocolError("Unsupported signaling protocol version.");
        const auto identifier_value = field(envelope, "sessionId");
        if (!identifier(identifier_value))
            throw ProtocolError("Invalid session ID.");
        const auto payload = field(envelope, "payload");
        if (!payload.is_object())
            throw ProtocolError("A signaling payload object is required.");
        const auto kind = field(envelope, "type");
        if (session_id.empty()) {
            if (kind != "session.hello")
                throw ProtocolError("Begin with session.hello.");
            session_id = identifier_value.get<std::string>();
            // Authorize each individual file on the device before accepting
            // its bytes; connecting to the radio requires no separate code.
            paired = true;
        }
        if (identifier_value != session_id)
            throw ProtocolError("The BLE connection already belongs to another session.");
        if (kind == "session.hello") {
            signal("session.accepted", {{"pairingRequired", false}, {"transport", "ble-webrtc"},
                                        {"maxChunkSize", kMaxChunkSize}, {"maxFileSize", kMaxFileSize}});
        } else if (kind == "session.close") {
            close_locked();
        } else if (kind == "sdp.offer") {
            if (!paired) {
                signal_error("connection-required", "Connect to the device before sending an offer.");
                return;
            }
            if (peer) {
                signal_error("already-connected", "This session already has a peer connection.");
                return;
            }
            try {
                accept_offer(payload);
            } catch (const std::exception &error) {
                signal_error("negotiation-failed", error.what());
                close_locked();
            }
        } else {
            signal_error("unsupported-message", "Use a complete SDP offer after connecting.");
        }
    }

    void accept_offer(const Json &payload)
    {
        const auto description = payload.contains("description") ? payload["description"] : payload;
        if (!description.is_object() || field(description, "type") != "offer")
            throw ProtocolError("An SDP offer is required.");
        const auto sdp = host_only_sdp(field(description, "sdp"));
        rtc::Configuration configuration;
        configuration.disableAutoNegotiation = true;
        configuration.maxMessageSize = kMaxChunkSize + 5;
        peer = std::make_shared<rtc::PeerConnection>(configuration);
        const std::weak_ptr<Impl> weak = weak_from_this();
        peer->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> value) {
            if (const auto self = weak.lock())
                self->post([value = std::move(value)](Impl &state) { state.bind_channel(value); });
            else
                value->close();
        });
        peer->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete)
                if (const auto self = weak.lock())
                    self->post([](Impl &value) { value.send_answer(); });
        });
        peer->onStateChange([weak](rtc::PeerConnection::State state) {
            if (const auto self = weak.lock()) {
                self->post([state](Impl &value) {
                    if (state == rtc::PeerConnection::State::Connected)
                        value.status("Connected locally. Ready to receive files.");
                    else if (state == rtc::PeerConnection::State::Failed
                             || state == rtc::PeerConnection::State::Closed
                             || state == rtc::PeerConnection::State::Disconnected)
                        value.connection_failed();
                });
            }
        });
        peer->setRemoteDescription(rtc::Description(sdp, "offer"));
        peer->setLocalDescription(rtc::Description::Type::Answer);
        if (peer->gatheringState() == rtc::PeerConnection::GatheringState::Complete)
            send_answer();
    }

    void send_answer()
    {
        if (answer_sent || !peer)
            return;
        const auto description = peer->localDescription();
        if (!description)
            throw ProtocolError("The local peer did not provide an SDP answer.");
        const auto sdp = host_only_sdp(std::string(*description));
        answer_sent = true;
        signal("sdp.answer", {{"description", {{"type", "answer"}, {"sdp", sdp}}}});
    }

    void connection_failed()
    {
        try {
            signal_error("connection-failed", "The local connection was lost.");
        } catch (const std::exception &) {
        }
        close_locked("The local connection was lost. Reopen file sharing to reconnect.");
    }

    void bind_channel(const std::shared_ptr<rtc::DataChannel> &value)
    {
        const auto reliability = value->reliability();
        if (!paired || channel || value->label() != "file-share" || reliability.unordered
            || reliability.maxRetransmits || reliability.maxPacketLifeTime) {
            value->close();
            connection_failed();
            return;
        }
        channel = value;
        has_channel = true;
        const std::weak_ptr<Impl> weak = weak_from_this();
        value->onOpen([weak] {
            if (const auto self = weak.lock())
                self->post([](Impl &state) { state.is_connected = state.channel && state.channel->isOpen(); });
        });
        value->onClosed([weak] {
            if (const auto self = weak.lock())
                self->post([](Impl &state) { state.connection_failed(); });
        });
        value->onError([weak](const std::string &) {
            if (const auto self = weak.lock())
                self->post([](Impl &state) { state.connection_failed(); });
        });
        value->onMessage([weak](rtc::message_variant message) {
            if (const auto self = weak.lock())
                self->enqueue(std::move(message));
        });
        if (!is_closed)
            is_connected = value->isOpen();
    }

    void enqueue(rtc::message_variant message)
    {
        if (is_closed || overflowing)
            return;
        const auto *text = std::get_if<std::string>(&message);
        const auto size = text ? text->size() : std::get<rtc::binary>(message).size();
        const auto limit = text ? kMaxControlSize : kMaxChunkSize + 4;
        // Do not queue bytes sent before the user has approved the file.
        if (!text && size <= limit && !accepting_binary)
            return;
        bool schedule = false;
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (size > limit || queue.size() >= kMaxPendingMessages) {
                overflow = !overflowing.exchange(true);
            } else if (!overflowing && !is_closed) {
                queue.push_back(std::move(message));
                schedule = !drain_pending;
                drain_pending = true;
            }
        }
        if (overflow)
            post([](Impl &state) {
                state.send_data({{"kind", "file-error"}, {"fileId", state.file_id()},
                                 {"message", "The sender exceeded the receive window or message size."}});
                state.close_locked();
            });
        else if (schedule)
            post([](Impl &state) { state.drain(); });
    }

    void clear_queue()
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        queue.clear();
        drain_pending = false;
    }

    void drain()
    {
        while (!is_closed && !overflowing) {
            rtc::message_variant message;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (queue.empty()) {
                    drain_pending = false;
                    return;
                }
                message = std::move(queue.front());
                queue.pop_front();
            }
            try {
                receive(message);
            } catch (const std::exception &error) {
                const auto announced = file_id().empty() ? announced_file_id(message) : file_id();
                incoming.reset();
                accepting_binary = false;
                discarding = true;
                clear_queue();
                try {
                    storage.request('C');
                } catch (const std::exception &storage_error) {
                    std::fprintf(stderr, "Unable to cancel failed incoming file: %s\n", storage_error.what());
                }
                send_data({{"kind", "file-error"}, {"fileId", announced}, {"message", error.what()}});
            }
        }
    }

    void send_data(Json payload)
    {
        if (channel && channel->isOpen())
            channel->send(payload.dump());
    }

    std::string file_id() const
    {
        return incoming ? incoming->file_id : std::string();
    }

    static std::string announced_file_id(const rtc::message_variant &data)
    {
        const auto *text = std::get_if<std::string>(&data);
        if (!text)
            return {};
        try {
            const auto message = parse_control(*text);
            if (!message.is_object())
                return {};
            const auto candidate = field(message, "kind") == "file-start" ? field(message, "metadata") : message;
            if (candidate.is_object() && identifier(field(candidate, "fileId")))
                return candidate["fileId"].get<std::string>();
        } catch (const std::exception &) {
        }
        return {};
    }

    void receive(const rtc::message_variant &data)
    {
        if (const auto *packet = std::get_if<rtc::binary>(&data)) {
            if (!discarding && incoming && !incoming->awaiting_approval)
                receive_chunk(*packet);
            return;
        }
        const auto message = parse_control(std::get<std::string>(data));
        if (!message.is_object())
            throw ProtocolError("A file control object is required.");
        const auto kind = field(message, "kind");
        if (kind == "file-start") {
            start_file(field(message, "metadata"));
        } else if (kind == "file-cancel" || kind == "file-error") {
            if (incoming && field(message, "fileId") != incoming->file_id)
                throw ProtocolError("Cancellation does not match the active file.");
            if (incoming)
                storage.request('C');
            incoming.reset();
            accepting_binary = false;
            discarding = true;
            send_data({{"kind", "file-cancelled"},
                       {"fileId", message.contains("fileId") ? message["fileId"] : Json("")}});
        } else if (kind == "file-complete") {
            if (!discarding)
                finish_file(message);
        } else {
            throw ProtocolError("Unknown file control message.");
        }
    }

    void start_file(const Json &metadata)
    {
        if (incoming)
            throw ProtocolError("Finish or cancel the active file before starting another.");
        if (!metadata.is_object())
            throw ProtocolError("File metadata is required.");
        if (field(metadata, "sessionId") != session_id || !identifier(field(metadata, "fileId")))
            throw ProtocolError("Invalid file session or file ID.");
        const auto name_value = field(metadata, "fileName");
        if (!name_value.is_string())
            throw ProtocolError("The file name must be a single safe path component.");
        const auto name = name_value.get<std::string>();
        if (name.empty() || name == "." || name == ".." || name.size() > 255
            || std::any_of(name.begin(), name.end(), [](unsigned char character) {
                return character == '/' || character == '\\' || character < 32 || character == 127;
            }))
            throw ProtocolError("The file name must be a single safe path component.");
        if (!integer(field(metadata, "fileSize"), 0, kMaxFileSize))
            throw ProtocolError("The file exceeds the 4 GiB receiver limit.");
        if (!integer(field(metadata, "chunkSize"), 1, kMaxChunkSize))
            throw ProtocolError("The chunk size must be between 1 and 16384 bytes.");
        if (!digest(field(metadata, "sha256")))
            throw ProtocolError("A lowercase SHA-256 digest is required.");
        const std::string file_id = metadata["fileId"].get<std::string>();
        const std::uint64_t size = metadata["fileSize"].get<std::uint64_t>();
        const std::string digest_value = metadata["sha256"].get<std::string>();
        if (next_approval_id == 0)
            throw ProtocolError("The file confirmation sequence is exhausted.");
        const std::uint64_t approval_id = next_approval_id++;
        std::vector<std::uint8_t> approval_payload;
        append_u64(approval_payload, approval_id);
        append_u64(approval_payload, size);
        approval_payload.insert(approval_payload.end(), name.begin(), name.end());
        // Register the prompt without blocking DataChannel cancellation or close.
        storage.request('P', approval_payload);
        incoming = Incoming{approval_id, file_id, name, size,
                            metadata["chunkSize"].get<std::size_t>(), digest_value};
        incoming->approval_deadline = now() + std::chrono::seconds(45);
        discarding = false;
        accepting_binary = false;
        send_data({{"kind", "file-pending"}, {"fileId", file_id}, {"fileName", name}, {"fileSize", size}});
    }

    void poll_approval()
    {
        if (is_closed || !incoming || !incoming->awaiting_approval)
            return;
        try {
            if (now() >= incoming->approval_deadline)
                throw ProtocolError("approval-timeout: Confirmation timed out.");
            std::vector<std::uint8_t> query;
            append_u64(query, incoming->approval_id);
            const auto decision = storage.request('Q', query);
            if (decision == "pending")
                return;
            if (decision != "approved")
                throw ProtocolError("Invalid device confirmation response.");
            std::vector<std::uint8_t> payload;
            append_u64(payload, incoming->approval_id);
            append_u64(payload, incoming->size);
            payload.insert(payload.end(), incoming->digest.begin(), incoming->digest.end());
            payload.insert(payload.end(), incoming->name.begin(), incoming->name.end());
            storage.request('B', payload);
            incoming->awaiting_approval = false;
            accepting_binary = true;
            send_data({{"kind", "file-accepted"}, {"fileId", incoming->file_id}});
        } catch (const std::exception &error) {
            const auto rejected_id = file_id();
            incoming.reset();
            accepting_binary = false;
            discarding = true;
            try { storage.request('C'); } catch (const std::exception &) {}
            send_data({{"kind", "file-error"}, {"fileId", rejected_id}, {"message", error.what()}});
        }
    }

    void receive_chunk(const rtc::binary &packet)
    {
        if (!incoming)
            throw ProtocolError("Start a file before sending data.");
        if (packet.size() < 5)
            throw ProtocolError("A binary file chunk requires a sequence and data.");
        const auto octet = [&packet](std::size_t offset) { return std::to_integer<std::uint32_t>(packet[offset]); };
        const auto sequence = (octet(0) << 24) | (octet(1) << 16) | (octet(2) << 8) | octet(3);
        const auto expected = std::min<std::uint64_t>(incoming->chunk_size, incoming->size - incoming->received);
        if (sequence != incoming->chunks || packet.size() - 4 != expected)
            throw ProtocolError("Unexpected file chunk sequence or size.");
        std::vector<std::uint8_t> payload;
        append_u64(payload, sequence);
        for (auto offset = std::size_t(4); offset < packet.size(); ++offset)
            payload.push_back(std::to_integer<std::uint8_t>(packet[offset]));
        storage.request('D', payload);
        incoming->received += packet.size() - 4;
        ++incoming->chunks;
        if (incoming->chunks % kAckWindow == 0 || incoming->received == incoming->size)
            send_data({{"kind", "file-progress"}, {"fileId", incoming->file_id},
                       {"chunks", incoming->chunks}, {"receivedBytes", incoming->received}});
    }

    void finish_file(const Json &message)
    {
        if (!incoming || incoming->awaiting_approval)
            throw ProtocolError("No approved file is awaiting completion.");
        const auto chunks = field(message, "chunks");
        if (field(message, "fileId") != incoming->file_id || !integer(chunks, 0, 1ULL << 32)
            || chunks.get<std::uint64_t>() != incoming->chunks || incoming->received != incoming->size)
            throw ProtocolError("File completion does not match the received data.");
        const auto final_name = storage.request('F');
        const auto completed = *incoming;
        incoming.reset();
        accepting_binary = false;
        send_data({{"kind", "file-received"}, {"fileId", completed.file_id},
                   {"fileName", final_name}, {"size", completed.size}, {"sha256", completed.digest}});
    }

    void close_locked(const std::string &reason = {})
    {
        if (is_closed.exchange(true))
            return;
        is_connected = false;
        has_channel = false;
        accepting_binary = false;
        clear_queue();
        incoming.reset();
        try {
            storage.request('C');
        } catch (const std::exception &) {
        }
        if (!reason.empty()) {
            try {
                status(reason);
            } catch (const std::exception &) {
            }
        }
        if (channel) {
            try {
                channel->resetCallbacks();
                channel->close();
            } catch (const std::exception &error) {
                std::fprintf(stderr, "Unable to close the file channel: %s\n", error.what());
            }
            channel.reset();
        }
        if (peer) {
            try {
                peer->resetCallbacks();
                peer->close();
            } catch (const std::exception &error) {
                std::fprintf(stderr, "Unable to close the local peer: %s\n", error.what());
            }
            peer.reset();
        }
    }
};

Session::Session(Storage &storage, Signal send_signal, Dispatch dispatch, Now now)
    : impl_(std::make_shared<Impl>(storage, std::move(send_signal), std::move(dispatch), std::move(now)))
{
}

Session::~Session()
{
    close();
}

void Session::handle_signal(const Json &envelope)
{
    impl_->handle_signal(envelope);
}

void Session::tick()
{
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    impl_->poll_approval();
}

bool Session::connected() const
{
    return !impl_->is_closed && impl_->is_connected;
}

bool Session::channel_pending() const
{
    return !impl_->is_closed && impl_->has_channel;
}

bool Session::closed() const
{
    return impl_->is_closed;
}

void Session::close()
{
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    impl_->close_locked();
}

}
