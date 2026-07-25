# rdmntfy

Redmine の更新チケットを Windows Toast 通知で知らせ、未処理チケットをシステムトレイから一覧できる軽量常駐アプリ。

## 機能

- Redmine のグローバル保存クエリを `schedule` 設定に従って自動ポーリング
- 追跡集合への新規流入と既知チケットの更新を Toast 通知と通知音で通知
  - 自分が起票したチケットの流入と、自分の操作による更新は通知しない
  - 1 件のときはチケット番号と件名を表示し、「チケットを開く」でブラウザ表示
  - 複数件のときは件数のみのサマリ 1 通とし、「チケットを開く」で保存クエリ画面を表示
- トレイ左クリックで未処理チケットの一覧を表示
  - 更新日時降順で `list_limit`（デフォルト 20）件を表示
  - 行の左クリックでチケットをブラウザ表示、右クリックでピン留めをトグル（最大 5 件）
  - ピン留めしたチケットはクローズや担当変更で保存クエリから外れても一覧に残る（クローズ済は打ち消し線）
- 未処理件数と未読件数を tooltip に表示し、未読があればトレイアイコンに赤バッジを点灯
- 通知音の EBU R128 ラウドネス正規化、19kHz ガードトーン、指定プロセスのダッキング
- マイク・カメラ使用中（会議中）の通知音自動ミュート
- 起動時の GitHub リリース更新チェック

## 動作要件

- Windows 10/11
- Redmine の API アクセスキーとグローバル保存クエリ（作成手順は「使用方法」を参照）

## インストール方法

任意フォルダで zip を展開して rdmntfy.exe を実行。

## 使用方法

1. Redmine で **プロジェクトを指定せず** にチケット一覧を絞り込み、カスタムクエリとして保存する
   - 特定プロジェクト配下で保存したクエリは API から参照できないため、必ずグローバルクエリとして作成する
   - 保存後の URL `/issues?query_id=N` の `N` を控える
2. Redmine の「個人設定」→「API アクセスキー」でキーを取得する
3. exe 同フォルダに `rdmntfy.local.toml` を作成して接続情報を記載する

   ```toml
   [redmine]
   url      = "https://redmine.example.com"
   api_key  = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
   query_id = 12
   ```

4. rdmntfy.exe を起動する

動作設定は exe 同フォルダの `rdmntfy.toml` を参照。
`rdmntfy.local.toml` を置くと同名キーをキー単位で上書きできる。（接続情報はこちらに書く）
検知済み状態は `state.json`、ピン留めは `pins.json` に保存され、再起動しても保持される。

## ビルド方法

Visual Studio Build Tools、vcpkg、go-task、PowerShell 7、git が必要。

```powershell
task build      # 通常ビルド（out/rdmntfy.exe）
task release    # リリースビルドと zip 作成
```

## 技術仕様

- C++20 / Win32 API（単一翻訳単位）
- WinHTTP（Redmine REST API）
- C++/WinRT（Windows.UI.Notifications, Windows.Data.Json）
- WASAPI（通知音再生）
- libebur128（ラウドネス測定、vcpkg：`libebur128:x64-windows-static`）
- toml++（設定ファイル）
