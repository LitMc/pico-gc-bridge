#pragma once
#include "joybus/driver/joybus_pio_port.hpp"
#include "joybus/protocol/protocol.hpp"
#include "link/pad_console_link.hpp"
#include "link/shared/shared_console.hpp"
#include "link/shared/shared_pad_hub.hpp"
#include <atomic>
#include <span>

namespace gcinput {
class PadClient {
  public:
    explicit PadClient(JoybusPioPort::Config host_to_pad_config, PadConsoleLink &link)
        : link_{link}, host_to_pad_(host_to_pad_config, &gcinput::PadClient::callback, this) {
        last_reset_epoch_ = link_.load_reset_epoch();
    };

    // mainループから呼ぶ（非ブロッキング）
    void tick(uint32_t now_us, const ConsoleState &console);

    // Padからの応答を処理するISRコールバック
    void on_pad_response_isr(joybus::Command command, std::span<const uint8_t> rx);

    // パッドからの応答を受信したときに呼ぶコールバック
    static std::size_t callback(void *user, const uint8_t *rx, std::size_t rx_len, uint8_t *tx,
                                std::size_t tx_max);

    // パッドの状態
    enum class State : uint8_t {
        Disconnected,
        Resetting,       // 本体からのResetをコントローラに伝えて再初期化
        BootId,          // 初回のID取得待ち
        BootOrigin,      // 初回のOrigin取得待ち
        BootRecalibrate, // 初回のRecalibrate取得待ち
        WarmStatus,      // 初回のStatus取得待ち
        Ready,           // Statusのポーリング開始済み
    };

  private:
    template <std::size_t N>
    bool send_request_(const joybus::Request<N> &request, uint32_t now_us, uint32_t timeout_us) {
        if (waiting_response_()) {
            return false;
        }
        if (request.bytes().empty()) {
            return false;
        }

        // 送信前に待ち条件を確定
        response_deadline_us_ = now_us + timeout_us; // この時刻までに応答が来なければタイムアウト
        await_command_.store(static_cast<uint8_t>(request.command()),
                             std::memory_order_release); // このコマンドを待つ

        const auto bytes = request.bytes();

        // 送信直前にpublish_countを読み取り、送信後に来た応答だけを受け付ける
        const auto before_publish_count =
            link_.real_pad_hub().load_original_snapshot().publish_count;
        await_publish_count_ = before_publish_count; // このカウントからずれたら応答あり

        bool send_ok = host_to_pad_.send_now(bytes.data(), bytes.size());
        if (!send_ok) {
            abort_wait_();
            return false;
        }

        return true;
    }

    // 本体からのReset要求エポックを更新
    void load_reset_epoch_();

    // 次の状態へ遷移
    void enter_state_(State next);

    // 応答待ち状態を強制解除して再送できるようにする
    void abort_wait_();

    // コマンド送信後の応答待ち時間が経過したか
    static bool is_timeout_reached(uint32_t now_us, uint32_t deadline_us) {
        // deadline_us = deadline設定時のnow + timeout_us
        // timeout_usが2^31未満なら判定時点のnow_usがラップしていても正しく判定できる
        // reach前の差分 = ラップ直前の大きな値をとるnow_us - ラップ直後の小さな値をとるdeadline_us
        // この差分の最上位ビットに1が立っていれば符号付き整数の負の値になる
        // 実際0xFFFFFFB0 - 0x00000020 = 0xFFFFFF90 (負の値)のような感じで負になる
        // 差分が0x80000000以上だとこの判定は壊れるがtimeout_usを2^31未満にすれば起こらない
        // 2^31usも待たないので大丈夫
        return (int32_t)(now_us - deadline_us) >= 0;
    }

    // コンソール側クライアントへパッド状態を公開
    void publish_pad_state_to_link() {
        switch (state_) {
        case State::Ready:
            link_.publish_pad_state_from_main(PadConsoleLink::PadConnectionState::Ready);
            break;
        case State::BootId:
        case State::BootOrigin:
        case State::BootRecalibrate:
        case State::WarmStatus:
            link_.publish_pad_state_from_main(PadConsoleLink::PadConnectionState::Booting);
            break;
        case State::Disconnected:
        case State::Resetting:
        default:
            link_.publish_pad_state_from_main(PadConsoleLink::PadConnectionState::Disconnected);
            break;
        }
    }

  private:
    PadConsoleLink &link_;
    JoybusPioPort host_to_pad_;

    State state_{State::Disconnected};

    // この送信以降にpublish_countが増えたら応答が来たと判断する基準
    uint32_t await_publish_count_{0};
    // 応答待ちのタイムアウト時間
    uint32_t response_deadline_us_{0};

    std::atomic<uint8_t> await_command_{static_cast<uint8_t>(joybus::Command::Invalid)};

    // 応答を待っているコマンド
    joybus::Command awaiting_command_() const {
        return static_cast<joybus::Command>(await_command_.load(std::memory_order_acquire));
    }

    // 応答待ちか否か
    bool waiting_response_() const { return joybus::is_valid_command(awaiting_command_()); }

    // alive判定
    uint32_t last_seen_us_{0};
    uint32_t last_publish_count_{0};

    // 本体からReset要求を受けた回数（いつのReset要求まで捌いたか判別用エポック）
    uint32_t last_reset_epoch_{0};

    // 本体からのReset要求があればtrue、なければfalse
    bool pending_console_reset_() { return link_.consume_pad_reset_request(last_reset_epoch_); }

    // Ready中のStatus送信間隔
    uint32_t next_status_due_us_{0};

    // 最後の応答からこれ以上経過するとパッド切断とみなす時間
    static constexpr uint32_t PAD_TIMEOUT_US = 100'000;

    // 初回接続時の応答待ちタイムアウト時間
    static constexpr uint32_t BOOT_TIMEOUT_US = 30'000;

    // 二重送信の心配はないので送信可能になるまで最速でポーリングをかけ続ける（入力遅延を減らすため）
    // Statusのポーリング周期
    static constexpr uint32_t STATUS_PERIOD_US = 0;
    // 送信失敗後にリトライするまでの待ち時間
    static constexpr uint32_t RETRY_DELAY_US = 0;
};
} // namespace gcinput
