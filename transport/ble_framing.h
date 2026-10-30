#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

namespace launcher::transport {

inline constexpr char kServiceUuid[] = "fde6eb6b-0d5f-49c1-b7ae-296288a7ab0b";
inline constexpr char kRxUuid[] = "fde6eb6c-0d5f-49c1-b7ae-296288a7ab0b";
inline constexpr char kTxUuid[] = "fde6eb6d-0d5f-49c1-b7ae-296288a7ab0b";
inline constexpr char kAdvertisementNamePrefix[] = "lilygo_";
inline constexpr std::size_t kMaxSignalMessage = 65536;
inline constexpr std::size_t kFramePayload = 16;
inline constexpr auto kFrameTimeout = std::chrono::seconds(15);

std::vector<std::vector<std::uint8_t>> encode_message(const nlohmann::json &message);

class FrameDecoder {
public:
    using Clock = std::chrono::steady_clock;

    std::optional<nlohmann::json> feed(const std::vector<std::uint8_t> &frame,
                                      Clock::time_point now = Clock::now());
    void reset();

private:
    std::vector<std::uint8_t> data_;
    std::uint32_t sequence_ = 0;
    Clock::time_point deadline_{};
    bool active_ = false;
};

}
