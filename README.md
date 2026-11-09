# IC Balance Viewer for New 3DS（交通系IC残高ビューア）

New Nintendo 3DS LL（日本モデル）内蔵のFeliCaリーダーで、交通系ICカードの残高と利用履歴を表示する自作ソフト（homebrew）。**決済機能は一切なく、読み取り専用。**

## 何をする物か
- カードをかざすと残高（円）と履歴（最大20件：日付／種別／駅コード／増減額／取引後残高）を表示する
- カードを離すと次のスキャン待ちに戻り、別のカードも連続で読める

## 使い方
1. `ic-balance.3dsx` をSDカードの `/3ds/` に入れてHomebrew Launcherから起動する
   （CIA派は `ic-balance.cia` をFBIでインストール。TitleID: 00040000001CBD000、バナーなし）
2. 起動後、カードを下画面に置くと自動で読んで表示する
3. STARTで終了

詳しい技術仕様と読み取りパラメータは `docs/original-readme-v1.md`、開発経緯のPoC版は `poc/` を参照。

## 技術
- C言語、devkitPro（libctru）＋ makerom でビルド
- `nfc:u` の RawNFC（type3）でカードを読み、FeliCa の Read Without Encryption のみを使う（書き込みコマンドは送らない）
- 残高＝サービス `008B`、履歴＝サービス `090F` を読む

## 状態・確認済み事項
- v1完成版。2026-11-09に実機（New 3DS LL）で動作確認済み
  - PoCが初回試行で読み取りに成功し、表示された残高が実際のカード残高と一致することをユーザーが確認した
  - v1も実機で「反応が速く実用的」との確認あり
- 先行事例調査（Web／GitHub／GBAtemp／GameBrew、日英）では、3DS自作ソフトで交通系ICの残高を読む事例は見つからなかった（公式ソフトの事例のみ）。「初事例」と断定はしない

## 未確認・注意点
- 駅コードから駅名への変換は未搭載（駅コードDBが必要。今後の課題）
- 対象は実物の交通系ICカードとNew 3DS系。モバイルFeliCaやOld 3DS外付けリーダーは未検証
- PiTaPaはポストペイ型で残高の概念がないため対象外

## ファイルと公開時の扱い
- `source/main.c`、`Makefile`、`icbalance.rsf` — v1ソース一式
- `ic-balance.3dsx`（約143KB）／`ic-balance.cia`（約130KB）／`ic-balance.smdh`（約14KB）— ビルド成果物。小さいためこの準備フォルダには入れたが、**GitHubではリポジトリ本体ではなく Release に添付する候補**（最終判断は CHECKLIST 参照）
- 除外したもの：`ic-balance.elf`（約880KB、デバッグ用中間成果物）、`build/` フォルダ（中間ファイル）
- `poc/` — 実験版PoCのソースのみ（バイナリは除外）。PoCのログにはカード情報が含まれるため絶対に公開しないこと（CHECKLIST参照）
