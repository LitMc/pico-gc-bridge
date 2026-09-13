# bridge リファクタ計画

`examples/bridge` の見通しをよくするための作業計画。機能は現状維持。
LRトリガー計測はこの計画が終わってから着手する。

- 1 PR ≒ 差分 400 行以内
- ファイル移動は「`git mv` + include 修正だけのコミット」と「中身を変えるコミット」に分ける
- 途中で見つけたバグは同じ PR 内でも別の `fix:` コミットにする
- `examples/debug_probe` と `examples/measure` は触らない（bridge のコピーだが共通化しない方針）
- 行番号リンクは `68b70ec` 時点のもの。作業が進むとずれるので、以降はシンボル名で探す

## 進捗

| PR | ブランチ | 内容 | 差分目安 | 状態 |
|---|---|---|---|---|
| 0 | `ci/bridge-debug-build` | CI で Debug もビルド | 20 | [ ] |
| 1 | `refactor/bridge-board` | ボード・ポート設定・振動を main から分離 | 300 | [ ] |
| 2 | `refactor/bridge-correction` | 補正モード管理を CorrectionController へ | 350 | [ ] |
| 3 | `refactor/bridge-log-ring` | ログ基盤を `diag/` へ、呼び出しを1行化 | 300 | [ ] |
| 4 | `refactor/bridge-monitor` | ドレイン・サマリーを Monitor へ | 380 | [ ] |
| 5 | `refactor/bridge-trace` | DBG トレース移動、main.cpp 仕上げ | 250 | [ ] |
| 6 | `refactor/bridge-link` | PadClient / BridgeContext の定型処理を畳む | 350 | [ ] |
| 7 | `refactor/bridge-cleanup` | 未使用コード削除、ドキュメント更新 | 150 | [ ] |

依存: 0 → 1 → 2 → 3 → 4 → 5 → 6 → 7（6 は 3 以降ならどこでも可。ログ呼び出しを触るので 3 より前は衝突する）

---

## 現状（68b70ec 時点）

[main.cpp](../examples/bridge/main.cpp)（498行）に以下が同居している。

