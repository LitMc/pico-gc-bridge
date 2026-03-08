#pragma once
#include "domain/state.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// コントローラ応答の変換処理
namespace gcinput::domain::transform {

// コントローラ応答変換関数の型定義。ISRから呼ぶ想定
using TransformFunction = void (*)(void *user, domain::PadState &state);

struct Stage {
    TransformFunction func{nullptr};
    void *user{nullptr};
};

template <class Context, void (*Func)(Context &, domain::PadState &)>
inline void thunk(void *user, domain::PadState &state) {
    auto *ctx = static_cast<Context *>(user);
    Func(*ctx, state);
}

template <class Context, void (*Func)(Context &, domain::PadState &)>
inline Stage make_stage(Context &context) {
    return Stage{
        .func{&thunk<Context, Func>},
        .user{&context},
    };
}

inline Stage make_stage(TransformFunction func) {
    return Stage{
        .func{func},
        .user{nullptr},
    };
}

// コントローラ応答を多段階のステージで変換するパイプライン
class Pipeline {
  public:
    static constexpr std::size_t kMaxStages = 16;
    Pipeline() = default;

    // ステージを登録
    bool add_stage(Stage stage) {
        if (!stage.func) {
            return false;
        }
        if (stage_count_ >= kMaxStages) {
            return false;
        }

        const std::size_t index = stage_count_;
        stages_[index] = stage;
        ++stage_count_;
        // ステージ登録はmainから最初に一度だけ呼ばれる想定なので割り込みを気にする必要はない
        enable_mask_.fetch_or(1u << index, std::memory_order_release);
        return true;
    }

    std::size_t size() const { return stage_count_; }

    void set_stage_enabled(std::size_t index, bool enable) {
        if (index >= stage_count_) {
            return;
        }
        const uint32_t bit = 1u << index;
        if (enable) {
            enable_mask_.fetch_or(bit, std::memory_order_release);
        } else {
            enable_mask_.fetch_and(~bit, std::memory_order_release);
        }
    }

    bool is_stage_enabled(std::size_t index) const {
        if (index >= stage_count_) {
            return false;
        }
        const uint32_t enabled = enable_mask_.load(std::memory_order_acquire);
        return (enabled & (1u << index)) != 0;
    }

    void apply_from_isr(domain::PadState &state) const {
        const uint32_t enabled = enable_mask_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < stage_count_; ++i) {
            // is_stage_enabledでもいいけどISR内で何度もenabledをload()しなくて済むよう直接確認
            if ((enabled & (1u << i)) == 0) {
                continue;
            }
            const Stage &stage = stages_[i];
            if (!stage.func) {
                continue;
            }
            stage.func(stage.user, state);
        }
    }

  private:
    std::array<Stage, kMaxStages> stages_{};
    std::size_t stage_count_ = 0;
    std::atomic<uint32_t> enable_mask_{0};
};

// 何もしない空のパイプラインを返す
inline const Pipeline &empty_pipeline() {
    static const Pipeline pipeline{};
    return pipeline;
}

// 命令別に束ねたパイプライン
struct PipelineSet {
    Pipeline status{};
    Pipeline origin{};
    Pipeline recalibrate{};
    Pipeline id{};
    Pipeline reset{};
};
} // namespace gcinput::domain::transform
