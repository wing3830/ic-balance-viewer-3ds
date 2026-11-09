# IC Balance Viewer v1 — New 3DS で交通系ICの残高・履歴を見る

PoC（`../ic-balance-poc/`）で実機確認できたパラメータを固定した完成版。
総当たりやダンプは全部外して、普通の残高確認アプリとして使える形にした。

## 使い方
1. `ic-balance.3dsx` を SD の `/3ds/` に入れて Homebrew Launcher から起動
   （CIA派は `ic-balance.cia` を FBI で。TitleID: 00040000001CBD000、バナーなし）
2. 起動したらカードを下画面に置く → 自動で読んで表示
3. カードを離すと次のスキャン待ちに戻る（別のカードも連続で読める）
4. START で終了

## 表示内容
- 残高（円）
- 履歴 最大20件（新しい順）：日付 / 種別（TRAIN・CHARGE・BUS・SHOP等）/
  駅コード（入場>出場、例 `0D53>0D4E`）/ その取引の増減額 / 取引後残高
- 駅コードの「駅名」変換は未搭載（駅コードDBが必要なので今後の課題）

## 実機で確定した読み取りパラメータ（PoCログより）
- `nfcInit(NFC_OpType_RawNFC)` → `nfcStartOtherTagScanning(0, 0x04)`
  （マスク 0x04 が FeliCa）
- IDm は `nfcGetTagInfo()` の `id[0..7]`（id_offset_size = 8）
- `nfcSendTagCommand()` のフレームは **先頭に FeliCa 長さバイト付き**
- 残高 = サービス `008B` ブロック0、バイト 11-12 の LE u16
  （検証：最新履歴の残高と一致。別候補の 10-11 は 64,000円となり
  SF上限 20,000円を超えるため誤りと確定）
- 履歴 = サービス `090F`（20ブロック）。残高はバイト 10-11 LE、
  日付はバイト 4-5 にパック
- 決済機能なし・カードへの書き込みなし（Read Without Encryption のみ）

## ファイル
- `ic-balance.3dsx` — SHA-256: 2935a8cf09f4e07378af2b75de0bdd0fed44fc9fa18b522f9636367c61d85935
- `ic-balance.cia` — SHA-256: db527ea865a34ac936742a37e22cfb1bb3ef52159a3c706fb730602b4e73486d
- 検証状況：2026-11-09 実機（New 3DS LL）で動作確認済み。反応が速く実用的との報告あり
