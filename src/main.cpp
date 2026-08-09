// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * redntfy - Redmine の更新チケットを Windows Toast 通知で知らせる常駐アプリ
 *
 * exe 同フォルダの redntfy.toml（redntfy.local.toml がキー単位で上書き）から設定を読み込み、
 * [redmine] で指定した複数のグローバル保存クエリ（query_ids）を schedule に従ってポーリングし、
 * チケット id で重複排除した和集合を追跡する。
 * query_ids 省略時はフォールバックモードとして、自分（と所属グループ）が担当のオープンチケットを追跡する。
 * schedule は 0 時〜23 時の 24 要素配列。（回/時、0 でその時間帯は休止）
 * 追跡集合への新規流入と既知チケットの updated_on 進行・新クエリ流入を Toast 通知と音声で知らせる。
 * 自分が起票したチケットの流入（author.id で判定）は通知しない。
 * 自分の操作による更新と、前回ポーリング以降の自分の更新が原因の流入（最終 journal の user.id で判定）も通知しない。
 * 検知済み状態は「チケット id → updated_on ＋所属クエリ集合」を state.json（v2）に永続化して重複通知を防ぐ。
 * トレイアイコンのホバーまたは左クリックで、フォーカスを奪わない非アクティブの自前ポップアップに
 * 未処理チケットの一覧を表示し、行の右クリックで 通常 → ピン留め → 非表示 → 通常 の順に
 * 状態を切り替えられる。（ピンの件数に上限はない）
 * ピンは pins.json に永続化し、保存クエリの集合から外れたチケットも一覧に表示し続ける。
 * 非表示チケットは hidden.json に永続化し、グレー表示・通知と件数から除外・
 * 「非表示チケットを除外」トグル ON で一覧からも出さない。
 *
 * 終了コード：
 *   0  - 正常終了（トレイメニューの「終了」による）
 *   1  - トレイウィンドウ生成失敗（fail-fast）
 *   2  - 予期しない初期化エラー
 * [redmine] url / api_key の未設定・無効（実行時 401 含む）と query_ids 設定時の実行時 404 は終了せず、
 * 原因別の案内（Toast・設定ファイル・ブラウザ誘導）を出して無効モードで常駐する。
 * 無効モード：トレイは app-disable.ico・「今すぐ更新」非活性・ポーリング停止。復帰は再起動のみ。
 *
 * 依存ライブラリ：WinHTTP, WinRT (Windows.UI.Notifications, Windows.Data.Json), Propsys
 * 外部依存：libebur128（vcpkg: libebur128:x64-windows-static）
 * ビルド：build.ps1 参照（rc + cl、/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup）
 */

// ebur128 は windows.h より先にインクルードする（ヘッダ内マクロ衝突回避）
#include <ebur128.h>

// C++/WinRT ヘッダは windows.h より先にインクルードする
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

// winsock2.h は windows.h より先にインクルードする
// （netioapi.h が要求する ws2def.h と、windows.h が既定で取り込む旧 winsock.h の衝突回避）
#include <winsock2.h>
#include <windows.h>
#undef GetObject  // GDI マクロを解除（winrt::IJsonValue::GetObject と競合するため）
#include <winhttp.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>

#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#include <netioapi.h>
#pragma comment(lib, "iphlpapi.lib")

// ピンマーカー（📌）のカラー絵文字描画に使う DirectWrite / Direct2D。
// GDI の DrawTextW はフォントリンクで字形自体は出るが、カラーフォント（Segoe UI Emoji の
// COLR/CPAL レイヤ）を解釈せず単色になるため、マーカー列だけ D2D で描画する。
#include <dwrite.h>
#pragma comment(lib, "dwrite.lib")
#include <d2d1_1.h>   // ID2D1Factory1 / ID2D1DeviceContext（d2d1.h も取り込まれる）
#pragma comment(lib, "d2d1.lib")

#include "toml.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cmath>
#include <climits>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "user32.lib")

#include "resource.h"
#include "version.h"  // ビルド時生成（APP_VERSION を定義）

// アプリケーション識別子（Toast 通知に使用）
static const wchar_t* APP_AUMID = L"com.redntfy";

// エラー時のリトライ待機時間（ミリ秒）
static constexpr DWORD RETRY_WAIT_MS = 60u * 1000u;

// ホバーで一覧を表示するまでの遅延（ms）。0 は即時表示
static constexpr long long DEFAULT_HOVER_DELAY_MS = 100;
static constexpr long long MIN_HOVER_DELAY_MS     = 0;
static constexpr long long MAX_HOVER_DELAY_MS     = 5000;

// ホバー自動表示直後にアイコン左クリックの「閉じる」を無視する猶予（ms）。0 で無効
// （一覧を出すつもりのクリックが、先行したホバー表示により「閉じる」へ化けるのを防ぐ）
static constexpr long long DEFAULT_HOVER_CLICK_GUARD_MS = 300;
static constexpr long long MIN_HOVER_CLICK_GUARD_MS     = 0;
static constexpr long long MAX_HOVER_CLICK_GUARD_MS     = 5000;

// トレイアイコン用メッセージ ID
static constexpr UINT WM_TRAYICON        = WM_USER + 1;
static constexpr UINT WM_UPDATE_TOOLTIP  = WM_USER + 2;

// 無効モード遷移をメインスレッドへ委譲するメッセージ（wParam = DisabledReason）
// トレイアイコン更新（Shell_NotifyIconW）をメインスレッドに限定する不変条件を守るため、
// ポーリングスレッドは直接 UI を触らずこのメッセージを投函する
static constexpr UINT WM_ENTER_DISABLED  = WM_USER + 3;

// コンテキストメニューコマンド ID
static constexpr UINT IDM_EXIT             = 40002;
static constexpr UINT IDM_MUTE_IN_MEETING  = 40004;
static constexpr UINT IDM_SOUND_ENABLED       = 40005;
static constexpr UINT IDM_OPEN_CONFIG         = 40006;
static constexpr UINT IDM_OPEN_LOG            = 40007;
static constexpr UINT IDM_OPEN_GITHUB         = 40008; // GitHub リポジトリページを開く
static constexpr UINT IDM_OPEN_QUERY          = 40009; // Redmine の代表画面（保存クエリ画面、フォールバック時は担当一覧）を開く
static constexpr UINT IDM_STARTUP             = 40010; // Windows スタートアップ登録トグル
static constexpr UINT IDM_ASSIGNED_TO_ME      = 40011; // 担当がグループのチケットを一覧・tooltip・通知から除外するトグル
static constexpr UINT IDM_UPDATE_NOW          = 40012; // 休止時間帯・クールダウンを無視した即時ポーリング
static constexpr UINT IDM_SORT_BY_DUE         = 40013; // 一覧を期日昇順に並べるトグル
static constexpr UINT IDM_EXCLUDE_NO_VERSION  = 40014; // バージョン未指定を一覧・tooltip・通知から除外するトグル（期日ありは例外的に残す）
static constexpr UINT IDM_MUTE_OWN_CHANGES    = 40015; // 自分の操作による起票・更新を通知抑止するトグル（一覧・tooltip は変更しない）
static constexpr UINT IDM_OPEN_GUIDE          = 40016; // セットアップガイド（GitHub Pages）を開く
static constexpr UINT IDM_EXCLUDE_HIDDEN      = 40017; // 非表示チケットを一覧から除外するトグル（OFF はグレーで表示）
static constexpr UINT IDM_HOVER_POPUP         = 40018; // ホバーで一覧を自動表示するトグル（OFF でも左クリックでは開ける）

static constexpr wchar_t GITHUB_URL[]                 = L"https://github.com/aviscaerulea/redntfy";
// セットアップガイド（GitHub Pages。docs/ 配下の内容が公開される）
static constexpr wchar_t GUIDE_URL[]                  = L"https://aviscaerulea.github.io/redntfy/";
static constexpr wchar_t GITHUB_RELEASES_URL[]        = L"https://github.com/aviscaerulea/redntfy/releases";
static constexpr wchar_t GITHUB_API_RELEASES_LATEST[] = L"https://api.github.com/repos/aviscaerulea/redntfy/releases/latest";

// 一覧ポップアップの最大行数（ピン・非表示込みのハードキャップ。旧メニュー実装の 50 を踏襲）
static constexpr UINT LIST_ROW_MAX = 50;

// ホバー表示のワンショット遅延タイマーと、一覧ポップアップの監視用ポーリングタイマー
// IDT_LIST_WATCH は表示中の離脱検出と、明示クローズ後のホバー再アーム保留の解除を兼ねる
static constexpr UINT  IDT_HOVER_TRIGGER  = 1;
static constexpr UINT  IDT_LIST_WATCH     = 2;
static constexpr DWORD LIST_WATCH_POLL_MS = 200;
// カーソルがアイコン・ポップアップの外に連続でこの tick 数（約 400ms）観測されたら閉じる
static constexpr int   LIST_LEAVE_TICKS   = 2;

// 即時ポーリングの抑制間隔（前回ポーリングからこの時間内は即時ポーリングをスキップ）
static constexpr DWORD FORCE_POLL_COOLDOWN_MS = 60'000;

// 通知音のデフォルトファイル名（exe 同フォルダに配置）
static constexpr wchar_t DEFAULT_SOUND_FILE[] = L"sound.wav";

// 通知音 WAV ファイルの最大サイズ（バイト）。これを超えると不正ファイル扱いで読み込みを拒否する。
static constexpr DWORD MAX_WAV_FILE_BYTES = 16u * 1024 * 1024;

// 円周率（MSVC では M_PI に _USE_MATH_DEFINES が必要なため自前定義）
static constexpr double PI = 3.14159265358979323846;

// エラー Toast の最小間隔（30 分）
static constexpr ULONGLONG ERROR_TOAST_COOLDOWN_MS = 30uLL * 60 * 1000;

// stale 判定のしきい値（1 時間）
// ループが目覚めた時点で前回ポーリングからこの時間が経過していた場合、stale としてログに残す。
// スリープ復帰は WM_POWERBROADCAST の forcePoll が主経路で、本判定はその補足検知にあたる。
static constexpr ULONGLONG STALE_POLL_THRESHOLD_MS = 3'600'000ULL;

// チケットなし時の一覧表示文言（クリックでクエリ画面を開く）
static constexpr wchar_t NO_ISSUES[] = L"チケットはありません";

// 設定ファイル名（exe 同フォルダ。local が同名キーをキー単位で上書きする）
static constexpr wchar_t CONFIG_FILENAME[]       = L"redntfy.toml";
static constexpr wchar_t CONFIG_LOCAL_FILENAME[] = L"redntfy.local.toml";

// 検知済み状態の永続化ファイル名（exe 同フォルダに保存）
static constexpr wchar_t STATE_FILENAME[] = L"state.json";

// ピン留めの永続化ファイル名（exe 同フォルダに保存）
static constexpr wchar_t PINS_FILENAME[] = L"pins.json";

// 非表示チケットの永続化ファイル名（exe 同フォルダに保存。チケット id の JSON 配列のみ）
static constexpr wchar_t HIDDEN_FILENAME[] = L"hidden.json";

// シャットダウンフラグ（メインスレッド・WndProc・ポーリングスレッドから参照）
static std::atomic<bool> g_shutdownRequested{false};

// 音声通知の有効/無効フラグ（レジストリで永続化、トレイメニューの親項目）
static std::atomic<bool> g_soundEnabled{true};

// マイク/カメラ使用中の音声自動ミュートフラグ（レジストリで永続化）
static std::atomic<bool> g_muteInMeeting{true};

// 担当者が自分個人のチケットだけを対象とするフラグ（レジストリで永続化、トレイメニュー）
// グループ担当のチケットを一覧・tooltip・通知から外すためのもの。
// 取得と state.json は常に全件のまま扱い、表示と通知の直前だけで絞る。
// 追跡集合そのものを絞ると、OFF に戻したとき state.json に無い id が「新規」と誤検知される。
// 対価として、ON 中に抑止した更新は state.json に記録済みのため OFF に戻しても再通知されない。
static std::atomic<bool> g_assignedToMeOnly{false};

// 一覧を期日昇順に並べるトグル（レジストリ永続化。OFF は更新日時降順）
static std::atomic<bool> g_sortByDue{false};

// バージョン未指定のチケットを一覧・tooltip・通知から除外するトグル（レジストリ永続化）
// 期日が設定されているチケットは「時限のある未来課題」として、バージョン未指定でも残す。
// 取得と state.json は常に全件のまま扱い、表示と通知の直前だけで絞る。
// 追跡集合そのものを絞ると、OFF に戻したとき state.json に無い id が「新規」と誤検知される。
// 対価として、ON 中に抑止した更新は state.json に記録済みのため OFF に戻しても再通知されない。
static std::atomic<bool> g_excludeNoVersion{false};

// 非表示チケットを一覧から除外するトグル（レジストリ永続化。既定 OFF ＝グレーで表示）
// ON で g_hiddenIds のチケットを一覧に出さない。OFF では非活性色（グレー）で参考表示する。
// 未処理件数・未読件数・通知は本トグルと無関係に常に非表示チケットを除外する。
// （「見なくて良いもの」の意思表示は id 単位の g_hiddenIds 側が持ち、本トグルは見え方だけを変える）
static std::atomic<bool> g_excludeHidden{false};

// ホバーで一覧を自動表示するトグル（レジストリ永続化。既定 ON）
// OFF は hover_delay_ms を無視してホバーでは表示しない。（左クリックでの表示は従来どおり）
static std::atomic<bool> g_hoverPopupEnabled{true};

// 自分の操作による起票・更新を通知抑止するトグル（レジストリ永続化。既定 ON）
// deliverPollResults の 3 か所の判定（新規流入の起票者、新規流入の直近更新者、既存更新の更新者）を
// 一括で ON/OFF する。抑止対象になったチケットも state.json には全件記録するため、
// OFF に戻したときに再通知は起きない。
// 一覧・tooltip・バッジ・並び順は本トグルの影響を受けない。（表示ではなく通知のみを制御する）
static std::atomic<bool> g_muteOwnChanges{true};

// トレイウィンドウのハンドル（メインスレッドで作成し、ポーリングループと通知スレッドが参照）
static HWND g_hWnd = nullptr;

// トレイのポップアップ表示中フラグ（一覧ポップアップ可視、または右クリックメニュー表示中。
// ツールチップ・バッジ更新の抑制用）
static std::atomic<bool> g_popupShowing{false};

// ホバー遅延（ms、0〜5000 にクランプ済み）。起動時に loadConfig の値を反映し以降不変
static std::atomic<DWORD> g_hoverDelayMs{static_cast<DWORD>(DEFAULT_HOVER_DELAY_MS)};

// ホバー自動表示直後のクリック猶予（ms、0〜5000 にクランプ済み）。
// 起動時に loadConfig の値を反映し以降不変
static std::atomic<DWORD> g_hoverClickGuardMs{static_cast<DWORD>(DEFAULT_HOVER_CLICK_GUARD_MS)};

// 一覧ポップアップの開閉制御。トレイ WndProc スレッドのみが読み書きするため atomic 不要
// g_listOutsideTicks：カーソルがアイコン・ポップアップ矩形の外に居た連続 tick 数
// g_hoverRearmPending：明示クローズ後、カーソルがアイコンから離れるまでホバー再表示を
//   保留するフラグ。（閉じてもカーソルが乗ったままだと、わずかな動きで即座に開き直るため。
//   IDT_LIST_WATCH の監視でアイコン離脱を検出したら解除する）
// g_hoverShownAt：ホバーで自動表示した時刻（GetTickCount64）。左クリック表示とクローズで
//   0 に戻す。（不変条件：非 0 はホバー起点の一覧が表示中のときだけ）
//   ホバー自動表示直後の左クリックを「閉じる」と解釈しないための判定に使う
static int       g_listOutsideTicks  = 0;
static bool      g_hoverRearmPending = false;
static ULONGLONG g_hoverShownAt      = 0;

// スリープ復帰・ロック解除時の即時ポーリングフラグ
static std::atomic<bool> g_forcePoll{false};

// トレイメニュー「今すぐ更新」の即時ポーリングフラグ
// 明示のユーザ操作のため、g_forcePoll と違い休止時間帯・クールダウンの抑止を受けない
static std::atomic<bool> g_manualPoll{false};

// 前回ポーリング実行時刻（GetTickCount64、連続ポーリング抑制・stale 判定用）
static std::atomic<ULONGLONG> g_lastPollTick{0};

// 前回エラー Toast 表示時刻（GetTickCount64、スパム防止用。0 = 未表示）
// GetTickCount64 はシステム起動からの経過時間のため、初期値 0 との減算比較は
// 「起動後 30 分間はすべて抑制」という誤動作になる。0 は必ずセンチネルとして特別扱いする。
static std::atomic<ULONGLONG> g_lastErrorToastTime{0};

// 無効モードの原因（設定不備の分類）
// None 以外のとき無効モード：ポーリング停止・トレイは app-disable.ico・「今すぐ更新」非活性。
// 起動時の静的検査（wmain）または実行時の確定的判定（401/404）で一度だけ None 以外になり、
// ホットリロードしない仕様のため復帰は再起動のみ。（プロセス生存中に None へは戻らない）
// atomic<int> にするのは atomic<enum class> が処理系依存で non lock-free になり得るため。
enum class DisabledReason : int {
    None            = 0,
    InvalidUrl      = 1,  // url 未設定または非 http(s)（静的検査のみ）
    InvalidApiKey   = 2,  // api_key 未設定（静的）または実行時 HTTP 401
    InvalidQueryIds = 3,  // 実行時 HTTP 404（query_ids 設定時のみ。未設定はフォールバックモードで正常動作）
};
static std::atomic<int> g_disabledReason{ static_cast<int>(DisabledReason::None) };

// 現在の無効モード原因を返す
static DisabledReason disabledReason() {
    return static_cast<DisabledReason>(g_disabledReason.load());
}

// 無効モード中かを返す
static bool isDisabled() {
    return disabledReason() != DisabledReason::None;
}

// TaskbarCreated メッセージ ID（エクスプローラ再起動対策）
static UINT WM_TASKBAR_CREATED = 0;

// ==================== データ構造 ====================

// Redmine チケット 1 件分
// updated_on は UTC ISO 8601（"YYYY-MM-DDTHH:MM:SSZ"）のため、文字列の辞書順比較で新旧を判定できる。
struct Issue {
    int         id        = 0;
    std::string subject;
    std::string projectName;       // 一覧の表示専用。通知判定にも state.json にも使わない
    std::string updatedOn;
    int         authorId  = 0;     // 自分の起票を通知対象から外すために保持する
    std::string authorName;        // 起票者名（新規流入 Toast の表示用。更新者名が取れない場合のフォールバック）
    int         assignedToId = 0;  // 担当者 id（0 = 未割当）
                                   // Redmine はユーザとグループが同一 id 空間のため、自分の id と
                                   // 一致するかを見るだけでグループ担当を弾ける。型の判別は不要。
    bool        closed    = false; // status.is_closed（一覧で打ち消し線表示）
    std::string dueDate;           // due_date（"YYYY-MM-DD"、期限なしは空。一覧の日付表示に使う）
    std::vector<int> queryIds;     // このチケットが現れた保存クエリ id（昇順）。クエリ流入の検知に使う
    bool assignedToGroup = false;  // 担当がグループ（一覧の 👥 マーカー。取得後にグループ id 集合と突合して設定）
    bool isBugTracker    = false;  // トラッカーが bug_trackers に一致（一覧の 💥 マーカー。パース時に判定）
    bool hasFixedVersion = false;  // fixed_version が JSON に含まれ非 null。バージョンフィルタで見る
    int  trackerId = 0;            // トラッカー id（バージョン欄の有無判定用。0 = 不明）
    int  projectId = 0;            // プロジェクト id（バージョン欄の有無判定用。0 = 不明）
    // バージョン欄がこのチケットに設けられているか（取得後にポーリングスレッドが付与）
    // トラッカーで欄が無効、またはプロジェクトにバージョン未定義なら false。
    // false のチケットは「バージョン未指定の除外」の対象にしない。（設定しようがないため）
    // 不明・判定不能時は true = 従来どおり除外対象。
    bool hasVersionField = true;
    // 最終更新者（resolveUpdaters が journals から確定する。journal なしは起票者で代替）
    int         updaterId = 0;     // 自分の操作による通知の抑止判定に使う（0 = 未確定）
    std::string updaterName;       // フルネーム（Toast の「更新：○○」表示用）
    std::string updaterDisplay;    // 一覧の表示名（姓。取得できない場合はフルネーム）
    std::string updaterFirstName;  // 一覧の {firstname} 用（姓と違いフォールバックせず、未取得は空）
};

// ピン留め 1 件分
// id 以外もキャッシュするのは、起動直後や API 失敗時にもピンだけは一覧に出せるようにするため。
struct PinEntry {
    int         id = 0;
    std::string subject;
    std::string projectName;       // プロジェクト名（集合外ピンも表示できるよう永続化）
    std::string updatedOn;
    bool        closed = false;
    std::string dueDate;           // due_date（"YYYY-MM-DD"、期限なしは空。一覧の日付表示に使う）
    bool assignedToGroup = false;  // 担当がグループ（👥 マーカー。集合外ピンも表示できるよう永続化）
    bool isBugTracker    = false;  // バグ・障害トラッカー（💥 マーカー。集合外ピンも表示できるよう永続化）
    std::string updaterDisplay;    // 最終更新者の表示名（姓。集合外ピンも表示できるよう永続化）
    std::string updaterFirst;      // 最終更新者の名（{firstname} 用。集合外ピンも表示できるよう永続化）
};

// 一覧行フォーマットの要素種別（FormatToken::element の値）
enum FormatElement {
    FMT_LITERAL = -1,  // リテラル文字列（未知プレースホルダの原文もこれで保持）
    FMT_ID, FMT_LASTNAME, FMT_FIRSTNAME, FMT_GROUP, FMT_PROJECT,
    FMT_DUE, FMT_BUG, FMT_SUBJECT, FMT_AGO,
};

// list_format の解析済みトークン
// 起動時に 1 回解析して保持し、行組み立て（buildIssueLabel）はこの列の走査だけで済ませる。
struct FormatToken {
    std::wstring literal;              // FMT_LITERAL のときの出力文字列
    int          element = FMT_LITERAL;
    int          maxChars = 0;         // 最大文字数（0 = 切り詰めなし）
};

// list_format の既定値（v1.4 までの固定並びと同一の表示になる）
static constexpr wchar_t LIST_FORMAT_DEFAULT[] =
    L"#{id}  {lastname}  {group}[{project:5}] {due} {bug}{subject:40}{ago}";

// フォールバックモード（query_ids 省略）の擬似クエリ id
// Redmine のクエリ id は正の整数のため 0 は衝突しない。state.json の queries／
// 各チケットの所属クエリにそのまま流し、流入検知・黙って採用の既存ロジックに乗せる
static constexpr int FALLBACK_QUERY_ID = 0;

// loadConfig の戻り値
struct Config {
    // [redmine] 接続設定（url・api_key は必須。いずれか欠けると無効モードで常駐する）
    std::wstring redmineUrl;       // Redmine の URL（末尾スラッシュを除去して保持する）
    std::wstring apiKey;           // 個人 API アクセスキー
    // 追跡対象のグローバル保存クエリ id（省略可。設定の記述順を保持し std::set にしない）
    // 先頭要素は「代表クエリ」で、複数件 Toast と一覧フッタから開く URL に使う。
    // 空のときはフォールバックモードとして、自分（と所属グループ）が担当のチケットを追跡する。
    std::vector<int> queryIds;

    std::vector<int>          schedule;         // 24 要素（0 時〜23 時の 1 時間あたりポーリング回数、0 で休止）
    int                       listLimit;        // 一覧の非ピン表示件数（デフォルト 20。ピンと非表示は本値の予算外）
    // 一覧の行フォーマット（解析済みトークン列。既定は従来の固定並びと同一）
    std::vector<FormatToken>  listFormat;
    // 💥 を付けるトラッカー名（* のワイルドカード可。既定は空＝どのチケットにも付けない）
    // 比較対象の Redmine のトラッカー名が UTF-8 のため、wstring へ変換せず持つ
    std::vector<std::string>  bugTrackers;
    std::vector<std::wstring> duckTargets;      // 通知音再生中にミュートするプロセス名
    // バージョン欄判定情報（トラッカー定義・プロジェクトのバージョン定義）の再取得間隔（時間）
    // 超過していたら次のポーリングで直ちに再取得する。デフォルト 24。（1〜168）
    int versionMetaRefreshHours;
    // ホバーで一覧を表示するまでの遅延（ms、0〜5000、0 で即時、デフォルト 100）
    long long hoverDelayMs;

    // ホバー自動表示直後にアイコン左クリックを無視する猶予（ms、0〜5000、0 で無効、デフォルト 300）
    long long hoverClickGuardMs;

    // [guard] ガードトーン設定（BLE ヘッドホン対処）
    int   guardToneMs;      // ガードトーン長（冒頭・末尾共通、ms。0 で無効、デフォルト 1500）

    // [loudness] ラウドネスノーマライズ設定
    bool  loudnessEnabled;      // ノーマライズ有効/無効（デフォルト true）
    float loudnessTarget;       // 目標ラウドネス LUFS（デフォルト -16.0）
    float loudnessPeakCeiling;  // ピーク上限（デフォルト 0.891 = -1dBFS）

    // [update] 更新チェック設定
    bool  updateCheckEnabled;   // 起動時の GitHub リリースチェック有効/無効（デフォルト true）
};

// ノーマライズ済み WAV データキャッシュ（起動時に 1 回だけ構築する）
struct WavCache {
    std::vector<int16_t> samples;
    WAVEFORMATEX         fmt;
    bool                 valid = false;
};
static WavCache g_wavCache;

// ポーリングスレッド → WndProc スレッド：チケット一覧の受け渡し（g_mtx で保護）
static std::mutex              g_mtx;
static std::vector<Issue>      g_issues;   // updated_on 降順ソート済み
static std::vector<PinEntry>   g_pins;     // ピン留め（g_mtx で保護。上限なし）

// 起動設定（wmain で loadConfig 直後・スレッド起動前に 1 回だけ設定し、以降は不変。
// ホットリロードしないため、スレッド起動後は全スレッドからロック無しで読み取ってよい）
static Config                  g_currentConfig;

// 未読チケットの id 集合（g_mtx で保護。一覧の太字表示と tooltip の未読件数の唯一の根拠）
// 通知対象になった id を入れ、一覧の行クリックでそのチケットを開いた時だけ取り除く。
// 一覧を開いただけでは既読にしない。（開いたことは読んだことではない）
// 件数とバッジは buildListRows が返す行、すなわち一覧に出る行だけから数える。
// そのため list_limit の窓外に落ちた id は数にも太字にも出ない。
// （表示できない行でバッジが消せなくなるのを防ぐため、表示範囲を件数の基準に揃えた）
// 追跡集合から外れた id も同様に出ないが、ピン留め行は一覧に残るため数に入る。
// 刈り取りはしないので、再び一覧に出た時点で未読として現れる。
// 永続化しない。
static std::unordered_set<int> g_unreadIds;

// 非表示チケットの id 集合（g_mtx で保護。hidden.json で永続化）
// 一覧の右クリックループ（通常 → ピン留め → 非表示 → 通常）で出入りする。
// 含まれる id は通知（Toast・音・未読）の対象外で、未処理件数にも数えない。
// ピン留めとは排他。（ループ遷移でピン → 非表示になるときピンは解除される）
// クエリの取得集合から外れた id はポーリング成功時に自動削除する。（pruneHidden。
// 後日クエリへ戻ったチケットは通常状態で再出現し、必要なら改めて非表示にする）
static std::unordered_set<int> g_hiddenIds;

// 自分の Redmine user id（起動時に /users/current.json で確定。0 = 取得失敗＝自分除外判定なし）
// ポーリングスレッドが書き込み、担当者フィルタのため WndProc スレッドも読むので atomic とする。
static std::atomic<int>        g_myUserId{0};

// 一覧・tooltip・通知に出す対象かを判定する（トレイメニューの「担当がグループのチケットを除外」）
// user id が未取得（0）の間はフィルタを一時的に無効化して全件通す。判定不能を理由に
// 一覧が空になる方が実害が大きいため、既存の「判定できないものは通知側に倒す」方針と揃える。
static bool passesAssigneeFilter(const Issue& is) {
    int me = g_myUserId.load();
    if (!g_assignedToMeOnly.load() || me == 0) return true;
    return is.assignedToId == me;
}

// 一覧・tooltip・通知に出す対象かを判定する（トレイメニューの「バージョン未指定のチケットを除外」）
// バージョン未指定でも以下は通す：
// - 期日あり（時限のある未来課題）
// - バージョン欄がそもそも無いチケット（トラッカーで欄が無効、またはプロジェクトに
//   バージョン未定義。設定しようがないものは「将来の課題」と見なせない）
// 「将来の課題として起票しておいた」チケットを日常の一覧から外すのが本フィルタの意図。
static bool passesVersionFilter(const Issue& is) {
    if (!g_excludeNoVersion.load()) return true;
    return is.hasFixedVersion || !is.dueDate.empty() || !is.hasVersionField;
}

// トレイアイコンの表示状態（通常／未読バッジ付き／無効モード）
// NIM_MODIFY の無駄な呼び出しを抑制するために直前の状態を保持する
enum class TrayIconStyle { Normal, NormalBadged, Disabled };
static TrayIconStyle           g_trayIconStyle    = TrayIconStyle::Normal;
// updateTrayTooltip のリエントランシーガード
// Shell_NotifyIconW が内部でメッセージポンプして WM_TIMER 等を呼ぶことへの対処
static bool                    g_tooltipUpdating  = false;

// 更新チェック結果（起動時に 1 回書き込まれ、以降は読み取り専用）
static std::atomic<bool>  g_updateAvailable { false };
static std::wstring        g_latestVersion;   // g_mtx で保護

// 通知音再生スレッドのハンドル
//
// アクセスは launchSound（呼び出し元は pollThreadFunc）と、wmain のシャットダウン処理
// （pollThread.join() の後）に限定する。ポーリングスレッド終了後にのみメインスレッドが
// 触るため並行アクセスがなく、ミューテックス保護は不要。新たな呼び出し箇所を追加する
// 場合はこの排他条件が保たれることを確認すること。
static HANDLE g_soundThread = nullptr;

// exe ディレクトリパス（wmain 起動時に確定し、WndProc スレッドからも参照する）
static std::wstring g_exeDir;

// 一覧ポップアップのチケット行描画用フォント（initMenuFonts で初期化）
static HFONT g_hMenuFont     = nullptr;
static HFONT g_hMenuFontBold = nullptr;  // 未読行用の太字（フェイス・サイズは g_hMenuFont と同一）
static HFONT g_hMenuFontSemiBold = nullptr;  // ラベル内の部分強調用（同上）

// 通知音再生スレッドへの受け渡し用コンテキスト
// ダッキング操作（duck/unduck）はすべて soundThread 内で実行する。
// ISimpleAudioVolume を取得したスレッドと別スレッドで Release すると、
// COM スレッド境界をまたいだプロキシ解放となり潜在リスクがあるため、
// 取得・復元・解放をすべて同一スレッドに集約する。
struct SoundContext {
    Config cfg;
};

// ==================== ユーティリティ ====================

// exe のあるディレクトリパスを取得する
static std::wstring getExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

// SYSTEMTIME を ULARGE_INTEGER（100 ナノ秒単位）に変換する
static ULARGE_INTEGER systemTimeToUli(const SYSTEMTIME& st) {
    FILETIME ft = {};
    SystemTimeToFileTime(&st, &ft);
    return ULARGE_INTEGER{ ft.dwLowDateTime, ft.dwHighDateTime };
}

// ULARGE_INTEGER（100 ナノ秒単位）を SYSTEMTIME に変換する
static SYSTEMTIME uliToSystemTime(ULARGE_INTEGER uli) {
    FILETIME ft = { uli.LowPart, uli.HighPart };
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    return st;
}

// SYSTEMTIME を指定時間（100 ナノ秒単位）だけシフトする
static SYSTEMTIME shiftSystemTime(SYSTEMTIME st, long long offsetHns) {
    auto uli = systemTimeToUli(st);
    uli.QuadPart += offsetHns;
    return uliToSystemTime(uli);
}

// JST オフセット（100 ナノ秒単位で +9 時間）
static constexpr long long JST_OFFSET_HNS = 9LL * 60 * 60 * 10000000LL;

// 100 ナノ秒単位の 1 日（通算日の算出用）
static constexpr long long HNS_PER_DAY = 24LL * 60 * 60 * 10000000LL;

// UTC SYSTEMTIME を JST SYSTEMTIME に変換する
static SYSTEMTIME utcToJst(SYSTEMTIME st) { return shiftSystemTime(st, +JST_OFFSET_HNS); }

