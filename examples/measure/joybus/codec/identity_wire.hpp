#pragma once
#include "domain/identity.hpp"
#include "domain/report.hpp"
#include "joybus/codec/common.hpp"
#include "joybus/codec/report_wire.hpp"
#include "joybus/protocol/protocol.hpp"
#include "joybus/protocol/reply.hpp"
#include <array>
#include <cstdint>
#include <span>

// コントローラのサポートする機能や実行時レポートをJoybusレスポンス形式に変換、復元するための処理群
namespace gcinput::joybus::identity {
// --- ID bytes1..2 (16bit) のビット定義（Joybus固有） ---
static constexpr uint16_t kIsWireless = (1u << 15);
static constexpr uint16_t kSupportsWirelessReceive = (1u << 14);
static constexpr uint16_t kRumbleNotAvailable = (1u << 13);
static constexpr uint16_t kIsGamecube = (1u << 11);     // 1=GC, 0=N64
static constexpr uint16_t kWirelessTypeRf = (1u << 10); // 1=RF, 0=IF
static constexpr uint16_t kWirelessStateFixed = (1u << 9);
static constexpr uint16_t kIsStandardController = (1u << 8);

// --- ID byte3 (8bit) のビット定義（Joybus固有） ---
static constexpr uint8_t kPollMask = 0x07u;   // bits[2:0]
static constexpr uint8_t kRumbleMask = 0x18u; // bits[4:3]

inline constexpr std::array<uint8_t, kIdResponseSize>
encode_identity_bytes(const domain::PadIdentity &id) {
    uint16_t device_capabilities = 0;

    const auto &capabilities = id.capabilities;

    if (capabilities.is_wireless) {
        device_capabilities |= kIsWireless;
    }
    if (capabilities.supports_wireless_receive) {
        device_capabilities |= kSupportsWirelessReceive;
    }
    if (!capabilities.rumble_available) {
        device_capabilities |= kRumbleNotAvailable;
    }
    if (capabilities.is_gamecube) {
        device_capabilities |= kIsGamecube;
    }
    if (capabilities.wireless_is_rf) {
        device_capabilities |= kWirelessTypeRf;
    }
    if (capabilities.wireless_state_fixed) {
        device_capabilities |= kWirelessStateFixed;
    }
    if (capabilities.is_standard_controller) {
        device_capabilities |= kIsStandardController;
    }

    uint8_t runtime_flags = 0;
    const auto &runtime = id.runtime;
    if (runtime.report.error_last) {
        runtime_flags |= report::to_mask(report::IdByte3Bits::ErrorLast);
    }
    if (runtime.report.error_latched) {
        runtime_flags |= report::to_mask(report::IdByte3Bits::ErrorLatched);
    }
    if (!runtime.report.origin_sent) {
        runtime_flags |= report::to_mask(report::IdByte3Bits::OriginNotSent);
    }
    const uint8_t poll_mode = joybus::clamp_poll_mode(static_cast<uint8_t>(runtime.poll_mode));
    const uint8_t rumble_mode =
        joybus::clamp_rumble_mode(static_cast<uint8_t>(runtime.rumble_mode));
    runtime_flags |= (rumble_mode << 3) & kRumbleMask;
    runtime_flags |= poll_mode & kPollMask;

    std::array<uint8_t, kIdResponseSize> out{};
    common::write_u16_le(device_capabilities, std::span<uint8_t, 2>{out.data(), 2});
    out[2] = runtime_flags;
    return out;
}

inline JoybusReply encode_identity(const domain::PadIdentity &id) {
    return JoybusReply{Command::Id, encode_identity_bytes(id)};
}

inline JoybusReply encode_reset_as_id(const domain::PadIdentity &id) {
    // ResetはIDと同じ形式
    return JoybusReply{Command::Reset, encode_identity_bytes(id)};
}

inline void update_capabilities_from_id_bytes(domain::PadIdentity &out,
                                              std::span<const uint8_t, kIdResponseSize> rx) {
    const uint16_t device_capabilities = common::read_u16_le(rx.first<2>());
    auto &capabilities = out.capabilities;
    capabilities.is_wireless = (device_capabilities & kIsWireless) != 0;
    capabilities.supports_wireless_receive = (device_capabilities & kSupportsWirelessReceive) != 0;
    capabilities.rumble_available = (device_capabilities & kRumbleNotAvailable) == 0;
    capabilities.is_gamecube = (device_capabilities & kIsGamecube) != 0;
    capabilities.wireless_is_rf = (device_capabilities & kWirelessTypeRf) != 0;
    capabilities.wireless_state_fixed = (device_capabilities & kWirelessStateFixed) != 0;
    capabilities.is_standard_controller = (device_capabilities & kIsStandardController) != 0;
}

inline void update_runtime_from_id_byte3(domain::PadIdentity &out, uint8_t byte3) {
    auto &runtime = out.runtime;
    const uint8_t runtime_flags = byte3;
    runtime.poll_mode =
        static_cast<domain::PollMode>(joybus::clamp_poll_mode(runtime_flags & kPollMask));
    runtime.rumble_mode = static_cast<domain::RumbleMode>(
        joybus::clamp_rumble_mode((runtime_flags & kRumbleMask) >> 3));
    report::update_report_from_id_byte3(runtime.report, byte3);
}

inline void update_identity_from_id_bytes(domain::PadIdentity &out,
                                          std::span<const uint8_t, kIdResponseSize> rx) {
    update_capabilities_from_id_bytes(out, rx);
    update_runtime_from_id_byte3(out, rx[2]);
}

} // namespace gcinput::joybus::identity
