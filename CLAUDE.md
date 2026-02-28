# pico-gc-bridge

## 概要

GCコントローラーとRaspberry Pi Picoを用いたブリッジ補正システム。
スティック入力の補正・計測を行うファームウェアと、計測データ処理用のPythonツールで構成される。

## 技術スタック

- **ファームウェア**: C++20 / CMake / Pico SDK 2.x / PIO
- **Python ツール**: Python 3.13+ / uv
- **CI**: GitHub Actions（Docker: `ghcr.io/litmc/gc-playground/builder:latest`）

## ディレクトリ構成

```
examples/
  bridge/     ← ブリッジファームウェア（メインターゲット）
  measure/    ← 計測ファームウェア
tools/
  measurement_lib/  ← 計測データ処理ライブラリ
  *.py              ← LUT生成・テンプレート作成等のスクリプト
resources/
  rois/       ← ROI定義（rois.json）
docs/         ← 設計ドキュメント
```

## ビルド手順

```bash
# Debug ビルド
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build --target bridge
cmake --build build --target measure

# Release ビルド
cmake --preset release -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build --preset release
```

## コーディング制約

- **ISRルール**: IRQハンドラ内ではメモリ確保・printf禁止。volatileフラグでメインループに通知
- **タイミング制約**: JoyBus通信は4us/bitの厳密タイミング。PIOプログラムで処理
- **共通化の温度感**: bridge/measure間でコードが重複していても、無理に共通化しない。各ターゲットの独立性を優先
- **ヘッダオンリー**: domain/ 配下は .hpp のみ（テンプレート・constexpr中心）

## Pico への書き込み

1. BOOTSELボタンを押しながらUSB接続（またはGP26ボタンでBOOTSELモードへ遷移）
2. `build/examples/bridge/bridge.uf2` をマウントされたドライブにコピー

## コミットルール

- 日本語メッセージ
- 接頭辞: `feat:`, `fix:`, `docs:`, `refactor:`, `ci:`
- 例: `feat: スティック補正テーブルを更新`

## PR ワークフロー

- ブランチ命名: `feat/xxx`, `fix/xxx`
- CIが通ってからマージ

## 触れてはいけないファイル

- `pico_sdk_import.cmake` — Pico SDK標準ファイル。変更禁止

## Entire 連携

- `.entire/` 配下は手動作成禁止（Entireが自動管理）
