#include "ble_framing.h"
#include "storage_ipc.h"
#include "webrtc_session.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/sha.h>
#include <rtc/rtc.hpp>
#include <sys/socket.h>
#include <unistd.h>

using namespace launcher::transport;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<class Operation> void rejects(Operation operation)
{
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "The invalid operation was accepted");
}

std::string sha256(const std::string &data)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest);
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string text;
    for (const auto octet : digest) {
        text.push_back(hexadecimal[octet >> 4]);
        text.push_back(hexadecimal[octet & 15]);
    }
    return text;
}

bool read_exact(int descriptor, std::uint8_t *data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = recv(descriptor, data + offset, size - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool write_exact(int descriptor, const std::vector<std::uint8_t> &data)
{
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = send(descriptor, data.data() + offset, data.size() - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

std::vector<std::uint8_t> read_packet(int descriptor)
{
    std::uint8_t header[4];
    if (!read_exact(descriptor, header, 4))
        return {};
    const auto size = (std::uint32_t(header[0]) << 24) | (std::uint32_t(header[1]) << 16)
                    | (std::uint32_t(header[2]) << 8) | header[3];
    require(size <= 1024 * 1024 + 9, "Oversized storage request");
    std::vector<std::uint8_t> packet(size);
    return read_exact(descriptor, packet.data(), packet.size()) ? packet : std::vector<std::uint8_t>();
}

bool reply(int descriptor, bool success, const std::string &text = {})
{
    const auto size = static_cast<std::uint32_t>(text.size() + 2);
    std::vector<std::uint8_t> packet{static_cast<std::uint8_t>(size >> 24),
                                     static_cast<std::uint8_t>(size >> 16),
                                     static_cast<std::uint8_t>(size >> 8),
                                     static_cast<std::uint8_t>(size), 'R',
                                     static_cast<std::uint8_t>(success)};
    packet.insert(packet.end(), text.begin(), text.end());
    return write_exact(descriptor, packet);
}

std::uint64_t u64(const std::vector<std::uint8_t> &packet, std::size_t offset)
{
    require(offset + 8 <= packet.size(), "Short storage request");
    std::uint64_t value = 0;
    for (std::size_t index = offset; index < offset + 8; ++index)
        value = (value << 8) | packet[index];
    return value;
}

class MemoryStorage {
public:
    struct File {
        std::uint64_t size;
        std::string digest;
        std::string name;
        std::string data;
        std::uint64_t next = 0;
    };

    MemoryStorage()
    {
        int sockets[2];
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
        client = std::make_unique<Storage>(sockets[0]);
        server_ = sockets[1];
        worker_ = std::thread([this] { run(); });
    }

    ~MemoryStorage()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing_ = true;
        }
        approval_changed_.notify_all();
        client->close();
        shutdown(server_, SHUT_RDWR);
        worker_.join();
        ::close(server_);
    }

    bool pending()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return incoming_.has_value();
    }

    std::vector<File> committed()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return committed_;
    }

    std::vector<char> commands()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    void fail(char command)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failing_ = command;
    }

    void hold_approval()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        approval_decision_.reset();
    }

    void wait_for_approval()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        require(approval_changed_.wait_for(lock, 5s, [this] { return awaiting_approval_; }),
                "File approval was not requested");
        require(!incoming_, "Destination file was opened before approval");
    }

    void decide_approval(bool accepted)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            approval_decision_ = accepted;
        }
        approval_changed_.notify_all();
    }

    std::unique_ptr<Storage> client;

