#include "debug_log.hpp"
#include "domain/state.hpp"
#include "domain/transform/builtins.hpp"
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

// ─── ピン定義 ───
constexpr uint ONBOARD_LED_PIN = PICO_DEFAULT_LED_PIN;
constexpr uint BOOT_BTN_PIN = 26; // GP26
constexpr uint PIN_TO_REAL_PAD = 15;
constexpr uint PIN_TO_REAL_CONSOLE = 16;

// ─── BOOTSELボタン ───
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
    busy_wait_ms(100);
    if (gpio_get(BOOT_BTN_PIN) == 0) {
        printf("M,%lu,BOOTSEL button pressed. Entering USB boot mode\n",
               (unsigned long)time_us_32());
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

// ─── ログ出力 ───

void print_csv_header() {
    printf("# debug_probe v1\n");
    printf("# format: T,timestamp_us,port,dir,len,hex_data[,pm=P{n}/C{n}/R{n}]\n");
    printf("# format: S,timestamp_us,from_state,to_state\n");
    printf("# format: M,timestamp_us,message\n");
    printf("# format: U,timestamp_us,port,polls,ok,timeout\n");
}

void print_log_entry(const debug_log::LogEntry &e) {
    using debug_log::Port;
    using debug_log::Dir;

    const char port_char = (e.port == Port::Pad) ? 'P' : 'C';

    if (e.is_state) {
        // S行: 状態遷移 — state_str は "from -> to" 形式
        // " -> " で分割して from/to を取得
        char buf[48];
        strncpy(buf, e.state_str, sizeof(buf));
        buf[sizeof(buf) - 1] = '\0';
        char *arrow = strstr(buf, " -> ");
        if (arrow) {
            *arrow = '\0';
            const char *from_state = buf;
            const char *to_state = arrow + 4;
            printf("S,%lu,%s,%s\n", (unsigned long)e.timestamp_us, from_state, to_state);
        } else {
            // フォールバック: パース不能ならM行で出力
            printf("M,%lu,%s\n", (unsigned long)e.timestamp_us, e.state_str);
        }
    } else if (e.is_timeout) {
        // M行: タイムアウトメッセージ
        printf("M,%lu,%c TIMEOUT %s\n", (unsigned long)e.timestamp_us, port_char, e.state_str);
    } else {
        // T行: データフレーム
        const char dir_char = (e.dir == Dir::TX) ? 'T' : 'R';
        printf("T,%lu,%c,%c,%u,", (unsigned long)e.timestamp_us, port_char, dir_char, e.data_len);
        for (uint8_t i = 0; i < e.data_len; i++) {
            if (i > 0) printf(" ");
            printf("%02X", e.data[i]);
        }
        if (e.has_poll_mode) {
            printf(",pm=P%u/C%u/R%u",
                   e.poll_mode_pad, e.poll_mode_console, e.poll_mode_reply);
        }
        printf("\n");
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

    void reset(uint32_t now_us) {
        pad_tx_count = 0;
        pad_rx_count = 0;
        pad_timeout_count = 0;
        con_rx_count = 0;
        con_tx_count = 0;
        interval_start_us = now_us;
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
    bootsel_button_init();
    init_led();

    // PIOステートマシンの確保
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

    // パイプラインなし（素通しブリッジ）
    // Origin/Recalibrate のみニュートラル固定を適用
    auto &pipelines = client_link.transform_pipelines();
    const auto &fix_origin_to_neutral = gcinput::domain::transform::builtins::fix_origin_to_neutral;
    pipelines.origin.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));
    pipelines.recalibrate.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));
    // Status パイプラインは空（素通し）

    gcinput::PadClient pad_client(host_to_pad_config, client_link);
    gcinput::ConsoleClient console_client(device_to_console_config, client_link);

    print_csv_header();
    printf("M,0,Debug Probe firmware ready\n");
    printf("M,0,host_to_pad: PIO%d SM%u pin GP%u\n", pio_get_index(host_to_pad_config.pio),
           host_to_pad_config.state_machine, PIN_TO_REAL_PAD);
    printf("M,0,device_to_console: PIO%d SM%u pin GP%u\n", pio_get_index(device_to_console_config.pio),
           device_to_console_config.state_machine, PIN_TO_REAL_CONSOLE);

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
        pad_client.tick(now_us, console_state);

        // Ready状態の検出
        const bool ready_now = client_link.is_pad_ready();
        if (ready_now && !was_ready) {
            in_ready_state = true;
            status_summary.reset(now_us);
        } else if (!ready_now && was_ready) {
            in_ready_state = false;
        }
        was_ready = ready_now;

        // リングバッファドレイン
        drain_ring(in_ready_state, status_summary);

        // Statusポーリングサマリー出力（Ready中のみ、500ms間隔）
        if (in_ready_state &&
            (int32_t)(now_us - status_summary.interval_start_us) >= (int32_t)kSummaryIntervalUs) {
            if (status_summary.pad_tx_count > 0 || status_summary.con_tx_count > 0) {
                printf("U,%lu,P,%lu,%lu,%lu\n",
                       (unsigned long)now_us, (unsigned long)status_summary.pad_tx_count,
                       (unsigned long)status_summary.pad_rx_count,
                       (unsigned long)status_summary.pad_timeout_count);
                printf("U,%lu,C,%lu,%lu,0\n",
                       (unsigned long)now_us, (unsigned long)status_summary.con_rx_count,
                       (unsigned long)status_summary.con_tx_count);
            }
            status_summary.reset(now_us);
        }

        // ドロップ警告
        if (debug_log::g_drop_count != last_reported_drops) {
            printf("M,%lu,WARNING Ring buffer dropped %lu entries\n",
                   (unsigned long)now_us,
                   (unsigned long)(debug_log::g_drop_count - last_reported_drops));
            last_reported_drops = debug_log::g_drop_count;
        }

        tight_loop_contents();
    }
}
