# pico-gc-bridge

[![build](https://github.com/LitMc/pico-gc-bridge/actions/workflows/build.yaml/badge.svg)](https://github.com/LitMc/pico-gc-bridge/actions/workflows/build.yaml)

GCコントローラーのスティック入力を補正するRaspberry Pi Picoブリッジファームウェア。

## 前提

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.x
- CMake 3.13+
- Ninja
- Python 3.13+（計測ツール用）
- [uv](https://docs.astral.sh/uv/)（Python パッケージ管理）

## ビルド

```bash
# pico-sdk をクローン
git clone --depth 1 https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init --depth 1 && cd ..

# Configure + Build
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=./pico-sdk
cmake --build build --target bridge
cmake --build build --target measure
```

生成される UF2 ファイル:
- `build/examples/bridge/bridge.uf2`
- `build/examples/measure/measure.uf2`

## 計測ワークフロー

```bash
# Python 環境セットアップ
uv sync

# ROI画像をダンプ
uv run tools/dump_measurement_rois.py --video <video> --rois resources/rois/rois.json --out-dir <output>

# テンプレート画像を生成
uv run tools/make_templates_from_dump.py --dump-dir <dump> --out-dir <templates>

# 計測 CSV を生成
uv run tools/generate_measurement_csv.py --video <video> --rois resources/rois/rois.json --templates <templates> --out <output.csv>

# 逆LUTヘッダを生成
uv run tools/generate_inverse_lut.py --csv <measurements.csv> --out examples/bridge/domain/transform/inverse_lut_data.hpp
```

## ディレクトリ構成

```
examples/
  bridge/     GCコントローラー補正ブリッジ（メインファームウェア）
  measure/    スティック計測ファームウェア
tools/        計測データ処理スクリプト
resources/    ROI定義等のリソース
docs/         設計ドキュメント
```

## ライセンス

MIT
