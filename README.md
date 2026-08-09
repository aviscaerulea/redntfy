# redntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/redntfy)](https://github.com/aviscaerulea/redntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/redntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/redntfy/actions/workflows/release.yml)

Redmine の更新チケットを Windows の通知で知らせ、未処理チケットをタスクトレイから一覧できる軽量常駐アプリです。

主な機能は、チケットの新着や更新を知らせる通知と、トレイアイコンから開く未処理チケット一覧の表示です。

実測での物理メモリ使用量は約 7MB です。

## 機能

- チケットの通知：Redmine をポーリングし、新着や更新を Windows 通知と音声で知らせる
- システムトレイ：未処理チケット一覧の表示や設定をトレイアイコンから操作できる
  - 一覧の書式：行の並びや表示項目をプレースホルダで指定できる
  - ピン留め：気になるチケットを一覧に固定する（クローズしても消えない）
  - 非表示：見なくて良いチケットを通知と未処理件数から除外する
  - ブラウザ表示：行やフッタをクリックするとブラウザで開く
- 追跡対象の調整：Redmine のカスタムクエリ（query_ids）で通知対象を絞り込める
- ゼロ設定起動：URL と API キーだけで自分と所属グループの担当チケットを追跡する

### システムトレイ

トレイアイコンは、未読のチケットがあると右下へ赤いバッジを表示します。カーソルを乗せたままにすると、左クリックと同じ未処理チケット一覧を表示します。ホバー表示の ON/OFF は、トレイメニューの「マウスホバーで一覧を自動表示」で切り替えます。

未処理チケット一覧はフォーカスを奪わないため、直前まで使っていたウィンドウでの入力を妨げません。カーソルがアイコンと一覧の外に出ると自動で閉じます。左クリックは開閉のトグルとして働きます。未処理チケット一覧はマウス専用で、キーボードでは操作できません。

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

`redntfy.local.toml` が無いまま起動しても大丈夫です。テンプレートを自動生成して設定ファイルを開き、キーの取得ページへ案内します。

これだけで、自分（と所属グループ）が担当のオープンチケットの追跡が始まります。
追跡範囲を自分で調整したいときは、Redmine のカスタムクエリを作成して `query_ids` を設定します。

1. Redmine で **プロジェクトを指定せず** に絞り込んだチケット一覧を、カスタムクエリとして保存する
   - 特定プロジェクト配下で保存したクエリは API から参照できないため、必ずグローバルなカスタムクエリとして作成する
2. 保存後の URL `/issues?query_id=N` の `N` を控える（複数クエリを追跡するなら各クエリ分）
3. `redntfy.local.toml` に `query_ids = [12, 34]` の形式で追記する
4. `redntfy.exe` を再起動する

## 設定

動作設定は `redntfy.exe` と同じフォルダの `redntfy.toml` に記載します。
`redntfy.local.toml` を置くとキー単位で上書きでき、接続情報の分離や環境ごとの差分管理に使えます。
検知済み状態は `state.json`、ピン留めは `pins.json`、非表示チケットは `hidden.json` に保存し、再起動後も保持します。

主要な設定項目は以下のとおりです。詳細は `redntfy.toml` のコメントを参照してください。

| セクション | キー | 説明 |
| --- | --- | --- |
| `[app]` | `schedule` | 時間帯ごとのポーリング回数（24 要素の配列、回/時。0 で休止） |
| `[app]` | `list_limit` | 一覧の表示件数（デフォルト 20） |
| `[app]` | `list_format` | 一覧の行フォーマット（プレースホルダ指定） |
| `[app]` | `hover_delay_ms` | ホバーで一覧を表示するまでの遅延（ミリ秒、0〜5000。デフォルト 100、0 で即時） |
| `[app]` | `hover_click_guard_ms` | ホバー自動表示の直後、左クリックで閉じない猶予（ミリ秒、0〜5000。デフォルト 300、0 で無効） |
| `[app]` | `bug_trackers` | 💥 を付けるトラッカー名のパターン |
| `[app]` | `duck_targets` | 通知音を鳴らす間だけミュートする他アプリのプロセス名 |
| `[redmine]` | `url`, `api_key` | 接続情報（必須） |
| `[redmine]` | `query_ids` | 追跡するカスタムクエリの id（省略時は担当チケットを追跡） |
| `[loudness]` | `enabled`, `target` | 通知音のラウドネス正規化 |
| `[update]` | `enabled` | 起動時の更新チェック |

### query_ids の設定あり・なしの違い

`query_ids` は省略できます。設定の有無で追跡対象が次のように変わります。

| 項目 | 設定あり | 設定なし |
| --- | --- | --- |
| 追跡対象 | 指定したカスタムクエリの和集合 | 自分（と所属グループ）が担当のオープンチケット |
| 事前準備 | Redmine でカスタムクエリの作成が必要 | `url` と `api_key` だけで使える |
| 追跡範囲の調整 | クエリのフィルタで自由に絞り込める | 固定（絞り込み不可） |
| 通知・一覧から開く Redmine の画面 | 先頭クエリのチケット一覧 | 担当者を自分で絞ったチケット一覧 |

まずは設定なしで使い始め、追跡範囲を調整したくなったらカスタムクエリを作成して `query_ids` を設定する使い方がおすすめです。
設定の切り替え時に通知があふれることはありません。

## 制限事項

- 追跡クエリは Redmine のグローバルなカスタムクエリのみ対応（プロジェクト配下のクエリは API から参照できない）
- グループ担当の判定は admin 権限があれば全グループが対象、無ければ自分の所属グループのみ
- オーバーフロー領域（隠れているインジケーター）のアイコンはホバーで開けない（左クリックは可能）
- 設定ファイルはホットリロード非対応で、変更反映には再起動が必要
- 設定に不備があるときは通知を止めて案内モードで常駐する（トレイのアイコンと説明に従い、設定を直したら再起動）

## ライセンス

アプリケーションアイコンには Redmine 公式ロゴを使用しています。
ロゴは Martin Herr 氏の著作物で、[CC BY-SA 2.5](https://creativecommons.org/licenses/by-sa/2.5/) でライセンスされています。
