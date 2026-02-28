# 測定の意義
本プロジェクトはスティック入力値をPicoで補正することによってSwitch 2のGameCube Classics版での挙動を実機へ近づけることを目的としている。
補正するには
- 正確な生のコントロールスティック入力値（以後、「生の入力値」と呼ぶ）
  - X軸とY軸がそれぞれ0..255の範囲の値をとる
  - 256 * 256 = 65536通り
- ゲーム側が認識しているスティック入力値（以後、「ゲーム側入力値」と呼ぶ）
  - ゲームタイトルはF-ZERO GX
  - F-ZERO GXにはゲーム内のキャリブレーション機能が存在
  - キャリブレーション機能の画面にてスティック入力値を表示する機能がある
  - そこで表示される入力値は生の入力値と異なる
  - 「補正前」座標のフォーマット
    - X軸とY軸がそれぞれ-125..125の範囲の値をとる
    - （実機では）生の入力値から128を引き、端のわずかな区間をクランプしたものに等しい
      - -128..-126を-125に、126..127を125に
    - Switch 2版ではこの値が実機と異なる
      - GameCube Classics内部のどこかの処理で補正をかけている様子
      - この補正を打ち消す逆補正をPicoでかけることで実機に近づけるのが本プロジェクトの目的

の組が必要。どの入力がどう認識されるかすべてのパターンで分かれば、それを打ち消す逆補正を計算できる可能性があるため。

# 測定アプローチ
生の入力値はシリアル通信を介して正確な値をテキスト形式でリアルタイムに取得できる。
一方ゲーム側入力値の値は画面に映るだけなので、テキストデータとして得るには「目で読みとる」必要がある。

本プロジェクトでは65536組の生の入力値とそれに対応するゲーム内入力値を動画として録画することにした。
この動画を見ながら目で読んで値を書き出していけばデータ化できる。
しかしデータ数が多すぎて人力では難しいため読み取りとテキストデータ化をプログラムで行うこととする。

# 測定方法
2つの値をもつ入力値（X軸、Y軸）が生とゲーム側で2つ、計4つの値が65536組あることになるが、これをまとめてOCRで読もうとすると誤認識が避けられない。
複雑な文字認識をなるべく減らすため以下の方法をとる。

- スティック入力はPicoから本体へ正確な値を送信
  - 実装は `examples/measure`
- スティックの生入力値とあわせて同期用のフレームカウントを送る
  - 65536個あるパターンのうち何個目か
- 生の入力値はバーコード（後述）として動画に埋め込み
  - 本来シリアル通信から正確な値を得られるためゲーム側入力値のように文字を再度読み取る必要はない
  - 文字ではなく白黒のバーコードとすることでドットの輝度から値を読み取れるようにした
    - 結局映像には載るが読み取るべき文字が減る
- ゲーム側入力値はゲーム画面を切り出して拡大し白黒2値に近づける色補正を適用
  - 文字の形と位置が一定なので機械的なパターンマッチで判別できるはず

## 生の入力値のバーコード化
- `tools/overlay_server.py`
- 特定のフォーマットをもつシリアル通信のログから生の入力値をバーコードとして表示
- 入力値だけでなく送信値の識別用フレームカウントやプリアンブル、誤り検出用の8ビットCRCを含む

### 実行イメージ
```
uv run tools/overlay_server.py --serial /dev/tty.usbmodem14102
```
<img width="918" height="195" alt="image" src="https://github.com/user-attachments/assets/88360970-34c0-4a48-bc83-5cb62b39432d" />

### ログの形式
```
D,frame,sx,sy,crc
```
名称 | 意味 | 値の例
-- | -- | --
D | 計測データを表す固定のマーカ | `D`
frame | 何番目の送信値かを表すカウント | `1234`
sx | 生のスティック入力値 X軸 （10進、0..255）| `128`
sy | 生のスティック入力値 Y軸（10進、0..255） | `128`
crc | 読み取り誤認識の検出用。frame, sx, syから計算した8bitのCRC | `0x45`

