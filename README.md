# redntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/redntfy)](https://github.com/aviscaerulea/redntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/redntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml)

タスクトレイから Redmine チケットの通知と一覧表示を行う軽量常駐アプリです。

主な機能は、チケットの新着や更新を知らせる通知と、トレイアイコンから開く未処理チケット一覧の表示です。
通知と一覧の対象は、Redmine のカスタムクエリで自由に絞り込めます。

実測での物理メモリ使用量は約 7MB です。

Google カレンダーの予定を同じ仕組みで通知する姉妹ツール [gcalntfy](https://github.com/aviscaerulea/gcalntfy) もあります。

## 機能

- チケットの通知：Redmine をポーリングし、新着や更新を Windows 通知と音声で知らせる
- システムトレイ：未処理チケット一覧の表示や設定をトレイアイコンから操作できる
  - 一覧の書式：行の並びや表示項目をプレースホルダで指定できる
  - ピン留め：気になるチケットを一覧に固定する（クローズしても消えない）
  - 非表示：見なくて良いチケットを通知と未処理件数から除外する
  - ブラウザ表示：行やフッタをクリックするとブラウザで開く
- 対象の絞り込み：Redmine のカスタムクエリ（複数可）で通知と一覧対象のチケットを自由に選べる
- ゼロ設定起動：URL と API アクセスキーだけで自分と所属グループの担当チケットを対象にする

### システムトレイ

トレイアイコンは、未読のチケットがあると右下へ赤いバッジを表示します。

未処理チケット一覧の各行には、期日、担当、プロジェクト、最終更新からの経過時間をアイコン付きで表示します。未読のチケットは太字で強調し、未処理の件数はフッタに表示します。行のクリックでブラウザが開き、そのチケットを既読にします。行の右クリックでは、ピン留め、非表示、通常の 3 状態を順に切り替えます。

各種設定は、トレイアイコンの右クリックで開くトレイメニューから操作できます。

## インストール

### 動作要件

- Windows 10/11
- Redmine の API アクセスキー

### 手順

Scoop での導入手順です。

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install redntfy
```

zip から手動で導入するときは、[Releases](https://github.com/aviscaerulea/redntfy/releases/latest) から zip をダウンロードしてください。次に任意のフォルダへ展開します。最後に `redntfy.exe` を実行します。

## 使い方

初回セットアップの手順です。  
画像付きの詳しい手順は [セットアップガイド](https://aviscaerulea.github.io/redntfy/) を参照してください。

1. Redmine の「個人設定」→「API アクセスキー」でキーを取得する
2. `redntfy.exe` と同じフォルダに、接続情報を書いた `redntfy.local.toml` を作成する

   ```toml
   [redmine]
   url     = "https://redmine.example.com"
   api_key = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
   ```

3. `redntfy.exe` を起動する

これだけで、自分（と所属グループ）が担当する未完了チケットの更新を通知し始めます。
対象を自分で絞り込みたいときは、Redmine でグローバルなカスタムクエリを保存します。  
保存したクエリの id を `query_ids` に設定します。  
手順の詳細はセットアップガイドを参照してください。

## 設定

動作設定は `redntfy.exe` と同じフォルダの `redntfy.toml` に記載します。
`redntfy.local.toml` を置くと、同名のキーをキー単位で上書きします。
接続情報の分離や環境ごとの差分管理に使えます。

設定できるのは、接続情報、対象にするカスタムクエリ、時間帯ごとのポーリング回数、一覧の表示件数と行の書式、通知音のラウドネス正規化、起動時の更新チェックです。
各キーの意味と既定値は `redntfy.toml` のコメントを参照してください。

### `query_ids` の記載例

`query_ids` は省略できます。  
省略すると、自分（と所属グループ）が担当する未完了チケットを対象にします。

```toml
[redmine]
url     = "https://redmine.example.com"
api_key = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

# プロジェクトを指定せずに保存したグローバル保存クエリの id を配列で指定する。
# 複数指定時は全クエリの和集合を追跡する。
# 先頭の id は複数件通知・一覧フッタから開くクエリ画面に使う。
query_ids = [12, 34]
```

まずは設定なしで使い始めてください。
対象を絞り込みたくなったら、カスタムクエリを作成して `query_ids` を設定します。
設定の切り替え時に通知があふれることはありません。

## 制限事項

- `query_ids` に指定できるのはグローバルなカスタムクエリのみ（プロジェクト配下のクエリは参照不可）
- Redmine の権限が足りないと、グループ担当の印は自分の所属グループの分だけ付く
- 設定ファイルはホットリロード非対応で、変更を反映するには再起動が必要
- 接続設定に不備があるときは通知を止めて常駐する（直し方をトレイアイコンに表示する）

## ライセンス

アプリケーションアイコンには Redmine 公式ロゴを使用しています。
ロゴは Martin Herr 氏の著作物で、[CC BY-SA 2.5](https://creativecommons.org/licenses/by-sa/2.5/) でライセンスされています。
