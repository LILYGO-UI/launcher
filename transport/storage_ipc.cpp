#include "storage_ipc.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace launcher::transport {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxPacket = 1024 * 1024 + 9;

void wait_socket(int descriptor, short events, Clock::time_point deadline)
{
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0)
            throw std::runtime_error("Launcher storage request timed out");
        pollfd state{descriptor, events, 0};
        const int result = poll(&state, 1, static_cast<int>(std::min<std::int64_t>(remaining.count(), INT_MAX)));
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            throw std::runtime_error(std::string("Launcher storage poll failed: ") + std::strerror(errno));
        if (result == 0)
            continue;
        if ((state.revents & events) != 0)
            return;
        throw std::runtime_error("Launcher closed the storage session");
    }
}

void transfer(int descriptor, std::uint8_t *data, std::size_t size, bool writing, Clock::time_point deadline)
{
    std::size_t offset = 0;
    while (offset < size) {
        wait_socket(descriptor, writing ? POLLOUT : POLLIN, deadline);
        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        if (writing)
            flags |= MSG_NOSIGNAL;
#endif
        const ssize_t count = writing ? send(descriptor, data + offset, size - offset, flags)
                                      : recv(descriptor, data + offset, size - offset, flags);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count <= 0)
            throw std::runtime_error("Launcher closed the storage session");
        offset += static_cast<std::size_t>(count);
    }
}

}

Storage::Storage(int fd) : fd_(fd)
{
    if (fd < 0)
        throw std::invalid_argument("A storage socket is required");
    const int descriptor_flags = fcntl(fd_, F_GETFD);
    if (descriptor_flags < 0 || fcntl(fd_, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Unable to protect the Launcher storage descriptor");
    }
#ifdef SO_NOSIGPIPE
    const int no_sigpipe = 1;
    setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
}

Storage::~Storage()
{
    close();
}

std::string Storage::request(char command, const std::vector<std::uint8_t> &payload)
{
    if (payload.size() >= kMaxPacket || static_cast<unsigned char>(command) > 127)
        throw std::invalid_argument("Invalid storage request");
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0)
        throw std::runtime_error("Launcher closed the storage session");
    bool rejected = false;
    try {
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        const auto length = static_cast<std::uint32_t>(payload.size() + 1);
        std::vector<std::uint8_t> packet{static_cast<std::uint8_t>(length >> 24),
                                         static_cast<std::uint8_t>(length >> 16),
                                         static_cast<std::uint8_t>(length >> 8),
                                         static_cast<std::uint8_t>(length),
                                         static_cast<std::uint8_t>(command)};
        packet.insert(packet.end(), payload.begin(), payload.end());
        transfer(fd_, packet.data(), packet.size(), true, deadline);
        std::uint8_t header[4];
        transfer(fd_, header, sizeof(header), false, deadline);
        const auto response_length = (std::uint32_t(header[0]) << 24) | (std::uint32_t(header[1]) << 16)
                                   | (std::uint32_t(header[2]) << 8) | header[3];
        if (response_length < 2 || response_length > 4096)
            throw std::runtime_error("Invalid Launcher storage response length");
        std::vector<std::uint8_t> response(response_length);
        transfer(fd_, response.data(), response.size(), false, deadline);
        if (response[0] != 'R' || response[1] > 1)
            throw std::runtime_error("Invalid Launcher storage response");
        const std::string text(response.begin() + 2, response.end());
        static_cast<void>(nlohmann::json(text).dump());
        if (response[1] == 0) {
            rejected = true;
            throw std::runtime_error(text.empty() ? "Launcher rejected the transfer" : text);
        }
        return text;
    } catch (...) {
        if (!rejected) {
            ::close(fd_);
            fd_ = -1;
        }
        throw;
    }
}

void Storage::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}