private:
    void run()
    {
        while (true) {
            const auto packet = read_packet(server_);
            if (packet.empty())
                return;
            bool success = true;
            std::string response;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                const char command = static_cast<char>(packet[0]);
                commands_.push_back(command);
                try {
                    if (command == failing_)
                        throw std::runtime_error("storage rejected operation");
                    if (command == 'P') {
                        approved_.reset();
                        require(!incoming_ && packet.size() >= 18, "invalid approval request");
                        const auto id = u64(packet, 1);
                        const auto size = u64(packet, 9);
                        const std::string name(packet.begin() + 17, packet.end());
                        requested_ = Approval{id, size, name};
                        awaiting_approval_ = true;
                        response = "pending";
                        approval_changed_.notify_all();
                    } else if (command == 'Q') {
                        require(packet.size() == 9 && requested_ && u64(packet, 1) == requested_->id,
                                "invalid approval query");
                        if (!approval_decision_) {
                            response = "pending";
                        } else {
                            awaiting_approval_ = false;
                            const auto decision = *approval_decision_;
                            if (decision) approved_ = requested_;
                            requested_.reset();
                            require(decision, "user-rejected: File receiving was not approved.");
                            response = "approved";
                        }
                    } else if (command == 'B') {
                        require(!incoming_, "already receiving");
                        require(packet.size() >= 82 && approved_ && u64(packet, 1) == approved_->id &&
                                u64(packet, 9) == approved_->size &&
                                std::string(packet.begin() + 81, packet.end()) == approved_->name,
                                "unapproved begin packet");
                        approved_.reset();
                        incoming_ = File{u64(packet, 9), {packet.begin() + 17, packet.begin() + 81},
                                         {packet.begin() + 81, packet.end()}, {}, 0};
                    } else if (command == 'D') {
                        require(incoming_ && u64(packet, 1) == incoming_->next, "bad storage sequence");
                        ++incoming_->next;
                        incoming_->data.append(packet.begin() + 9, packet.end());
                    } else if (command == 'F') {
                        require(incoming_.has_value(), "no active file");
                        require(incoming_->data.size() == incoming_->size, "size mismatch");
                        require(sha256(incoming_->data) == incoming_->digest, "digest mismatch");
                        response = incoming_->name;
                        committed_.push_back(*incoming_);
                        incoming_.reset();
                    } else if (command == 'C') {
                        requested_.reset();
                        awaiting_approval_ = false;
                        approved_.reset();
                        incoming_.reset();
                    } else {
                        require(command == 'S', "unknown storage command");
                    }
                } catch (const std::exception &error) {
                    success = false;
                    response = error.what();
                }
            }
            if (!reply(server_, success, response))
                return;
        }
    }

    int server_ = -1;
    std::thread worker_;
    std::mutex mutex_;
    struct Approval { std::uint64_t id; std::uint64_t size; std::string name; };
    std::optional<Approval> approved_;
    std::optional<Approval> requested_;
    std::optional<bool> approval_decision_ = true;
    std::condition_variable approval_changed_;
    bool awaiting_approval_ = false;
    bool closing_ = false;
    std::optional<File> incoming_;
    std::vector<File> committed_;
    std::vector<char> commands_;
    char failing_ = 0;
};

class Executor {
public:
    Executor() : worker_([this] { run(); }) {}
    ~Executor()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_one();
        worker_.join();
    }

    void post(std::function<void()> operation)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            operations_.push_back(std::move(operation));
        }
        changed_.notify_one();
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            changed_.wait(lock, [this] { return stopping_ || !operations_.empty(); });
            if (operations_.empty())
                return;
            auto operation = std::move(operations_.front());
            operations_.pop_front();
            lock.unlock();
            operation();
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> operations_;
    bool stopping_ = false;
    std::thread worker_;
};

Json envelope(const std::string &kind, Json payload = Json::object(),
              const std::string &session_id = "browser-session")
{
    return {{"type", kind}, {"protocolVersion", 1}, {"sessionId", session_id}, {"payload", payload}};
}

Json metadata(const std::string &data, const std::string &file_id = "file-1",
              std::size_t chunk_size = kMaxChunkSize)
{
    return {{"sessionId", "browser-session"}, {"fileId", file_id}, {"fileName", "测试.txt"},
            {"fileSize", data.size()}, {"chunkSize", chunk_size}, {"sha256", sha256(data)}};
}

