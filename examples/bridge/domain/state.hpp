#pragma once
#include "domain/report.hpp"
#include <cstdint>
#include <type_traits>

namespace gcinput::domain {

// 意味上のボタン集合
// 値はStatusレスポンス先頭2バイト（Status word）の最下位ビットから数えた位置に対応
// https://jefflongo.dev/posts/gc-controller-reverse-engineering-part-1/?utm_source=chatgpt.com#status-response-bytes-1-and-2
enum class PadButton : uint16_t {
    A = (1u << 0),
    B = (1u << 1),
    X = (1u << 2),
    Y = (1u << 3),
    Start = (1u << 4),

    // 5~7はボタン以外のビット

    DpadLeft = (1u << 8),
    DpadRight = (1u << 9),
    DpadDown = (1u << 10),
    DpadUp = (1u << 11),

    Z = (1u << 12),
    R = (1u << 13),
    L = (1u << 14),
};

constexpr inline uint16_t to_mask(PadButton button) { return static_cast<uint16_t>(button); }

struct ButtonInput {
    bool a{false};
    bool b{false};
    bool x{false};
    bool y{false};
    bool start{false};

    bool dpad_left{false};
    bool dpad_right{false};
    bool dpad_down{false};
    bool dpad_up{false};

    bool z{false};
    bool r{false};
    bool l{false};
};

struct AnalogInput {
    // スティックの中心
    static constexpr uint8_t kAxisCenter{0x80};

    // トリガーが離されている状態
    static constexpr uint8_t kTriggerReleased{0x00};

    // （アナログ）ボタンが離されている状態
    static constexpr uint8_t kAnalogButtonReleased{0x00};

    // スティック（0..255, center=128）
    uint8_t stick_x{kAxisCenter};
    uint8_t stick_y{kAxisCenter};
    uint8_t c_stick_x{kAxisCenter};
    uint8_t c_stick_y{kAxisCenter};

    // アナログトリガー（0..255）
    uint8_t l_analog{kTriggerReleased};
    uint8_t r_analog{kTriggerReleased};
    // アナログボタン（未使用）
    uint8_t a_analog{kAnalogButtonReleased};
    uint8_t b_analog{kAnalogButtonReleased};
};

// プロジェクト内共通のコントローラ入力表現
struct PadInput {
    // ボタンのON/OFF
    ButtonInput buttons{};
    // アナログ入力の値
    AnalogInput analog{};

    constexpr bool pressed(PadButton button) const {
        switch (button) {
        case PadButton::A:
            return buttons.a;
        case PadButton::B:
            return buttons.b;
        case PadButton::X:
            return buttons.x;
        case PadButton::Y:
            return buttons.y;
        case PadButton::Start:
            return buttons.start;
        case PadButton::DpadLeft:
            return buttons.dpad_left;
        case PadButton::DpadRight:
            return buttons.dpad_right;
        case PadButton::DpadDown:
            return buttons.dpad_down;
        case PadButton::DpadUp:
            return buttons.dpad_up;
        case PadButton::Z:
            return buttons.z;
        case PadButton::R:
            return buttons.r;
        case PadButton::L:
            return buttons.l;
        default:
            return false;
        }
        return false;
    }

    constexpr void set(PadButton button, bool on = true) {
        switch (button) {
        case PadButton::A:
            buttons.a = on;
            break;
        case PadButton::B:
            buttons.b = on;
            break;
        case PadButton::X:
            buttons.x = on;
            break;
        case PadButton::Y:
            buttons.y = on;
            break;
        case PadButton::Start:
            buttons.start = on;
            break;
        case PadButton::DpadLeft:
            buttons.dpad_left = on;
            break;
        case PadButton::DpadRight:
            buttons.dpad_right = on;
            break;
        case PadButton::DpadDown:
            buttons.dpad_down = on;
            break;
        case PadButton::DpadUp:
            buttons.dpad_up = on;
            break;
        case PadButton::Z:
            buttons.z = on;
            break;
        case PadButton::R:
            buttons.r = on;
            break;
        case PadButton::L:
            buttons.l = on;
            break;
        default:
            break;
        }
    }

    constexpr void clear(PadButton button) { set(button, false); }

    constexpr void clear_buttons() { buttons = ButtonInput{}; }
};

// Status, Origin, Recalibrateレスポンスの共通形式。PollModeに依存しない。
struct PadState {
    PadReport report{};
    PadInput input{};
};

// ISRやダブルバッファでコピーしても安全
static_assert(std::is_trivially_copyable_v<PadInput>);
static_assert(std::is_standard_layout_v<PadInput>);
} // namespace gcinput::domain
