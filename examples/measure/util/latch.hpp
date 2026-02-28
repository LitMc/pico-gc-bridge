#pragma once
#include <array>
#include <atomic>

namespace gcinput {
// 単一ライタ、複数リーダー向けの汎用コンテナ
template <class T> class Latch {
  public:
    void publish(const T &v) {
        const uint8_t current = index_.load(std::memory_order_relaxed);
        const uint8_t next = current ^ 1u;
        buffer_[next] = v;
        index_.store(next, std::memory_order_release);
    }

    T load() const {
        const uint8_t current = index_.load(std::memory_order_acquire);
        return buffer_[current];
    }

  private:
    std::array<T, 2> buffer_{};
    std::atomic<uint8_t> index_{0};
};
} // namespace gcinput
