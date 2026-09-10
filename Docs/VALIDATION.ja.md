# CinderLink 検証記録

2026-09-10 / Windows / UE 5.8 / Win64 Development Editor。
ソースは `git/CinderLink` で作成。Arrietty-row の実行コード・風景には変更を加えていません。

## 1.0.2: Python の初期 ON・処理中の追加送信

- UE 5.8 のパッケージビルドとリポジトリ監査に合格。
- UE 自動テスト 12 件に合格。従来の 10 件に加えて、以下を検証。
  - 実際の Slate パネルを自動接続なしで生成。Python は初期 ON、送信前の実行権限はなし。
    停止・新規スレッド・切断通知で選択を維持し、手動 OFF と読み取り専用への切り替えを尊重する。
    Stop はチェック状態を保っても、そのターンの Python 実行権限を解除する。
  - 追加送信の正しいターン ID の受付、別ターン ID・サーバー拒否への対応、入力保持を検証。
    追加送信が Python 権限を付与しないこと、最終応答で途中のユーザーメッセージが消えないことも確認。
- 追加送信の仕様は公式 App Server の `turn/steer` とローカル CLI が生成したスキーマで確認。
  テストの追加送信応答は模擬応答であり、実際のモデル処理中に追加送信する往復試験ではない。
- 未保存アセットのバックアップ拒否は従来どおり維持。今回の利用者提供画像の拒否理由は
  Python の許可不足ではなく、未保存アセットだった。Python 初期 ON で保存済みバックアップの要件は変わらない。
- 状態表示を追加。会話欄のスクロールは手動のまま。UE の同期 Python 実行中に UI が応答しない制約も維持。
- 利用者の UE 終了後、指定の Engine Marketplace フォルダーへ 1.0.2 をコピー。
  旧版の 32 ファイルをバックアップして照合し、新版の 31 ファイルを SHA-256 で照合。
  パッケージ内のソースと現在のチェックアウトも一致した。
- プロジェクト側にプラグインを置かない独立した検証用プロジェクトで、Engine 側の導入済み
  1.0.2 を読み込み、同じ 12 件の自動テストがすべて成功した。
- 1.0.2 の画面目視確認と実モデルへの追加送信の往復試験は未実施。

ローカル証跡（Git 管理外）:

- `BuildArtifacts/build-v1-0-2.log`
- `BuildArtifacts/test-v1-0-2.log`
- `BuildArtifacts/TestReport-20260910T055652-cea2d022528e446895afa4cc66c66c3e/index.json`
- `BuildArtifacts/install-v1-0-2.json`
- `BuildArtifacts/InstalledReport-1-0-2/index.json`
- `BuildArtifacts/test-installed-v1-0-2.log`

## 1.0.1: モデルと推論量の表示

- UE 5.8 のパッケージビルドとリポジトリ監査に合格。
- パッケージ版と Engine の指定先に導入した版で、それぞれ既存の UE 自動テスト 10 件が成功。
- 実際の Codex App Server の `thread/start` 応答で、モデル `gpt-6-astra`、推論量 `xhigh` を確認。
  設定ファイルから推測した値ではない。テストはモデルへのプロンプトを送らない。
- 表示は `thread/start` と、該当スレッドの `thread/settings/updated` を使用する。
  未取得の値は `not reported` とし、切断や新しいスレッドでは以前の表示情報を消す。
  `model/rerouted` はスレッド・ターンを照合し、元の設定と区別して表示する実装を確認。
  実際のサービス側のモデル切り替えは今回発生させていない。
- 指定の Engine Marketplace フォルダーへコピー。旧版の 32 ファイルをバックアップして照合し、
  新版の 31 ファイルを SHA-256 で照合。
- Windows の画面確認ツールは native pipe に接続できず、今回の表示の目視確認は未実施。
  UE Editor を起動し直し、Window → CinderLink の上部で確認できる。
