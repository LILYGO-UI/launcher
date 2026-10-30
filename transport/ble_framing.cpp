#include "ble_framing.h"

#include <algorithm>
#include <stdexcept>

namespace launcher::transport {

std::vector<std::vector<std::uint8_t>> encode_message(const nlohmann::json &message)
{
    const std::string data = message.dump();
    if (data.empty() || data.size() > kMaxSignalMessage)
        throw std::invalid_argument("Bluetooth signaling message is too large");
    std::vector<std::vector<std::uint8_t>> frames;
    frames.reserve((data.size() + kFramePayload - 1) / kFramePayload);
    for (std::size_t offset = 0; offset < data.size(); offset += kFramePayload) {
        const auto count = std::min(kFramePayload, data.size() - offset);
        const auto sequence = offset / kFramePayload;
        const auto flags = (offset == 0 ? 1 : 0) | (offset + count == data.size() ? 2 : 0);
        std::vector<std::uint8_t> frame{static_cast<std::uint8_t>(flags), 0,
                                       static_cast<std::uint8_t>(sequence >> 8),
                                       static_cast<std::uint8_t>(sequence)};
        frame.insert(frame.end(), data.begin() + offset, data.begin() + offset + count);
        frames.push_back(std::move(frame));
    }
    return frames;
}

void FrameDecoder::reset()
{
    data_.clear();
    sequence_ = 0;
    deadline_ = {};
    active_ = false;
}

std::optional<nlohmann::json> FrameDecoder::feed(const std::vector<std::uint8_t> &frame,
                                                Clock::time_point now)
{
    try {
        if (frame.size() < 5 || frame.size() > 20)
            throw std::invalid_argument("Invalid Bluetooth frame length");
        const auto flags = frame[0];
        const auto sequence = (static_cast<std::uint32_t>(frame[2]) << 8) | frame[3];
        if ((flags & ~3) != 0 || frame[1] != 0)
            throw std::invalid_argument("Invalid Bluetooth frame header");
        if (active_ && now > deadline_)
            throw std::invalid_argument("Bluetooth signaling message timed out");
        if ((flags & 1) != 0) {
            if (active_ || sequence != 0)
                throw std::invalid_argument("Unexpected Bluetooth message start");
            active_ = true;
            sequence_ = 0;
            data_.clear();
        }
        if (!active_ || sequence != sequence_)
            throw std::invalid_argument("Bluetooth signaling frames are out of order");
        data_.insert(data_.end(), frame.begin() + 4, frame.end());
        if (data_.size() > kMaxSignalMessage)
            throw std::invalid_argument("Bluetooth signaling message is too large");
        ++sequence_;
        deadline_ = now + kFrameTimeout;
        if ((flags & 2) == 0)
            return std::nullopt;
        auto message = nlohmann::json::parse(data_, [](int depth, nlohmann::json::parse_event_t,
                                                      nlohmann::json &) {
            if (depth > 64)
                throw std::invalid_argument("Bluetooth signaling JSON is nested too deeply");
            return true;
        });
        if (!message.is_object())
            throw std::invalid_argument("Bluetooth signaling requires a JSON object");
        reset();
        return message;
    } catch (...) {
        reset();
        throw;
    }
}

}