// JST の今日を YYYYMMDD の整数で返す（期限日との比較用）
// 日付だけの比較なので、時刻を持たない due_date と粒度が揃う。JST 固定は utcToJst と同じ方針。
static int todayJstYmd() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    st = utcToJst(st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

// 期限日の表示情報
struct DueDateView {
    std::wstring text;            // 今年は "7/28"、他年は "2025/6/30"（期限なし・解釈不能なら空）
    bool         overdue = false; // 期限 ≦ 今日（JST）＝期日、件名、経過日数を赤で描く
};

// Redmine の due_date（"YYYY-MM-DD"、期限なしは空）を表示情報へ変換する
// todayYmd は todayJstYmd() の値。呼び出し側で 1 回だけ求めて全行に使い、
// 一覧の途中で日付が変わって行ごとに判定が揺れることを防ぐ。
static DueDateView makeDueDateView(const std::string& due, int todayYmd) {
    DueDateView v;
    int y = 0, m = 0, d = 0;
    if (sscanf_s(due.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return v;
    if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31) return v;
    // 今年以外は年を添える。月日だけでは期日順に並べた一覧が順不同に見える。
    // （例：2025-12-22 の次に 2026-04-09 が来ると「12/22 → 4/9」と読める）
    // 年込みも月日と同じ書式に揃える。（区切りは "/"、ゼロパディングなし）
    v.text = std::to_wstring(m) + L"/" + std::to_wstring(d);
    if (y != todayYmd / 10000) v.text = std::to_wstring(y) + L"/" + v.text;
    v.overdue = (y * 10000 + m * 100 + d) <= todayYmd;
    return v;
}

// UTC の SYSTEMTIME を JST の通算日（1601-01-01 起点の日数）へ変換する
// カレンダー日付差の算出用。YYYYMMDD 整数と違い月またぎの引き算がそのまま日数になる。
static long long jstDaySerial(const SYSTEMTIME& utc) {
    return static_cast<long long>(systemTimeToUli(utcToJst(utc)).QuadPart / HNS_PER_DAY);
}

// JST の今日を通算日で返す（経過日数表示の基準日）
// 呼び出し側で 1 回だけ求めて全行に使い、一覧の途中で日付が変わって表示が揺れることを防ぐ。
static long long todayJstDaySerial() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    return jstDaySerial(st);
}

// updated_on（UTC ISO 8601）から一覧行末尾の経過日数表示を作る
// JST のカレンダー日付差で「（今日）」「（昨日）」「（x 日前）」を返す。
// 解釈不能・空（旧形式 pins.json 由来のピン等）は空文字列を返し、何も表示しない。
// クロックずれで未来になったら「（今日）」へ丸める。
static std::wstring makeUpdatedAgoText(const std::string& updatedOn, long long todayDays) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf_s(updatedOn.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
        return L"";
    SYSTEMTIME st = {};
    st.wYear   = static_cast<WORD>(y);
    st.wMonth  = static_cast<WORD>(mo);
    st.wDay    = static_cast<WORD>(d);
    st.wHour   = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    st.wSecond = static_cast<WORD>(s);
    // 実在しない日付（2/31 等）や範囲外の時分秒は変換失敗で弾く。systemTimeToUli は
    // 変換失敗を検知せずゼロを返すため、ここで検証しないと通算日 0 との差が
    // 桁外れの「x 日前」表示になる
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return L"";
    long long diff = todayDays - jstDaySerial(st);
    if (diff <= 0) return L"（今日）";
    if (diff == 1) return L"（昨日）";
    return L"（" + std::to_wstring(diff) + L" 日前）";
}

// 現在時刻を Redmine の updated_on と同形式の UTC ISO 8601 文字列で返す
// state.json の polled_on に記録し、次回ポーリングで「前回以降の更新か」を辞書順比較で判定する。
// ローカル時計とサーバ時計のずれはそのまま判定窓のずれになるが、秒〜分単位であり実害は無視できる。
static std::string nowUtcIso() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    sprintf_s(buf, "%04u-%02u-%02uT%02u:%02u:%02uZ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Toast XML の特殊文字をエスケープし、XML 1.0 が禁じる C0 制御文字を除去する
// 除去対象は 0x00〜0x1F のうち tab（0x09）・LF（0x0A）・CR（0x0D）を除く文字。
// 混入すると Toast XML パース失敗で通知全体が出なくなるため、ここで無音で落とす。
static std::wstring escapeXml(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size() + 16);
    for (wchar_t c : s) {
        switch (c) {
        case L'&':  r += L"&amp;";  break;
        case L'<':  r += L"&lt;";   break;
        case L'>':  r += L"&gt;";   break;
        case L'"':  r += L"&quot;"; break;
        default:
            if (c < 0x20 && c != L'\t' && c != L'\n' && c != L'\r') break;
            r += c;
            break;
        }
    }
    return r;
}

// https:// または http:// のみ許可する（任意プロトコルハンドラ悪用防止）
static bool isHttpUrl(const std::wstring& url) {
    return url.starts_with(L"https://") || url.starts_with(L"http://");
}

// UTF-8 std::string を UTF-16 std::wstring に変換する
// 変換 API が失敗（戻り値 0 以下）した場合は空文字列を返す。
// （n - 1 が size_t に負で渡ると巨大確保になり bad_alloc でスレッドが落ちるため）
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// UTF-16 std::wstring を UTF-8 std::string に変換する
// 変換 API が失敗（戻り値 0 以下）した場合は空文字列を返す。（理由は toWide と同じ）
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ==================== ログ出力 ====================

// ログディレクトリパス（初期化後に設定）
static std::wstring g_logDir;

// ログファイルに追記する
// g_logDir\YYYY-MM-DD.log に "YYYY-MM-DD HH:mm:ss msg\n" を書き込む
static void writeLog(const std::string& msg) {
    if (g_logDir.empty()) return;

    SYSTEMTIME st;
    GetSystemTime(&st);
    st = utcToJst(st);

    // 日付部分（ファイル名とタイムスタンプ共通）
    char dateBuf[12];
    sprintf_s(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    std::wstring path = g_logDir + L"\\" + toWide(dateBuf) + L".log";
    // FILE_APPEND_DATA でアトミックな末尾追記を保証する（SetFilePointer 不要）
    // FILE_SHARE_WRITE で再起動の一瞬だけ旧・新プロセスが並走しても SHARING_VIOLATION を防ぐ
    HANDLE hFile = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    char tsBuf[24];
    sprintf_s(tsBuf, sizeof(tsBuf), "%s %02d:%02d:%02d",
        dateBuf, st.wHour, st.wMinute, st.wSecond);

    std::string line = std::string(tsBuf) + " " + msg + "\n";
    DWORD written;
    WriteFile(hFile, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(hFile);
}

// schedule 配列と 1 日の概算ポーリング回数をログ出力する
static void logSchedule(const std::vector<int>& schedule) {
    int total = 0;
    std::string s = "schedule: [";
    for (size_t i = 0; i < schedule.size(); ++i) {
        if (i > 0) s += ',';
        s += std::to_string(schedule[i]);
        total += schedule[i];
    }
    s += "] (" + std::to_string(total) + " polls/day, 0 = idle hour)";
    writeLog(s);
}

// 次のポーリング予定時刻（時・分）を計算する共通ロジック
// 60/pollsPerHour 分間隔で正時 :00 起点。次の境界が 60 分以上に達したら翌時 00 分へ繰り上げる。
// 設定ロード側で [1, 60] にクランプ済みだが、ヘルパー単体での除算ゼロを防ぐためガードする。
static void calcNextPollTime(int pollsPerHour, int& outHour, int& outMin) {
    if (pollsPerHour <= 0) pollsPerHour = 1;
    SYSTEMTIME now;
    GetLocalTime(&now);
    int intervalMin = 60 / pollsPerHour;
    int nextMin = intervalMin * (now.wMinute / intervalMin + 1);
    int nextHour = now.wHour;
    if (nextMin >= 60) {
        nextMin  = 0;
        nextHour = (now.wHour + 1) % 24;
    }
    outHour = nextHour;
    outMin  = nextMin;
}

// 次のポーリング予定時刻を "HH:MM" 形式で返す
static std::string nextPollTimeStr(int pollsPerHour) {
    int h = 0, m = 0;
    calcNextPollTime(pollsPerHour, h, m);
    char buf[6];
    sprintf_s(buf, sizeof(buf), "%02d:%02d", h, m);
    return buf;
}

// 次のポーリング予定時刻までのスリープ時間（ms）を計算
// 正時 :00 起点で 60/pollsPerHour 分間隔の次の予定分までの残り時間を返す
static DWORD calcSleepUntilNextPoll(int pollsPerHour) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    int nextHour = 0, nextMin = 0;
    calcNextPollTime(pollsPerHour, nextHour, nextMin);
    // 翌時 00 分への繰り上がりは「現在時の 60 分時点」として扱う
    int targetMinFromNowHour = (nextHour == now.wHour) ? nextMin : 60;
    long long sleepMs = (long long)(targetMinFromNowHour - now.wMinute) * 60000LL
                        - (long long)now.wSecond * 1000LL
                        - (long long)now.wMilliseconds;
    if (sleepMs < 1000) sleepMs = 1000;
    return static_cast<DWORD>(sleepMs);
}

// ==================== HTTP ====================

// WinHTTP で HTTPS リクエストを実行する
// method: L"GET" or L"POST"
// authHeader: 空でなければ "名前: 値" 形式のヘッダ 1 行としてそのまま付与
// authHeader 付きのリクエストは自動リダイレクトを無効化し、3xx をそのままステータスとして返す。
// （WinHTTP の既定はカスタムヘッダを保持したまま追従するため、別ドメインへの 3xx で
// API キーが第三者に送られる。認証なしのリクエストは従来どおり追従する）
// outStatusCode が非 null の場合、最終 HTTP ステータスコードを書き込む
// （応答ヘッダ受信前の失敗は 0。ボディ受信途中の失敗はヘッダ確定値を保持して空ボディを返す）
static std::string httpRequest(const wchar_t* method, const std::wstring& url,
    const std::string& body, const wchar_t* contentType,
    const std::wstring& authHeader, DWORD* outStatusCode = nullptr)
{
    if (outStatusCode) *outStatusCode = 0;
    HINTERNET hSession = WinHttpOpen(L"redntfy/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    WinHttpSetTimeouts(hSession, 0, 15000, 30000, 30000);

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName     = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath      = path;
    uc.dwUrlPathLength  = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD reqFlags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // 認証ヘッダ付きは自動リダイレクトを禁止（API キー漏洩防止。関数コメント参照）
    if (!authHeader.empty()) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }

    // ヘッダ構築
    std::wstring headers;
    if (contentType && contentType[0]) headers += std::wstring(L"Content-Type: ") + contentType + L"\r\n";
    if (!authHeader.empty())           headers += authHeader + L"\r\n";

    auto* bodyData = body.empty() ? nullptr : static_cast<LPVOID>(const_cast<char*>(body.c_str()));
    auto  bodyLen  = static_cast<DWORD>(body.size());
    bool ok = WinHttpSendRequest(hRequest,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(-1),
        bodyData, bodyLen, bodyLen, 0)
        && WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (outStatusCode) *outStatusCode = statusCode;

    // ボディ受信（受信途中の失敗はボディ破棄で失敗扱い）
    // 受信 API の失敗で抜けると途切れたボディを正常応答として返してしまい、呼び出し側が
    // 部分 JSON を正当な応答と誤解釈しかねないため、失敗時は空ボディを返す。
    // status はヘッダで確定済みの値を保持する。（401/404 による設定不備の確定的判定を
    // 接続エラーへ格下げしないため。正常終端は QueryDataAvailable 成功かつ avail == 0）
    std::string respBody;
    std::vector<char> buf;
    bool readOk = true;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) { readOk = false; break; }
        if (avail == 0) break;
        if (buf.size() < avail) buf.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) { readOk = false; break; }
        respBody.append(buf.data(), read);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    if (!readOk) {
        writeLog("httpRequest: response body read failed: " + wideToUtf8(url)
                 + ", status=" + std::to_string(statusCode));
        return "";
    }
    return respBody;
}

// 汎用 GET（認証ヘッダなし。更新チェック等の外部 API 用）
static std::string httpGet(const std::wstring& url, DWORD* outStatusCode = nullptr) {
    return httpRequest(L"GET", url, "", nullptr, {}, outStatusCode);
}

// Redmine API の GET（API キーヘッダ付き）
static std::string redmineGet(const std::wstring& url, const std::wstring& apiKey,
    DWORD* outStatusCode = nullptr)
{
    return httpRequest(L"GET", url, "", nullptr,
                       L"X-Redmine-API-Key: " + apiKey, outStatusCode);
}

// ==================== 状態永続化 ====================

// JSON 文字列をアトミックにファイルへ書き出す（一時ファイル経由で MoveFileEx 置換）
// 電源断・クラッシュで本体ファイルが壊れる可能性を避ける。
// 一時ファイル名にはスレッド id を含める。pins.json はメインスレッド（cycleIssueState）と
// ポーリングスレッド（refreshPins）から並行して保存され得るため、固定名 ".tmp" だと
// 片方の書き込み途中をもう片方が切り詰め、壊れた JSON が本体へ公開される窓がある。
// logTag はエラー出力用の識別子（"state" / "pins" 等）。成功時 true、失敗時 false。
static bool atomicWriteJson(const std::wstring& path, const std::string& json,
    const char* logTag)
{
    auto tmpPath = path + L".tmp" + std::to_wstring(GetCurrentThreadId());
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        writeLog(std::string(logTag) + ": save failed (CreateFileW error "
            + std::to_string(GetLastError()) + ")");
        return false;
    }
    DWORD written = 0;
    BOOL  writeOk  = WriteFile(hFile, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    DWORD writeErr = writeOk ? 0 : GetLastError();
    BOOL  flushOk  = TRUE;
    DWORD flushErr = 0;
    if (writeOk && written == static_cast<DWORD>(json.size())) {
        flushOk = FlushFileBuffers(hFile);
        if (!flushOk) flushErr = GetLastError();
    }
    CloseHandle(hFile);
    if (!writeOk || written != static_cast<DWORD>(json.size())) {
        writeLog(std::string(logTag) + ": write failed ("
            + std::to_string(written) + "/" + std::to_string(json.size())
            + " bytes, error " + std::to_string(writeErr) + ")");
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    if (!flushOk) {
        writeLog(std::string(logTag) + ": flush failed (error "
            + std::to_string(flushErr) + ")");
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        writeLog(std::string(logTag) + ": rename failed (error "
            + std::to_string(GetLastError()) + ")");
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

// JSON ファイルを丸ごと読み込む
// ファイル不在は正常系（nullopt を静かに返す）。サイズ 0・maxBytes 超・読み込み失敗は
// 破損とみなして nullopt を返す。（logTag 付きでログに残す）
static std::optional<std::string> readJsonFile(const std::wstring& path, const char* logTag,
    DWORD maxBytes = 1024 * 1024)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return std::nullopt;

    SetLastError(0);
    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0 || fileSize > maxBytes) {
        CloseHandle(hFile);
        writeLog(std::string(logTag) + ": unexpected file size");
        return std::nullopt;
    }
    std::string buf(fileSize, '\0');
    DWORD readBytes = 0;
    BOOL ok = ReadFile(hFile, buf.data(), fileSize, &readBytes, nullptr);
    CloseHandle(hFile);
    if (!ok || readBytes != fileSize) {
        writeLog(std::string(logTag) + ": read failed ("
            + std::to_string(readBytes) + "/" + std::to_string(fileSize) + " bytes)");
        return std::nullopt;
    }
    return buf;
}

// state.json の issues 要素 1 件分（前回ポーリング時点の検知済み状態）
struct StateEntry {
    std::string      updatedOn;
    std::vector<int> queryIds;           // 前回の所属クエリ id（昇順）
    bool             hasQueries = false; // queries キーがあったか（旧形式 v1 の判別）
    // 最終更新者のキャッシュ（updated_on が変わっていなければ journals を引き直さないため）
    int              updaterId = 0;
    std::string      updaterDisplay;
    std::string      updaterFirst;  // 名（旧形式はキー不在＝空で読まれ、チケット更新時に埋まる）
};

// state.json の読み込み結果
struct PollState {
    bool                                baseline = false; // ベースライン確立済みか
    std::unordered_map<int, StateEntry> issues;
    std::vector<int>                    knownQueries;     // 前回追跡していたクエリ id（昇順）
    std::string                         polledOn;         // 前回ポーリング時刻（UTC ISO 8601。旧形式は空）
};

// 検知済み状態の読み込み
// 戻り値の baseline はベースライン確立済みか。ファイル不在・パースエラーは未確立として扱い、
// 次回ポーリングで通知なしのベースライン再確立が走る。（誤通知より通知欠落側に倒す）
//
// v1（queries なし）互換：エントリの queries 欠落は hasQueries=false とし、そのエントリの
// クエリ流入検知を見送り、現所属を通知なしで採用する。ルートの queries 欠落は knownQueries を
// 空にし、全クエリを「今回追加されたクエリ」として流入検知の対象外にする。
// どちらも、バージョンアップや設定追加の直後に既存全チケットが「更新」通知になる嵐を防ぐため。
static PollState loadState(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    PollState st;
    // state.json は追跡集合の件数に比例して育つため、書き込み側と非対称にならないよう
    // 上限を 16MB（v2 は 1 件約 110 バイトで約 14 万件相当）まで広げる
    auto buf = readJsonFile(dir + L"\\" + STATE_FILENAME, "state", 16u * 1024 * 1024);
    if (!buf) return st;
    try {
        auto obj = JsonObject::Parse(winrt::to_hstring(*buf));
        st.baseline = obj.GetNamedBoolean(L"baseline", false);
        st.polledOn = winrt::to_string(obj.GetNamedString(L"polled_on", L""));
        if (obj.HasKey(L"queries")) {
            for (auto q : obj.GetNamedArray(L"queries")) {
                int qid = static_cast<int>(q.GetNumber());
                // 0 はフォールバックモードの擬似クエリ（FALLBACK_QUERY_ID）として有効
                if (qid >= 0) st.knownQueries.push_back(qid);
            }
            std::sort(st.knownQueries.begin(), st.knownQueries.end());
        }
        if (obj.HasKey(L"issues")) {
            for (auto item : obj.GetNamedArray(L"issues")) {
                auto o  = item.GetObject();
                int  id = static_cast<int>(o.GetNamedNumber(L"id", 0));
                auto up = winrt::to_string(o.GetNamedString(L"updated_on", L""));
                if (id <= 0 || up.empty()) continue;
                StateEntry e;
                e.updatedOn      = std::move(up);
                e.updaterId      = static_cast<int>(o.GetNamedNumber(L"updater_id", 0));
                e.updaterDisplay = winrt::to_string(o.GetNamedString(L"updater", L""));
                e.updaterFirst   = winrt::to_string(o.GetNamedString(L"updater_first", L""));
                if (o.HasKey(L"queries")) {
                    e.hasQueries = true;
                    for (auto q : o.GetNamedArray(L"queries")) {
                        int qid = static_cast<int>(q.GetNumber());
                        // 0 はフォールバックモードの擬似クエリ（FALLBACK_QUERY_ID）として有効
                        if (qid >= 0) e.queryIds.push_back(qid);
                    }
                    std::sort(e.queryIds.begin(), e.queryIds.end());
                }
                st.issues.emplace(id, std::move(e));
            }
        }
        return st;
    }
    catch (...) {
        writeLog("state: parse failed, re-establishing baseline");
        return PollState{};  // 破損時はベースライン未確立として通知せず作り直す
    }
}

// 今回追跡するクエリ id 集合を返す
// query_ids 省略（フォールバックモード）時は擬似クエリ FALLBACK_QUERY_ID の 1 件とし、
// state.json の queries・新規追跡ログを通常クエリと同じ仕組みで扱えるようにする。
static std::vector<int> trackedQueryIds(const Config& cfg) {
    if (cfg.queryIds.empty()) return {FALLBACK_QUERY_ID};
    return cfg.queryIds;
}

// 検知済み状態の保存
// ポーリング成功のたびに追跡集合全体で上書きする。（集合から消えた id は自然に落ちる）
// baseline フラグを明示するのは、保存クエリが正常に 0 件を返した状態と初回起動を区別するため。
// version と queries（今回追跡したクエリ id）を持つのは、次回に旧形式からの移行と
// query_ids へのクエリ追加を検出して通知の嵐を防ぐため。
// 戻り値は保存成否。書き込み不能環境ではログも残せない可能性があるため、呼び出し側が
// 失敗を Toast でユーザに知らせる。
static bool saveState(const std::wstring& dir, const Config& cfg, const std::vector<Issue>& issues) {
    using namespace winrt::Windows::Data::Json;
    try {
        JsonObject root;
        root.Insert(L"version",  JsonValue::CreateNumberValue(2));
        root.Insert(L"baseline", JsonValue::CreateBooleanValue(true));
        root.Insert(L"polled_on", JsonValue::CreateStringValue(winrt::to_hstring(nowUtcIso())));
        JsonArray qarr;
        for (int q : trackedQueryIds(cfg)) qarr.Append(JsonValue::CreateNumberValue(q));
        root.Insert(L"queries", qarr);
        JsonArray arr;
        for (const auto& is : issues) {
            JsonObject o;
            o.Insert(L"id",         JsonValue::CreateNumberValue(is.id));
            o.Insert(L"updated_on", JsonValue::CreateStringValue(winrt::to_hstring(is.updatedOn)));
            o.Insert(L"updater_id", JsonValue::CreateNumberValue(is.updaterId));
            o.Insert(L"updater",    JsonValue::CreateStringValue(winrt::to_hstring(is.updaterDisplay)));
            o.Insert(L"updater_first", JsonValue::CreateStringValue(winrt::to_hstring(is.updaterFirstName)));
            JsonArray iq;
            for (int q : is.queryIds) iq.Append(JsonValue::CreateNumberValue(q));
            o.Insert(L"queries", iq);
            arr.Append(o);
        }
        root.Insert(L"issues", arr);
        return atomicWriteJson(dir + L"\\" + STATE_FILENAME, winrt::to_string(root.Stringify()), "state");
    }
    catch (...) {
        writeLog("state: save failed (exception)");
        return false;
    }
}

// ピン留めの保存
// トグル操作とポーリング時の鮮度更新のたびに g_pins を上書き保存する。g_mtx ロック外で呼ぶこと。
static void savePins(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    try {
        JsonArray arr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (const auto& p : g_pins) {
                JsonObject o;
                o.Insert(L"id",         JsonValue::CreateNumberValue(p.id));
                o.Insert(L"subject",    JsonValue::CreateStringValue(winrt::to_hstring(p.subject)));
                o.Insert(L"project",    JsonValue::CreateStringValue(winrt::to_hstring(p.projectName)));
                o.Insert(L"updated_on", JsonValue::CreateStringValue(winrt::to_hstring(p.updatedOn)));
                o.Insert(L"closed",     JsonValue::CreateBooleanValue(p.closed));
                o.Insert(L"due_date",   JsonValue::CreateStringValue(winrt::to_hstring(p.dueDate)));
                o.Insert(L"assigned_to_group", JsonValue::CreateBooleanValue(p.assignedToGroup));
                o.Insert(L"bug_tracker", JsonValue::CreateBooleanValue(p.isBugTracker));
                o.Insert(L"updater",    JsonValue::CreateStringValue(winrt::to_hstring(p.updaterDisplay)));
                o.Insert(L"updater_first", JsonValue::CreateStringValue(winrt::to_hstring(p.updaterFirst)));
                arr.Append(o);
            }
        }
        atomicWriteJson(dir + L"\\" + PINS_FILENAME, winrt::to_string(arr.Stringify()), "pins");
    }
    catch (...) {
        writeLog("pins: save failed (exception)");
    }
}

// 表示用文字列の全角空白（U+3000、UTF-8 では E3 80 80）を半角空白に置換する
// Toast と一覧の表示幅を無駄にしないため、取込時点で正規化する。
static std::string normalizeSpaces(std::string s) {
    size_t pos = 0;
    while ((pos = s.find("\xE3\x80\x80", pos)) != std::string::npos) {
        s.replace(pos, 3, " ");
        ++pos;
    }
    return s;
}

// ピン留めの読み込み
// 起動時に 1 回呼び出し、g_pins へ復元する。（件数の上限はない）
// ファイル不在・パースエラー時は何もしない。（ピンなしで開始する）
static void loadPins(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    auto buf = readJsonFile(dir + L"\\" + PINS_FILENAME, "pins");
    if (!buf) return;
    try {
        auto arr = JsonArray::Parse(winrt::to_hstring(*buf));
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pins.clear();
        for (auto item : arr) {
            auto o = item.GetObject();
            PinEntry p;
            p.id        = static_cast<int>(o.GetNamedNumber(L"id", 0));
            p.subject   = normalizeSpaces(winrt::to_string(o.GetNamedString(L"subject", L"")));
            p.updatedOn = winrt::to_string(o.GetNamedString(L"updated_on", L""));
            p.closed    = o.GetNamedBoolean(L"closed", false);
            // 旧形式（キーなし）は既定値で開始し、次のポーリングの refreshPins で実値になる
            p.dueDate   = winrt::to_string(o.GetNamedString(L"due_date", L""));
            p.projectName     = normalizeSpaces(winrt::to_string(o.GetNamedString(L"project", L"")));
            p.assignedToGroup = o.GetNamedBoolean(L"assigned_to_group", false);
            p.isBugTracker    = o.GetNamedBoolean(L"bug_tracker", false);
            // updater は集合内ピンのみ refreshPins で実値になる。（集合外ピンの個別取得は
            // journals を含まないため、保存値のまま維持される）
            p.updaterDisplay  = winrt::to_string(o.GetNamedString(L"updater", L""));
            p.updaterFirst    = winrt::to_string(o.GetNamedString(L"updater_first", L""));
            if (p.id > 0) g_pins.push_back(std::move(p));
        }
        writeLog("pins: loaded " + std::to_string(g_pins.size()) + " entries");
    }
    catch (...) {
        writeLog("pins: load failed (exception)");
    }
}

// 非表示チケットの保存
// 右クリックの状態遷移とポーリング時の自動削除（pruneHidden）のたびに g_hiddenIds を
// id の JSON 配列で上書き保存する。g_mtx ロック外で呼ぶこと。
static void saveHidden(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    try {
        JsonArray arr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (int id : g_hiddenIds) arr.Append(JsonValue::CreateNumberValue(id));
        }
        atomicWriteJson(dir + L"\\" + HIDDEN_FILENAME, winrt::to_string(arr.Stringify()), "hidden");
    }
    catch (...) {
        writeLog("hidden: save failed (exception)");
    }
}

// 非表示チケットの読み込み
// 起動時に 1 回呼び出し、g_hiddenIds へ復元する。id が正の数値だけを採用する。
// ファイル不在・パースエラー時は何もしない。（非表示なしで開始する）
static void loadHidden(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    auto buf = readJsonFile(dir + L"\\" + HIDDEN_FILENAME, "hidden");
    if (!buf) return;
    try {
        auto arr = JsonArray::Parse(winrt::to_hstring(*buf));
        std::lock_guard<std::mutex> lk(g_mtx);
        g_hiddenIds.clear();
        for (auto item : arr) {
            int id = static_cast<int>(item.GetNumber());
            if (id > 0) g_hiddenIds.insert(id);
        }
        writeLog("hidden: loaded " + std::to_string(g_hiddenIds.size()) + " entries");
    }
    catch (...) {
        writeLog("hidden: load failed (exception)");
    }
}

// ==================== 設定読み込み ====================

// list_format 文字列をトークン列へ解析する
// {要素名} または {要素名:最大文字数} を認識する。（最大文字数は 1 以上の 10 進整数のみ有効）
// 解釈できない部分（未知の要素名・不正な長さ・閉じ括弧なし）はリテラルとして原文のまま残し、
// ログに記録する。誤記が一覧にそのまま現れるため、ユーザが確実に気付ける。
static std::vector<FormatToken> parseListFormat(const std::wstring& fmt) {
    static const std::pair<const wchar_t*, int> NAMES[] = {
        { L"id", FMT_ID }, { L"lastname", FMT_LASTNAME }, { L"firstname", FMT_FIRSTNAME },
        { L"group", FMT_GROUP }, { L"project", FMT_PROJECT }, { L"due", FMT_DUE },
        { L"bug", FMT_BUG }, { L"subject", FMT_SUBJECT }, { L"ago", FMT_AGO },
    };
    std::vector<FormatToken> tokens;
    std::wstring lit;  // 連続するリテラル文字の蓄積（プレースホルダ確定時にトークン化する）
    auto flushLiteral = [&]() {
        if (!lit.empty()) {
            tokens.push_back({.literal = std::move(lit)});
            lit.clear();
        }
    };
    size_t pos = 0;
    while (pos < fmt.size()) {
        if (fmt[pos] != L'{') {
            lit += fmt[pos++];
            continue;
        }
        size_t close = fmt.find(L'}', pos);
        if (close == std::wstring::npos) {
            // 閉じ括弧なし。残り全部をリテラルへ
            lit += fmt.substr(pos);
            break;
        }
        std::wstring inner = fmt.substr(pos + 1, close - pos - 1);
        std::wstring name  = inner;
        int maxChars = 0;
        bool valid   = true;
        if (auto colon = inner.find(L':'); colon != std::wstring::npos) {
            name = inner.substr(0, colon);
            std::wstring len = inner.substr(colon + 1);
            // 桁数を 4 桁までに抑えて stoi の桁あふれも防ぐ（実用上は 3 桁で十分）
            valid = !len.empty() && len.size() <= 4
                && std::all_of(len.begin(), len.end(),
                               [](wchar_t c) { return c >= L'0' && c <= L'9'; });
            if (valid) {
                maxChars = std::stoi(len);
                valid = maxChars >= 1;
            }
        }
        int element = FMT_LITERAL;
        if (valid) {
            valid = false;
            for (const auto& [n, e] : NAMES) {
                if (name == n) {
                    element = e;
                    valid   = true;
                    break;
                }
            }
        }
        if (valid) {
            flushLiteral();
            tokens.push_back({.element = element, .maxChars = maxChars});
        }
        else {
            std::wstring raw = fmt.substr(pos, close - pos + 1);
            writeLog("config: list_format unrecognized placeholder: " + wideToUtf8(raw));
            lit += raw;
        }
        pos = close + 1;
    }
    flushLiteral();
    return tokens;
}

// TOML ファイルをパースして table を返す（ファイル不在・パースエラーは nullopt）
static std::optional<toml::table> loadToml(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return std::nullopt;
    try {
        return toml::parse_file(path);
    }
    catch (const toml::parse_error& e) {
        writeLog("TOML parse error in " + wideToUtf8(path)
            + ": " + std::string(e.description()));
        return std::nullopt;
    }
}

// schedule 配列を TOML テーブルから読み込む（なければ nullopt）
// 各要素は [0, 60] にクランプする。（0 = その時間帯は休止）
static std::optional<std::vector<int>> readSchedule(const std::optional<toml::table>& tbl) {
    if (!tbl) return std::nullopt;
    const auto* arr = (*tbl)["app"]["schedule"].as_array();
    if (!arr) return std::nullopt;
    std::vector<int> sched;
    for (const auto& el : *arr) {
        if (sched.size() >= 24) break;
        sched.push_back((std::min)(60, (std::max)(0, el.value_or(1))));
    }
    while (sched.size() < 24) sched.push_back(1);
    return sched;
}

// redntfy.toml と redntfy.local.toml を読み込んで Config を構築する
//
// local.toml のキーが優先。（キー単位でオーバーライド）
// 配列は「local にキーが存在するか」で採否を決める。（空配列でも local を採用）
// 結果が空なら base に戻す方式だと local から base の設定を無効化できないため。
static Config loadConfig(const std::wstring& exeDir) {
    auto base  = loadToml(exeDir + L"\\" + CONFIG_FILENAME);
    auto local = loadToml(exeDir + L"\\" + CONFIG_LOCAL_FILENAME);
    if (local) writeLog("Loaded redntfy.local.toml (override active)");

    // 文字列配列の読み込み（キー不在は nullopt を返し、空配列との区別に使う）
    // [app] セクション下のキーだけを対象とする。（トップレベルへの記述は無視する）
    auto readStrArray = [](const std::optional<toml::table>& tbl, const char* key)
            -> std::optional<std::vector<std::string>> {
        if (!tbl) return std::nullopt;
        const auto* arr = (*tbl)["app"][key].as_array();
        if (!arr) return std::nullopt;
        std::vector<std::string> out;
        for (const auto& el : *arr) {
            if (auto s = el.value<std::string>()) out.push_back(*s);
        }
        return out;
    };
    // キーが存在する側を採用する（local 優先。空配列でも local を採る）
    auto pickStrArray = [&](const char* key) -> std::vector<std::string> {
        if (auto v = readStrArray(local, key)) return *v;
        if (auto v = readStrArray(base, key))  return *v;
        return {};
    };

    Config cfg;
    if (auto s = readSchedule(local)) {
        cfg.schedule = std::move(*s);
    }
    else if (auto s = readSchedule(base)) {
        cfg.schedule = std::move(*s);
    }
    else {
        cfg.schedule.resize(24, 1);
    }

    cfg.bugTrackers = pickStrArray("bug_trackers");
    // ダッキング対象はプロセス名の比較に wstring を使うため、ここで変換する
    for (const auto& s : pickStrArray("duck_targets")) cfg.duckTargets.push_back(toWide(s));

    // スカラー読み込みヘルパー（local → base → デフォルトの優先順）
    auto readConfigBool = [&](const char* section, const char* key, bool def) -> bool {
        if (local && (*local)[section][key].is_boolean()) return **(*local)[section][key].as_boolean();
        if (base && (*base)[section][key].is_boolean())   return **(*base)[section][key].as_boolean();
        return def;
    };
    auto readConfigFloat = [&](const char* section, const char* key, float def, float lo, float hi) -> float {
        double v = def;
        if (local && (*local)[section][key].is_number()) v = (*local)[section][key].value_or(def);
        else if (base && (*base)[section][key].is_number()) v = (*base)[section][key].value_or(def);
        return static_cast<float>((std::max)((double)lo, (std::min)((double)hi, v)));
    };
    // [app] セクション下の整数
    auto readAppInt = [&](const char* key, int def, int lo, int hi) -> int {
        long long v = def;
        if (local && (*local)["app"][key].is_integer())      v = **(*local)["app"][key].as_integer();
        else if (base && (*base)["app"][key].is_integer())   v = **(*base)["app"][key].as_integer();
        return static_cast<int>((std::max)((long long)lo, (std::min)((long long)hi, v)));
    };
    // [app] セクション下の文字列
    auto readAppString = [&](const char* key) -> std::wstring {
        if (local && (*local)["app"][key].is_string()) return toWide(**(*local)["app"][key].as_string());
        if (base && (*base)["app"][key].is_string())   return toWide(**(*base)["app"][key].as_string());
        return {};
    };
    // [redmine] セクションの文字列・整数
    auto readRedmineString = [&](const char* key) -> std::wstring {
        if (local && (*local)["redmine"][key].is_string()) return toWide(**(*local)["redmine"][key].as_string());
        if (base && (*base)["redmine"][key].is_string())   return toWide(**(*base)["redmine"][key].as_string());
        return {};
    };
    // [redmine] の整数配列（query_ids）
    // 配列は「local にキーが存在するか」で採否を決める。（duck_targets / schedule と同じ方針）
    // 0 以下・非整数の要素と重複 id は除外する。（同じ HTTP を 2 回叩かない）
    auto readRedmineIntArray = [&](const std::optional<toml::table>& tbl, const char* key)
            -> std::optional<std::vector<int>> {
        if (!tbl) return std::nullopt;
        const auto* arr = (*tbl)["redmine"][key].as_array();
        if (!arr) return std::nullopt;
        std::vector<int> ids;
        for (const auto& el : *arr) {
            auto v = el.value<int64_t>();
            if (!v || *v <= 0 || *v > INT_MAX) continue;
            int id = static_cast<int>(*v);
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        return ids;
    };

    // [redmine] 接続設定（url・api_key の欠落チェックは wmain で行い、欠けていれば無効モードで常駐する。
    // query_ids は省略可でフォールバックモードになる）
    cfg.redmineUrl = readRedmineString("url");
    while (!cfg.redmineUrl.empty() && cfg.redmineUrl.back() == L'/')
        cfg.redmineUrl.pop_back();  // 末尾スラッシュを除去して URL 連結を単純化する
    cfg.apiKey  = readRedmineString("api_key");
    if (auto q = readRedmineIntArray(local, "query_ids"))     cfg.queryIds = std::move(*q);
    else if (auto q = readRedmineIntArray(base, "query_ids")) cfg.queryIds = std::move(*q);

    // 旧キー query_id は廃止。設定エラー Toast だけでは移行漏れの原因が分からないためログで補う
    if ((local && (*local)["redmine"]["query_id"].is_integer())
        || (base && (*base)["redmine"]["query_id"].is_integer())) {
        writeLog("config: [redmine] query_id is obsolete; use query_ids = [12, 34]");
    }

    // 一覧の表示件数と行フォーマット
    cfg.listLimit = readAppInt("list_limit", 20, 1, 25);

    // 一覧の行フォーマット（空・未指定は既定テンプレート）
    std::wstring fmtStr = readAppString("list_format");
    if (fmtStr.empty()) fmtStr = LIST_FORMAT_DEFAULT;
    cfg.listFormat = parseListFormat(fmtStr);

    // 旧キーは廃止。読み捨てられていることをログで補う（query_id の前例に倣う）
    for (const char* obsolete : { "subject_max_chars", "project_max_chars" }) {
        if ((local && (*local)["app"][obsolete].is_integer())
            || (base && (*base)["app"][obsolete].is_integer())) {
            writeLog(std::string("config: [app] ") + obsolete
                + " is obsolete; use list_format placeholders like {subject:40}");
        }
    }

    // バージョン欄判定情報の再取得間隔（時間）
    cfg.versionMetaRefreshHours = readAppInt("version_meta_refresh_hours", 24, 1, 168);

    // ホバーで一覧を表示するまでの遅延（ms 単位。デフォルト 100、0〜5000 にクランプ、0 で即時）
    cfg.hoverDelayMs = readAppInt("hover_delay_ms",
        static_cast<int>(DEFAULT_HOVER_DELAY_MS),
        static_cast<int>(MIN_HOVER_DELAY_MS),
        static_cast<int>(MAX_HOVER_DELAY_MS));

    // ホバー自動表示直後に左クリックを無視する猶予（ms 単位。デフォルト 300、0〜5000 にクランプ、0 で無効）
    cfg.hoverClickGuardMs = readAppInt("hover_click_guard_ms",
        static_cast<int>(DEFAULT_HOVER_CLICK_GUARD_MS),
        static_cast<int>(MIN_HOVER_CLICK_GUARD_MS),
        static_cast<int>(MAX_HOVER_CLICK_GUARD_MS));

    // [guard] ガードトーン設定
    cfg.guardToneMs = (int)readConfigFloat("guard", "tone_ms", 1500.0f, 0.0f, 10000.0f);

    // [loudness] ラウドネスノーマライズ設定
    cfg.loudnessEnabled     = readConfigBool("loudness", "enabled", true);
    cfg.loudnessTarget      = readConfigFloat("loudness", "target", -16.0f, -60.0f, 0.0f);
    cfg.loudnessPeakCeiling = readConfigFloat("loudness", "peak_ceiling", 0.891f, 0.1f, 1.0f);

    // [update] 更新チェック設定
    cfg.updateCheckEnabled = readConfigBool("update", "enabled", true);

    return cfg;
}

// ==================== Redmine API ====================

// チケット画面の URL（{url}/issues/{id}。API 用途は末尾に ".json" 等を連結する）
static std::wstring issueUrl(const Config& cfg, int id) {
    return cfg.redmineUrl + L"/issues/" + std::to_wstring(id);
}

// 代表画面の URL（複数件 Toast と一覧フッタの遷移先）
// 通常は代表クエリ（query_ids の先頭）の画面。和集合を表す URL は Redmine に無いため先頭で代表する。
// フォールバックモード（query_ids 空）は担当フィルタ付き一覧。Web 側も me を
// 自分＋所属グループに展開するため、アプリの表示集合と同じ範囲が開く。
static std::wstring queryUrl(const Config& cfg) {
    if (cfg.queryIds.empty())
        return cfg.redmineUrl + L"/issues?set_filter=1&assigned_to_id=me";
    return cfg.redmineUrl + L"/issues?query_id=" + std::to_wstring(cfg.queryIds.front());
}

// ワイルドカード照合（`*` は 0 文字以上の任意列に一致する）
// トラッカー名にリテラルの `*` は来ない前提のため、エスケープ機構は持たない。
// UTF-8 のバイト単位で比較する。`*` はマルチバイト文字の継続バイトに現れないため、
// バイト単位でも文字境界を跨いだ誤一致は起きない。
// PathMatchSpecW を使わないのは、ファイル名向けの MS-DOS 由来の特例を持ち込まないため。
static bool matchWildcard(const std::string& text, const std::string& pattern) {
    size_t t = 0, p = 0, mark = 0;
    size_t star = std::string::npos;  // 直近に読んだ `*` の位置（npos = まだ無い）
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == text[t]) {
            ++t;
            ++p;
        }
        else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        }
        else if (star != std::string::npos) {
            // 直近の `*` に 1 文字多く食わせてやり直す（後戻り）
            p = star + 1;
            t = ++mark;
        }
        else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// トラッカー名が bug_trackers のいずれかに一致するか（一覧の 💥 マーカー用）
