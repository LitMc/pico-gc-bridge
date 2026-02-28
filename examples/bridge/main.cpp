#include "domain/state.hpp"
#include "domain/transform/builtins.hpp"
#include "domain/transform/correction.hpp"
#include "domain/transform/pipeline.hpp"
#include "hardware/pio.h"
#include "joybus/driver/joybus_pio_port.hpp"
#include "joybus_console.pio.h"
#include "joybus_pad.pio.h"
#include "link/console_client.hpp"
#include "link/pad_client.hpp"
#include "link/shared/shared_pad_hub.hpp"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include <cstdio>

namespace {
// 通電確認用のオンボードLED
constexpr uint ONBOARD_LED_PIN = PICO_DEFAULT_LED_PIN;
// BOOTSELに入るためのボタン入力
constexpr uint BOOT_BTN_PIN = 26; // GP26
// JoyBus
constexpr uint PIN_TO_REAL_PAD = 15;
constexpr uint PIN_TO_REAL_CONSOLE = 16;

// BOOTSELボタン押下をIRQからメインループへ伝えるためのフラグ
volatile bool g_boot_btn_requested = false;

void boot_btn_irq(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    g_boot_btn_requested = true;
}

void handle_boot_btn_if_requested() {
    if (!g_boot_btn_requested) {
        return;
    }
    g_boot_btn_requested = false;

    // デバウンス: 依然として押下中かを確認
    busy_wait_ms(100);
    if (gpio_get(BOOT_BTN_PIN) == 0) {
        printf("BOOTSEL button pressed. Entering USB boot mode...\n");
        reset_usb_boot(0, 0);
    }
}

void bootsel_button_init() {
    gpio_init(BOOT_BTN_PIN);
    gpio_set_dir(BOOT_BTN_PIN, GPIO_IN);
    gpio_pull_up(BOOT_BTN_PIN);
    gpio_set_irq_enabled_with_callback(BOOT_BTN_PIN, GPIO_IRQ_EDGE_FALL, true, &boot_btn_irq);
}

void init_led() {
    gpio_init(ONBOARD_LED_PIN);
    gpio_set_dir(ONBOARD_LED_PIN, GPIO_OUT);
    gpio_put(ONBOARD_LED_PIN, 1);
}

// 原点正規化コンテキスト（main から更新、ISR から参照）
gcinput::domain::transform::correction::OriginOffsetContext origin_ctx{};

// 振動パターン再生（モード切替通知用）
struct RumbleOverride {
    uint8_t remaining_pulses{0};
    bool motor_on{false};
    uint32_t phase_start_us{0};

    static constexpr uint32_t kOnDurationUs = 150'000;
    static constexpr uint32_t kOffDurationUs = 100'000;

    void start(uint8_t pulses, uint32_t now_us) {
        remaining_pulses = pulses;
        motor_on = true;
        phase_start_us = now_us;
    }

    gcinput::domain::RumbleMode tick(uint32_t now_us) {
        if (!motor_on && remaining_pulses == 0) {
            return gcinput::domain::RumbleMode::Off;
        }
        const uint32_t elapsed = now_us - phase_start_us;
        if (motor_on) {
            if (elapsed >= kOnDurationUs) {
                motor_on = false;
                phase_start_us = now_us;
                if (remaining_pulses > 0) {
                    --remaining_pulses;
                }
            }
            return gcinput::domain::RumbleMode::On;
        }
        // motor_on == false, remaining_pulses > 0 → OFF ギャップ中
        if (elapsed >= kOffDurationUs) {
            motor_on = true;
            phase_start_us = now_us;
        }
        return gcinput::domain::RumbleMode::Off;
    }
};
} // namespace

