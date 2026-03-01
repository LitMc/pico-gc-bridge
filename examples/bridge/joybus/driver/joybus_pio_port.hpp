#pragma once
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gcinput {

class JoybusPioPort {
  public:
    static constexpr std::size_t kMaxFrameBytes = 16;
    // RX: stop(0x01) を末尾に 1byte 追加で受け取る
    static constexpr std::size_t kRxBufferSize = kMaxFrameBytes + 1;
    // TX: stopはPIO側で生成するのでデータ分のみ
    static constexpr std::size_t kTxBufferSize = kMaxFrameBytes;

    struct Config {
        PIO pio = nullptr;
        uint state_machine = 0;
        uint pin = 0;

        const pio_program_t *program = nullptr;
        pio_sm_config (*get_default_config)(uint offset) = nullptr;

        // generated header の *_offset_* をそのまま入れる（= program先頭からの相対）
        uint rx_start_offset = 0;
        uint tx_start_offset = 0;

        float pio_hz = 4'000'000;

        // PIO側が `irq set 0 rel` 前提なら base=0 でOK（実IRQ=(base+sm)&7）
        uint irq_base = 0;
    };

    // 返信生成用コールバック
    // 戻り値: 返信(tx)のバイト数（0なら返信なし）
    using PacketCallback = std::size_t (*)(void *user, const uint8_t *rx, std::size_t rx_len,
                                           uint8_t *tx, std::size_t tx_max);

    explicit JoybusPioPort(const Config &config, PacketCallback callback, void *user);
    ~JoybusPioPort();

    JoybusPioPort(const JoybusPioPort &) = delete;
    JoybusPioPort &operator=(const JoybusPioPort &) = delete;
    JoybusPioPort(JoybusPioPort &&) = delete;
    JoybusPioPort &operator=(JoybusPioPort &&) = delete;

    void __time_critical_func(start_receive)();

    // デバッグ/テスト用：直近のRX結果
    bool rx_ready() const { return rx_ready_.load(); }
    bool rx_bad() const { return rx_bad_.load(); }
    uint32_t rx_length() const { return rx_length_.load(); }
    const uint8_t *rx_data() const { return received_frame_.data(); }

    void clear_rx_status() {
        rx_ready_.store(false);
        rx_bad_.store(false);
        rx_length_.store(0);
    }

    // テスト用: 手動で1フレーム送信
    bool __time_critical_func(send_now)(const uint8_t *data, std::size_t nbytes);

  private:
    // 実行時間を安定させるため応答までに呼ばれる処理はRAMに置く
    void __time_critical_func(finish_receive_from_irq)();
    void __time_critical_func(start_transmit_from_irq)(std::size_t nbytes);
    void __time_critical_func(on_pio_irq)();

    struct IrqMux;
    static IrqMux &irq_mux();
    static void __isr __time_critical_func(pio0_irq0_handler)();
    static void __isr __time_critical_func(pio1_irq0_handler)();

    uint irq_index() const { return (config_.irq_base + config_.state_machine) & 7u; }
    uint rx_start_pc() const { return program_offset_ + config_.rx_start_offset; }
    uint tx_start_pc() const { return program_offset_ + config_.tx_start_offset; }

  private:
    Config config_{};
    uint program_offset_ = 0;

    int dma_channel_ = -1;
    dma_channel_config dma_rx_config_{};
    dma_channel_config dma_tx_config_{};

    std::atomic<bool> tx_busy_{false};

    std::array<uint8_t, kRxBufferSize> rx_work_buffer_{};
    std::array<uint8_t, kRxBufferSize> received_frame_{};
    std::array<uint8_t, kTxBufferSize> tx_buffer_{};

    std::atomic<uint32_t> rx_length_{0};
    std::atomic<bool> rx_ready_{false};
    std::atomic<bool> rx_bad_{false};

    PacketCallback callback_ = nullptr;
    void *callback_user_ = nullptr;
};

} // namespace gcinput