// 判定材料を引数で受ける形は isGroupAssignee と揃える。
static bool matchesBugTracker(const std::vector<std::string>& patterns, const std::string& name) {
    return std::any_of(patterns.begin(), patterns.end(),
                       [&](const std::string& pat) { return matchWildcard(name, pat); });
}

// issue JSON オブジェクトを Issue に変換する
// status.is_closed はキー不在を「未クローズ」として扱う。（Redmine の版による差異対策）
static Issue parseIssueObject(const Config& cfg,
                              const winrt::Windows::Data::Json::JsonObject& obj) {
    using namespace winrt::Windows::Data::Json;
    Issue is;
    is.id        = static_cast<int>(obj.GetNamedNumber(L"id", 0));
    is.subject   = normalizeSpaces(winrt::to_string(obj.GetNamedString(L"subject", L"")));
    is.updatedOn = winrt::to_string(obj.GetNamedString(L"updated_on", L""));
    if (obj.HasKey(L"author")) {
        auto author = obj.GetNamedObject(L"author", nullptr);
        if (author) {
            is.authorId   = static_cast<int>(author.GetNamedNumber(L"id", 0));
            is.authorName = winrt::to_string(author.GetNamedString(L"name", L""));
        }
    }
    // project は include 指定なしでも issues.json と issues/{id}.json の双方に常に含まれる。
    // name は一覧表示用、id はバージョン欄の有無判定用。
    if (obj.HasKey(L"project")) {
        auto proj = obj.GetNamedObject(L"project", nullptr);
        if (proj) {
            is.projectName = normalizeSpaces(winrt::to_string(proj.GetNamedString(L"name", L"")));
            is.projectId   = static_cast<int>(proj.GetNamedNumber(L"id", 0));
        }
    }
    // tracker も include 指定なしで常に含まれる。
    // 名前は bug_trackers 照合だけに使い残さない。id はバージョン欄の有無判定用。
    if (obj.HasKey(L"tracker")) {
        auto tracker = obj.GetNamedObject(L"tracker", nullptr);
        if (tracker) {
            is.isBugTracker = matchesBugTracker(
                cfg.bugTrackers, winrt::to_string(tracker.GetNamedString(L"name", L"")));
            is.trackerId = static_cast<int>(tracker.GetNamedNumber(L"id", 0));
        }
    }
    // assigned_to はキー自体が無ければ未割当。ユーザとグループで形は同じ。（id と name のみ）
    if (obj.HasKey(L"assigned_to")) {
        auto assignee = obj.GetNamedObject(L"assigned_to", nullptr);
        if (assignee) is.assignedToId = static_cast<int>(assignee.GetNamedNumber(L"id", 0));
    }
    // fixed_version は include 指定なしでも常に返り、未設定ならキー自体が現れない。
    // 除外判定にしか使わないため name は保持せず、有無だけを持つ。
    if (obj.HasKey(L"fixed_version")) {
        auto ver = obj.GetNamedObject(L"fixed_version", nullptr);
        if (ver) is.hasFixedVersion = true;
    }
    // closed 判定は status.is_closed を根拠とする。closed_on は再オープン時に null へ
    // 戻らない（過去のクローズ日時として残る）ため、判定に使うと再開後も取消線が残る。
    if (obj.HasKey(L"status")) {
        auto st = obj.GetNamedObject(L"status", nullptr);
        if (st) is.closed = st.GetNamedBoolean(L"is_closed", false);
    }
    // due_date はキー不在と null（期限なし）の両方を空として扱う。
    // GetNamedString は値が null のとき例外になるため、GetNamedValue で型を確かめてから読む。
    if (obj.HasKey(L"due_date")) {
        auto v = obj.GetNamedValue(L"due_date", nullptr);
        if (v && v.ValueType() == JsonValueType::String) is.dueDate = winrt::to_string(v.GetString());
    }
    return is;
}

// Redmine API へ GET し JSON オブジェクトを得る（取得系関数の共通骨格）
// 非 200・空応答・パース失敗は nullptr を返し、logTag 付きの失敗ログを残す。
// outStatus は省略可。HTTP ステータスをそのまま書き込む。（401・403・404 の判別用）
// パース成功後のフィールド抽出は本関数の範囲外。キー欠落はデフォルト値アクセスで安全に扱える。
// 型不一致は WinRT が例外を投げるため、配列走査などを行う呼び出し側は従来どおり
// try-catch で囲む。
static winrt::Windows::Data::Json::JsonObject redmineGetJson(
    const Config& cfg, const std::wstring& url, const std::string& logTag,
    DWORD* outStatus = nullptr)
{
    DWORD status = 0;
    auto body = redmineGet(url, cfg.apiKey, &status);
    if (outStatus) *outStatus = status;
    if (status != 200 || body.empty()) {
        writeLog(logTag + ": request failed, status=" + std::to_string(status));
        return nullptr;
    }
    try {
        return winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
    }
    catch (...) {
        writeLog(logTag + ": JSON parse failed");
        return nullptr;
    }
}

// /users/current.json から自分の user id と所属グループ id を取得する（起動時 1 回）
// 失敗時は 0 を返す。0 のときは自分の操作の除外判定を行わない。（通知欠落より過剰通知側に倒す）
// outOwnGroups は成功時のみ上書きする。グループ担当判定（/groups.json）が権限不足で
// 使えない場合のフォールバック用。
static int fetchMyUserId(const Config& cfg, std::vector<int>& outOwnGroups) {
    auto obj = redmineGetJson(cfg, cfg.redmineUrl + L"/users/current.json?include=groups",
                              "fetchMyUserId");
    if (!obj) return 0;
    try {
        auto user = obj.GetNamedObject(L"user", nullptr);
        if (!user) return 0;
        if (user.HasKey(L"groups")) {
            outOwnGroups.clear();
            for (auto g : user.GetNamedArray(L"groups")) {
                int gid = static_cast<int>(g.GetObject().GetNamedNumber(L"id", 0));
                if (gid > 0) outOwnGroups.push_back(gid);
            }
        }
        return static_cast<int>(user.GetNamedNumber(L"id", 0));
    }
    catch (...) {
        writeLog("fetchMyUserId: unexpected JSON structure");
        return 0;
    }
}

// /groups.json から全グループの id を取得する（グループ担当マーカーの判定用、起動時 1 回）
// この API は admin 権限が必要。403（権限なし）は確定的な失敗として outStatus で呼び出し側に
// 伝え、所属グループへのフォールバックを促す。接続エラー等は nullopt で再試行対象とする。
// この API はページングされず全グループを一括で返す。（groups_controller.rb の
// format.api が scope.to_a で全件取得するため。limit/offset 対応は API 側に無い）
static std::optional<std::vector<int>> fetchAllGroupIds(const Config& cfg, DWORD* outStatus) {
    auto obj = redmineGetJson(cfg, cfg.redmineUrl + L"/groups.json", "fetchAllGroupIds", outStatus);
    if (!obj) return std::nullopt;
    try {
        std::vector<int> ids;
        if (obj.HasKey(L"groups")) {
            for (auto g : obj.GetNamedArray(L"groups")) {
                int gid = static_cast<int>(g.GetObject().GetNamedNumber(L"id", 0));
                if (gid > 0) ids.push_back(gid);
            }
        }
        return ids;
    }
    catch (...) {
        writeLog("fetchAllGroupIds: unexpected JSON structure");
        return std::nullopt;
    }
}

// 担当者 id がグループかどうかを判定する（一覧の 👥 マーカー用）
// groupIds は起動時に確定した判定集合。（全グループ、または権限不足時は自分の所属グループ）
static bool isGroupAssignee(const std::vector<int>& groupIds, int assignedToId) {
    return assignedToId != 0
        && std::find(groupIds.begin(), groupIds.end(), assignedToId) != groupIds.end();
}

// /trackers.json からバージョン欄（fixed_version_id）が有効なトラッカー id 集合を取得する
//
// 戻り値 true = 取得成功（結果を out に確定）。false = 接続失敗等（呼び出し側が再試行）。
// enabled_standard_fields は Redmine 5.0 以降のみ返る。1 件も現れない旧版は判定不能として
// out = nullopt で成功扱いにし、全トラッカー有効（従来動作）へ縮退する。
// admin 権限は不要。（/groups.json と異なり認証済みユーザなら取得できる）
static bool fetchVersionedTrackerIds(const Config& cfg,
                                     std::optional<std::unordered_set<int>>& out) {
    auto obj = redmineGetJson(cfg, cfg.redmineUrl + L"/trackers.json", "fetchVersionedTrackerIds");
    if (!obj) return false;
    try {
        std::unordered_set<int> ids;
        bool sawFields = false;
        if (obj.HasKey(L"trackers")) {
            for (auto t : obj.GetNamedArray(L"trackers")) {
                auto tr = t.GetObject();
                if (!tr.HasKey(L"enabled_standard_fields")) continue;
                sawFields = true;
                for (auto f : tr.GetNamedArray(L"enabled_standard_fields")) {
                    if (f.GetString() == L"fixed_version_id") {
                        int tid = static_cast<int>(tr.GetNamedNumber(L"id", 0));
                        if (tid > 0) ids.insert(tid);
                        break;
                    }
                }
            }
        }
        if (sawFields) out = std::move(ids);
        else           out = std::nullopt;
        return true;
    }
    catch (...) {
        writeLog("fetchVersionedTrackerIds: unexpected JSON structure");
        return false;
    }
}

// プロジェクトにバージョンが 1 件でも定義されているかを判定する（セッション内キャッシュ付き）
//
// /projects/:id/versions.json は他プロジェクトからの共有バージョンも含めて返すため、
// 空配列 = バージョン欄が画面に出ない（設定不能）と判定できる。
// 取得失敗時も true（従来どおり除外対象）をキャッシュする。失敗をキャッシュしないと、
// 権限不足などで恒常的に失敗するプロジェクトのチケット件数分だけ毎ポーリングで
// 同じ問合せを繰り返すため。次の再取得契機（「今すぐ更新」・間隔超過・再起動）で解消する。
static bool projectHasAnyVersion(const Config& cfg, int projectId,
                                 std::unordered_map<int, bool>& cache) {
    if (projectId <= 0) return true;
    auto it = cache.find(projectId);
    if (it != cache.end()) return it->second;
    auto obj = redmineGetJson(cfg,
        cfg.redmineUrl + L"/projects/" + std::to_wstring(projectId) + L"/versions.json",
        "projectHasAnyVersion(project=" + std::to_string(projectId) + ")");
    if (!obj) {
        cache[projectId] = true;
        return true;
    }
    try {
        bool has = obj.HasKey(L"versions") && obj.GetNamedArray(L"versions").Size() > 0;
        cache[projectId] = has;
        return has;
    }
    catch (...) {
        writeLog("projectHasAnyVersion: unexpected JSON structure, project="
            + std::to_string(projectId));
        cache[projectId] = true;
        return true;
    }
}

// /issues.json のフィルタクエリ文字列を組み立てる（純粋関数。単体テスト対象）
// 通常クエリは query_id=N。FALLBACK_QUERY_ID は assigned_to_id=me とする。
// me は Redmine がサーバ側で「自分の user id ＋所属グループ id」に展開するため
// （query.rb の me 置換。Redmine 2.1 以降）、グループ宛チケットも取得に含まれる。
static std::wstring issuesFilterQuery(int queryId) {
    if (queryId != FALLBACK_QUERY_ID)
        return L"query_id=" + std::to_wstring(queryId);
    return L"assigned_to_id=me";
}

// 保存クエリ 1 件の結果を total_count に達するまでページングして取得する
// 成功時 true。表示順のソートは行わない。（呼び出し側が和集合を作ってからまとめてソートする）
// API には sort=id を明示指定する。既定やクエリ設定のソート（updated_on 等）は
// ページ取得の合間にチケットが更新されると順位が動き、オフセット走査で同一チケットの
// 重複や取りこぼしが起きる。取りこぼしは次回ポーリングで「新規」誤通知になるため、
// ページング中に順位が変わらない安定キー id で走査する。（表示順には使わない）
// ただし順位の安定は「走査済みページからのチケット離脱による前詰め」を防げない。
// 前詰めが起きると offset 境界の 1 件が飛び、total_count も同数減るため
// 末尾の offset < total チェックでは構造的に検出できない。そこで全ページで
// total_count の不変を検証し、変化したら取得を破棄して次回リトライに委ねる。
// （空振りのコストは誤「新規」通知よりはるかに軽い）
// 失敗ログにクエリ id を含めるのは、複数クエリ運用でどのクエリが壊れているかを
// ログだけで切り分けられるようにするため。
// outAuthError は省略可（nullptr）。HTTP 401 検出時に true を書き込む。呼び出し側が
// 認証エラー（api_key 無効）と一般的な取得失敗を区別するために使う。
// outQueryError は省略可（nullptr）。HTTP 404 検出時に true を書き込む。
// Redmine は存在しない・削除済み・プロジェクト配下の非グローバル・アクセス権のないクエリの
// いずれでも 404 を返し、どれも query_ids の修正なしにはリトライで解決しない確定的な失敗。
// ただし queryId が FALLBACK_QUERY_ID のときは書き込まない。（担当フィルタの 404 は
// 設定不備を意味しないため、無効モードに落とさず通常の接続エラーとして扱う）
// ネットワーク断は status 0、サーバ障害は 5xx になるため 404 と混同しない。
static bool fetchQueryIssues(const Config& cfg, int queryId, std::vector<Issue>& outIssues,
                             bool* outAuthError = nullptr, bool* outQueryError = nullptr) {
    outIssues.clear();

    const std::string logTag = "fetchQueryIssues(" + std::to_string(queryId) + ")";
    int offset = 0;
    int total  = -1;  // 初回ページの total_count で確定する（-1 は未確定）
    do {
        std::wstring url = cfg.redmineUrl + L"/issues.json?" + issuesFilterQuery(queryId)
            + L"&sort=id&limit=100&offset=" + std::to_wstring(offset);
        DWORD status = 0;
        auto obj = redmineGetJson(cfg, url, logTag + " offset=" + std::to_string(offset), &status);
        if (!obj) {
            if (status == 401 && outAuthError)  *outAuthError  = true;
            if (status == 404 && queryId != FALLBACK_QUERY_ID && outQueryError) *outQueryError = true;
            return false;
        }
        try {
            if (obj.HasKey(L"errors")) {
                writeLog(logTag + ": API error response");
                return false;
            }
            // total_count がページ間で変化した＝走査中に集合が増減した。前詰めによる
            // 取りこぼしの可能性があるため、部分結果を破棄して失敗扱いにする
            const int pageTotal = static_cast<int>(obj.GetNamedNumber(L"total_count", 0));
            if (total < 0) {
                total = pageTotal;
            }
            else if (pageTotal != total) {
                writeLog(logTag + ": total_count changed during paging ("
                    + std::to_string(total) + " -> " + std::to_string(pageTotal)
                    + "), discarded");
                return false;
            }
            auto arr = obj.GetNamedArray(L"issues");
            if (arr.Size() == 0) break;  // total_count 不整合による無限ループ防止
            for (auto item : arr) {
                auto is = parseIssueObject(cfg, item.GetObject());
                if (is.id > 0 && !is.updatedOn.empty()) outIssues.push_back(std::move(is));
            }
            offset += static_cast<int>(arr.Size());
        }
        catch (...) {
            writeLog(logTag + ": unexpected JSON structure");
            return false;
        }
    } while (offset < total && !g_shutdownRequested);

    // シャットダウンによる中断や total_count 不整合で全件に達しなかった場合は失敗扱いとする。
    // 部分集合のまま成功を返すと state.json が欠落込みで上書きされ、次回に「新規」誤通知が出る
    if (offset < total) {
        writeLog(logTag + ": incomplete (" + std::to_string(offset) + "/"
            + std::to_string(total) + "), discarded");
        return false;
    }
    return true;
}

// query_ids の全クエリを取得し、チケット id で重複排除した和集合を返す
// 成功時 true。outIssues は updated_on 降順ソート済みで、各要素の queryIds に
// 「そのチケットが現れたクエリ id」を昇順で保持する。
// query_ids 省略（フォールバックモード）時は擬似クエリ FALLBACK_QUERY_ID の 1 件として
// 自分（と所属グループ）が担当のチケットを取得し、所属は {FALLBACK_QUERY_ID} になる。
// 1 クエリでも失敗したら全体を false として部分結果を破棄する。欠落込みの集合で
// state.json を上書きすると次回に「新規」誤通知が出るため。（単一クエリ時代の方針を維持）
// outAuthError は省略可。fetchQueryIssues のいずれかで HTTP 401 を観測したときに true を書く。
// outQueryError は省略可。同じく HTTP 404（query_id 不正）を観測したときに true を書く。
static bool fetchIssues(const Config& cfg, std::vector<Issue>& outIssues,
                        bool* outAuthError = nullptr, bool* outQueryError = nullptr) {
    outIssues.clear();
    std::unordered_map<int, size_t> indexById;  // チケット id → outIssues の位置
    for (int qid : trackedQueryIds(cfg)) {
        std::vector<Issue> part;
        if (!fetchQueryIssues(cfg, qid, part, outAuthError, outQueryError)) return false;
        for (auto& is : part) {
            auto [it, inserted] = indexById.try_emplace(is.id, outIssues.size());
            if (inserted) {
                is.queryIds = {qid};
                outIssues.push_back(std::move(is));
                continue;
            }
            // 重複チケットの表示フィールドは最初に現れたクエリの値を採用する。
            // 同一時刻に同一 API から取るため内容は一致し、差はポーリング中の更新時だけ。
            auto& dst = outIssues[it->second];
            if (std::find(dst.queryIds.begin(), dst.queryIds.end(), qid) == dst.queryIds.end())
                dst.queryIds.push_back(qid);
        }
        if (g_shutdownRequested) return false;  // 途中終了は部分集合なので失敗扱い
    }
    // 所属クエリは state.json 上で前回値と比較するため、query_ids の記述順に依存しない昇順へ正規化
    for (auto& is : outIssues) std::sort(is.queryIds.begin(), is.queryIds.end());
    std::sort(outIssues.begin(), outIssues.end(), [](const Issue& a, const Issue& b) {
        return a.updatedOn > b.updatedOn;
    });
    return true;
}

// 単一チケットを取得する（保存クエリの集合から外れたピンの表示用）
// 成功時 true。404（削除済み等）や接続エラーは false。
static bool fetchIssue(const Config& cfg, int id, Issue& out) {
    auto obj = redmineGetJson(cfg, issueUrl(cfg, id) + L".json",
                              "fetchIssue(" + std::to_string(id) + ")");
    if (!obj) return false;
    try {
        auto issue = obj.GetNamedObject(L"issue", nullptr);
        if (!issue) return false;
        out = parseIssueObject(cfg, issue);
        return out.id > 0;
    }
    catch (...) {
        writeLog("fetchIssue: unexpected JSON structure (id=" + std::to_string(id) + ")");
        return false;
    }
}

// 最終更新者（直近 journal の更新者）
// ok は journals の取得・解析に成功したか。（journals が空でも成功。通信失敗と区別する）
// userId 0 は journals 空（journal の無い新規チケット等）または取得失敗を表す。
struct LastUpdater {
    bool        ok     = false;
    int         userId = 0;
    std::string userName;
};

// 指定チケットの最終 journal の更新者を取得する
// journals は作成順（昇順）で返るため末尾要素が最新の更新。
// 自分の操作の抑止判定（userId 比較）と Toast の更新者表示（userName）で 1 回の取得を共用する。
// 取得失敗時は userId 0 のまま返し、呼び出し側の抑止判定は通知する側に倒れる。
static LastUpdater fetchLastUpdater(const Config& cfg, int id) {
    LastUpdater lu;
    auto obj = redmineGetJson(cfg, issueUrl(cfg, id) + L".json?include=journals",
                              "fetchLastUpdater(" + std::to_string(id) + ")");
    if (!obj) return lu;
    try {
        auto issue = obj.GetNamedObject(L"issue", nullptr);
        if (!issue || !issue.HasKey(L"journals")) return lu;
        auto journals = issue.GetNamedArray(L"journals");
        lu.ok = true;  // journals を取得できた（空でも成功。呼び出し側が失敗時の再解決を判断する）
        if (journals.Size() == 0) return lu;
        auto last = journals.GetObjectAt(journals.Size() - 1);
        auto user = last.GetNamedObject(L"user", nullptr);
        if (!user) return lu;
        lu.userId   = static_cast<int>(user.GetNamedNumber(L"id", 0));
        lu.userName = winrt::to_string(user.GetNamedString(L"name", L""));
        return lu;
    }
    catch (...) {
        return lu;
    }
}

// ユーザの姓・名（/users/{id}.json の lastname / firstname）
struct UserNames {
    std::string lastName;
    std::string firstName;
};

// ユーザ id から姓・名を取得する（セッション内キャッシュ）
// Redmine のユーザ名文字列は姓名が無区切りで分割できない。一覧の {lastname} と {firstname} に
// 使うため /users/{id}.json を引く。取得失敗は両方空をキャッシュして毎回の再試行を抑える。
// （呼び出し側が姓をフルネームへフォールバックし、再起動で再試行される）
static UserNames resolveUserNames(const Config& cfg, int userId,
                                  std::unordered_map<int, UserNames>& cache)
{
    if (userId <= 0) return {};
    auto it = cache.find(userId);
    if (it != cache.end()) return it->second;
    UserNames names;
    auto obj = redmineGetJson(cfg, cfg.redmineUrl + L"/users/" + std::to_wstring(userId) + L".json",
                              "resolveUserNames(user=" + std::to_string(userId) + ")");
    if (obj) {
        try {
            auto user = obj.GetNamedObject(L"user", nullptr);
            if (user) {
                names.lastName  = winrt::to_string(user.GetNamedString(L"lastname", L""));
                names.firstName = winrt::to_string(user.GetNamedString(L"firstname", L""));
            }
        }
        catch (...) {
            writeLog("resolveUserNames: unexpected JSON structure (user="
                + std::to_string(userId) + ")");
        }
    }
    cache[userId] = names;
    return names;
}

// 一覧・Toast 用に各チケットの最終更新者を確定する
// updated_on が前回ポーリングから変わっていないチケットは state.json のキャッシュを使う。
// 変わったチケットと新規のチケットだけ journals を取得する。（定常時の追加 HTTP は変化分のみ）
// journal の無いチケットは起票者を最終更新者として扱い、取得失敗は表示名を空のまま残して
// 次回ポーリングで再解決する。通知抑止（deliverPollResults）もここで確定した updaterId を
// 使うため、journals の取得は本関数だけで行う。
static void resolveUpdaters(const Config& cfg, std::vector<Issue>& issues,
                            const PollState& prev,
                            std::unordered_map<int, UserNames>& nameCache)
{
    for (auto& is : issues) {
        if (g_shutdownRequested) return;
        auto it = prev.issues.find(is.id);
        if (it != prev.issues.end() && it->second.updatedOn == is.updatedOn
            && !it->second.updaterDisplay.empty()) {
            is.updaterId        = it->second.updaterId;
            is.updaterDisplay   = it->second.updaterDisplay;
            is.updaterFirstName = it->second.updaterFirst;
            continue;
        }
        LastUpdater lu = fetchLastUpdater(cfg, is.id);
        is.updaterId   = lu.userId;
        is.updaterName = lu.userName;
        // 取得失敗は表示名を空のまま残し、次回ポーリングのキャッシュミスで再解決する。
        // （誤った名前を state.json に焼き付けない。抑止判定は userId 0 で通知側に倒れる）
        if (!lu.ok) continue;
        // journal の無いチケットは起票者を最終更新者として扱う
        int         dispId   = lu.userId != 0 ? lu.userId : is.authorId;
        std::string dispName = lu.userId != 0 ? lu.userName : is.authorName;
        auto names = resolveUserNames(cfg, dispId, nameCache);
        is.updaterDisplay   = names.lastName;
        is.updaterFirstName = names.firstName;
        if (is.updaterDisplay.empty()) is.updaterDisplay = dispName;
    }
}

// ==================== ダッキング ====================

// プロセス ID からプロセス名（小文字）を取得する
// アクセス不可・取得失敗時は空文字列を返す
static std::wstring getProcessName(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t buf[MAX_PATH];
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(hProc, 0, buf, &size) != 0;
    CloseHandle(hProc);
    if (!ok) return {};
    std::wstring name = PathFindFileNameW(buf);
    CharLowerW(name.data());
    return name;
}

// 対象プロセスのオーディオセッションをミュートし、復元用リストを返す
//
// targets が空の場合は空リストを返す。（ダッキング無効）
// COM デバイス取得失敗時はログ出力して空リストを返す。
// 元々ミュート済みのセッションはスキップする。（復元時にアンミュートしない）
// 呼び出し元は COM が初期化済みであること。（STA/MTA 問わず）
static std::vector<winrt::com_ptr<ISimpleAudioVolume>> duckAudioSessions(
    const std::vector<std::wstring>& targets)
{
    std::vector<winrt::com_ptr<ISimpleAudioVolume>> muted;
    if (targets.empty()) return muted;

    // targets を小文字化した比較セットを作成
    std::set<std::wstring> targetSet;
    for (const auto& t : targets) {
        std::wstring lower = t;
        CharLowerW(lower.data());
        targetSet.insert(lower);
    }

    // デフォルト再生デバイスの取得
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), enumerator.put_void()))) {
        writeLog("duckAudioSessions: failed to create IMMDeviceEnumerator");
        return muted;
    }

    winrt::com_ptr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put()))) {
        writeLog("duckAudioSessions: failed to get default audio endpoint");
        return muted;
    }

    // セッションマネージャ取得
    winrt::com_ptr<IAudioSessionManager2> mgr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
            nullptr, mgr.put_void()))) {
        writeLog("duckAudioSessions: failed to activate IAudioSessionManager2");
        return muted;
    }

    // セッション列挙
    winrt::com_ptr<IAudioSessionEnumerator> sessionEnum;
    if (FAILED(mgr->GetSessionEnumerator(sessionEnum.put()))) {
        writeLog("duckAudioSessions: failed to get session enumerator");
        return muted;
    }

    int count = 0;
    if (FAILED(sessionEnum->GetCount(&count))) {
        writeLog("duckAudioSessions: failed to get session count");
        return muted;
    }

    for (int i = 0; i < count; i++) {
        winrt::com_ptr<IAudioSessionControl> ctrl;
        if (FAILED(sessionEnum->GetSession(i, ctrl.put()))) continue;

        auto ctrl2 = ctrl.try_as<IAudioSessionControl2>();
        if (!ctrl2) continue;

        DWORD pid = 0;
        ctrl2->GetProcessId(&pid);
        if (pid == 0) continue;

        auto name = getProcessName(pid);
        if (name.empty() || targetSet.find(name) == targetSet.end()) continue;

        auto vol = ctrl.try_as<ISimpleAudioVolume>();
        if (!vol) continue;

        // 元々ミュート済みのセッションはスキップ
        BOOL alreadyMuted = FALSE;
        vol->GetMute(&alreadyMuted);
        if (alreadyMuted) continue;

        vol->SetMute(TRUE, nullptr);
        muted.push_back(vol);
    }

    if (!muted.empty()) {
        writeLog("duckAudioSessions: muted " + std::to_string(muted.size()) + " session(s)");
    }
    return muted;
}

// ミュートしたセッションを復元する
static void unduckAudioSessions(std::vector<winrt::com_ptr<ISimpleAudioVolume>>& muted) {
    for (auto& vol : muted) {
        vol->SetMute(FALSE, nullptr);
    }
    muted.clear();
}

// ==================== レジストリ設定 ====================

// レジストリパス（ユーザ設定の永続化先）
static constexpr const wchar_t* REG_KEY_PATH        = L"SOFTWARE\\redntfy";
static constexpr const wchar_t* REG_SOUND_ENABLED     = L"SoundEnabled";
static constexpr const wchar_t* REG_MUTE_IN_MEETING   = L"MuteInMeeting";
static constexpr const wchar_t* REG_ASSIGNED_TO_ME    = L"AssignedToMeOnly";
static constexpr const wchar_t* REG_SORT_BY_DUE       = L"SortByDue";
static constexpr const wchar_t* REG_EXCLUDE_NO_VERSION = L"ExcludeNoVersion";
static constexpr const wchar_t* REG_MUTE_OWN_CHANGES  = L"MuteOwnChanges";
static constexpr const wchar_t* REG_EXCLUDE_HIDDEN    = L"ExcludeHidden";
static constexpr const wchar_t* REG_HOVER_POPUP       = L"HoverPopupEnabled";
static constexpr const wchar_t* REG_NOTIFIED_VERSION  = L"NotifiedUpdateVersion";

// Windows スタートアップ登録用レジストリ（HKCU Run キー）
static constexpr const wchar_t* REG_RUN_KEY_PATH    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr const wchar_t* REG_RUN_VALUE_NAME  = L"redntfy";

// レジストリ DWORD 値の読み取り
// キーまたは値が存在しない場合は defaultVal を返す
static DWORD readRegDword(const wchar_t* valueName, DWORD defaultVal) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY_PATH, valueName,
            RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value;
    return defaultVal;
}

// レジストリ DWORD 値の書き込み
// キーが存在しない場合は自動作成する
static void writeRegDword(const wchar_t* valueName, DWORD value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, nullptr,
            0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        writeLog("registry key create failed: " + wideToUtf8(valueName));
        return;
    }
    if (RegSetValueExW(hKey, valueName, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value)) != ERROR_SUCCESS)
        writeLog("registry write failed: " + wideToUtf8(valueName));
    RegCloseKey(hKey);
}

// レジストリ REG_SZ 値の読み取り
// キーまたは値が存在しない場合は空文字列を返す
static std::wstring readRegString(const wchar_t* valueName) {
    DWORD type = 0, size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY_PATH, valueName,
            RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY_PATH, valueName,
            RRF_RT_REG_SZ, &type, value.data(), &size) != ERROR_SUCCESS)
        return {};
    // RegGetValueW は null 終端を含むサイズを返すため、末尾の null を除去
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

// レジストリ REG_SZ 値の書き込み
// キー REG_KEY_PATH が存在しない場合は RegCreateKeyExW が自動作成する。
static void writeRegString(const wchar_t* valueName, const std::wstring& value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, nullptr,
            0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        writeLog("registry key create failed: " + wideToUtf8(valueName));
        return;
    }
    DWORD byteSize = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    if (RegSetValueExW(hKey, valueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()), byteSize) != ERROR_SUCCESS)
        writeLog("registry write failed: " + wideToUtf8(valueName));
    RegCloseKey(hKey);
}

