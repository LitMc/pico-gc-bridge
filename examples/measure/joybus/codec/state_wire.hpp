#pragma once
#include "domain/report.hpp"
#include "domain/state.hpp"
#include "joybus/codec/common.hpp"
#include "joybus/codec/report_wire.hpp"
#include "joybus/protocol/protocol.hpp"
#include "joybus/protocol/reply.hpp"
#include <array>
#include <span>
#include <stdio.h>

// https://jefflongo.dev/posts/gc-controller-reverse-engineering-part-1/#poll-mode
// コントローラ入力をJoybus形式に変換、復元するための処理群
namespace gcinput::joybus::state {
// 下位4ビットを取り出して8ビットに拡張。PollMode0..2のレスポンスには4bit幅のアナログ値が入る
// これを内部のアナログ値0..255に変換。中点0x8(8)が0x80(128)に対応するよう<<4する
inline constexpr uint8_t expand4bitTo8bit(uint8_t value4bit) {
    return static_cast<uint8_t>((value4bit & 0x0Fu) << 4);
}

// 8ビット値を4ビット値に縮小。内部のアナログ値0..255を4bit幅に変換
// PollMode0..2のレスポンスに使う
inline constexpr uint8_t shrink8bitTo4bit(uint8_t value8bit) {
    return static_cast<uint8_t>(value8bit >> 4);
}

// 2つの4ビット値を1バイトにまとめる
// PollMode0..2のレスポンスに使う
inline constexpr uint8_t pack4bitsToByte(uint8_t high4bits, uint8_t low4bits) {
    return static_cast<uint8_t>(((high4bits & 0x0Fu) << 4) | (low4bits & 0x0Fu));
}

// Status wordから共通形式のボタン入力を抽出
inline constexpr domain::ButtonInput
decode_buttons_from_status_word(std::span<const uint8_t, 2> byte2) {
    domain::ButtonInput buttons{};

    const uint16_t status_word = common::read_u16_le(byte2);
    buttons.a = (status_word & domain::to_mask(domain::PadButton::A)) != 0;
    buttons.b = (status_word & domain::to_mask(domain::PadButton::B)) != 0;
    buttons.x = (status_word & domain::to_mask(domain::PadButton::X)) != 0;
    buttons.y = (status_word & domain::to_mask(domain::PadButton::Y)) != 0;
    buttons.start = (status_word & domain::to_mask(domain::PadButton::Start)) != 0;
    buttons.dpad_left = (status_word & domain::to_mask(domain::PadButton::DpadLeft)) != 0;
    buttons.dpad_right = (status_word & domain::to_mask(domain::PadButton::DpadRight)) != 0;
    buttons.dpad_down = (status_word & domain::to_mask(domain::PadButton::DpadDown)) != 0;
    buttons.dpad_up = (status_word & domain::to_mask(domain::PadButton::DpadUp)) != 0;
    buttons.z = (status_word & domain::to_mask(domain::PadButton::Z)) != 0;
    buttons.r = (status_word & domain::to_mask(domain::PadButton::R)) != 0;
    buttons.l = (status_word & domain::to_mask(domain::PadButton::L)) != 0;

    return buttons;
}

// 共通形式の実行時レポートをStatus wordに変換
inline constexpr void encode_to_status_word(const domain::PadState &state,
                                            std::span<uint8_t, 2> status_word_bytes) {
    uint16_t status_word = 0;

    // ボタン情報をStatus wordにセット
    const auto &buttons = state.input.buttons;
    status_word |= (static_cast<uint16_t>(buttons.a) ? domain::to_mask(domain::PadButton::A) : 0u);
    status_word |= (static_cast<uint16_t>(buttons.b) ? domain::to_mask(domain::PadButton::B) : 0u);
    status_word |= (static_cast<uint16_t>(buttons.x) ? domain::to_mask(domain::PadButton::X) : 0u);
    status_word |= (static_cast<uint16_t>(buttons.y) ? domain::to_mask(domain::PadButton::Y) : 0u);
    status_word |=
        (static_cast<uint16_t>(buttons.start) ? domain::to_mask(domain::PadButton::Start) : 0u);
    status_word |=
        (static_cast<uint16_t>(buttons.dpad_left) ? domain::to_mask(domain::PadButton::DpadLeft)
                                                  : 0u);
    status_word |=
        (static_cast<uint16_t>(buttons.dpad_right) ? domain::to_mask(domain::PadButton::DpadRight)
                                                   : 0u);
    status_word |=
        (static_cast<uint16_t>(buttons.dpad_down) ? domain::to_mask(domain::PadButton::DpadDown)
                                                  : 0u);
    status_word |=
        (static_cast<uint16_t>(buttons.dpad_up) ? domain::to_mask(domain::PadButton::DpadUp) : 0u);
    status_word |= (static_cast<uint16_t>(buttons.z) ? domain::to_mask(domain::PadButton::Z) : 0u);
    status_word |= (static_cast<uint16_t>(buttons.r) ? domain::to_mask(domain::PadButton::R) : 0u);
    status_word |= (static_cast<uint16_t>(buttons.l) ? domain::to_mask(domain::PadButton::L) : 0u);

    // レポートフラグをStatus wordにセット
    // OriginNotSentはビットが立っているとOrigin未送信を意味するので反転
    const auto &report = state.report;
    status_word |=
        report.origin_sent ? 0 : static_cast<uint16_t>(report::StatusWordBits::OriginNotSent);
    status_word |=
        report.error_latched ? static_cast<uint16_t>(report::StatusWordBits::ErrorLatched) : 0;

    // 2バイト目の7ビット目は常に1となる（Longo氏の資料では直近エラーの有無となっているが）
    // ここを1にしないとコントローラが認識されない
    status_word |= static_cast<uint16_t>(report::StatusWordBits::Always1);

    status_word |= report.use_controller_origin
                       ? static_cast<uint16_t>(report::StatusWordBits::UseControllerOrigin)
                       : 0;

    common::write_u16_le(status_word, status_word_bytes);
}

// JoybusのStatusレスポンスを共通形式に変換
inline constexpr domain::PadState
decode_status(std::span<const uint8_t, joybus::kStatusResponseSize> rx,
              joybus::PollMode poll_mode) {
    domain::PadState out{};

    // 先頭2バイト（Status word）を共通形式のレポートに変換
    out.report = report::decode_report_from_status_word(rx.first<2>());
    // Status wordを共通形式のボタン入力に変換
    out.input.buttons = decode_buttons_from_status_word(rx.first<2>());

    auto &analog_input = out.input.analog;
    analog_input.stick_x = rx[2];
    analog_input.stick_y = rx[3];

    // https://jefflongo.dev/posts/gc-controller-reverse-engineering-part-1/#poll-mode
    switch (poll_mode) {
    case joybus::PollMode::Mode0:
        analog_input.c_stick_x = rx[4];
        analog_input.c_stick_y = rx[5];
        analog_input.l_analog = expand4bitTo8bit((rx[6] >> 4) & 0x0Fu);
        analog_input.r_analog = expand4bitTo8bit(rx[6] & 0x0Fu);
        analog_input.a_analog = expand4bitTo8bit((rx[7] >> 4) & 0x0Fu);
        analog_input.b_analog = expand4bitTo8bit(rx[7] & 0x0Fu);
        break;
    case joybus::PollMode::Mode1:
        analog_input.c_stick_x = expand4bitTo8bit((rx[4] >> 4) & 0x0Fu);
        analog_input.c_stick_y = expand4bitTo8bit(rx[4] & 0x0Fu);
        analog_input.l_analog = rx[5];
        analog_input.r_analog = rx[6];
        analog_input.a_analog = expand4bitTo8bit((rx[7] >> 4) & 0x0Fu);
        analog_input.b_analog = expand4bitTo8bit(rx[7] & 0x0Fu);
        break;
    case joybus::PollMode::Mode2:
        analog_input.c_stick_x = expand4bitTo8bit((rx[4] >> 4) & 0x0Fu);
        analog_input.c_stick_y = expand4bitTo8bit(rx[4] & 0x0Fu);
        analog_input.l_analog = expand4bitTo8bit((rx[5] >> 4) & 0x0Fu);
        analog_input.r_analog = expand4bitTo8bit(rx[5] & 0x0Fu);
        analog_input.a_analog = rx[6];
        analog_input.b_analog = rx[7];
        break;
    case joybus::PollMode::Mode3:
        analog_input.c_stick_x = rx[4];
        analog_input.c_stick_y = rx[5];
        analog_input.l_analog = rx[6];
        analog_input.r_analog = rx[7];
        break;
    case joybus::PollMode::Mode4:
        analog_input.c_stick_x = rx[4];
        analog_input.c_stick_y = rx[5];
        analog_input.a_analog = rx[6];
        analog_input.b_analog = rx[7];
        break;
    default:
        break;
    }

    return out;
}

// 共通形式のStatus情報をJoybusレスポンス形式に変換
inline JoybusReply encode_status(const domain::PadState &state, joybus::PollMode poll_mode) {
    std::array<uint8_t, joybus::kStatusResponseSize> out{};

    // out[0], out[1]: Status word
    encode_to_status_word(state, std::span<uint8_t, 2>{out.data(), 2});

    const auto &analog_input = state.input.analog;

    out[2] = analog_input.stick_x;
    out[3] = analog_input.stick_y;

    // https://jefflongo.dev/posts/gc-controller-reverse-engineering-part-1/#poll-mode
    switch (poll_mode) {
    case joybus::PollMode::Mode0:
        out[4] = analog_input.c_stick_x;
        out[5] = analog_input.c_stick_y;
        out[6] = pack4bitsToByte(shrink8bitTo4bit(analog_input.l_analog),
                                 shrink8bitTo4bit(analog_input.r_analog));
        out[7] = pack4bitsToByte(shrink8bitTo4bit(analog_input.a_analog),
                                 shrink8bitTo4bit(analog_input.b_analog));
        break;
    case joybus::PollMode::Mode1:
        out[4] = pack4bitsToByte(shrink8bitTo4bit(analog_input.c_stick_x),
                                 shrink8bitTo4bit(analog_input.c_stick_y));
        out[5] = analog_input.l_analog;
        out[6] = analog_input.r_analog;
        out[7] = pack4bitsToByte(shrink8bitTo4bit(analog_input.a_analog),
                                 shrink8bitTo4bit(analog_input.b_analog));
        break;
    case joybus::PollMode::Mode2:
        out[4] = pack4bitsToByte(shrink8bitTo4bit(analog_input.c_stick_x),
                                 shrink8bitTo4bit(analog_input.c_stick_y));
        out[5] = pack4bitsToByte(shrink8bitTo4bit(analog_input.l_analog),
                                 shrink8bitTo4bit(analog_input.r_analog));
        out[6] = analog_input.a_analog;
        out[7] = analog_input.b_analog;
        break;
    case joybus::PollMode::Mode3:
        out[4] = analog_input.c_stick_x;
        out[5] = analog_input.c_stick_y;
        out[6] = analog_input.l_analog;
        out[7] = analog_input.r_analog;
        break;
    case joybus::PollMode::Mode4:
        out[4] = analog_input.c_stick_x;
        out[5] = analog_input.c_stick_y;
        out[6] = analog_input.a_analog;
        out[7] = analog_input.b_analog;
        break;
    default:
        break;
    }

    return JoybusReply(joybus::Command::Status, out);
}

inline constexpr domain::PadState
decode_origin(std::span<const uint8_t, joybus::kOriginResponseSize> rx) {
    domain::PadState out{};

    out.report = report::decode_report_from_status_word(rx.first<2>());
    out.input.buttons = decode_buttons_from_status_word(rx.first<2>());
    auto &analog_input = out.input.analog;
    analog_input.stick_x = rx[2];
    analog_input.stick_y = rx[3];
    analog_input.c_stick_x = rx[4];
    analog_input.c_stick_y = rx[5];
    analog_input.l_analog = rx[6];
    analog_input.r_analog = rx[7];
    analog_input.a_analog = rx[8];
    analog_input.b_analog = rx[9];
    return out;
}

inline constexpr domain::PadState
decode_recalibrate(std::span<const uint8_t, joybus::kRecalibrateResponseSize> rx) {
    return decode_origin(rx);
}

inline constexpr std::array<uint8_t, joybus::kOriginResponseSize>
encode_origin_byte(const domain::PadState &state) {
    std::array<uint8_t, joybus::kOriginResponseSize> out{};

    // out[0], out[1]: Status word
    encode_to_status_word(state, std::span<uint8_t, 2>{out.data(), 2});
    const auto &analog_input = state.input.analog;

    out[2] = analog_input.stick_x;
    out[3] = analog_input.stick_y;
    out[4] = analog_input.c_stick_x;
    out[5] = analog_input.c_stick_y;
    out[6] = analog_input.l_analog;
    out[7] = analog_input.r_analog;
    out[8] = analog_input.a_analog;
    out[9] = analog_input.b_analog;

    return out;
}

inline JoybusReply encode_origin(const domain::PadState &state) {
    return JoybusReply(joybus::Command::Origin, encode_origin_byte(state));
}

inline JoybusReply encode_recalibrate(const domain::PadState &state) {
    return JoybusReply(joybus::Command::Recalibrate, encode_origin_byte(state));
}

static_assert(shrink8bitTo4bit(expand4bitTo8bit(0x0)) == 0x0);
static_assert(shrink8bitTo4bit(expand4bitTo8bit(0x8)) == 0x8);
static_assert(shrink8bitTo4bit(expand4bitTo8bit(0xF)) == 0xF);
static_assert(pack4bitsToByte(0xA, 0x5) == 0xA5);
} // namespace gcinput::joybus::state
