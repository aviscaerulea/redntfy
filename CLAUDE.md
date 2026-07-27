# CLAUDE.md

## 開発環境

- Windows 11
- Visual Studio Build Tools（`cl.exe`、`rc.exe` を vswhere で自動検出）
- vcpkg：`libebur128:x64-windows-static`（build.ps1 が未インストール時に自動導入）
- go-task（Taskfile ランナー）
- PowerShell 7（pwsh）
- git（バージョン番号を `git describe --tags` から生成）

## ビルド方法

```powershell
task build      # 通常ビルド（out/redntfy.exe。アセットも out/ にコピー）
task rebuild    # クリーンビルドして再起動
task release    # リリースビルド（最適化＋zip 作成）
task clean      # 成果物削除（実行中プロセスは強制終了）
```

build.ps1 は `Microsoft.VisualStudio.DevShell.dll` + `Enter-VsDevShell` で VC++ 環境をロードし、rc → `out/version.h`（`APP_VERSION`）生成 → cl の順に実行する。

## テスト方法

自動テストは無い。`out/redntfy.exe` を起動して動作確認する。

- `[redmine]` の url / api_key / query_ids がどちらの toml にも無い場合、設定エラー Toast を出して終了コード 1 で終了する（接続情報なしでの起動確認に使える）
- ログは `out/logs/YYYY-MM-DD.log` に出力される

## 実装上の注意点

- 実装は `src/main.cpp` の単一翻訳単位（姉妹プロジェクト gcalntfy の構造を踏襲）

- 設定は `redntfy.toml` を読み、`redntfy.local.toml` が同名キーをキー単位で上書きする  
  ホットリロードはせず、変更反映には再起動が必要。

- スレッド構成：メイン（メッセージループ・トレイ UI）／`pollThreadFunc`（HTTP・Toast・音・状態保存）／`soundThread`（WASAPI 再生）／`checkForUpdates`（起動時 1 回、detach）

- 共有状態は `g_mtx`（`g_issues`・`g_pins`・`g_unreadIds`・`g_latestVersion`）と atomic（`g_unreadCount`・`g_myUserId`・`g_assignedToMeOnly` など）で保護する  
  `g_currentConfig` は起動時に 1 回設定した後は不変で、ロック無しで読み取る。

- 永続化は `state.json`（検知済み）と `pins.json`（ピン留め）で、書き出しは `atomicWriteJson`（tmp 経由の置換）  
  `state.json` は v2（チケット id → updated_on ＋所属クエリ、ルートに追跡クエリ id）。  
  旧形式と `query_ids` への追加分は流入検知を見送り、現在の所属を黙って採用する。（通知の嵐の防止）

- `fetchIssues` は 1 クエリでも失敗すると全体を失敗にして部分結果を破棄する（誤「新規」通知の防止）

- グループ担当（👥）の判定は初回ポーリング時（user id 取得後）に `/groups.json` を試し、403 なら自分の所属グループへ縮退する  
  admin 権限が無いと 403 になる。縮退時は他グループ宛の判定が漏れるが誤判定はしない。
  403 以外の失敗は次回ポーリングで再試行する。

- schedule の 0（休止時間帯）は force poll・stale 判定より優先される  
  この順序を崩すと深夜に通知が鳴る。起動直後の 1 回だけは休止時間帯でも実行する。
  トレイメニューの「今すぐ更新」（`g_manualPoll`）だけは明示操作として休止時間帯・クールダウンを無視する。

- Toast には AUMID 付きスタートメニューショートカットが必須（`ensureShortcut` が自動作成）

## References

- README.md
