#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gcinput::joybus {
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

enum class PollMode : uint8_t {
    Mode0 = 0x00,
    Mode1 = 0x01,
    Mode2 = 0x02,
    Mode3 = 0x03,
    Mode4 = 0x04,
    Default = 0x03,
};

inline constexpr joybus::PollMode sanitize_poll_mode(const uint8_t v) {
    return (v <= static_cast<uint8_t>(joybus::PollMode::Mode4)) ? static_cast<joybus::PollMode>(v)
                                                                : joybus::PollMode::Default;
}

enum class RumbleMode : uint8_t {
    Off = 0x00,
    On = 0x01,
    Brake = 0x02,
};

inline constexpr joybus::RumbleMode sanitize_rumble_mode(const uint8_t v) {
    return (v <= static_cast<uint8_t>(joybus::RumbleMode::Brake))
               ? static_cast<joybus::RumbleMode>(v)
               : joybus::RumbleMode::Off;
}

inline constexpr uint8_t clamp_poll_mode(uint8_t v) { return (v <= 4) ? v : 3; }   // fallback Mode3
inline constexpr uint8_t clamp_rumble_mode(uint8_t v) { return (v <= 2) ? v : 0; } // fallback Off

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

[[nodiscard]] constexpr Request<3> Status(PollMode poll_mode = PollMode::Default,
                                          RumbleMode rumble_mode = RumbleMode::Off) {
    return {{static_cast<uint8_t>(Command::Status), static_cast<uint8_t>(poll_mode),
             static_cast<uint8_t>(rumble_mode)},
            kStatusResponseSize};
};

inline constexpr Request<3> Recalibrate{{static_cast<uint8_t>(Command::Recalibrate), 0x00, 0x00},
                                        kRecalibrateResponseSize};

inline constexpr Request<1> Reset{{static_cast<uint8_t>(Command::Reset)}, kResetResponseSize};

} // namespace gcinput::joybus
