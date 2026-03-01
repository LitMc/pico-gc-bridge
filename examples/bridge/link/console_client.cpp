#include "link/console_client.hpp"
#include "domain/transform/pipeline.hpp"
#include "joybus/codec/identity_wire.hpp"
#include "joybus/codec/state_wire.hpp"

namespace gcinput {

std::size_t ConsoleClient::write_tx(const joybus::JoybusReply &reply, uint8_t *tx, std::size_t tx_max) {
    const auto view = reply.view();
    const auto length = view.size();
    if (length == 0 || tx_max < length) {
        return 0;
    }
    std::copy_n(view.data(), length, tx);
    return length;
}

std::size_t ConsoleClient::callback(void *user, const uint8_t *rx, std::size_t rx_len, uint8_t *tx,
                                    std::size_t tx_max) {
    if (rx_len < 1) {
        return 0;
    }

    auto *self = static_cast<ConsoleClient *>(user);
    self->link_.shared_console().on_request_isr(std::span<const uint8_t>(rx, rx_len));

    if (!self->link_.is_pad_ready()) {
        return 0;
    }

    auto &pad_hub = self->link_.real_pad_hub();
    const auto original_snapshot = pad_hub.load_original_snapshot();

    const auto cmd = static_cast<joybus::Command>(rx[0]);

    // コンソールに指定されたPollModeとRumbleModeを応答に使う
    const auto host_console = self->link_.shared_console().load();
    joybus::PollMode host_poll_mode = host_console.poll_mode;
    joybus::RumbleMode host_rumble_mode = host_console.rumble_mode;

    joybus::JoybusReply original_reply;
    joybus::JoybusReply modified_reply;

    const auto &pipelines = self->link_.transform_pipelines();
    switch (cmd) {
    case joybus::Command::Status: {
        const domain::PadState original_state = original_snapshot.status;
        original_reply = joybus::state::encode_status(original_state, host_poll_mode);

        domain::PadState modified_state = original_state;
        pipelines.status.apply_from_isr(modified_state);
        modified_reply = joybus::state::encode_status(modified_state, host_poll_mode);
        break;
    }
    case joybus::Command::Origin: {
        // パッドへOrigin中継を要求
        self->link_.publish_pad_origin_request_from_isr();

        const domain::PadState original_state = original_snapshot.origin;
        original_reply = joybus::state::encode_origin(original_state);
        domain::PadState modified_state = original_state;
        pipelines.origin.apply_from_isr(modified_state);
        modified_reply = joybus::state::encode_origin(modified_state);
        break;
    }
    case joybus::Command::Recalibrate: {
        // パッドへRecalibrate中継を要求
        self->link_.publish_pad_recalibrate_request_from_isr();

        const domain::PadState original_state = original_snapshot.origin;
        original_reply = joybus::state::encode_recalibrate(original_state);
        domain::PadState modified_state = original_state;
        pipelines.recalibrate.apply_from_isr(modified_state);
        modified_reply = joybus::state::encode_recalibrate(modified_state);
        break;
    }
    case joybus::Command::Id: {
        domain::PadIdentity identity = original_snapshot.identity;
        // 直近のコンソールから指定されたPollModeとRumbleModeを反映する
        // パッドへのポーリングはMode3固定でコンソールへの応答はコンソールからの指示に従う仕様のため
        identity.runtime.poll_mode = host_poll_mode;
        identity.runtime.rumble_mode = host_rumble_mode;
        original_reply = joybus::identity::encode_identity(identity);
        // Identityは変換する意義が薄いのでそのまま返す
        modified_reply = original_reply;
        break;
    }
    case joybus::Command::Reset: {
        // パッドへリセットを要求
        self->link_.publish_pad_reset_request_from_isr();

        // Idと同じ
        domain::PadIdentity identity = original_snapshot.identity;
        identity.runtime.poll_mode = host_poll_mode;
        identity.runtime.rumble_mode = host_rumble_mode;
        original_reply = joybus::identity::encode_reset_as_id(identity);
        modified_reply = original_reply;
        break;
    }
    default:
        return 0;
    }

    if (original_reply.view().empty()) {
        return 0;
    }

    const std::size_t tx_len = self->write_tx(modified_reply, tx, tx_max);
    if (tx_len == 0) {
        return 0;
    }

    pad_hub.publish_tx_from_isr(original_snapshot.publish_count, original_reply, modified_reply);
    return tx_len;
}

} // namespace gcinput
