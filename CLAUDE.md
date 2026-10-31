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

build.ps1 は `Microsoft.VisualStudio.DevShell.dll` + `Enter-VsDevShell` で VC++ 環境をロードし、`version.h`（`APP_VERSION`）生成 →（`-Test` ならテスト exe の cl で終了）→ rc → cl の順に実行する。出力先は Taskfile の `OUT_DIR` を `-OutDir` で受ける。

## テスト方法

単体テストは `task test` で実行する。実体は tests/test_main.cpp で、src/main.cpp を丸ごと
include して static 関数を直接検査する自前ミニハーネスだ。対象は純粋ロジックのみで、
HTTP・UI・音声は含まない。アプリ全体の動作確認は `out/redntfy.exe` を起動して行う。

- 接続情報なしでも起動確認できる。`[redmine]` の url が不正、または api_key が空（キー不在を含む）なら無効モードで常駐する（挙動は「実装上の注意点」の無効モードの項）
- query_ids 空でも url と api_key だけで動作確認できる（フォールバックモード。同じく「実装上の注意点」を参照）
- ログは `%LOCALAPPDATA%\redntfy\logs\YYYY-MM-DD.log` へ出力する  
  `out/redntfy.exe` と Scoop 版は同じデータディレクトリを共有する。（置き場は「実装上の注意点」の永続化の項を参照）
  多重起動制御で先に動いていた方は終了するが、state.json・ピン・非表示・ログはそのまま引き継ぐ。
  Scoop 版の状態を持ち込みたくない動作確認では、起動前に `%LOCALAPPDATA%\redntfy` を退避する。

## 実装上の注意点

本節には、コードから導出できない設計判断とその理由、および落とし穴を書く。各機能の挙動の詳細は
仕様を兼ねる src/main.cpp の該当関数のコメントにある（.claude/rules/vibes.local.md）。

- 実装は `src/main.cpp` の単一翻訳単位（姉妹プロジェクト gcalntfy の構造を踏襲）

- 設定は `redntfy.toml` を読み、`redntfy.local.toml` が同名キーをキー単位で上書きする  
  ホットリロードはせず、変更反映には再起動が必要。
  採否はキーの有無だけで決める（`loadConfig` の `findNode`）。local にキーがあれば値の型が合わなくても base の値は使わず、ログを残して既定値（schedule は全時間帯で 1 時間に 1 回、他の配列は空）になる。（local の不正値で黙って base を拾わないため）

- `[redmine]` の url が不正、または api_key が空（キー不在を含む）の場合、終了せず無効モードで常駐する  
  無効モード：トレイが `app-disable.ico`、tooltip が原因別の案内文言、「今すぐ更新」非活性、一覧なし、ポーリングなし。復帰は再起動のみ。
  起動時の静的検証は url 不正と api_key 空の 2 分岐で、どちらも `redntfy.local.toml` 不在ならテンプレートを自動生成して設定ファイルを開き、セットアップガイド（GitHub Pages）をブラウザで開く。
  ポーリング中の HTTP 401（api_key 無効）と、query_ids 設定時の 404（query_ids 無効）は確定的な設定不備として無効モードへ遷移する（`enterDisabledMode`）。401 は `<url>/my/account`、404 は `<url>/issues` を開く。
  フォールバックモードの 404 は通常の接続エラー扱い。

- query_ids 空はフォールバックモードとして正常動作する（ゼロ設定起動：url と api_key だけで使える）  
  取得は `/issues.json?assigned_to_id=me`。（me は Redmine がサーバ側で自分＋所属グループに展開するため、グループ宛も含まれる）
  state.json 上は擬似クエリ id 0（`FALLBACK_QUERY_ID`）で表現し、流入検知・黙って採用の既存ロジックにそのまま乗る。（query_ids の設定・解除の切替も無通知で移行する）

- 設定不備（401/404）以外の接続エラーは 60 秒リトライ＋30 分クールダウンの Toast  
  例外として、スリープ復帰（`PBT_APMRESUMEAUTOMATIC`）を起点とする最初の取得が失敗したときだけは Toast を出さない（`g_resumePoll`）。回線の再確立前に失敗しやすいためで、60 秒後のリトライ以降の失敗は通常どおり出す。
  「今すぐ更新」が同じ取得を要求していたときは、失敗しても従来どおり Toast を出す。

