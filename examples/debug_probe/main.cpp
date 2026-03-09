#include "debug_log.hpp"
#include "domain/transform/builtins.hpp"
#include "domain/transform/pipeline.hpp"
#include "hardware/pio.h"
#include "joybus/driver/joybus_pio_port.hpp"
#include "joybus_console.pio.h"
#include "joybus_pad.pio.h"
#include "link/console_client.hpp"
#include "link/pad_client.hpp"
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
        printf("[SYS] BOOTSEL button pressed. Entering USB boot mode.\n");
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

// ─── ヘルパー ───

const char *poll_mode_str(uint8_t pm) {
    switch (pm) {
    case 0: return "Mode0";
    case 1: return "Mode1";
    case 2: return "Mode2";
    case 3: return "Mode3";
    case 4: return "Mode4";
    default: return "Mode?";
    }
}

const char *rumble_mode_str(uint8_t rm) {
    switch (rm) {
    case 0: return "Off";
    case 1: return "On";
    case 2: return "Brake";
    default: return "?";
    }
}

// Id応答(3バイト)の簡易解釈
const char *id_description(const uint8_t *data, uint8_t len) {
    if (len >= 2) {
        uint16_t dev = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        if (dev == 0x0900) return "GC standard controller";
        if (dev == 0x0800) return "GC keyboard";
        if (dev == 0x0920) return "GC wireless controller";
    }
    return "unknown device";
}

// ボタン文字列の生成（押されているもののみ + でつなぐ）
// Status/Origin/Recalibrateの先頭2バイト(status word)からデコード
// Status wordのビットレイアウト (LE):
//   byte0: [OriginNotSent, ErrorLatched, Always1, UseCtrlOrigin, Start, Y, X, B]
//   byte1: [A, L-dig, R-dig, Z, DpadUp, DpadDown, DpadRight, DpadLeft]
void format_buttons(const uint8_t *status_word, char *buf, size_t buf_size) {
    struct { uint16_t mask; const char *name; } button_map[] = {
        {0x0001, "B"},
        {0x0002, "X"},
        {0x0004, "Y"},
        {0x0008, "Start"},
        {0x0100, "A"},
        {0x0200, "L"},
        {0x0400, "R"},
        {0x0800, "Z"},
        {0x1000, "DpadUp"},
        {0x2000, "DpadDown"},
        {0x4000, "DpadRight"},
        {0x8000, "DpadLeft"},
    };

    uint16_t sw = static_cast<uint16_t>(status_word[0]) | (static_cast<uint16_t>(status_word[1]) << 8);
    buf[0] = '\0';
    bool first = true;
    for (const auto &b : button_map) {
        if (sw & b.mask) {
            if (!first) {
                strncat(buf, "+", buf_size - strlen(buf) - 1);
            }
            strncat(buf, b.name, buf_size - strlen(buf) - 1);
            first = false;
        }
    }
    if (first) {
        strncpy(buf, "----", buf_size);
        buf[buf_size - 1] = '\0';
    }
}

// Origin/Recalibrate応答(10バイト)のデコード: stick, cstick, L, R
void format_origin_data(const uint8_t *data, uint8_t len, char *buf, size_t buf_size) {
    if (len >= 10) {
        char btn_buf[64];
        format_buttons(data, btn_buf, sizeof(btn_buf));
        snprintf(buf, buf_size, "stick=(%u,%u) cstick=(%u,%u) L=%u R=%u buttons=%s",
                 data[2], data[3], data[4], data[5], data[6], data[7], btn_buf);
    } else {
        // データ不足: hex dump
        buf[0] = '\0';
        for (uint8_t i = 0; i < len; i++) {
            char hex[4];
            snprintf(hex, sizeof(hex), "%s%02X", i > 0 ? " " : "", data[i]);
            strncat(buf, hex, buf_size - strlen(buf) - 1);
        }
    }
}

