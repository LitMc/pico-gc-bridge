#pragma once
#include "domain/identity.hpp"

namespace gcinput::joybus::common {

inline constexpr domain::PollMode to_domain_poll_mode(joybus::PollMode joybus_poll_mode) {
    return static_cast<domain::PollMode>(static_cast<uint8_t>(joybus_poll_mode));
}

inline constexpr domain::RumbleMode to_domain_rumble_mode(joybus::RumbleMode joybus_rumble_mode) {
    return static_cast<domain::RumbleMode>(
        joybus::clamp_rumble_mode(static_cast<uint8_t>(joybus_rumble_mode)));
}

inline constexpr joybus::PollMode to_joybus_poll_mode(domain::PollMode poll_mode) {
    return static_cast<joybus::PollMode>(joybus::clamp_poll_mode(static_cast<uint8_t>(poll_mode)));
}

inline constexpr joybus::RumbleMode to_joybus_rumble_mode(domain::RumbleMode rumble_mode) {
    return static_cast<joybus::RumbleMode>(
        joybus::clamp_rumble_mode(static_cast<uint8_t>(rumble_mode)));
}

// Little-endianで2バイトを読み取る
// Longo氏の資料ではStatus word部分のbitがLittle-endianで書かれているため
inline constexpr uint16_t read_u16_le(std::span<const uint8_t, 2> b) {
    return static_cast<uint16_t>(static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8));
}

// Little-endianで2バイトを書き込む
// Longo氏の資料ではStatus word部分のbitがLittle-endianで書かれているため
inline constexpr void write_u16_le(uint16_t v, std::span<uint8_t, 2> b) {
    b[0] = static_cast<uint8_t>(v & 0xFFu);        // low byte first
    b[1] = static_cast<uint8_t>((v >> 8) & 0xFFu); // high byte second
}
} // namespace gcinput::joybus::common