// スタートアップ登録の有無判定
// HKCU Run キーに redntfy 値が存在すれば登録済みとみなす
static bool isStartupRegistered() {
    return RegGetValueW(HKEY_CURRENT_USER, REG_RUN_KEY_PATH, REG_RUN_VALUE_NAME,
        RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

// スタートアップへ登録
// 現在の実行ファイルパスを二重引用符で括って HKCU Run キーに書き込む
static void registerStartup() {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        writeLog("startup register: GetModuleFileNameW failed");
        return;
    }
    std::wstring quoted = std::wstring(L"\"") + exePath + L"\"";
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY_PATH, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        writeLog("startup register: RegOpenKeyExW failed");
        return;
    }
    DWORD byteSize = static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t));
    if (RegSetValueExW(hKey, REG_RUN_VALUE_NAME, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(quoted.c_str()), byteSize) != ERROR_SUCCESS)
        writeLog("startup register: RegSetValueExW failed");
    RegCloseKey(hKey);
}

// スタートアップ登録を解除
// HKCU Run キーから redntfy 値を削除する。値が存在しない場合はエラーを無視
static void unregisterStartup() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY_PATH, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        writeLog("startup unregister: RegOpenKeyExW failed");
        return;
    }
    LONG r = RegDeleteValueW(hKey, REG_RUN_VALUE_NAME);
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND)
        writeLog("startup unregister: RegDeleteValueW failed");
    RegCloseKey(hKey);
}

// ==================== マイク/カメラ使用検出 ====================

// レジストリ（CapabilityAccessManager）でデバイス使用中かを判定する
//
// deviceType: "microphone" または "webcam"
// LastUsedTimeStop == 0 のサブキーがあれば使用中。（UWP 配下 + NonPackaged 配下の両方を走査）
static bool isRegistryDeviceInUse(const wchar_t* deviceType) {
    std::wstring basePath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
        L"\\CapabilityAccessManager\\ConsentStore\\";
    basePath += deviceType;

    auto checkSubKeys = [](const std::wstring& keyPath, bool skipNonPackaged) -> bool {
        // RAII ガード: 例外（std::bad_alloc 等）でもハンドルを確実に閉じる
        struct Guard { HKEY h = nullptr; ~Guard() { if (h) RegCloseKey(h); } };

        Guard kg;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &kg.h) != ERROR_SUCCESS)
            return false;

        bool inUse = false;
        wchar_t subName[256];
        DWORD subNameSize;

        for (DWORD idx = 0; !inUse; idx++) {
            subNameSize = _countof(subName);
            if (RegEnumKeyExW(kg.h, idx, subName, &subNameSize,
                    nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            if (skipNonPackaged && wcscmp(subName, L"NonPackaged") == 0) continue;

            Guard sg;
            std::wstring subPath = keyPath + L"\\" + subName;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, subPath.c_str(), 0, KEY_READ, &sg.h) != ERROR_SUCCESS)
                continue;

            DWORD64 lastUsedTimeStop = 0;
            DWORD dataSize = sizeof(lastUsedTimeStop);
            DWORD dataType;
            if (RegQueryValueExW(sg.h, L"LastUsedTimeStop", nullptr, &dataType,
                    reinterpret_cast<LPBYTE>(&lastUsedTimeStop), &dataSize) == ERROR_SUCCESS
                && dataType == REG_QWORD && lastUsedTimeStop == 0) {
                inUse = true;
            }
        }
        return inUse;
    };

    if (checkSubKeys(basePath, true))                              return true; // UWP
    if (checkSubKeys(basePath + L"\\NonPackaged", false))          return true; // Win32
    return false;
}

// WASAPI でマイクキャプチャセッションがアクティブかを判定する
//
// 通知スレッドの MTA COM を利用。（呼び出し元スレッドで CoInitializeEx 済み前提）
// レジストリで検出できない仮想オーディオデバイス経由の使用を補完検出する。
static bool isMicCaptureActive() {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), enumerator.put_void())))
        return false;

    winrt::com_ptr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.put())))
        return false;

    UINT deviceCount = 0;
    collection->GetCount(&deviceCount);

    for (UINT i = 0; i < deviceCount; i++) {
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) continue;

        winrt::com_ptr<IAudioSessionManager2> mgr;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                nullptr, mgr.put_void()))) continue;

        winrt::com_ptr<IAudioSessionEnumerator> sessionEnum;
        if (FAILED(mgr->GetSessionEnumerator(sessionEnum.put()))) continue;

        int sessionCount = 0;
        sessionEnum->GetCount(&sessionCount);

        for (int si = 0; si < sessionCount; si++) {
            winrt::com_ptr<IAudioSessionControl> ctrl;
            if (FAILED(sessionEnum->GetSession(si, ctrl.put()))) continue;

            // システムサウンドセッションはスキップ
            auto ctrl2 = ctrl.try_as<IAudioSessionControl2>();
            if (ctrl2 && ctrl2->IsSystemSoundsSession() == S_OK) continue;

            AudioSessionState state;
            if (SUCCEEDED(ctrl->GetState(&state)) && state == AudioSessionStateActive)
                return true;
        }
    }
    return false;
}

// マイクまたはカメラの使用状態を判定する
//
// レジストリ → WASAPI の順で検出し、いずれかが true なら使用中。
// WASAPI 補完（isMicCaptureActive）はマイクのみ対象。（カメラはレジストリ検出のみ）
static bool isMeetingActive() {
    if (isRegistryDeviceInUse(L"microphone")) return true;
    if (isRegistryDeviceInUse(L"webcam"))     return true;
    return isMicCaptureActive(); // レジストリ未検出分の補完
}

// ==================== 通知音前処理 ====================

// WAV の 16bit PCM サンプルにラウドネスノーマライズを適用する
//
// EBU R128 に基づく統合ラウドネス測定（libebur128）でゲインを算出し、
// 全サンプルに乗算する。ピークが peak_ceiling を超えないようゲインを制限する。
// ほぼ無音（ピーク < 1e-6f）ならスキップする。
static void normalizeLoudness(std::vector<int16_t>& samples, int channels,
                              int sampleRate, float target, float peakCeiling) {
    if (samples.empty()) return;

    UINT32 frames = static_cast<UINT32>(samples.size()) / channels;
    std::vector<float> flt(samples.size());
    for (size_t i = 0; i < samples.size(); i++)
        flt[i] = static_cast<float>(samples[i]) / 32768.0f;

    // ピーク確認（ほぼ無音はスキップ）
    float peak = 0.0f;
    for (float s : flt) {
        float v = std::fabs(s);
        if (v > peak) peak = v;
    }
    if (peak < 1e-6f) return;

    // 統合ラウドネス測定
    ebur128_state* state = ebur128_init(
        static_cast<unsigned>(channels),
        static_cast<unsigned long>(sampleRate),
        EBUR128_MODE_I);
    if (!state) return;

    if (ebur128_add_frames_float(state, flt.data(), frames) != EBUR128_SUCCESS) {
        ebur128_destroy(&state);
        return;
    }

    double loudness = 0.0;
    int result = ebur128_loudness_global(state, &loudness);
    ebur128_destroy(&state);

    if (result != EBUR128_SUCCESS || std::isinf(loudness)) return;

    float gain = static_cast<float>(std::pow(10.0, (target - loudness) / 20.0));
    if (peak * gain > peakCeiling) gain = peakCeiling / peak;

    for (int16_t& s : samples) {
        float v = static_cast<float>(s) * gain;
        if (v > 32767.0f)  v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        s = static_cast<int16_t>(v);
    }
}

// WAV ファイルを読み込み、ラウドネスノーマライズを適用して g_wavCache に格納する
//
// 起動時（設定読み込み後）に 1 回だけ呼び出す。以降の再生は g_wavCache を使い回す。
// 16bit PCM WAV のみ対応。ファイルが存在しない場合は g_wavCache.valid = false のまま。
static void loadWavAndNormalize(const std::wstring& exeDir, const Config& cfg) {
    g_wavCache = WavCache{};  // リセット

    std::wstring soundPath = exeDir + L"\\" + DEFAULT_SOUND_FILE;
    HANDLE hFile = CreateFileW(soundPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        writeLog("loadWavAndNormalize: sound.wav not found");
        return;
    }

    // ファイルサイズ上限の検証（不正ファイルでの過大メモリ確保を防止）
    {
        LARGE_INTEGER fsz = {};
        if (!GetFileSizeEx(hFile, &fsz) || fsz.QuadPart <= 0
                || static_cast<ULONGLONG>(fsz.QuadPart) > MAX_WAV_FILE_BYTES) {
            writeLog("loadWavAndNormalize: sound.wav size out of range");
            CloseHandle(hFile);
            return;
        }
    }

    WAVEFORMATEX wavFmt = {};
    std::vector<int16_t> samples;

    // RIFF/WAVE ヘッダ検証
    {
        char buf[12] = {};
        DWORD nRead = 0;
        ReadFile(hFile, buf, 12, &nRead, nullptr);
        if (nRead != 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
            writeLog("loadWavAndNormalize: invalid RIFF/WAVE header");
            goto cleanup;
        }
    }

    // チャンク走査
    {
        bool hasFmt  = false;
        bool hasData = false;
        while (!hasData) {
            char  id[4]     = {};
            DWORD chunkSize = 0;
            DWORD nRead     = 0;
            if (!ReadFile(hFile, id, 4, &nRead, nullptr) || nRead != 4) break;
            if (!ReadFile(hFile, &chunkSize, 4, &nRead, nullptr) || nRead != 4) break;

            if (memcmp(id, "fmt ", 4) == 0) {
                DWORD readSize = min(chunkSize, (DWORD)sizeof(WAVEFORMATEX));
                if (!ReadFile(hFile, &wavFmt, readSize, &nRead, nullptr) || nRead != readSize) {
                    writeLog("loadWavAndNormalize: failed to read fmt chunk");
                    goto cleanup;
                }
                if (chunkSize > readSize)
                    SetFilePointer(hFile, (LONG)(chunkSize - readSize), nullptr, FILE_CURRENT);
                if (wavFmt.wFormatTag != WAVE_FORMAT_PCM || wavFmt.wBitsPerSample != 16
                        || wavFmt.nSamplesPerSec == 0 || wavFmt.nBlockAlign == 0
                        || wavFmt.nChannels == 0) {
                    writeLog("loadWavAndNormalize: unsupported format, only 16bit PCM WAV is supported");
                    goto cleanup;
                }
                hasFmt = true;
            }
            else if (memcmp(id, "data", 4) == 0) {
                if (!hasFmt) {
                    writeLog("loadWavAndNormalize: data chunk before fmt chunk");
                    goto cleanup;
                }
                // data チャンク全体を一括読み込み（チャンクサイズも上限で防御）
                if (chunkSize > MAX_WAV_FILE_BYTES) {
                    writeLog("loadWavAndNormalize: data chunk size out of range");
                    goto cleanup;
                }
                // 奇数サイズの WAV で ReadFile がバッファ境界外を要求しないよう int16_t に整列
                DWORD totalBytes = chunkSize & ~1u;
                samples.resize(totalBytes / sizeof(int16_t));
                ReadFile(hFile, samples.data(), totalBytes, &nRead, nullptr);
                samples.resize(nRead / sizeof(int16_t));
                hasData = true;
            }
            else {
                // 奇数サイズのチャンクは 1 バイトパディングを含む（RIFF 仕様）
                // chunkSize+1 のオーバーフロー・LONG 範囲超過・シーク失敗は走査終了として扱う
                DWORD skipSize = chunkSize + 1;
                if (skipSize < chunkSize || skipSize > (DWORD)LONG_MAX) break;
                skipSize &= ~1u;
                if (SetFilePointer(hFile, (LONG)skipSize, nullptr, FILE_CURRENT) == INVALID_SET_FILE_POINTER)
                    break;
            }
        }
        if (!hasFmt || !hasData) {
            writeLog("loadWavAndNormalize: fmt or data chunk not found");
            goto cleanup;
        }
    }

    // ラウドネスノーマライズ
    if (cfg.loudnessEnabled) {
        normalizeLoudness(samples, wavFmt.nChannels, (int)wavFmt.nSamplesPerSec,
                          cfg.loudnessTarget, cfg.loudnessPeakCeiling);
        writeLog("loadWavAndNormalize: normalization applied (target="
            + std::to_string((int)cfg.loudnessTarget) + " LUFS)");
    }

    g_wavCache.samples = std::move(samples);
    g_wavCache.fmt     = wavFmt;
    g_wavCache.valid   = true;
    writeLog("loadWavAndNormalize: loaded sound.wav");

cleanup:
    CloseHandle(hFile);
}

// ==================== 通知音再生 ====================

// WASAPI バッファに不可聴正弦波を書き込む
//
// サンプルレートがナイキスト周波数未満の場合はゼロ埋めにフォールバックする。
// phase はバッファ分割供給間で位相を維持するための参照引数。
static void fillToneBuffer(BYTE* buf, UINT32 frames,
                           const WAVEFORMATEX& wavFmt, double& phase) {
    // BLE 省電力モード抑止用の不可聴高周波トーン固定パラメータ
    constexpr float FREQ      = 19000.0f; // 周波数 Hz（成人不可聴域）
    constexpr float AMPLITUDE = 0.001f;   // 振幅（約 -60dB）

    // ナイキスト周波数チェック（例：44.1kHz のナイキスト = 22.05kHz）
    if (static_cast<double>(FREQ) >= static_cast<double>(wavFmt.nSamplesPerSec) / 2.0) {
        // フォールバック：完全無音（ナイキスト以上の周波数は表現不可）
        memset(buf, 0, frames * wavFmt.nBlockAlign);
        return;
    }

    int16_t* samples   = reinterpret_cast<int16_t*>(buf);
    double   phaseStep = 2.0 * PI * FREQ / wavFmt.nSamplesPerSec;
    float    ampFloat  = AMPLITUDE * 32767.0f;

    for (UINT32 i = 0; i < frames; i++) {
        int16_t sampleValue = static_cast<int16_t>(ampFloat * std::sin(phase));
        for (int ch = 0; ch < wavFmt.nChannels; ch++) {
            samples[i * wavFmt.nChannels + ch] = sampleValue;
        }
        phase += phaseStep;
    }

    // 位相を [0, 2π) に正規化（精度維持）
    phase = std::fmod(phase, 2.0 * PI);
}

// g_wavCache のノーマライズ済み PCM データを WASAPI 共有モードで再生する
//
// 再生フロー（ガードトーン長 tone_ms が 0 より大きい場合）：
//   ガードトーン（リードイン） → 通知音（チャイム）→ ガードトーン（リードアウト）
// g_wavCache.valid == false（sound.wav なし）の場合は何もしない。
// WASAPI 共有モードで再生するため、OS のオーディオエンジンがリサンプリングを自動処理する。
// g_shutdownRequested が true になると再生を中断する。
static bool playWavToWasapi(const Config& cfg) {
    if (!g_wavCache.valid) return false;

    const WAVEFORMATEX& wavFmt     = g_wavCache.fmt;
    const int16_t*      pcmData    = g_wavCache.samples.data();
    UINT32              totalFrames = static_cast<UINT32>(g_wavCache.samples.size())
                                      / wavFmt.nChannels;

    bool   ok     = false;
    HANDLE hEvent = nullptr;

    // WASAPI デバイス初期化・再生
    {
        winrt::com_ptr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), enumerator.put_void()))) {
            writeLog("playWavToWasapi: CoCreateInstance IMMDeviceEnumerator failed");
            goto cleanup;
        }

        winrt::com_ptr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) {
            writeLog("playWavToWasapi: GetDefaultAudioEndpoint failed");
            goto cleanup;
        }

        winrt::com_ptr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) {
            writeLog("playWavToWasapi: Activate IAudioClient failed");
            goto cleanup;
        }

        hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent) goto cleanup;

        constexpr REFERENCE_TIME bufDuration = 500'000; // 50ms = 500,000 * 100ns
        // AUTOCONVERTPCM + SRC_DEFAULT_QUALITY で BLE 等フォーマットが異なるデバイスにも対応する。
        constexpr DWORD initFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                  | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                  | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, initFlags,
                bufDuration, 0, &wavFmt, nullptr))) {
            writeLog("playWavToWasapi: IAudioClient::Initialize failed");
            goto cleanup;
        }

        client->SetEventHandle(hEvent);

        winrt::com_ptr<IAudioRenderClient> render;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), render.put_void()))) {
            writeLog("playWavToWasapi: GetService IAudioRenderClient failed");
            goto cleanup;
        }

        UINT32 bufFrames = 0;
        client->GetBufferSize(&bufFrames);

        // ガードトーン供給ループ（冒頭・末尾共用）
        auto runToneLoop = [&](UINT32 toneFrames) {
            UINT32 written = 0;
            double phase   = 0.0;
            while (written < toneFrames && !g_shutdownRequested) {
                WaitForSingleObject(hEvent, 200);
                UINT32 padding = 0;
                client->GetCurrentPadding(&padding);
                UINT32 avail  = bufFrames - padding;
                UINT32 frames = min(avail, toneFrames - written);
                if (frames == 0) continue;
                BYTE* buf = nullptr;
                if (SUCCEEDED(render->GetBuffer(frames, &buf))) {
                    fillToneBuffer(buf, frames, wavFmt, phase);
                    render->ReleaseBuffer(frames, 0);
                }
                written += frames;
            }
        };

        // 冒頭ガードトーン（BLE ヘッドホン対処：省電力移行防止）
        if (cfg.guardToneMs > 0) {
            UINT32 toneFrames = wavFmt.nSamplesPerSec * cfg.guardToneMs / 1000;
            client->Start();
            runToneLoop(toneFrames);
            client->Stop();
            client->Reset();
        }

        // WAV PCM 供給ループ（メモリバッファから読み込み）
        UINT32 sentFrames = 0;
        bool   eof        = false;
        client->Start();
        while (!eof && !g_shutdownRequested) {
            WaitForSingleObject(hEvent, 200);
            UINT32 padding = 0;
            client->GetCurrentPadding(&padding);
            UINT32 avail = bufFrames - padding;
            if (avail == 0) continue;

            UINT32 frames = min(avail, totalFrames - sentFrames);
            if (frames == 0) {
                // 全フレーム送信済み。残りバッファが再生されるまで待機（最大約 1 秒）
                // シャットダウン要求時は排出を待たず抜け、終了時の 5 秒待ちを圧迫しない
                for (int i = 0; i < 100 && !g_shutdownRequested; i++) {
                    UINT32 rem = 0;
                    client->GetCurrentPadding(&rem);
                    if (rem == 0) break;
                    Sleep(10);
                }
                eof = true;
                break;
            }

            BYTE* buf = nullptr;
            if (SUCCEEDED(render->GetBuffer(frames, &buf))) {
                memcpy(buf, pcmData + sentFrames * wavFmt.nChannels,
                       frames * wavFmt.nBlockAlign);
                render->ReleaseBuffer(frames, 0);
                sentFrames += frames;
            }
        }

        // 末尾ガードトーン（BLE ヘッドホン対処：省電力移行防止、ダッキング解除前の緩衝）
        if (cfg.guardToneMs > 0 && eof) {
            UINT32 trailFrames = wavFmt.nSamplesPerSec * cfg.guardToneMs / 1000;
            runToneLoop(trailFrames);
        }

        client->Stop();
        ok = eof;
    }

cleanup:
    if (hEvent) CloseHandle(hEvent);
    return ok;
}

// 通知音を再生し、ダッキングの開始・解除を行うスレッド関数
//
// MTA で COM 初期化し、ダッキング開始 → playWavToWasapi 同期呼び出し → ダッキング解除の順で実行する。
// ISimpleAudioVolume の取得・復元・解放をすべて本スレッド内に閉じ込めることで、
// COM スレッド境界をまたいだプロキシ操作を回避する。
static DWORD WINAPI soundThread(LPVOID param) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* ctx  = static_cast<SoundContext*>(param);
    bool comOk = (hr == S_OK || hr == S_FALSE);

    if (comOk) {
        auto muted = duckAudioSessions(ctx->cfg.duckTargets);
        playWavToWasapi(ctx->cfg);
        if (!muted.empty()) {
            unduckAudioSessions(muted);
            writeLog("unduckAudioSessions: restored");
        }
    }
    else {
        writeLog("soundThread: CoInitializeEx failed");
    }

    delete ctx;
    if (comOk) CoUninitialize();
    return 0;
}

// WASAPI で通知音（16bit PCM WAV）を再生する
//
// 再生フロー（ガードトーン長 tone_ms が 0 より大きい場合）：
//   ガードトーン（リードイン）→ 通知音（チャイム）→ ガードトーン（リードアウト）
// g_wavCache.valid == false の場合は音声を再生せずに終了する。（Toast 通知は呼び出し側で別途表示）
// ダッキング：cfg.duckTargets に指定されたプロセスを再生中ミュートし、全再生完了後に復元する。
static void launchSound(const Config& cfg) {
    if (!g_wavCache.valid) {
        writeLog("launchSound: sound.wav not loaded, skipping sound");
        return;
    }

    // 前回スレッドの完了を待ってから新スレッドを起動する
    // 旧スレッドが再生中に新スレッドを起動すると、旧スレッドの unduck と新スレッドの duck が競合し、
    // 新スレッド再生中に他プロセスが意図せずミュート解除される問題が起きる。
    // タイムアウト時はハンドルを保持したまま今回の再生を諦める（強制終了するとスレッド固有 COM/WASAPI
    // リソースが宙に浮くため）。次回 launchSound 呼び出し時に再度 join を試みる。
    // 待機ループは 1 秒単位で g_shutdownRequested を監視し、シャットダウン要求があれば即時放棄する。
    // WAIT_OBJECT_0 以外（WAIT_FAILED 等）は異常終了として再生を見送る。
    if (g_soundThread) {
        DWORD waitResult = WAIT_TIMEOUT;
        for (int waited = 0; waited < 10; ++waited) {
            if (g_shutdownRequested.load()) {
                writeLog("launchSound: shutdown requested while waiting previous thread, skipping this play");
                return;
            }
            waitResult = WaitForSingleObject(g_soundThread, 1000);
            if (waitResult != WAIT_TIMEOUT) break;
        }
        if (waitResult == WAIT_TIMEOUT) {
            writeLog("launchSound: previous sound thread did not finish within 10s, skipping this play");
            return;
        }
        if (waitResult != WAIT_OBJECT_0) {
            // WAIT_FAILED はハンドル無効の可能性があるため、クリアして次回の再試行を可能にする。
            // WAIT_TIMEOUT は上記で処理済みなので、ここに来るのは WAIT_FAILED のみ。（実質）
            DWORD failErr = (waitResult == WAIT_FAILED) ? GetLastError() : 0;
            writeLog("launchSound: WaitForSingleObject returned " + std::to_string(waitResult)
                + " (error " + std::to_string(failErr) + "), skipping this play");
            CloseHandle(g_soundThread);
            g_soundThread = nullptr;
            return;
        }
        CloseHandle(g_soundThread);
        g_soundThread = nullptr;
    }

    // スレッドで再生（ダッキング開始 → 通知音 → ダッキング解除を soundThread 内で完結）
    auto* ctx = new SoundContext{ .cfg = cfg };

    HANDLE hThread = CreateThread(nullptr, 0, soundThread, ctx, 0, nullptr);
    if (!hThread) {
        delete ctx;
        return;
    }
    g_soundThread = hThread;  // ハンドルを保持（シャットダウン時 join に使用）
}

// ==================== ショートカット ====================

// AUMID 付きスタートメニューショートカットを作成する（Toast 通知に必要）
// Windows 10/11 ではデスクトップアプリの Toast に AUMID 付き .lnk が必要。既存の場合はスキップ。
static void ensureShortcut() {
    wchar_t appData[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return;

    std::wstring linkPath = std::wstring(appData)
        + L"\\Microsoft\\Windows\\Start Menu\\Programs\\redntfy.lnk";

    if (GetFileAttributesW(linkPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    winrt::com_ptr<IShellLinkW> psl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(psl.put())))) return;

    psl->SetPath(exePath);

    if (auto pps = psl.as<IPropertyStore>()) {
        PROPVARIANT pv;
        if (SUCCEEDED(InitPropVariantFromString(APP_AUMID, &pv))) {
            pps->SetValue(PKEY_AppUserModel_ID, pv);
            PropVariantClear(&pv);
        }
        pps->Commit();
    }

    if (auto ppf = psl.as<IPersistFile>()) {
        ppf->Save(linkPath.c_str(), TRUE);
    }
}

// ==================== Toast 通知 ====================

// アプリアイコンの Toast XML タグを生成する
//
// exe 同フォルダの app.ico が存在する場合のみタグを返す。存在しない場合は空文字列。
// 初回呼び出し時に結果をキャッシュする。（app.ico は起動後に変化しない）
static std::wstring buildIconTag() {
    static const std::wstring tag = []() -> std::wstring {
        auto iconPath = getExeDir() + L"\\app.ico";
        if (!PathFileExistsW(iconPath.c_str())) return {};
        return L"<image placement=\"appLogoOverride\" src=\"" + escapeXml(iconPath) + L"\"/>";
    }();
    return tag;
}

// Toast XML を WinRT に渡して通知を表示する
//
// https:// / http:// 以外のスキームは拒否して任意プロトコルハンドラの悪用を防ぐ。
// xml は </visual> まで構築済みの文字列を渡す。（</toast> は内部で付加する）
// buttonLabel はボタンの表示文言。既定はチケット通知向けで、新版通知など開く先が
// チケットでない Toast は呼び出し側が文言を差し替える。
static void dispatchToastXml(std::wstring xml, const std::wstring& permalink,
                             const std::wstring& buttonLabel = L"チケットを開く")
{
    if (!permalink.empty() && isHttpUrl(permalink)) {
        xml += L"<actions>"
               L"<action activationType=\"protocol\" content=\"" + escapeXml(buttonLabel) + L"\""
               L" arguments=\"" + escapeXml(permalink) + L"\"/>"
               L"</actions>";
    }
    xml += L"</toast>";

    winrt::Windows::Data::Xml::Dom::XmlDocument doc;
    doc.LoadXml(xml);

    auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager
        ::CreateToastNotifier(APP_AUMID);
    auto notification = winrt::Windows::UI::Notifications::ToastNotification(doc);

    notifier.Show(notification);
}

// Toast 通知を表示する
//
// OS に通知を登録して即 return する。（コールバック待機なし）
// アプリアイコン（exe 同フォルダの app.ico）と「チケットを開く」ボタンを含む通知を表示する。
// silent=true（デフォルト）：OS 通知音を無効化する。（アプリ側で sound.wav を鳴らすため）
// silent=false：<audio> タグを省略し OS 標準通知音を鳴らす。
static void showToast(const std::wstring& line1, const std::wstring& line2,
                      const std::wstring& permalink, bool silent = true)
{
    std::wstring xml =
        L"<toast>"
        L"<visual><binding template=\"ToastGeneric\">"
        + buildIconTag() +
        L"<text>" + escapeXml(line1) + L"</text>"
        L"<text>" + escapeXml(line2) + L"</text>"
        L"</binding></visual>"
        + (silent ? L"<audio silent=\"true\"/>" : L"");

    dispatchToastXml(std::move(xml), permalink);
}

// 3 行 Toast 通知を表示する（更新チェックの新版通知、無効モードの案内用）
//
// line1 を title スタイル（太字大）で表示する。
// silent=true（デフォルト false）で OS 通知音を無効化する。
// buttonLabel は permalink 付き Toast のボタン文言。（dispatchToastXml の既定と同じ）
static void showToast3(const std::wstring& line1, const std::wstring& line2,
                       const std::wstring& line3, const std::wstring& permalink,
                       bool silent = false, const std::wstring& buttonLabel = L"チケットを開く")
{
    std::wstring xml =
        L"<toast>"
        L"<visual><binding template=\"ToastGeneric\">"
        + buildIconTag() +
        L"<text hint-style=\"title\">" + escapeXml(line1) + L"</text>"
        L"<text>" + escapeXml(line2) + L"</text>"
        L"<text>" + escapeXml(line3) + L"</text>"
        L"</binding></visual>"
        + (silent ? L"<audio silent=\"true\"/>" : L"");

    dispatchToastXml(std::move(xml), permalink, buttonLabel);
}

// エラー Toast 表示（クールダウン制御付き）
//
// 前回通知から ERROR_TOAST_COOLDOWN_MS 以内は抑制する。
// showToast の 1 行目にエラー種別、2 行目に本文を表示する。
// force=true でクールダウンを無視する。「今すぐ更新」など明示操作への応答は、
// 無音で失敗すると操作が届いたか判断できないため抑制しない。
// force で出した場合もクールダウンの起点は更新する。取得失敗の 60 秒後には自動リトライが
// 走るため、起点を据え置くと同じ内容の Toast が続けて 2 通出る。
static void showErrorToast(const std::wstring& title, const std::wstring& body,
                           bool force = false)
{
    ULONGLONG now  = GetTickCount64();
    ULONGLONG last = g_lastErrorToastTime.load();
    if (!force && last != 0 && now - last < ERROR_TOAST_COOLDOWN_MS) return;
    g_lastErrorToastTime.store(now);
    try {
        showToast(title, body, L"");
    }
    catch (winrt::hresult_error const& e) {
        writeLog("showErrorToast failed: " + winrt::to_string(e.message()));
    }
    catch (...) {
        writeLog("showErrorToast failed: unknown exception");
    }
}

// ==================== トレイアイコン ====================

// バックグラウンドスレッド用の中断可能 Sleep
//
// メッセージは処理しない。（呼び出し元がメインスレッドではないため）
// g_shutdownRequested・g_forcePoll・g_manualPoll のいずれかが true になった時点で即座にリターンする。
// 100 ms 単位で各フラグをポーリングするため、最大 100 ms の中断遅延が発生する。
static void waitInterruptible(DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (!g_shutdownRequested && !g_forcePoll.load() && !g_manualPoll.load()) {
        ULONGLONG now = GetTickCount64();
        if (end <= now) break;
        ULONGLONG remain = end - now;
        DWORD chunk = static_cast<DWORD>((std::min)(remain, static_cast<ULONGLONG>(100)));
        Sleep(chunk);
    }
}

// NOTIFYICONDATAW の共通フィールドを初期化する
static NOTIFYICONDATAW makeTrayNid(HWND hWnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hWnd;
    nid.uID    = 1;
    return nid;
}

// トレイアイコンの登録
static void addTrayIcon(HWND hWnd) {
    // 無効モードなら最初から無効アイコンで登録する（後からの差し替えによるチラつき防止）
    // TaskbarCreated（エクスプローラ再起動）の再登録でも同じ経路を通り状態が一致する
    g_trayIconStyle = isDisabled() ? TrayIconStyle::Disabled : TrayIconStyle::Normal;
    auto nid = makeTrayNid(hWnd);
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    // 件数ツールチップは全廃のため、通常時は初期 tooltip も出さない。（一覧はホバーで出る）
    // 無効モードのみ表示し、直後の案内 tooltip 更新に引き継ぐ
    if (isDisabled())
        wcscpy_s(nid.szTip, L"読み込み中...");
    UINT iconId = isDisabled() ? IDI_APP_ICON_DISABLE : IDI_APP_ICON;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(iconId));
    Shell_NotifyIconW(NIM_ADD, &nid);
    if (nid.hIcon) DestroyIcon(nid.hIcon);
}

// バッジ付きトレイアイコンの生成
// ベースアイコンの右下に白リング付きの赤い円バッジを合成した HICON を返す。
// 32bpp DIBSection にピクセルを直接書き込むことで alpha=255 を確実に設定する。
// GDI Ellipse では alpha バイトが 0 のままになり DWM 合成で透明化されるため使わない。
// 呼び出し側が DestroyIcon で解放する責務を持つ。失敗時は nullptr を返す。
static HICON createBadgedIcon() {
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);

    // 32bpp BGRA の DIBSection を作成（pixels ポインタで直接アクセスできる）
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bmi              = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = cx;
    bmi.bmiHeader.biHeight      = -cy;  // top-down（y=0 が左上）
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    UINT32* pixels = nullptr;
    HBITMAP hbm  = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, (void**)&pixels, nullptr, 0);
    if (!hbm || !pixels) {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return nullptr;
    }
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbm);

    // ベースアイコンを DIBSection に描画（DrawIconEx は 32bpp DIB に alpha を正しく書き込む）
    HICON hBase = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON),
                                    IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!hBase) {
        SelectObject(hdcMem, hOld);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return nullptr;
    }
    DrawIconEx(hdcMem, 0, 0, hBase, cx, cy, 0, nullptr, DI_NORMAL);
    DestroyIcon(hBase);

    // バッジ円のパラメータ（赤丸をアイコン十字 4 等分の右下領域に収める。
    // 白リングが 1px、外周 AA がさらに 1px、中心から最大 r + 1.5px まで掛かる）
    // 原点の +1 オフセットは白リング導入時に廃止した。リング拡張分（+1.5px）を
    // アイコン下端に収めるため。
    int badgeSize = (std::max)(cx / 2 - 2, 3);
    int ox   = cx / 2;
    int oy   = cy / 2;
    float midX = ox + badgeSize / 2.0f;
    float midY = oy + badgeSize / 2.0f;
    float r    = badgeSize / 2.0f;

    // 赤丸の外側に 1px の白リングを足す。アイコンが Redmine ロゴ（レンガ色）のため、
    // 純赤バッジは同系色で埋没する。赤丸は従来と同寸・同色のまま残す。
    // 赤と白の境界は AA せず硬いエッジとする。AA を挟むと 16px では 1px 幅のリングが
    // 内外の AA に食われ、不透明な白画素が 1 つも残らないため。（外周の透明境界のみ AA）
    float rOuter = r + 1.0f;  // 白リング外周の半径

    // 距離ベースのアルファブレンドで円エッジを滑らかに描画（アンチエイリアス）
    int scanPad = static_cast<int>(rOuter) + 1;
    for (int y = oy - scanPad; y < oy + scanPad + badgeSize; ++y) {
        if (y < 0 || y >= cy) continue;
        for (int x = ox - scanPad; x < ox + scanPad + badgeSize; ++x) {
            if (x < 0 || x >= cx) continue;
            float d     = sqrtf((x - midX) * (x - midX) + (y - midY) * (y - midY));
            float alpha = (d <= rOuter - 0.5f) ? 1.0f : (d <= rOuter + 0.5f) ? (rOuter + 0.5f - d) : 0.0f;
            if (alpha <= 0.0f) continue;
            // 赤丸（d <= r - 0.5）の外側は白リング（G/B 成分を立てて白にする）
            float w = (d <= r - 0.5f) ? 0.0f : 255.0f;
            // 外周 AA 帯は代入でなく src-over 合成にする。代入だと半透明の白画素が
            // ロゴ画素を置き換えて透過の欠けになり、バッジ周囲のロゴが削れて見えるため。
            // ベース・バッジとも straight alpha。（DrawIconEx / CreateIconIndirect と同じ扱い）
            // alpha = 1 の画素は da = 0 で結果が代入と同値になるため、不透明側の分岐は設けない。
            UINT32 dst = pixels[y * cx + x];
            float da = ((dst >> 24) & 0xFF) / 255.0f * (1.0f - alpha);  // 合成後に残るベースの寄与
            float oa = alpha + da;  // 合成後のアルファ（alpha > 0 なので 0 除算はない）
            UINT32 a  = static_cast<UINT32>(oa * 255.0f + 0.5f);
            UINT32 rr = static_cast<UINT32>((255.0f * alpha + ((dst >> 16) & 0xFF) * da) / oa + 0.5f);
            UINT32 gg = static_cast<UINT32>((w * alpha + ((dst >> 8) & 0xFF) * da) / oa + 0.5f);
            UINT32 bb = static_cast<UINT32>((w * alpha + (dst & 0xFF) * da) / oa + 0.5f);
            pixels[y * cx + x] = (a << 24) | (rr << 16) | (gg << 8) | bb;
        }
    }

    SelectObject(hdcMem, hOld);

    // モノクロマスク（黒 = 不透明）を作成
    HBITMAP hbmMask  = CreateBitmap(cx, cy, 1, 1, nullptr);
    HDC hdcMono      = CreateCompatibleDC(hdcScreen);
    HBITMAP hOldMono = (HBITMAP)SelectObject(hdcMono, hbmMask);
    PatBlt(hdcMono, 0, 0, cx, cy, BLACKNESS);
    SelectObject(hdcMono, hOldMono);
    DeleteDC(hdcMono);

    ICONINFO ii   = { TRUE, 0, 0, hbmMask, hbm };
    HICON hResult = CreateIconIndirect(&ii);

    DeleteObject(hbmMask);
    DeleteDC(hdcMem);
    DeleteObject(hbm);
    ReleaseDC(nullptr, hdcScreen);
    return hResult;
}