- その後、利用者提供の 2026-09-10 14:34 の画像で `Model: gpt-6-astra | Reasoning: xhigh` の表示を確認。

ローカル証跡（Git 管理外）:

- `BuildArtifacts/build-v1-0-1.log`
- `BuildArtifacts/TestReport-20260910T050304-72c13a0f658b4d24beea9067adc44acc/index.json`
- `BuildArtifacts/InstalledReport-1-0-1/index.json`
- `BuildArtifacts/install-v1-0-1.json`

## 1.0.0: 実施した検証

- リポジトリ監査と UE プラグインのパッケージビルドに合格。
- UE 自動テスト 10 件に合格。
  - 実際の Codex App Server への接続、20 操作のスキーマ受理、MCP 無効化、
    通常コマンドのプロジェクト外読み取り拒否、子プロセスの環境変数除外。
  - 通常の読み取り・編集プロファイルと既存 UE 操作の許可。
  - Python 制作モードが無効な場合の拒否、引数・保存先検査、停止・完了・切断での権限解除。
  - UE Python 実行、出力取得、実行ごとの変数スコープ、ソース・結果の保存、
    再利用スクリプトの書き出しと上書き拒否。
  - 実行前ファイルのバックアップと実行後の作成・変更の記録。
  - 本物の UE マテリアルを作成・コンパイル・保存し、後続スクリプトで材質グラフを再編集。
    未保存のパッケージがバックアップ範囲にある場合は実行を拒否。
  - 意図的な Python 例外を失敗として返し、出力と例外の記録を保存。
- PowerShell 復元テストに合格。
  プレビューで変更しないこと、元ファイルの復元、新規ファイルの退避、復元直前の編集の保管、
  壊れたバックアップとディレクトリを遡るパスの拒否を確認。
- 実際のマテリアル編集の実行記録から `.uasset` を復元し、別の UE プロセスで読み直した。
  Base Color の接続先の色が変更前の `(0.12, 0.35, 0.08)` に戻ることを確認。
- 指定の `C:/Program Files/Epic Games/UE_5.8/Engine/Plugins/Marketplace/CinderLink` へコピー。
  旧 0.2.0 の 26 ファイルをバックアップして照合し、1.0.0 の 31 ファイルをコピー後に照合。
  プロジェクト側にプラグインを置かない独立した検証用プロジェクトで、Engine 側の導入済み
  プラグインを読み込み、同じ 10 件の自動テストがすべて成功した。

ローカル証跡は Git 管理外の `BuildArtifacts` にあります。

- `test-v1-0-0-verified.log`
- `TestReport-20260910T042201-51a1ed82faf44f1e97bd7ac25e45abab/index.json`
- `TestHost/Saved/CinderLink/Python/`: 実際のスクリプト・実行結果・バックアップ
- `recovered-material-ue.log`: 復元後の UE 読み込み確認
- `RecoveryTest-*/`: PowerShell 復元テストのファイルと退避結果
- `install-v1-0-0.json`: コピー先・旧版バックアップ先・照合数
- `InstalledReport-1-0-0/index.json`: 指定先に導入したプラグインのテスト結果

## 限界

自動テストは新規の検証用 UE プロジェクトで実施しました。App Server 接続テストは
モデルへプロンプトを送らず、プロトコルと通常コマンドの隔離を確認するものです。
「モデルが自然文の依頼から画像を見て風景を完成させる」全工程の実証ではありません。
テストは NullRHI を使うため、画面の見栄えや HMD での評価は含みません。

任意 Python の完全な隔離、強制中断、全副作用の巻き戻しは提供しません。
バックアップは申告されたフォルダーの保存済みファイルだけです。

初回・2 回目の検証では、意図して発生させた例外の Traceback をテストが予期済みの
ログとして認識できず、ExceptionEvidence が失敗しました。例外の返却と保存は動作しており、
テストの文字列照合を修正後、10 件すべてが成功しています。