- スレッド構成：メイン（メッセージループ・トレイ UI）／`pollThreadFunc`（HTTP・Toast・音・状態保存）／`soundThread`（WASAPI 再生）／`checkForUpdates`（起動時 1 回）／`registerIssueThreadFunc`・`issueUpdateThreadFunc`（送信 1 回ごとに起動）  
  ワーカースレッドはシャットダウン時に join する。COM はワーカースレッドが各自 MTA で、メインスレッドが STA で初期化する。（`ShellExecuteW` と `IShellLink` が STA を要求するため。Toast と JSON の WinRT 呼び出しは STA でも動く）
  終了処理は `wmain` の `teardown` に集約し、正常終了と例外終了で同じ順序を踏む。（順序の理由は同所のコメント）

- 共有状態は `g_mtx`（`g_issues`・`g_pins`・`g_unreadIds`・`g_hiddenIds`・`g_priorities`・`g_priorityPositions`・`g_latestVersion`・`g_registerProject`・`g_personalProjectName`）と atomic（`g_myUserId`・`g_assignedToMeOnly` など）で保護する  
  `g_currentConfig` は起動時に 1 回設定した後は不変で、ロックなしで読み取る。

- 永続化は `state.json`（検知済み）、`pins.json`（ピン留め）、`hidden.json`（非表示チケットの id 配列）で、書き出しは `atomicWriteJson`（tmp 経由の置換）  
  置き場は `%LOCALAPPDATA%\redntfy`（`getDataDir`）で、ログもここに置く。exe 同フォルダにしないのは、
  Scoop の更新で旧バージョンのディレクトリに取り残されるのと、書き込み不可の展開先で動かないためだ。
  設定ファイルと通知音だけは exe 同フォルダ。
  旧形式の state.json と `query_ids` への追加分は流入検知を見送り、現在の所属を黙って採用する。（通知の嵐の防止）

- `fetchIssues` は 1 クエリでも失敗すると全体を失敗にして部分結果を破棄する（誤「新規」通知の防止）  
  `fetchQueryIssues` はページング中に total_count が変わると先頭ページから取り直す。（取り直しは 3 回までで、超えたら失敗にする。取り直さずに失敗にすると、集合の正常な更新が接続エラーの Toast になるため）

- グループ担当（👥）の判定は `/groups.json` を試し、403 なら自分の所属グループへ縮退する  
  admin 権限がないと 403 になる。縮退時は他グループ宛の判定が漏れるが誤判定はしない。

- 「バージョン未指定の除外」はバージョン欄がないチケット（トラッカーで欄が無効、またはプロジェクトにバージョン未定義）を除外しない  
  判定材料は `/trackers.json`（Redmine `v5.0` 未満は全トラッカー有効に縮退）と `/projects/:id/versions.json`（プロジェクト単位のキャッシュ）。

- 一覧の行は `list_format`（[app]）のプレースホルダテンプレートで組み立てる（`buildIssueLabel`。語彙と除去規則は同所のコメントと tests/test_main.cpp）  
  `list_limit`（[app]、既定 20、1〜40）は通常行の表示件数の上限で、ピン・非表示の行は数えない。（上限 40 は `LIST_ROW_MAX`（50 行）の内側にピン・非表示の余地を残す値）
  既定の `list_format` は `{author}/{commenter}`（起票者/最後のコメント投稿者）を使い、最終更新者を含めない。（優先度・期日の変更だけで名前が操作者に変わるのを避けるため）
  `{new}` の ✨ は金色、🆙 は青で、赤は期限切れと 💥 に割り当て済みのため使わない。
  解釈できないトークンはリテラルのまま行に表示して起動ログに残す。（アプリは止めない）
  `subject_max_chars`・`project_max_chars` は廃止。（読み捨て＋廃止ログ）

