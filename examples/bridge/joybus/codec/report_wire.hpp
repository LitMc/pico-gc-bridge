#pragma once
#include "domain/report.hpp"
#include "domain/state.hpp"
#include "util/endian.hpp"
#include <cstdint>
#include <span>

// プロジェクト固有の実行時レポートをJoybus形式に変換、復元するための処理群
namespace gcinput::joybus::report {

// JoybusのStatus wordにおけるレポートフラグのビット定義
enum class StatusWordBits : uint16_t {
    OriginNotSent = (1u << 5),
    ErrorLatched = (1u << 6),
    Always1 = (1u << 7),
    UseControllerOrigin = (1u << 15),
};

enum class IdByte3Bits : uint8_t {
    OriginNotSent = (1u << 5),
    ErrorLatched = (1u << 6),
    ErrorLast = (1u << 7),
    // UseControllerOrigin bitはIDレスポンスに存在しない
};

constexpr uint16_t to_mask(StatusWordBits bit) { return static_cast<uint16_t>(bit); }

constexpr uint8_t to_mask(IdByte3Bits bit) { return static_cast<uint8_t>(bit); }

// Status word（Status, Origin, Recalibrateの先頭2バイト）を共通形式のレポートに変換
inline constexpr domain::PadReport
decode_report_from_status_word(std::span<const uint8_t, 2> byte2) {
    domain::PadReport out{};

    const uint16_t status_word = util::read_u16_le(byte2);
    out.origin_sent =
        (status_word & report::to_mask(report::StatusWordBits::OriginNotSent)) == 0;
    out.error_latched =
        (status_word & report::to_mask(report::StatusWordBits::ErrorLatched)) != 0;
    out.error_last = (status_word & report::to_mask(report::StatusWordBits::Always1)) != 0;
    out.use_controller_origin =
        (status_word & report::to_mask(report::StatusWordBits::UseControllerOrigin)) != 0;

    return out;
}

// IDレスポンスの3バイト目を共通形式のレポートに変換
// UseControllerOriginはIDレスポンスに存在しないので触らない
inline constexpr void update_report_from_id_byte3(domain::PadReport &report, uint8_t byte3) {
    report.origin_sent = (byte3 & report::to_mask(report::IdByte3Bits::OriginNotSent)) == 0;
    report.error_latched = (byte3 & report::to_mask(report::IdByte3Bits::ErrorLatched)) != 0;
    report.error_last = (byte3 & report::to_mask(report::IdByte3Bits::ErrorLast)) != 0;
}
} // namespace gcinput::joybus::report
