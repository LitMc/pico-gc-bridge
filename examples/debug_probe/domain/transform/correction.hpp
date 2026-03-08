#pragma once
#include "domain/state.hpp"
#include "domain/transform/inverse_lut_data.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <utility>

namespace gcinput::domain::transform::correction {

// ── 原点正規化コンテキスト ──
// Origin/Recalibrate で取得した実際のニュートラル座標を保持する。
// main ループから store、ISR から load される。
struct OriginOffsetContext {
    std::atomic<uint8_t> origin_x{AnalogInput::kAxisCenter};
    std::atomic<uint8_t> origin_y{AnalogInput::kAxisCenter};
};

// ── origin_normalize: 原点正規化 ──
// スティック値から実際のニュートラルオフセットを引き、中心を (128, 128) に揃える。
inline void origin_normalize(OriginOffsetContext &ctx, domain::PadState &state) {
    const int32_t ox = ctx.origin_x.load(std::memory_order_acquire);
    const int32_t oy = ctx.origin_y.load(std::memory_order_acquire);

    auto &analog = state.input.analog;
    const int32_t x = static_cast<int32_t>(analog.stick_x) - ox + AnalogInput::kAxisCenter;
    const int32_t y = static_cast<int32_t>(analog.stick_y) - oy + AnalogInput::kAxisCenter;

    analog.stick_x = static_cast<uint8_t>(std::clamp(x, int32_t{0}, int32_t{255}));
    analog.stick_y = static_cast<uint8_t>(std::clamp(y, int32_t{0}, int32_t{255}));
}

// ── 固定小数点定数 (Q15) ──
static constexpr int32_t kCos8Q15 = 30274;  // cos(π/8) × 2^15
static constexpr int32_t kSin8Q15 = 12540;  // sin(π/8) × 2^15
static constexpr int32_t kApothem125Q15 = 125 * kCos8Q15; // h = 125 × cos(π/8)
static constexpr int32_t kCenter = AnalogInput::kAxisCenter;

// ── octagon_clamp: Oct(125) への放射クランプ ──
// 中心 (128, 128) 基準で4つの半平面制約を評価し、外側なら境界に射影する。
inline void octagon_clamp(void *, domain::PadState &state) {
    auto &analog = state.input.analog;

    const int32_t px = static_cast<int32_t>(analog.stick_x) - kCenter;
    const int32_t py = static_cast<int32_t>(analog.stick_y) - kCenter;

    if (px == 0 && py == 0) {
        return;
    }

    // 4つの半平面制約（Q15スケール）
    const int32_t c0 = kCos8Q15 * px + kSin8Q15 * py;
    const int32_t c1 = kCos8Q15 * px - kSin8Q15 * py;
    const int32_t c2 = kSin8Q15 * px + kCos8Q15 * py;
    const int32_t c3 = kSin8Q15 * px - kCos8Q15 * py;

    const int32_t abs_c0 = (c0 < 0) ? -c0 : c0;
    const int32_t abs_c1 = (c1 < 0) ? -c1 : c1;
    const int32_t abs_c2 = (c2 < 0) ? -c2 : c2;
    const int32_t abs_c3 = (c3 < 0) ? -c3 : c3;

    int32_t max_abs = abs_c0;
    if (abs_c1 > max_abs) max_abs = abs_c1;
    if (abs_c2 > max_abs) max_abs = abs_c2;
    if (abs_c3 > max_abs) max_abs = abs_c3;

    // Oct(125) 内部ならそのまま
    if (max_abs <= kApothem125Q15) {
        return;
    }

    // 放射方向に境界へ射影: new = p × h / max_abs + 128
    const int32_t new_px = (px * kApothem125Q15) / max_abs;
    const int32_t new_py = (py * kApothem125Q15) / max_abs;

    analog.stick_x =
        static_cast<uint8_t>(std::clamp(new_px + kCenter, int32_t{0}, int32_t{255}));
    analog.stick_y =
        static_cast<uint8_t>(std::clamp(new_py + kCenter, int32_t{0}, int32_t{255}));
}

// ── linear_scale: Oct(125) → Oct(100) の線形スケーリング ──
// φ(s) = 0.8 × (s − 128) + 128 = 4/5 × (s − 128) + 128
inline void linear_scale(void *, domain::PadState &state) {
    auto &analog = state.input.analog;

    const int32_t px = static_cast<int32_t>(analog.stick_x) - kCenter;
    const int32_t py = static_cast<int32_t>(analog.stick_y) - kCenter;

    // k = 4/5。四捨五入を乗算+右シフトで実現 (13108 ≈ 2^16/5)。
    // Cortex-M0+ は hardware divider 非搭載のため、/ 5 は __aeabi_idiv
    // (約30-40サイクル) になる。乗算+シフトで約1サイクルに削減。
    const int32_t rx = (px >= 0) ? ((px * 4 + 2) * 13108) >> 16
                                 : -((((-px) * 4 + 2) * 13108) >> 16);
    const int32_t ry = (py >= 0) ? ((py * 4 + 2) * 13108) >> 16
                                 : -((((-py) * 4 + 2) * 13108) >> 16);

    analog.stick_x =
        static_cast<uint8_t>(std::clamp(rx + kCenter, int32_t{0}, int32_t{255}));
    analog.stick_y =
        static_cast<uint8_t>(std::clamp(ry + kCenter, int32_t{0}, int32_t{255}));
}

// ── inverse_lut: S⁻¹⁺ LUT ルックアップ ──
// 現在のスティック値をインデックスとして逆変換テーブルを参照する。
inline void inverse_lut(void *, domain::PadState &state) {
    auto &analog = state.input.analog;

    const uint8_t mx = analog.stick_x;
    const uint8_t my = analog.stick_y;

    analog.stick_x = kInverseLutX[mx][my];
    analog.stick_y = kInverseLutY[mx][my];
}

// ── forward_lut: S 順方向 LUT ルックアップ（デバッグ用） ──
// S(sx, sy) = ゲームが受け取る座標を返す。
inline std::pair<uint8_t, uint8_t> forward_lut(uint8_t sx, uint8_t sy) {
    return {kForwardLutX[sx][sy], kForwardLutY[sx][sy]};
}

} // namespace gcinput::domain::transform::correction
