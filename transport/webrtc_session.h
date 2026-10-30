#pragma once

#include "storage_ipc.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace launcher::transport {

inline constexpr int kProtocolVersion = 1;
inline constexpr std::uint64_t kMaxFileSize = 4ULL * 1024 * 1024 * 1024;
inline constexpr std::size_t kMaxChunkSize = 16 * 1024;
inline constexpr std::size_t kMaxControlSize = 8192;
inline constexpr std::size_t kMaxSdpSize = 64 * 1024;
inline constexpr std::size_t kMaxPendingMessages = 128;
inline constexpr std::size_t kAckWindow = 32;

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string host_only_sdp(const nlohmann::json &description);

class Session {
public:
    using Clock = std::chrono::steady_clock;
    using Signal = std::function<void(const nlohmann::json &)>;
    using Dispatch = std::function<void(std::function<void()>)>;
    using Now = std::function<Clock::time_point()>;

    Session(Storage &storage, Signal send_signal, Dispatch dispatch = {}, Now now = {});
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    void handle_signal(const nlohmann::json &envelope);
    void tick();
    bool connected() const;
    bool channel_pending() const;
    bool closed() const;
    void close();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}
