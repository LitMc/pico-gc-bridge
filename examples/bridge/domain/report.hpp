#pragma once
#include <cstdint>
#include <type_traits>

namespace gcinput::domain {

// 入力以外のパッドからコンソールへ伝える状態。エラーの有無やOrigin送信済みなど
struct PadReport {
    // 本体へOriginコマンド送信済み。ここがfalseだと本体からOriginが送られてくる
    // trueにしないといつまでもOriginが来るので送ったら速やかに反映したほうがいい
    // FIXME:
    // 本来はfalse初期化ののちtrueにすべき。状態管理が面倒なのでtrue固定してしまっているのを直す
    bool origin_sent{true};

    bool error_latched{false}; // これまでの通信のどこかでエラーがあった

    // 直近の送信でエラーがあった。Status応答では常に1のためずっとtrue。IDで意味を持つのかは不明
    bool error_last{true};

    bool use_controller_origin{false}; // コントローラのOriginを使う（用途不明）
};

static_assert(std::is_trivially_copyable_v<PadReport>);
static_assert(std::is_standard_layout_v<PadReport>);
} // namespace gcinput::domain