- schedule の 0（休止時間帯）は force poll・stale 判定より先に評価する  
  この順序を保つ。（崩すと深夜に通知が鳴る）起動直後の 1 回だけは休止時間帯でも実行する。
  「今すぐ更新」（`g_manualPoll`）だけは明示操作として休止時間帯・クールダウンを無視し、完了時に必ず応答を返す。（通知件数 0 のときだけ応答 Toast を出し、更新を検知した回は通知 Toast が応答になる）
  登録・更新直後の反映（`g_reflectPoll`）も休止時間帯・クールダウンを無視するが、応答 Toast は出さず、接続エラー Toast も force にしない。
  判定材料の取得（`resolvePollMetadata`）は休止時間帯とクールダウンの判定より後に置き、休止中と延期した回には HTTP 要求を送らない。

- 応答 Toast の文言は抑止件数で切り替える（`showPollDoneToast`）  
  「新しい更新はありません」だけでは、自分が直前に起票・更新したチケットを取得できたのか判別できないため、
  トレイメニュー「自分の操作による更新を通知しない」（`g_muteOwnChanges`、既定 ON）で抑止した件数が 1 件以上なら件数を添える。

- 行コンテキストメニュー（`showRowContextMenu`）は `TrackPopupMenu` を使い、所有をトレイウィンドウとする  
  一覧は `WS_EX_NOACTIVATE` のため所有者にできない。モーダルループ中はポーリングが一覧を組み直し得るため、対象は行の添字でなく id で扱う。
  優先度・期日・終了は `PUT /issues/{id}.json` で 1 項目ずつ送る。終了だけはコメントを兼ねた確認ダイアログを出し、コメントは `status_id` と同じ 1 回の PUT で送る。
  終了は `[redmine] close_status_id` のステータスへ更新し、未設定なら項目を出さない。（ステータス id はサーバごとに異なるため推測しない）
  終了は成功応答の後にチケットを取得し直して照合する。（Redmine は選べない `status_id` をエラーにせず無視して 204 を返すため。取得し直しの失敗は成功扱い）
  取得集合外の行はピン留めにも非表示にもできない。（集合外のチケットは非表示にしても一覧に出ず、次のポーリングで `pruneHidden` が非表示設定を消すだけで意味がないため）
  非表示（`g_hiddenIds`）は参考行で、通知・未処理件数から常に除外する。state.json への記録は全件のまま続けるため、非表示解除時に通知の嵐は起きない。
  ピンと非表示は `list_limit` の予算に数えない。（枠を消費させると未読の通常行を窓外へ押し出すため）

- 一覧の並び順（`ListSort` の 5 方式）の「優先度順（期日考慮）」は、期日が当日以前の未クローズ行の順位を「最上位の 1 つ下」まで繰り上げて並べる  
  繰り上げ先を名前で探さず順位昇順の `g_priorities` の末尾から 2 番目にするのは、優先度の表記が違うサーバでも効かせるためだ。
  繰り上げは並べ替えだけに使い、表示と Redmine 上の優先度は変えない。多数の行が上位に集まると埋もれるため、優先度順と別の方式にした。
  有効な優先度が 2 つ以上のとき、繰り上げ先は、行背景の強調（`priorityHighlightOf`）で淡く塗る段と同じ位置だ。どちらかの位置を変えるときは、もう一方も同じ位置へ揃える。
  優先度の順位表は `/enumerations/issue_priorities.json` から取得し、未取得の間と順位表にない id は priority id を順位にして縮退する。

- 一覧の太字は未読または期日ありの行（`issueRowBold`）  
  未読の既読化は行を開く操作だけで、一覧を開いただけでは既読にしない。（開いた≠読んだ。読み落としを防ぐのが未読表示の目的）
  Toast の「チケットを開く」は OS がブラウザを直接起動するため、アプリは既読化しない。
  赤バッジ（未読あり）は `buildListRows` の結果から数え、表示と同じ選定を通すことでバッジと画面上の未読行の有無を一致させる。作業領域による打ち切りだけはバッジと揃わない。（表示時にしか分からないため）
  非表示への遷移では未読を取り除かない。（非表示は「見なくて良い」であって「読んだ」ではない）
  ✨（起票から `new_mark_hours` 以内）と 🆙（他人の最後の主要変更から同時間以内）は状態を持たない判定で、既読化によらず永続化も不要。

