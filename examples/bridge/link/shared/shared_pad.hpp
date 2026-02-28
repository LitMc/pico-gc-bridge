#pragma once
#include "domain/identity.hpp"
#include "domain/report.hpp"
#include "domain/state.hpp"
#include "joybus/codec/identity_wire.hpp"
#include "joybus/codec/report_wire.hpp"
#include "joybus/codec/state_wire.hpp"
#include "joybus/protocol/protocol.hpp"
#include "link/policy.hpp"
#include "util/latch.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace gcinput {
struct PadSnapshot {
    uint32_t publish_count{0};
    joybus::Command last_rx_command{joybus::Command::Id};

    domain::PadIdentity identity{};
    domain::PadState status{};
    domain::PadState origin{};
};

class SharedPad {
  public:
    // パッドの最新スナップショットを得る
    PadSnapshot load() const { return latch_.load(); }

    // パッドからの応答を記録
    void on_response_isr(joybus::Command command, std::span<const uint8_t> rx) {
        bool got_valid_frame = false;
        switch (command) {
        case joybus::Command::Status: {
            if (rx.size() != joybus::kStatusResponseSize) {
                break;
            }
            auto view = std::span<const uint8_t, joybus::kStatusResponseSize>(rx);

            auto decoded =
                gcinput::joybus::state::decode_status(view, policy::kPadPollModeForQuery);
            shadow_.status.report = decoded.report;
            shadow_.status.input = decoded.input;
            got_valid_frame = true;
            break;
        }
        // OriginとRecalibrateは同じフォーマット
        case joybus::Command::Origin:
        case joybus::Command::Recalibrate: {
            if (rx.size() != joybus::kOriginResponseSize) {
                break;
            }
            auto view = std::span<const uint8_t, joybus::kOriginResponseSize>(rx);
            auto decoded = gcinput::joybus::state::decode_origin(view);
            shadow_.origin.report = decoded.report;
            shadow_.origin.input = decoded.input;
            got_valid_frame = true;
            break;
        }
        case joybus::Command::Id:
        case joybus::Command::Reset: {
            if (rx.size() != joybus::kIdResponseSize) {
                break;
            }
            auto view = std::span<const uint8_t, joybus::kIdResponseSize>(rx);
            joybus::identity::update_identity_from_id_bytes(shadow_.identity, view);
            got_valid_frame = true;
            break;
        }
        default:
            break;
        }

        if (got_valid_frame) {
            shadow_.publish_count++;
            shadow_.last_rx_command = command;
            latch_.publish(shadow_);
        }
    }

  private:
    PadSnapshot shadow_{};    // IRQでの書き込み専用
    Latch<PadSnapshot> latch_{}; // 外部から読み取る用
};

} // namespace gcinput
