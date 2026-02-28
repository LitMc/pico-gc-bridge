#include "joybus_pio_port.hpp"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include <algorithm>
#include <cassert>

namespace gcinput {

// IRQラインが競合しないよう管理するマルチプレクサ
struct JoybusPioPort::IrqMux {
    std::array<std::array<JoybusPioPort *, 8>, 2> owners{};
    std::array<uint8_t, 2> owned_mask{0, 0};
    std::array<bool, 2> installed{false, false};

    static int pio_index(PIO pio) { return (pio == pio0) ? 0 : 1; }

    void ensure_installed(PIO pio) {
        const int index = pio_index(pio);
        if (installed[(size_t)index]) {
            return;
        }
        const int irq = (index == 0) ? PIO0_IRQ_0 : PIO1_IRQ_0;
        if (index == 0) {
            irq_add_shared_handler(irq, &JoybusPioPort::pio0_irq0_handler,
                                   PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
        } else {
            irq_add_shared_handler(irq, &JoybusPioPort::pio1_irq0_handler,
                                   PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
        }

        irq_set_priority(irq, PICO_HIGHEST_IRQ_PRIORITY);
        irq_set_enabled(irq, true);
        installed[(size_t)index] = true;
    }

    void register_owner(PIO pio, uint bit, JoybusPioPort *self) {
        assert(bit < 8);
        const int index = pio_index(pio);

        // 登録中に割り込まれないよう止めてから登録
        uint32_t s = save_and_disable_interrupts();
        {
            assert(owners[(size_t)index][bit] == nullptr);
            owners[(size_t)index][bit] = self;
            owned_mask[(size_t)index] |= (uint8_t)(1u << bit);
        }
        restore_interrupts(s);

        pio_interrupt_clear(pio, bit);
        pio_set_irq0_source_enabled(pio, (pio_interrupt_source_t)(pis_interrupt0 + bit), true);
    }

    void unregister_owner(PIO pio, uint bit, JoybusPioPort *self) {
        assert(bit < 8);
        const int index = pio_index(pio);

        // 登録中に割り込まれないよう止めてから
        uint32_t s = save_and_disable_interrupts();
        if (owners[(size_t)index][bit] == self) {
            owners[(size_t)index][bit] = nullptr;
            owned_mask[(size_t)index] &= (uint8_t)~(1u << bit);
        }
        restore_interrupts(s);
        // このbitをIRQ0から外す
        pio_set_irq0_source_enabled(pio, (pio_interrupt_source_t)(pis_interrupt0 + bit), false);
        pio_interrupt_clear(pio, bit);
    }

    // 発生した割り込みフラグを適切なownerのハンドラに振り分ける
    void dispatch(PIO pio) {
        const int index = pio_index(pio);
        const uint8_t mask = owned_mask[(size_t)index];

        uint32_t pending = pio->irq & (uint32_t)mask;
        // 最下位から順に1が立った割り込みフラグだけを処理
        while (pending) {
            // 一番下位に立っている1の位置
            const uint bit = (uint)__builtin_ctz(pending);
            // 一番下位の1を消す
            pending &= (pending - 1);
            // そのビットのIRQフラグをクリア
            pio_interrupt_clear(pio, bit);
            // index番目のPIOのIRQフラグ bitを使っているインスタンスのon_pio_irqを呼ぶ
            // pio0のIRQ0ならowners[0][0]
            JoybusPioPort *self = owners[(size_t)index][bit];
            if (self) {
                self->on_pio_irq();
            }
        }
    }
};

JoybusPioPort::IrqMux &JoybusPioPort::irq_mux() {
    static IrqMux mux;
    return mux;
}

void __isr JoybusPioPort::pio0_irq0_handler() { irq_mux().dispatch(pio0); }

void __isr JoybusPioPort::pio1_irq0_handler() { irq_mux().dispatch(pio1); }

JoybusPioPort::JoybusPioPort(const Config &config, PacketCallback callback, void *user)
    : config_(config), callback_(callback), callback_user_(user) {
    program_offset_ = pio_add_program(config_.pio, config_.program);

    pio_sm_config c = config_.get_default_config(program_offset_);
    sm_config_set_out_pins(&c, config_.pin, 1);
    sm_config_set_set_pins(&c, config_.pin, 1);
    sm_config_set_in_pins(&c, config_.pin);
    sm_config_set_jmp_pin(&c, config_.pin);

    // MSB-firstで1バイトずつ自動プル/プッシュ
    sm_config_set_out_shift(&c,
                            /*shift_right=*/false,
                            /*autopull=*/true,
                            /*pull_thresh=*/8);
    sm_config_set_in_shift(&c,
                           /*shift_right=*/false,
                           /*autopush=*/true,
                           /*push_thresh=*/8);

    const float div = (float)clock_get_hz(clk_sys) / config_.pio_hz;
    sm_config_set_clkdiv(&c, div);

    // PIOにピンを割り当て
    pio_gpio_init(config_.pio, config_.pin);

    // 初期化中にLowが送出されないよう止めておく
    gpio_set_oeover(config_.pin, GPIO_OVERRIDE_LOW);

    pio_sm_set_pins_with_mask(config_.pio, config_.state_machine, 0u, 1u << config_.pin);
    pio_sm_set_pindirs_with_mask(config_.pio, config_.state_machine, 1u << config_.pin,
                                 1u << config_.pin);
    // オープンドレインなのでpindirsが0のときLow、1のときHi-Zとなるよう入れ替える
    gpio_set_oeover(config_.pin, GPIO_OVERRIDE_INVERT);

    // DMAチャンネルは内部でclaim
    dma_channel_ = dma_claim_unused_channel(true);

    // RX向けDMA設定
    dma_rx_config_ = dma_channel_get_default_config(dma_channel_);
    channel_config_set_transfer_data_size(&dma_rx_config_, DMA_SIZE_8);
    channel_config_set_dreq(&dma_rx_config_,
                            pio_get_dreq(config_.pio, config_.state_machine, false));
    channel_config_set_read_increment(&dma_rx_config_, false);
    channel_config_set_write_increment(&dma_rx_config_, true);

    // TX向けDMA設定
    dma_tx_config_ = dma_channel_get_default_config(dma_channel_);
    channel_config_set_transfer_data_size(&dma_tx_config_, DMA_SIZE_8);
    channel_config_set_dreq(&dma_tx_config_,
                            pio_get_dreq(config_.pio, config_.state_machine, true));
    channel_config_set_read_increment(&dma_tx_config_, true);
    channel_config_set_write_increment(&dma_tx_config_, false);

    // IRQフラグへの割り当てを登録
    irq_mux().ensure_installed(config_.pio);
    irq_mux().register_owner(config_.pio, irq_index(), this);

    // ステートマシンを開始
    pio_sm_init(config_.pio, config_.state_machine, program_offset_ + config_.rx_start_offset, &c);
    pio_sm_set_enabled(config_.pio, config_.state_machine, true);
    start_receive();
}

JoybusPioPort::~JoybusPioPort() {
    if (!config_.pio) {
        return;
    }
    irq_mux().unregister_owner(config_.pio, irq_index(), this);
    pio_sm_set_enabled(config_.pio, config_.state_machine, false);
    if (dma_channel_ >= 0) {
        dma_channel_abort(dma_channel_);
        dma_channel_unclaim(dma_channel_);
        dma_channel_ = -1;
    }

    gpio_set_oeover(config_.pin, GPIO_OVERRIDE_NORMAL);
    gpio_set_dir(config_.pin, GPIO_IN);
}

void JoybusPioPort::start_receive() {
    dma_channel_abort(dma_channel_);
    dma_channel_set_config(dma_channel_, &dma_rx_config_, false);
    dma_channel_set_read_addr(dma_channel_, &config_.pio->rxf[config_.state_machine], false);
    dma_channel_transfer_to_buffer_now(dma_channel_, rx_work_buffer_.data(), kRxBufferSize);
}

void JoybusPioPort::finish_receive_from_irq() {
    dma_channel_hw_t *dma = dma_channel_hw_addr(dma_channel_);
    // dma->transfer_countは残りの転送数なので元のサイズから引いて受信済みバイト数を得る
    uint32_t received = kRxBufferSize - dma->transfer_count;

    dma_channel_abort(dma_channel_);

    rx_length_ = 0;
    rx_ready_ = false;
    rx_bad_ = false;

    if (received < 2) {
        rx_bad_ = true;
        return;
    }

    const uint32_t frame_length = received - 1; // ストップビット分を除く
    std::copy_n(rx_work_buffer_.begin(), frame_length, received_frame_.begin());
    rx_length_ = frame_length;
    rx_ready_ = true;
}

void JoybusPioPort::start_transmit_from_irq(std::size_t nbytes) {
    // 送信すべきビット数をPIOに教える
    pio_sm_put(config_.pio, config_.state_machine, static_cast<uint32_t>(nbytes * 8));
    dma_channel_set_config(dma_channel_, &dma_tx_config_, false);
    dma_channel_set_read_addr(dma_channel_, tx_buffer_.data(), false);
    dma_channel_set_write_addr(dma_channel_, &config_.pio->txf[config_.state_machine], false);
    dma_channel_transfer_from_buffer_now(dma_channel_, tx_buffer_.data(), nbytes);

    uint jmp_to_tx_start = pio_encode_jmp(tx_start_pc());
    pio_sm_exec_wait_blocking(config_.pio, config_.state_machine, jmp_to_tx_start);
}

void JoybusPioPort::on_pio_irq() {
    // 送信中だったなら送信完了の通知を受けた
    if (tx_busy_.load(std::memory_order_acquire)) {
        tx_busy_.store(false, std::memory_order_release);
        start_receive();
        return;
    }

    // 送信中でないなら受信完了の通知を受けた
    finish_receive_from_irq();

    // 受信結果から即座に返信を生成
    std::size_t tx_length = 0;
    if (callback_ && rx_ready_ && rx_length_ > 0) {
        tx_length =
            callback_(callback_user_, received_frame_.data(), static_cast<std::size_t>(rx_length_),
                      tx_buffer_.data(), kTxBufferSize);
        if (tx_length > kTxBufferSize) {
            tx_length = kTxBufferSize;
        }
    }

    if (tx_length > 0) {
        tx_busy_.store(true, std::memory_order_release);
        start_transmit_from_irq(tx_length);
    } else {
        // 返信なしなら受信待ちに戻る
        start_receive();
    }
}

bool JoybusPioPort::send_now(const uint8_t *data, std::size_t nbytes) {
    if (nbytes == 0 || nbytes > kTxBufferSize) {
        return false;
    }

    uint32_t s = save_and_disable_interrupts();
    if (tx_busy_.load(std::memory_order_acquire)) {
        restore_interrupts(s);
        return false;
    }
    std::copy_n(data, nbytes, tx_buffer_.begin());
    dma_channel_abort(dma_channel_);

    tx_busy_.store(true, std::memory_order_release);
    start_transmit_from_irq(nbytes);
    restore_interrupts(s);
    return true;
}

} // namespace gcinput
