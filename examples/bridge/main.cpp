#include "debug_log.hpp"
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
#include <cstring>

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

// ─── PadClient状態名 ───
const char *pad_state_name(gcinput::PadClient::State s) {
    switch (s) {
    case gcinput::PadClient::State::Disconnected:     return "Disconnected";
    case gcinput::PadClient::State::Resetting:        return "Resetting";
    case gcinput::PadClient::State::BootId:           return "BootId";
    case gcinput::PadClient::State::BootOrigin:       return "BootOrigin";
    case gcinput::PadClient::State::BootRecalibrate:  return "BootRecalibrate";
    case gcinput::PadClient::State::WarmStatus:       return "WarmStatus";
    case gcinput::PadClient::State::Ready:            return "Ready";
    case gcinput::PadClient::State::RelayOrigin:      return "RelayOrigin";
    case gcinput::PadClient::State::RelayRecalibrate: return "RelayRecalibrate";
    default: return "?";
    }
}

// ─── 平文ログ出力 ───

void print_log_entry(const debug_log::LogEntry &e) {
    using debug_log::Port;
    using debug_log::Dir;

    const char *port_tag = (e.port == Port::Pad) ? "[PAD]" : "[CON]";

    if (e.is_state) {
        printf("%s %s\n", port_tag, e.state_str);
    } else if (e.is_timeout) {
        printf("%s TIMEOUT %s\n", port_tag, e.state_str);
    } else {
        const char *dir_str = (e.dir == Dir::TX) ? ">>" : "<<";
        const char *cmd = debug_log::cmd_name(e.command_byte);
        printf("%s %s %s", port_tag, dir_str, cmd);
        if (e.data_len > 0) {
            printf(":");
            for (uint8_t i = 0; i < e.data_len; i++) {
                printf(" %02X", e.data[i]);
            }
        }
        if (e.has_poll_mode) {
            printf(" (Mode%u Rumble=%u)", e.poll_mode, e.rumble_mode);
        }
        printf(" (%u bytes)\n", e.data_len);
    }
}

// ─── Ready状態用 Statusポーリングサマリー ───
struct StatusSummary {
    uint32_t pad_tx_count;
    uint32_t pad_rx_count;
    uint32_t pad_timeout_count;
    uint32_t con_rx_count;
    uint32_t con_tx_count;
    uint32_t interval_start_us;
    uint8_t last_poll_mode;
    uint8_t last_rumble_mode;

    void reset(uint32_t now_us) {
        pad_tx_count = 0;
        pad_rx_count = 0;
        pad_timeout_count = 0;
        con_rx_count = 0;
        con_tx_count = 0;
        interval_start_us = now_us;
        last_poll_mode = 0;
        last_rumble_mode = 0;
    }
};

constexpr uint32_t kSummaryIntervalUs = 500'000; // 500ms

// mainループでのドレイン
// Ready状態のStatusコマンドはサマリーに集計、それ以外は全出力
void drain_ring(bool in_ready_state, StatusSummary &summary) {
    using debug_log::Port;
    using debug_log::Dir;

    while (debug_log::g_ring_tail != debug_log::g_ring_head) {
        const debug_log::LogEntry &e = debug_log::g_ring[debug_log::g_ring_tail];
        debug_log::g_ring_tail = (debug_log::g_ring_tail + 1) % debug_log::kRingSize;

        // Ready状態のStatusコマンドデータログはサマリーに集計
        if (in_ready_state && !e.is_state && !e.is_timeout && e.command_byte == 0x40) {
            if (e.port == Port::Pad && e.dir == Dir::TX) {
                summary.pad_tx_count++;
            } else if (e.port == Port::Pad && e.dir == Dir::RX) {
                summary.pad_rx_count++;
            } else if (e.port == Port::Console && e.dir == Dir::RX) {
                summary.con_rx_count++;
                if (e.has_poll_mode) {
                    summary.last_poll_mode = e.poll_mode;
                }
            } else if (e.port == Port::Console && e.dir == Dir::TX) {
                summary.con_tx_count++;
            }
            continue;
        }

        // タイムアウトログもReady Statusならサマリーにカウントのみ
        if (in_ready_state && e.is_timeout && e.command_byte == 0x40) {
            summary.pad_timeout_count++;
            continue;
        }

        print_log_entry(e);
    }
}

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

    gcinput::BridgeContext client_link{};

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

#ifdef GCINPUT_ENABLE_LOG
    printf("=== Bridge ready ===\n");
    printf("Mode: origin_fix (L+R+DUp+Start+Y to activate correction)\n");
    printf("host_to_pad: PIO%d SM%u pin GP%u\n", pio_get_index(host_to_pad_config.pio),
           host_to_pad_config.state_machine, PIN_TO_REAL_PAD);
    printf("device_to_console: PIO%d SM%u pin GP%u\n", pio_get_index(device_to_console_config.pio),
           device_to_console_config.state_machine, PIN_TO_REAL_CONSOLE);
