# redntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)

[![Release](https://img.shields.io/github/v/release/aviscaerulea/redntfy)](https://github.com/aviscaerulea/redntfy/releases/latest)
[![Build](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml)

Redmine の更新チケットを Windows Toast 通知で知らせ、未処理チケットをタスクトレイから一覧できる軽量常駐アプリです。

## 機能

- 通知対象を Redmine のカスタムクエリで自由に設定でき、定期チェックで新着や更新を Toast 通知
- タスクトレイから未処理チケット一覧を表示し、期日、担当、プロジェクトをアイコンで整形
- 気になるチケットはピン留めで一覧の先頭に固定（クローズしても残る）
- トレイメニューから即時更新、フィルタ、並び替えを操作
- 通知音の音量を自動で揃え、会議中（マイクやカメラの使用中）は自動でミュート
- 起動時に GitHub リリースの新版を確認
- 常駐時の物理メモリ使用量は約 7MB と軽量

## インストール

### 動作要件

- Windows 10/11
- Redmine の API アクセスキー
- Redmine のグローバル保存クエリ（プロジェクト指定なしで保存したもの）

### 手順

Scoop を使う場合。

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install redntfy
```

zip から手動で導入する場合、[Releases](https://github.com/aviscaerulea/redntfy/releases/latest) から zip をダウンロードし、任意のフォルダに展開して `redntfy.exe` を実行してください。

## 使い方

初回セットアップの手順です。

1. Redmine で **プロジェクトを指定せず** にチケット一覧を絞り込み、カスタムクエリとして保存する
   - 特定プロジェクト配下で保存したクエリは API から参照できないため、必ずグローバルクエリとして作成する
   - 保存後の URL `/issues?query_id=N` の `N` を控える（複数クエリを追跡する場合は各クエリ分）
2. Redmine の「個人設定」→「API アクセスキー」でキーを取得する
3. `redntfy.exe` と同じフォルダに `redntfy.local.toml` を作成し、接続情報を記載する

   ```toml
   [redmine]
   url       = "https://redmine.example.com"
   api_key   = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
   query_ids = [12, 34]
   ```

4. `redntfy.exe` を起動する

起動後の日常操作は以下のとおりです。

- トレイアイコン左クリックで未処理チケット一覧を表示
- 行の左クリックでブラウザ表示して既読化、右クリックでピン留めをトグル
- トレイアイコン右クリックのメニューから即時更新、フィルタ、並び替えを操作

## 設定

動作設定は `redntfy.exe` と同じフォルダの `redntfy.toml` に記載します。
`redntfy.local.toml` を置くとキー単位で上書きされ、接続情報の分離や環境毎の差分管理に使えます。
検知済み状態は `state.json`、ピン留めは `pins.json` に保存され、再起動後も保持されます。

主要な設定項目は以下のとおりです。詳細は `redntfy.toml` のコメントを参照してください。

| セクション | キー | 説明 |
| --- | --- | --- |
| `[app]` | `schedule` | 24 時間分のポーリング回数（回/時） |
| `[app]` | `list_limit` | 一覧の表示件数（デフォルト 20） |
| `[app]` | `bug_trackers` | 💥 を付けるトラッカー名のパターン |
| `[app]` | `duck_targets` | 通知音再生中にミュートするプロセス名 |
| `[redmine]` | `url`, `api_key`, `query_ids` | 接続情報（必須） |
| `[loudness]` | `enabled`, `target` | 通知音のラウドネス正規化 |
| `[update]` | `enabled` | 起動時の更新チェック |

## 制限事項

- 追跡クエリは Redmine のグローバル保存クエリのみ対応（プロジェクト配下のクエリは API から参照できない）
- グループ担当の判定は admin 権限があれば全グループが対象、無ければ自分の所属グループのみ
- 設定ファイルはホットリロード非対応で、変更反映には再起動が必要

## ビルド

Visual Studio Build Tools、vcpkg、go-task、PowerShell 7、git が必要です。

```powershell
task build      # 通常ビルド（out/redntfy.exe）
task release    # リリースビルドと zip 作成
```

## 技術スタック

- C++20 / Win32 API（単一翻訳単位）
- WinHTTP（Redmine REST API）
- C++/WinRT（Windows.UI.Notifications, Windows.Data.Json）
- WASAPI（通知音再生）
- libebur128（ラウドネス測定）
- toml++（設定ファイル）

## ライセンス

アプリケーションアイコンには Redmine 公式ロゴを使用しています。
ロゴは Martin Herr 氏の著作物で、[CC BY-SA 2.5](https://creativecommons.org/licenses/by-sa/2.5/) でライセンスされています。
