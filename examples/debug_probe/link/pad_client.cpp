#include "link/pad_client.hpp"
#include "debug_log.hpp"
#include "link/policy.hpp"

namespace gcinput {
void PadClient::load_reset_epoch_() { last_reset_epoch_ = link_.load_reset_epoch(); }

void PadClient::on_pad_response_isr(joybus::Command command, std::span<const uint8_t> rx) {
    // PAD RX: パッドからの応答をログ
    debug_log::ring_push_data(debug_log::Port::Pad, debug_log::Dir::RX,
                              static_cast<uint8_t>(command), rx.data(), rx.size());
    link_.real_pad_hub().on_pad_response_isr(command, rx);
}

std::size_t PadClient::callback(void *user, const uint8_t *rx, std::size_t rx_len, uint8_t *tx,
                                std::size_t tx_max) {
    auto *self = static_cast<PadClient *>(user);
    const auto command =
        static_cast<joybus::Command>(self->await_command_.load(std::memory_order_acquire));
    if (!joybus::is_valid_command(command)) {
        // 取り扱うべきでないコマンド
        return 0;
    }
    self->on_pad_response_isr(command, std::span<const uint8_t>(rx, rx_len));
    // Picoからコントローラへの応答は不要
    return 0;
}

static const char *state_name(PadClient::State s) {
    switch (s) {
    case PadClient::State::Disconnected:     return "Disconnected";
    case PadClient::State::Resetting:        return "Resetting";
    case PadClient::State::BootId:           return "BootId";
    case PadClient::State::BootOrigin:       return "BootOrigin";
    case PadClient::State::BootRecalibrate:  return "BootRecalibrate";
    case PadClient::State::WarmStatus:       return "WarmStatus";
    case PadClient::State::Ready:            return "Ready";
    case PadClient::State::RelayOrigin:      return "RelayOrigin";
    case PadClient::State::RelayRecalibrate: return "RelayRecalibrate";
    default: return "?";
    }
}

void PadClient::enter_state_(State next) {
    // 状態遷移ログ
    char msg[48];
    snprintf(msg, sizeof(msg), "%s -> %s", state_name(state_), state_name(next));
    debug_log::ring_push_state(debug_log::Port::Pad, msg);

    state_ = next;
    abort_wait_();
    publish_pad_state_to_link_();
}

void PadClient::abort_wait_() {
    await_command_.store(static_cast<uint8_t>(joybus::Command::Invalid), std::memory_order_release);
    response_deadline_us_ = 0;
}

void PadClient::tick(uint32_t now_us, const ConsoleState &console) {
    const auto pad_snapshot = link_.real_pad_hub().load_original_snapshot();

    // パッドからの応答があれば最後に来た時刻を更新
    if (pad_snapshot.publish_count != last_publish_count_) {
        last_publish_count_ = pad_snapshot.publish_count;
        last_seen_us_ = now_us;
    }

    // 最近応答があったならまだ生きている
    const bool pad_alive =
        (last_seen_us_ != 0) && !is_timeout_reached_(now_us, last_seen_us_ + kPadTimeoutUs);

    // 繋がっていたコントローラとの接続が切れた
    if (!pad_alive && state_ != State::Disconnected) {
        debug_log::ring_push_timeout(debug_log::Port::Pad, "pad connection lost");
        enter_state_(State::Disconnected);
        next_status_due_us_ = 0;
    }

    // 本体からResetが来ていたらリセット待ちに入る
    if (pending_console_reset_() && state_ != State::Disconnected && state_ != State::Resetting) {
        // Resetのboot sequenceでOrigin/Recalibrateも取得するため中継epochを消化
        load_origin_epoch_();
        load_recalibrate_epoch_();
        enter_state_(State::Resetting);
    }

    const bool pad_has_response = (pad_snapshot.publish_count != await_publish_count_);
    // 指定のコマンドの応答が来たか
    auto got = [&](joybus::Command command) {
        return waiting_response_() && (awaiting_command_() == command) && pad_has_response &&
               ((pad_snapshot.last_rx_command == command));
    };

    switch (state_) {
    // 接続確立
    case State::Disconnected: {
        // 応答待ちでなければID取得から始める
        if (!waiting_response_()) {
            send_request_(joybus::Id, now_us, kBootTimeoutUs);
            break;
        }

        if (got(joybus::Command::Id)) {
            // IDの応答が来たら次はOrigin
            enter_state_(State::BootOrigin);
        } else if (is_timeout_reached_(now_us, response_deadline_us_)) {
            debug_log::ring_push_timeout(debug_log::Port::Pad, "waiting for Id");
            abort_wait_();
        }
        break;
    }
    // 再初期化
    case State::Resetting: {
        if (!waiting_response_()) {
            send_request_(joybus::Reset, now_us, kBootTimeoutUs);
            break;
        }

        if (got(joybus::Command::Reset)) {
            load_reset_epoch_();
            enter_state_(State::BootId);
        } else if (is_timeout_reached_(now_us, response_deadline_us_)) {
            debug_log::ring_push_timeout(debug_log::Port::Pad, "waiting for Reset");
            abort_wait_();
        }
        break;
    }
    // 初回ID取得
    case State::BootId: {
        if (!waiting_response_()) {
            send_request_(joybus::Id, now_us, kBootTimeoutUs);
            break;
        }
        if (got(joybus::Command::Id)) {
            enter_state_(State::BootOrigin);
        } else if (is_timeout_reached_(now_us, response_deadline_us_)) {
            debug_log::ring_push_timeout(debug_log::Port::Pad, "waiting for Id (boot)");
            abort_wait_();
        }
        break;
    }
    // 初回Origin取得
    case State::BootOrigin: {
        if (!waiting_response_()) {
            send_request_(joybus::Origin, now_us, kBootTimeoutUs);
            break;
        }
        if (got(joybus::Command::Origin)) {
            enter_state_(State::BootRecalibrate);
        } else if (is_timeout_reached_(now_us, response_deadline_us_)) {
            debug_log::ring_push_timeout(debug_log::Port::Pad, "waiting for Origin");
            abort_wait_();
        }
        break;
    }
    // 初回Recalibrate取得
    case State::BootRecalibrate: {
        if (!waiting_response_()) {
            send_request_(joybus::Recalibrate, now_us, kBootTimeoutUs);
            break;
        }
        if (got(joybus::Command::Recalibrate)) {
            enter_state_(State::WarmStatus);
        } else if (is_timeout_reached_(now_us, response_deadline_us_)) {
            debug_log::ring_push_timeout(debug_log::Port::Pad, "waiting for Recalibrate");
            abort_wait_();
        }
        break;
    }
    // 初回Status取得
    case State::WarmStatus: {
        if (!waiting_response_()) {
            // スティックとLRトリガーの分解能を重視してMode3に固定
            // Mode3ではアナログAとBが犠牲になるが実機で使われていないので都合がいい
            const auto req = joybus::Status(policy::kPadPollModeForQuery, console.rumble_mode);
            send_request_(req, now_us, kBootTimeoutUs);
            break;
        }

        if (got(joybus::Command::Status)) {
            enter_state_(State::Ready);
            next_status_due_us_ = now_us + kStatusPeriodUs;
        } else if (is_timeout_reached_(now_us, response_deadline_us_)) {
            debug_log::ring_push_timeout(debug_log::Port::Pad, "waiting for Status (warm)");
            abort_wait_();
        }
        break;
    }
    // 定期Status送信
    case State::Ready: {
        if (pending_console_reset_()) {
            // もし本体からResetが来ていたらリセット待ちに入る
            load_origin_epoch_();
            load_recalibrate_epoch_();
            enter_state_(State::Resetting);
            break;
        }

        // 本体からRecalibrate中継要求が来ていたらパッドへ中継
        if (pending_console_recalibrate_()) {
            enter_state_(State::RelayRecalibrate);
            break;
        }

        // 本体からOrigin中継要求が来ていたらパッドへ中継
        if (pending_console_origin_()) {
            enter_state_(State::RelayOrigin);
            break;
        }

        if (waiting_response_()) {
            if (got(joybus::Command::Status)) {
                next_status_due_us_ = now_us + kStatusPeriodUs;
                abort_wait_();
            } else if (is_timeout_reached_(now_us, response_deadline_us_)) {
                debug_log::ring_push_timeout(debug_log::Port::Pad, "waiting for Status");
                next_status_due_us_ = now_us + kRetryDelayUs;
                abort_wait_();
            }
            break;
        }

        if (next_status_due_us_ == 0 || is_timeout_reached_(now_us, next_status_due_us_)) {
            // Mode3固定
            const auto req = joybus::Status(policy::kPadPollModeForQuery, console.rumble_mode);
            if (send_request_(req, now_us, kBootTimeoutUs)) {
                // 送信できたら次の送信予定をセット
                next_status_due_us_ = now_us + kStatusPeriodUs;
            } else {
                // 送信できなかったら次のtickで再試行
                next_status_due_us_ = now_us + kRetryDelayUs;
            }
        }
        break;
    }
    // 本体からのOriginをパッドへ中継
    case State::RelayOrigin: {
        if (!waiting_response_()) {
            send_request_(joybus::Origin, now_us, kBootTimeoutUs);
            break;
        }
        if (got(joybus::Command::Origin)) {
            enter_state_(State::Ready);
            next_status_due_us_ = now_us + kStatusPeriodUs;
        } else if (is_timeout_reached_(now_us, response_deadline_us_)) {
            debug_log::ring_push_timeout(debug_log::Port::Pad, "waiting for Origin (relay)");
            // タイムアウトしてもパッドは接続済みなのでReadyに戻る
            enter_state_(State::Ready);
            next_status_due_us_ = now_us + kRetryDelayUs;
        }
        break;
    }
    // 本体からのRecalibrateをパッドへ中継
    case State::RelayRecalibrate: {
        if (!waiting_response_()) {
            send_request_(joybus::Recalibrate, now_us, kBootTimeoutUs);
            break;
        }
        if (got(joybus::Command::Recalibrate)) {
            enter_state_(State::Ready);
            next_status_due_us_ = now_us + kStatusPeriodUs;
        } else if (is_timeout_reached_(now_us, response_deadline_us_)) {
            debug_log::ring_push_timeout(debug_log::Port::Pad, "waiting for Recalibrate (relay)");
            // タイムアウトしてもパッドは接続済みなのでReadyに戻る
            enter_state_(State::Ready);
            next_status_due_us_ = now_us + kRetryDelayUs;
        }
        break;
    }
    }
}
} // namespace gcinput
