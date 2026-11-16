# NVR デモ

## ライブ表示の停止回避（2026-11-16）

ライブ表示は最新映像への追従を優先します。3枚の表示FIFOが満杯になった場合は、まだ取り出されていない最古のフレームを捨て、新しいフレームを受け入れます。初期蓄積は2フレーム周期のままです。表示時刻が大幅に遅れた場合は滞留分を破棄して追従し、起動直後の集中到着でも表示予定が先へ伸び続けないよう補正します。描画・GPU処理中のサーフェスは完了確認前に再利用しません。

ライブ表示を初めて開始してから10秒未満の破棄はdrop集計から除外します。この除外期間は全カメラ共通で、再接続・表示再開では延長しません。起動時の破棄もフレーム診断には残し、presentation_startup_dropとして区別します。10秒以降の表示破棄はタイルの1行表示に drop N、AI Statisticsに合計件数を表示し、通常ログにもカメラ別に記録します。実表示通知での破棄も別イベントとして数えます。latency 10s maxは実際に表示されたフレームのみの値であり、低い値だけで取りこぼしがないとは判断できません。録画経路・保持期間・AI頻度は変更していません。

診断解析の --allow-live-drops は明示的な表示破棄を照合しますが、全フレーム表示の合格にはしません。既定の厳密な検査は維持します。不正なPTSやデバイス障害は隠しません。以下の過去の「満杯時100msで停止」「破棄なし」の説明は当時の方式で、現在のライブ表示方針は本節が優先します。


NVR デモは、このフレームワークで作ったネットワークビデオレコーダーのサンプルです。登録したすべての RTSP カメラを録画しながら、最大 16 台をデスクトップに表示し、表示中のカメラで人物や車両の検出とトラッキングを行います。録画はタイムラインから再生でき、ネイティブの埋め込みモデルを用意すれば人物を文章や画像で検索することもできます。

## 画面構成

![NVR デモの画面構成（模式図）](images/nvr-ui-ja.svg)

| タブ | できること |
| --- | --- |
| Live | カメラを 1・4・9・16 分割のタイルに割り当てて表示します。検出枠とトラッキング ID を重ねて描画します |
| Search | 人物を文章、画像の領域、服装などの条件で検索します（別途セットアップが必要です） |
| AI Statistics | 最初は Simple 表示で、映像デコード、GPU の AI 推論、NPU の AI 推論、人物追跡の 4 項目を、直近 1 分の推移グラフ付きで表示します。Detailed 表示では推論の処理時間、検出の頻度、キューの状態を表示します |
| Cameras | カメラの追加・編集・削除、録画の保持期間の設定、録画の削除を行います |

タイルをダブルクリックすると 1 画面表示になり、左側の縦のタイムラインで録画を再生できます。

![1 画面表示と録画のタイムライン（模式図）](images/nvr-single-ja.svg)

タイムラインの緑の区間をクリックするとその時刻から再生し、マウスを重ねるとその時刻のプレビューを表示します。**Back to Live** でライブ表示に戻ります。表示していないカメラも録画は続けます。

## 起動する

[インストール](install.ja.md) を終え、`source scripts/env.sh` を実行したシェルで作業します。デスクトップ（Wayland または X11）と、AMD の GPU による OpenGL ES 3 が必要です。

### 1. モデルを準備する

[クイックスタート](quickstart.ja.md) と同じ手順で、バッチサイズ 4 のモデルも作ります。

```bash
mkdir -p artifacts/nvr
curl -fsSL -o artifacts/nvr/yolov8n.pt \
  https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8n.pt
PATH="$PWD/build/src/gst:$PATH" .deps/model-export/bin/python \
  examples/nvr/scripts/export_nvr_model.py artifacts/nvr/yolov8n.pt artifacts/nvr/nvr-model --batch=4
```

`artifacts/nvr/nvr-model/` に `yolov8n-b4.mxr`、`yolov8n-b1.mxr`、`model-info.json` ができれば成功です。