void test_framing()
{
    std::string text;
    for (int repeat = 0; repeat < 2000; ++repeat)
        text += "设备📁";
    const Json message{{"type", "sdp.offer"}, {"payload", {{"sdp", text}}}};
    const auto frames = encode_message(message);
    FrameDecoder decoder;
    std::optional<Json> decoded;
    for (const auto &frame : frames) {
        require(frame.size() >= 5 && frame.size() <= 20, "Invalid encoder frame size");
        decoded = decoder.feed(frame);
    }
    require(decoded && *decoded == message, "UTF-8 signaling failed to round-trip");
    decoder.feed(frames[0]);
    rejects([&] { decoder.feed(frames[2]); });
    for (const auto &frame : frames)
        decoded = decoder.feed(frame);
    require(decoded && *decoded == message, "Decoder did not recover");
    const auto start = FrameDecoder::Clock::time_point{};
    decoder.feed(frames[0], start);
    rejects([&] { decoder.feed(frames[0], start + 1s); });
    decoder.feed(frames[0], start + 1s);
    rejects([&] { decoder.feed(frames[1], start + 17s); });
    for (const auto &invalid : std::vector<std::vector<std::uint8_t>>{
             {}, {3, 0, 0, 0}, {4, 0, 0, 0, 'x'}, {3, 1, 0, 0, '{', '}'},
             {3, 0, 0, 0, 255}, {3, 0, 0, 0, '[', ']'}, {3, 0, 0, 1, '{', '}'}})
        rejects([&] { decoder.feed(invalid); });
    rejects([] { encode_message(Json{{"sdp", std::string(kMaxSignalMessage, 'a')}}); });
    for (std::size_t sequence = 0; sequence < 4096; ++sequence) {
        std::vector<std::uint8_t> frame(20, 'x');
        frame[0] = sequence == 0 ? 1 : 0;
        frame[1] = 0;
        frame[2] = sequence >> 8;
        frame[3] = sequence;
        decoder.feed(frame);
    }
    rejects([&] { decoder.feed({2, 0, 16, 0, 'x'}); });
}

void test_storage()
{
    int sockets[2];
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
    Storage storage(sockets[0]);
    auto first = std::async(std::launch::async, [&] { return storage.request('D', {'x'}); });
    require(read_packet(sockets[1]) == std::vector<std::uint8_t>{'D', 'x'}, "Invalid request framing");
    auto cancel = std::async(std::launch::async, [&] { return storage.request('C'); });
    require(reply(sockets[1], true, "old reply"), "Reply failed");
    require(first.get() == "old reply", "Reply was consumed by the cancellation");
    require(read_packet(sockets[1]) == std::vector<std::uint8_t>{'C'}, "Cancellation was reordered");
    require(reply(sockets[1], true, "cancelled"), "Reply failed");
    require(cancel.get() == "cancelled", "Wrong cancellation response");
    auto rejected = std::async(std::launch::async, [&] { return storage.request('B'); });
    read_packet(sockets[1]);
    reply(sockets[1], false, "invalid filename");
    rejects([&] { rejected.get(); });
    auto second = std::async(std::launch::async, [&] { return storage.request('F'); });
    read_packet(sockets[1]);
    reply(sockets[1], true, "received.txt");
    require(second.get() == "received.txt", "Storage rejection poisoned the socket");
    auto malformed = std::async(std::launch::async, [&] { return storage.request('C'); });
    read_packet(sockets[1]);
    write_exact(sockets[1], {0, 1, 134, 160});
    rejects([&] { malformed.get(); });
    rejects([&] { storage.request('C'); });
    ::close(sockets[1]);
}

