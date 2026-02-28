#pragma once
#include "joybus/codec/common.hpp"
#include "joybus/codec/identity_wire.hpp"
#include "joybus/codec/state_wire.hpp"
#include "link/pad_console_link.hpp"

// テスト用に初期応答を流し込む
namespace gcinput::measure {

struct SeedOptions {
    bool status{true};
    bool origin{true};
    bool recalibrate{true};
    bool id{true};
    bool reset{true};
};

inline domain::PadState make_neutral_pad_state() {
    // 初期値と変わらないが明示
    domain::PadState state{};
    state.input.clear_buttons();
    state.input.set_analog_neutral();
    state.report = domain::PadReport{};
    return state;
}

inline domain::PadIdentity make_default_pad_identity_from_console(const ConsoleState &console) {
    domain::PadIdentity id{};
    id.runtime.poll_mode = joybus::common::to_domain_poll_mode(console.poll_mode);
    id.runtime.rumble_mode = joybus::common::to_domain_rumble_mode(console.rumble_mode);
    return id;
}

// テスト用PadHubにコントローラ応答を流す
inline void feed_reply_to_hub(SharedPadHub &hub, const JoybusReply &reply) {
    if (reply.command() == joybus::Command::Invalid) {
        return;
    }

    const auto view = reply.view();
    if (view.empty()) {
        return;
    }
    hub.on_pad_response_isr(reply.command(), view);
}

// テスト開始直後のOriginで困らないよう初期応答をセットする
inline void seed_initial_responses(PadConsoleLink &link, const ConsoleState &console,
                                   SeedOptions options = {}) {
    auto &hub = link.measure_pad_hub();

    const domain::PadState neutral = make_neutral_pad_state();

    if (options.status) {
        const auto reply = joybus::state::encode_status(neutral, console.poll_mode);
        feed_reply_to_hub(hub, reply);
    }
    if (options.origin) {
        const auto reply = joybus::state::encode_origin(neutral);
        feed_reply_to_hub(hub, reply);
    }
    if (options.recalibrate) {
        const auto reply = joybus::state::encode_recalibrate(neutral);
        feed_reply_to_hub(hub, reply);
    }
    if (options.id || options.reset) {
        const auto id = make_default_pad_identity_from_console(console);
        if (options.id) {
            const auto reply = joybus::identity::encode_identity(id);
            feed_reply_to_hub(hub, reply);
        }
        if (options.reset) {
            const auto reply = joybus::identity::encode_reset_as_id(id);
            feed_reply_to_hub(hub, reply);
        }
    }
}

} // namespace gcinput::measure
