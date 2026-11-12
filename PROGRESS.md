# Ubuntu 26.04 / ROCm 10.1 対応の進捗

更新日: 2026-11-12

## 現在の状態

- Ubuntu 26.04.1 LTS、ROCm 10.1、AMD Radeon 8060S Graphics（gfx1151）でフレームワークをビルドした。
- 実行ファイルには `build/examples/nvr/amdva-nvr` と `build/examples/nvr/amdva-streams` がある。実行前に `source scripts/env.sh` が必要。
- `ubuntu-2604-rocm-101` ブランチに Ubuntu 26.04 対応、16動画デモの起動・停止スクリプト、NPU 相互運用テストの修正をコミットしている。ブランチは DL Streamer ヘッダー修正の `fix-dlstreamer-cstdint` を基点とする。
- 16動画デモは GPU の YOLO11m と OSNet、NPU の MobileCLIP2-S2 を使用する。GUI の16分割表示、各モデルの推論、および短時間の同時実行を確認した。実カメラ入力、長時間の安定性、データセット全体での精度は未検証。

## 16動画デモの設定

`./start_all.sh` は `amdva-rtsp-files` で動画を配信し、NVR の GUI を起動する。`./stop_all.sh` はこのスクリプトが起動した NVR と RTSP サーバーを停止する。カメラの代わりに動画を使用し、NVR には `rtsp://127.0.0.1:18558/cam0` から `cam15` を入力する。NVR に直接ファイルを入力する試作は破棄した。

```bash
./start_all.sh
./stop_all.sh
```

起動設定は `artifacts/video-demo/nvr-rtsp-videos.json`、配信一覧は `artifacts/video-demo/rtsp-files.txt` にある。入力動画16本とライセンス情報は `videos/` にある。Video 03 と 05 は元動画の色形式が NVR の入力条件に合わなかったため、BT.709・リミテッドレンジに変換したファイルを配信一覧で使用する。起動時に `artifacts/video-demo/run/config.json` を生成し、ログも同じディレクトリに保存する。GUI は起動時に録画ルートの既存録画を削除するため、スクリプトは起動ごとに別の録画ディレクトリを作成する。

| 項目 | 現在の設定 |
| --- | --- |
| 検出 | YOLO11m、GPU、FP32、バッチ1、各入力1 FPS |
| 人物再識別 | OSNet x1.0、MSMT17、GPU、FP16、バッチ1 |
| 画像埋め込み | MobileCLIP2-S2、NPU、BF16、バッチ1 |
| 検索 | 有効。Embedding 間隔3000 ms、最小人物高さはフレーム高さの5% |

YOLO のモデルと設定は `artifacts/video-demo/`、ReID は `artifacts/reid-model/`、MobileCLIP の NPU バンドルは `artifacts/mobileclip-npu/` にある。Ryzen AI 1.9 の環境は `$HOME/ryzenai_1_9/venv`、ORT SDK はその `onnxruntime/` にあり、`AMDVA_BUILD_MOBILECLIP=ON`、`AMDVA_BUILD_NPU=ON` でビルドした。実機は NPU Strix Halo（aie2p）、XRT 2.26.89、ORT 1.30.0 を使用する。NPU の初期化には Ryzen AI 環境の読み込みと Python 共有ライブラリの先行読み込みが必要で、`start_all.sh` が設定から環境を読み取って行う。

これらのモデル、設定、動画、依存環境は Git 管理外であり、チェックアウトだけではデモを再現できない。コンパイル済みモデルと NPU キャッシュは対象のハードウェアと実行環境に依存する。

## 変更内容

- `dependencies.lock.json` の Ubuntu パッケージを 26.04（resolute）向けに更新し、GLib、EGL、Qt などの依存関係を追加した。GStreamer は 1.28.2 のまま。
- `scripts/bootstrap.py` は固定パッケージの展開、pkg-config パスの調整、GStreamer と本体のビルドを行う。ROCm と MIGraphX は自動インストールしない。
- `scripts/env.sh` と CMake の ROCm 既定パスは `/opt/rocm/core-10.1`。MIGraphX の既定パスは `.deps/migraphx`。それぞれ `AMDVA_ROCM_ROOT`、`AMDVA_MIGRAPHX_ROOT` で変更できる。
- Ubuntu 26.04 のコンパイラーに対応するため、同梱 DL Streamer のヘッダーに `<cstdint>` を追加した。
- README とインストールガイドの日英両方を更新した。

## MIGraphX

- `.deps/AMDMIGraphX` に公式 `rocm-10.1` タグ（コミット `95672916ed289d4be9d230a9be732e1bbad97e8c`）を取得し、`.deps/migraphx-build` でビルドした。
- GPU ターゲットは `GPU_TARGETS=gfx1151`、`GPU_BUILD_TARGETS=gfx1151`。MIOpen、rocBLAS、hipBLASLt は有効。
- rocMLIR が見つからなかったため `MIGRAPHX_ENABLE_MLIR=OFF`。Composable Kernel と Eigen も無効。
- ヘッダーとビルド済み共有ライブラリを `.deps/migraphx` に配置した。ビルド時に既存の `$HOME/AMDMIGraphX/deps` を依存ライブラリの検索先として利用している。この MIGraphX の準備はブートストラップに組み込まれておらず、別環境での再現手順は未整備。
- rocMLIR を有効にする場合は ROCm 10.1 と互換性のある rocMLIR を用意して MIGraphX を再ビルドし、実モデルで正確性と性能を比較する。

