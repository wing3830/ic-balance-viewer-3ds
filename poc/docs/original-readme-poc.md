# IC Balance PoC — New 3DS で交通系ICの残高を見る（読み取り専用）

New 3DS LL（日本モデル）内蔵の FeliCa リーダーで、Suica / PASMO / ICOCA 等の
交通系ICカードの **残高と利用履歴を読むだけ** の実験アプリ（PoC）。

## 決済機能は入ってないし、要らない理由

- 決済は「相互認証つき書き込み＋決済サーバとの精算」が必要で全くの別物。
- 残高（サービスコード `008B`）と履歴（`090F`）は、駅の残高確認機や
  Android の残高確認アプリと同じ **認証不要の読み取り専用領域** にある。
  読むのに鍵は要らない（FeliCa の Read Without Encryption コマンド）。
- このアプリはカードへの書き込みコマンドを一切送らない。

## 仕組みと、前例

- eショップ純正決済は `mint` アプレット専用の `nfc:p` を使うので真似できない。
- ただし通常タイトル『めがみめぐり』が `nfc:u` の RawNFC モード
  （`nfcInit(NFC_OpType_RawNFC)` → `nfcStartOtherTagScanning()` →
  `nfcSendTagCommand()`）で同じ交通系カードを読んでいた実績がある。
  この PoC はその経路を使う。

## この PoC が実機で確かめること（未確定ポイント）

1. `nfcStartOtherTagScanning` の第2引数ビットマスクのうち、どれが FeliCa 用か
   （文書化されていない。amiibo 用は `0x01` という実績のみ）。
   → **0x00〜0x0F の候補を順に全部試して、反応したマスクを記録する。**
2. `nfcGetTagInfo()` で FeliCa の IDm が取れるか、どこに入っているか
   （生構造体ごとダンプする。ついでに未文書化の GetTagInfo2 も生 IPC で叩く）
3. `nfcSendTagCommand()` に渡す FeliCa フレームは「長さバイト付き」か「なし」か
   → **両方試して応答を全部記録する。** Polling コマンドも試す。
4. 読めた場合：残高ブロックのオフセット候補を2通り（10-11 / 11-12、LE）表示する。
   実際の残高と一致する方が正解。

## 使い方

### 3DSX（推奨・まずこっち）
1. `ic-balance-poc.3dsx` を SD の `/3ds/` フォルダに入れる
2. Homebrew Launcher から起動

### CIA
1. `ic-balance-poc.cia` を FBI でインストール（バナーなしの最小構成。
   アイコンとタイトルのみ表示される。TitleID: 00040000001CBC000）

### テスト手順
1. アプリを起動する
2. **交通系ICカードを下画面に載せて、そのまま動かさない**
   （スキャンは最大で約1分＋読み取り時間かかる。途中で離すと失敗する）
3. Aボタンで開始
4. 上画面の最後に `RESULT:` が出る：
   - `BALANCE READ OK` → 成功。表示された残高候補のどちらが正しいか確認してほしい
   - `tag was detected, but ... did not parse` → 検出はできた。あと一歩
   - `no tag detected under any mask` → どのマスクでも検出できなかった
5. 終了は START

### ログ
- 実行内容は全部 `sdmc:/icpoc_log.txt` に追記される。
- 結果がどうあれ、このログを Muse に渡せば次の一手が決まる
  （上画面の写真でもいいけど、ログの方が確実）。
- ログにはカードの IDm（カード固有ID）が含まれる。自分のカードのものだけど、
  公開場所に貼るときはその点だけ注意。

## 注意・既知の制限
- 対象は実物の交通系ICカード。おサイフケータイ等のモバイルFeliCaは
  応答が違う可能性あり（未検証）。
- PiTaPa はポストペイ型で「残高」の概念がないので対象外。
- New 3DS 系専用（Old 3DS の外付けリーダーは未対応）。システム 10.0.0 以降が必要
  （RawNFC の要件）。

## ファイル
- `ic-balance-poc.3dsx` — SHA-256: 9003fd88ef30fcbeb181f8d09cefda5a01c5f9a96aed3e90945ee1e53067812b
- `ic-balance-poc.cia` — SHA-256: e55893c85991c687de520d314182f316c1411d8e663d5c13381efc3216279414
- `source/main.c` — ソース全部（コメントに設計判断を記載）
- ビルド: devkitPro (libctru) + makerom、検証は「ビルド成功・CIA構造パース」まで。
  **実機未検証**（それを確かめるのがこの PoC の目的）。