| 行 | 中身 | 行き先 |
|---|---|---|
| [L18-61](../examples/bridge/main.cpp#L18-L61) | LED, BOOTSEL ボタン, ピン定義 | PR1 `app/board` |
| [L224-251](../examples/bridge/main.cpp#L224-L251) | PIO/SM 確保, `JoybusPioPort::Config` | PR1 `app/joybus_ports` |
| [L67-103](../examples/bridge/main.cpp#L67-L103) | `RumbleOverride` | PR1 `app/rumble_pulser` |
| [L64](../examples/bridge/main.cpp#L64), [L256-284](../examples/bridge/main.cpp#L256-L284), [L388-437](../examples/bridge/main.cpp#L388-L437) | パイプライン構築, 原点追従, モード切替 | PR2 `app/correction_controller` |
| [L106-119](../examples/bridge/main.cpp#L106-L119) | `pad_state_name`（pad_client.cpp の `state_name` と重複） | PR3 で統合 |
| [L121-210](../examples/bridge/main.cpp#L121-L210), [L334-386](../examples/bridge/main.cpp#L334-L386) | ログ整形, ドレイン, `[POLL]`, ドロップ警告 | PR4 `diag/` |
| [L440-494](../examples/bridge/main.cpp#L440-L494) | DBG 行（補正ステージを手で再計算） | PR5 |

## 目標の構成

```
examples/bridge/
  main.cpp                         ← 初期化・配線・ループだけ（100行前後）
  app/
    board.hpp/.cpp                 ← ピン定義, LED, BOOTSELボタン
    joybus_ports.hpp               ← PIO/SM確保 + JoybusPioPort::Config生成
    rumble_pulser.hpp              ← モード切替通知の振動
    correction_controller.hpp/.cpp ← パイプライン構築, 原点追従, モード切替, トレース
  diag/                            ← Debugビルド専用。Releaseでは空のinline実装
    log_ring.hpp                   ← 旧 debug_log.hpp
    log_printer.hpp/.cpp           ← ドレイン, 整形, [POLL]サマリー
    monitor.hpp/.cpp               ← mainからの唯一の窓口
  domain/ joybus/ link/ util/      ← 既存
```

完成時の main ループ:

```cpp
while (true) {
    board::poll_bootsel();
    const uint32_t now_us = time_us_32();

    auto console = ctx.shared_console().load();
    if (rumble.forcing_on(now_us)) {
        console.rumble_mode = domain::RumbleMode::On;
    }
    pad_client.tick(now_us, console);

    const auto snapshot = ctx.real_pad_hub().load_original_snapshot();
    const auto update = correction.update(snapshot);
    if (update.mode_changed) {
        rumble.start(correction.mode() == CorrectionController::Mode::Correction ? 1 : 2, now_us);
    }

    monitor.tick(now_us, pad_client.current_state(), console, update, snapshot);
    tight_loop_contents();
}
```

---

## 共通の確認手順

各 PR の「完了条件」から参照する。

### B: ビルド（毎回 Debug / Release 両方）

```bash
cmake --preset default && cmake --build --preset default --target bridge   # Debug（ログあり）
cmake --preset release && cmake --build --preset release --target bridge   # Release（ログなし）
arm-none-eabi-size build-release/examples/bridge/bridge.elf
```

Release のサイズを下の表に記録する。PR1〜4 はほぼ横ばい、PR5 で減るのが正常。
大きく増減したら、何かを落としたか余計に残している。

| 時点 | text | data | bss |
|---|---|---|---|
| ベースライン (68b70ec) | | | |
| PR1 | | | |
| PR2 | | | |
| PR3 | | | |
| PR4 | | | |
| PR5 | | | |
| PR6 | | | |
| PR7 | | | |

### L: ログ比較

**着手前に一度だけ**、ベースラインの Debug ビルドを書き込み、UART ログを保存しておく
（保存先はリポジトリの外。例: `~/bridge-logs/baseline.log`）。

採取シナリオ（毎回同じ順で行う）:

1. パッド未接続で電源投入 → `=== Bridge ready ===` を確認
2. パッド接続 → Ready まで
3. 5秒放置（`[POLL]` が数行出る）
4. コンボ（L+R+↑+Start+Y）で補正モードにし、スティックを一周
5. もう一度コンボで FIX モードに戻す
6. 本体から Reset / Origin が飛ぶ操作をする（ログに `[CON] << Reset` や `<< Origin` が出ればよい）
7. パッドを抜いて挿し直す

各 PR の後に同じシナリオで採取し、**出る行の種類・形式・順序**がベースラインと同じかを見る
（`[POLL]` のカウント値や DBG 行の数値はタイミングで変わるので一致しなくてよい）。
意図的に変わる行は各 PR の完了条件に書いてある。

### H: 実機チェック

| # | 操作 | 期待結果 |
|---|---|---|
| H1 | パッドを接続 | すぐ Ready（`[PAD] Controller connected.`）。本体でボタンが効く |
| H2 | 起動直後（FIX モード）でスティック・Cスティック・LR を動かす | 本体側では動かない（ニュートラル固定）。ボタンは効く |
| H3 | コンボ → もう一度コンボ | 1回目: 補正モード・振動1回。2回目: FIX モード・振動2回 |
| H4 | 本体から Reset / Origin / Recalibrate | その後も入力が生きている。Origin 時は `Origin updated: (x, y)` |
| H5 | パッドを抜く → 挿す | `TIMEOUT pad connection lost` → 再び Ready |
| H6 | GP26 ボタンを押す | BOOTSEL ドライブがマウントされる |

---

## PR0 `ci:` CI で Debug もビルドする

**なぜ**: `CMAKE_BUILD_TYPE` が未指定だと Pico SDK は Release を選ぶ（`pico-sdk/cmake/pico_pre_load_toolchain.cmake`）。
今の CI は `#ifdef GCINPUT_ENABLE_LOG` 側を一度もコンパイルしていない。PR3〜5 はそこを大きく触る。

**触るファイル**
- 変更: [.github/workflows/build.yaml](../.github/workflows/build.yaml)

**手順**
- [ ] `jobs.build` に `strategy.matrix.build_type: [Debug, Release]` を追加
- [ ] Configure に `-DCMAKE_BUILD_TYPE=${{ matrix.build_type }}` を追加
- [ ] ccache の `key` と `restore-keys` に `${{ matrix.build_type }}` を含める
- [ ] `Upload UF2 artifacts` の `if` に `matrix.build_type == 'Release'` を追加（アーティファクト名の衝突を避ける）

**完了条件**
- [ ] PR の Checks に `build (Debug)` と `build (Release)` の2つが並び、両方通る
- [ ] Debug ジョブの Configure ログに `Defaulting build type to 'Release'` が**出ていない**
- [ ] （任意）main.cpp の `#ifdef GCINPUT_ENABLE_LOG` 内にわざと構文エラーを入れた捨てコミットで、Debug だけ落ちることを確認してから revert

---

## PR1 `refactor:` ボード・ポート設定・振動を main から分離

**目的**: main.cpp から、機能と無関係な独立部品を追い出す。ロジックは一切変えない。

**触るファイル**
- 新規: `app/board.hpp`, `app/board.cpp`, `app/joybus_ports.hpp`, `app/rumble_pulser.hpp`
- 変更: [main.cpp](../examples/bridge/main.cpp), [CMakeLists.txt](../examples/bridge/CMakeLists.txt)（`app/board.cpp` を追加）

**手順**
- [ ] `app/board.*` を作る
  ```cpp
  namespace gcinput::board {
  inline constexpr uint kPinToRealPad = 15;      // GP15
  inline constexpr uint kPinToRealConsole = 16;  // GP16
  void init();          // 旧 bootsel_button_init() + init_led()
  void poll_bootsel();  // 旧 handle_boot_btn_if_requested()
  }
  ```
  - `ONBOARD_LED_PIN`, `BOOT_BTN_PIN`, `g_boot_btn_requested`, `boot_btn_irq` は board.cpp の無名名前空間へ
  - フラグは CLAUDE.md の ISR ルールどおり `volatile bool` のまま
  - `BOOTSEL button pressed.` の printf は今も `#ifdef` の外なので、そのまま（現状維持）
- [ ] `app/joybus_ports.hpp` を作る
  ```cpp
  JoybusPioPort::Config make_pad_port_config();      // pio0, joybus_console プログラム, GP15
  JoybusPioPort::Config make_console_port_config();  // pio1, joybus_pad プログラム, GP16
  ```
  - `pio_claim_unused_sm` も中へ
  - 「パッド側ポートは Pico がコンソール役なので `joybus_console.pio` を使う」という逆転をコメントに残す
- [ ] `app/rumble_pulser.hpp` を作る（`RumbleOverride` → `RumblePulser`）
  - `tick(now) -> RumbleMode` を `forcing_on(now) -> bool` に改名（`On` → `true`, `Off` → `false`）
  - 分岐ロジックは触らない。OFF ギャップ中は本体の振動要求を素通しする（現状維持）
- [ ] main.cpp を差し替える
  ```cpp
  if (rumble.forcing_on(now_us)) {
      console_state.rumble_mode = gcinput::domain::RumbleMode::On;
  }
  ```

**完了条件**
- [ ] `grep -nE "gpio_|pio_claim|RumbleOverride|PIN_TO_REAL" examples/bridge/main.cpp` → 0件
- [ ] B: 両ビルド OK。Release サイズがベースラインとほぼ同じ
- [ ] L: ベースラインと同じ
- [ ] H1, H3（振動回数）, H6

---

## PR2 `refactor:` 補正モード管理を CorrectionController へ

**目的**: 補正に関する知識（ステージ構成・原点・モード・コンボ）を1クラスに集める。
LRトリガー補正を後で足すとき、ここだけ触れば済む形にする。

**触るファイル**
- 新規: `app/correction_controller.hpp`, `app/correction_controller.cpp`
- 変更: [domain/transform/pipeline.hpp](../examples/bridge/domain/transform/pipeline.hpp), [main.cpp](../examples/bridge/main.cpp), CMakeLists.txt

**手順**
- [ ] `pipeline.hpp` を変更（domain/ はヘッダオンリーのまま）
  - `add_stage` の戻り値を `bool` から `std::optional<std::size_t>`（登録したインデックス）に変える
  - `void set_enabled_mask(uint32_t mask)` を追加（`enable_mask_.store(mask, release)` を1回）
- [ ] `CorrectionController` を作る
  ```cpp
  class CorrectionController {
    public:
      enum class Mode : uint8_t { OriginFix, Correction };
      struct Update {
          bool origin_updated{false};
          bool mode_changed{false};
      };

      // ConsoleClient を構築する前に呼ぶこと（構築直後から ISR がパイプラインを読む）
      void install(domain::transform::PipelineSet &pipelines);
      // 毎ループ呼ぶ。原点の追従とコンボ検出
      Update update(const PadSnapshot &snapshot);

      Mode mode() const { return mode_; }
      // DBG 行用の一時的なアクセサ。PR5 で trace() に置き換えて消す
      domain::transform::correction::OriginOffsetContext &origin_context() { return origin_ctx_; }

    private:
      domain::transform::Pipeline *status_{nullptr};
      domain::transform::correction::OriginOffsetContext origin_ctx_{};
      uint32_t fix_mask_{0};
      uint32_t correction_mask_{0};
      Mode mode_{Mode::OriginFix};
      bool prev_combo_{false};
      uint32_t last_origin_publish_count_{0};
  };
  ```
  - `install()`: 今と**同じ順序**でステージを登録し、戻り値のインデックスから `fix_mask_` と `correction_mask_` を作る。最後に `status_->set_enabled_mask(fix_mask_)`
  - `update()`: 今の main の2ブロックをそのまま移す
    - 原点: `publish_count` が変わり、かつ `last_rx_command` が Origin / Recalibrate のとき
    - コンボ: `last_rx_command == Status` のときだけ評価し、立ち上がりでトグル
  - コンボは `constexpr std::array kToggleCombo{L, R, DpadUp, Start, Y}` と `all_pressed(input, combo)` で書く
  - モード切替は `set_enabled_mask()` 1回（下の「判断メモ」を参照）
- [ ] main.cpp を差し替える
  - `origin_ctx` グローバル, `kStage*`, `BridgeMode`, `prev_combo`, `last_origin_publish_count` を削除
  - `correction.install(ctx.transform_pipelines())` を **PadClient / ConsoleClient の構築より前**に置く
  - `Mode:` と `Origin updated` の printf は、この PR ではまだ main に残す（`Update` を見て表示。値は `snapshot.origin.input.analog` から取る）。PR4 で移す
  - DBG ブロックは `correction.origin_context()` と `correction.mode()` を使うように直すだけ（PR5 で移す）

**注意**
- ISR は `&origin_ctx_` を持ち続けるので、`CorrectionController` はパイプラインより長く生きている必要がある。main のローカルでよい（main は返らない）が、コメントに残す

**完了条件**
- [ ] `grep -nE "origin_ctx|kStage|set_stage_enabled|BridgeMode|prev_combo|add_stage" examples/bridge/main.cpp` → 0件
- [ ] main.cpp で `correction.install(...)` が `PadClient` / `ConsoleClient` の構築より上にある（目視）
- [ ] B: 両ビルド OK
- [ ] L: ベースラインと同じ
- [ ] H2, H3, H4。補正モードでは DBG 行の `lut=` が `raw=` と異なり、FIX モードでは `tx=(128,128)`

---

## PR3 `refactor:` ログ基盤を `diag/` へ移し、呼び出しを1行にする

**目的**: ログの内部構造を隠し、link 層からは意味単位の関数を1回呼ぶだけにする。

**触るファイル**
- 移動: `debug_log.hpp` → `diag/log_ring.hpp`
- 変更: [link/console_client.cpp](../examples/bridge/link/console_client.cpp), [link/pad_client.cpp](../examples/bridge/link/pad_client.cpp), [link/pad_client.hpp](../examples/bridge/link/pad_client.hpp), main.cpp

**手順**
- [ ] コミット1: `git mv examples/bridge/debug_log.hpp examples/bridge/diag/log_ring.hpp` と include 3か所の修正だけ
- [ ] namespace を `debug_log` から `gcinput::diag` に変え、リングの実体（`g_ring`, `g_ring_head`, `g_ring_tail`, `g_drop_count`）を `detail` に隠す
- [ ] 読み出し API を追加して、main の `drain_ring` をこれに乗せ換える
  - `bool try_pop(LogEntry &out)`
  - `uint32_t drop_count()`
  - Release 版は `false` と `0` を返すだけの inline 関数
- [ ] 書き込み API を意味単位に揃える
  - `log_console_rx(std::span<const uint8_t> rx)`: Status なら PollMode / Rumble 付き。取り出しは diag の中で行う
  - `log_console_tx(joybus::Command, std::span<const uint8_t>, PollMode, RumbleMode)`
  - `log_pad_rx(joybus::Command, std::span<const uint8_t>)`
  - `log_pad_transition(const char *from, const char *to)`
  - `log_pad_timeout(const char *what)`
- [ ] （推奨）`LogEntry::state_str[48]` をやめ、`const char *` を2つ持つ
  - 渡すのは文字列リテラルか `to_string()` の戻り値だけ（静的寿命なのでコピー不要）。そのことを API のコメントに書く
  - `enter_state_` の `snprintf` が消える（Release でも無駄に動いていた）
  - エントリが約84Bから約32Bに縮む
- [ ] 状態名を1つにまとめる
  - `pad_client.hpp` に `constexpr const char *to_string(PadClient::State)` を公開する
  - main の `pad_state_name` と pad_client.cpp の `state_name` を削除する
- [ ] コミット `fix:` Status 要求ログの Rumble を `rx[2]` から取る（後述「既存バグ 1」）
- [ ] コミット `fix:` `ring_push` を `save_and_disable_interrupts` / `restore_interrupts` で囲む（後述「既存バグ 2」）

**完了条件**
- [ ] `grep -rnE "debug_log|g_ring" examples/bridge --include='*.cpp' --include='*.hpp' | grep -v "^examples/bridge/diag/"` → 0件
- [ ] `grep -rnE "state_name|snprintf" examples/bridge/link examples/bridge/main.cpp` → 0件
- [ ] console_client.cpp のログ呼び出しが RX 1行、TX 1行
- [ ] B: 両ビルド OK（この PR は特に Debug が重要）
- [ ] L: ベースラインと同じ。変わってよいのは、Ready 以外で出る `[CON] << Status ... (Mode3 Rumble=N)` の `N` だけ（本体の振動要求を反映するようになる）。`WARNING: Ring buffer dropped` の頻度はベースライン以下
- [ ] H1, H5

---

## PR4 `refactor:` ドレイン・サマリーを diag::Monitor に集約

**目的**: main から `printf` と `#ifdef GCINPUT_ENABLE_LOG` を消す（DBG 行を除く）。

**触るファイル**
- 新規: `diag/log_printer.hpp`, `diag/log_printer.cpp`, `diag/monitor.hpp`, `diag/monitor.cpp`
- 変更: main.cpp, CMakeLists.txt

**手順**
- [ ] `log_printer.*` に `print_log_entry`, `StatusSummary`, drain（`try_pop` のループ）を移す
- [ ] `Monitor` を作る
  ```cpp
  class Monitor {
    public:
      Monitor(BridgeContext &ctx, CorrectionController &correction);
      void print_banner(const JoybusPioPort::Config &pad, const JoybusPioPort::Config &console);
      void tick(uint32_t now_us, PadClient::State pad_state, const ConsoleState &console,
                const CorrectionController::Update &update, const PadSnapshot &snapshot);
  };
  ```
  - `monitor.hpp` は `#ifdef GCINPUT_ENABLE_LOG` なら宣言だけ、`#else` なら全メソッドが空の inline 実装
  - `monitor.cpp` は全体を `#ifdef GCINPUT_ENABLE_LOG` で囲む
  - 中へ移すもの: 起動バナー, Ready 検出, drain + `[POLL]`, ドロップ警告, `Mode:` / `Origin updated` の表示
- [ ] 整理
  - `in_ready_state` と `was_ready` は冗長（前者は常に `ready_now` と同じ値）。前回値の bool 1つにする
  - main の `[PAD] state: A -> B` 表示を削除する（リング側の `[PAD] A -> B` と二重になっている）
  - `[PAD] Controller connected.` は Monitor が Ready への立ち上がりで出す

**完了条件**
- [ ] `grep -n "printf" examples/bridge/main.cpp` → DBG ブロックの中だけ
- [ ] `grep -nE "in_ready_state|was_ready|StatusSummary|drain_ring|last_reported_drops" examples/bridge/main.cpp` → 0件
- [ ] B: 両ビルド OK。Release サイズがベースラインと同じか少し小さい
- [ ] L: ベースラインと同じ。変わってよいのは `[PAD] state: A -> B` の行が消えることだけ（`[PAD] A -> B` は残る）。`[POLL]` は約500msごと、同じ形式
- [ ] H1, H3, H4（ログ上で確認）

---

## PR5 `refactor:` DBG トレースを移して main.cpp を仕上げる

**目的**: ステージ順を知っている場所を `CorrectionController` 1か所にし、main を完成形にする。

**触るファイル**
- 変更: `app/correction_controller.*`, `diag/monitor.*`, main.cpp

**手順**
- [ ] `CorrectionController` にトレースを追加する（`install()` のすぐ隣に書く）
  ```cpp
  struct Xy { uint8_t x; uint8_t y; };
  struct CorrectionTrace { Xy origin, raw, norm, clamp, scale, lut; };
  CorrectionTrace trace(const domain::PadState &raw);  // origin_normalize が非 const 参照を取るので非 const
  ```
- [ ] `origin_context()` アクセサを削除する
- [ ] Monitor に DBG 行を移す
  - `consume_tx_if_new`, `last_tx_publish_count`, `last_debug_log_us`, `kDebugLogIntervalUs` を Monitor へ
  - `tx` の値と `forward_lut(tx)` は Monitor 側で計算し、ステージ値は `correction.trace(snapshot.status)` から取る
- [ ] main.cpp を仕上げる
  - 不要な include を削除（`hardware/pio.h`, `joybus_*.pio.h`, `<cstring>`, `correction.hpp` など）
  - 順序: `stdio_init_all` → `board::init` → ポート設定 → `BridgeContext` → `correction.install` → クライアント → `monitor.print_banner` → ループ

**完了条件**
- [ ] `grep -nE "printf|#ifdef|origin_normalize|octagon_clamp|linear_scale|inverse_lut" examples/bridge/main.cpp` → 0件
- [ ] `grep -rnE "origin_normalize|octagon_clamp|linear_scale|inverse_lut\(" examples/bridge/app examples/bridge/diag` → `correction_controller.cpp` だけ
- [ ] `wc -l examples/bridge/main.cpp` → 120行以下
- [ ] B: 両ビルド OK。**Release サイズがベースラインより減る**（Release で走っていた DBG 用の再計算が消えるため）
- [ ] L: DBG 行が約500msごとに同じ形式で出る
- [ ] H1〜H6 一通り

---

## PR6 `refactor:` link 層の定型処理を畳む

**目的**: PadClient の状態機械と BridgeContext の epoch 周りのコピペを減らす。

**触るファイル**
- 変更: [link/pad_client.hpp](../examples/bridge/link/pad_client.hpp), [link/pad_client.cpp](../examples/bridge/link/pad_client.cpp), [link/bridge_context.hpp](../examples/bridge/link/bridge_context.hpp), [link/console_client.cpp](../examples/bridge/link/console_client.cpp)

**手順**
- [ ] `PadClient::tick` の定型処理をヘルパーにまとめる
  ```cpp
  enum class Step : uint8_t { Sent, Waiting, Done, TimedOut };
  // 未送信なら送る / 期待した応答が来たら Done / 期限切れならログ + abort して TimedOut
  template <std::size_t N>
  Step step_(const joybus::Request<N> &req, uint32_t now_us, const PadSnapshot &snapshot,
             const char *timeout_what);
  ```
  - 今の `got` ラムダは `got_(command, snapshot)` というメンバ関数にする
  - Disconnected / Resetting / BootId / BootOrigin / BootRecalibrate / WarmStatus を `step_` で書き直す（各 case 3〜5行）
  - RelayOrigin / RelayRecalibrate は `TimedOut` のときに Ready へ戻す点だけが違う
  - Ready は形が違うので無理に揃えない
  - タイムアウトの文言（`"waiting for Id (boot)"` など）は一字一句変えない
- [ ] Ready ケースの先頭にある Reset 判定を削除する
  - [tick の冒頭](../examples/bridge/link/pad_client.cpp#L82) ですでに消費されているので、実際には到達しない
- [ ] tick 冒頭の `pending_console_reset_() && state_ != ...` は評価順を変えない
  - Disconnected / Resetting 中の Reset 要求は「消費して捨てる」のが現状の挙動。その旨をコメントに書く
- [ ] BridgeContext の epoch 3組を小クラスにまとめる
  ```cpp
  class RequestEpoch {
    public:
      void __isr publish_from_isr() { epoch_.fetch_add(1, std::memory_order_relaxed); }
      uint32_t load() const { return epoch_.load(std::memory_order_relaxed); }
      [[nodiscard]] bool consume(uint32_t &seen) const;
    private:
      std::atomic<uint32_t> epoch_{0};
  };
  ```
  - BridgeContext には `pad_reset_requests()`, `pad_origin_requests()`, `pad_recalibrate_requests()` の3つのアクセサだけを残す

**完了条件**
- [ ] `grep -c "is_timeout_reached_(now_us, response_deadline_us_)" examples/bridge/link/pad_client.cpp` → 2（`step_` の中と Ready。現状は9）
- [ ] `grep -nE "consume_pad_|publish_pad_.*_request_from_isr|load_.*_epoch\(\)" examples/bridge/link/bridge_context.hpp` → 0件
- [ ] `wc -l examples/bridge/link/pad_client.cpp` → 190行前後（現状274）
- [ ] B: 両ビルド OK
- [ ] L: 状態遷移の順序とタイムアウト行の文言がベースラインと同じ
  - 接続時: `Disconnected -> BootOrigin -> BootRecalibrate -> WarmStatus -> Ready`
  - 本体 Reset 時: `Ready -> Resetting -> BootId -> BootOrigin -> BootRecalibrate -> WarmStatus -> Ready`
- [ ] H1, H4, H5 を重点的に

---

## PR7 `refactor:` / `docs:` 掃除

**触るファイル**
- 変更: `domain/transform/pipeline.hpp`, `link/shared/shared_pad_hub.hpp`, `link/shared/shared_console.hpp`, `joybus/driver/joybus_pio_port.hpp`, `joybus/protocol/protocol.hpp`, `link/console_client.hpp`, [CLAUDE.md](../CLAUDE.md), この文書

**手順**
- [ ] 未使用コードを削除する（68b70ec 時点で grep 確認済み）
  - `Pipeline::is_stage_enabled`, `empty_pipeline()`
  - PR2 以降に未使用なら `Pipeline::set_stage_enabled` も
  - `SharedPadHub::load_last_tx`
  - `JoybusPioPort::clear_rx_status`
  - `Request::expected_rx_size`（`Id{...}` などの初期化子5か所から2番目の要素も消す）
  - `ConsoleState::reset_count`（書くだけで読まれていない。`SharedConsole` の Reset ケースは `break` だけでよい）
- [ ] `ConsoleClient::write_tx` を private にする
- [ ] CLAUDE.md のディレクトリ構成を更新する（`debug_probe`, `input_viewer`, bridge の `app/` と `diag/`）
- [ ] この文書を `docs/bridge_architecture.md`（構成と各モジュールの責務）に書き換えるか、完了済みとして残す

**完了条件**
- [ ] `grep -rnE "is_stage_enabled|empty_pipeline|load_last_tx|clear_rx_status|expected_rx_size|reset_count" examples/bridge` → 0件
- [ ] B: 両ビルド OK。Release サイズが PR6 と同じか小さい
- [ ] L: ベースラインとの差分が、PR3 / PR4 で意図したものだけ
- [ ] H1〜H6 一通り

---

## 既存バグ（PR3 で `fix:` コミットにする）

1. **Status 要求ログの Rumble が常に 0**
   [console_client.cpp:34-35](../examples/bridge/link/console_client.cpp#L34-L35) は PollMode を `rx[1] & 0x07`、Rumble を `(rx[1] >> 3) & 0x03` で取っている。
   Status 要求は `{0x40, poll, rumble}` なので、Rumble は `rx[2]` が正しい（[SharedConsole](../examples/bridge/link/shared/shared_console.hpp#L27-L29) は正しく読んでいる）。
   Id 応答 byte3 のビット配置と混同したものと思われる。
2. **ログのリングの書き込みが競合しうる**
   `ring_push` はパッド側 ISR・コンソール側 ISR・main（状態遷移・タイムアウト）から呼ばれるが、ロックがない。
   main が書き込んでいる途中に ISR が割り込むと、同じスロットを上書きしてエントリが壊れる。Debug 専用なので致命的ではない。

## スコープ外（気づいたが今回はやらない）

- Debug ビルドでは、コンソール側 ISR が応答を作る**前に** LogEntry を memcpy している（応答レイテンシに乗る）。PR3 でエントリを縮めれば軽くはなる
- `PadStatusFlags::origin_sent` の FIXME（true 固定）
- debug_probe / measure への反映
- LRトリガーの計測・補正（この計画の後で。補正は `CorrectionController::install()` にステージを追加し、`correction_mask_` に OR する）

## 判断メモ

- **PR2 のモード切替を1回の atomic store にする**: 今は `set_stage_enabled` を5回呼ぶので、切替の瞬間に「全ステージ無効」や「FIX と補正が両方有効」の1フレームが混ざる窓がある。1回の store にするとこの窓が消える。計画中で唯一の意図的な挙動変化（よい方向）。厳密に現状維持したい場合は、今の for ループを残す
- **PR4 で状態遷移ログを一本化する**: 二重に出ていたものをリング側に寄せる。Debug ログの見た目だけの変化