void test_auto_connection()
{
    MemoryStorage storage;
    std::vector<Json> signals;
    Session session(*storage.client, [&](const Json &signal) { signals.push_back(signal); });
    for (auto invalid : std::vector<Json>{nullptr, Json::object(), envelope("session.hello", {}, "../other"),
                                          envelope("session.hello", Json::array())})
        rejects([&] { session.handle_signal(invalid); });
    auto invalid_version = envelope("session.hello");
    invalid_version["protocolVersion"] = true;
    rejects([&] { session.handle_signal(invalid_version); });
    session.handle_signal(envelope("session.hello"));
    require(signals.back()["type"] == "session.accepted" &&
            !signals.back()["payload"]["pairingRequired"].get<bool>(), "Missing automatic session acceptance");
    rejects([&] { session.handle_signal(envelope("session.hello", Json::object(), "other")); });
    require(!session.connected() && !session.channel_pending(), "Hello marked the channel connected");
    session.close();
    require(session.closed(), "Session failed to close");
}

void test_sdp()
{
    const std::string valid = "v=0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                              "a=candidate:1 1 UDP 1 192.168.1.2 1234 typ host\r\n";
    require(host_only_sdp(valid) == valid, "Valid local SDP rejected");
    rejects([&] { host_only_sdp(nullptr); });
    rejects([&] { host_only_sdp("v=0\r\n"); });
    for (const auto &replacement : {"relay", "srflx"}) {
        auto invalid = valid;
        invalid.replace(invalid.find("typ host"), 8, std::string("typ ") + replacement);
        rejects([&] { host_only_sdp(invalid); });
    }
    auto media = valid;
    media.replace(media.find("m=application"), 13, "m=audio");
    rejects([&] { host_only_sdp(media); });
    rejects([&] { host_only_sdp(valid + "m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"); });
}

class LocalPeer {
public:
    explicit LocalPeer(rtc::DataChannelInit initialization = {}, bool obscure_candidates = false)
    {
        rtc::Configuration configuration;
        configuration.disableAutoNegotiation = true;
        browser = std::make_shared<rtc::PeerConnection>(configuration);
        session = std::make_unique<Session>(*storage.client, [this](const Json &signal) {
            if (signal["type"] == "sdp.answer") {
                const auto sdp = host_only_sdp(signal["payload"]["description"]["sdp"]);
                browser->setRemoteDescription(rtc::Description(sdp, "answer"));
            }
        }, [this](std::function<void()> operation) { executor.post(std::move(operation)); },
        [this] { return Session::Clock::now() + std::chrono::milliseconds(clock_offset_.load()); });
        channel = browser->createDataChannel("file-share", initialization);
        channel->onMessage([this](rtc::message_variant message) {
            const auto *text = std::get_if<std::string>(&message);
            if (!text)
                return;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                messages_.push_back(Json::parse(*text));
            }
            changed_.notify_all();
        });
        browser->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    gathered_ = true;
                }
                changed_.notify_all();
            }
        });
        session->handle_signal(envelope("session.hello"));
        browser->setLocalDescription(rtc::Description::Type::Offer);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            require(changed_.wait_for(lock, 10s, [this] { return gathered_; }), "Local ICE gathering timed out");
        }
        const auto offer = browser->localDescription();
        require(offer.has_value(), "Missing local offer");
        auto offer_sdp = std::string(*offer);
        if (obscure_candidates) {
            std::istringstream lines(offer_sdp);
            std::string line;
            offer_sdp.clear();
            while (std::getline(lines, line)) {
                if (line.compare(0, 12, "a=candidate:") == 0) {
                    std::istringstream fields(line);
                    std::string token;
                    line.clear();
                    for (std::size_t index = 0; fields >> token; ++index) {
                        if (index == 4)
                            token = "lilygo-unresolvable-browser-candidate.local";
                        line += (index == 0 ? "" : " ") + token;
                    }
                    line += '\r';
                }
                offer_sdp += line + '\n';
            }
        }
        session->handle_signal(envelope("sdp.offer", {{"description",
            {{"type", "offer"}, {"sdp", host_only_sdp(offer_sdp)}}}}));
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!session->connected() && !session->closed() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
    }

    ~LocalPeer()
    {
        session->close();
        channel->resetCallbacks();
        browser->resetCallbacks();
        channel->close();
        browser->close();
        session.reset();
    }

    void send(Json message)
    {
        channel->send(message.dump());
    }

    void chunk(std::uint32_t sequence, const std::string &data)
    {
        rtc::binary packet{std::byte(sequence >> 24), std::byte(sequence >> 16),
                           std::byte(sequence >> 8), std::byte(sequence)};
        for (unsigned char character : data)
            packet.push_back(std::byte(character));
        channel->send(std::move(packet));
    }

    Json wait(const std::string &kind)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            lock.unlock();
            session->tick();
            lock.lock();
            if (messages_.empty()) {
                require(std::chrono::steady_clock::now() < deadline, "Timed out waiting for " + kind);
                changed_.wait_for(lock, 5ms);
                continue;
            }
            auto message = std::move(messages_.front());
            messages_.pop_front();
            if (message["kind"] == kind)
                return message;
            require(message["kind"] != "file-error", "Unexpected file error: " + message.dump());
        }
    }

    void start(const std::string &data, const std::string &file_id = "file-1", std::size_t chunk_size = kMaxChunkSize)
    {
        send({{"kind", "file-start"}, {"metadata", metadata(data, file_id, chunk_size)}});
        require(wait("file-accepted")["fileId"] == file_id, "Wrong accepted file");
    }

    void advance_time(std::chrono::seconds delta)
    {
        clock_offset_ += std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    }

    void expect_closed()
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!session->closed() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        require(session->closed() && !session->connected(), "Peer disconnect did not close the session");
    }

    MemoryStorage storage;
    Executor executor;
    std::shared_ptr<rtc::PeerConnection> browser;
    std::shared_ptr<rtc::DataChannel> channel;
    std::unique_ptr<Session> session;

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool gathered_ = false;
    std::atomic<std::int64_t> clock_offset_{0};
    std::deque<Json> messages_;
};