- 一覧は `WS_EX_NOACTIVATE` の自前ポップアップで、入力欄のクリック、検索の Ctrl+F、開閉のショートカットキー以外ではフォーカスを奪わない  
  モーダルメニューではないため、フォーカス復元・EndMenu・クローズ直後のクリック猶予は存在しない。開閉の規則は `showListPopup`・`hideListPopup` と `IDT_LIST_WATCH` のコメント。
  開閉の規則には例外が 1 つだけあり、ホバー自動表示から `hover_click_guard_ms` 以内のアイコン左クリックは無視する。（開くつもりのクリックの直前に自動表示が割り込むと、クリックが「閉じる」操作になるため）
  位置はタスクバーの辺に密着させ、タスクバー沿いはアイコンを範囲に収めたまま画面中央方向へ広げる（`listWindowOrigin`）。アイコンがオーバーフローのフライアウト内なら、フライアウトを覆わないよう横へずらす。
  ショートカットキーで開くときの基準点（`trayListAnchor`）はトレイアイコンの中心で、実ピクセル座標の矩形をタスクバーのウィンドウを基準に `PhysicalToLogicalPointForPerMonitorDPI` で直す。（不可視の自ウィンドウを基準にすると変換が失敗し、常にタスクバーの端へ退避してホバーと位置が食い違った）
  トレイは `NOTIFYICON_VERSION_4` を宣言し、ホバーの開始・終了を OS 通知（`NIN_POPUPOPEN` / `NIN_POPUPCLOSE`）で受ける。（DPI 非対応プロセスでは表示スケール 100% 超でアイコン矩形とカーソル座標の突き合わせが成立しないため）
  `NIM_SETVERSION` 失敗時は旧方式の生マウスメッセージに戻し、ホバー自動表示は無効になる。オーバーフロー領域でのホバーは未検証。
  描画は GDI（📌 のみ D2D）で、作業領域に収まらない行は打ち切る。（スクロールなし。フッタは必ず出す）
  件数ツールチップは全廃で、通常時の tooltip は常に空。（件数はフッタ、未読の有無は赤バッジ）

- 一覧の検索（Ctrl+F の検索欄）は表示中だけトレイウィンドウへ `RegisterHotKey` する  
  一覧は非アクティブでキーが届かないためだ。他アプリが登録済みなら失敗し、検索できないだけで一覧は使える。
  一致判定（`issueLabelMatches`）は空白区切りの全語の部分一致（AND）で、大文字小文字・全角半角・ひらがなカタカナを区別しない。一致行は着色だけで、選定・並び順・件数は変えない。
  検索欄を先頭に置くのは、画面下のタスクバーでは一覧が上へ伸び、出し入れしても既存の行が動かないためだ。

- 一覧のキー操作（上下キー・Enter・Esc）は、入力モードでない表示中はトレイウィンドウのホットキーで受ける  
  表示中は前面アプリのこれらのキーより優先する。（ユーザ判断）登録の要否は `updateListNavHotkeys` が状態から決める。

- 一覧はグローバルショートカットキー（`[app] toggle_hotkey`、既定 `Ctrl+Shift+Win+R`）でも開閉できる  
  開くと検索欄へ入力できる状態にする。実行中に無効モードへ遷移したら登録を解除する。（登録を残すと他アプリからキーを奪ったまま何もしないため）

- 一覧末尾の登録欄（登録先の確定時のみ）は、クリックで入力モードに入り `WS_EX_NOACTIVATE` を外して一覧をアクティブ化する  
  登録先は `register_project` の明示値か、未設定ならログイン名から作った識別子のプロジェクトの存在確認で確定する（`resolvePollMetadata`）。
  送信は `POST /issues.json`（担当＝自分）で、成功の Toast は出さない。（新チケットが一覧に載ることを応答とする）
  他アプリへの切り替えの検出は `WM_ACTIVATE` と、`IDT_LIST_WATCH` がフォアグラウンドを突き合わせる判定の 2 経路だ。（DTP のカレンダーを開いたまま切り替わると `WM_ACTIVATE` が一覧に届かないため）
  DTP の見た目のため common controls `v6` のマニフェストを `build.ps1` の `/MANIFEST:EMBED` で埋め込む。

- Toast には AUMID 付きスタートメニューショートカットが必須（`ensureShortcut` が自動作成）  
  Toast の例外は `dispatchToastXml` が捕捉してログに記録し、呼び出し側は失敗で処理を変えない。

## 参考

- README.md
