#pragma once

#include "domain/state.hpp"
#include <concepts>
#include <cstdint>

namespace gcinput::measure {
// テストパターンのコンセプト
template <class P>
concept TestPattern = requires(P p, domain::PadState &state, uint32_t steps) {
    // リセットできること
    { p.reset() } -> std::same_as<void>;
    // 現在のパッド状態とステップ数から次の状態へ進められること
    { p.sample_and_advance(state, steps) } -> std::same_as<bool>;
};
} // namespace gcinput::measure