// トレイアイコンの状態切り替え（無効モード最優先、次いでバッジ = 未読チケットあり）
// 望ましい状態が前回と同じなら NIM_MODIFY をスキップする。
// 無効モード中はポーリングが止まっており未読は増えないため、バッジとの複合状態は無い。
static void updateTrayIcon(HWND hWnd, bool hasUnread) {
    TrayIconStyle style = isDisabled() ? TrayIconStyle::Disabled
                        : hasUnread    ? TrayIconStyle::NormalBadged
                                       : TrayIconStyle::Normal;
    if (style == g_trayIconStyle) return;

    auto nid   = makeTrayNid(hWnd);
    nid.uFlags = NIF_ICON;
    switch (style) {
    case TrayIconStyle::Disabled:
        nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON_DISABLE));
        break;
    case TrayIconStyle::NormalBadged:
        nid.hIcon = createBadgedIcon();
        if (!nid.hIcon)
            nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
        break;
    default:
        nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
        break;
    }
    // 成功時のみ前回状態を更新する。失敗時に先へ進めると、次回同じ状態の要求が差分なしと
    // 判定されてスキップされ、実アイコンが古いまま残り続けるため
    if (Shell_NotifyIconW(NIM_MODIFY, &nid)) g_trayIconStyle = style;
    if (nid.hIcon) DestroyIcon(nid.hIcon);
}

// トレイアイコンのツールチップをクリアする（ポップアップ表示前に呼ぶ）
static void clearTrayTooltip(HWND hWnd) {
    auto nid = makeTrayNid(hWnd);
    nid.uFlags  = NIF_TIP;
    nid.szTip[0] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 一覧に出す 1 行の素材（表示文字列の組み立て前）
// 生成側は指定初期化子で書く。同じ型のメンバが連続するため、位置指定だと
// 順序の取り違えが型エラーにならず表示バグとして黙って現れる
struct ListRow {
    int         id = 0;
    std::string subject;
    std::string projectName;          // 空 = 未取得（旧形式の pins.json 由来など）
    std::string updater;              // 最終更新者の姓（{lastname}。取得できない場合はフルネーム）
    std::string updaterFirst;         // 最終更新者の名（{firstname}。未取得は空）
    std::string dueDate;              // "YYYY-MM-DD"（期日なしは空）
    std::string updatedOn;
    bool        assignedToGroup = false;
    bool        isBugTracker    = false;
    bool        pinned          = false;
    bool        hidden          = false;  // 非表示チケット（グレー描画。件数・未読に数えない）
    bool        closed          = false;
    bool        unread          = false;
};

// 一覧に出す行を選定し、並べ替えて list_limit 件へ絞る
//
//   1. g_issues から表示フィルタ（担当者フィルタ AND バージョンフィルタ）を通った行をすべて採る
//      （フィルタで外れてもピン留め済みなら残す。ピンはすべてのフィルタより優先する）
//   2. 1 に含まれないピンを追加する（保存クエリの集合外ピンはキャッシュ内容で表示）
//   3. 全体を並べ替える（既定は updated_on 降順。「期日順に並べる」ON なら期日昇順で
//      期日なしは末尾。ピンも同じ規則で本来の位置に置く）
//   4. 先頭 list_limit 件へ絞る（ピン留めと非表示チケットは上限適用外で常に残す）
// 非表示チケット（g_hiddenIds）は「非表示チケットを除外」トグル ON なら行に出さず、
// OFF なら hidden フラグ付きで通す。（グレー参考表示。フィルタは通常行と同じく適用するが、
// list_limit の予算には数えない。枠を消費させると更新の多い非表示チケットが上位に浮上して
// 未読の通常行を窓外へ押し出し、バッジ・未読件数から消してしまうため）
// visible には表示フィルタを通った未処理件数（絞り込み前）を返す。ピン留めは数えない。
// （明示の意思表示であって未処理件数ではないため、フィルタで外れたピンを件数に足し戻さない）
// 非表示チケットもトグルにかかわらず数えない。（「見なくて良い」の意思表示のため）
// tooltip の未読件数も本関数の結果から数える。表示と同じ選定を通すことで「未読 N 件」と
// 画面上の太字行数を一致させる。（一覧に出ない未読は数に出さず、バッジも点けない）
// 非表示チケットの unread は常に false にする。（太字にも未読件数にも出さない）
static std::vector<ListRow> buildListRows(int& visible) {
    std::vector<Issue>      issues;
    std::vector<PinEntry>   pins;
    std::unordered_set<int> unread;
    std::unordered_set<int> hiddenIds;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        issues    = g_issues;
        pins      = g_pins;
        unread    = g_unreadIds;
        hiddenIds = g_hiddenIds;
    }

    auto isPinned = [&pins](int id) {
        for (const auto& p : pins) {
            if (p.id == id) return true;
        }
        return false;
    };

    std::vector<ListRow>    rows;
    std::unordered_set<int> shown;
    visible = 0;
    for (const auto& is : issues) {
        // 非表示チケットはトグル ON なら行ごと出さず、OFF ならグレー参考表示で通す。
        // どちらでも未処理件数（visible）には数えない。（ピンと違いフィルタは免除しない）
        bool hidden = hiddenIds.count(is.id) != 0;
        if (hidden && g_excludeHidden.load()) continue;
        // 表示フィルタで外れた行は出さない。ただしピン留め済みは明示の意思表示として常に残す
        // （クローズ済・集合外でも表示する既存のピン仕様と揃える）
        bool pinned = isPinned(is.id);
        bool excluded = !passesAssigneeFilter(is) || !passesVersionFilter(is);
        if (excluded) {
            if (!pinned) continue;
        }
        else if (!hidden) {
            ++visible;
        }
        rows.push_back({.id = is.id, .subject = is.subject, .projectName = is.projectName,
                        .updater = is.updaterDisplay, .updaterFirst = is.updaterFirstName,
                        .dueDate = is.dueDate,
                        .updatedOn = is.updatedOn, .assignedToGroup = is.assignedToGroup,
                        .isBugTracker = is.isBugTracker,
                        .pinned = pinned, .hidden = hidden, .closed = is.closed,
                        .unread = !hidden && unread.count(is.id) != 0});
        shown.insert(is.id);
    }
    for (const auto& p : pins) {
        if (shown.count(p.id)) continue;
        rows.push_back({.id = p.id, .subject = p.subject, .projectName = p.projectName,
                        .updater = p.updaterDisplay, .updaterFirst = p.updaterFirst,
                        .dueDate = p.dueDate,
                        .updatedOn = p.updatedOn, .assignedToGroup = p.assignedToGroup,
                        .isBugTracker = p.isBugTracker,
                        .pinned = true, .closed = p.closed,
                        .unread = unread.count(p.id) != 0});
    }

    if (g_sortByDue.load()) {
        // 期日は ISO 形式のため文字列辞書順で昇順に比較できる。期日なしは末尾へ回し、
        // 同順位は更新日時降順で安定させる
        std::sort(rows.begin(), rows.end(), [](const ListRow& a, const ListRow& b) {
            bool aNone = a.dueDate.empty(), bNone = b.dueDate.empty();
            if (aNone != bNone) return bNone;
            if (a.dueDate != b.dueDate) return a.dueDate < b.dueDate;
            return a.updatedOn > b.updatedOn;
        });
    }
    else {
        std::sort(rows.begin(), rows.end(),
            [](const ListRow& a, const ListRow& b) { return a.updatedOn > b.updatedOn; });
    }

    // 並べ替えの後に list_limit 件へ絞る。（ピン留めは上限適用外で常に残す）
    // 絞り込みを並べ替えの前に行うと、期日順 ON のとき「更新は古いが期日が近い」チケットが
    // 更新日時降順の窓から落ちて一覧に出ない。
    size_t limit = static_cast<size_t>(g_currentConfig.listLimit);
    if (rows.size() > limit) {
        std::vector<ListRow> kept;
        kept.reserve(rows.size());
        // 上限の予算は通常行（非ピン・非 hidden）だけで数える。kept.size() で判定すると
        // ピン行・非表示行が枠を消費し、通常行が list_limit より少なくなる。
        // （「最大 list_limit ＋ピン件数＋非表示件数」の約束が破れる）
        size_t normal = 0;
        for (auto& r : rows) {
            if (r.pinned || r.hidden) {
                kept.push_back(std::move(r));
            }
            else if (normal < limit) {
                kept.push_back(std::move(r));
                ++normal;
            }
        }
        rows = std::move(kept);
    }
    return rows;
}

// トレイアイコンの状態表示（ツールチップ・バッジ）を更新する
// 件数ツールチップは全廃済みで、通常時は空 tooltip の維持と赤バッジ（未読あり）の更新のみを行う。
// （件数は一覧ポップアップのフッタが、未読の有無はバッジが担う。ツールチップを出すと
// 同じホバー操作で一覧と重なって衝突する）
// 未読件数は一覧に出る行から数えるため、バッジの有無と画面上の太字行の有無が一致する。
// 無効モード中は原因別の設定確認案内をツールチップに表示する。（一覧が開けないため例外）
// ポップアップ表示中は更新しない
static void updateTrayTooltip(HWND hWnd) {
    if (g_popupShowing.load()) return;
    if (g_tooltipUpdating) return;
    g_tooltipUpdating = true;

    // 無効モード：件数の代わりに設定確認の案内を出す（一覧は開けないため件数計算も不要）
    if (isDisabled()) {
        std::wstring dtip;
        switch (disabledReason()) {
        case DisabledReason::InvalidUrl:      dtip = L"設定ファイルの url を確認してください"; break;
        case DisabledReason::InvalidApiKey:   dtip = L"設定ファイルの api_key を確認してください"; break;
        default:                              dtip = L"設定ファイルの query_ids を確認してください"; break;
        }
        auto dnid = makeTrayNid(hWnd);
        dnid.uFlags = NIF_TIP;
        wcscpy_s(dnid.szTip, dtip.c_str());
        Shell_NotifyIconW(NIM_MODIFY, &dnid);
        updateTrayIcon(hWnd, false);
        g_tooltipUpdating = false;
        return;
    }

    int visible = 0;
    auto rows = buildListRows(visible);
    int unread = 0;
    for (const auto& r : rows) {
        if (r.unread) ++unread;
    }

    // szTip は makeTrayNid のゼロ初期化で空文字列のまま送る。（tooltip なしを維持する）
    auto nid = makeTrayNid(hWnd);
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    updateTrayIcon(hWnd, unread > 0);
    g_tooltipUpdating = false;
}

// トレイアイコンを除去する（一覧ポップアップ関連タイマーも道連れに破棄する）
static void removeTrayIcon(HWND hWnd) {
    KillTimer(hWnd, IDT_HOVER_TRIGGER);
    KillTimer(hWnd, IDT_LIST_WATCH);
    auto nid = makeTrayNid(hWnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}


// 一覧・メニュー描画用フォントの初期化
// OS のメニューフォント設定を取得して、一覧ポップアップの行描画用フォントを作成する。
// （右クリックメニューのバージョン更新通知行のオーナードローも g_hMenuFont を共用する）
static void initMenuFonts() {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hMenuFont = CreateFontIndirectW(&ncm.lfMenuFont);
    // 未読行の太字はメニューフォントのウェイトだけ変えて作る（フェイス・サイズは揃える）
    LOGFONTW lfBold = ncm.lfMenuFont;
    lfBold.lfWeight = FW_BOLD;
    g_hMenuFontBold = CreateFontIndirectW(&lfBold);
    // ラベル内の一部だけ強調する用（期日と、期限切れの件名、経過日数）。未読行の太字より控えめな
    // ウェイトにして、行全体の太字（未読）と部分強調を見分けられるようにする。
    // 半太字は「<フェイス> Semibold」という別ファミリで、ウェイト値だけ FW_SEMIBOLD にしても
    // GDI は同一ファミリ内の近いウェイト＝太字へ丸める。（実測：Yu Gothic UI・Segoe UI とも 600 → 700）
    // そのためフェイス名で指定し、得られたウェイトとフェイスを検証して駄目なら太字へ落とす。
    LOGFONTW lfSemi = ncm.lfMenuFont;
    lfSemi.lfWeight = FW_SEMIBOLD;
    wcsncat_s(lfSemi.lfFaceName, L" Semibold", _TRUNCATE);
    g_hMenuFontSemiBold = CreateFontIndirectW(&lfSemi);
    if (g_hMenuFontSemiBold) {
        // フェイスが無い環境では GDI が別書体で代替してしまうため、フェイス名も突き合わせる
        HDC   hdc = GetDC(nullptr);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, g_hMenuFontSemiBold));
        TEXTMETRICW tm = {};
        wchar_t     face[LF_FACESIZE] = {};
        GetTextMetricsW(hdc, &tm);
        GetTextFaceW(hdc, LF_FACESIZE, face);
        SelectObject(hdc, old);
        ReleaseDC(nullptr, hdc);
        if (tm.tmWeight >= FW_BOLD || _wcsicmp(face, lfSemi.lfFaceName) != 0) {
            DeleteObject(g_hMenuFontSemiBold);
            g_hMenuFontSemiBold = nullptr;
            writeLog("menu font: semibold face unavailable, using bold for emphasis");
        }
    }
    // フォントは明示解放せずプロセス終了まで保持するため、太字ハンドルの共有で問題ない
    if (!g_hMenuFontSemiBold) g_hMenuFontSemiBold = g_hMenuFontBold;
}

// 注意を促す文字色（期限切れの期日、件名、経過日数と、バグマーカー。更新通知メニューの新バージョン表示と同じ赤）
static constexpr COLORREF ALERT_TEXT_COLOR = RGB(220, 0, 0);

// ラベル内で文字色とウェイトを変える範囲（オフセットと長さは UTF-16 コードユニット単位）
// bold と keepColor を組み合わせて「色は据え置き、ウェイトだけ変える」ような部分強調にも使える。
struct ColorRange {
    size_t   offset    = 0;
    size_t   len       = 0;
    COLORREF color     = 0;
    bool     bold      = false;  // 半太字で描く（範囲外との幅の差は走査側が吸収する）
    bool     keepColor = false;  // true なら color を無視して範囲外と同じ色で描く
};

// 一覧ポップアップのチケット項目（行レイアウトの index に対応、トレイ WndProc スレッドのみ使用）
struct IssueItem {
    int          id     = 0;
    std::wstring url;            // {redmine.url}/issues/{id}
    std::wstring label;          // 描画テキスト（drawIssueRow / measureIssueRow で使用）
    // label 内で文字色とウェイトを変える範囲（空 = 分割描画しない）。buildIssueLabel が組み立てる
    std::vector<ColorRange> ranges;
    bool         unread     = false; // 未読（まだ一覧から開いていない）＝太字で描く
    bool         pinned = false; // ピン留め中（マーカー列の描画条件。右クリック遷移時にもその場で更新する）
    bool         hidden = false; // 非表示（グレー描画の条件。右クリック遷移時にもその場で更新する）
    bool         closed = false; // クローズ済（打ち消し線の描画条件）
};
static std::vector<IssueItem> g_issueItems;

// ピンマーカーの文字列（一覧行の先頭マーカー列。全行で同じ幅を確保し、ピン留め行のみ描く）
// 📌 は非 BMP でフォントリンク（Segoe UI Emoji へのフォールバック）に頼る。
// 列幅の計測は GDI に一本化する。GDI のフォントリンクは GetTextExtentPoint32W にも効き、
// 実描画幅と一致するため DT_CALCRECT は不要。（メニューフォント Yu Gothic UI で実測確認済み）
// 実描画は drawPinMarkColor（DirectWrite/Direct2D）で行い、GDI はカラーフォントを解釈せず
// 単色になるため、D2D が使えない場合のみ従来の GDI DrawTextW にフォールバックする。
static constexpr wchar_t PIN_MARK[] = L"📌 ";

// ピンマーカーのカラー絵文字描画資源（初回描画時に遅延生成し、以降は再利用する）
// 明示解放はしない。プロセス終了まで保持する。（g_hMenuFont と同じ扱い）
// アクセスはトレイ WndProc スレッド（paintListWindow → drawIssueRow）のみでロック不要。
static ID2D1Factory1*        g_pD2DFactory  = nullptr;  // デバイス非依存（作り直し不要）
static IDWriteTextFormat*    g_pPinFormat   = nullptr;  // 同上（メニューフォント由来）
static ID2D1DCRenderTarget*  g_pPinRT       = nullptr;  // BindDC / BeginDraw / EndDraw 用
static ID2D1DeviceContext*   g_pPinCtx      = nullptr;  // g_pPinRT の QI。カラー描画に必須
static ID2D1SolidColorBrush* g_pPinBrush    = nullptr;
static bool                  g_pinD2DFailed = false;    // 失敗を記憶して行ごとの再試行を避ける

// COLORREF を D2D の正規化 RGB へ変換する
// D2D1::ColorF(UINT32) は 0xRRGGBB 前提で COLORREF（0x00BBGGRR）とバイト順が逆のため成分指定で作る。
static D2D1_COLOR_F toD2DColor(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f);
}

// ピンマーカー描画のデバイス依存資源の解放
// EndDraw が D2DERR_RECREATE_TARGET（表示構成変更等）を返した際に作り直すため一括で解放する。
static void releasePinD2DTarget() {
    if (g_pPinBrush) { g_pPinBrush->Release(); g_pPinBrush = nullptr; }
    if (g_pPinCtx)   { g_pPinCtx->Release();   g_pPinCtx   = nullptr; }
    if (g_pPinRT)    { g_pPinRT->Release();    g_pPinRT    = nullptr; }
}

// ピンマーカー描画資源の遅延初期化
// hdc は描画先の DC で、DirectWrite のフォントサイズをメニューフォントの
// 実効 em 高（px）へ合わせる計測にのみ使う。戻り値 false は GDI フォールバックを表す。
static bool ensurePinD2D(HDC hdc) {
    if (g_pinD2DFailed) return false;
    if (g_pPinCtx)      return true;

    // デバイス非依存資源（初回のみ）
    if (!g_pD2DFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory))) {
            g_pinD2DFailed = true;
            writeLog("pin marker: D2D1CreateFactory failed, fallback to GDI");
            return false;
        }
        // フェイス名と em 高は実際に GDI が描画に使う g_hMenuFont 自身から引く。（二重管理を避ける）
        // RT を 96 DPI 固定にするため 1 DIP = 1 px。lfHeight の符号解釈を避け、
        // GDI の実効 em 高（セル高 - internal leading）を渡して送り幅を GDI 計測値と揃える。
        LOGFONTW    lf = {};
        TEXTMETRICW tm = {};
        GetObjectW(g_hMenuFont, sizeof(lf), &lf);
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, oldFont);
        float emSize = static_cast<float>(tm.tmHeight - tm.tmInternalLeading);

        IDWriteFactory* pDWrite = nullptr;
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&pDWrite));
        if (SUCCEEDED(hr)) {
            hr = pDWrite->CreateTextFormat(lf.lfFaceName, nullptr,
                     DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, emSize, L"", &g_pPinFormat);
            pDWrite->Release();  // テキストフォーマットが内部でファクトリを保持するため手放して良い
        }
        if (FAILED(hr)) {
            g_pinD2DFailed = true;
            writeLog("pin marker: CreateTextFormat failed, fallback to GDI");
            return false;
        }
        g_pPinFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        g_pPinFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        // 行矩形内での縦センタリング（GDI 側 DrawTextW の DT_VCENTER と揃える）
        g_pPinFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // デバイス依存資源（D2DERR_RECREATE_TARGET 後はここから作り直す）
    // 96 DPI 固定で 1 DIP = 1 px とし、GDI 計測の px 矩形・フォントサイズと揃える。
    // SOFTWARE 指定は、20 px 程度の矩形に GPU 経路は無駄で、RDP 等での初期化失敗も避けるため。
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    HRESULT hr = g_pD2DFactory->CreateDCRenderTarget(&props, &g_pPinRT);
    // ENABLE_COLOR_FONT を解釈するのは D2D 1.1 のデバイスコンテキストのみ。
    // レガシー RT を QI して同一オブジェクトを 1.1 インタフェースとして扱う。
    if (SUCCEEDED(hr)) {
        hr = g_pPinRT->QueryInterface(__uuidof(ID2D1DeviceContext),
                                      reinterpret_cast<void**>(&g_pPinCtx));
    }
    if (SUCCEEDED(hr)) {
        hr = g_pPinRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &g_pPinBrush);
    }
    if (FAILED(hr)) {
        releasePinD2DTarget();
        g_pinD2DFailed = true;
        writeLog("pin marker: DC render target setup failed, fallback to GDI");
        return false;
    }
    return true;
}

// ピンマーカー（📌）をカラー絵文字で描画する
// rect はマーカー列の矩形。（幅は GDI 計測値）bgColor はメニュー背景色（AA の合成先を実背景に
// 一致させる）、fgColor はカラーフォント非対応で単色描画に落ちた場合の文字色。
// 戻り値 false は GDI フォールバックが必要なことを表す。
static bool drawPinMarkColor(HDC hdc, const RECT& rect, COLORREF bgColor, COLORREF fgColor) {
    if (!ensurePinD2D(hdc)) return false;
    // BindDC のサブ矩形でマーカー列だけを描画対象にする。RT 原点が rect 左上に対応し、
    // EndDraw のブリット範囲もこの矩形に限られるためラベル側の描画を壊さない。
    if (FAILED(g_pPinRT->BindDC(hdc, &rect))) return false;
    g_pPinBrush->SetColor(toD2DColor(fgColor));
    g_pPinRT->BeginDraw();
    g_pPinRT->Clear(toD2DColor(bgColor));  // BindDC は DC の既存内容を引き継がないため下地を塗る
    D2D1_RECT_F layout = D2D1::RectF(0.0f, 0.0f,
        static_cast<float>(rect.right - rect.left),
        static_cast<float>(rect.bottom - rect.top));
    g_pPinCtx->DrawText(PIN_MARK, static_cast<UINT32>(wcslen(PIN_MARK)), g_pPinFormat,
                        layout, g_pPinBrush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    if (FAILED(g_pPinRT->EndDraw())) {
        releasePinD2DTarget();  // ターゲット失効時は解放し、次回描画で作り直す
        return false;
    }
    return true;
}

// フォアグラウンド権限を確実に取得するユーティリティ
// 起動直後は自プロセスがフォアグラウンド権限を持たないため SetForegroundWindow が失敗する。
// 現フォアグラウンドスレッドの入力キューに一時アタッチして権限制限を回避する。
static void forceForeground(HWND hWnd) {
    HWND  hFg   = GetForegroundWindow();
    DWORD fgTid = hFg ? GetWindowThreadProcessId(hFg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();
    if (fgTid != 0 && fgTid != myTid) {
        AttachThreadInput(myTid, fgTid, TRUE);
        SetForegroundWindow(hWnd);
        AttachThreadInput(myTid, fgTid, FALSE);
        return;
    }
    SetForegroundWindow(hWnd);
}

// 表示用文字列を最大文字数で省略する（超過分を切り詰める）
// UTF-16 単位で数えるため、サロゲートペアの途中で切らないよう上位サロゲート終わりは 1 つ手前で切る。
// ellipsis=true（デフォルト）で切り詰めた印の "…" を末尾に付ける。
static std::wstring truncateText(const std::wstring& s, size_t maxChars, bool ellipsis = true) {
    if (s.size() <= maxChars) return s;
    size_t cut = maxChars;
    if (cut > 0 && s[cut - 1] >= 0xD800 && s[cut - 1] <= 0xDBFF) --cut;
    return ellipsis ? s.substr(0, cut) + L"…" : s.substr(0, cut);
}

// 一覧行のラベル（描画テキストと、その中で色とウェイトを変える範囲）
// セグメント文字列を別々に持つと text 全体との不整合が起き得るため、text を単一の真実とし
// 位置だけを保持する。幅計測と打ち消し線の長さは walkIssueLabel の走査で求める。
// ranges はオフセット昇順で重なりなく並べる。（描画側が順に走査する前提）
struct IssueLabel {
    std::wstring            text;
    std::vector<ColorRange> ranges;
};

// グループ担当マーカー（👥 + 半角スペース）
// ラベルに埋め込み GDI で描く。フォントリンク経由の単色描画で足りるため、
// 📌 のような D2D カラー描画はしない。（単色でも輪郭が明瞭で意味が通る）
static constexpr wchar_t GROUP_MARK[] = L"👥 ";

// バグ・障害トラッカーのマーカー（💥 + 半角スペース）
// 扱いは GROUP_MARK と同じ。（ラベル埋め込みの GDI 単色描画）
static constexpr wchar_t BUG_MARK[] = L"💥 ";

// 一覧行のラベルを組み立てる
//   並びは list_format（g_currentConfig.listFormat）のトークン列に従う。
//   既定は「番号、姓、グループ担当マーカー、[プロジェクト名]、期日、バグマーカー、件名、経過日数」
//   （例："#12345  山田  👥 [ロケモニ] 7/28 💥 件名…（3 日前）"）。
//   {要素:N} の最大文字数で切り詰める。「…」は自由文の {subject} のみ付け、
//   識別子的な要素（プロジェクト名・姓・名など）は横幅を優先して付けない。
//   空に展開された要素（期限なし・更新者不明など）は、直後のリテラル先頭空白を
//   出力末尾が空白または行頭なら取り除いて詰める。（従来の「詰めて省く」を再現）
//   {group}＝"👥 "・{bug}＝"💥 "・{ago}＝"（3 日前）" は装飾込みで展開し、空なら装飾ごと消える。
//   期限切れの期日、件名、経過日数と、常に赤いバグマーカーは ALERT_TEXT_COLOR で描くため、
//   位置を ranges に記録する。
//   （ranges はオフセット昇順で並べる契約。展開順の追記がそのまま昇順になる）
// 引数に ListRow を丸ごと取るのは、同じ型の要素が増えて位置引数では取り違えを防げないため。
// ピン記号はラベルに含めない。drawIssueRow が IssueItem::pinned を見てマーカー列に描く。
static IssueLabel buildIssueLabel(const ListRow& row, const DueDateView& due,
                                  long long todayDays) {
    IssueLabel r;
    bool prevEmpty = false;  // 直前のプレースホルダが空に展開されたか（空白整理の条件）
    for (const auto& tk : g_currentConfig.listFormat) {
        if (tk.element == FMT_LITERAL) {
            std::wstring lit = tk.literal;
            if (prevEmpty && (r.text.empty() || r.text.back() == L' ')) {
                size_t i = 0;
                while (i < lit.size() && lit[i] == L' ') ++i;
                lit.erase(0, i);
            }
            if (!lit.empty()) {
                r.text += lit;
                prevEmpty = false;
            }
            continue;
        }
        std::wstring val;
        switch (tk.element) {
        case FMT_ID:        val = std::to_wstring(row.id); break;
        case FMT_LASTNAME:  val = toWide(row.updater); break;
        case FMT_FIRSTNAME: val = toWide(row.updaterFirst); break;
        case FMT_GROUP:     if (row.assignedToGroup) val = GROUP_MARK; break;
        case FMT_PROJECT:   val = toWide(row.projectName); break;
        case FMT_DUE:       val = due.text; break;
        case FMT_BUG:       if (row.isBugTracker) val = BUG_MARK; break;
        case FMT_SUBJECT:   val = toWide(row.subject); break;
        case FMT_AGO:       val = makeUpdatedAgoText(row.updatedOn, todayDays); break;
        }
        if (tk.maxChars > 0)
            val = truncateText(val, static_cast<size_t>(tk.maxChars), tk.element == FMT_SUBJECT);
        if (val.empty()) {
            prevEmpty = true;
            continue;
        }
        if (tk.element == FMT_DUE) {
            // 期日は常に半太字で強調する。（一覧の中で期日行を素早く拾えるようにする）
            // 期限切れは加えて赤にし、期限内は色を据え置く。（範囲外と同じ色で描く）
            ColorRange range{r.text.size(), val.size(), ALERT_TEXT_COLOR, true};
            if (!due.overdue) range.keepColor = true;
            r.ranges.push_back(range);
        }
        if ((tk.element == FMT_SUBJECT || tk.element == FMT_AGO) && due.overdue) {
            // 期限切れは件名と経過日数も赤の半太字にする。（期日だけでは行全体の緊急度に気付けない）
            // 判定は期日と同じ overdue を使う。
            // {due} を持たない書式では、件名と経過日数だけが赤になる。
            r.ranges.push_back({r.text.size(), val.size(), ALERT_TEXT_COLOR, true});
        }
        if (tk.element == FMT_BUG) {
            // マーカー自体を赤くする。（絵文字は GDI が現在の文字色で単色描画するため色が乗る）
            // 末尾の空白は色を変えても見えないので範囲に含めない
            size_t len = val.size() - (val.back() == L' ' ? 1 : 0);
            if (len > 0) r.ranges.push_back({r.text.size(), len, ALERT_TEXT_COLOR});
        }
        r.text += val;
        prevEmpty = false;
    }
    while (!r.text.empty() && r.text.back() == L' ') r.text.pop_back();
    return r;
}

// トレイポップアップの表示位置とアライメントを算出する
//
// タスクバーが配置された辺（下・上・左・右）にポップアップを密着させて表示する。
// タスクバーに沿った軸（水平タスクバーなら X、垂直なら Y）はカーソル位置を起点とする。
// 画面端超過の扱いは呼び出し側の責務。（右クリックメニューは TrackPopupMenu の自動反転、
// 一覧ポップアップは showListPopup がモニタ作業領域へクランプする）
// SHAppBarMessage 失敗時や uEdge が想定外なら現状挙動（カーソル位置＋左上アライメント）
// に戻し、必ずポップアップが出るようにする。
struct TrayPopupPos {
    int  x;
    int  y;
    UINT alignFlags;  // TPM_ アライメントのみ。ボタン系（TPM_LEFTBUTTON 等）は呼び出し側で OR する
};
static TrayPopupPos computeTrayPopupPos(const POINT& cursor) {
    APPBARDATA abd = { sizeof(abd) };
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) {
        return { cursor.x, cursor.y, TPM_LEFTALIGN | TPM_TOPALIGN };
    }
    switch (abd.uEdge) {
    case ABE_BOTTOM:
        // 底辺をタスクバー上端に密着、カーソル X から右方向に展開
        return { cursor.x, abd.rc.top,    TPM_LEFTALIGN  | TPM_BOTTOMALIGN };
    case ABE_TOP:
        // 上辺をタスクバー下端に密着、カーソル X から右方向に展開
        return { cursor.x, abd.rc.bottom, TPM_LEFTALIGN  | TPM_TOPALIGN };
    case ABE_LEFT:
        // 左辺をタスクバー右端に密着、カーソル Y から下方向に展開
        return { abd.rc.right, cursor.y, TPM_LEFTALIGN  | TPM_TOPALIGN };
    case ABE_RIGHT:
        // 右辺をタスクバー左端に密着、カーソル Y から下方向に展開
        return { abd.rc.left,  cursor.y, TPM_RIGHTALIGN | TPM_TOPALIGN };
    default:
        return { cursor.x, cursor.y, TPM_LEFTALIGN | TPM_TOPALIGN };
    }
}

// トレイアイコンの画面矩形を取得する
// 失敗（アイコン未登録・過渡状態）は false を返し、呼び出し側は判定を保守的に扱う。
static bool getTrayIconRect(HWND hWnd, RECT& rcOut) {
    NOTIFYICONIDENTIFIER nii = { sizeof(nii) };
    nii.hWnd = hWnd;
    nii.uID  = 1;
    return SUCCEEDED(Shell_NotifyIconGetRect(&nii, &rcOut));
}

// 一覧ポップアップの表示・非表示・可視判定（実装は walkIssueLabel 等の描画基盤の後方にある）
static void showListPopup(HWND trayWnd);
static void hideListPopup(HWND trayWnd);
static bool isListPopupVisible();

// ==================== 更新チェック ====================

// バージョン文字列から数値の MAJOR.MINOR.PATCH を抽出する
// "v2.7.4" / "2.7.4-dirty" / "2.7.4-5-gHASH" のいずれにも対応する
static bool parseVersion(const std::wstring& ver, int& major, int& minor, int& patch) {
    std::wstring s = ver;
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s = s.substr(1);
    auto dashPos = s.find(L'-');
    if (dashPos != std::wstring::npos) s = s.substr(0, dashPos);
    int a = 0, b = 0, c = 0;
    if (swscanf_s(s.c_str(), L"%d.%d.%d", &a, &b, &c) != 3) return false;
    major = a; minor = b; patch = c;
    return true;
}

// a が b より新しいバージョンなら true を返す
// どちらかが解釈不能なら false（新しくない側）へ倒す。（更新誤検知を防ぐ安全側）
static bool isNewerVersion(const std::wstring& a, const std::wstring& b) {
    int aMaj, aMin, aPat, bMaj, bMin, bPat;
    if (!parseVersion(a, aMaj, aMin, aPat)) return false;
    if (!parseVersion(b, bMaj, bMin, bPat)) return false;
    if (aMaj != bMaj) return aMaj > bMaj;
    if (aMin != bMin) return aMin > bMin;
    return aPat > bPat;
}

// ワーカースレッド用 WinRT アパートメントの RAII ガード
// init_apartment は hresult_error を投げ得る。スレッドの未捕捉例外は std::terminate で
// プロセス全体を落とすため、例外を吸収して成否を ok に持つ。呼び出し側は ok が false なら
// ログを残してスレッドを終える。デストラクタは初期化成功時のみ uninit_apartment を呼ぶので、
// 早期 return でも解放漏れ・過剰解放が起きない。
struct WinRtApartment {
    bool ok = false;
    WinRtApartment() {
        try {
            winrt::init_apartment();
            ok = true;
        }
        catch (...) {}
    }
    ~WinRtApartment() {
        if (ok) winrt::uninit_apartment();
    }
};

// GitHub の最新リリースを確認し、新版があれば Toast 通知とグローバル状態を更新する
// 起動時のスレッドで 1 回だけ実行する。スレッドは detach せず wmain が終了時に join する。
// （detach だとプロセス終了時の静的破棄と実行中の本関数が競合し、破棄済みの
// g_mtx・g_latestVersion・g_logDir 等に触れる未定義動作になり得るため）
static void checkForUpdates() {
    WinRtApartment apartment;
    if (!apartment.ok) {
        writeLog("update check: WinRT init failed");
        return;
    }
    // 予期しない例外でスレッドが std::terminate しないよう全体を保護する
    try {
        do {
            DWORD status = 0;
            std::string body = httpGet(GITHUB_API_RELEASES_LATEST, &status);
            // HTTP 中にシャットダウンが始まっていたら以降（Toast・レジストリ書き込み）を
            // 行わない。終了操作の直後に更新 Toast が出る違和感と、join 待ちの延伸を防ぐ
            if (g_shutdownRequested) break;
            if (status != 200 || body.empty()) {
                writeLog("update check: request failed, status=" + std::to_string(status));
                break;
            }

            std::wstring tagName;
            try {
                auto json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                tagName = json.GetNamedString(L"tag_name");
            }
            catch (...) {
                writeLog("update check: JSON parse failed");
                break;
            }
            if (tagName.empty()) {
                writeLog("update check: tag_name empty");
                break;
            }

            // 現在版より新しければグローバル状態を更新
            if (!isNewerVersion(tagName, APP_VERSION)) break;

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_latestVersion = tagName;
            }
            g_updateAvailable.store(true);
            writeLog("update available: " + wideToUtf8(tagName));

            // Toast: 同一版は 1 回のみ（通知済み版をレジストリに先書きし、変化があれば通知）
            std::wstring notifiedVer = readRegString(REG_NOTIFIED_VERSION);
            writeRegString(REG_NOTIFIED_VERSION, tagName);
            if (notifiedVer != tagName) {
                try {
                    showToast3(L"新しいバージョンがあります",
                               std::wstring(L"v") + APP_VERSION + L" → " + tagName,
                               L"ボタンからリリースページを開いてください",
                               GITHUB_RELEASES_URL, false, L"リリースページを開く");
                }
                catch (...) {}
            }
        } while (false);
    }
    catch (...) {
        writeLog("update check: unexpected exception");
    }
}

