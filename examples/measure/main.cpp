#include "domain/state.hpp"
#include "domain/transform/builtins.hpp"
#include "domain/transform/pipeline.hpp"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "joybus/driver/joybus_pio_port.hpp"
#include "joybus_console.pio.h"
#include "joybus_pad.pio.h"
#include "link/console_client.hpp"
#include "link/pad_client.hpp"
#include "link/shared/shared_pad_hub.hpp"
#include "measure/pad_injector.hpp"
#include "measure/patterns/stick_grid_sweep.hpp"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include <array>
#include <span>
#include <stdio.h>

namespace {
// 通電確認用のオンボードLED
constexpr uint ONBOARD_LED_PIN = PICO_DEFAULT_LED_PIN;
// BOOTSELに入るためのボタン入力
constexpr uint BOOT_BTN_PIN = 26; // GP26
// JoyBus
constexpr uint PIN_TO_REAL_PAD = 15;
constexpr uint PIN_TO_REAL_CONSOLE = 16;

void boot_btn_irq(uint gpio, uint32_t events) {
    // ちょいデバウンス（押しっぱなし連打対策）
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

static uint8_t crc8(const std::array<uint8_t, 4> &data) {
    uint8_t crc = 0x00;
    for (std::size_t i = 0; i < data.size(); ++i) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; ++j) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}
struct WireByteOffsets {
    uint8_t first;
    uint8_t second;
};

constexpr WireByteOffsets
wire_offsets_for_target(gcinput::measure::StickGridSweep::Target target) {
    using Target = gcinput::measure::StickGridSweep::Target;
    switch (target) {
    case Target::Joystick: return {2, 3}; // stick_x, stick_y
    case Target::Cstick:   return {4, 5}; // c_stick_x, c_stick_y
    case Target::Trigger:  return {6, 7}; // l_analog, r_analog
    }
    return {2, 3};
}

// 計測対象の選択（コンパイル時）
// ターゲットを変えたらリビルドするだけで走査対象が切り替わる
constexpr auto kMeasureTarget = gcinput::measure::StickGridSweep::Target::Joystick;

// --- 構成例 ---
// スティック 2D 全走査:        target=Joystick, x=[0,255], y=[0,255]  (65536点)
// Cスティック 2D 全走査:       target=Cstick,   x=[0,255], y=[0,255]  (65536点)
// Lトリガー 1D 走査 (R=0固定): target=Trigger,  x=[0,255], y=[0,0]    (256点)
// Rトリガー 1D 走査 (L=0固定): target=Trigger,  x=[0,0],   y=[0,255]  (256点)
// LRトリガー 2D 全走査:        target=Trigger,  x=[0,255], y=[0,255]  (65536点)

constexpr auto kWireOffsets = wire_offsets_for_target(kMeasureTarget);
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

    // 入力変換処理
    auto &pipelines = client_link.transform_pipelines();
    const auto &fix_origin_to_neutral = gcinput::domain::transform::builtins::fix_origin_to_neutral;
    pipelines.origin.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));
    pipelines.recalibrate.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));
    pipelines.status.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));

    gcinput::PadClient pad_client(host_to_pad_config, client_link);

    // 計測
    const uint32_t interval_1f = 16'667; // 約60Hz
    gcinput::measure::Schedule schedule{gcinput::measure::ScheduleConfig{
        .interval_us = interval_1f * 10,
        .catch_up = false,
    }};

    gcinput::measure::StickGridSweep pattern{gcinput::measure::StickGridSweep::Config{
        .x = {.begin = 0, .end = 255, .step = 1},
        .y = {.begin = 0, .end = 255, .step = 1},
        .loop = true,
        .target = kMeasureTarget,
    }};

    gcinput::measure::PadInjector pad_injector(client_link, schedule, pattern);

    gcinput::ConsoleClient console_client(device_to_console_config, client_link);

    printf("JoybusPioPort ready.\n");
    printf("host_to_pad: PIO%d SM%u pin GP%u\n", pio_get_index(host_to_pad_config.pio),
           host_to_pad_config.state_machine, PIN_TO_REAL_PAD);
    printf("device_to_console: PIO%d SM%u pin GP%u\n", pio_get_index(device_to_console_config.pio),
           device_to_console_config.state_machine, PIN_TO_REAL_CONSOLE);

    bool is_pad_connected = false;

    uint32_t last_tx_publish_count = client_link.active_pad_hub().load_last_tx().publish_count;

    uint32_t last_measure_epoch = client_link.load_measure_epoch();

    uint32_t frame_count = 0;
    std::pair<uint8_t, uint8_t> last_analog{128, 128};

    while (true) {
        pad_client.tick(time_us_32(), client_link.shared_console().load());
        pad_injector.tick(time_us_32());

        const auto real_pad_snapshot = client_link.real_pad_hub().load_original_snapshot();
        if (real_pad_snapshot.last_rx_command == gcinput::joybus::Command::Status) {
            const bool measure_enable =
                real_pad_snapshot.status.input.pressed(gcinput::domain::PadButton::Z);
            const bool measure_disable =
                real_pad_snapshot.status.input.pressed(gcinput::domain::PadButton::DpadUp);

            if (measure_enable && !client_link.is_measure_enabled()) {
                frame_count = 0;
                client_link.enable_measure_from_main();
            } else if (measure_disable && client_link.is_measure_enabled()) {
                client_link.disable_measure_from_main();
            }
        }

        if (client_link.consume_measure_epoch(last_measure_epoch)) {
            last_tx_publish_count = client_link.active_pad_hub().load_last_tx().publish_count;
            printf("PadInjector: sending fixed patterns %s.\n",
                   client_link.is_measure_enabled() ? "enabled" : "disabled");
        }

        gcinput::TxPair last_tx = client_link.active_pad_hub().load_last_tx();
        if (client_link.active_pad_hub().consume_tx_if_new(last_tx_publish_count, last_tx)) {
            last_tx_publish_count = last_tx.publish_count;
            const auto raw = last_tx.raw;
            const auto modified = last_tx.modified;
            const auto command = raw.command();
            if (command == gcinput::joybus::Command::Status && client_link.is_measure_enabled()) {
                const auto status = modified.view();
                if (status.size() < 8) {
                    continue;
                }
                std::pair<uint8_t, uint8_t> current_analog{
                    status[kWireOffsets.first],
                    status[kWireOffsets.second],
                };
                if (current_analog != last_analog) {
                    last_analog = current_analog;
                    std::array<uint8_t, 4> crc_data{
                        static_cast<uint8_t>((frame_count >> 8) & 0xFF),
                        static_cast<uint8_t>(frame_count & 0xFF),
                        current_analog.first,
                        current_analog.second,
                    };
                    const uint8_t crc = crc8(crc_data);
                    printf("D,%u,%u,%u,%02X\n", frame_count, current_analog.first,
                           current_analog.second, crc);
                    frame_count = (frame_count + 1) % 65536;
                }
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
