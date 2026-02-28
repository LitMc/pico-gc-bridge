#pragma once
#include "domain/transform/pipeline.hpp"
#include "hardware/sync.h"
#include "link/shared/shared_console.hpp"
#include "link/shared/shared_pad_hub.hpp"
#include <atomic>

namespace gcinput {

// パッド向けクライアントとコンソール向けクライアントで共有する情報
class PadConsoleLink {
  public:
    SharedPadHub &real_pad_hub() { return real_pad_hub_; }
    const SharedPadHub &real_pad_hub() const { return real_pad_hub_; }
    SharedConsole &shared_console() { return shared_console_; }
    const SharedConsole &shared_console() const { return shared_console_; }

    // 外から見たパッドの状態
    enum class PadConnectionState : uint8_t {
        Disconnected, // 接続未確立
        Booting,      // 初期化（ID、Origin、Recalibrate取得）中
        Ready,        // Statusポーリング開始済み
    };

    // Pad->Console: パッドの状態を公開
    void publish_pad_state_from_main(PadConnectionState state) {
        pad_state_.store(static_cast<uint8_t>(state), std::memory_order_release);
    }

    // Console<-Link: パッドの状態を取得
    PadConnectionState load_pad_state() const {
        return static_cast<PadConnectionState>(pad_state_.load(std::memory_order_acquire));
    }

    // Console<-Link: パッドとの接続が確立しているか
    bool is_pad_ready() const { return load_pad_state() == PadConnectionState::Ready; }

    // Console->Pad: パッドのResetを要求
    void __isr publish_pad_reset_request_from_isr() {
        reset_epoch_.fetch_add(1, std::memory_order_relaxed);
    }

    // Console->Pad: パッドのOrigin中継を要求
    void __isr publish_pad_origin_request_from_isr() {
        origin_epoch_.fetch_add(1, std::memory_order_relaxed);
    }

    // Console->Pad: パッドのRecalibrate中継を要求
    void __isr publish_pad_recalibrate_request_from_isr() {
        recalibrate_epoch_.fetch_add(1, std::memory_order_relaxed);
    }

    // Pad<-Link: 最後に発行されたReset要求のエポックを取得
    uint32_t load_reset_epoch() const { return reset_epoch_.load(std::memory_order_relaxed); }

    // Pad<-Link: 最後に発行されたOrigin中継要求のエポックを取得
    uint32_t load_origin_epoch() const { return origin_epoch_.load(std::memory_order_relaxed); }

    // Pad<-Link: 最後に発行されたRecalibrate中継要求のエポックを取得
    uint32_t load_recalibrate_epoch() const {
        return recalibrate_epoch_.load(std::memory_order_relaxed);
    }

    // Pad<-Link: Reset要求があればlast_reset_epochを更新しtrue、なければ更新せずfalse
    [[nodiscard]] bool consume_pad_reset_request(uint32_t &last_reset_epoch) const {
        const uint32_t cur = load_reset_epoch();
        if (cur == last_reset_epoch) {
            return false;
        }
        last_reset_epoch = cur;
        return true;
    }

    // Pad<-Link: Origin中継要求があればlast_origin_epochを更新しtrue、なければ更新せずfalse
    [[nodiscard]] bool consume_pad_origin_request(uint32_t &last_origin_epoch) const {
        const uint32_t cur = load_origin_epoch();
        if (cur == last_origin_epoch) {
            return false;
        }
        last_origin_epoch = cur;
        return true;
    }

    // Pad<-Link: Recalibrate中継要求があればlast_recalibrate_epochを更新しtrue
    [[nodiscard]] bool consume_pad_recalibrate_request(uint32_t &last_recalibrate_epoch) const {
        const uint32_t cur = load_recalibrate_epoch();
        if (cur == last_recalibrate_epoch) {
            return false;
        }
        last_recalibrate_epoch = cur;
        return true;
    }

    // コマンド応答の変換パイプライン
    domain::transform::PipelineSet &transform_pipelines() { return pipelines_; }
    // コマンド応答の変換パイプライン
    const domain::transform::PipelineSet &transform_pipelines() const { return pipelines_; }

  private:
    std::atomic<uint8_t> pad_state_{static_cast<uint8_t>(PadConnectionState::Disconnected)};
    std::atomic<uint32_t> reset_epoch_{0};
    std::atomic<uint32_t> origin_epoch_{0};
    std::atomic<uint32_t> recalibrate_epoch_{0};
    SharedPadHub real_pad_hub_{};
    SharedConsole shared_console_{};
    domain::transform::PipelineSet pipelines_{};
};

} // namespace gcinput
