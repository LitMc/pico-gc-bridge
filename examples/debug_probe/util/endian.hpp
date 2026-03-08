#pragma once
#include <cstdint>
#include <span>

namespace gcinput::util {

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

} // namespace gcinput::util