#endif

    // モード管理
    // origin_fix: 接続直後。Status に (128,128) を返してコンソールに原点を確定させる
    // correction: 補正パイプラインが有効。L+R+DpadUp+Start+Y で切り替え
    enum class BridgeMode : uint8_t { OriginFix, Correction };
    BridgeMode mode = BridgeMode::OriginFix;

    bool prev_combo = false;
    RumbleOverride rumble_override{};
    uint32_t last_origin_publish_count = 0;
    uint32_t last_tx_publish_count = 0;
    uint32_t last_debug_log_us = 0;
    constexpr uint32_t kDebugLogIntervalUs = 500'000; // 500ms間隔

    // PAD状態変化検知
    auto last_pad_state = gcinput::PadClient::State::Disconnected;

    // Statusポーリングサマリー
    StatusSummary status_summary{};
    status_summary.reset(0);
    bool in_ready_state = false;
    bool was_ready = false;

    // ドロップカウント表示用
    uint32_t last_reported_drops = 0;

    while (true) {
        handle_boot_btn_if_requested();

        const uint32_t now_us = time_us_32();
        auto console_state = client_link.shared_console().load();
        const auto rumble = rumble_override.tick(now_us);
        if (rumble != gcinput::domain::RumbleMode::Off) {
            console_state.rumble_mode = rumble;
        }
        pad_client.tick(now_us, console_state);

        // PAD状態変化の検知とログ出力
        auto cur_state = pad_client.current_state();
        if (cur_state != last_pad_state) {
#ifdef GCINPUT_ENABLE_LOG
            printf("[PAD] state: %s -> %s\n", pad_state_name(last_pad_state),
                   pad_state_name(cur_state));
#endif
            last_pad_state = cur_state;
#ifdef GCINPUT_ENABLE_LOG
            if (cur_state == gcinput::PadClient::State::Ready) {
                printf("[PAD] Controller connected.\n");
            }
#endif
        }

        // Ready状態の検出
        const bool ready_now = (cur_state == gcinput::PadClient::State::Ready);
        if (ready_now && !was_ready) {
            in_ready_state = true;
            status_summary.reset(now_us);
        } else if (!ready_now && was_ready) {
            in_ready_state = false;
        }
        was_ready = ready_now;

        // リングバッファドレイン
        drain_ring(in_ready_state, status_summary);

#ifdef GCINPUT_ENABLE_LOG
        // [POLL]サマリー出力（Ready中のみ、500ms間隔）
        if (in_ready_state &&
            (int32_t)(now_us - status_summary.interval_start_us) >= (int32_t)kSummaryIntervalUs) {
            if (status_summary.pad_tx_count > 0 || status_summary.con_tx_count > 0) {
                printf("[POLL] %lums: PAD %lutx/%lurx/%luerr | CON %lurx/%lutx | Mode%u Rumble=%s\n",
                       (unsigned long)(kSummaryIntervalUs / 1000),
                       (unsigned long)status_summary.pad_tx_count,
                       (unsigned long)status_summary.pad_rx_count,
                       (unsigned long)status_summary.pad_timeout_count,
                       (unsigned long)status_summary.con_rx_count,
                       (unsigned long)status_summary.con_tx_count,
                       status_summary.last_poll_mode,
                       (console_state.rumble_mode == gcinput::domain::RumbleMode::On) ? "On" : "Off");
            }
            status_summary.reset(now_us);
        }

        // ドロップ警告
        if (debug_log::g_drop_count != last_reported_drops) {
            printf("WARNING: Ring buffer dropped %lu entries\n",
                   (unsigned long)(debug_log::g_drop_count - last_reported_drops));
            last_reported_drops = debug_log::g_drop_count;
        }
#endif

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
#ifdef GCINPUT_ENABLE_LOG
                printf("Origin updated: (%u, %u)\n", ox, oy);
#endif
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
#ifdef GCINPUT_ENABLE_LOG
                    printf("Mode: correction (pipeline active)\n");
#endif
                } else {
                    mode = BridgeMode::OriginFix;
                    pipelines.status.set_stage_enabled(kStageFixOrigin, true);
                    for (std::size_t i = kStageCorrectionFirst; i <= kStageCorrectionLast; ++i) {
                        pipelines.status.set_stage_enabled(i, false);
                    }
                    rumble_override.start(2, now_us);
#ifdef GCINPUT_ENABLE_LOG
                    printf("Mode: origin_fix (L+R+DUp+Start+Y to activate correction)\n");
#endif
                }
            }
            prev_combo = combo_held;
        }

        // デバッグログ: 各ステージの中間値を出力
        gcinput::TxRecord last_tx{};
        if (client_link.real_pad_hub().consume_tx_if_new(last_tx_publish_count, last_tx)) {
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

#ifdef GCINPUT_ENABLE_LOG
                printf("DBG [%s] origin=(%3u,%3u) raw=(%3u,%3u) norm=(%3u,%3u) clamp=(%3u,%3u) "
                       "scale=(%3u,%3u) lut=(%3u,%3u) tx=(%3u,%3u) S(tx)=(%3u,%3u)\n",
                       mode == BridgeMode::Correction ? "COR" : "FIX", ox, oy, raw_x, raw_y,
                       norm_x, norm_y, clamp_x, clamp_y, scale_x, scale_y, lut_x, lut_y, tx_sx,
                       tx_sy, stx_x, stx_y);
#endif
            }
        }

        tight_loop_contents();
    }
}