### バーコードの形式
左右のガードを除くとプリアンブル、ペイロード、CRCの計48ビット。
```
[preamble 8] [payload 32] [crc8 8]
```
`1`は長い縦棒、`0`は短い縦棒で表現。

#### プリアンブル
`0xA5`固定。バーコードの先頭を読み取って`10100101`になっていれば読めているだろうというマーカ。

#### ペイロード
何番目の送信かを表すフレームとスティック入力値。

byte index | 内容 | ビット
-- | -- | --
0 | frame_hi | frame の上位8bit
1 | frame_lo | frame の下位8bit
2 | sx | 生のスティック入力値 X軸 0..255
3 | sy | 生のスティック入力値 Y軸 0..255

#### CRC
CRC自体は8ビット。
対象：payload の4バイト（preambleは含まない）
アルゴリズム：CRC-8（poly=`0x07`、init=`0x00`、MSB-first）
計算対象バイト列：`[frame_hi, frame_lo, sx, sy]`

# 動画からのテキストデータ化フロー
現在は 1スクリプト1機能 に分割して運用する。

- ROIの切り出し(dump): `tools/dump_measurement_rois.py`
- テンプレート作成(ラベリング): `tools/make_templates_from_dump.py`
- CSV生成(バーコード + テンプレートマッチ): `tools/generate_measurement_csv.py`
- 可視化デバッグ: `tools/debug_template_match.py`

## 1) ROI dump の作成
計測動画から、ゲーム側入力値の各ROI画像を保存する。

```bash
uv run tools/dump_measurement_rois.py \
  --video resources/videos/<video>.mp4 \
  --rois resources/rois/rois.json \
  --out-dir resources/templates/raw
```

指定フレームを一括dumpする例:

```bash
uv run tools/dump_measurement_rois.py \
  --video resources/videos/<video>.mp4 \
  --rois resources/rois/rois.json \
  --out-dir resources/templates/raw \
  --frames-file resources/templates/frames_to_dump.txt
```

`--frames-file` は 1行1フレーム番号。
例:

```text
342
812
1222
1352
```

dumpツール主要オプション:

- `--start-frame`: 手動モード開始フレーム
- `--frames`: `342,812,1222` のようなカンマ区切り指定
- `--frames-file`: 1行1フレーム番号のテキストファイル

操作キー:

- `space`: 再生/停止
- `.` `,`: 1フレーム送り/戻し
- `e` `f`: 10フレーム送り/戻し
- `g`: 任意フレームへジャンプ
- `d`: 現在フレームのROIを保存
- `q` or `Esc`: 終了

保存ファイル名の形式:

```text
f000123_x_sign.png
f000123_x_100.png
...
f000123_y_1.png
```

## 2) テンプレート画像の作成
dump済みROIに対して `gx gy` を入力し、テンプレート画像を作る。

```bash
uv run tools/make_templates_from_dump.py \
  --raw-dir resources/templates/raw \
  --out-dir resources/templates/game_digits
```

入力仕様:

- 1フレームごとに `gx gy` を入力（例: `-12 34`）
- `s`: スキップ
- `q`: 終了

テンプレート命名規約:

- 数字: `digit_0*.png` ... `digit_9*.png`
- マイナス: `sign_minus*.png`
- (任意)プラス: `sign_plus*.png`

## 3) 計測CSVの生成
テンプレートマッチとバーコード復号を同時に行い、`frame,sx,sy,gx,gy` を出力する。

```bash
uv run tools/generate_measurement_csv.py \
  --video resources/videos/<video>.mp4 \
  --rois resources/rois/rois.json \
  --templates-dir resources/templates/game_digits \
  --out-csv resources/switch2/readings.csv \
  --out-diagnostics-csv resources/switch2/readings_diagnostics.csv \
  --workers 4 \
  --chunk-size 1000 \
  --log-every 1000 \
  --min-support 2
```

