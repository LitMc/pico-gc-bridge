#pragma once
#include <array>
#include <atomic>

namespace gcinput {
// 単一ライタ、複数リーダー向けの汎用コンテナ
//
// Single-writer assumption: This class is safe only when a single writer (e.g., one ISR)
// calls publish() and a single reader calls load(). Concurrent writers are NOT supported.
// ISR safety: publish() and load() are safe from ISR context on Cortex-M0+ because
// the platform has no store buffer. The ISR firing interval (>=320us) far exceeds
// the load() execution time (<=0.3us), making torn reads practically impossible.
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