### 2. カメラを用意する

RTSP カメラがない場合は、付属の `amdva-rtsp-files` で動画ファイルをカメラの代わりに配信できます。ファイルごとに 1 つの URL を割り当て、ファイルの終わりまで来ると先頭から繰り返します。別のターミナルで実行し、デモを使っている間は動かしたままにします。

```bash
build/examples/rtsp-files/amdva-rtsp-files --port 8558 \
  artifacts/quickstart/bus.h264 artifacts/quickstart/out.mp4
```

`rtsp://127.0.0.1:8558/cam0` と `rtsp://127.0.0.1:8558/cam1` が表示されれば成功です。入力は H.264 または H.265 の、プログレッシブ、8 ビット、2560×1440 以下の映像です。

### 3. 設定ファイルを作る

次の内容で `artifacts/nvr/nvr-local.json` を作ります。パスは設定ファイルからの相対パスです。

```json
{
  "model": "nvr-model/yolov8n-b4.mxr",
  "single_model": "nvr-model/yolov8n-b1.mxr",
  "model_info": "nvr-model/model-info.json",
  "recording_root": "nvr-recordings",
  "sources": [
    {"id": "cam0", "name": "Camera 0", "uri": "rtsp://127.0.0.1:8558/cam0", "selected": true},
    {"id": "cam1", "name": "Camera 1", "uri": "rtsp://127.0.0.1:8558/cam1", "selected": true}
  ]
}
```

カメラは起動後に **Cameras** タブからも追加できます。変更は設定ファイルに保存されます。

### 4. 起動する

```bash
WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland-egl \
  build/examples/nvr/amdva-nvr artifacts/nvr/nvr-local.json
```

X11 のデスクトップでは `QT_QPA_PLATFORM=xcb` を指定します。ウィンドウが開き、2 台のカメラの映像に検出枠と ID が表示されれば成功です。録画は `artifacts/nvr/nvr-recordings/` に保存されます。

起動するたびに、そのディレクトリにある以前の録画は削除されます。残したい録画は、起動の前に別の場所へ移してください。

## 画面を使わずに動かす

`amdva-streams` は、同じ設定ファイルで受信、録画、検出だけを行います。ディスプレイがなくても動き、1 秒ごとに状態を JSON で出力します。

```bash
build/examples/nvr/amdva-streams artifacts/nvr/nvr-local.json 20
```

20 秒間動作し、最後の行が `{"live":2,"recording":2,...}` となっていれば成功です。

## 人物検索

人物検索には、外部のEmbeddingGemma 2ランタイム、またはこのリポジトリでビルドするMobileCLIP2ランタイムを使います。検索結果を同じ人物ごとのカードにまとめるには、さらに ReID モデル（OSNet）を用意します。準備の手順と設定項目は [人物検索](../dev/nvr/nvr-search.ja.md) を参照してください。

## 仕組み

NVR デモは、このフレームワークの部品、GStreamer の標準の要素、NVR デモ独自の実装を組み合わせて作っています。次の図は、カメラの映像がどう処理され、それぞれの処理がどのハードウェアで動くかを示します。各ハードウェアについては [仕組み](concepts.ja.md) の「Ryzen AI APU のハードウェア」を参照してください。人物検索の埋め込み（Embedding）は、MobileCLIP2 を NPU で計算する設定（`"embedding_model": "mobileclip2-s2"`、`"mobileclip_backend": "npu"`）の場合を示しています。

[![NVR デモの処理の流れとハードウェア](images/nvr-architecture-ja.svg)](images/nvr-architecture-ja.svg)

図をクリックすると拡大して表示します。MobileCLIPの緑の矢印はアプリケーションからEPへ入力を渡す範囲を示し、EP内部のコピーや変換は検証対象に含みません。箱の色は処理を実行するハードウェア、右上の札は部品の提供元を表します。矢印の色はデータの受け渡し方で、緑は同じバッファーを共有する受け渡し（ゼロコピー）、赤は CPU と GPU または NPU の間のコピー、灰色は CPU のメモリにある圧縮データやメタデータです。

