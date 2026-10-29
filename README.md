# redntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/redntfy)](https://github.com/aviscaerulea/redntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/redntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml)

システムトレイから Redmine チケットを通知し、一覧に表示する軽量常駐アプリです。
実測での物理メモリ使用量は約 7MB です。
Google カレンダーの予定を同じ仕組みで通知する姉妹ツール [gcalntfy](https://github.com/aviscaerulea/gcalntfy) もあります。

## 機能

- チケットの通知：Redmine をポーリングし、新着や更新を Windows 通知と音声で知らせる
- システムトレイ：未処理チケット一覧の表示や設定をトレイアイコンから操作できる
  - 一覧の書式：行に表示する項目と並びを自由に変えられる
  - 並び替え：更新日、期日、作成日、優先度などの順に並べ替える
  - ピン留め：気になるチケットを一覧に固定する
  - 非表示：見なくて良いチケットを通知と未処理件数から除外する
  - ブラウザ表示：行をクリックするとチケットをブラウザで開く
  - 検索：入力した文字を含むチケットを色付けして示す
  - キー操作：キーボードだけでチケットを選んで開ける
  - ショートカットキー：どのアプリを使っていても、キー操作で一覧を開閉できる
  - チケット登録：Redmine を開かずに、一覧から自分担当のチケットを登録する
  - 行メニュー：右クリックで、チケットのコピー、優先度と期日の変更、チケットの終了ができる
- 対象の絞り込み：Redmine のカスタムクエリ（複数可）で通知と一覧対象のチケットを自由に選べる
- ゼロ設定起動：URL と API アクセスキーだけで自分と所属グループの担当チケットを対象にする

### システムトレイ

トレイアイコンにマウスを乗せるかクリックすると、未処理チケットの一覧が開きます。
未読のチケットがあると、トレイアイコンに赤いバッジが付きます。

一覧では、未読や期日のあるチケットを太字に、優先度の高いチケットを赤い背景にして目立たせます。
新しく起票されたチケットには ✨ を、他の人が更新したチケットには 🆙 を付けます。

各種設定は、トレイアイコンの右クリックで開くトレイメニューから操作できます。

## インストール

### 動作要件

- Windows 10/11
- Redmine の API アクセスキー

### 手順

#### リリースの ZIP から

[Releases](https://github.com/aviscaerulea/redntfy/releases/latest) から ZIP をダウンロードしてください。次に任意のフォルダへ展開します。最後に `redntfy.exe` を実行します。

#### Scoop から

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install redntfy
```

#### アンインストール

アンインストールしても、以下の 3 つが残ります。不要であれば手動で削除してください。

- データフォルダ `%LOCALAPPDATA%\redntfy`
- 設定のレジストリキー `HKEY_CURRENT_USER\SOFTWARE\redntfy`
- スタートメニューのショートカット `redntfy.lnk`

Windows へのスタートアップ登録は、削除の前にトレイメニューで解除してください。

## 使い方

1. Redmine の「個人設定」で API アクセスキーを取得する
2. `redntfy.exe` と同じフォルダに、接続情報を書いた `redntfy.local.toml` を置く

   ```toml
   [redmine]
   url     = "https://redmine.example.com"
   api_key = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
   ```

3. `redntfy.exe` を起動する

起動すると、自分が担当するチケットの通知を始めます。
通知の対象を絞り込む手順は、画像付きの [セットアップガイド](https://aviscaerulea.github.io/redntfy/) を参照してください。

## 設定

設定は `redntfy.exe` と同じフォルダの `redntfy.toml` に記載します。
`redntfy.local.toml` に書いたキーは、`redntfy.toml` の同じキーを上書きします。

接続先、通知の対象、ポーリングの頻度、一覧の見た目などを変えられます。
各項目の説明は `redntfy.toml` のコメントを参照してください。

## 制限事項

- 設定ファイルの変更を反映するには再起動が必要
- 通知の対象に使えるのは、プロジェクトに属さない（グローバルな）カスタムクエリのみ
- 接続設定に誤りがあると通知を止め、直し方をトレイアイコンに表示する

## ライセンス

アプリケーションアイコンには Redmine 公式ロゴを使用しています。
ロゴは Martin Herr 氏の著作物で、ライセンスは [CC BY-SA 2.5](https://creativecommons.org/licenses/by-sa/2.5/) です。