// Status応答(8バイト)のデコード: PollModeに応じて解釈を変える
void format_status_data(const uint8_t *data, uint8_t len, uint8_t pm, char *buf, size_t buf_size) {
    if (len < 8) {
        buf[0] = '\0';
        for (uint8_t i = 0; i < len; i++) {
            char hex[4];
            snprintf(hex, sizeof(hex), "%s%02X", i > 0 ? " " : "", data[i]);
            strncat(buf, hex, buf_size - strlen(buf) - 1);
        }
        return;
    }

    char btn_buf[64];
    format_buttons(data, btn_buf, sizeof(btn_buf));

    uint8_t stick_x = data[2], stick_y = data[3];
    uint8_t cx, cy, la, ra;

    switch (pm) {
    case 3: // Mode3: full c-stick, full L/R (no A/B)
        cx = data[4]; cy = data[5]; la = data[6]; ra = data[7];
        snprintf(buf, buf_size, "stick=(%u,%u) cstick=(%u,%u) L=%u R=%u buttons=%s",
                 stick_x, stick_y, cx, cy, la, ra, btn_buf);
        break;
    case 0: // Mode0: full c-stick, 4bit L/R/A/B
        cx = data[4]; cy = data[5];
        la = (data[6] >> 4) & 0x0F; ra = data[6] & 0x0F;
        snprintf(buf, buf_size, "stick=(%u,%u) cstick=(%u,%u) L=0x%02X R=0x%02X buttons=%s",
                 stick_x, stick_y, cx, cy, la, ra, btn_buf);
        break;
    case 1: // Mode1: 4bit c-stick, full L/R, 4bit A/B
        cx = (data[4] >> 4) & 0x0F; cy = data[4] & 0x0F;
        la = data[5]; ra = data[6];
        snprintf(buf, buf_size, "stick=(%u,%u) cstick=(0x%X,0x%X) L=%u R=%u buttons=%s",
                 stick_x, stick_y, cx, cy, la, ra, btn_buf);
        break;
    case 2: // Mode2: 4bit c-stick, 4bit L/R, full A/B
        cx = (data[4] >> 4) & 0x0F; cy = data[4] & 0x0F;
        la = (data[5] >> 4) & 0x0F; ra = data[5] & 0x0F;
        snprintf(buf, buf_size, "stick=(%u,%u) cstick=(0x%X,0x%X) L=0x%X R=0x%X buttons=%s",
                 stick_x, stick_y, cx, cy, la, ra, btn_buf);
        break;
    case 4: // Mode4: full c-stick, full A/B (no L/R)
        cx = data[4]; cy = data[5];
        snprintf(buf, buf_size, "stick=(%u,%u) cstick=(%u,%u) A=%u B=%u buttons=%s",
                 stick_x, stick_y, cx, cy, data[6], data[7], btn_buf);
        break;
    default:
        snprintf(buf, buf_size, "stick=(%u,%u) buttons=%s", stick_x, stick_y, btn_buf);
        break;
    }
}

// ─── ログ出力（平文） ───

