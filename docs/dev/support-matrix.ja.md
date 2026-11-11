<!-- source: docs/dev/support-matrix.md 05d6eb72230e2940f87d7347cdc1582ff59f2045 -->
# ホストSDKの検証状況

対応目標はROCm 10.1.0／MIGraphX 2.18.0とホストのMesa／libva／libdrmです。新規構築と実機受入が通るまで検証済みとは表示しません。

| OS | GPU | 新規ビルド・契約テスト | GPU probe | 長時間デモ |
|---|---|---|---|---|
| Ubuntu 24.04 | Strix Point gfx1150 | 検証機待ち | 未検証 | 未検証 |
| Ubuntu 24.04 | Strix Halo gfx1151 | 検証機待ち | 未検証 | 未検証 |
| Ubuntu 26.04 | Strix Point gfx1150 | 検証機待ち | 未検証 | 未検証 |
| Ubuntu 26.04.1 | Strix Halo gfx1151 | x86-084-maple、kernel 7.0.0-43で成功 | VA／VPE／HIPの共有を確認 | 20分成功（ユーザー承認で短縮） |

過去のUbuntu 24.04／gfx1150の結果はROCm 7.2／MIGraphX 2.15であり、今回の構成の検証には含めません。NPUには別途Ryzen AI SW 1.9／XRT 2.26.89と配布版ドライバーが必要です。AMDのRyzen AI Linux公式対象OSは24.04のままで、26.04の結果はframework側の証跡です。CPUへの代替は実機検証の成功としません。probeの成功だけではパイプライン全体のコピー監査の証明にはなりません。

今回の受入はユーザーのネットワーク切断予定に合わせ20分へ短縮し、1204.9秒のログを外部監視で判定して停止しました。コピー監査0件、RSS中央値1956→1976MiB、FD227で安定、検索・録画・全17入力・DB整合性を確認しました。VCN容量の補正はその後の短時間検証で確認し、長時間検証中のライブラリは置き換えていません。
