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
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include <array>
#include <span>
#include <stdio.h>

namespace {
// オンボードLED
constexpr uint ONBOARD_LED_PIN = PICO_DEFAULT_LED_PIN;
// BOOTSELボタン
constexpr uint BOOT_BTN_PIN = 26; // GP26
// JoyBus
constexpr uint PIN_TO_REAL_PAD = 15;
constexpr uint PIN_TO_REAL_CONSOLE = 16;

void boot_btn_irq(uint gpio, uint32_t events) {
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

// CRC-8 ATM (poly=0x07, init=0x00) — 8バイト版
static uint8_t crc8(const std::array<uint8_t, 8> &data) {
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
} // namespace

int main() {
    stdio_init_all();

    bootsel_button_init();
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

    // 入力変換処理
    auto &pipelines = client_link.transform_pipelines();
    const auto &fix_origin_to_neutral = gcinput::domain::transform::builtins::fix_origin_to_neutral;
    pipelines.origin.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));
    pipelines.recalibrate.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));
    pipelines.status.add_stage(gcinput::domain::transform::make_stage(&fix_origin_to_neutral));

    gcinput::PadClient pad_client(host_to_pad_config, client_link);
    gcinput::ConsoleClient console_client(device_to_console_config, client_link);

    printf("input_viewer ready.\n");
    printf("host_to_pad: PIO%d SM%u pin GP%u\n", pio_get_index(host_to_pad_config.pio),
           host_to_pad_config.state_machine, PIN_TO_REAL_PAD);
    printf("device_to_console: PIO%d SM%u pin GP%u\n", pio_get_index(device_to_console_config.pio),
           device_to_console_config.state_machine, PIN_TO_REAL_CONSOLE);

    bool is_pad_connected = false;
    uint32_t last_tx_publish_count = client_link.active_pad_hub().load_last_tx().publish_count;

    while (true) {
        pad_client.tick(time_us_32(), client_link.shared_console().load());

        gcinput::TxRecord last_tx = client_link.active_pad_hub().load_last_tx();
        if (client_link.active_pad_hub().consume_tx_if_new(last_tx_publish_count, last_tx)) {
            last_tx_publish_count = last_tx.publish_count;
            const auto command = last_tx.raw.command();
            if (command == gcinput::joybus::Command::Status) {
                const auto status = last_tx.modified.view();
                if (status.size() < 8) {
                    continue;
                }
                // wireバイト: [0]=BH, [1]=BL, [2]=SX, [3]=SY, [4]=CX, [5]=CY, [6]=LT, [7]=RT
                const uint8_t bh = status[0];
                const uint8_t bl = status[1];
                const uint8_t sx = status[2];
                const uint8_t sy = status[3];
                const uint8_t cx = status[4];
                const uint8_t cy = status[5];
                const uint8_t lt = status[6];
                const uint8_t rt = status[7];

                std::array<uint8_t, 8> crc_data{bh, bl, sx, sy, cx, cy, lt, rt};
                const uint8_t cc = crc8(crc_data);

                printf("I,%02X,%02X,%u,%u,%u,%u,%u,%u,%02X\n",
                       bh, bl, sx, sy, cx, cy, lt, rt, cc);
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