void test_local_peer()
{
    LocalPeer peer;
    require(peer.session->connected() && peer.session->channel_pending(), "Real local DataChannel failed to open");
    std::string data;
    for (std::size_t index = 0; index < kAckWindow * 3 * kMaxChunkSize + 53; ++index)
        data.push_back(static_cast<char>(index % 251));
    peer.start(data);
    std::uint32_t count = 0;
    for (std::size_t offset = 0; offset < data.size(); offset += kMaxChunkSize) {
        peer.chunk(count++, data.substr(offset, kMaxChunkSize));
        if (count % kAckWindow == 0)
            require(peer.wait("file-progress")["chunks"] == count, "Invalid receive credit acknowledgement");
    }
    peer.send({{"kind", "file-complete"}, {"fileId", "file-1"}, {"chunks", count}});
    const auto result = peer.wait("file-received");
    require(result["sha256"] == sha256(data) && result["size"] == data.size(), "Completion was not verified");
    require(peer.storage.committed().back().data == data, "Received file contents differ");
    peer.start("", "empty");
    peer.send({{"kind", "file-complete"}, {"fileId", "empty"}, {"chunks", 0}});
    require(peer.wait("file-received")["size"] == 0, "Empty file failed");
    peer.start("ab", "cancel", 1);
    peer.chunk(0, "a");
    peer.send({{"kind", "file-cancel"}, {"fileId", "cancel"}});
    peer.wait("file-cancelled");
    require(!peer.storage.pending(), "Cancellation retained a partial file");
    peer.start("x", "digest");
    peer.chunk(0, "y");
    peer.send({{"kind", "file-complete"}, {"fileId", "digest"}, {"chunks", 1}});
    require(peer.wait("file-error")["message"].get<std::string>().find("digest") != std::string::npos,
            "Digest mismatch was accepted");
    require(!peer.storage.pending() && peer.storage.committed().size() == 2, "Failed digest committed a file");
    const auto before = peer.storage.commands();
    const auto begin_count = std::count(before.begin(), before.end(), 'B');
    const std::vector<Json> changes{
        {{"fileName", "../secret"}}, {{"fileName", "/secret"}}, {{"fileName", "a\\b"}},
        {{"fileName", "."}}, {{"fileName", ".."}}, {{"fileName", std::string("bad\0name", 8)}},
        {{"fileName", std::string(256, 'a')}}, {{"sessionId", "other"}}, {{"fileId", ""}},
        {{"fileSize", -1}}, {{"fileSize", kMaxFileSize + 1}}, {{"fileSize", true}},
        {{"chunkSize", 0}}, {{"chunkSize", kMaxChunkSize + 1}}, {{"chunkSize", true}},
        {{"sha256", std::string(63, '0')}}, {{"sha256", std::string(64, 'x')}}};
    for (const auto &change : changes) {
        auto item = metadata("x");
        item.update(change);
        peer.send({{"kind", "file-start"}, {"metadata", item}});
        peer.wait("file-error");
    }
    const auto after = peer.storage.commands();
    require(std::count(after.begin(), after.end(), 'B') == begin_count, "Invalid metadata reached storage");
    peer.start("ab", "ordered", 1);
    peer.chunk(1, "a");
    peer.wait("file-error");
    require(!peer.storage.pending(), "Bad sequence retained a partial file");
    peer.chunk(0, "a");
    peer.send({{"kind", "file-complete"}, {"fileId", "ordered"}, {"chunks", 1}});
    peer.start("x", "retry");
    peer.chunk(0, "x");
    peer.send({{"kind", "file-complete"}, {"fileId", "retry"}, {"chunks", 1}});
    peer.wait("file-received");
    peer.start("x", "bad-count");
    peer.chunk(0, "x");
    peer.send({{"kind", "file-complete"}, {"fileId", "bad-count"}, {"chunks", true}});
    peer.wait("file-error");
    peer.start("x", "failed-commit");
    peer.chunk(0, "x");
    peer.storage.fail('F');
    peer.send({{"kind", "file-complete"}, {"fileId", "failed-commit"}, {"chunks", 1}});
    peer.wait("file-error");
    require(!peer.storage.pending() && peer.storage.committed().size() == 3, "Storage error acknowledged completion");
    peer.storage.fail(0);
    peer.start("ab", "disconnect", 1);
    peer.chunk(0, "a");
    peer.browser->close();
    peer.expect_closed();
    require(!peer.storage.pending(), "Disconnect retained a partial file");
}

