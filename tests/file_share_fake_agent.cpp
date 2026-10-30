#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

const std::string kPayload = "hello world";
const std::string kPayloadSha = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
const std::string kEmptySha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string encode_u32(std::uint32_t value)
{
    std::string result(4, '\0');
    for (unsigned index = 0; index < 4; ++index)
        result[index] = static_cast<char>(value >> (24 - index * 8));
    return result;
}

std::string encode_u64(std::uint64_t value)
{
    std::string result(8, '\0');
    for (unsigned index = 0; index < 8; ++index)
        result[index] = static_cast<char>(value >> (56 - index * 8));
    return result;
}

class Peer {
public:
    explicit Peer(int descriptor) : descriptor_(descriptor)
    {
        const timeval timeout{5, 0};
        require(setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
                "Cannot set the IPC receive timeout");
        require(setsockopt(descriptor_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
                "Cannot set the IPC send timeout");
    }

    ~Peer() { close(descriptor_); }

    void send_bytes(std::string_view bytes)
    {
        while (!bytes.empty()) {
            const ssize_t count = send(descriptor_, bytes.data(), bytes.size(), 0);
            if (count < 0 && errno == EINTR) continue;
            require(count > 0, "Receiver closed the IPC channel while sending");
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }

    std::string receive_bytes(std::size_t size)
    {
        std::string result(size, '\0');
        std::size_t offset = 0;
        while (offset < size) {
            const ssize_t count = recv(descriptor_, result.data() + offset, size - offset, 0);
            if (count < 0 && errno == EINTR) continue;
            require(count > 0, "Receiver closed the IPC channel while receiving");
            offset += static_cast<std::size_t>(count);
        }
        return result;
    }

    std::pair<bool, std::string> response()
    {
        const std::string header = receive_bytes(4);
        std::uint32_t size = 0;
        for (unsigned char value : header) size = (size << 8) | value;
        require(size >= 2 && size <= 1024 * 1024, "Invalid response frame size");
        const std::string frame = receive_bytes(size);
        require(frame[0] == 'R' && (frame[1] == 0 || frame[1] == 1), "Invalid response frame");
        return {frame[1] == 1, frame.substr(2)};
    }

    std::pair<bool, std::string> request(char kind, const std::string &body = {})
    {
        const std::string frame = std::string(1, kind) + body;
        const std::string length = encode_u32(static_cast<std::uint32_t>(frame.size()));
        send_bytes(std::string_view(length).substr(0, 1));
        send_bytes(length.substr(1) + frame);
        return response();
    }

    std::string command(char kind, const std::string &body = {}, bool expected_ok = true)
    {
        const auto reply = request(kind, body);
        require(reply.first == expected_ok, std::string("Unexpected result for command ") + kind + ": " + reply.second);
        return reply.second;
    }

    void begin(const std::string &filename, bool expected_ok = true,
               const std::string &data = kPayload, const std::string &sha = kPayloadSha,
               std::optional<std::uint64_t> size = {})
    {
        const auto bytes = size.value_or(data.size());
        const auto id = next_approval_id_++;
        if (expected_ok) approve(id, filename, bytes);
        begin_approved(id, filename, bytes, sha, expected_ok);
    }

    void approve(std::uint64_t id, const std::string &name, std::uint64_t size, bool expected_ok = true)
    {
        const auto start = request('P', encode_u64(id) + encode_u64(size) + name);
        if (!start.first) {
            require(!expected_ok, "Approval request was rejected: " + start.second);
            return;
        }
        require(start.second == "pending", "Approval did not enter pending state");
        for (;;) {
            const auto decision = request('Q', encode_u64(id));
            if (decision.first && decision.second == "pending") {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            require(decision.first == expected_ok && (!decision.first || decision.second == "approved"),
                    "Unexpected device approval decision: " + decision.second);
            return;
        }
    }

    void request_approval(std::uint64_t id, const std::string &name, std::uint64_t size)
    {
        require(command('P', encode_u64(id) + encode_u64(size) + name) == "pending", "Expected pending approval");
    }

    void begin_approved(std::uint64_t id, const std::string &name, std::uint64_t size,
                        const std::string &sha = kPayloadSha, bool expected_ok = true)
    {
        command('B', encode_u64(id) + encode_u64(size) + sha + name, expected_ok);
    }

    void chunk(std::uint64_t sequence, const std::string &data, bool expected_ok = true)
    {
        command('D', encode_u64(sequence) + data, expected_ok);
    }

    void expect_closed()
    {
        char byte = 0;
        ssize_t count;
        do {
            count = recv(descriptor_, &byte, 1, 0);
        } while (count < 0 && errno == EINTR);
        require(count == 0, "Receiver did not close the invalid IPC stream");
    }

private:
    int descriptor_;
    std::uint64_t next_approval_id_ = 1;
};

void run_scenario(Peer &peer, const std::string &scenario)
{
    if (scenario == "success") {
        peer.command('Z', {}, false);
        peer.command('B', "short", false);
        peer.command('D', {}, false);
        peer.command('F', {}, false);
        peer.command('S', std::string(1, '\0'), false);
        peer.command('S', std::string(1025, 'x'), false);
        for (const char *expected : {"hello.txt", "hello (1).txt"}) {
            peer.begin("hello.txt");
            peer.begin("other.txt", false);
            peer.chunk(0, kPayload.substr(0, 5));
            peer.chunk(1, kPayload.substr(5));
            require(peer.command('F') == expected, "Unexpected committed filename");
        }
        peer.begin("empty.txt", true, {}, kEmptySha);
        require(peer.command('F') == "empty.txt", "Unexpected empty-file name");
        peer.command('C');
    } else if (scenario == "rejected") {
        peer.begin("../escape.txt", false);
        peer.begin("too-large.txt", false, kPayload, kPayloadSha, 4ULL * 1024 * 1024 * 1024 + 1);
        peer.begin("hash.txt", true, kPayload, std::string(64, '0'));
        peer.chunk(0, kPayload);
        peer.command('F', {}, false);
        peer.begin("short.txt", true, kPayload, kPayloadSha, kPayload.size() + 1);
        peer.chunk(0, kPayload);
        peer.command('F', {}, false);
        peer.begin("order.txt");
        peer.chunk(1, kPayload, false);
        peer.command('F', {}, false);
        peer.begin("cancel.txt");
        peer.chunk(0, kPayload.substr(0, 5));
        peer.command('C');
        peer.command('F', {}, false);
        peer.begin("digest-on-close.txt", true, kPayload, std::string(64, '0'));
        peer.chunk(0, kPayload);
        peer.command('F', {}, false);
        peer.command('C');
    } else if (scenario == "status-close") {
        peer.command('S', "File sharing closed.");
        peer.command('C');
    } else if (scenario == "interrupted") {
        peer.begin("partial.txt");
        peer.chunk(0, kPayload.substr(0, 5));
    } else if (scenario == "zero-frame" || scenario == "large-frame") {
        peer.send_bytes(encode_u32(scenario == "zero-frame" ? 0 : 1024 * 1024 + 10));
        const auto reply = peer.response();
        require(!reply.first && reply.second.find("length") != std::string::npos, "Invalid frame was accepted");
        peer.expect_closed();
    } else if (scenario == "stop") {
        require(signal(SIGTERM, SIG_IGN) != SIG_ERR, "Cannot ignore SIGTERM");
        peer.begin("stopped.txt");
        peer.chunk(0, kPayload.substr(0, 5));
        peer.command('S', "Ready for stop test: " + std::to_string(getpid()));
        std::this_thread::sleep_for(std::chrono::seconds(60));
    } else if (scenario == "approval") {
        const bool accepted = std::getenv("LILYGO_TEST_APPROVAL_ACCEPT") != nullptr;
        peer.approve(1, "approval.txt", kPayload.size(), accepted);
        if (accepted) {
            peer.begin_approved(1, "approval.txt", kPayload.size());
            peer.chunk(0, kPayload);
            require(peer.command('F') == "approval.txt", "Approved file was not committed");
        } else {
            peer.begin_approved(1, "approval.txt", kPayload.size(), kPayloadSha, false);
        }
    } else if (scenario == "approval-binding") {
        peer.begin_approved(1, "approval.txt", kPayload.size(), kPayloadSha, false);
        peer.approve(1, "approval.txt", kPayload.size());
        peer.begin_approved(1, "different.txt", kPayload.size(), kPayloadSha, false);
        peer.begin_approved(1, "approval.txt", kPayload.size(), kPayloadSha, false);
        peer.approve(2, "approval.txt", kPayload.size());
        peer.begin_approved(2, "approval.txt", kPayload.size() + 1, kPayloadSha, false);
        peer.approve(3, "approval.txt", kPayload.size());
        peer.command('C');
        peer.begin_approved(3, "approval.txt", kPayload.size(), kPayloadSha, false);
        peer.approve(4, "approval.txt", kPayload.size());
        peer.approve(5, "../escape.txt", kPayload.size(), false);
        peer.begin_approved(4, "approval.txt", kPayload.size(), kPayloadSha, false);
        peer.approve(6, "approval.txt", kPayload.size());
        peer.begin_approved(5, "approval.txt", kPayload.size(), kPayloadSha, false);
        peer.request_approval(7, "cancel-pending.txt", kPayload.size());
        peer.command('C');
        peer.command('Q', encode_u64(7), false);
        peer.begin_approved(7, "cancel-pending.txt", kPayload.size(), kPayloadSha, false);
    } else if (scenario == "approval-stop") {
        peer.request_approval(1, "pending.txt", kPayload.size());
        std::this_thread::sleep_for(std::chrono::seconds(60));
    } else if (scenario == "approval-timeout") {
        peer.request_approval(1, "timeout.txt", kPayload.size());
        for (;;) {
            const auto decision = peer.request('Q', encode_u64(1));
            if (!decision.first) {
                require(decision.second.find("approval-timeout:") == 0, "Missing approval timeout response");
                break;
            }
            require(decision.second == "pending", "Unconfirmed request was approved");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } else {
        throw std::runtime_error("Unknown test scenario");
    }
}

} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc == 3 && std::strcmp(argv[1], "--ipc-fd") == 0 &&
                    std::strcmp(argv[2], "3") == 0,
                "Expected the native helper command line and IPC descriptor 3");
        const char *private_descriptor = std::getenv("LILYGO_TEST_PRIVATE_FD");
        require(private_descriptor != nullptr, "Private descriptor fixture is missing");
        struct stat metadata{};
        require(fstat(std::atoi(private_descriptor), &metadata) < 0 && errno == EBADF,
                "The helper inherited a private launcher descriptor");
        require(signal(SIGPIPE, SIG_IGN) != SIG_ERR, "Cannot ignore SIGPIPE");
        const char *scenario = std::getenv("LILYGO_TEST_SHARE_SCENARIO");
        require(scenario != nullptr, "Test scenario is missing");
        Peer peer(3);
        run_scenario(peer, scenario);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Native fake agent failed: %s\n", error.what());
        return 1;
    }
}