## 検証結果

- `cmake --build build -j 8`: 成功。
- `ctest --test-dir build --output-on-failure`: NPU を有効にする前の構成で29件すべて成功。`publication-contract` はローカルソケットを使用するため、制限されたサンドボックス内では失敗し、サンドボックス外で成功した。
- `build/amdva-probe`: 実機で `gate_passed: true`。VA・HIP・VPE と GPU メモリ共有の基本検査に成功。結果の `pipeline_zero_copy_verified` は `false` で、パイプライン全体のゼロコピーは未検証。
- MIGraphX のテスト用 `thresholdedrelu_default_test.onnx` を、`amdva-compile-model` で gfx1151 向けにコンパイルできた。推論の実行や出力値の検証は行っていない。成果物は `/tmp/amdva-rocm101-smoke.mxr`（一時ファイル）。
- ローカルの `$HOME/LLaVA/yolo11m.onnx`（FP32、入力 1×3×640×640）を gfx1151 向けにコンパイルし、`amdva-validate-model` で H.264 動画の 1 フレームを VA デコード、GPU 前処理、推論、後処理、オーバーレイまで実行した。前処理済みの同じテンソルを ONNX Runtime 1.25.1 の CPU に入力し、生出力 1×84×8400 を比較したところ、全要素が `rtol=1e-3, atol=1e-3` で一致した。最大絶対誤差は 0.002686、平均絶対誤差は 0.00000169、スコアの最大絶対誤差は 0.00000355。信頼度 0.25 超の候補は双方 75 個。これは 1 フレームの数値比較であり、精度や性能の評価ではない。検証時のモデル、入力、出力は `/tmp/amdva-yolo11m-*` にある一時ファイル。
- 同じモデルと 512×384 H.264 動画 20 フレームを、VA デコード → 前処理 → 推論 → DL Streamer 追跡・メタデータ出力 → `amdwatermark` → fakesink で実行した。一時的に変更した `amdva-run` で描画直前の元のメモリと PTS の一致を確認し、2 フレーム目からコピー監査を開始した。入力・出力は各 20 フレーム、検出は延べ 80 件、メモリ／PTS 違反とコピー監査違反はともに 0 件。監査対象 API の範囲で禁止されたホスト転送や画像マッピングは観測されなかった。描画後は別の GPU 出力バッファーになるため、元のメモリの同一性検査は描画直前まで。ドライバー内部の全コピー、実カメラ、長時間動作は対象外。
- `amdva-rtsp-files` と `artifacts/video-demo/rtsp-files.txt` で16動画を配信し、`artifacts/video-demo/nvr-rtsp-videos.json` から NVR に入力した。YOLO11m FP32、バッチ1、各入力の AI 1 FPS の40秒間のヘッドレス検証で、16入力すべてのデコードと推論の進行を確認し、GUI の16分割表示も確認した。長時間性能は未検証。NVR の直接ファイル入力の試作は破棄し、既存の RTSP 入力を使用する。
- MobileCLIP2-S2 の NPU バンドルを `artifacts/mobileclip-npu` に作成し、Ryzen AI 1.9／ORT 1.30 の Vitis AI EP で実行した。動画1フレームの NPU 出力と CPU FP32 参照のコサイン類似度は 0.9736。ネイティブ相互運用テストの再利用判定を、直前のアドレスだけでなく過去に使ったアドレスとの一致に修正した。100回の NPU 推論で2バッファーを98回再利用し、入力コピー・監査違反は0件、入力ポインターと前処理の検証が合格した。EP 内部のコピーは監査対象外で、旧ライブラリとの出力比較は行っていない。
- 固定ハッシュを確認した OSNet x1.0 MSMT17 を gfx1151 向け FP16、バッチ1で `artifacts/reid-model` に用意した。`amdva-reid-validate` では VA フレームからの GPU ROI 前処理・ReID 推論を検証し、監査違反0件、バッチ再現性の最小コサイン類似度1.00で合格した。NPU と同時に16動画を短時間再生したところ、15入力から人物 ID 付き観測400件を保存した。これは機能検証であり、人物再識別の精度評価ではない。
- 既存の監査ランナーは、この環境では監査ライブラリ単独の `LD_PRELOAD` では VA の実関数を解決できず、デコード開始に失敗した。`libva.so.2` と `libamdhip64.so` を監査ライブラリの後に明示したところ実行できた。また既存ランナーの固定転送数判定は初期化時の転送を含むため不一致となった。今回の一時ランナーはウォームアップ後の違反数で判定しており、これらの再現手順・ランナーの恒久的な修正は未整備。
- `python3 scripts/bootstrap.py --check`: キャッシュされた 100 ファイルのチェックサムが一致。
- `python3 scripts/docs_site.py --check-translations`: 古い翻訳 0 件。

## 次の作業

1. コピー監査のライブラリ解決とウォームアップ後の判定を既存ランナーで再現可能にする。
2. 実カメラや複数フレームで検出結果と性能を検証する。
3. MIGraphX と必要に応じて rocMLIR の準備手順を、このホスト固有の依存先に頼らず再現できるようにする。