void test_file_approval()
{
    LocalPeer peer;
    require(peer.session->connected(), "Connection required manual approval");
    peer.storage.hold_approval();
    peer.send({{"kind", "file-start"}, {"metadata", metadata("x", "rejected")}});
    require(peer.wait("file-pending")["fileId"] == "rejected", "Missing approval wait notification");
    peer.storage.wait_for_approval();
    auto commands = peer.storage.commands();
    require(std::count(commands.begin(), commands.end(), 'B') == 0, "File opened before device approval");
    peer.storage.decide_approval(false);
    require(peer.wait("file-error")["message"].get<std::string>().find("user-rejected:") == 0,
            "Device rejection was not reported");
    commands = peer.storage.commands();
    require(std::count(commands.begin(), commands.end(), 'B') == 0 && !peer.storage.pending(),
            "Rejected file reached storage");

    peer.storage.hold_approval();
    peer.send({{"kind", "file-start"}, {"metadata", metadata("x", "approved")}});
    peer.wait("file-pending");
    peer.storage.wait_for_approval();
    peer.chunk(0, "x");
    // Premature bytes are discarded, rather than queued for a later approval.
    std::this_thread::sleep_for(20ms);
    commands = peer.storage.commands();
    require(std::count(commands.begin(), commands.end(), 'D') == 0, "Bytes reached storage before approval");
    peer.storage.decide_approval(true);
    require(peer.wait("file-accepted")["fileId"] == "approved", "Approval was not acknowledged");
    peer.chunk(0, "x");
    peer.send({{"kind", "file-complete"}, {"fileId", "approved"}, {"chunks", 1}});
    peer.wait("file-received");
    require(peer.storage.committed().size() == 1 && peer.storage.committed().front().data == "x",
            "Approved bytes were not committed exactly once");

    peer.storage.hold_approval();
    peer.send({{"kind", "file-start"}, {"metadata", metadata("x", "cancel-pending")}});
    peer.wait("file-pending");
    peer.storage.wait_for_approval();
    const auto cancelled_at = std::chrono::steady_clock::now();
    peer.send({{"kind", "file-cancel"}, {"fileId", "cancel-pending"}});
    require(peer.wait("file-cancelled")["fileId"] == "cancel-pending", "Pending approval was not cancelled");
    require(std::chrono::steady_clock::now() - cancelled_at < 2s, "Cancellation waited for device approval");
    peer.storage.decide_approval(true);
    peer.session->tick();
    require(!peer.storage.pending(), "A late approval opened cancelled storage");

    peer.storage.hold_approval();
    peer.send({{"kind", "file-start"}, {"metadata", metadata("x", "expired")}});
    peer.wait("file-pending");
    peer.advance_time(46s);
    require(peer.wait("file-error")["message"].get<std::string>().find("approval-timeout:") == 0,
            "Device approval did not expire");
    peer.storage.decide_approval(true);
    peer.session->tick();
    require(!peer.storage.pending(), "An expired approval opened storage");

    peer.storage.hold_approval();
    peer.send({{"kind", "file-start"}, {"metadata", metadata("x", "disconnect-pending")}});
    peer.wait("file-pending");
    const auto disconnected_at = std::chrono::steady_clock::now();
    peer.browser->close();
    peer.expect_closed();
    require(std::chrono::steady_clock::now() - disconnected_at < 2s, "Disconnect waited for device approval");
    peer.storage.decide_approval(true);
    peer.session->tick();
    require(!peer.storage.pending() && peer.storage.committed().size() == 1,
            "A disconnected request retained writable storage");
}

