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
task test       # 単体テストをビルドして実行（out/redntfy_test.exe）
task rebuild    # クリーンビルドして再起動
task release    # リリースビルド（最適化＋zip 作成）
task clean      # 成果物削除（実行中プロセスは強制終了）
```

build.ps1 は `Microsoft.VisualStudio.DevShell.dll` + `Enter-VsDevShell` で VC++ 環境をロードし、`out/version.h`（`APP_VERSION`）生成 →（`-Test` ならテスト exe の cl で終了）→ rc → cl の順に実行する。

## テスト方法

単体テストは `task test` で実行する。実体は tests/test_main.cpp で、src/main.cpp を丸ごと
include して static 関数を直接検査する自前ミニハーネスだ。対象は純粋ロジックのみで、
HTTP・UI・音声は含まない。アプリ全体の動作確認は `out/redntfy.exe` を起動して行う。

- `[redmine]` の url / api_key がどちらの toml にも無い場合、終了せず無効モードで常駐する（接続情報なしでの起動確認に使える）
  - 無効モード：トレイが `app-disable.ico`、tooltip が原因別の案内文言、「今すぐ更新」非活性、左クリック一覧なし、ポーリングなし。復帰は再起動のみ
  - 静的 2 分岐とも `redntfy.local.toml` 不在ならテンプレートを自動生成して設定ファイルを開き、セットアップガイド（GitHub Pages）をブラウザで開く
  - url 空・非 http(s)：Toast「無効な Redmine URL」（ブラウザはガイドのみ）
  - api_key 空：Toast「認証エラー／Redmine の API アクセスキーを api_key に設定してください」＋ `<url>/my/account` もブラウザで開く
- query_ids 空はフォールバックモードとして正常動作する（ゼロ設定起動：url と api_key だけで使える）
  - 取得は `/issues.json?assigned_to_id=me`（me は Redmine がサーバ側で自分＋所属グループに展開するため、グループ宛も含まれる）
  - state.json 上は擬似クエリ id 0（`FALLBACK_QUERY_ID`）で表現し、流入検知・黙って採用の既存ロジックにそのまま乗る（query_ids の設定・解除の切替も無通知で移行する）
  - 複数件 Toast・一覧フッタの遷移先は `<url>/issues?set_filter=1&assigned_to_id=me`（Web 側も同じ展開のため表示集合と一致する）
- ポーリング中の HTTP 401（api_key 無効）と、query_ids 設定時の 404（query_ids 無効）は確定的な設定不備として上記の無効モードへ遷移する（Toast・設定ファイル・ブラウザ誘導は 404 なら `<url>/issues` を開く）。フォールバックモードの 404 は通常の接続エラー扱い。それ以外の接続エラーは従来どおり 60 秒リトライ＋30 分クールダウンの Toast
- ログは `out/logs/YYYY-MM-DD.log` に出力される

## 実装上の注意点

- 実装は `src/main.cpp` の単一翻訳単位（姉妹プロジェクト gcalntfy の構造を踏襲）

- 設定は `redntfy.toml` を読み、`redntfy.local.toml` が同名キーをキー単位で上書きする  
  ホットリロードはせず、変更反映には再起動が必要。

- スレッド構成：メイン（メッセージループ・トレイ UI）／`pollThreadFunc`（HTTP・Toast・音・状態保存）／`soundThread`（WASAPI 再生）／`checkForUpdates`（起動時 1 回、detach）

- 共有状態は `g_mtx`（`g_issues`・`g_pins`・`g_unreadIds`・`g_latestVersion`）と atomic（`g_myUserId`・`g_assignedToMeOnly` など）で保護する  
  `g_currentConfig` は起動時に 1 回設定した後は不変で、ロック無しで読み取る。

- 永続化は `state.json`（検知済み）と `pins.json`（ピン留め）で、書き出しは `atomicWriteJson`（tmp 経由の置換）  
  `state.json` は v2（チケット id → updated_on ＋所属クエリ＋最終更新者、ルートに追跡クエリ id と前回ポーリング時刻）。  
  旧形式と `query_ids` への追加分は流入検知を見送り、現在の所属を黙って採用する。（通知の嵐の防止）

- `fetchIssues` は 1 クエリでも失敗すると全体を失敗にして部分結果を破棄する（誤「新規」通知の防止）

- グループ担当（👥）の判定は初回ポーリング時（user id 取得後）に `/groups.json` を試し、403 なら自分の所属グループへ縮退する  
  admin 権限が無いと 403 になる。縮退時は他グループ宛の判定が漏れるが誤判定はしない。
  403 以外の失敗は次回ポーリングで再試行する。

- 「バージョン未指定の除外」はバージョン欄が無いチケット（トラッカーで欄が無効、またはプロジェクトにバージョン未定義）を除外しない  
  判定材料は `/trackers.json`（Redmine 5.0 未満は全トラッカー有効に縮退）と  
  `/projects/:id/versions.json`（プロジェクト単位のキャッシュ）。  
  起動時・「今すぐ更新」・`version_meta_refresh_hours`（デフォルト 24 時間）超過で再取得する。

- 一覧の行は `list_format`（[app]）のプレースホルダテンプレートで組み立てる  
  語彙は {id} {lastname} {firstname} {group} {project} {due} {bug} {subject} {ago} の 9 要素で、
  `{要素:N}` で最大文字数を指定できる。（「…」は件名のみ）
  解釈できないトークンはリテラルのまま行に表示して起動ログに残す。（アプリは止めない）
  空に展開された要素の直後のリテラル先頭空白は、出力末尾が空白か行頭なら取り除く。
  `subject_max_chars`・`project_max_chars` は廃止。（読み捨て＋廃止ログ）

- schedule の 0（休止時間帯）は force poll・stale 判定より優先される  
  この順序を崩すと深夜に通知が鳴る。起動直後の 1 回だけは休止時間帯でも実行する。
  トレイメニューの「今すぐ更新」（`g_manualPoll`）だけは明示操作として休止時間帯・クールダウンを無視する。

- 「今すぐ更新」は完了時に必ず応答を返す  
  成功時は `deliverPollResults` の戻り値（通知件数）が 0 のときだけ完了 Toast（`showPollDoneToast`）を出す。
  更新を検知した回は通知 Toast が完了の合図になるため重ねない。
  取得失敗時は `showErrorToast` の force でクールダウンを無視して接続エラーを出す。
  （force でもクールダウンの起点は更新する。据え置くと 60 秒後の自動リトライで同じ Toast が 2 通出る）

- 未読（一覧の太字）の既読化は一覧の行クリックのみ  
  一覧を開いただけでは既読にしない。（開いた≠読んだ。読み落としを防ぐのが未読表示の目的）
  tooltip の未読件数とバッジは `buildListRows` の結果、すなわち一覧に出る行から数える。
  一覧の選定を `showIssuePopup` と共有することで、件数と画面上の太字行数を一致させる。
  `list_limit` の窓外に落ちた未読は数にもバッジにも出ない。
  （クリックできない行でバッジが消せなくなるのを防ぐため、表示範囲を件数の基準に揃えた）
  Toast の「チケットを開く」は OS がブラウザを直接起動するため既読化されない。

- Toast には AUMID 付きスタートメニューショートカットが必須（`ensureShortcut` が自動作成）

## References

- README.md
