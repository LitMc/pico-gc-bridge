#pragma once
#include "domain/state.hpp"
#include <cstdint>

namespace gcinput::measure {
struct Uint8bitRange {
    uint8_t begin{0};
    uint8_t end{255};
    uint8_t step{1};
};

// 指定された範囲内の要素数
inline uint32_t count_range(const Uint8bitRange &range) {
    if (range.step == 0) {
        return 0;
    }
    if (range.begin > range.end) {
        return 0;
    }

    const uint32_t span = static_cast<uint32_t>(range.end) - static_cast<uint32_t>(range.begin);
    return span / static_cast<uint32_t>(range.step) + 1u;
}

class StickGridSweep {
  public:
    enum class Target : uint8_t {
        Joystick,
        Cstick,
        Trigger, // x → l_analog, y → r_analog
    };

    struct Config {
        Uint8bitRange x{};
        Uint8bitRange y{};
        bool loop{true};
        Target target{Target::Joystick};

        // パターン生成のベースとする状態。指定なければニュートラル
        domain::PadState base{};
        bool base_is_custom{false};
    };

    explicit StickGridSweep(Config config) : config_(config) {
        if (!config_.base_is_custom) {
            config_.base.input.clear_buttons();
            config_.base.input.set_analog_neutral();
            config_.base.report = domain::PadReport{};
        }

        x_count_ = count_range(config_.x);
        y_count_ = count_range(config_.y);
        // オーバーフロー対策
        if (UINT32_MAX / x_count_ < y_count_) {
            total_ = UINT32_MAX;
        } else {
            total_ = x_count_ * y_count_;
        }
    }

    void reset() { index_ = 0; }

    bool sample_and_advance(domain::PadState &out, uint32_t steps) {
        if (steps == 0) {
            steps = 1;
        }
        if (total_ == 0) {
            return false;
        }

        // 範囲をstepsごとに刻んで出力
        uint32_t out_index = index_ + (steps - 1);

        if (config_.loop) {
            out_index %= total_;
            index_ = (index_ + steps) % total_;
        } else {
            if (out_index >= total_) {
                return false;
            }
            index_ += steps;
        }

        const uint32_t x_index = out_index % x_count_;
        const uint32_t y_index = out_index / x_count_;

        const uint32_t x = static_cast<uint32_t>(config_.x.begin) + x_index * config_.x.step;
        const uint32_t y = static_cast<uint32_t>(config_.y.begin) + y_index * config_.y.step;

        // 毎回ベースから組み立てる（前回の状態にインクリメントというわけではない）
        out = config_.base;

        switch (config_.target) {
        case Target::Joystick:
            out.input.analog.stick_x = static_cast<uint8_t>(x);
            out.input.analog.stick_y = static_cast<uint8_t>(y);
            break;
        case Target::Cstick:
            out.input.analog.c_stick_x = static_cast<uint8_t>(x);
            out.input.analog.c_stick_y = static_cast<uint8_t>(y);
            break;
        case Target::Trigger:
            out.input.analog.l_analog = static_cast<uint8_t>(x);
            out.input.analog.r_analog = static_cast<uint8_t>(y);
            break;
        }
        return true;
    }

    // デバッグ用
    // 走査する範囲内にある点の数
    uint32_t total_steps() const { return total_; }
    // 何番目の点か
    uint32_t current_index() const { return index_; }

  private:
    Config config_;

    uint32_t x_count_{0};
    uint32_t y_count_{0};
    uint32_t total_{0};

    // 次の出力点のインデックス
    uint32_t index_{0};
};
} // namespace gcinput::measure