// 更新通知メニュー項目のサイズを計算する
// GetDC 失敗時は既定サイズ（200×20）を返し、項目自体は必ず描かせる。
static BOOL measureVersionMenuItem(HWND hWnd, MEASUREITEMSTRUCT* mis) {
    std::wstring prefix = std::wstring(L"redntfy v") + APP_VERSION + L" → ";
    std::wstring latest;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        latest = g_latestVersion;
    }
    std::wstring full = prefix + latest;
    HDC hdc = GetDC(hWnd);
    if (!hdc) {
        mis->itemWidth  = 200;
        mis->itemHeight = 20;
        return TRUE;
    }
    HFONT old = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
    SIZE  sz  = {};
    GetTextExtentPoint32W(hdc, full.c_str(), static_cast<int>(full.size()), &sz);
    SelectObject(hdc, old);
    ReleaseDC(hWnd, hdc);
    mis->itemWidth  = static_cast<UINT>(sz.cx) + 32;
    mis->itemHeight = static_cast<UINT>(sz.cy) + 6;
    return TRUE;
}

// 更新通知メニュー項目を描画する
// プレフィックス部分を通常色、新バージョン部分を赤色で描く
static BOOL drawVersionMenuItem(DRAWITEMSTRUCT* dis) {
    std::wstring prefix = std::wstring(L"redntfy v") + APP_VERSION + L" → ";
    std::wstring latest;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        latest = g_latestVersion;
    }
    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    FillRect(dis->hDC, &dis->rcItem,
        reinterpret_cast<HBRUSH>(
            static_cast<INT_PTR>(selected ? COLOR_HIGHLIGHT + 1 : COLOR_MENU + 1)));

    RECT textRect = dis->rcItem;
    textRect.left += 16;
    SetBkMode(dis->hDC, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, g_hMenuFont));

    // プレフィックス部分（通常色）
    SetTextColor(dis->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));
    SIZE prefixSz = {};
    GetTextExtentPoint32W(dis->hDC, prefix.c_str(), static_cast<int>(prefix.size()), &prefixSz);
    RECT prefixRect = textRect;
    DrawTextW(dis->hDC, prefix.c_str(), -1, &prefixRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    // 新バージョン部分（選択時はハイライトテキスト色、通常時は赤）
    RECT newVerRect = textRect;
    newVerRect.left += prefixSz.cx;
    SetTextColor(dis->hDC, selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : ALERT_TEXT_COLOR);
    DrawTextW(dis->hDC, latest.c_str(), -1, &newVerRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    SelectObject(dis->hDC, oldFont);
    return TRUE;
}

// トレイ右クリックメニューの構築と表示
// メニュー項目はトグル状態（音声通知・スタートアップ等）を読み取り、
// その場で構築する。（チェック状態は呼び出し時の最新値を反映）
static void showTrayContextMenu(HWND hWnd) {
    // 一覧ポップアップが出ていれば先に閉じる。（メニューと重なるのを防ぎ、メニュー終了時の
    // g_popupShowing.store(false) が可視の一覧とフラグを食い違わせるのも防ぐ）
    if (isListPopupVisible()) hideListPopup(hWnd);
    g_popupShowing.store(true);
    clearTrayTooltip(hWnd);
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        writeLog("showTrayContextMenu: CreatePopupMenu failed");
        g_popupShowing.store(false);
        updateTrayTooltip(hWnd);
        return;
    }
    if (g_updateAvailable.load()) {
        // 新版あり：オーナードローで "redntfy vX.Y.Z → vNew" を赤文字で表示する
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_FTYPE | MIIM_ID;
        mii.fType = MFT_OWNERDRAW;
        mii.wID   = IDM_OPEN_GITHUB;
        InsertMenuItemW(hMenu, 0, TRUE, &mii);
    }
    else {
        AppendMenuW(hMenu, MF_STRING, IDM_OPEN_GITHUB, L"redntfy v" APP_VERSION);
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 即時ポーリング（明示操作のため休止時間帯・クールダウンを無視する。g_manualPoll 経由）
    // 無効モード中は消費するポーリングスレッドが存在しないため非活性にする
    UINT updateNowFlags = MF_STRING | (isDisabled() ? (MF_DISABLED | MF_GRAYED) : 0u);
    AppendMenuW(hMenu, updateNowFlags, IDM_UPDATE_NOW, L"今すぐ更新");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 担当者フィルタ（レジストリ永続化。一覧・tooltip・通知のすべてに効く）
    AppendMenuW(hMenu, MF_STRING | (g_assignedToMeOnly ? MF_CHECKED : MF_UNCHECKED),
        IDM_ASSIGNED_TO_ME, L"担当がグループのチケットを除外");

    // バージョンフィルタ（レジストリ永続化。期日ありは例外的に残す）
    AppendMenuW(hMenu, MF_STRING | (g_excludeNoVersion ? MF_CHECKED : MF_UNCHECKED),
        IDM_EXCLUDE_NO_VERSION, L"バージョン未指定のチケットを除外");

    // 非表示チケットの除外（レジストリ永続化。OFF はグレーで参考表示）
    AppendMenuW(hMenu, MF_STRING | (g_excludeHidden ? MF_CHECKED : MF_UNCHECKED),
        IDM_EXCLUDE_HIDDEN, L"非表示チケットを除外");

    // 自分の操作による起票・更新の通知抑止（レジストリ永続化。既定 ON）
    AppendMenuW(hMenu, MF_STRING | (g_muteOwnChanges ? MF_CHECKED : MF_UNCHECKED),
        IDM_MUTE_OWN_CHANGES, L"自分の操作による更新を通知しない");

    // 一覧の並び順トグル（レジストリ永続化。ON で期日昇順、期日なしは末尾）
    AppendMenuW(hMenu, MF_STRING | (g_sortByDue ? MF_CHECKED : MF_UNCHECKED),
        IDM_SORT_BY_DUE, L"期日順に並べる");

    // 音声通知（親：レジストリ永続化）
    AppendMenuW(hMenu, MF_STRING | (g_soundEnabled ? MF_CHECKED : MF_UNCHECKED),
        IDM_SOUND_ENABLED, L"通知音を鳴らす");

    // 子項目：親が OFF なら非活性
    UINT childFlags = g_soundEnabled ? 0u : (MF_DISABLED | MF_GRAYED);
    AppendMenuW(hMenu, MF_STRING | childFlags | (g_muteInMeeting ? MF_CHECKED : MF_UNCHECKED),
        IDM_MUTE_IN_MEETING, L"　　マイク/カメラ使用中は無効にする");

    // ホバーで一覧を自動表示するトグル（レジストリ永続化。既定 ON。OFF でも左クリックでは開ける）
    AppendMenuW(hMenu, MF_STRING | (g_hoverPopupEnabled ? MF_CHECKED : MF_UNCHECKED),
        IDM_HOVER_POPUP, L"マウスホバーで一覧を自動表示");

    // スタートアップ登録トグル（HKCU Run キー）
    AppendMenuW(hMenu, MF_STRING | (isStartupRegistered() ? MF_CHECKED : MF_UNCHECKED),
        IDM_STARTUP, L"スタートアップ登録");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    // 設定に詰まったユーザの導線のため、無効モードでも活性のままにする
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_GUIDE,  L"使い方ガイド");
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_CONFIG, L"設定ファイルを開く");
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_LOG,    L"ログファイルを開く");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT,    L"終了");
    auto pos = computeTrayPopupPos(pt);
    forceForeground(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | pos.alignFlags, pos.x, pos.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
    g_popupShowing.store(false);
    updateTrayTooltip(hWnd);
}

// トレイアイコンホバー時のチケット一覧表示
// 契約：IDT_HOVER_TRIGGER の発火（または hover_delay_ms = 0 の即時経路）からのみ呼ばれる。
// 無効モード・トグル OFF（「マウスホバーで一覧を自動表示」）・表示中・再アーム保留中（明示クローズ後、
// カーソルがアイコンを離れるまで）は無反応。発火時点でカーソルがアイコン矩形内に留まって
// いるかを再確認してから表示する。（遅延中の離脱で発火した空タイマー対策）
static void handleTrayHover(HWND hWnd) {
    if (isDisabled())                 return;
    if (!g_hoverPopupEnabled.load())  return;
    if (g_popupShowing.load())        return;
    if (g_hoverRearmPending)          return;

    RECT icon;
    if (!getTrayIconRect(hWnd, icon)) return;
    POINT pt;
    GetCursorPos(&pt);
    if (!PtInRect(&icon, pt)) return;

    showListPopup(hWnd);
    // ホバー起点の時刻を記録する。（左クリックの「閉じる」猶予判定用）
    // 表示が成立したときだけ記録し、不変条件「非 0 はホバー起点の一覧が表示中のときだけ」を
    // 保つ。（クローズ側の hideListPopup が 0 に戻す）
    if (g_popupShowing.load()) g_hoverShownAt = GetTickCount64();
}

// トレイアイコン左クリック時の処理
// 一覧ポップアップのトグル：表示中なら閉じ、非表示なら遅延なしで即表示する。
// ただしホバー自動表示から hover_click_guard_ms（0 で無効）以内の左クリックは無視する。
// （一覧を出すつもりのクリックの直前にホバー表示が割り込むと、クリックが「閉じる」に
// 化けて「クリックしたのに何も出ない」体験になるため。左クリックで表示した場合は
// 意図が明確なので猶予なしで直ちに閉じられる。右クリックメニューは対象外）
// ポップアップは非アクティブでマウスキャプチャも取らないため、アイコンのクリックは
// 表示中でも通常どおりここへ届く。（モーダルメニュー時代の解除補正は不要）
// 無効モード中は表示しない。（ポーリング停止中で出すものがない）
static void handleTrayLeftClick(HWND hWnd) {
    if (g_popupShowing.load()) {
        DWORD guard = g_hoverClickGuardMs.load();
        if (g_hoverShownAt != 0 && guard != 0 &&
            GetTickCount64() - g_hoverShownAt < guard) {
            return;
        }
        hideListPopup(hWnd);
        return;
    }
    if (isDisabled()) return;
    showListPopup(hWnd);
    g_hoverShownAt = 0;  // 左クリック起点は猶予なし（直ちに左クリックで閉じられる）
}

