#pragma once
#include "joybus/protocol/protocol.hpp"
#include "util/latch.hpp"
#include <span>

namespace gcinput {

struct ConsoleState {
    joybus::PollMode poll_mode = joybus::PollMode::Default;
    joybus::RumbleMode rumble_mode = joybus::RumbleMode::Off;
    uint16_t reset_count = 0;
};

class SharedConsole {
  public:
    ConsoleState load() const { return db_.load(); }

    void on_request_isr(std::span<const uint8_t> rx) {
        if (rx.empty()) {
            return;
        }

        bool updated = false;
        joybus::Command command = static_cast<joybus::Command>(rx[0]);
        switch (command) {
        case joybus::Command::Status:
            if (rx.size() >= 3) {
                const auto poll = joybus::sanitize_poll_mode(rx[1]);
                const auto rumble = joybus::sanitize_rumble_mode(rx[2]);
                if (poll != shadow_.poll_mode || rumble != shadow_.rumble_mode) {
                    shadow_.poll_mode = poll;
                    shadow_.rumble_mode = rumble;
                    updated = true;
                }
            }
            break;
        case joybus::Command::Reset:
            shadow_.reset_count++;
            updated = true;
            break;
        case joybus::Command::Id:
        case joybus::Command::Origin:
        case joybus::Command::Recalibrate:
        default:
            break;
        }

        if (updated) {
            db_.publish(shadow_);
        }
    }

  private:
    ConsoleState shadow_{};
    Latch<ConsoleState> db_{};
};

} // namespace gcinput