**受信と録画**: 受信した圧縮データを、CPU がそのまま MKV ファイルに書き込みます。映像をデコードしないので負荷が小さく、画面に表示していないカメラも録画を続けます。録画の再エンコードを設定したカメラは、デコードした映像を GPU で縮小し、VCN でエンコードしてから録画します（[再エンコード録画](../dev/nvr/vms.ja.md)）。

**ライブ表示と物体検出**: VCN でデコードした映像は、GPU のメモリから出ないまま、VPE で縮小と色の変換をされ、画面の表示（OpenGL ES）と物体検出（HIP、MIGraphX）に使われます。CPU に戻るのは検出結果（1 フレームあたり 8 KB）だけで、それを使って ID を割り当て、検出枠を描きます。

**人物追跡と検索**: 人物の特徴量（ReID）の前処理、検索用画像の保存、NPU への受け渡しは CPU で行うため、ここで初めて映像を CPU にコピーします。コピーするのは、GPU で人物の部分だけを切り出して縮小した小さな画像（最大 384×384）で、フレーム全体はコピーしません。ReID のモデルは GPU で、埋め込みのモデルは別のプロセスから NPU で動かします。検索の文章の埋め込みとベクトル検索は CPU で行います。

**再生**: 録画ファイルを CPU が読み込み、そこから先はライブ表示と同じく、VCN、VPE、GPU の間をゼロコピーで受け渡します。

| 処理 | ハードウェア | データの受け渡し | 提供元 |
| --- | --- | --- | --- |
| RTSP の受信 | CPU | 圧縮データ（CPU のメモリ） | GStreamer の `rtspsrc` と、フレームワークの `RtspIngest` |
| 録画 | CPU | 圧縮データのまま書き込み | GStreamer の `splitmuxsink` と、フレームワークの `SegmentRecorder`、`Catalog` |
| デコード | VCN | 映像を GPU のメモリ（NV12）に出力 | GStreamer の `vah264dec`、`vah265dec` |
| 表示用の縮小と色の変換 | VPE | ゼロコピー | フレームワーク |
| 画面の表示、検出枠と文字の描画 | GPU（OpenGL ES） | ゼロコピー | NVR デモ |
| 推論の入力への縮小 | VPE | ゼロコピー | フレームワーク |
| 正規化、推論（YOLO）、検出枠の絞り込み | GPU（HIP、MIGraphX） | ゼロコピー。検出結果だけ CPU に読み戻し（8 KB） | フレームワーク |
| ID の割り当て | CPU | メタデータだけ | フレームワーク（`gvatrack`） |
| 人物の切り出し | VPE、GPU（HIP） | 切り出した画像だけ CPU にコピー（最大 384×384） | NVR デモ |
| ReID | CPU、GPU（MIGraphX） | 入力を GPU に、特徴量（1 人 2 KB）を CPU にコピー | NVR デモ |
| 人物の対応付け、解析結果の保存 | CPU | — | NVR デモ、フレームワークの `Catalog` |
| 埋め込み（MobileCLIP2） | GPU（前処理）、GPU／NPU（推論） | 同一プロセスで入力を共有。EP内部はコピー監査対象外 | NVRデモ、MIGraphX、ONNX RuntimeとRyzen AIソフトウェア |
| 検索用画像の保存、ベクトル検索、文章の埋め込み | CPU | — | NVR デモ、SQLite と sqlite-vec |
| 再エンコード | GPU（縮小）、VCN | ゼロコピー。圧縮データだけ CPU へ | フレームワークの `Transcoder` と `amdencode`、GStreamer の `vapostproc` |
| 再生、タイムラインのプレビュー | CPU（読み込み）、VCN、VPE、GPU | 読み込み以降はゼロコピー | フレームワークの `PlaybackSession`、GStreamer の `matroskademux` |