// 当日ログファイルのパスを取得し、存在しなければ logs フォルダのパスを返す
//
// 「当日」は JST 基準で判定する。（writeLog の日付ロールオーバ判定と同じ基準）
static std::wstring getCurrentLogTarget() {
    if (g_logDir.empty()) return {};
    SYSTEMTIME st;
    GetSystemTime(&st);
    st = utcToJst(st);
    char dateBuf[12];
    sprintf_s(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    std::wstring logPath = g_logDir + L"\\" + toWide(dateBuf) + L".log";
    DWORD attr = GetFileAttributesW(logPath.c_str());
    bool logExists = (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    return logExists ? logPath : g_logDir;
}

// 開いたチケット 1 件を既読にする
// 行クリック時点では一覧ポップアップがまだ可視で、g_popupShowing 中の直接呼びは
// updateTrayTooltip に捨てられる。PostMessage でキューに積んでおけば、
// この後の hideListPopup（フラグ解除）を経てから必ず反映される。
static void markIssueRead(int issueId) {
    bool wasUnread;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        wasUnread = g_unreadIds.erase(issueId) != 0;
    }
    if (wasUnread && g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
}

// WM_COMMAND ディスパッチ
// 右クリックメニューの選択（IDM_*）を処理する。（一覧の行操作は listWndProc が直接扱う）
static void handleTrayCommand(UINT id) {
    if (id == IDM_UPDATE_NOW) {
        if (isDisabled()) return;  // メニュー非活性と揃えた二重ガード
        // 明示のユーザ操作のため、休止時間帯・クールダウンを無視して直ちに再取得する
        g_manualPoll.store(true);
        return;
    }
    if (id == IDM_SORT_BY_DUE) {
        g_sortByDue.store(!g_sortByDue.load());
        writeRegDword(REG_SORT_BY_DUE, g_sortByDue.load() ? 1u : 0u);
        return;
    }
    if (id == IDM_EXIT) {
        g_shutdownRequested = true;
        PostQuitMessage(0);
        return;
    }
    if (id == IDM_ASSIGNED_TO_ME) {
        g_assignedToMeOnly.store(!g_assignedToMeOnly.load());
        writeRegDword(REG_ASSIGNED_TO_ME, g_assignedToMeOnly.load() ? 1u : 0u);
        // 未読は消さない。未読件数は一覧に出る行から数えるため、切り替えても件数と
        // 一覧の太字は食い違わない。（表示条件を変えただけで読んだことにはならない）
        // 一覧は次に開いた時点で g_issues から組み直されるが、tooltip とバッジは即時に更新する。
        // 取得済みの全件を保持しているため再ポーリングは不要。
        if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
        return;
    }
    if (id == IDM_MUTE_OWN_CHANGES) {
        g_muteOwnChanges.store(!g_muteOwnChanges.load());
        writeRegDword(REG_MUTE_OWN_CHANGES, g_muteOwnChanges.load() ? 1u : 0u);
        // 通知経路のみを切り替える設定で、一覧・tooltip・バッジは影響を受けない。
        // 表示側の再計算も再ポーリングも不要。次回ポーリング以降の通知に効く。
        return;
    }
    if (id == IDM_EXCLUDE_NO_VERSION) {
        g_excludeNoVersion.store(!g_excludeNoVersion.load());
        writeRegDword(REG_EXCLUDE_NO_VERSION, g_excludeNoVersion.load() ? 1u : 0u);
        // 担当者フィルタと同じ扱い。未読は消さず、tooltip・バッジは即時更新、再ポーリングは不要
        if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
        return;
    }
    if (id == IDM_EXCLUDE_HIDDEN) {
        g_excludeHidden.store(!g_excludeHidden.load());
        writeRegDword(REG_EXCLUDE_HIDDEN, g_excludeHidden.load() ? 1u : 0u);
        // 件数・未読・通知はトグルと無関係に常に非表示チケットを除外しているため変化しない。
        // （非表示行は list_limit の予算外で、切り替えても通常行の窓は動かない）
        // 一覧は次に開いた時点で組み直されるが、経路は他のフィルタ系トグルと揃えておく
        if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
        return;
    }
    if (id == IDM_SOUND_ENABLED) {
        g_soundEnabled.store(!g_soundEnabled.load());
        writeRegDword(REG_SOUND_ENABLED, g_soundEnabled.load() ? 1u : 0u);
        return;
    }
    if (id == IDM_MUTE_IN_MEETING) {
        // 音声通知 OFF 中はグレーアウト項目への誤クリックを無視する
        if (g_soundEnabled.load()) {
            g_muteInMeeting.store(!g_muteInMeeting.load());
            writeRegDword(REG_MUTE_IN_MEETING, g_muteInMeeting.load() ? 1u : 0u);
        }
        return;
    }
    if (id == IDM_HOVER_POPUP) {
        // ホバー発火のゲート 2 箇所（trayWndProc の WM_MOUSEMOVE のアーム条件と
        // handleTrayHover の先頭ガード）に効く。KillTimer は投函済み WM_TIMER を
        // 除去しないが、遅れて発火しても先頭ガードが止めるため OFF は直ちに効く。
        g_hoverPopupEnabled.store(!g_hoverPopupEnabled.load());
        writeRegDword(REG_HOVER_POPUP, g_hoverPopupEnabled.load() ? 1u : 0u);
        return;
    }
    if (id == IDM_STARTUP) {
        if (isStartupRegistered()) unregisterStartup();
        else                       registerStartup();
        return;
    }
    if (id == IDM_OPEN_GUIDE) {
        ShellExecuteW(nullptr, L"open", GUIDE_URL, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_GITHUB) {
        const wchar_t* url = g_updateAvailable.load() ? GITHUB_RELEASES_URL : GITHUB_URL;
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_QUERY) {
        // Redmine の代表画面（queryUrl）をブラウザで開く。
        // 無効モード中は一覧が開けず本来到達しないが、url が isHttpUrl 未検証のまま
        // ShellExecuteW へ渡る経路を将来にわたり残さないための直接ガード
        if (isDisabled()) return;
        std::wstring url = queryUrl(g_currentConfig);
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_CONFIG) {
        // 設定ファイルを OS デフォルトのエディタで開く。（変更反映には再起動が必要）
        // 実質的な設定（接続情報）は local 側にあるため、存在すれば local を優先して開く
        std::wstring localToml = getExeDir() + L"\\" + CONFIG_LOCAL_FILENAME;
        std::wstring toml = PathFileExistsW(localToml.c_str())
            ? localToml : getExeDir() + L"\\" + CONFIG_FILENAME;
        ShellExecuteW(nullptr, L"open", toml.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_LOG) {
        auto target = getCurrentLogTarget();
        if (!target.empty())
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
}

// ラベルを色範囲の境界でセグメントに分けて走査し、総幅を返す
//
// 範囲ごとにフォントを切り替えるため、幅は各セグメントの合算になる。計測と描画で同じ走査を
// 通すことが要点で、片方だけ「ラベル全体を 1 回計測」に戻すと行幅・取消線と実描画幅が食い違う。
// セグメント境界は空白位置に来ることが多いが、期限切れの件名のように本文中で分かれる場合もある。
// 分割による字形整形の差は数 px にとどまるため受容する。
// textRect が nullptr なら描画せず幅だけを返す。（measureIssueRow 用）
// uniformColor=true はホット行用で、範囲の色を無視して textColor 一色で描く。
// （ハイライト背景上の赤は読みにくい。ウェイトは幅が変わるためホット行でも維持する）
static int walkIssueLabel(HDC hdc, const IssueItem& item, const RECT* textRect,
                          COLORREF textColor, HFONT baseFont, HFONT emphFont,
                          bool uniformColor) {
    // DT_NOPREFIX がないと件名中の & がニーモニック指定として食われ、次の文字に下線が付く
    // （幅は & を 1 文字として計測するため、描画幅とのずれで取消線も伸び過ぎる）
    constexpr UINT DT_ROW = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX;
    const wchar_t* base = item.label.c_str();
    int x = 0;
    auto segment = [&](size_t from, size_t to, COLORREF color, HFONT font) {
        // 範囲の指定を信用せずラベル長で丸める。（境界外読み取りと位置の巻き戻りを防ぐ）
        to = (std::min)(to, item.label.size());
        if (from >= to) return;
        SelectObject(hdc, font);
        int len = static_cast<int>(to - from);
        SIZE sz = {};
        GetTextExtentPoint32W(hdc, base + from, len, &sz);
        if (textRect) {
            RECT seg = *textRect;
            seg.left += x;
            SetTextColor(hdc, uniformColor ? textColor : color);
            DrawTextW(hdc, base + from, len, &seg, DT_ROW);
        }
        x += sz.cx;
    };
    size_t done = 0;
    for (const auto& r : item.ranges) {
        segment(done, r.offset, textColor, baseFont);
        segment(r.offset, r.offset + r.len,
                r.keepColor ? textColor : r.color, r.bold ? emphFont : baseFont);
        done = r.offset + r.len;
    }
    segment(done, item.label.size(), textColor, baseFont);
    return x;
}

// 一覧行のラベルに使うフォントの組を返す
// 未読行は行全体が太字のため、部分強調にはそれ以上のウェイトが無い。太字のまま据え置く。
static void issueLabelFonts(bool unread, HFONT& baseFont, HFONT& emphFont) {
    baseFont = unread ? g_hMenuFontBold : g_hMenuFont;
    emphFont = unread ? g_hMenuFontBold : g_hMenuFontSemiBold;
}

// 一覧行のサイズ計算
// ラベル幅は描画と同じ走査で求める。（別経路で測ると行幅・取消線が実描画幅と食い違う）
// 幅にはピンマーカー列（全行で同幅）と左 4px・右 16px のパディングを含める。
static SIZE measureIssueRow(HDC hdc, const IssueItem& item) {
    HFONT old = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
    HFONT baseFont = nullptr, emphFont = nullptr;
    issueLabelFonts(item.unread, baseFont, emphFont);
    SIZE sz = {};
    sz.cx = walkIssueLabel(hdc, item, nullptr, 0, baseFont, emphFont, false);
    // 行の高さはフォント由来でセグメント分割に依存しないため、metrics から直接得る
    SelectObject(hdc, baseFont);
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    sz.cy = tm.tmHeight;
    // ピンマーカー列は全行で同幅を保つため、常に通常フォントで測る
    SelectObject(hdc, g_hMenuFont);
    // ピンマーカー列の幅（ピン有無で行幅が変わらないよう全行に確保する）
    // 実描画は D2D だが幅計測は GDI に一本化する。フォールバック先の Segoe UI Emoji と em
    // サイズが GDI・DirectWrite で同一のため送り幅は一致し、末尾スペース分が丸め差を吸収する。
    // ここを DWrite 計測に替えると、GDI フォールバック時に行幅と描画幅が食い違う。
    SIZE markSz = {};
    GetTextExtentPoint32W(hdc, PIN_MARK, static_cast<int>(wcslen(PIN_MARK)), &markSz);
    SelectObject(hdc, old);
    // パディングは左 4 px + 右 16 px。（左はピンマーカー列が続くため控えめにする）
    sz.cx += markSz.cx + 20;
    sz.cy += 6;
    return sz;
}

// 一覧行の描画
// hot（カーソルが乗っている行）に応じた背景色・テキスト色を切り替え、closed フラグが
// 立つ項目には DrawTextW 後に 2 px の取消線を手動で重ね描画する。
// IssueLabel::ranges に色付け範囲がある行は、範囲ごとに文字色を変えて分割描画する。
// hidden フラグが立つ項目は非活性の慣例に合わせて全体を COLOR_GRAYTEXT 一色で描く。
// （期日超過などの範囲色より優先する）ホット時はハイライト背景に GRAYTEXT が沈んで
// 読めないため、GRAYTEXT と HIGHLIGHTTEXT の中間色へ明度を上げる。（非活性感は保つ）
static void drawIssueRow(HDC hdc, const RECT& rcItem, const IssueItem& item, bool hot) {
    FillRect(hdc, &rcItem,
        reinterpret_cast<HBRUSH>(
            static_cast<INT_PTR>(hot ? COLOR_HIGHLIGHT + 1 : COLOR_MENU + 1)));

    RECT textRect  = rcItem;
    textRect.left += 4;  // 左パディング（measureIssueRow の確保幅と揃える）
    SetBkMode(hdc, TRANSPARENT);
    COLORREF textColor;
    if (item.hidden) {
        textColor = GetSysColor(COLOR_GRAYTEXT);
        if (hot) {
            // ハイライト背景上でも読めるよう HIGHLIGHTTEXT と 1:1 で混色する。
            // 固定色でなくシステム色から作ることでテーマ（ダーク・ハイコントラスト）に追随する
            COLORREF hi = GetSysColor(COLOR_HIGHLIGHTTEXT);
            textColor = RGB((GetRValue(textColor) + GetRValue(hi)) / 2,
                            (GetGValue(textColor) + GetGValue(hi)) / 2,
                            (GetBValue(textColor) + GetBValue(hi)) / 2);
        }
    }
    else {
        textColor = GetSysColor(hot ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
    }
    SetTextColor(hdc, textColor);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
    // ピンマーカー列（measureIssueRow と同じ幅を全行に確保し、ピン留め行のみ 📌 を描く）
    SIZE markSz = {};
    GetTextExtentPoint32W(hdc, PIN_MARK, static_cast<int>(wcslen(PIN_MARK)), &markSz);
    if (item.pinned) {
        // マーカー列だけ Direct2D で描いてカラー絵文字にする。列幅は GDI 計測値のまま使い、
        // 行幅・インデント・取消線の座標計算（measureIssueRow と共有）を変えない。
        RECT markRect  = textRect;
        markRect.right = markRect.left + markSz.cx;
        COLORREF bgColor = GetSysColor(hot ? COLOR_HIGHLIGHT : COLOR_MENU);
        if (!drawPinMarkColor(hdc, markRect, bgColor, textColor)) {
            // D2D が使えない環境では従来どおり GDI で単色の 📌 を描く
            DrawTextW(hdc, PIN_MARK, -1, &textRect,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        }
    }
    textRect.left += markSz.cx;
    // ラベルは色範囲ごとにフォントと色を切り替えて描く。（ピンマーカー列の幅は通常フォント基準）
    // 走査は measureIssueRow と共有するため、描画幅と行幅・取消線が必ず一致する
    HFONT baseFont = nullptr, emphFont = nullptr;
    issueLabelFonts(item.unread, baseFont, emphFont);
    int labelWidth = walkIssueLabel(hdc, item, &textRect, textColor,
                                   baseFont, emphFont, hot || item.hidden);
    SetTextColor(hdc, textColor);  // 打ち消し線が textColor を使うため戻す
    if (item.closed) {
        constexpr int STRIKE_THICKNESS = 2;
        // 中央から 1 px だけ下寄せにして視認性を上げる
        constexpr int STRIKE_Y_OFFSET  = 1;
        // テキスト左端より 3 px、右端より 4 px 外側まで線を伸ばす
        constexpr int STRIKE_MARGIN_LEFT  = 3;
        constexpr int STRIKE_MARGIN_RIGHT = 4;
        int lineY = (textRect.top + textRect.bottom) / 2 + STRIKE_Y_OFFSET;
        RECT strikeRect = {
            textRect.left - STRIKE_MARGIN_LEFT,
            lineY - STRIKE_THICKNESS / 2,
            textRect.left + labelWidth + STRIKE_MARGIN_RIGHT,
            lineY - STRIKE_THICKNESS / 2 + STRIKE_THICKNESS
        };
        HBRUSH hLineBrush = CreateSolidBrush(textColor);
        FillRect(hdc, &strikeRect, hLineBrush);
        DeleteObject(hLineBrush);
    }
    SelectObject(hdc, oldFont);
}

// ==================== 一覧ポップアップウィンドウ ====================
// TrackPopupMenu のモーダルメニューをやめ、フォーカスを一切奪わない非アクティブの
// 自前ポップアップ（WS_EX_NOACTIVATE）で一覧を表示する。
// 非モーダルのため、フォーカス復元・EndMenu・クローズ直後のクリック猶予といった
// モーダルメニュー時代の補正処理は存在しない。キー入力は受けないマウス専用の UI。
// 開く：ホバー（IDT_HOVER_TRIGGER 経由）またはアイコン左クリック（即時）。
// 閉じる：アイコンとポップアップ両方からの離脱（IDT_LIST_WATCH が監視）・
// アイコン左クリックのトグル・行クリックでチケットを開いたとき。起点によらず同一ルール。
// 唯一の例外として、ホバー自動表示から hover_click_guard_ms 以内のアイコン左クリックは
// 無視する。（詳細は handleTrayLeftClick を参照）

static constexpr wchar_t LIST_WND_CLASS[] = L"redntfy_list";

// 一覧ポップアップのウィンドウスタイル（CreateWindowExW と AdjustWindowRectEx で共有する）
// 3 箇所で食い違うと枠サイズと実ウィンドウのレイアウトがずれるため 1 箇所に集約する。
// WS_POPUP | WS_BORDER：メニュー相当の枠付きポップアップ。
// WS_EX_NOACTIVATE：表示・クリックでもフォアグラウンドを奪わない。（本ウィンドウの核）
// WS_EX_TOOLWINDOW：タスクバー・Alt+Tab に出さない。
// WS_EX_TOPMOST：タスクバー近傍でも手前に出す。
static constexpr DWORD LIST_WND_STYLE   = WS_POPUP | WS_BORDER;
static constexpr DWORD LIST_WND_EXSTYLE = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;

// 一覧の行種別と配置（クライアント座標）。表示のたびに構築する
enum class ListRowKind { Issue, Separator, Footer, Empty };
struct ListRowLayout {
    ListRowKind kind;
    int         top;
    int         height;
    size_t      index;  // Issue のとき g_issueItems のインデックス（他種別は未使用の 0）
};
static HWND g_listWnd = nullptr;                 // 初回表示時に生成して以降使い回す
static std::vector<ListRowLayout> g_listLayout;  // トレイ WndProc スレッド専用
static std::wstring g_listFooterText;            // フッタ行の文言（0 件時は未使用）
static int g_listHotRow = -1;                    // ホット行（g_listLayout の添字。-1 = なし）

// 一覧ポップアップが画面に出ているか（ウィンドウ未生成は非表示扱い）
static bool isListPopupVisible() {
    return g_listWnd && IsWindowVisible(g_listWnd);
}

// 後方定義の関数を一覧ウィンドウの WndProc から呼ぶための前方宣言
static void markIssueRead(int issueId);
static void handleTrayCommand(UINT id);
static void cycleIssueState(size_t itemIndex);

// クライアント座標 y の行ヒットテスト（セパレータは対象外）。ヒットなしは -1
static int listRowHitTest(int y) {
    for (size_t i = 0; i < g_listLayout.size(); ++i) {
        const auto& row = g_listLayout[i];
        if (row.kind == ListRowKind::Separator) continue;
        if (y >= row.top && y < row.top + row.height) return static_cast<int>(i);
    }
    return -1;
}

// 行のクライアント矩形（横幅はウィンドウ全幅）
static RECT listRowRect(HWND hWnd, const ListRowLayout& row) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    rc.top    = row.top;
    rc.bottom = row.top + row.height;
    return rc;
}

// フッタ・0 件行の描画（単色テキスト行。クリック可能なためホット時はハイライトする）
static void drawTextRow(HDC hdc, const RECT& rc, const wchar_t* text, bool hot) {
    FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(
        static_cast<INT_PTR>(hot ? COLOR_HIGHLIGHT + 1 : COLOR_MENU + 1)));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(hot ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));
    RECT textRect = rc;
    textRect.left += 4;  // チケット行と同じ左パディング
    HFONT old = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
    DrawTextW(hdc, text, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    SelectObject(hdc, old);
}

// 一覧ウィンドウの全面描画
// メモリ DC にダブルバッファで全行を描いてから転送し、チラつきを防ぐ。
// （drawPinMarkColor の D2D は BindDC で任意の HDC に描けるため、メモリ DC でも成立する）
static void paintListWindow(HWND hWnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT client;
    GetClientRect(hWnd, &client);
    HDC     hMem    = CreateCompatibleDC(hdc);
    HBITMAP hBmp    = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    HBITMAP hOldBmp = static_cast<HBITMAP>(SelectObject(hMem, hBmp));
    FillRect(hMem, &client, reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_MENU + 1)));
    for (size_t i = 0; i < g_listLayout.size(); ++i) {
        const auto& row = g_listLayout[i];
        RECT rc = client;
        rc.top    = row.top;
        rc.bottom = row.top + row.height;
        bool hot = (static_cast<int>(i) == g_listHotRow);
        switch (row.kind) {
        case ListRowKind::Issue:
            if (row.index < g_issueItems.size())
                drawIssueRow(hMem, rc, g_issueItems[row.index], hot);
            break;
        case ListRowKind::Separator: {
            // メニューのセパレータ相当の 1px 水平線
            int  y    = (rc.top + rc.bottom) / 2;
            RECT line = { rc.left + 2, y, rc.right - 2, y + 1 };
            HBRUSH br = CreateSolidBrush(GetSysColor(COLOR_GRAYTEXT));
            FillRect(hMem, &line, br);
            DeleteObject(br);
            break;
        }
        case ListRowKind::Footer:
            drawTextRow(hMem, rc, g_listFooterText.c_str(), hot);
            break;
        case ListRowKind::Empty:
            drawTextRow(hMem, rc, NO_ISSUES, hot);
            break;
        }
    }
    BitBlt(hdc, 0, 0, client.right, client.bottom, hMem, 0, 0, SRCCOPY);
    SelectObject(hMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hMem);
    EndPaint(hWnd, &ps);
}

// 一覧ポップアップのウィンドウプロシージャ
// 非アクティブ（WS_EX_NOACTIVATE + MA_NOACTIVATE）のためキー入力は届かない。マウス専用。
// 行の左クリック＝チケットを開いて既読化して閉じる。フッタ・0 件行＝クエリ画面を開いて閉じる。
// 行の右クリック＝状態遷移で、一覧は開いたまま。（通常 → ピン留め → 非表示 → 通常）
// 離脱による自動クローズはトレイ側の IDT_LIST_WATCH が担い、ここでは扱わない。
static LRESULT CALLBACK listWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        paintListWindow(hWnd);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) {
        // クリックでもアクティブ化しない（WS_EX_NOACTIVATE の補強。フォーカス非奪取の要）
        return MA_NOACTIVATE;
    }
    if (msg == WM_MOUSEMOVE) {
        int hit = listRowHitTest(static_cast<short>(HIWORD(lParam)));
        if (hit != g_listHotRow) {
            // 変化した行だけ再描画してチラつきを抑える（erase FALSE：行描画が背景ごと塗る）
            int prev = g_listHotRow;
            g_listHotRow = hit;
            if (prev >= 0 && prev < static_cast<int>(g_listLayout.size())) {
                RECT rc = listRowRect(hWnd, g_listLayout[prev]);
                InvalidateRect(hWnd, &rc, FALSE);
            }
            if (hit >= 0) {
                RECT rc = listRowRect(hWnd, g_listLayout[hit]);
                InvalidateRect(hWnd, &rc, FALSE);
            }
        }
        // ウィンドウ外へ出たときのホット解除用（毎回の再登録は無害）
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        if (g_listHotRow >= 0 && g_listHotRow < static_cast<int>(g_listLayout.size())) {
            RECT rc = listRowRect(hWnd, g_listLayout[g_listHotRow]);
            g_listHotRow = -1;
            InvalidateRect(hWnd, &rc, FALSE);
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        int hit = listRowHitTest(static_cast<short>(HIWORD(lParam)));
        if (hit < 0) return 0;
        const auto& row = g_listLayout[hit];
        if (row.kind == ListRowKind::Issue && row.index < g_issueItems.size()) {
            // 参照はメッセージポンプ越しに持ち越さない。ShellExecuteW と markIssueRead は
            // 内部でポンプを回し得るため、実行中に g_issueItems が差し替わると
            // 参照が dangling になる。id と URL を値でコピーしてから呼ぶ。
            const int          id  = g_issueItems[row.index].id;
            const std::wstring url = g_issueItems[row.index].url;
            // URL 検証を通った行だけ開いて既読化する（誤既読の防止。旧メニュー実装と同じ契約）
            if (isHttpUrl(url)) {
                ShellExecuteW(nullptr, L"open", url.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
                markIssueRead(id);
            }
        }
        else {
            // フッタ・0 件行はクエリ画面を開く（既存の IDM_OPEN_QUERY 処理を共用）
            handleTrayCommand(IDM_OPEN_QUERY);
        }
        // チケットを開いたら一覧は役目を終える
        if (g_hWnd) hideListPopup(g_hWnd);
        return 0;
    }
    if (msg == WM_RBUTTONUP) {
        int hit = listRowHitTest(static_cast<short>(HIWORD(lParam)));
        if (hit >= 0 && g_listLayout[hit].kind == ListRowKind::Issue)
            cycleIssueState(g_listLayout[hit].index);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// 一覧ポップアップウィンドウの生成（初回のみ。以降は表示/非表示で使い回す）
// WS_EX_NOACTIVATE：表示・クリックでもフォアグラウンドを奪わない（本ウィンドウの核）
// WS_EX_TOOLWINDOW：タスクバー・Alt+Tab に出さない
// WS_EX_TOPMOST：タスクバー近傍でも手前に出す
static HWND ensureListWindow() {
    if (g_listWnd) return g_listWnd;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DROPSHADOW;
    wc.lpfnWndProc   = listWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    // UNICODE 未定義ビルドのため IDC_ARROW（MAKEINTRESOURCE）は LPSTR に展開される。W 版へ読み替える
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = LIST_WND_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        writeLog("list: RegisterClassExW failed: " + std::to_string(GetLastError()));
        return nullptr;
    }
    g_listWnd = CreateWindowExW(LIST_WND_EXSTYLE,
        LIST_WND_CLASS, nullptr, LIST_WND_STYLE,
        0, 0, 0, 0, g_hWnd, nullptr, wc.hInstance, nullptr);
    if (!g_listWnd)
        writeLog("list: CreateWindowExW failed: " + std::to_string(GetLastError()));
    return g_listWnd;
}

// 一覧ポップアップの表示（ホバー・左クリック共通）
// 表示行の選定・並べ替え・絞り込みは buildListRows に委ねる。（バッジの未読判定と同じ根拠）
// 行の左クリックでチケットを開いてその 1 件だけ既読にする。（非表示のグレー行も同様に開ける）
// フッタの「未処理 N 件」はフィルタを通った件数で、フィルタで外れたピンと非表示チケットは数えない。
// 表示中は IDT_LIST_WATCH（トレイ側タイマー）が離脱を監視して閉じる。
static void showListPopup(HWND trayWnd) {
    HWND hWnd = ensureListWindow();
    if (!hWnd) return;

    const Config& cfg = g_currentConfig;
    int visible = 0;
    auto rows = buildListRows(visible);

    // 「今日」（期日の赤判定用）と経過日数の基準日は 1 回だけ求めて全行に使う。
    // 行ごとに求めると日付境界をまたいだ瞬間に同じ一覧内で判定が揺れる
    const int todayYmd = todayJstYmd();
    const long long todayDays = todayJstDaySerial();
    auto makeItem = [&](const ListRow& row) {
        IssueItem it;
        it.id  = row.id;
        it.url = issueUrl(cfg, row.id);
        auto lbl = buildIssueLabel(row, makeDueDateView(row.dueDate, todayYmd), todayDays);
        it.label  = std::move(lbl.text);
        it.ranges = std::move(lbl.ranges);
        it.unread = row.unread;
        it.pinned = row.pinned;
        it.hidden = row.hidden;
        it.closed = row.closed;
        return it;
    };

    g_issueItems.clear();
    g_listLayout.clear();
    g_listHotRow = -1;

    // カーソル位置のモニタ作業領域を先に取得する。（行の打ち切り判定と位置クランプの両方に使う）
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi = { sizeof(mi) };
    const bool haveMi =
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi) != FALSE;
    // WS_BORDER の枠を差し引いたクライアント高の上限（取得失敗時は打ち切りなし）
    RECT frame = { 0, 0, 0, 0 };
    AdjustWindowRectEx(&frame, LIST_WND_STYLE, FALSE, LIST_WND_EXSTYLE);
    const int maxClientH = haveMi
        ? static_cast<int>(mi.rcWork.bottom - mi.rcWork.top) - (frame.bottom - frame.top)
        : INT_MAX;

    // 行レイアウトの構築と全体サイズの計測
    // GetDC 失敗（GDI ハンドル枯渇）時は一覧を出さずに諦める。以降の計測が全滅し、
    // パディングの下駄だけが残った幅 20px・行高 6px の判読不能な極小ウィンドウが出るためだ。
    // 次のホバー・左クリックで再試行するので操作は失われない。
    HDC hdc = GetDC(hWnd);
    if (!hdc) {
        writeLog("list: GetDC failed; popup skipped");
        return;
    }
    int width = 0;
    int y     = 0;
    auto pushRow = [&](ListRowKind kind, int h, size_t index) {
        g_listLayout.push_back({ kind, y, h, index });
        y += h;
    };
    // テキスト行（フッタ・0 件行）の高さと幅（チケット行と同じ左 4・右 16 のパディング込み）
    // 計測は描画（drawTextRow）と同じ g_hMenuFont で行う。（DC の選択状態に依存させない）
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    const int textRowHeight = tm.tmHeight + 6;
    SelectObject(hdc, oldFont);
    auto textRowWidth = [&](const wchar_t* text) {
        HFONT prev = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
        SIZE sz = {};
        GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
        SelectObject(hdc, prev);
        return static_cast<int>(sz.cx) + 20;
    };

    if (rows.empty()) {
        g_listFooterText.clear();
        width = textRowWidth(NO_ISSUES);
        pushRow(ListRowKind::Empty, textRowHeight, 0);
    }
    else {
        // セパレータとフッタは必ず出すため、その高さを先に予約して行を詰める
        const int tailHeight = 9 + textRowHeight;
        size_t idx = 0;
        for (const auto& row : rows) {
            if (idx >= LIST_ROW_MAX) break;
            g_issueItems.push_back(makeItem(row));
            SIZE sz = measureIssueRow(hdc, g_issueItems.back());
            // 作業領域に収まらない行は打ち切る。（スクロールを持たないため、
            // 画面外へはみ出して操作不能な行を作らない）
            if (y + static_cast<int>(sz.cy) + tailHeight > maxClientH) {
                g_issueItems.pop_back();
                break;
            }
            width = (std::max)(width, static_cast<int>(sz.cx));
            pushRow(ListRowKind::Issue, static_cast<int>(sz.cy), idx);
            ++idx;
        }
        g_listFooterText = L"未処理 " + std::to_wstring(visible)
            + L" 件（クリックでブラウザ表示 ／ 右クリックでピン留め・非表示）";
        width = (std::max)(width, textRowWidth(g_listFooterText.c_str()));
        pushRow(ListRowKind::Separator, 9, 0);  // メニューのセパレータ相当（余白込み）
        pushRow(ListRowKind::Footer, textRowHeight, 0);
    }
    ReleaseDC(hWnd, hdc);

    // クライアントサイズ → ウィンドウサイズ（WS_BORDER の枠分を上乗せ）
    RECT wr = { 0, 0, width, y };
    AdjustWindowRectEx(&wr, LIST_WND_STYLE, FALSE, LIST_WND_EXSTYLE);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;

    // 位置はタスクバーの辺に密着させる。computeTrayPopupPos のアライメント指示
    // （BOTTOMALIGN＝底辺を y に、RIGHTALIGN＝右辺を x に合わせる）を座標へ翻訳し、
    // メニューの自動反転の代わりにモニタ作業領域内へクランプして画面外を防ぐ
    auto pos = computeTrayPopupPos(cursor);
    int wx = pos.x;
    int wy = pos.y;
    if (pos.alignFlags & TPM_RIGHTALIGN)  wx -= w;
    if (pos.alignFlags & TPM_BOTTOMALIGN) wy -= h;
    if (haveMi) {
        wx = (std::max)(static_cast<int>(mi.rcWork.left),
                        (std::min)(wx, static_cast<int>(mi.rcWork.right) - w));
        wy = (std::max)(static_cast<int>(mi.rcWork.top),
                        (std::min)(wy, static_cast<int>(mi.rcWork.bottom) - h));
    }

    // SWP_NOACTIVATE でフォーカスを奪わずに表示する
    SetWindowPos(hWnd, HWND_TOPMOST, wx, wy, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hWnd, nullptr, FALSE);

    g_popupShowing.store(true);
    g_listOutsideTicks = 0;
    // 離脱監視の開始。失敗時は表示を諦めて閉じる。（閉じる手段が離脱かクリックしかなく、
    // 監視なしでは出しっぱなしになるため）
    if (!SetTimer(trayWnd, IDT_LIST_WATCH, LIST_WATCH_POLL_MS, nullptr)) {
        writeLog("list: SetTimer(IDT_LIST_WATCH) failed");
        ShowWindow(hWnd, SW_HIDE);
        g_popupShowing.store(false);
    }
}

// 一覧ポップアップを閉じる（離脱・トグル・行クリックの共通経路）
// カーソルがまだアイコン上にあるときはホバー再アームを保留し、IDT_LIST_WATCH を
// 保留解除の監視に転用する。（閉じた直後の微動で即座に開き直るのを防ぐ。
// アイコンから離れたら監視側が保留を解除してタイマーを止める）
// SetTimer は既存タイマーの張り直しとして常に呼ぶ。（失敗すると保留が解除不能になるため、
// 失敗時は保留しない）
static void hideListPopup(HWND trayWnd) {
    if (g_listWnd) ShowWindow(g_listWnd, SW_HIDE);
    g_popupShowing.store(false);
    g_listHotRow = -1;
    // ホバー起点の記録を破棄する。（クリック猶予は表示中の一覧にだけ効かせる。
    // 残すと右クリックメニュー表示中など後続の g_popupShowing = true で猶予が誤発動する）
    g_hoverShownAt = 0;

    POINT pt;
    GetCursorPos(&pt);
    RECT icon;
    if (getTrayIconRect(trayWnd, icon) && PtInRect(&icon, pt)) {
        g_hoverRearmPending =
            SetTimer(trayWnd, IDT_LIST_WATCH, LIST_WATCH_POLL_MS, nullptr) != 0;
    }
    else {
        g_hoverRearmPending = false;
        KillTimer(trayWnd, IDT_LIST_WATCH);
    }
    // バッジ（未読の有無）を最新化する
    updateTrayTooltip(trayWnd);
}

// チケット項目の状態を通常 → ピン留め → 非表示 → 通常の順に遷移させる
// （一覧ポップアップ上の行右クリック）
// g_pins・g_hiddenIds と item.pinned/hidden を更新し、当該行を再描画する。
// （マーカー・グレーの描画自体は drawIssueRow が pinned/hidden を参照して行う）
// 状態は排他で、ピン留め → 非表示の遷移でピンは解除される。ピンの件数に上限はない。
// 非表示は通知対象外のため、非表示への遷移時は未読からも取り除く。（太字と件数の残留を防ぐ）
static void cycleIssueState(size_t itemIndex) {
    if (itemIndex >= g_issueItems.size()) return;

    auto& item = g_issueItems[itemIndex];
    bool nowPinned, nowHidden;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = std::find_if(g_pins.begin(), g_pins.end(),
            [&](const PinEntry& p) { return p.id == item.id; });
        if (it != g_pins.end()) {
            // ピン留め → 非表示
            g_pins.erase(it);
            g_hiddenIds.insert(item.id);
            g_unreadIds.erase(item.id);
            nowPinned = false;
            nowHidden = true;
        }
        else if (g_hiddenIds.erase(item.id) != 0) {
            // 非表示 → 通常
            nowPinned = false;
            nowHidden = false;
        }
        else {
            // 通常 → ピン留め
            // 件名・プロジェクト名・更新日時・期限日は g_issues から引く。
            // （一覧行のラベルは省略済みで復元できないため）
            PinEntry p;
            p.id     = item.id;
            p.closed = item.closed;
            for (const auto& is : g_issues) {
                if (is.id == item.id) {
                    p.subject         = is.subject;
                    p.projectName     = is.projectName;
                    p.updatedOn       = is.updatedOn;
                    p.closed          = is.closed;
                    p.dueDate         = is.dueDate;
                    p.assignedToGroup = is.assignedToGroup;
                    p.isBugTracker    = is.isBugTracker;
                    p.updaterDisplay  = is.updaterDisplay;
                    p.updaterFirst    = is.updaterFirstName;
                    break;
                }
            }
            g_pins.push_back(std::move(p));
            nowPinned = true;
            nowHidden = false;
        }
    }
    // 描画は drawIssueRow が pinned/hidden を参照するため、フラグ更新と再描画だけで見た目が切り替わる
    item.pinned = nowPinned;
    item.hidden = nowHidden;
    if (nowHidden) item.unread = false;  // 非表示行は太字にしない（buildListRows と同じ扱い）

    // 当該行だけを再描画する。（erase は FALSE：行描画が背景ごと塗るため消去は不要で、
    // TRUE だと全面消去→再描画の白フラッシュ（チラつき）が見える）
    if (g_listWnd) {
        for (const auto& row : g_listLayout) {
            if (row.kind == ListRowKind::Issue && row.index == itemIndex) {
                RECT rc = listRowRect(g_listWnd, row);
                InvalidateRect(g_listWnd, &rc, FALSE);
                UpdateWindow(g_listWnd);
                break;
            }
        }
    }
    // 遷移で必ずどちらかの集合が変わるが、片側だけの変化を追う分岐より両方の保存の方が単純で、
    // 書き出しはどちらも小さい。（tmp 経由の atomicWriteJson で破損もしない）
    savePins(g_exeDir);
    saveHidden(g_exeDir);
    // 非表示への遷移で未読が消えることがあるため、tooltip とバッジを追随させる
    if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
    writeLog("issue state: #" + std::to_string(item.id)
        + (nowPinned ? " pinned" : nowHidden ? " hidden" : " normal"));
}

// redntfy.local.toml のテンプレートを生成する（既存ファイルは絶対に上書きしない）
//
// 無効モード遷移時に「設定ファイルを開く」先を保証するためのブートストラップ。
// BOM なし UTF-8・LF。内容は redntfy.toml の [redmine] セクションのコメントに準拠する。
// CREATE_NEW により既存ファイル・同時生成との競合を排他する。
// 生成失敗は警告ログのみで続行する。（redntfy.toml を直接編集する道が残るため）
static void ensureLocalTomlTemplate(const std::wstring& exeDir) {
    std::wstring path = exeDir + L"\\" + CONFIG_LOCAL_FILENAME;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    static const char kTemplate[] =
        "# vim: set ft=toml fenc=utf-8 ff=unix sw=4 ts=4 et :\n"
        "##################################################\n"
        "# redntfy ローカル設定ファイル\n"
        "##################################################\n"
        "# redntfy.toml と同名のキーをキー単位で上書きする。接続情報はこちらに書く。\n"
        "\n"
        "# Redmine 接続設定（url と api_key は必須）\n"
        "# url       ：Redmine の URL（http:// または https:// で始まる）\n"
        "# api_key   ：Redmine の「個人設定」→「API アクセスキー」で取得した値\n"
        "# query_ids ：プロジェクトを指定せずに保存したグローバル保存クエリの id を配列で指定する。\n"
        "#             省略時は自分（と所属グループ）が担当のオープンチケットを対象にする。\n"
        "[redmine]\n"
        "# url       = \"https://redmine.example.com\"\n"
        "# api_key   = \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"\n"
        "# query_ids = [12, 34]\n";

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        writeLog("ensureLocalTomlTemplate: CreateFileW failed, err=" + std::to_string(GetLastError()));
        return;
    }
    DWORD written = 0;
    WriteFile(h, kTemplate, static_cast<DWORD>(sizeof(kTemplate) - 1), &written, nullptr);
    CloseHandle(h);
    writeLog("ensureLocalTomlTemplate: created template");
}

// 無効モードへ移行する（案内 Toast・設定ファイル/ブラウザ起動・トレイ表示の一式）
//
// メインスレッドから呼ぶ。（Shell_NotifyIconW をメインスレッドに限定する不変条件のため。
// ポーリングスレッドからは WM_ENTER_DISABLED の投函で本関数に委譲する）
// 呼び出し前に g_disabledReason が設定済みであること。
// 全分岐共通でセットアップガイド（定数 URL）を開き、原因別の Redmine ページ誘導と併用する。
// InvalidApiKey / InvalidQueryIds は url の有効性が検証済みの場合のみ到達するため、
// ここでの ShellExecuteW は http(s) URL しか受け取らない。
static void enterDisabledMode(HWND hWnd, DisabledReason reason,
                              const Config& cfg, const std::wstring& exeDir) {
    // 設定ファイルを開く（不在ならテンプレートを生成してから）
    ensureLocalTomlTemplate(exeDir);
    std::wstring configPath = exeDir + L"\\" + CONFIG_LOCAL_FILENAME;
    ShellExecuteW(nullptr, L"open", configPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    // 設定不備の解消手順を示すセットアップガイドを開く（原因別の Redmine ページ誘導と併用）
    ShellExecuteW(nullptr, L"open", GUIDE_URL, nullptr, nullptr, SW_SHOWNORMAL);

    // 原因別のブラウザ誘導と Toast（permalink 空 = ボタンなし。silent=false で OS 通知音）
    try {
        if (reason == DisabledReason::InvalidUrl) {
            showToast3(L"無効な Redmine URL",
                       L"設定ファイルに url を設定してください",
                       L"設定後は redntfy を再起動してください", L"");
        }
        else if (reason == DisabledReason::InvalidApiKey) {
            std::wstring accountUrl = cfg.redmineUrl + L"/my/account";
            ShellExecuteW(nullptr, L"open", accountUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            showToast3(L"認証エラー",
                       L"Redmine の API アクセスキーを api_key に設定してください",
                       L"設定後は redntfy を再起動してください", L"");
        }
        else {
            std::wstring issuesUrl = cfg.redmineUrl + L"/issues";
            ShellExecuteW(nullptr, L"open", issuesUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            showToast3(L"設定エラー",
                       L"Redmine のマイカスタムクエリを query_ids に設定してください",
                       L"設定後は redntfy を再起動してください", L"");
        }
    }
    catch (...) {
        writeLog("enterDisabledMode: toast failed");
    }

    // トレイ表示を無効モードへ（アイコンは updateTrayTooltip 経由で切り替わる）
    updateTrayTooltip(hWnd);
}

// トレイ用ウィンドウプロシージャ
// トレイアイコン操作（左クリックトグル・右クリックメニュー・ホバー一覧）、一覧関連タイマー、
// tooltip・バッジ更新、無効モード遷移、右クリックメニューのオーナードロー（新版通知）、
// スリープ復帰・ロック解除の即時ポーリング、エクスプローラ再起動時のアイコン再登録を振り分ける。
static LRESULT CALLBACK trayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            KillTimer(hWnd, IDT_HOVER_TRIGGER);  // 保留中のホバートリガーを取消（クリック優先）
            showTrayContextMenu(hWnd);
        }
        else if (lParam == WM_LBUTTONUP) {
            KillTimer(hWnd, IDT_HOVER_TRIGGER);  // 同上
            handleTrayLeftClick(hWnd);
        }
        else if (lParam == WM_MOUSEMOVE) {
            // ホバー検出のデバウンス：静止中は WM_MOUSEMOVE が来ないため、動くたびに
            // ワンショットタイマーを張り直せば「delay 時間静止したら表示」を判定できる。
            // hover_delay_ms = 0 は即時表示。（デバウンスなし）
            // トグル OFF・表示中・再アーム保留中（明示クローズ後、アイコン離脱まで）は
            // アームしない
            if (!isDisabled() && g_hoverPopupEnabled.load()
                && !g_popupShowing.load() && !g_hoverRearmPending) {
                DWORD delay = g_hoverDelayMs.load();
                if (delay == 0) {
                    KillTimer(hWnd, IDT_HOVER_TRIGGER);
                    handleTrayHover(hWnd);
                }
                else {
                    SetTimer(hWnd, IDT_HOVER_TRIGGER, delay, nullptr);
                }
            }
        }
        return 0;
    }
    if (msg == WM_TIMER && wParam == IDT_HOVER_TRIGGER) {
        // ワンショット化：発火したら即座に殺してから表示に進む
        KillTimer(hWnd, IDT_HOVER_TRIGGER);
        handleTrayHover(hWnd);
        return 0;
    }
    if (msg == WM_TIMER && wParam == IDT_LIST_WATCH) {
        POINT pt;
        GetCursorPos(&pt);
        if (g_listWnd && IsWindowVisible(g_listWnd)) {
            // 表示中：アイコンとポップアップの両方から離れた状態が連続したら閉じる。
            // アイコン矩形の取得失敗（過渡状態）は場外と数えず今回の判定を見送る。
            // getTrayIconRect の契約どおり保守的＝閉じない側に倒し、次の tick で再判定する
            RECT icon = {};
            if (!getTrayIconRect(hWnd, icon)) return 0;
            RECT wnd = {};
            GetWindowRect(g_listWnd, &wnd);
            if (PtInRect(&icon, pt) || PtInRect(&wnd, pt)) {
                g_listOutsideTicks = 0;
            }
            else if (++g_listOutsideTicks >= LIST_LEAVE_TICKS) {
                hideListPopup(hWnd);
            }
        }
        else if (g_hoverRearmPending) {
            // 明示クローズ後の再アーム保留：カーソルがアイコンから離れたら解除して止める。
            // 矩形の取得失敗は離脱扱いにする。（保留に倒し続けるとホバーが復帰不能になるため）
            RECT icon = {};
            if (!getTrayIconRect(hWnd, icon) || !PtInRect(&icon, pt)) {
                g_hoverRearmPending = false;
                KillTimer(hWnd, IDT_LIST_WATCH);
            }
        }
        else {
            KillTimer(hWnd, IDT_LIST_WATCH);  // 非表示かつ保留なしの迷子タイマーは止める
        }
        return 0;
    }
    if (msg == WM_UPDATE_TOOLTIP) {
        updateTrayTooltip(hWnd);
        return 0;
    }
    // ポーリングスレッドからの無効モード遷移依頼（Toast・ブラウザ誘導・トレイ更新を実施）
    if (msg == WM_ENTER_DISABLED) {
        enterDisabledMode(hWnd, static_cast<DisabledReason>(wParam), g_currentConfig, g_exeDir);
        return 0;
    }
    if (msg == WM_COMMAND) {
        handleTrayCommand(LOWORD(wParam));
        return 0;
    }
    // オーナードローは右クリックメニューのバージョン更新通知行のみ（一覧は自前ウィンドウ描画）
    if (msg == WM_MEASUREITEM) {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis->CtlType == ODT_MENU && mis->itemID == IDM_OPEN_GITHUB)
            return measureVersionMenuItem(hWnd, mis) ? TRUE : DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    if (msg == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlType == ODT_MENU && dis->itemID == IDM_OPEN_GITHUB)
            return drawVersionMenuItem(dis) ? TRUE : DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    // スリープ復帰・ロック解除：即時ポーリングをトリガー
    if ((msg == WM_POWERBROADCAST && wParam == PBT_APMRESUMEAUTOMATIC) ||
        (msg == WM_WTSSESSION_CHANGE && wParam == WTS_SESSION_UNLOCK)) {
        g_forcePoll.store(true);
        writeLog(msg == WM_POWERBROADCAST ? "resume from sleep" : "session unlock");
        return msg == WM_POWERBROADCAST ? TRUE : 0;
    }
    if (WM_TASKBAR_CREATED != 0 && msg == WM_TASKBAR_CREATED) {
        addTrayIcon(hWnd);
        updateTrayTooltip(hWnd);  // バッジ状態とツールチップをエクスプローラ再起動後も復元
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// 非表示トップレベルウィンドウを作成してトレイメッセージ受信に使用する
// HWND_MESSAGE ではなく nullptr 親（トップレベル）にすることで WM_POWERBROADCAST を受信できる
// 失敗時は nullptr を返す。（GetLastError 込みでログを残す）トレイ UI はこのアプリの
// 唯一の操作面かつ唯一の終了経路のため、呼び出し側は失敗時に常駐へ進まず終了すること。
// （ウィンドウ無しでメッセージループに入ると、終了手段のない不可視常駐になる）
static HWND createTrayWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = trayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"redntfy_tray";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        writeLog("RegisterClassExW failed: " + std::to_string(GetLastError()));
        return nullptr;
    }
    HWND hWnd = CreateWindowExW(0, L"redntfy_tray", nullptr, 0,
        0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hWnd)
        writeLog("CreateWindowExW failed: " + std::to_string(GetLastError()));
    return hWnd;
}

// ==================== エントリポイント ====================

// ネットワークインターフェース変化コールバック
//
// MibAddInstance（新規追加）と MibParameterNotification（パラメータ変更）で発火する。
// MibParameterNotification はルーティング変更等でも頻発するが、60 秒クールダウン期間中の
// トリガーはポーリングループでスキップされる。（クールダウン後に 1 回ポーリングが走る）
// MibDeleteInstance（切断）は無視する。後続の MibAddInstance で対応されるため不要。
// ※ システムスレッドプールから呼ばれるため、ここでは atomic 操作のみ行う。
static VOID WINAPI onNetworkChange(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE type) {
    if (type != MibAddInstance && type != MibParameterNotification) return;
    g_forcePoll.store(true);
}

// ピン留めの鮮度維持
//
// 取得集合に居るピンは集合側の内容で更新し、集合から外れたピン（クローズ・担当変更等）は
// 個別取得で最新化する。（集合外ピンの件数分の HTTP/ポーリング）個別取得の失敗時（削除済み・
// 接続エラー）は前回キャッシュの内容のまま表示を継続する。
static void refreshPins(const std::wstring& exeDir, const Config& cfg,
                        const std::vector<Issue>& issues, const std::vector<int>& groupIds,
                        bool groupIdsResolved)
{
    std::vector<PinEntry> pinsSnapshot;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        pinsSnapshot = g_pins;
    }
    bool changed = false;
    std::unordered_map<int, const Issue*> byId;
    for (const auto& is : issues) byId[is.id] = &is;
    for (auto& p : pinsSnapshot) {
        if (g_shutdownRequested) return;  // 個別取得は HTTP を伴うため中断可能にする
        Issue fetched;
        const Issue* src = nullptr;
        auto it = byId.find(p.id);
        if (it != byId.end()) {
            src = it->second;
        }
        else if (fetchIssue(cfg, p.id, fetched)) {
            // 個別取得分にもグループ担当を付与する（取得集合と同じ判定を通す）
            fetched.assignedToGroup = isGroupAssignee(groupIds, fetched.assignedToId);
            src = &fetched;
        }
        // 判定集合が未解決の間はグループフラグを比較・更新しない。空集合との突合結果（全 false）で
        // 保存済みの true を書き戻してしまうため。（解決後のポーリングで追いつく）
        bool groupChanged = src && groupIdsResolved
            && p.assignedToGroup != src->assignedToGroup;
        // 最終更新者は src 側で確定している場合だけ比較・更新する。集合外ピンの個別取得
        // （fetchIssue）は journals を含まず updaterDisplay が空のため、保存済みの値を保つ
        bool updaterChanged = src && !src->updaterDisplay.empty()
            && (p.updaterDisplay != src->updaterDisplay
                || p.updaterFirst != src->updaterFirstName);
        if (src && (p.subject != src->subject || p.updatedOn != src->updatedOn
                    || p.closed != src->closed || p.dueDate != src->dueDate
                    || p.projectName != src->projectName
                    || p.isBugTracker != src->isBugTracker
                    || groupChanged || updaterChanged)) {
            p.subject     = src->subject;
            // project は journals と違って include 不要で常に含まれるため、updaterDisplay の
            // ような「非空のときだけ更新」ガードは要らない。（別プロジェクトへの移動を追随する）
            p.projectName = src->projectName;
            p.updatedOn   = src->updatedOn;
            p.closed      = src->closed;
            p.dueDate     = src->dueDate;
            // トラッカー判定は設定との照合だけで決まり、グループ判定のような未解決状態がない
            p.isBugTracker  = src->isBugTracker;
            if (groupIdsResolved) p.assignedToGroup = src->assignedToGroup;
            if (!src->updaterDisplay.empty()) {
                p.updaterDisplay = src->updaterDisplay;
                p.updaterFirst   = src->updaterFirstName;
            }
            changed = true;
        }
    }
    if (!changed) return;

    // HTTP 中に WndProc スレッドがトグルした可能性があるため、丸ごと置換せず
    // id 一致分のフィールドだけを書き戻す（追加・削除を失わない）
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& gp : g_pins) {
            for (const auto& sp : pinsSnapshot) {
                // id 一致分は丸ごと代入で足りる。（id は等しく、残りは全て更新対象）
                // フィールド単位に複写すると、PinEntry へ項目を足したときに追記を忘れて
                // ポーリングの更新結果が黙って捨てられる
                if (gp.id == sp.id) {
                    gp = sp;
                    break;
                }
            }
        }
    }
    savePins(exeDir);
}

// 非表示チケットの自動削除
// ポーリング成功時に呼び、取得集合に無い id を g_hiddenIds から取り除いて保存する。
// クローズ・担当変更等でクエリから外れたチケットの非表示設定を残さないための掃除。
// 同じチケットが後日クエリへ戻ったときは通常状態で再出現する。（改めて非表示にすれば良い）
// fetchIssues は 1 クエリでも失敗すると全体を失敗にするため、成功時の issues は完全な集合で
// 誤削除は起きない。（本関数は成功パスからのみ呼ぶこと）
static void pruneHidden(const std::wstring& exeDir, const std::vector<Issue>& issues) {
    bool changed = false;
    {
        std::unordered_set<int> current;
        for (const auto& is : issues) current.insert(is.id);
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto it = g_hiddenIds.begin(); it != g_hiddenIds.end();) {
            if (current.count(*it) == 0) {
                writeLog("hidden: pruned #" + std::to_string(*it) + " (left query set)");
                it = g_hiddenIds.erase(it);
                changed = true;
            }
            else {
                ++it;
            }
        }
    }
    if (changed) saveHidden(exeDir);
}

// 通知理由（1 件時の Toast 文言切替とログ内訳に使う）
enum class NotifyKind { New, Updated, QueryEntered };

// 通知対象 1 件（prev の再検索を省くため理由を持たせる）
struct NotifyTarget {
    const Issue* issue = nullptr;
    NotifyKind   kind  = NotifyKind::Updated;
    std::string  updaterName;  // Toast に添える表示名。新規は前回ポーリング以降の更新者
                               //（居なければ起票者）、更新は最終更新者。クエリ流入は空。
};

// now に有り prev に無いクエリ id があるか（＝新たなクエリへの流入があるか）
// known（前回追跡していたクエリ id）に無いクエリは流入と見なさない。query_ids に追加した
// 直後、そのクエリの全件が「更新」通知になるのを防ぐため。（v1 移行と同じ「黙って採用」方針）
static bool hasNewQueryEntry(const std::vector<int>& now, const std::vector<int>& prev,
                             const std::vector<int>& known)
{
    for (int q : now) {
        if (std::find(prev.begin(), prev.end(), q) != prev.end()) continue;
        if (std::find(known.begin(), known.end(), q) == known.end()) continue;
        return true;
    }
    return false;
}

