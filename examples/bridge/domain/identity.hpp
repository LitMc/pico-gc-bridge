#pragma once
#include "domain/mode.hpp"
#include "domain/report.hpp"
#include <cstdint>
#include <type_traits>

namespace gcinput::domain {

// コントローラのサポートする機能（不変）
struct PadIdentityCapabilities {
    bool is_gamecube{true};
    bool is_standard_controller{true};

    bool rumble_available{true};

    bool is_wireless{false};
    bool supports_wireless_receive{false};

    bool wireless_is_rf{false};
    bool wireless_state_fixed{false};
};

// コントローラの現在状態（変わりうる）
struct PadIdentityRuntime {
    PadReport report{};
    PollMode poll_mode{PollMode::Mode3};
    RumbleMode rumble_mode{RumbleMode::Off};
};

struct PadIdentity {
    PadIdentityCapabilities capabilities{};
    PadIdentityRuntime runtime{};
};

static_assert(std::is_trivially_copyable_v<PadIdentity>);
static_assert(std::is_standard_layout_v<PadIdentity>);

} // namespace gcinput::domain