並列化オプション:

- `--workers`: ワーカープロセス数（`1` で単一プロセス互換、既定 `4`）
- `--chunk-size`: 1タスクあたりのフレーム数（既定 `1000`）

目安:

- まず `--workers 4` を基準にして、`2/4/6` で速度比較
- 発熱や負荷でスループットが落ちる場合は `workers` を下げる
- `workers > 1` でも最終集約は親プロセスで1回実施するため、連続run判定のロジックは維持される

進捗ログ（`--log-every` フレームごと）:

- `accepted`: 有効観測として採用された件数
- `decode_fail`: 48bit復号自体が失敗した件数
- `preamble_fail`: preamble 不一致件数
- `crc_fail`: CRC 不一致件数
- `sample(...)`: 直近の成功サンプル `(raw_frame,sx,sy,gx,gy,conf)`

出力CSV:

- 主出力: `frame,sx,sy,gx,gy`
- 診断出力(任意): `frame,support_len,confidence_sum`

## 同期ずれへの対応方針
ゲーム側入力値(gx,gy)とバーコード由来の生入力値(frame,sx,sy)は、動画内で数フレームずれることがある。
そのため、CSV確定時は以下のルールで同期ずれを吸収する。

1. 動画フレームごとに観測値 `(frame,sx,sy,gx,gy,conf)` を収集
2. バーコードの `frame` ごとに観測列をグループ化
3. 同一 `frame` 内で `(sx,sy,gx,gy)` が連続して出現する run を抽出
4. **最長run** の組を、その `frame` の計測値として採用
5. 同点時は run内の `conf` 合計が高い方を採用
6. run長が `--min-support` 未満の候補は除外

この手順により、一時的な読み取り揺れや表示遅延の影響を受けにくくする。

## 可視化デバッグ
`tools/debug_template_match.py` は可視化/デバッグ専用。
CSV生成やdumpはこのスクリプトでは行わない。

```bash
uv run tools/debug_template_match.py \
  --video resources/videos/<video>.mp4 \
  --rois resources/rois/rois.json \
  --templates-dir resources/templates/game_digits
```

## 計測CSVの対応可視化
`readings.csv` (`frame,sx,sy,gx,gy`) から、`sx,sy -> gx,gy` の対応を俯瞰するPNGを生成する。

```bash
uv run tools/visualize_measurement_map.py \
  --input resources/switch2/readings.csv \
  --diagnostics resources/switch2/readings_diagnostics.csv \
  --outdir resources/switch2/plots
```

主な出力:

- `map_heatmaps_raw.png` / `map_heatmaps_centered.png`
- `map_surface_gx.png` / `map_surface_gy.png`
- `map_residuals.png`
- `map_scatter_pairs.png`
- `map_slices.png`
- `map_diagnostics.png` (`--diagnostics` 指定時)

## 計測CSVのインタラクティブ可視化
ブラウザで回転・拡大・ホバー確認できるHTMLを生成する。

```bash
uv run tools/visualize_measurement_map_interactive.py \
  --input resources/switch2/readings.csv \
  --diagnostics resources/switch2/readings_diagnostics.csv \
  --outdir resources/switch2/plots_interactive
```

主な出力:

- `interactive_dashboard.html` (全体を1ページで切り替え表示)
- `interactive_heatmaps_raw.html` / `interactive_heatmaps_centered.html`
- `interactive_surface_gx.html` / `interactive_surface_gy.html`
- `interactive_surface_residual_mag.html` (理想なら平坦になる residual 3D)
- `interactive_residuals.html`
- `interactive_scatter_pairs.html`
- `interactive_slices.html`
- `interactive_diagnostics.html` (`--diagnostics` 指定時)

## 補正変換の構築

readings.csv から補正変換 P(s) を構築する数学的パイプラインについては [docs/transforms.md](transforms.md) を参照。