// 通知対象を選定する（I/O・共有状態の変更を持たない純粋ロジック。単体テスト対象）
//
// 前回状態（state.json）と突合して「新規流入」「updated_on の進行」「既知チケットの
// 新クエリ流入」を検知する。クエリ流入は「更新」として通知する。ただし前回追跡して
// いなかったクエリについては、そのクエリへの流入も、そのクエリだけに属するチケットの
// 新規流入も検知しない。（設定追加直後の通知の嵐を防ぐ）
// 自分が起票したチケットの流入は author.id で、自分の操作による更新と自分の更新が原因の
// 流入は最終 journal の更新者 id で除外する。（myUserId == 0 のときは除外せず通知側に倒す）
// この抑止は muteOwnChanges で一括 ON/OFF できる。OFF なら自分の起票・更新も通知する。
// 担当者フィルタ ON なら、自分が担当でないチケットもあわせて除外する。
// バージョンフィルタ ON なら、fixed_version 未指定かつ期日なしのチケットもあわせて除外する。
// hiddenIds（非表示チケット）は「見なくて良い」の意思表示のため無条件に除外する。
// （state.json への記録は呼び出し側が全件で行うので、非表示解除時に溜まった更新が
// 一斉通知される「通知の嵐」は起きない）
// issues の最終更新者は resolveUpdaters が確定済みであることを前提とする。
// 戻り値の NotifyTarget::issue は issues の要素を指す。（issues より長く保持しない）
// 終了要求による中断は nullopt を返す。（呼び出し側は state.json を書かずに抜ける）
static std::optional<std::vector<NotifyTarget>> selectNotifyTargets(
    const std::vector<Issue>& issues, const PollState& prev,
    bool muteOwnChanges, int myUserId,
    const std::unordered_set<int>& hiddenIds = {})
{
    std::vector<NotifyTarget> targets;
    for (const auto& is : issues) {
        if (g_shutdownRequested) return std::nullopt;
        // 非表示チケットは通知しない
        if (hiddenIds.count(is.id) != 0) continue;
        // 担当者フィルタで外れたチケットは通知しない
        if (!passesAssigneeFilter(is)) continue;
        // バージョン未指定は「将来の課題」なので通知しない（期日ありは通す）
        if (!passesVersionFilter(is)) continue;
        auto it = prev.issues.find(is.id);
        if (it == prev.issues.end()) {
            // 新規流入（自分の起票は通知しない。muteOwnChanges OFF なら通知に倒す）
            if (muteOwnChanges && myUserId != 0 && is.authorId == myUserId) continue;
            // 前回追跡していたクエリのいずれにも属さないチケットは、query_ids へ追加した
            // 直後のクエリ固有の既存チケットなので黙って採用する。（既知チケットの流入抑止と
            // 同じ方針。通知するとサマリ Toast・通知音・未読バッジが件数分跳ね上がる）
            // v1 移行時は knownQueries が空で判定できないため、従来どおり通知側に倒す。
            bool inKnown = prev.knownQueries.empty();
            for (int q : is.queryIds) {
                if (std::find(prev.knownQueries.begin(), prev.knownQueries.end(), q)
                        != prev.knownQueries.end()) {
                    inKnown = true;
                    break;
                }
            }
            if (!inKnown) continue;
            // 自分の更新が原因の流入は通知しない。既存チケットは自分の操作（期日削除等）で
            // 保存クエリの条件に入り直すことがある。起票者チェックだけでは弾けないため、
            // 前回ポーリング以降に更新されたチケットは最終更新者でも判定する。
            // それより古い更新の流入は時間経過（期日接近等）によるもので、最終更新者が
            // 自分でも通知する。（既知チケットのクエリ流入の扱いと揃える）
            // polled_on の無い旧形式 state.json では判定せず通知側に倒す。
            bool recentUpdate = !prev.polledOn.empty() && is.updatedOn > prev.polledOn;
            if (muteOwnChanges && recentUpdate && myUserId != 0 && is.updaterId == myUserId) continue;
            // 表示名も同じ基準で選ぶ。直近の更新が原因の流入はその更新者、時間経過の流入は
            // 起票者を出す。（古い最終更新者を「新規：○○」と出すと起票者と誤読される）
            targets.push_back({&is, NotifyKind::New,
                recentUpdate && !is.updaterName.empty() ? is.updaterName : is.authorName});
            continue;
        }
        const StateEntry& pv = it->second;
        bool updated = is.updatedOn > pv.updatedOn;
        // 既知チケットの新クエリ流入（期限が近づいて期限クエリに入った等）。「更新」扱いで通知する
        bool entered = pv.hasQueries
            && hasNewQueryEntry(is.queryIds, pv.queryIds, prev.knownQueries);
        if (!updated && !entered) continue;
        // 自分の操作による更新は通知しない。（muteOwnChanges OFF なら通知に倒す）
        // 最終更新者は resolveUpdaters が確定済み。
        // updated_on が進んでいない純粋な流入では判定しない：時間経過による流入が典型で
        // 「自分の操作」ではないため。（クエリ流入の Toast は更新者名も出さない）
        if (muteOwnChanges && updated && myUserId != 0 && is.updaterId == myUserId) continue;
        targets.push_back({&is, updated ? NotifyKind::Updated : NotifyKind::QueryEntered,
                           updated ? is.updaterName : std::string()});
    }
    return targets;
}

// 選定済みの通知対象をユーザへ届ける（Toast・通知音・未読記録・内訳ログ）
// targets は 1 件以上を前提とする。Toast の失敗はログのみ残して通知音・未読記録は続行する。
// （音と未読バッジだけでも更新の発生は伝わるため）
static void emitNotifications(const Config& cfg, const std::vector<NotifyTarget>& targets) {
    try {
        if (targets.size() == 1) {
            // 1 件：チケット番号＋件名を出し、クリックでそのチケットを開く。
            // 種別に更新者名を添える。（例「更新：山田太郎」。名前が取れない場合は種別のみ）
            const NotifyTarget& nt = targets[0];
            const Issue* t = nt.issue;
            std::wstring kind = nt.kind == NotifyKind::New ? L"新規" : L"更新";
            if (!nt.updaterName.empty()) kind += L"：" + toWide(nt.updaterName);
            showToast(L"#" + std::to_wstring(t->id) + L" " + toWide(t->subject),
                      kind, issueUrl(cfg, t->id));
        }
        else {
            // 複数件：合計件数のみのサマリ 1 通とし、クリックで代表画面（queryUrl）を開く
            showToast(L"チケットが " + std::to_wstring(targets.size()) + L" 件更新されました",
                      L"",
                      queryUrl(cfg));
        }
    }
    catch (winrt::hresult_error const& e) {
        writeLog("notify toast failed: " + winrt::to_string(e.message()));
    }
    catch (...) {
        writeLog("notify toast failed: unknown exception");
    }
    // 通知音（ミーティング中ミュートの判定は発火時点の状態で行う）
    if (g_soundEnabled.load() && !(g_muteInMeeting.load() && isMeetingActive()))
        launchSound(cfg);
    {
        // 未読 id を記録する。（一覧の太字と tooltip の未読件数。行クリックで開いた分だけ取り除く）
        std::lock_guard<std::mutex> lk(g_mtx);
        for (const auto& t : targets) g_unreadIds.insert(t.issue->id);
    }
    int nNew = 0, nUpd = 0, nEnt = 0;
    for (const auto& t : targets) {
        if (t.kind == NotifyKind::New)          ++nNew;
        else if (t.kind == NotifyKind::Updated) ++nUpd;
        else                                    ++nEnt;
    }
    // 内訳を残すのは、クエリ流入だけで通知が出た回を後から切り分けるため
    writeLog("notify: " + std::to_string(targets.size()) + " issue(s) (new="
        + std::to_string(nNew) + " updated=" + std::to_string(nUpd)
        + " entered=" + std::to_string(nEnt) + ")");
}

// ポーリング結果の処理（共有状態の公開 → 通知対象の選定 → 通知 → 状態保存）
//
// 選定基準は selectNotifyTargets、通知内容は emitNotifications のコメントを参照。
// ベースライン未確立（初回起動・state.json 破損）の場合は通知せず状態保存のみ行う。
// 終了要求で選定が中断された場合は state.json を書かずに抜ける。前回のまま残るため、
// 未通知分は次回ポーリングで再検知される。（通知は失われない）
// prev は呼び出し側が loadState で読んだ前回状態。（resolveUpdaters のキャッシュと共有するため外で読む）
// 戻り値：通知対象と判定した件数。（0 は通知なし。「今すぐ更新」の完了通知の出し分けに使う）
static int deliverPollResults(const std::wstring& exeDir, const Config& cfg,
                              const std::vector<Issue>& issues, const PollState& prev)
{
    // 一覧・tooltip 用の共有状態を更新する
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_issues = issues;
    }

    if (!prev.baseline) {
        // 保存失敗を放置すると baseline が永久に確立せず無言で通知ゼロになるため、Toast で知らせる
        // （書き込み不可のディレクトリに展開した場合など、ログ自体が残せない環境を想定）
        if (!saveState(exeDir, cfg, issues))
            showErrorToast(L"状態保存エラー", L"state.json を書き込めません。展開先の書き込み権限を確認してください");
        if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
        writeLog("baseline established (" + std::to_string(issues.size()) + " issues)");
        return 0;
    }

    // 旧形式からの移行と query_ids への追加分をログに残す。
    // どちらも「流入検知を見送った回」で、通知が出ないことの説明になる
    if (prev.knownQueries.empty()) {
        writeLog("state: v1 format (no query membership), adopting membership silently");
    }
    else {
        for (int q : trackedQueryIds(cfg)) {
            if (std::find(prev.knownQueries.begin(), prev.knownQueries.end(), q)
                    == prev.knownQueries.end())
                writeLog("state: query " + std::to_string(q)
                    + " newly tracked, adopting membership silently");
        }
    }

    // 非表示チケットは通知から除外する。（スナップショットを取り、選定中のロック競合を避ける）
    std::unordered_set<int> hiddenIds;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        hiddenIds = g_hiddenIds;
    }
    auto targets = selectNotifyTargets(issues, prev, g_muteOwnChanges.load(), g_myUserId.load(),
                                       hiddenIds);
    if (!targets) return 0;  // 終了要求による中断

    if (!targets->empty()) emitNotifications(cfg, *targets);

    // 保存に失敗すると次回も同じ更新を再検知して通知が重複するため、Toast で知らせる
    if (!saveState(exeDir, cfg, issues))
        showErrorToast(L"状態保存エラー", L"state.json を書き込めません。通知が重複する可能性があります");
    if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);

    return static_cast<int>(targets->size());
}

// 「今すぐ更新」の完了 Toast
//
// 明示のユーザ操作に対し、新しい更新がなかったことだけを 1 行で知らせる。
// 「更新が完了しました」等の正常終了文言や未処理件数は出さない。
// （件数は一覧フッタが担うため、Toast に出すと二重表示になる）
// 更新を検知した回は通知 Toast 自体が完了の合図になるため、呼び出し側で出し分ける。
// 通知音は鳴らさない。（チケットの更新通知と違い、ユーザが待っている場面での応答のため）
static void showPollDoneToast()
{
    try {
        showToast(L"新しい更新はありません", L"", L"", true);
    }
    catch (winrt::hresult_error const& e) {
        writeLog("poll done toast failed: " + winrt::to_string(e.message()));
    }
    catch (...) {
        writeLog("poll done toast failed: unknown exception");
    }
}

// shutdown と手動更新（g_manualPoll）だけを監視して待つ（100ms 刻み）
// waitInterruptible と違い forcePoll では起きない。休止時間帯とクールダウン待ちで使う。
// （forcePoll で即復帰すると NIC 変化の連発時に周回して待機の目的が破れるため）
// waitInterruptible と同じ GetTickCount64 の期限方式とする。Sleep の積算方式は
// サスペンド中に実時間が進まず、復帰後に残り全量を待ち直して休止時間帯明けの
// ポーリングが最大 1 時間遅れるため使わない。（GetTickCount64 はサスペンド時間を含む）
static void waitIgnoringForcePoll(ULONGLONG ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (!g_shutdownRequested && !g_manualPoll.load()) {
        ULONGLONG now = GetTickCount64();
        if (end <= now) break;
        ULONGLONG remain = end - now;
        DWORD chunk = static_cast<DWORD>((std::min)(remain, static_cast<ULONGLONG>(100)));
        Sleep(chunk);
    }
}

// ポーリングスレッドがセッション中に確定・キャッシュする判定材料
// 本スレッド専用でロック不要。resolvePollMetadata が休止時間帯の判定後に毎周回更新する。
struct PollSession {
    std::vector<int> ownGroups;        // 自分の所属グループ（fetchMyUserId が設定）
    std::vector<int> groupIds;         // グループ担当判定に使う集合（全グループ、権限不足時は所属グループ）
    bool groupIdsResolved = false;

    // ユーザ id → 姓・名のセッション内キャッシュ（一覧の最終更新者表示。resolveUserNames が使う）
    std::unordered_map<int, UserNames> userNames;

    // バージョン欄が有効なトラッカー id 集合
    // nullopt = 未取得または enabled_standard_fields 非対応（Redmine 5.0 未満）。
    // nullopt の間は全トラッカー有効（従来動作）として扱う
    std::optional<std::unordered_set<int>> versionedTrackers;
    bool versionedTrackersResolved = false;

    // プロジェクト id → バージョン定義有無のセッション内キャッシュ（projectHasAnyVersion が使う）
    std::unordered_map<int, bool> projectHasVersions;

    // バージョン欄判定情報を最後に取得した時刻（GetTickCount64。0 = 未取得）
    // プロセス内のみ保持し永続化しない。（起動時は必ず取得する）
    ULONGLONG versionMetaFetchTick = 0;
};

// ポーリングの判定材料（user id・グループ集合・バージョン欄情報）を確定・更新する
// 未確定分の取得を毎周回試み、失敗分は次回ポーリングで再試行する。HTTP を伴うため、
// 呼び出し側は休止時間帯の判定より後に呼ぶこと。（休止中はネットワークに触れない）
// manualTriggered はバージョン欄情報のキャッシュ破棄条件。（「今すぐ更新」で強制再取得）
static void resolvePollMetadata(const Config& cfg, PollSession& s, bool manualTriggered) {
    // 自分の user id を確定する。（失敗時 0 = 自分の操作の除外判定なし）
    // 自動起動直後などネットワーク未接続で失敗した場合に備え、取得できるまで毎回試みる
    if (g_myUserId == 0) {
        g_myUserId = fetchMyUserId(cfg, s.ownGroups);
        if (g_myUserId != 0) writeLog("my user id: " + std::to_string(g_myUserId.load()));
    }

    // グループ担当マーカーの判定集合を確定する（user id 取得後に 1 回）
    // /groups.json は admin 権限が必要なため、403 なら自分の所属グループへ縮退する。
    // （他グループ宛の判定は漏れるが誤判定はしない）接続エラーは次回ポーリングで再試行。
    if (!s.groupIdsResolved && g_myUserId != 0) {
        DWORD status = 0;
        if (auto all = fetchAllGroupIds(cfg, &status)) {
            s.groupIds = std::move(*all);
            s.groupIdsResolved = true;
            writeLog("group ids: " + std::to_string(s.groupIds.size()) + " (all groups)");
        }
        else if (status == 403) {
            s.groupIds = s.ownGroups;
            s.groupIdsResolved = true;
            writeLog("group ids: " + std::to_string(s.groupIds.size()) + " (own groups fallback)");
        }
    }

    // バージョン欄判定情報の鮮度管理
    // 「今すぐ更新」または設定間隔（version_meta_refresh_hours）の超過で破棄して
    // 再取得する。トラッカー・バージョン定義の変更を再起動なしで反映するため。
    // versionedTrackers の中身は破棄しない。直後の再取得が失敗しても前回の集合で
    // 判定を継続するためで、成功時に上書きされる
    if (s.versionedTrackersResolved
        && (manualTriggered
            || GetTickCount64() - s.versionMetaFetchTick
                >= static_cast<ULONGLONG>(cfg.versionMetaRefreshHours) * 3600000ULL)) {
        s.versionedTrackersResolved = false;
        s.projectHasVersions.clear();
        writeLog(manualTriggered ? "version metadata cache cleared (manual poll)"
                                 : "version metadata cache cleared (interval)");
    }

    // バージョン欄が有効なトラッカー集合を確定する（起動時と、上のキャッシュ破棄後）
    // バージョンフィルタが「欄の無いチケット」を誤って除外しないための判定材料。
    // 接続エラーは次回ポーリングで再試行する。（その間は前回の集合を使い続ける）
    if (!s.versionedTrackersResolved) {
        if (fetchVersionedTrackerIds(cfg, s.versionedTrackers)) {
            s.versionedTrackersResolved = true;
            s.versionMetaFetchTick = GetTickCount64();
            writeLog(s.versionedTrackers
                ? "versioned trackers: " + std::to_string(s.versionedTrackers->size())
                : "versioned trackers: not supported (treat all as versioned)");
        }
    }
}

// ポーリングスレッド本体
//
// メインスレッドからポーリング処理（HTTP I/O）を分離し、UI（右クリックメニュー等）の
// 応答性をネットワーク状態に依存させないことが目的。
// 実行内容：保存クエリの全件取得 → 通知判定・Toast・通知音 → 状態保存。
// 中断は g_shutdownRequested の atomic フラグ経由。（waitInterruptible が 100 ms 単位で監視）
static void pollThreadFunc(std::wstring exeDir, Config cfg) {
    // WinRT アパートメント初期化
    // 本スレッドは JSON パースと Toast 表示（deliverPollResults / showErrorToast）を持つため、
    // WinRT 呼び出しに先立ってアパートメントを初期化する。
    // 失敗はログのみ残して本スレッドだけ終える。（ポーリングなしで常駐は継続）
    WinRtApartment apartment;
    if (!apartment.ok) {
        writeLog("pollThreadFunc: WinRT init failed - polling unavailable");
        return;
    }

    bool startupPoll = true;  // 起動直後の 1 回だけ schedule・クールダウンに関わらずポーリングする

    // セッション中に確定・キャッシュする判定材料（内訳は PollSession のコメントを参照）
    PollSession session;

    while (!g_shutdownRequested) {
        try {
            SYSTEMTIME utcNow;
            GetSystemTime(&utcNow);
            auto jstNow = utcToJst(utcNow);
            int pollsPerHour = cfg.schedule[jstNow.wHour];

            // 休止時間帯（0）は取得せず次の正時まで待機する。強制・stale 判定より先に評価する
            // ことで、休止中はどのトリガーでもポーリングしない。（schedule の 0 を最優先とする）
            // 起動直後の 1 回だけは例外として実行し、一覧を出せる状態にする。
            // calcSleepUntilNextPoll(0) は内部ガードで 1 回/時扱いになり「次の正時まで」を返す。
            // 「今すぐ更新」は明示のユーザ操作のため、休止時間帯・クールダウンの抑止を受けない
            bool manualTriggered = g_manualPoll.exchange(false);

            if (pollsPerHour == 0 && !startupPoll && !manualTriggered) {
                g_forcePoll.store(false);  // 休止中に積まれたトリガーは破棄する（次の稼働正時に自然に取得される）
                waitIgnoringForcePoll(calcSleepUntilNextPoll(0));
                continue;
            }

            // 判定材料の確定・更新（HTTP を伴う。休止時間帯の判定より後 = 休止中は触れない）
            resolvePollMetadata(cfg, session, manualTriggered);

            // 即時ポーリング判定。（forcePoll フラグ or 1 時間以上未ポーリング）
            // 前回ポーリングからクールダウン以内の即時要求は、残り時間を待ってから取得し直す
            // （NIC 変化の連発による連続ポーリングの抑止。トリガー自体は消費してよい）
            bool forceTriggered = g_forcePoll.exchange(false);
            ULONGLONG tickNow   = GetTickCount64();
            ULONGLONG lastTick  = g_lastPollTick.load();
            bool stale = (lastTick > 0) && (tickNow - lastTick >= STALE_POLL_THRESHOLD_MS);

            if (!manualTriggered && (forceTriggered || stale) && !startupPoll && lastTick > 0
                && (tickNow - lastTick < FORCE_POLL_COOLDOWN_MS)) {
                writeLog("force poll deferred (cooldown)");
                // クールダウンの残り時間は forcePoll を無視して待つ。
                // 手動更新はクールダウンの対象外なので waitIgnoringForcePoll が監視して即座に抜ける
                waitIgnoringForcePoll(FORCE_POLL_COOLDOWN_MS - (tickNow - lastTick));
                continue;
            }
            if (manualTriggered) writeLog("manual poll triggered");
            if (forceTriggered && !startupPoll) writeLog("force poll triggered");
            if (stale && !startupPoll)
                writeLog("stale poll triggered (" + std::to_string((tickNow - lastTick) / 1000) + "s since last poll)");

            std::vector<Issue> issues;
            ULONGLONG t0    = GetTickCount64();
            bool authError  = false;
            bool queryError = false;
            bool ok = fetchIssues(cfg, issues, &authError, &queryError);
            ULONGLONG elapsed = GetTickCount64() - t0;

            // 取得試行をもって「起動直後の 1 回」は消費とする。（成功を待たない）
            // オフラインのまま休止時間帯に入った場合に、朝まで 60 秒間隔のリトライを続けないため
            startupPoll = false;

            if (!ok) {
                if (g_shutdownRequested) break;  // 終了による取得中断は接続エラーではない
                writeLog("HTTP request failed");
                // 401/404 は設定不備の確定的なシグナルのためリトライせず無効モードへ遷移する。
                // Toast・ブラウザ誘導・トレイ更新はメインスレッド（WM_ENTER_DISABLED）に委譲し、
                // 本スレッドは終了する。（復帰は再起動のみ。ホットリロードしない仕様のため）
                if (authError || queryError) {
                    DisabledReason reason = authError ? DisabledReason::InvalidApiKey
                                                      : DisabledReason::InvalidQueryIds;
                    writeLog(authError ? "HTTP 401 - entering disabled mode (api_key)"
                                       : "HTTP 404 - entering disabled mode (query_ids)");
                    g_disabledReason.store(static_cast<int>(reason));
                    // 他の投函箇所と同様に null を検査する。（PostMessage(nullptr) は
                    // 呼び出しスレッド自身のキューへ投函され、通知が黙って消えるため）
                    if (g_hWnd) PostMessage(g_hWnd, WM_ENTER_DISABLED, static_cast<WPARAM>(reason), 0);
                    break;
                }
                // 手動更新の失敗はクールダウンを無視して必ず知らせる。無音のままだと
                // 操作が届いたのか失敗したのか区別できず、完了通知の目的を果たせない
                showErrorToast(L"接続エラー", L"Redmine API に接続できません", manualTriggered);
                waitInterruptible(RETRY_WAIT_MS);
                continue;
            }

            writeLog("poll: " + std::to_string(issues.size()) + " issues ("
                + std::to_string(elapsed) + "ms), next: " + nextPollTimeStr(pollsPerHour));

            // 表示・判定用の属性を取得後に一括で付与する
            // assignedToGroup：一覧の 👥 マーカー用
            // hasVersionField：バージョン欄の有無（バージョンフィルタの誤除外防止）。
            //   バージョン設定済みなら欄は自明に在るため判定を省き、プロジェクト単位の
            //   キャッシュで 2 回目以降のポーリングは HTTP なしで済む
            for (auto& is : issues) {
                if (g_shutdownRequested) break;
                is.assignedToGroup = isGroupAssignee(session.groupIds, is.assignedToId);
                if (is.hasFixedVersion) continue;  // hasVersionField は既定 true のまま
                // trackerId 不明（0）は「欄あり」に倒す。（従来どおり除外対象。Issue のコメントと整合）
                bool trackerHas = is.trackerId <= 0 || !session.versionedTrackers
                    || session.versionedTrackers->count(is.trackerId) != 0;
                is.hasVersionField = trackerHas
                    && projectHasAnyVersion(cfg, is.projectId, session.projectHasVersions);
            }

            // 一覧・tooltip は最終更新者の解決を待たずに先行公開する。初回起動や移行直後は
            // resolveUpdaters が全件分の HTTP を打つため、完了を待つと一覧が空のまま待たされる。
            // （姓の列は解決後、deliverPollResults の再公開で入る）
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_issues = issues;
            }

            // 前回状態はここで 1 回だけ読み、最終更新者の解決と通知判定で共有する
            PollState prevState = loadState(exeDir);
            resolveUpdaters(cfg, issues, prevState, session.userNames);
            if (g_shutdownRequested) break;  // resolveUpdaters は HTTP を伴うため中断を確認する

            int notified = deliverPollResults(exeDir, cfg, issues, prevState);
            refreshPins(exeDir, cfg, issues, session.groupIds, session.groupIdsResolved);
            pruneHidden(exeDir, issues);

            // 「今すぐ更新」の完了通知。更新を検知した回は通知 Toast が出ているため重ねない
            if (manualTriggered && notified == 0 && !g_shutdownRequested)
                showPollDoneToast();

            g_lastPollTick.store(GetTickCount64());
            waitInterruptible(calcSleepUntilNextPoll(pollsPerHour));
        }
        catch (...) {
            writeLog("unexpected error in polling loop");
            waitInterruptible(RETRY_WAIT_MS);
        }
    }

    // 通知音スレッドの終了待ちはここでは行わない。wmain のシャットダウン処理が
    // pollThread.join() の後に一元的に待つ。（401/404 の break で本スレッドが先に
    // 終了する経路でも、実際のプロセス終了時に必ず待たれることを保証するため）
}

// 通知音スレッドの完了を待つ（ダッキング復元を保証）
// pollThread の join 後に呼ぶこと。（g_soundThread への並行アクセスを避ける排他条件）
// タイムアウト時は静的オブジェクト（g_wavCache・g_logDir）の破棄と走行中スレッドが
// 競合して use-after-free になるのを防ぐため、CRT の静的破棄を走らせず
// ExitProcess(exitCode) で即終了する。正常終了と catch の異常終了の両経路から呼ぶ。
static void waitSoundThreadOrExit(UINT exitCode) {
    if (!g_soundThread) return;
    DWORD r = WaitForSingleObject(g_soundThread, 5000);
    if (r != WAIT_TIMEOUT) {
        CloseHandle(g_soundThread);
        g_soundThread = nullptr;
    }
    else {
        writeLog("shutdown: sound thread did not finish within 5s, exiting without static destruction");
        ExitProcess(exitCode);
    }
}

// エントリポイント
// ログ初期化 → 多重起動制御（Job Object、新プロセス優先）→ WinRT・トレイ・NIC 監視の初期化
// → 設定読込と必須キーの静的検証 → ポーリングスレッド起動 → メッセージループ → 終了処理の順。
// 設定不備では終了せず無効モードで常駐する。
// 終了コード：0 = 正常、1 = トレイウィンドウ生成失敗（fail-fast）、2 = 予期しない初期化エラー。
int wmain() {
    // ログ初期化（Job Object 処理前に実施してすべてのイベントをログに残す）
    auto exeDir = getExeDir();
    g_exeDir = exeDir;
    g_logDir = exeDir + L"\\logs";
    CreateDirectoryW(g_logDir.c_str(), nullptr);

    // 多重起動制御（新プロセス優先）
    // 名前付き Job Object で旧プロセスをまとめて終了させる。
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE により hJob は閉じずプロセス終了まで保持する。
    HANDLE hJob = CreateJobObjectW(nullptr, L"Local\\redntfy_job");
    if (hJob && GetLastError() == ERROR_ALREADY_EXISTS) {
        writeLog("terminating previous instance");
        TerminateJobObject(hJob, 0);
        CloseHandle(hJob);
        // カーネルが Job Object 名を解放するまで待機
        Sleep(100);
        hJob = CreateJobObjectW(nullptr, L"Local\\redntfy_job");
        // 旧プロセスがまだ終了していない場合の競合対策（警告のみで続行）
        if (hJob && GetLastError() == ERROR_ALREADY_EXISTS) {
            writeLog("warning: previous instance still alive");
            CloseHandle(hJob);
            hJob = nullptr;
        }
    }
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            writeLog("warning: failed to set job object limits");
        }
        if (!AssignProcessToJobObject(hJob, GetCurrentProcess())) {
            writeLog("warning: failed to assign to job object");
        }
    }
    else {
        writeLog("warning: failed to create job object");
    }

    // スレッド変数は try の外に置く。try 内ローカルにすると、スレッド生成後の例外で
    // catch へ入る前に joinable なまま破棄され std::terminate になり、
    // catch のログも終了コード 2 も残らない。（[thread.thread.destr]）
    std::thread updateThread;
    std::thread pollThread;

    try {
        winrt::init_apartment();
        SetCurrentProcessExplicitAppUserModelID(APP_AUMID);
        ensureShortcut();
        WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
        g_hWnd = createTrayWindow();
        if (!g_hWnd) {
            // 他の初期化失敗は warning で続行するが、ここだけは fail-fast とする。
            // トレイウィンドウ無しの常駐は操作面も終了経路も持たず、仕様上意味がないため
            writeLog("fatal: tray window creation failed - exiting");
            return 1;
        }
        WTSRegisterSessionNotification(g_hWnd, NOTIFY_FOR_THIS_SESSION);

        // NIC 変化（Wi-Fi 接続/切断、LAN 抜き差し等）の監視を登録
        // FALSE: 登録時に既存インターフェースの初期通知は不要
        HANDLE hNetNotify = nullptr;
        if (NotifyIpInterfaceChange(AF_UNSPEC, onNetworkChange, nullptr, FALSE, &hNetNotify) != NO_ERROR) {
            writeLog("NotifyIpInterfaceChange failed: " + std::to_string(GetLastError()));
            hNetNotify = nullptr;
        }

        auto cfg = loadConfig(exeDir);

        // 必須キーの静的検証（欠けていても終了せず、無効モードで常駐する）
        // 2 分岐（url・api_key）で原因ごとに案内する。url のスキームもここで検証する。
        // InvalidUrl のとき redmineUrl は ShellExecuteW を通るどの経路にも到達しない：
        // ポーリング停止・一覧非表示（フッタのクエリ画面も開けない）・enterDisabledMode は
        // InvalidUrl では Redmine 由来の URL を開かない。（定数のガイド URL は開く）
        // これにより http(s) 以外が ShellExecuteW に渡らない保証を維持する。
        DisabledReason initReason = DisabledReason::None;
        if (cfg.redmineUrl.empty() || !isHttpUrl(cfg.redmineUrl)) {
            writeLog("config error: [redmine] url is empty or not http(s) - disabled mode");
            initReason = DisabledReason::InvalidUrl;
        }
        else if (cfg.apiKey.empty()) {
            writeLog("config error: [redmine] api_key is empty - disabled mode");
            initReason = DisabledReason::InvalidApiKey;
        }
        // query_ids 省略はフォールバックモード（担当チケット追跡）として正常動作する
        if (initReason == DisabledReason::None && cfg.queryIds.empty())
            writeLog("config: query_ids not set - assigned-to-me fallback mode");
        // addTrayIcon より前に確定させる（登録時から無効アイコンで出すため）
        g_disabledReason.store(static_cast<int>(initReason));

        g_currentConfig = cfg;  // スレッド起動前に 1 回だけ設定（以降は不変・ロック不要）

        // 通知音を読み込みノーマライズしてキャッシュに格納（以降の再生はキャッシュを使用）
        loadWavAndNormalize(exeDir, cfg);

        // toml のホバー遅延・クリック猶予を確定（0〜5000 にクランプ済みの値）
        g_hoverDelayMs.store(static_cast<DWORD>(cfg.hoverDelayMs));
        g_hoverClickGuardMs.store(static_cast<DWORD>(cfg.hoverClickGuardMs));

        addTrayIcon(g_hWnd);

        // レジストリから設定を復元（キー未作成時はデフォルト値）
        g_soundEnabled  = readRegDword(REG_SOUND_ENABLED, 1u) != 0;
        g_muteInMeeting = readRegDword(REG_MUTE_IN_MEETING, 1u) != 0;
        g_assignedToMeOnly = readRegDword(REG_ASSIGNED_TO_ME, 0u) != 0;
        g_sortByDue        = readRegDword(REG_SORT_BY_DUE, 0u) != 0;
        g_excludeNoVersion = readRegDword(REG_EXCLUDE_NO_VERSION, 0u) != 0;
        g_muteOwnChanges   = readRegDword(REG_MUTE_OWN_CHANGES, 1u) != 0;
        g_excludeHidden    = readRegDword(REG_EXCLUDE_HIDDEN, 0u) != 0;
        g_hoverPopupEnabled = readRegDword(REG_HOVER_POPUP, 1u) != 0;

        writeLog("started");
        logSchedule(cfg.schedule);
        // どのクエリを追跡しているかは state.json の記録と通知挙動に直結するため起動時に残す
        {
            std::string s;
            for (int q : cfg.queryIds) s += (s.empty() ? "" : ",") + std::to_string(q);
            writeLog("query_ids: [" + s + "]"
                + (cfg.queryIds.empty() ? " (assigned-to-me fallback)" : ""));
        }

        // 更新チェックスレッド起動（起動時に 1 回のみ実行）
        // detach しない。プロセス終了時の静的破棄と競合しないよう、シャットダウン時に join する。
        // （HTTP タイムアウトは最大 30 秒のため、起動直後に即終了した場合のみ join が待つ）
        if (cfg.updateCheckEnabled) {
            try {
                updateThread = std::thread(checkForUpdates);
            }
            catch (const std::system_error& e) {
                writeLog(std::string("failed to start update check thread: ") + e.what());
            }
        }

        // 一覧・メニュー描画用フォントを初期化（一覧ポップアップとバージョン通知行が使用する）
        initMenuFonts();

        // ピン留めと非表示チケットを復元する（起動直後のポーリング前でも一覧に反映するため）
        loadPins(exeDir);
        loadHidden(exeDir);

        // ポーリングスレッド起動（無効モード時は起動しない：設定はホットリロードしないため
        // 何度試行しても結果が変わらず、案内はトレイ tooltip と Toast が担う）
        // メインスレッドはメッセージループに専念させるため、Redmine API ポーリング（HTTP I/O）を別スレッドへ分離する。
        // これによりネットワーク状態にかかわらずトレイアイコン右クリック等の UI が常時応答する。
        if (initReason == DisabledReason::None) {
            pollThread = std::thread(pollThreadFunc, exeDir, cfg);
        }
        else {
            enterDisabledMode(g_hWnd, initReason, cfg, exeDir);
        }

        // メッセージループ（純粋）
        // GetMessage は WM_QUIT で 0 を返してループを抜ける。
        // WM_QUIT は IDM_EXIT 等の終了経路で PostQuitMessage(0) により投函される。
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // メッセージループ終了 → シャットダウン処理開始
        g_shutdownRequested = true;

        // NIC 変化監視を解除してからスレッドを停止（コールバック発火を先に止める）
        // CancelMibChangeNotify2 は実行中コールバックの完了を待ってリターンするため UAF は発生しない（MSDN 保証）
        if (hNetNotify) CancelMibChangeNotify2(hNetNotify);

        // ループ終了後のクリーンアップ
        WTSUnRegisterSessionNotification(g_hWnd);
        removeTrayIcon(g_hWnd);

        // ポーリングスレッドを停止（waitInterruptible が 100 ms 単位でフラグを監視している）
        // 無効モード起動時はスレッド未起動のため joinable で判定する。
        // トレイアイコン削除より後に置き、ユーザから見た終了は即座に完了させる。
        // ただし PostMessage(g_hWnd, ...) を発火し得るため DestroyWindow より前で必ず join する。
        if (pollThread.joinable()) pollThread.join();

        DestroyWindow(g_hWnd);
        g_hWnd = nullptr;  // 破棄済みハンドルの再利用を防ぐ（catch の後始末ガードが誤発火しないため）

        // 更新チェックスレッドの完了を待つ（未起動・起動失敗時は joinable が false）
        // HTTP がブロック中だと最大タイムアウト（約 45 秒）まで待ち得るため、
        // トレイアイコン削除より後に置き、ユーザから見た終了は即座に完了させる
        if (updateThread.joinable()) updateThread.join();

        waitSoundThreadOrExit(0);

        writeLog("shutdown");
    }
    catch (...) {
        // 原因ログを最初に残す。後続の join（最大 45 秒）や waitSoundThreadOrExit の
        // ExitProcess で、終了コード 2 の理由がログに残らないまま終わるのを防ぐ
        writeLog("unexpected initialization error");
        // joinable なスレッドを残したまま return すると静的破棄と競合するため、
        // 停止要求を立ててから join して畳む
        g_shutdownRequested = true;
        if (pollThread.joinable()) pollThread.join();
        if (updateThread.joinable()) updateThread.join();
        // 正常終了パスと同様にトレイ登録を後始末する。（怠るとプロセス消滅後も
        // ゴーストアイコンが残る）通知音スレッドも待ち、静的破棄との競合を防ぐ
        if (g_hWnd) {
            WTSUnRegisterSessionNotification(g_hWnd);
            removeTrayIcon(g_hWnd);
            DestroyWindow(g_hWnd);
            g_hWnd = nullptr;
        }
        waitSoundThreadOrExit(2);
        return 2;
    }

    return 0;
}