void test_channel_limits()
{
    {
        LocalPeer peer({}, true);
        require(peer.session->connected(), "Browser mDNS host candidates prevented the local connection");
        peer.start("", "mdns");
        peer.send({{"kind", "file-complete"}, {"fileId", "mdns"}, {"chunks", 0}});
        peer.wait("file-received");
    }
    {
        rtc::DataChannelInit unreliable;
        unreliable.reliability.unordered = true;
        LocalPeer peer(unreliable);
        peer.expect_closed();
    }
    {
        LocalPeer peer;
        require(peer.session->connected(), "DataChannel failed to open");
        peer.channel->send(std::string(kMaxControlSize + 1, 'x'));
        peer.expect_closed();
        const auto commands = peer.storage.commands();
        require(std::count(commands.begin(), commands.end(), 'B') == 0, "Oversized control reached storage");
    }
    {
        LocalPeer peer;
        require(peer.session->connected(), "DataChannel failed to open");
        peer.channel->send(rtc::binary(kMaxChunkSize + 5));
        peer.expect_closed();
        const auto commands = peer.storage.commands();
        require(std::count(commands.begin(), commands.end(), 'D') == 0, "Oversized binary reached storage");
    }
    {
        LocalPeer peer;
        require(peer.session->connected(), "DataChannel failed to open");
        std::promise<void> blocked;
        peer.start(std::string(kMaxPendingMessages + 16, 'x'), "overflow", 1);
        std::promise<void> release;
        auto released = release.get_future().share();
        peer.executor.post([&] { blocked.set_value(); released.wait(); });
        blocked.get_future().wait();
        for (std::size_t count = 0; count < kMaxPendingMessages + 16; ++count)
            peer.chunk(0, "x");
        std::this_thread::sleep_for(200ms);
        release.set_value();
        peer.expect_closed();
    }
}

}

int main()
{
    try {
        rtc::InitLogger(rtc::LogLevel::Warning);
        test_framing();
        test_storage();
        test_auto_connection();
        test_sdp();
        test_local_peer();
        test_file_approval();
        test_channel_limits();
        rtc::Cleanup().wait();
        std::puts("Native transport framing, IPC, file approval, and real WebRTC transfers passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Native transport test failed: %s\n", error.what());
        return 1;
    }
}