提供元の「フレームワーク」は、このリポジトリの GStreamer プラグインと SDK ライブラリ（[SDK でアプリケーションに組み込む](sdk.ja.md)）、「NVR デモ」は `examples/nvr` の実装です。推論は MIGraphX、NPU での推論は ONNX Runtime と Ryzen AI ソフトウェアを使います。埋め込みに既定の EmbeddingGemma 2 を使う場合や、MobileCLIP2 を GPU で動かす場合は、埋め込みも GPU で計算します。処理の詳しい構成は [NVR の開発者向け情報](../dev/nvr/README.ja.md) を参照してください。

## さらに詳しく

- 設定項目、処理の構成、検証結果: [NVR の開発者向け情報](../dev/nvr/README.ja.md)
- 画面の操作と録画の管理: [デスクトップの映像管理](../dev/nvr/vms.ja.md)

## 準備済み1440pデモを別の環境に移す

`artifacts/1440p-demo`を使っている場合は、[Git管理するテンプレートと移行手順](../../examples/nvr/demo/1440p/README.ja.md)を使えます。起動・設定・画面配置はGitで管理し、モデルと動画は別途搬出します。移行先のパスを指定してローカル設定を生成でき、元PCの絶対パスを設定ファイルへ引き継ぐ必要はありません。NPUキャッシュには内部の絶対パスやSDK依存もあるため、移行先での再準備と実行検証が必要です。


## ライブ映像の入力→実表示遅延

ライブ映像下部の`latency 10s max XX.X ms`は、入力フレームの先頭RTPパケットを
受信してから、Waylandの実表示通知が示す提示時刻までの遅延です。直近10秒の最大値を
カメラ別に表示します。起動直後は取得済みの範囲で集計します。FIFO待ち・表示準備・描画・
コンポジターでの待ちを含み、GPUだけのデコード時間ではありません。カメラの撮像や、
アプリへ到着する前のネットワーク時間も含みません。

実表示が確認できた画像だけを一度ずつ集計し、通知が届いたCPU時刻やswap時刻で代用しません。
現在描画中の画像の予測ではなく、既に表示された画像の統計です。入力時刻の欠測、無効な時刻、
集計容量超過が窓内にある場合、世代の異なる画像、直近10秒に実表示がない場合は、
ゼロや以前のカメラの値で代用せず`N/A`にします。診断ログの有無によらず動作し、
この表示のためのGPU同期や画像CPU読出しは追加しません。従来の入力→デコーダー出力の
ソフトウェア計測は、詳細なタイミング統計に残しています。

10秒の集計窓は通知を受領した時刻で管理しますが、遅延の終点は通知が示す提示時刻のままです。提示時刻はCPU受領時刻をわずかに先行する場合があり、その前後関係だけで欠測にしたり受領時刻へ丸めたりしません。

## AI StatisticsのGPU-readyレイテンシ

Video decodeカードのinput→GPU-ready P99は、各カメラの直近10秒のP99のうち最大の値です。
入力先頭RTPの受信から、デコーダー出力サーフェスがVA-APIでReadyと確認できるまでを測ります。
従来の約1msというinput→decode値はCPU側のデコーダー出力通知までであり、非同期GPU処理の完了を保証しません。
従来値はカメラ詳細の「Input → decoder output notification」と、診断JSONのinput_decodeに残します。

完了確認は別スレッドで行い、映像処理へGPU完了待ちや画素のCPU転送を追加しません。
約1ms間隔のポーリングとCPUの実行待ちを含む観測値なので、純粋なVCNの実行時間ではありません。
実際の観測間隔は詳細ツールチップのpolling/scheduling intervalで確認できます。
観測失敗・容量超過・入力時刻欠測が直近10秒にある場合はN/Aにし、出力通知値やゼロへ切り替えません。
画面下部の入力→実表示レイテンシとは終点が異なり、表示用FIFOや描画・コンポジターの待ちは含みません。
30fpsの33.3msは入力フレームの周期であり、各フレームのデコードに必ず33.3msかかるという意味ではありません。