void print_log_entry(const debug_log::LogEntry &e) {
    using debug_log::Port;
    using debug_log::Dir;

    const char *tag = (e.port == Port::Pad) ? "[PAD]" : "[CON]";

    if (e.is_state) {
        // 状態遷移: [PAD] from → to
        printf("%s %s\n", tag, e.state_str);
    } else if (e.is_timeout) {
        // タイムアウト: [PAD] TIMEOUT message
        printf("%s TIMEOUT %s\n", tag, e.state_str);
    } else {
        // データフレーム
        const char *cmd = debug_log::cmd_name(e.command_byte);

        if (e.port == Port::Pad && e.dir == Dir::TX) {
            // [PAD] >> Cmd [ModeN, Rumble=X] (N bytes)
            if (e.has_poll_mode) {
                printf("%s >> %s [%s, Rumble=%s] (%u bytes)\n", tag, cmd,
                       poll_mode_str(e.poll_mode), rumble_mode_str(e.rumble_mode), e.data_len);
            } else {
                printf("%s >> %s (%u bytes)\n", tag, cmd, e.data_len);
            }
        } else if (e.port == Port::Pad && e.dir == Dir::RX) {
            // [PAD] << Cmd: decoded_data
            char decoded[128];
            if (e.command_byte == 0x00) {
                // Id: hex + description
                char hex[32] = "";
                for (uint8_t i = 0; i < e.data_len; i++) {
                    char h[4];
                    snprintf(h, sizeof(h), "%s%02X", i > 0 ? " " : "", e.data[i]);
                    strncat(hex, h, sizeof(hex) - strlen(hex) - 1);
                }
                snprintf(decoded, sizeof(decoded), "%s (%s)", hex, id_description(e.data, e.data_len));
            } else if (e.command_byte == 0x41 || e.command_byte == 0x42) {
                // Origin/Recalibrate
                format_origin_data(e.data, e.data_len, decoded, sizeof(decoded));
            } else if (e.command_byte == 0x40) {
                // Status
                if (e.has_poll_mode) {
                    char status_data[128];
                    format_status_data(e.data, e.data_len, e.poll_mode, status_data, sizeof(status_data));
                    snprintf(decoded, sizeof(decoded), "[%s]: %s", poll_mode_str(e.poll_mode), status_data);
                } else {
                    format_status_data(e.data, e.data_len, 3, decoded, sizeof(decoded));
                }
            } else {
                // Unknown: hex dump
                decoded[0] = '\0';
                for (uint8_t i = 0; i < e.data_len; i++) {
                    char h[4];
                    snprintf(h, sizeof(h), "%s%02X", i > 0 ? " " : "", e.data[i]);
                    strncat(decoded, h, sizeof(decoded) - strlen(decoded) - 1);
                }
            }
            printf("%s << %s: %s\n", tag, cmd, decoded);
        } else if (e.port == Port::Console && e.dir == Dir::RX) {
            // [CON] << Cmd request  or  [CON] << Status [ModeN, Rumble=X]
            if (e.command_byte == 0x40 && e.has_poll_mode) {
                printf("%s << %s [%s, Rumble=%s]\n", tag, cmd,
                       poll_mode_str(e.poll_mode), rumble_mode_str(e.rumble_mode));
            } else {
                printf("%s << %s request\n", tag, cmd);
            }
        } else if (e.port == Port::Console && e.dir == Dir::TX) {
            // [CON] >> Cmd: decoded_data
            char decoded[128];
            if (e.command_byte == 0x00) {
                char hex[32] = "";
                for (uint8_t i = 0; i < e.data_len; i++) {
                    char h[4];
                    snprintf(h, sizeof(h), "%s%02X", i > 0 ? " " : "", e.data[i]);
                    strncat(hex, h, sizeof(hex) - strlen(hex) - 1);
                }
                snprintf(decoded, sizeof(decoded), "%s", hex);
            } else if (e.command_byte == 0x41 || e.command_byte == 0x42) {
                format_origin_data(e.data, e.data_len, decoded, sizeof(decoded));
            } else if (e.command_byte == 0x40) {
                if (e.has_poll_mode) {
                    char status_data[128];
                    format_status_data(e.data, e.data_len, e.poll_mode, status_data, sizeof(status_data));
                    snprintf(decoded, sizeof(decoded), "[%s]: %s", poll_mode_str(e.poll_mode), status_data);
                } else {
                    format_status_data(e.data, e.data_len, 3, decoded, sizeof(decoded));
                }
            } else {
                decoded[0] = '\0';
                for (uint8_t i = 0; i < e.data_len; i++) {
                    char h[4];
                    snprintf(h, sizeof(h), "%s%02X", i > 0 ? " " : "", e.data[i]);
                    strncat(decoded, h, sizeof(decoded) - strlen(decoded) - 1);
                }
            }
            printf("%s >> %s: %s\n", tag, cmd, decoded);
        }
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
    uint8_t  last_poll_mode;
    uint8_t  last_rumble_mode;

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
            } else if (e.port == Port::Console && e.dir == Dir::TX) {
                summary.con_tx_count++;
            }
            if (e.has_poll_mode) {
                summary.last_poll_mode = e.poll_mode;
                summary.last_rumble_mode = e.rumble_mode;
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

    printf("\n=== Debug Probe v2 ===\n\n");
    printf("[SYS] host_to_pad: PIO%d SM%u pin GP%u\n", pio_get_index(host_to_pad_config.pio),
           host_to_pad_config.state_machine, PIN_TO_REAL_PAD);
    printf("[SYS] device_to_console: PIO%d SM%u pin GP%u\n", pio_get_index(device_to_console_config.pio),
           device_to_console_config.state_machine, PIN_TO_REAL_CONSOLE);
    printf("[SYS] Waiting for controller...\n\n");

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

        // [POLL] サマリー出力（Ready中のみ、500ms間隔）
        if (in_ready_state &&
            (int32_t)(now_us - status_summary.interval_start_us) >= (int32_t)kSummaryIntervalUs) {
            if (status_summary.pad_tx_count > 0 || status_summary.con_tx_count > 0) {
                printf("[POLL] 500ms: PAD %lutx/%lurx/%luerr | CON %lurx/%lutx | %s Rumble=%s\n",
                       (unsigned long)status_summary.pad_tx_count,
                       (unsigned long)status_summary.pad_rx_count,
                       (unsigned long)status_summary.pad_timeout_count,
                       (unsigned long)status_summary.con_rx_count,
                       (unsigned long)status_summary.con_tx_count,
                       poll_mode_str(status_summary.last_poll_mode),
                       rumble_mode_str(status_summary.last_rumble_mode));
            }
            status_summary.reset(now_us);
        }

        // ドロップ警告
        if (debug_log::g_drop_count != last_reported_drops) {
            printf("[SYS] WARNING: Ring buffer dropped %lu entries\n",
                   (unsigned long)(debug_log::g_drop_count - last_reported_drops));
            last_reported_drops = debug_log::g_drop_count;
        }

        tight_loop_contents();
    }
}
