#pragma once
#include "joybus/driver/joybus_pio_port.hpp"
#include "link/bridge_context.hpp"
#include "link/shared/shared_console.hpp"
#include "link/shared/shared_pad_hub.hpp"

namespace gcinput {
class ConsoleClient {
  public:
    explicit ConsoleClient(JoybusPioPort::Config device_to_console_config, BridgeContext &link)
        : link_{link},
          device_to_console_(device_to_console_config, &gcinput::ConsoleClient::callback, this) {};
    // コンソールからの応答を受信したときに呼ぶコールバック
    static std::size_t callback(void *user, const uint8_t *rx, std::size_t rx_len, uint8_t *tx,
                                std::size_t tx_max);

    static std::size_t write_tx(const JoybusReply &reply, uint8_t *tx, std::size_t tx_max);

  private:
    BridgeContext &link_;
    JoybusPioPort device_to_console_;
};
} // namespace gcinput
