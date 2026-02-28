#pragma once

#include <cstdint>

namespace gcinput::measure {
struct ScheduleConfig {
    uint32_t interval_us{16'667}; // デフォルト60Hz
    bool catch_up{false};         // 遅延してでもテストパターンは捨てない
};

class Schedule {
  public:
    Schedule() = default;
    explicit Schedule(ScheduleConfig config)
        : interval_us{config.interval_us}, catch_up{config.catch_up} {}

    uint32_t interval_us{16'667};
    bool catch_up{true};

    void reset() { armed_ = false; }

    uint32_t poll_steps(uint32_t now_us) {
        const uint32_t interval = interval_us == 0 ? 1 : interval_us;

        // 初回は即時で送る
        if (!armed_) {
            armed_ = true;
            next_due_us_ = now_us;
        }

        if (!has_passed_due(now_us, next_due_us_)) {
            // まだ期限に達していない
            return 0;
        }

        // 期限を過ぎている分
        const uint32_t late = now_us - next_due_us_;

        uint32_t steps = 1;
        if (catch_up) {
            // 途中のテストパターンを飛ばしてでも追いつく
            steps += late / interval;
            next_due_us_ += steps * interval;
        } else {
            // 用意したパターンは遅延してでも必ず送る
            next_due_us_ = now_us + interval;
        }
        return steps;
    }

  private:
    // 現在時刻が期限を過ぎているか
    static bool has_passed_due(uint32_t now_us, uint32_t due_us) {
        return static_cast<int32_t>(now_us - due_us) >= 0;
    }

  private:
    bool armed_{false};
    uint32_t next_due_us_{0};
};
} // namespace gcinput::measure
