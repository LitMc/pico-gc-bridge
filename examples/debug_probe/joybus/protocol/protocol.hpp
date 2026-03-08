#pragma once
#include "domain/mode.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gcinput::joybus {

using domain::PollMode;
using domain::RumbleMode;

enum class Command : uint8_t {
    Id = 0x00,
    Status = 0x40,
    Origin = 0x41,
    Recalibrate = 0x42,
    Reset = 0xFF,
    Invalid = 0xAA // 取り扱うべきでないことを示す
};

static inline constexpr bool is_valid_command(Command command) {
    switch (command) {
    case Command::Id:
    case Command::Status:
    case Command::Origin:
    case Command::Recalibrate:
    case Command::Reset:
        return true;
    default:
        return false;
    }
}

inline constexpr PollMode sanitize_poll_mode(const uint8_t v) {
    return (v <= static_cast<uint8_t>(PollMode::Mode4)) ? static_cast<PollMode>(v)
                                                        : PollMode::Mode3;
}

inline constexpr RumbleMode sanitize_rumble_mode(const uint8_t v) {
    return (v <= static_cast<uint8_t>(RumbleMode::Brake)) ? static_cast<RumbleMode>(v)
                                                          : RumbleMode::Off;
}

constexpr std::size_t kMaxResponseSize = 10;
constexpr std::size_t kIdResponseSize = 3;
constexpr std::size_t kOriginResponseSize = 10;
constexpr std::size_t kStatusResponseSize = 8;
constexpr std::size_t kRecalibrateResponseSize = kOriginResponseSize;
constexpr std::size_t kResetResponseSize = kIdResponseSize;

template <std::size_t N> struct Request {
    static_assert(N >= 1);
    std::array<uint8_t, N> tx;
    std::size_t expected_rx_size;
    constexpr std::span<const uint8_t> bytes() const { return tx; }
    constexpr Command command() const { return static_cast<Command>(tx[0]); }
};

inline constexpr Request<1> Id{{static_cast<uint8_t>(Command::Id)}, kIdResponseSize};

inline constexpr Request<1> Origin{{static_cast<uint8_t>(Command::Origin)}, kOriginResponseSize};

[[nodiscard]] constexpr Request<3> Status(PollMode poll_mode = PollMode::Mode3,
                                          RumbleMode rumble_mode = RumbleMode::Off) {
    return {{static_cast<uint8_t>(Command::Status), static_cast<uint8_t>(poll_mode),
             static_cast<uint8_t>(rumble_mode)},
            kStatusResponseSize};
};

inline constexpr Request<3> Recalibrate{{static_cast<uint8_t>(Command::Recalibrate), 0x00, 0x00},
                                        kRecalibrateResponseSize};

inline constexpr Request<1> Reset{{static_cast<uint8_t>(Command::Reset)}, kResetResponseSize};

class JoybusReply {
  public:
    JoybusReply() = default;
    template <std::size_t N>
    JoybusReply(Command cmd, const std::array<uint8_t, N> &src)
        : command_{cmd}, length_{N} {
        static_assert(N <= kMaxResponseSize);
        std::copy_n(src.data(), N, bytes_.begin());
    }

    JoybusReply(Command cmd, std::span<const uint8_t> src)
        : command_{cmd}, length_{static_cast<uint8_t>(
                             std::min<std::size_t>(src.size(), kMaxResponseSize))} {
        std::copy_n(src.data(), length_, bytes_.begin());
    }

    // 応答のコマンド種別
    Command command() const { return command_; }

    // 応答内容: modifiable
    std::span<uint8_t> view() { return {bytes_.data(), length_}; }

    // 応答内容: read-only
    std::span<const uint8_t> view() const { return {bytes_.data(), length_}; }

  private:
    Command command_{Command::Invalid};
    uint8_t length_{0};
    std::array<uint8_t, kMaxResponseSize> bytes_{};
};

} // namespace gcinput::joybus