int main() {
    stdio_init_all();

    // ボタンを押すだけでBOOTSELに入るようにする
    bootsel_button_init();

    // 動作開始の確認用にオンボードLEDを光らせる
    init_led();

    // コンソールとパッドそれぞれのステートマシンを確保
    PIO host_to_pad_pio = pio0;
    PIO device_to_console_pio = pio1;
    const uint sm_host_to_pad = pio_claim_unused_sm(host_to_pad_pio, true);
    const uint sm_device_to_host = pio_claim_unused_sm(device_to_console_pio, true);

    gcinput::JoybusPioPort::Config host_to_pad_config{
        .pio = host_to_pad_pio,
        .state_machine = sm_host_to_pad,
        .pin = PIN_TO_REAL_PAD,
        .program = &joybus_console_program,
        .get_default_config = &joybus_console_program_get_default_config,
        .rx_start_offset = joybus_console_offset_rx_start,
        .tx_start_offset = joybus_console_offset_tx_start,
        .pio_hz = 4'000'000,
        .irq_base = 0,
    };

    gcinput::JoybusPioPort::Config device_to_console_config{
        .pio = device_to_console_pio,
        .state_machine = sm_device_to_host,
        .pin = PIN_TO_REAL_CONSOLE,
        .program = &joybus_pad_program,
        .get_default_config = &joybus_pad_program_get_default_config,
        .rx_start_offset = joybus_pad_offset_rx_start,
        .tx_start_offset = joybus_pad_offset_tx_start,
        .pio_hz = 4'000'000,
        .irq_base = 0,
    };

    gcinput::PadConsoleLink client_link{};

    // 入力変換パイプライン
    auto &pipelines = client_link.transform_pipelines();

    // Origin/Recalibrate: ニュートラル固定
    const auto &fix_origin_to_neutral = gcinput::domain::transform::builtins::fix_origin_to_neutral;
    pipelines.origin.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));
    pipelines.recalibrate.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));

    // Status パイプライン:
    // Stage 0: fix_origin_to_neutral — 原点確定フェーズで有効（接続直後）
    // Stage 1-4: 補正パイプライン — 補正フェーズで有効（ボタン入力後）
    using namespace gcinput::domain::transform::correction;
    constexpr std::size_t kStageFixOrigin = 0;
    constexpr std::size_t kStageCorrectionFirst = 1;
    constexpr std::size_t kStageCorrectionLast = 4;

    // Stage 0: 原点確定用ニュートラル固定（起動時有効）
    pipelines.status.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));

    // Stage 1-4: 補正パイプライン P(s) = S⁻¹⁺(φ(C(s)))（起動時無効）
    pipelines.status.add_stage(
        gcinput::domain::transform::make_stage<OriginOffsetContext, origin_normalize>(origin_ctx));
    pipelines.status.add_stage(gcinput::domain::transform::make_stage(&octagon_clamp));
    pipelines.status.add_stage(gcinput::domain::transform::make_stage(&linear_scale));
    pipelines.status.add_stage(gcinput::domain::transform::make_stage(&inverse_lut));

    // 補正ステージを初期状態では無効にする
    for (std::size_t i = kStageCorrectionFirst; i <= kStageCorrectionLast; ++i) {
        pipelines.status.set_stage_enabled(i, false);
    }

    gcinput::PadClient pad_client(host_to_pad_config, client_link);
    gcinput::ConsoleClient console_client(device_to_console_config, client_link);

    printf("Bridge firmware ready.\n");
    printf("Mode: origin_fix (L+R+DUp+Start+Y to activate correction)\n");
    printf("host_to_pad: PIO%d SM%u pin GP%u\n", pio_get_index(host_to_pad_config.pio),
           host_to_pad_config.state_machine, PIN_TO_REAL_PAD);
    printf("device_to_console: PIO%d SM%u pin GP%u\n", pio_get_index(device_to_console_config.pio),
           device_to_console_config.state_machine, PIN_TO_REAL_CONSOLE);

    // モード管理
    // origin_fix: 接続直後。Status に (128,128) を返してコンソールに原点を確定させる
    // correction: 補正パイプラインが有効。L+R+DpadUp+Start+Y で切り替え
    enum class BridgeMode : uint8_t { OriginFix, Correction };
    BridgeMode mode = BridgeMode::OriginFix;

    bool is_pad_connected = false;
    bool prev_combo = false;
    RumbleOverride rumble_override{};
    uint32_t last_origin_publish_count = 0;
    uint32_t last_tx_publish_count = 0;
    uint32_t last_debug_log_us = 0;
    constexpr uint32_t kDebugLogIntervalUs = 500'000; // 500ms間隔

    while (true) {
        handle_boot_btn_if_requested();

        const uint32_t now_us = time_us_32();
        auto console_state = client_link.shared_console().load();
        const auto rumble = rumble_override.tick(now_us);
        if (rumble != gcinput::domain::RumbleMode::Off) {
            console_state.rumble_mode = rumble;
        }
        pad_client.tick(now_us, console_state);

        // Origin/Recalibrate 受信時に原点コンテキストを更新
        const auto snapshot = client_link.real_pad_hub().load_original_snapshot();
        if (snapshot.publish_count != last_origin_publish_count) {
            last_origin_publish_count = snapshot.publish_count;
            if (snapshot.last_rx_command == gcinput::joybus::Command::Origin ||
                snapshot.last_rx_command == gcinput::joybus::Command::Recalibrate) {
                const auto ox = snapshot.origin.input.analog.stick_x;
                const auto oy = snapshot.origin.input.analog.stick_y;
                origin_ctx.origin_x.store(ox, std::memory_order_release);
                origin_ctx.origin_y.store(oy, std::memory_order_release);
                printf("Origin updated: (%u, %u)\n", ox, oy);
            }
        }

        // モード切替: L+R+DpadUp+Start+Y 同時押しでトグル
        if (snapshot.last_rx_command == gcinput::joybus::Command::Status) {
            const auto &input = snapshot.status.input;
            const bool combo_held = input.pressed(gcinput::domain::PadButton::L) &&
                                    input.pressed(gcinput::domain::PadButton::R) &&
                                    input.pressed(gcinput::domain::PadButton::DpadUp) &&
                                    input.pressed(gcinput::domain::PadButton::Start) &&
                                    input.pressed(gcinput::domain::PadButton::Y);

            if (combo_held && !prev_combo) {
                if (mode == BridgeMode::OriginFix) {
                    mode = BridgeMode::Correction;
                    pipelines.status.set_stage_enabled(kStageFixOrigin, false);
                    for (std::size_t i = kStageCorrectionFirst; i <= kStageCorrectionLast; ++i) {
                        pipelines.status.set_stage_enabled(i, true);
                    }
                    rumble_override.start(1, now_us);
                    printf("Mode: correction (pipeline active)\n");
                } else {
                    mode = BridgeMode::OriginFix;
                    pipelines.status.set_stage_enabled(kStageFixOrigin, true);
                    for (std::size_t i = kStageCorrectionFirst; i <= kStageCorrectionLast; ++i) {
                        pipelines.status.set_stage_enabled(i, false);
                    }
                    rumble_override.start(2, now_us);
                    printf("Mode: origin_fix (L+R+DUp+Start+Y to activate correction)\n");
                }
            }
            prev_combo = combo_held;
        }

        // デバッグログ: 各ステージの中間値を出力
        gcinput::TxPair last_tx{};
        if (client_link.active_pad_hub().consume_tx_if_new(last_tx_publish_count, last_tx)) {
            if (last_tx.raw.command() == gcinput::joybus::Command::Status &&
                (int32_t)(now_us - last_debug_log_us) >= (int32_t)kDebugLogIntervalUs) {
                last_debug_log_us = now_us;

                // ISR が送った最終値（ワイヤフォーマットから読み取り）
                const auto modified_view = last_tx.modified.view();
                const uint8_t tx_sx = (modified_view.size() >= 3) ? modified_view[2] : 0;
                const uint8_t tx_sy = (modified_view.size() >= 4) ? modified_view[3] : 0;

                // 生の入力をスナップショットから取得
                const auto raw_state = snapshot.status;
                const uint8_t raw_x = raw_state.input.analog.stick_x;
                const uint8_t raw_y = raw_state.input.analog.stick_y;

                // 現在の Origin
                const uint8_t ox = origin_ctx.origin_x.load(std::memory_order_acquire);
                const uint8_t oy = origin_ctx.origin_y.load(std::memory_order_acquire);

                // 各ステージを再計算
                gcinput::domain::PadState s = raw_state;

                // Stage 1: origin_normalize
                origin_normalize(origin_ctx, s);
                const uint8_t norm_x = s.input.analog.stick_x;
                const uint8_t norm_y = s.input.analog.stick_y;

                // Stage 2: octagon_clamp
                octagon_clamp(nullptr, s);
                const uint8_t clamp_x = s.input.analog.stick_x;
                const uint8_t clamp_y = s.input.analog.stick_y;

                // Stage 3: linear_scale
                linear_scale(nullptr, s);
                const uint8_t scale_x = s.input.analog.stick_x;
                const uint8_t scale_y = s.input.analog.stick_y;

                // Stage 4: inverse_lut
                inverse_lut(nullptr, s);
                const uint8_t lut_x = s.input.analog.stick_x;
                const uint8_t lut_y = s.input.analog.stick_y;

                // S(tx): コンソールが実際に受け取る期待値
                const auto [stx_x, stx_y] = forward_lut(tx_sx, tx_sy);

                printf("DBG [%s] origin=(%3u,%3u) raw=(%3u,%3u) norm=(%3u,%3u) clamp=(%3u,%3u) "
                       "scale=(%3u,%3u) lut=(%3u,%3u) tx=(%3u,%3u) S(tx)=(%3u,%3u)\n",
                       mode == BridgeMode::Correction ? "COR" : "FIX", ox, oy, raw_x, raw_y,
                       norm_x, norm_y, clamp_x, clamp_y, scale_x, scale_y, lut_x, lut_y, tx_sx,
                       tx_sy, stx_x, stx_y);
            }
        }

        const bool ready = client_link.is_pad_ready();
        if (!is_pad_connected && ready) {
            printf("PadClient: console responses enabled.\n");
            is_pad_connected = true;
        } else if (is_pad_connected && !ready) {
            printf("PadClient: console responses disabled.\n");
            is_pad_connected = false;
        }
        tight_loop_contents();
    }
}
