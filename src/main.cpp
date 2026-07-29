// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * redntfy - Redmine の更新チケットを Windows Toast 通知で知らせる常駐アプリ
 *
 * exe 同フォルダの redntfy.toml（redntfy.local.toml がキー単位で上書き）から設定を読み込み、
 * [redmine] で指定した複数のグローバル保存クエリ（query_ids）を schedule に従ってポーリングし、
 * チケット id で重複排除した和集合を追跡する。
 * schedule は 0 時〜23 時の 24 要素配列。（回/時、0 でその時間帯は休止）
 * 追跡集合への新規流入と既知チケットの updated_on 進行・新クエリ流入を Toast 通知と音声で知らせる。
 * 自分が起票したチケットの流入（author.id で判定）は通知しない。
 * 自分の操作による更新と、前回ポーリング以降の自分の更新が原因の流入（最終 journal の user.id で判定）も通知しない。
 * 検知済み状態は「チケット id → updated_on ＋所属クエリ集合」を state.json（v2）に永続化して重複通知を防ぐ。
 * トレイ左クリックで未処理チケットの一覧を表示し、行の右クリックで最大 5 件をピン留めできる。
 * ピンは pins.json に永続化し、保存クエリの集合から外れたチケットも一覧に表示し続ける。
 *
 * 終了コード：
 *   0  - 正常終了（トレイメニューの「終了」による）
 *   1  - 設定エラー（[redmine] url / api_key / query_ids の未設定）
 *   2  - 予期しない初期化エラー
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

// トレイアイコン用メッセージ ID
static constexpr UINT WM_TRAYICON        = WM_USER + 1;
static constexpr UINT WM_UPDATE_TOOLTIP  = WM_USER + 2;

// コンテキストメニューコマンド ID
static constexpr UINT IDM_EXIT             = 40002;
static constexpr UINT IDM_MUTE_IN_MEETING  = 40004;
static constexpr UINT IDM_SOUND_ENABLED       = 40005;
static constexpr UINT IDM_OPEN_CONFIG         = 40006;
static constexpr UINT IDM_OPEN_LOG            = 40007;
static constexpr UINT IDM_OPEN_GITHUB         = 40008; // GitHub リポジトリページを開く
static constexpr UINT IDM_OPEN_QUERY          = 40009; // Redmine の保存クエリ画面を開く
static constexpr UINT IDM_STARTUP             = 40010; // Windows スタートアップ登録トグル
static constexpr UINT IDM_ASSIGNED_TO_ME      = 40011; // 担当がグループのチケットを一覧・tooltip・通知から除外するトグル
static constexpr UINT IDM_UPDATE_NOW          = 40012; // 休止時間帯・クールダウンを無視した即時ポーリング
static constexpr UINT IDM_SORT_BY_DUE         = 40013; // 一覧を期日昇順に並べるトグル

static constexpr wchar_t GITHUB_URL[]                 = L"https://github.com/aviscaerulea/redntfy";
static constexpr wchar_t GITHUB_RELEASES_URL[]        = L"https://github.com/aviscaerulea/redntfy/releases";
static constexpr wchar_t GITHUB_API_RELEASES_LATEST[] = L"https://api.github.com/repos/aviscaerulea/redntfy/releases/latest";

// 左クリック一覧のチケット項目（IDM_ISSUE_BASE + index で最大 50 件）
static constexpr UINT IDM_ISSUE_BASE = 41000;
static constexpr UINT IDM_ISSUE_MAX  = 41050;

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

// ピン留めの上限件数（仕様値。一覧は list_limit + 本値が表示最大）
static constexpr size_t PIN_LIMIT = 5;

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

// トレイウィンドウのハンドル（メインスレッドで作成し、ポーリングループと通知スレッドが参照）
static HWND g_hWnd = nullptr;

// トレイのポップアップメニュー表示中フラグ（ツールチップ更新抑制用）
static std::atomic<bool> g_popupShowing{false};

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
    bool        closed    = false; // closed_on が非 null（一覧で打ち消し線表示）
    std::string dueDate;           // due_date（"YYYY-MM-DD"、期限なしは空。一覧の日付表示に使う）
    std::vector<int> queryIds;     // このチケットが現れた保存クエリ id（昇順）。クエリ流入の検知に使う
    bool assignedToGroup = false;  // 担当がグループ（一覧の 👥 マーカー。取得後にグループ id 集合と突合して設定）
    // 最終更新者（resolveUpdaters が journals から確定する。journal なしは起票者で代替）
    int         updaterId = 0;     // 自分の操作による通知の抑止判定に使う（0 = 未確定）
    std::string updaterName;       // フルネーム（Toast の「更新：○○」表示用）
    std::string updaterDisplay;    // 一覧の表示名（姓。取得できない場合はフルネーム）
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
    std::string updaterDisplay;    // 最終更新者の表示名（姓。集合外ピンも表示できるよう永続化）
};

// loadConfig の戻り値
struct Config {
    // [redmine] 接続設定（必須。いずれか欠けると起動を中止する）
    std::wstring redmineUrl;       // Redmine の URL（末尾スラッシュを除去して保持する）
    std::wstring apiKey;           // 個人 API アクセスキー
    // 追跡対象のグローバル保存クエリ id（1 個以上必須。設定の記述順を保持し std::set にしない）
    // 先頭要素は「代表クエリ」で、複数件 Toast と一覧フッタから開く URL に使う。
    std::vector<int> queryIds;

    std::vector<int>          schedule;         // 24 要素（0 時〜23 時の 1 時間あたりポーリング回数、0 で休止）
    int                       listLimit;        // 一覧の非ピン表示件数（デフォルト 20）
    int                       subjectMaxChars;  // 件名の省略文字数（デフォルト 40）
    int                       projectMaxChars;  // プロジェクト名の省略文字数（0 で非表示、デフォルト 5）
    std::vector<std::wstring> duckTargets;      // 通知音再生中にミュートするプロセス名

    // [guard] ガードトーン設定（BLE ヘッドホン対処）
    int   guardToneMs;      // ガードトーン長（冒頭・末尾共通、ms。0 で無効、デフォルト 1500）

    // [loudness] ラウドネスノーマライズ設定
    bool  loudnessEnabled;      // ノーマライズ有効/無効（デフォルト true）
    float loudnessTarget;       // 目標ラウドネス LUFS（デフォルト -16.0）
    float loudnessPeakCeiling;  // ピーク上限（デフォルト 0.891 = -1 dBFS）

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

// ポーリングスレッド → WndProc スレッド: チケット一覧の受け渡し（g_mtx で保護）
static std::mutex              g_mtx;
static std::vector<Issue>      g_issues;   // updated_on 降順ソート済み
static std::vector<PinEntry>   g_pins;     // ピン留め（g_mtx で保護、最大 PIN_LIMIT 件）

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

// トレイアイコンのバッジ状態
// NIM_MODIFY の無駄な呼び出しを抑制するために直前のバッジ有無を保持する
static bool                    g_trayBadgeActive  = false;
// updateTrayTooltip のリエントランシーガード
// Shell_NotifyIconW が内部でメッセージポンプして WM_TIMER 等を呼ぶことへの対処
static bool                    g_tooltipUpdating  = false;

// 更新チェック結果（起動時に 1 回書き込まれ、以降は読み取り専用）
static std::atomic<bool>  g_updateAvailable { false };
static std::wstring        g_latestVersion;   // g_mtx で保護

// 通知音再生スレッドのハンドル
//
// アクセスは pollThreadFunc 1 スレッドに限定する。launchSound（呼び出し元は pollThreadFunc）と、
// pollThreadFunc 末尾のシャットダウン処理がすべての書き換え箇所であり、
// 並行アクセスがないためミューテックス保護は不要。新たな呼び出し箇所を追加する場合は
// 必ず pollThreadFunc コンテキスト内であることを確認すること。
static HANDLE g_soundThread = nullptr;

// exe ディレクトリパス（wmain 起動時に確定し、WndProc スレッドからも参照する）
static std::wstring g_exeDir;

// 左クリックポップアップのチケット項目描画用フォント（initMenuFonts で初期化）
static HFONT g_hMenuFont     = nullptr;
static HFONT g_hMenuFontBold = nullptr;  // 未読行用の太字（フェイス・サイズは g_hMenuFont と同一）

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
    bool         overdue = false; // 期限 ≦ 今日（JST）＝日付部分を赤で描く
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

// Toast XML の特殊文字をエスケープする
static std::wstring escapeXml(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size() + 16);
    for (wchar_t c : s) {
        switch (c) {
        case L'&':  r += L"&amp;";  break;
        case L'<':  r += L"&lt;";   break;
        case L'>':  r += L"&gt;";   break;
        case L'"':  r += L"&quot;"; break;
        default:    r += c;
        }
    }
    return r;
}

// https:// または http:// のみ許可する（任意プロトコルハンドラ悪用防止）
static bool isHttpUrl(const std::wstring& url) {
    return url.starts_with(L"https://") || url.starts_with(L"http://");
}

// UTF-8 std::string を UTF-16 std::wstring に変換する
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// UTF-16 std::wstring を UTF-8 std::string に変換する
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
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

// schedule 配列と1日の概算ポーリング回数をログ出力する
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
// outStatusCode が非 null の場合、最終 HTTP ステータスコードを書き込む（失敗時は 0）
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

    std::string respBody;
    std::vector<char> buf;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        if (buf.size() < avail) buf.resize(avail);
        DWORD read = 0;
        if (WinHttpReadData(hRequest, buf.data(), avail, &read)) respBody.append(buf.data(), read);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
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

// JSON 文字列をアトミックにファイルへ書き出す（"<path>.tmp" 経由で MoveFileEx 置換）
// 電源断・クラッシュで本体ファイルが壊れる可能性を避ける。
// logTag はエラー出力用の識別子（"state" / "pins" 等）。成功時 true、失敗時 false。
static bool atomicWriteJson(const std::wstring& path, const std::string& json,
    const char* logTag)
{
    auto tmpPath = path + L".tmp";
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
                if (qid > 0) st.knownQueries.push_back(qid);
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
                if (o.HasKey(L"queries")) {
                    e.hasQueries = true;
                    for (auto q : o.GetNamedArray(L"queries")) {
                        int qid = static_cast<int>(q.GetNumber());
                        if (qid > 0) e.queryIds.push_back(qid);
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
        for (int q : cfg.queryIds) qarr.Append(JsonValue::CreateNumberValue(q));
        root.Insert(L"queries", qarr);
        JsonArray arr;
        for (const auto& is : issues) {
            JsonObject o;
            o.Insert(L"id",         JsonValue::CreateNumberValue(is.id));
            o.Insert(L"updated_on", JsonValue::CreateStringValue(winrt::to_hstring(is.updatedOn)));
            o.Insert(L"updater_id", JsonValue::CreateNumberValue(is.updaterId));
            o.Insert(L"updater",    JsonValue::CreateStringValue(winrt::to_hstring(is.updaterDisplay)));
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
                o.Insert(L"updater",    JsonValue::CreateStringValue(winrt::to_hstring(p.updaterDisplay)));
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
// 起動時に 1 回呼び出し、PIN_LIMIT 件を上限に g_pins へ復元する。
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
            if (g_pins.size() >= PIN_LIMIT) break;
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
            // updater は集合内ピンのみ refreshPins で実値になる。（集合外ピンの個別取得は
            // journals を含まないため、保存値のまま維持される）
            p.updaterDisplay  = winrt::to_string(o.GetNamedString(L"updater", L""));
            if (p.id > 0) g_pins.push_back(std::move(p));
        }
        writeLog("pins: loaded " + std::to_string(g_pins.size()) + " entries");
    }
    catch (...) {
        writeLog("pins: load failed (exception)");
    }
}

// ==================== 設定読み込み ====================

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
    const auto* arr = (*tbl)["schedule"].as_array();
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

    // duck_targets 配列の読み込み（キーが存在する側を採用。local 優先）
    auto readDuckTargets = [&](const std::optional<toml::table>& tbl)
            -> std::optional<std::vector<std::wstring>> {
        if (!tbl) return std::nullopt;
        const auto* arr = (*tbl)["duck_targets"].as_array();
        if (!arr) return std::nullopt;
        std::vector<std::wstring> targets;
        for (const auto& el : *arr) {
            if (auto s = el.value<std::string>()) targets.push_back(toWide(*s));
        }
        return targets;
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

    if (auto d = readDuckTargets(local))      cfg.duckTargets = std::move(*d);
    else if (auto d = readDuckTargets(base))  cfg.duckTargets = std::move(*d);

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
    // トップレベル整数（セクションなし）
    auto readTopInt = [&](const char* key, int def, int lo, int hi) -> int {
        long long v = def;
        if (local && (*local)[key].is_integer())      v = **(*local)[key].as_integer();
        else if (base && (*base)[key].is_integer())   v = **(*base)[key].as_integer();
        return static_cast<int>((std::max)((long long)lo, (std::min)((long long)hi, v)));
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

    // [redmine] 接続設定（必須。欠落チェックは wmain で行い、欠けていれば起動を中止する）
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

    // 一覧の表示件数と各要素の省略文字数
    cfg.listLimit       = readTopInt("list_limit", 20, 1, 25);
    cfg.subjectMaxChars = readTopInt("subject_max_chars", 40, 10, 120);
    cfg.projectMaxChars = readTopInt("project_max_chars", 5, 0, 20);

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

// 代表クエリ（query_ids の先頭）の画面 URL（{url}/issues?query_id={qid}）
// 複数件 Toast と一覧フッタの遷移先。和集合を表す URL は Redmine に無いため先頭で代表する。
static std::wstring queryUrl(const Config& cfg) {
    // wmain の検証で非空が保証されるが、空でも 0 に落として未定義動作を作らない
    int qid = cfg.queryIds.empty() ? 0 : cfg.queryIds.front();
    return cfg.redmineUrl + L"/issues?query_id=" + std::to_wstring(qid);
}

// issue JSON オブジェクトを Issue に変換する
// closed_on はキー不在と null 値の両方を「未クローズ」として扱う。（Redmine の版による差異対策）
static Issue parseIssueObject(const winrt::Windows::Data::Json::JsonObject& obj) {
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
    // 一覧表示にしか使わないため name だけ採る。
    if (obj.HasKey(L"project")) {
        auto proj = obj.GetNamedObject(L"project", nullptr);
        if (proj) is.projectName = normalizeSpaces(winrt::to_string(proj.GetNamedString(L"name", L"")));
    }
    // assigned_to はキー自体が無ければ未割当。ユーザとグループで形は同じ。（id と name のみ）
    if (obj.HasKey(L"assigned_to")) {
        auto assignee = obj.GetNamedObject(L"assigned_to", nullptr);
        if (assignee) is.assignedToId = static_cast<int>(assignee.GetNamedNumber(L"id", 0));
    }
    if (obj.HasKey(L"closed_on")) {
        auto v = obj.GetNamedValue(L"closed_on", nullptr);
        is.closed = v && v.ValueType() == JsonValueType::String
                    && !winrt::to_string(v.GetString()).empty();
    }
    // due_date はキー不在と null（期限なし）の両方を空として扱う。
    // GetNamedString は値が null のとき例外になるため、closed_on と同じ判定形にする。
    if (obj.HasKey(L"due_date")) {
        auto v = obj.GetNamedValue(L"due_date", nullptr);
        if (v && v.ValueType() == JsonValueType::String) is.dueDate = winrt::to_string(v.GetString());
    }
    return is;
}

// /users/current.json から自分の user id と所属グループ id を取得する（起動時 1 回）
// 失敗時は 0 を返す。0 のときは自分の操作の除外判定を行わない。（通知欠落より過剰通知側に倒す）
// outOwnGroups は成功時のみ上書きする。グループ担当判定（/groups.json）が権限不足で
// 使えない場合のフォールバック用。
static int fetchMyUserId(const Config& cfg, std::vector<int>& outOwnGroups) {
    DWORD status = 0;
    auto body = redmineGet(cfg.redmineUrl + L"/users/current.json?include=groups", cfg.apiKey, &status);
    if (status != 200 || body.empty()) {
        writeLog("fetchMyUserId: request failed, status=" + std::to_string(status));
        return 0;
    }
    try {
        auto obj  = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
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
        writeLog("fetchMyUserId: JSON parse failed");
        return 0;
    }
}

// /groups.json から全グループの id を取得する（グループ担当マーカーの判定用、起動時 1 回）
// この API は admin 権限が必要。403（権限なし）は確定的な失敗として outStatus で呼び出し側に
// 伝え、所属グループへのフォールバックを促す。接続エラー等は nullopt で再試行対象とする。
static std::optional<std::vector<int>> fetchAllGroupIds(const Config& cfg, DWORD* outStatus) {
    DWORD status = 0;
    auto body = redmineGet(cfg.redmineUrl + L"/groups.json", cfg.apiKey, &status);
    if (outStatus) *outStatus = status;
    if (status != 200 || body.empty()) {
        writeLog("fetchAllGroupIds: request failed, status=" + std::to_string(status));
        return std::nullopt;
    }
    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
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
        writeLog("fetchAllGroupIds: JSON parse failed");
        return std::nullopt;
    }
}

// 担当者 id がグループかどうかを判定する（一覧の 👥 マーカー用）
// groupIds は起動時に確定した判定集合。（全グループ、または権限不足時は自分の所属グループ）
static bool isGroupAssignee(const std::vector<int>& groupIds, int assignedToId) {
    return assignedToId != 0
        && std::find(groupIds.begin(), groupIds.end(), assignedToId) != groupIds.end();
}

// 保存クエリ 1 件の結果を total_count に達するまでページングして取得する
// 成功時 true。ソートは行わない。（呼び出し側が和集合を作ってからまとめてソートする）
// API の sort には依存しない。（query_id 側のソート設定に左右されないため取得後にローカルでソートする）
// 失敗ログにクエリ id を含めるのは、複数クエリ運用でどのクエリが壊れているかを
// ログだけで切り分けられるようにするため。
static bool fetchQueryIssues(const Config& cfg, int queryId, std::vector<Issue>& outIssues) {
    using namespace winrt::Windows::Data::Json;
    outIssues.clear();

    const std::string logTag = "fetchQueryIssues(" + std::to_string(queryId) + ")";
    int offset = 0;
    int total  = 0;
    do {
        std::wstring url = cfg.redmineUrl + L"/issues.json?query_id=" + std::to_wstring(queryId)
            + L"&limit=100&offset=" + std::to_wstring(offset);
        DWORD status = 0;
        auto body = redmineGet(url, cfg.apiKey, &status);
        if (status != 200 || body.empty()) {
            writeLog(logTag + ": request failed, status=" + std::to_string(status)
                + " offset=" + std::to_string(offset));
            return false;
        }
        try {
            auto obj = JsonObject::Parse(winrt::to_hstring(body));
            if (obj.HasKey(L"errors")) {
                writeLog(logTag + ": API error response");
                return false;
            }
            total = static_cast<int>(obj.GetNamedNumber(L"total_count", 0));
            auto arr = obj.GetNamedArray(L"issues");
            if (arr.Size() == 0) break;  // total_count 不整合による無限ループ防止
            for (auto item : arr) {
                auto is = parseIssueObject(item.GetObject());
                if (is.id > 0 && !is.updatedOn.empty()) outIssues.push_back(std::move(is));
            }
            offset += static_cast<int>(arr.Size());
        }
        catch (...) {
            writeLog(logTag + ": JSON parse failed");
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
// 1 クエリでも失敗したら全体を false として部分結果を破棄する。欠落込みの集合で
// state.json を上書きすると次回に「新規」誤通知が出るため。（単一クエリ時代の方針を維持）
static bool fetchIssues(const Config& cfg, std::vector<Issue>& outIssues) {
    outIssues.clear();
    std::unordered_map<int, size_t> indexById;  // チケット id → outIssues の位置
    for (int qid : cfg.queryIds) {
        std::vector<Issue> part;
        if (!fetchQueryIssues(cfg, qid, part)) return false;
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
    DWORD status = 0;
    auto body = redmineGet(issueUrl(cfg, id) + L".json", cfg.apiKey, &status);
    if (status != 200 || body.empty()) return false;
    try {
        auto obj   = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
        auto issue = obj.GetNamedObject(L"issue", nullptr);
        if (!issue) return false;
        out = parseIssueObject(issue);
        return out.id > 0;
    }
    catch (...) {
        writeLog("fetchIssue: JSON parse failed (id=" + std::to_string(id) + ")");
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
    DWORD status = 0;
    auto body = redmineGet(issueUrl(cfg, id) + L".json?include=journals", cfg.apiKey, &status);
    if (status != 200 || body.empty()) return lu;
    try {
        auto obj   = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
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

// ユーザ id から姓を取得する（セッション内キャッシュ）
// Redmine のユーザ名文字列は姓名が無区切りで分割できない。一覧の最終更新者列に姓だけを
// 出すため /users/{id}.json の lastname を引く。取得失敗は空をキャッシュして毎回の再試行を
// 抑える。（呼び出し側がフルネームへフォールバックし、再起動で再試行される）
static std::string resolveLastName(const Config& cfg, int userId,
                                   std::unordered_map<int, std::string>& cache)
{
    if (userId <= 0) return {};
    auto it = cache.find(userId);
    if (it != cache.end()) return it->second;
    std::string lastName;
    DWORD status = 0;
    auto body = redmineGet(cfg.redmineUrl + L"/users/" + std::to_wstring(userId) + L".json",
                           cfg.apiKey, &status);
    if (status == 200 && !body.empty()) {
        try {
            auto obj  = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
            auto user = obj.GetNamedObject(L"user", nullptr);
            if (user) lastName = winrt::to_string(user.GetNamedString(L"lastname", L""));
        }
        catch (...) {
            writeLog("resolveLastName: JSON parse failed (user=" + std::to_string(userId) + ")");
        }
    }
    else {
        writeLog("resolveLastName: request failed, status=" + std::to_string(status)
            + " (user=" + std::to_string(userId) + ")");
    }
    cache[userId] = lastName;
    return lastName;
}

// 一覧・Toast 用に各チケットの最終更新者を確定する
// updated_on が前回ポーリングから変わっていないチケットは state.json のキャッシュを使う。
// 変わったチケットと新規のチケットだけ journals を取得する。（定常時の追加 HTTP は変化分のみ）
// journal の無いチケットは起票者を最終更新者として扱い、取得失敗は表示名を空のまま残して
// 次回ポーリングで再解決する。通知抑止（deliverPollResults）もここで確定した updaterId を
// 使うため、journals の取得は本関数だけで行う。
static void resolveUpdaters(const Config& cfg, std::vector<Issue>& issues,
                            const PollState& prev,
                            std::unordered_map<int, std::string>& lastNameCache)
{
    for (auto& is : issues) {
        if (g_shutdownRequested) return;
        auto it = prev.issues.find(is.id);
        if (it != prev.issues.end() && it->second.updatedOn == is.updatedOn
            && !it->second.updaterDisplay.empty()) {
            is.updaterId      = it->second.updaterId;
            is.updaterDisplay = it->second.updaterDisplay;
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
        is.updaterDisplay = resolveLastName(cfg, dispId, lastNameCache);
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

// レジストリパス（ユーザー設定の永続化先）
static constexpr const wchar_t* REG_KEY_PATH        = L"SOFTWARE\\redntfy";
static constexpr const wchar_t* REG_SOUND_ENABLED     = L"SoundEnabled";
static constexpr const wchar_t* REG_MUTE_IN_MEETING   = L"MuteInMeeting";
static constexpr const wchar_t* REG_ASSIGNED_TO_ME    = L"AssignedToMeOnly";
static constexpr const wchar_t* REG_SORT_BY_DUE       = L"SortByDue";
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
// 全サンプルに乗算する。peak_ceiling を超える場合はゲインを制限する。
// ほぼ無音（ピーク < 1e-6f）の場合はスキップする。
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
    constexpr float AMPLITUDE = 0.001f;   // 振幅（約 -60 dB）

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
// 再生フロー（guardEnabled が true の場合）:
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
                for (int i = 0; i < 100; i++) {
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
// 再生フロー（guard.enabled が true の場合）:
//   ガードトーン（リードイン）→ 通知音（チャイム）→ ガードトーン（リードアウト）
// g_wavCache.valid == false の場合は音声を再生せずに終了する。（Toast 通知は呼び出し側で別途表示）
// ダッキング: cfg.duckTargets に指定されたプロセスを再生中ミュートし、全再生完了後に復元する。
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
static void dispatchToastXml(std::wstring xml, const std::wstring& permalink) {
    if (!permalink.empty() && isHttpUrl(permalink)) {
        xml += L"<actions>"
               L"<action activationType=\"protocol\" content=\"チケットを開く\""
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
// silent=true（デフォルト）: OS 通知音を無効化する。（アプリ側で sound.wav を鳴らすため）
// silent=false: <audio> タグを省略し OS 標準通知音を鳴らす。
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

// 3 行 Toast 通知を表示する（更新チェックの新版通知、「今すぐ更新」の完了通知用）
//
// line1 を title スタイル（太字大）で表示する。
// silent=true（デフォルト false）で OS 通知音を無効化する。
static void showToast3(const std::wstring& line1, const std::wstring& line2,
                       const std::wstring& line3, const std::wstring& permalink,
                       bool silent = false)
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

    dispatchToastXml(std::move(xml), permalink);
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
    g_trayBadgeActive = false;  // バッジ状態をリセットしてアイコン再登録後の差分検出を保証
    auto nid = makeTrayNid(hWnd);
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    wcscpy_s(nid.szTip, L"読み込み中...");
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
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

// トレイアイコンのバッジ切り替え（バッジ = 未読チケットあり）
// hasUnread が g_trayBadgeActive（前回状態）と同じなら NIM_MODIFY をスキップする。
static void updateTrayIcon(HWND hWnd, bool hasUnread) {
    if (hasUnread == g_trayBadgeActive) return;
    g_trayBadgeActive = hasUnread;

    auto nid   = makeTrayNid(hWnd);
    nid.uFlags = NIF_ICON;
    if (hasUnread) {
        nid.hIcon = createBadgedIcon();
        if (!nid.hIcon)
            nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    }
    else {
        nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
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
    std::string updater;
    std::string dueDate;              // "YYYY-MM-DD"（期日なしは空）
    std::string updatedOn;
    bool        assignedToGroup = false;
    bool        pinned          = false;
    bool        closed          = false;
    bool        unread          = false;
};

// 一覧に出す行を選定し、並べ替えて list_limit 件へ絞る
//
//   1. g_issues から担当者フィルタを通った行をすべて採る
//      （フィルタで外れてもピン留め済みなら残す。ピンはフィルタより優先する）
//   2. 1 に含まれないピンを追加する（保存クエリの集合外ピンはキャッシュ内容で表示）
//   3. 全体を並べ替える（既定は updated_on 降順。「期日順に並べる」ON なら期日昇順で
//      期日なしは末尾。ピンも同じ規則で本来の位置に置く）
//   4. 先頭 list_limit 件へ絞る（ピン留めは上限適用外で常に残す）
// visible には担当者フィルタを通った未処理件数（絞り込み前）を返す。ピン留めは数えない。
// （明示の意思表示であって未処理件数ではないため、フィルタで外れたピンを件数に足し戻さない）
// tooltip の未読件数も本関数の結果から数える。表示と同じ選定を通すことで「未読 N 件」と
// 画面上の太字行数を一致させる。（一覧に出ない未読は数に出さず、バッジも点けない）
static std::vector<ListRow> buildListRows(int& visible) {
    std::vector<Issue>      issues;
    std::vector<PinEntry>   pins;
    std::unordered_set<int> unread;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        issues = g_issues;
        pins   = g_pins;
        unread = g_unreadIds;
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
        // 担当者フィルタで外れた行は出さない。ただしピン留め済みは明示の意思表示として常に残す
        // （クローズ済・集合外でも表示する既存のピン仕様と揃える）
        bool pinned = isPinned(is.id);
        if (!passesAssigneeFilter(is)) {
            if (!pinned) continue;
        }
        else {
            ++visible;
        }
        rows.push_back({.id = is.id, .subject = is.subject, .projectName = is.projectName,
                        .updater = is.updaterDisplay, .dueDate = is.dueDate,
                        .updatedOn = is.updatedOn, .assignedToGroup = is.assignedToGroup,
                        .pinned = pinned, .closed = is.closed,
                        .unread = unread.count(is.id) != 0});
        shown.insert(is.id);
    }
    for (const auto& p : pins) {
        if (shown.count(p.id)) continue;
        rows.push_back({.id = p.id, .subject = p.subject, .projectName = p.projectName,
                        .updater = p.updaterDisplay, .dueDate = p.dueDate,
                        .updatedOn = p.updatedOn, .assignedToGroup = p.assignedToGroup,
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
        for (auto& r : rows) {
            if (kept.size() < limit || r.pinned) kept.push_back(std::move(r));
        }
        rows = std::move(kept);
    }
    return rows;
}

// トレイアイコンのツールチップを更新する
// 「未処理 N 件」に未読があれば「（未読 M 件）」を続けて表示し、赤バッジは未読ありを表す。
// 未読件数は一覧に出る行から数えるため、画面上の太字行数と一致する。
// ポップアップメニュー表示中は更新しない
static void updateTrayTooltip(HWND hWnd) {
    if (g_popupShowing.load()) return;
    if (g_tooltipUpdating) return;
    g_tooltipUpdating = true;

    int visible = 0;
    auto rows = buildListRows(visible);
    int unread = 0;
    for (const auto& r : rows) {
        if (r.unread) ++unread;
    }
    std::wstring tip = L"未処理 " + std::to_wstring(visible) + L" 件";
    if (unread > 0) tip += L"（未読 " + std::to_wstring(unread) + L" 件）";

    auto nid = makeTrayNid(hWnd);
    nid.uFlags = NIF_TIP;
    wcscpy_s(nid.szTip, tip.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    updateTrayIcon(hWnd, unread > 0);
    g_tooltipUpdating = false;
}

// トレイアイコンを除去する
static void removeTrayIcon(HWND hWnd) {
    auto nid = makeTrayNid(hWnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}


// メニュー描画用フォントの初期化
// OS のメニューフォント設定を取得して左クリックポップアップのチケット項目描画用フォントを作成する。
static void initMenuFonts() {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hMenuFont = CreateFontIndirectW(&ncm.lfMenuFont);
    // 未読行の太字はメニューフォントのウェイトだけ変えて作る（フェイス・サイズは揃える）
    LOGFONTW lfBold = ncm.lfMenuFont;
    lfBold.lfWeight = FW_BOLD;
    g_hMenuFontBold = CreateFontIndirectW(&lfBold);
}

// 左クリックポップアップのチケット項目（IDM_ISSUE_BASE + index に対応、WndProc スレッドのみ使用）
struct IssueItem {
    int          id     = 0;
    std::wstring url;            // {redmine.url}/issues/{id}
    std::wstring label;          // 描画テキスト（WM_DRAWITEM / WM_MEASUREITEM で使用）
    // 日付部分だけ赤で描くための位置情報。セグメント文字列を別々に持つとラベル全体との
    // 不整合が起き得るため、label を単一の真実として位置だけを保持する。
    size_t       dateOffset = 0;
    size_t       dateLen    = 0;     // 0 = 期限なし（分割描画しない）
    bool         overdue    = false; // 期限 ≦ 今日（JST）＝日付部分を赤で描く
    bool         unread     = false; // 未読（まだ一覧から開いていない）＝太字で描く
    bool         pinned = false; // ピン留め中（マーカー列の描画条件。右クリックトグル時にもその場で更新する）
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
// アクセスは WndProc スレッド（WM_DRAWITEM）のみでロック不要。
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
// hdc は描画先のメニュー DC で、DirectWrite のフォントサイズをメニューフォントの
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

// 一覧行のラベル（描画テキストと、その中の日付部分の位置）
// 日付だけ色を変えて描くため位置と長さも返す。幅計測（measureIssueMenuItem）と打ち消し線は
// text 全体で行うため、text を単一の真実として保つ。
struct IssueLabel {
    std::wstring text;
    size_t       dateOffset = 0;  // text 内の日付開始位置（dateLen == 0 のとき無意味）
    size_t       dateLen    = 0;  // 日付部分の文字数（0 = 期限なし）
};

// グループ担当マーカー（👥 + 半角スペース）
// ラベルに埋め込み GDI で描く。フォントリンク経由の単色描画で足りるため、
// 📌 のような D2D カラー描画はしない。（単色でも輪郭が明瞭で意味が通る）
static constexpr wchar_t GROUP_MARK[] = L"👥 ";

// 一覧行のラベルを組み立てる
//   並びは「番号、最終更新者の姓、グループ担当マーカー、[プロジェクト名]、期日、件名」
//   （例："#12345  山田  👥 [ロケモニ] 7/28 件名"。番号と姓の後は空白 2、
//   閉じ角括弧と期日の後は空白 1）
//   角括弧は半角とし、project_max_chars の計数には含めない。
//   閉じ角括弧の後に空白を入れるのは、件名が [緊急] のように角括弧で始まる行で
//   境界を見失うため。
//   プロジェクト名は "…" を付けずに切り詰める。（先頭数文字で判別できる要素のため横幅を最優先する）
//   期日は今年以外だと "2025/6/30" のように年が付き、文字数が変わる。
//   更新者不明・期限なしなどの要素は詰めて省く。
// ピン記号はラベルに含めない。WM_DRAWITEM が IssueItem::pinned を見てマーカー列に描く。
static IssueLabel buildIssueLabel(int id, const std::string& subject, const std::string& updater,
                                  const std::wstring& dateText, bool assignedToGroup,
                                  const std::string& projectName) {
    IssueLabel r;
    r.text = L"#" + std::to_wstring(id) + L"  ";
    if (!updater.empty()) r.text += toWide(updater) + L"  ";
    if (assignedToGroup) r.text += GROUP_MARK;
    // 0 は非表示
    if (g_currentConfig.projectMaxChars > 0) {
        auto proj = truncateText(toWide(projectName),
                                 static_cast<size_t>(g_currentConfig.projectMaxChars), false);
        // 切り詰め位置が空白だと閉じ角括弧の直前が隙間になり、詰めた 1 桁を無駄にするため落とす
        while (!proj.empty() && (proj.back() == L' ' || proj.back() == L'　')) proj.pop_back();
        // 空判定は切り詰めた後に行う。角括弧だけが残ると意味のない装飾になるため要素ごと省く
        if (!proj.empty()) r.text += L"[" + proj + L"] ";
    }
    // 期日の位置はここで確定する。前に置く要素が増減しても追記の直前に測るため自動で追従する
    if (!dateText.empty()) {
        r.dateOffset = r.text.size();
        r.dateLen    = dateText.size();
        r.text += dateText + L" ";
    }
    r.text += truncateText(toWide(subject), static_cast<size_t>(g_currentConfig.subjectMaxChars));
    return r;
}

// 左クリック時のチケット一覧ポップアップ表示
//
// 表示行の選定・並べ替え・絞り込みは buildListRows に委ねる。（tooltip の未読件数と同じ根拠）
// 行の左クリックでチケットを開いてその 1 件だけ既読にする。
// 右クリックはピン留めのトグルで、既読にはしない。
// フッタの「未処理 N 件」はフィルタを通った件数で、フィルタで外れたピンは数えない。
static void showIssuePopup(HWND hWnd) {
    const Config& cfg = g_currentConfig;
    int visible = 0;
    auto rows = buildListRows(visible);

    // 「今日」は 1 回だけ求めて全行に使う。行ごとに求めると日付境界をまたいだ瞬間に
    // 同じ一覧内で赤判定が揺れる
    const int todayYmd = todayJstYmd();
    auto makeItem = [&](const ListRow& row) {
        IssueItem it;
        it.id  = row.id;
        it.url = issueUrl(cfg, row.id);
        auto due = makeDueDateView(row.dueDate, todayYmd);
        auto lbl = buildIssueLabel(row.id, row.subject, row.updater, due.text,
                                   row.assignedToGroup, row.projectName);
        it.label      = std::move(lbl.text);
        it.dateOffset = lbl.dateOffset;
        it.dateLen    = lbl.dateLen;
        it.overdue    = due.overdue;
        it.unread     = row.unread;
        it.pinned     = row.pinned;
        it.closed     = row.closed;
        return it;
    };

    g_issueItems.clear();
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        writeLog("showIssuePopup: CreatePopupMenu failed");
        return;
    }
    if (rows.empty()) {
        AppendMenuW(hMenu, MF_STRING, IDM_OPEN_QUERY, NO_ISSUES);
    }
    else {
        UINT idx = 0;
        for (const auto& row : rows) {
            if (idx >= (IDM_ISSUE_MAX - IDM_ISSUE_BASE)) break;
            // MFT_OWNERDRAW で WM_MEASUREITEM / WM_DRAWITEM に描画を委譲する。
            // dwItemData にインデックスを渡し、描画時に g_issueItems から参照する。
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask      = MIIM_FTYPE | MIIM_ID | MIIM_DATA;
            mii.fType      = MFT_OWNERDRAW;
            mii.wID        = IDM_ISSUE_BASE + idx;
            mii.dwItemData = static_cast<ULONG_PTR>(idx);
            InsertMenuItemW(hMenu, idx, TRUE, &mii);
            g_issueItems.push_back(makeItem(row));
            ++idx;
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        std::wstring footer = L"未処理 " + std::to_wstring(visible)
            + L" 件（クリックでウェブ表示 ／ 右クリックでピン留め）";
        AppendMenuW(hMenu, MF_STRING, IDM_OPEN_QUERY, footer.c_str());
    }

    POINT pt;
    GetCursorPos(&pt);
    forceForeground(hWnd);
    // TPM_LEFTBUTTON のみ指定する（TPM_RIGHTBUTTON を加えると右クリックも WM_COMMAND
    // を発火してしまい、ピン留めトグル用の WM_MENURBUTTONUP が届かなくなる）
    TrackPopupMenu(hMenu, TPM_LEFTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
}

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
static bool isNewerVersion(const std::wstring& a, const std::wstring& b) {
    int aMaj, aMin, aPat, bMaj, bMin, bPat;
    if (!parseVersion(a, aMaj, aMin, aPat)) return false;
    if (!parseVersion(b, bMaj, bMin, bPat)) return false;
    if (aMaj != bMaj) return aMaj > bMaj;
    if (aMin != bMin) return aMin > bMin;
    return aPat > bPat;
}

// GitHub の最新リリースを確認し、新版があれば Toast 通知とグローバル状態を更新する
// 起動時に detach したスレッドで 1 回だけ実行する
static void checkForUpdates() {
    winrt::init_apartment();
    // 予期しない例外でスレッドが std::terminate しないよう全体を保護する
    try {
        do {
            DWORD status = 0;
            std::string body = httpGet(GITHUB_API_RELEASES_LATEST, &status);
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
                               L"クリックしてリリースページを開いてください",
                               GITHUB_RELEASES_URL);
                }
                catch (...) {}
            }
        } while (false);
    }
    catch (...) {
        writeLog("update check: unexpected exception");
    }
    winrt::uninit_apartment();
}

// 更新通知メニュー項目のサイズを計算する
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
    SetTextColor(dis->hDC, selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(220, 0, 0));
    DrawTextW(dis->hDC, latest.c_str(), -1, &newVerRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    SelectObject(dis->hDC, oldFont);
    return TRUE;
}

// トレイアイコン用ウィンドウプロシージャ
// 右クリックトレイメニューの構築と表示
// メニュー項目はトグル状態（音声通知・スタートアップ等）を読み取り、
// その場で構築する。（チェック状態は呼び出し時の最新値を反映）
static void showTrayContextMenu(HWND hWnd) {
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
    AppendMenuW(hMenu, MF_STRING, IDM_UPDATE_NOW, L"今すぐ更新");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 担当者フィルタ（レジストリ永続化。一覧・tooltip・通知のすべてに効く）
    AppendMenuW(hMenu, MF_STRING | (g_assignedToMeOnly ? MF_CHECKED : MF_UNCHECKED),
        IDM_ASSIGNED_TO_ME, L"担当がグループのチケットを除外");

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

    // スタートアップ登録トグル（HKCU Run キー）
    AppendMenuW(hMenu, MF_STRING | (isStartupRegistered() ? MF_CHECKED : MF_UNCHECKED),
        IDM_STARTUP, L"スタートアップ登録");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_CONFIG, L"設定ファイルを開く");
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_LOG,    L"ログファイルを開く");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT,    L"終了");
    forceForeground(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
    g_popupShowing.store(false);
    updateTrayTooltip(hWnd);
}

// トレイアイコン左クリック時の処理
// チケット一覧ポップアップを表示する。
static void handleTrayLeftClick(HWND hWnd) {
    g_popupShowing.store(true);
    clearTrayTooltip(hWnd);
    showIssuePopup(hWnd);
    g_popupShowing.store(false);
    updateTrayTooltip(hWnd);
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
// TrackPopupMenu の WM_COMMAND がポップアップ終了前に届く環境がある。
// その場合は g_popupShowing 中の直接呼びが捨てられるため、PostMessage でキューに積む。
// 積んでおけば、WM_COMMAND がどちらの順序で届いてもポップアップ終了後に必ず反映される。
static void markIssueRead(int issueId) {
    bool wasUnread;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        wasUnread = g_unreadIds.erase(issueId) != 0;
    }
    if (wasUnread && g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
}

// WM_COMMAND ディスパッチ
// メニュー選択（IDM_*）と一覧クリック（IDM_ISSUE_BASE 以降。開いた 1 件を既読にする）を処理する。
static void handleTrayCommand(UINT id) {
    if (id == IDM_UPDATE_NOW) {
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
    if (id == IDM_STARTUP) {
        if (isStartupRegistered()) unregisterStartup();
        else                       registerStartup();
        return;
    }
    if (id == IDM_OPEN_GITHUB) {
        const wchar_t* url = g_updateAvailable.load() ? GITHUB_RELEASES_URL : GITHUB_URL;
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_QUERY) {
        // Redmine の保存クエリ画面をブラウザで開く（url は起動時に isHttpUrl 検証済み）
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
    if (id >= IDM_ISSUE_BASE && id < IDM_ISSUE_MAX) {
        UINT idx = id - IDM_ISSUE_BASE;
        if (idx < g_issueItems.size() && isHttpUrl(g_issueItems[idx].url)) {
            ShellExecuteW(nullptr, L"open", g_issueItems[idx].url.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            // URL の検証を通った行だけ既読にする。（不正な URL の行は開かないため未読のまま残す）
            // ShellExecuteW の戻り値は見ない。関連付け不備でブラウザが起動しない場合まで
            // 未読を守るより、経路を単純に保つ方を採る。
            markIssueRead(g_issueItems[idx].id);
        }
    }
}

// 左クリックポップアップの owner-draw 項目サイズ計算
// 戻り値：TRUE で処理済み、FALSE で未処理（DefWindowProcW へ）
static BOOL measureIssueMenuItem(HWND hWnd, MEASUREITEMSTRUCT* mis) {
    if (mis->CtlType != ODT_MENU) return FALSE;
    UINT eidx = static_cast<UINT>(mis->itemData);
    if (eidx >= g_issueItems.size()) return FALSE;
    const auto& item = g_issueItems[eidx];
    HDC   hdc = GetDC(hWnd);
    // 未読行はラベルを太字で測る（描画側と同じフォントでないと行幅・取消線が食い違う）
    HFONT old = static_cast<HFONT>(SelectObject(hdc, item.unread ? g_hMenuFontBold : g_hMenuFont));
    SIZE  sz  = {};
    GetTextExtentPoint32W(hdc, item.label.c_str(),
        static_cast<int>(item.label.size()), &sz);
    // ピンマーカー列は全行で同幅を保つため、常に通常フォントで測る
    SelectObject(hdc, g_hMenuFont);
    // ピンマーカー列の幅（ピン有無で行幅が変わらないよう全行に確保する）
    // 実描画は D2D だが幅計測は GDI に一本化する。フォールバック先の Segoe UI Emoji と em
    // サイズが GDI・DirectWrite で同一のため送り幅は一致し、末尾スペース分が丸め差を吸収する。
    // ここを DWrite 計測に替えると、GDI フォールバック時に行幅と描画幅が食い違う。
    SIZE markSz = {};
    GetTextExtentPoint32W(hdc, PIN_MARK, static_cast<int>(wcslen(PIN_MARK)), &markSz);
    SelectObject(hdc, old);
    ReleaseDC(hWnd, hdc);
    // パディングは左 4 px + 右 16 px。（左はピンマーカー列が続くため控えめにする）
    mis->itemWidth  = static_cast<UINT>(sz.cx + markSz.cx) + 20;
    mis->itemHeight = static_cast<UINT>(sz.cy) + 6;
    return TRUE;
}

// 期限切れ日付の文字色（更新通知メニューの新バージョン表示と同じ赤に揃える）
static constexpr COLORREF OVERDUE_DATE_COLOR = RGB(220, 0, 0);

// 左クリックポップアップの owner-draw 項目描画
// ODS_SELECTED に応じた背景色・テキスト色を切り替え、closed フラグが立つ項目には
// DrawTextW 後に 2 px の取消線を手動で重ね描画する。
// 期限切れ（overdue）の行は日付部分だけ OVERDUE_DATE_COLOR で描く。
static BOOL drawIssueMenuItem(DRAWITEMSTRUCT* dis) {
    if (dis->CtlType != ODT_MENU) return FALSE;
    UINT eidx = static_cast<UINT>(dis->itemData);
    if (eidx >= g_issueItems.size()) return FALSE;
    const auto& item     = g_issueItems[eidx];
    bool        selected = (dis->itemState & ODS_SELECTED) != 0;

    FillRect(dis->hDC, &dis->rcItem,
        reinterpret_cast<HBRUSH>(
            static_cast<INT_PTR>(selected ? COLOR_HIGHLIGHT + 1 : COLOR_MENU + 1)));

    RECT textRect  = dis->rcItem;
    textRect.left += 4;  // 左パディング（measureIssueMenuItem の確保幅と揃える）
    SetBkMode(dis->hDC, TRANSPARENT);
    COLORREF textColor = GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
    SetTextColor(dis->hDC, textColor);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, g_hMenuFont));
    // ピンマーカー列（measureIssueMenuItem と同じ幅を全行に確保し、ピン留め行のみ 📌 を描く）
    SIZE markSz = {};
    GetTextExtentPoint32W(dis->hDC, PIN_MARK, static_cast<int>(wcslen(PIN_MARK)), &markSz);
    if (item.pinned) {
        // マーカー列だけ Direct2D で描いてカラー絵文字にする。列幅は GDI 計測値のまま使い、
        // 行幅・インデント・取消線の座標計算（measureIssueMenuItem と共有）を変えない。
        RECT markRect  = textRect;
        markRect.right = markRect.left + markSz.cx;
        COLORREF bgColor = GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_MENU);
        if (!drawPinMarkColor(dis->hDC, markRect, bgColor, textColor)) {
            // D2D が使えない環境では従来どおり GDI で単色の 📌 を描く
            DrawTextW(dis->hDC, PIN_MARK, -1, &textRect,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        }
    }
    textRect.left += markSz.cx;
    // 未読行はラベルだけ太字で描く（ピンマーカー列の幅は通常フォント基準を保つ。
    // 以降の幅計測（日付セグメント・取消線）も太字で行われ、描画幅と一致する）
    if (item.unread) SelectObject(dis->hDC, g_hMenuFontBold);
    // DT_NOPREFIX がないと件名中の & がニーモニック指定として食われ、次の文字に下線が付く
    // （幅は & を 1 文字として計測するため、描画幅とのずれで取消線も伸び過ぎる）
    constexpr UINT DT_ROW = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX;
    if (item.overdue && item.dateLen > 0 && !selected) {
        // 期限切れの日付部分だけ赤にするため「前・日付・後」の 3 セグメントに分けて描く。
        // 選択行は drawVersionMenuItem と同じく赤をやめて textColor 一色（単発描画）。
        // 送り幅は「ラベル先頭からの累積」で測る。セグメント単位に測って足すと丸め差が
        // 積み上がり、ラベル全体で計測している行幅・打ち消し線とずれる
        const wchar_t* base = item.label.c_str();
        size_t preLen  = item.dateOffset;
        size_t dateEnd = item.dateOffset + item.dateLen;
        SIZE szPre = {}, szThroughDate = {};
        GetTextExtentPoint32W(dis->hDC, base, static_cast<int>(preLen), &szPre);
        GetTextExtentPoint32W(dis->hDC, base, static_cast<int>(dateEnd), &szThroughDate);

        RECT seg = textRect;
        DrawTextW(dis->hDC, base, static_cast<int>(preLen), &seg, DT_ROW);

        seg = textRect;
        seg.left += szPre.cx;
        SetTextColor(dis->hDC, OVERDUE_DATE_COLOR);
        DrawTextW(dis->hDC, base + preLen, static_cast<int>(item.dateLen), &seg, DT_ROW);

        seg = textRect;
        seg.left += szThroughDate.cx;
        SetTextColor(dis->hDC, textColor);  // 件名は通常色へ戻す（打ち消し線も textColor を使う）
        DrawTextW(dis->hDC, base + dateEnd,
            static_cast<int>(item.label.size() - dateEnd), &seg, DT_ROW);
    }
    else {
        DrawTextW(dis->hDC, item.label.c_str(), -1, &textRect, DT_ROW);
    }
    if (item.closed) {
        SIZE sz = {};
        GetTextExtentPoint32W(dis->hDC, item.label.c_str(),
            static_cast<int>(item.label.size()), &sz);
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
            textRect.left + sz.cx + STRIKE_MARGIN_RIGHT,
            lineY - STRIKE_THICKNESS / 2 + STRIKE_THICKNESS
        };
        HBRUSH hLineBrush = CreateSolidBrush(textColor);
        FillRect(dis->hDC, &strikeRect, hLineBrush);
        DeleteObject(hLineBrush);
    }
    SelectObject(dis->hDC, oldFont);
    return TRUE;
}

// チケット項目のピン留めをトグルする（左クリックポップアップ上の右クリック）
// g_pins と item.pinned をトグルし、自スレッド所有のメニューウィンドウを再描画する
// （マーカーの描画自体は WM_DRAWITEM が pinned を参照して行う）。
// ピンが上限（PIN_LIMIT）に達している場合の追加は行わず、Toast で上限をユーザに知らせる。
static void togglePin(UINT itemIdx, HMENU hm) {
    UINT id = GetMenuItemID(hm, static_cast<int>(itemIdx));
    if (id < IDM_ISSUE_BASE || id >= IDM_ISSUE_MAX) return;
    UINT eidx = id - IDM_ISSUE_BASE;
    if (eidx >= g_issueItems.size()) return;

    auto& item = g_issueItems[eidx];
    bool nowPinned;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = std::find_if(g_pins.begin(), g_pins.end(),
            [&](const PinEntry& p) { return p.id == item.id; });
        if (it != g_pins.end()) {
            g_pins.erase(it);
            nowPinned = false;
        }
        else {
            if (g_pins.size() >= PIN_LIMIT) {
                writeLog("pin: limit reached (" + std::to_string(PIN_LIMIT)
                    + "), ignored #" + std::to_string(item.id));
                // 右クリックが無反応に見えないよう、上限到達をユーザに知らせる
                try {
                    showToast(L"ピン留めは最大 " + std::to_wstring(PIN_LIMIT) + L" 件です",
                              L"既存のピンを右クリックで解除してから追加してください", L"");
                }
                catch (...) {}
                return;
            }
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
                    p.updaterDisplay  = is.updaterDisplay;
                    break;
                }
            }
            g_pins.push_back(std::move(p));
            nowPinned = true;
        }
    }
    // 描画は WM_DRAWITEM が pinned を参照するため、フラグ更新と再描画だけでマーカーが切り替わる
    item.pinned = nowPinned;

    // 自スレッド所有のポップアップメニューウィンドウ（クラス名 "#32768"）を全て再描画する
    // FindWindowW はグローバル検索でタイミング依存・他プロセスの誤ヒットがあるため EnumThreadWindows を用いる
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t className[16] = {};
        if (GetClassNameW(hwnd, className, ARRAYSIZE(className))
            && wcscmp(className, L"#32768") == 0) {
            // erase は FALSE にして背景消去を抑止する。メニューの WM_PAINT が全項目
            // （owner-draw 行・セパレータ・フッタ）を描き直すため消去は不要。
            // TRUE だと全面消去→再描画の白フラッシュ（チラつき）が見える
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
        }
        return TRUE;
    }, 0);
    savePins(g_exeDir);
    writeLog(std::string("pin: ") + (nowPinned ? "added #" : "removed #") + std::to_string(item.id));
}

static LRESULT CALLBACK trayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
            showTrayContextMenu(hWnd);
        else if (lParam == WM_LBUTTONUP)
            handleTrayLeftClick(hWnd);
        return 0;
    }
    if (msg == WM_UPDATE_TOOLTIP) {
        updateTrayTooltip(hWnd);
        return 0;
    }
    if (msg == WM_COMMAND) {
        handleTrayCommand(LOWORD(wParam));
        return 0;
    }
    if (msg == WM_MEASUREITEM) {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis->CtlType == ODT_MENU && mis->itemID == IDM_OPEN_GITHUB)
            return measureVersionMenuItem(hWnd, mis) ? TRUE : DefWindowProcW(hWnd, msg, wParam, lParam);
        if (measureIssueMenuItem(hWnd, mis)) return TRUE;
    }
    if (msg == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlType == ODT_MENU && dis->itemID == IDM_OPEN_GITHUB)
            return drawVersionMenuItem(dis) ? TRUE : DefWindowProcW(hWnd, msg, wParam, lParam);
        if (drawIssueMenuItem(dis)) return TRUE;
    }
    // 左クリックポップアップ上の右クリック: ピン留めをトグルする
    // WM_MENURBUTTONUP は TPM_RIGHTBUTTON 指定なしでも右クリックで届く。（選択は発生しない）
    if (msg == WM_MENURBUTTONUP) {
        togglePin(static_cast<UINT>(wParam), reinterpret_cast<HMENU>(lParam));
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    // スリープ復帰・ロック解除: 即時ポーリングをトリガー
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
static HWND createTrayWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = trayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"redntfy_tray";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, L"redntfy_tray", nullptr, 0,
        0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
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
// 個別取得で最新化する（最大 PIN_LIMIT 回/ポーリング）。個別取得の失敗時（削除済み・
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
            && p.updaterDisplay != src->updaterDisplay;
        if (src && (p.subject != src->subject || p.updatedOn != src->updatedOn
                    || p.closed != src->closed || p.dueDate != src->dueDate
                    || p.projectName != src->projectName
                    || groupChanged || updaterChanged)) {
            p.subject     = src->subject;
            // project は journals と違って include 不要で常に含まれるため、updaterDisplay の
            // ような「非空のときだけ更新」ガードは要らない。（別プロジェクトへの移動を追随する）
            p.projectName = src->projectName;
            p.updatedOn   = src->updatedOn;
            p.closed      = src->closed;
            p.dueDate     = src->dueDate;
            if (groupIdsResolved) p.assignedToGroup = src->assignedToGroup;
            if (!src->updaterDisplay.empty()) p.updaterDisplay = src->updaterDisplay;
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
                if (gp.id == sp.id) {
                    gp.subject         = sp.subject;
                    gp.projectName     = sp.projectName;
                    gp.updatedOn       = sp.updatedOn;
                    gp.closed          = sp.closed;
                    gp.dueDate         = sp.dueDate;
                    gp.assignedToGroup = sp.assignedToGroup;
                    gp.updaterDisplay  = sp.updaterDisplay;
                    break;
                }
            }
        }
    }
    savePins(exeDir);
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

// ポーリング結果の処理（通知判定 → Toast → 状態保存）
//
// 前回状態（state.json）と突合して「新規流入」「updated_on の進行」「既知チケットの
// 新クエリ流入」を検知する。クエリ流入は「更新」として通知する。ただし前回追跡して
// いなかったクエリについては、そのクエリへの流入も、そのクエリだけに属するチケットの
// 新規流入も検知しない。（設定追加直後の通知の嵐を防ぐ）
// 自分が起票したチケットの流入は author.id で、自分の操作による更新と自分の更新が原因の
// 流入は最終 journal の更新者 id で除外する。（g_myUserId == 0 のときは除外せず通知側に倒す）
// 担当者フィルタ ON なら、自分が担当でないチケットもあわせて除外する。
// ベースライン未確立（初回起動・state.json 破損）の場合は通知せず状態保存のみ行う。
// prev は呼び出し側が loadState で読んだ前回状態。（resolveUpdaters のキャッシュと共有するため外で読む）
// issues の最終更新者は resolveUpdaters が確定済みであることを前提とする。
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
        for (int q : cfg.queryIds) {
            if (std::find(prev.knownQueries.begin(), prev.knownQueries.end(), q)
                    == prev.knownQueries.end())
                writeLog("state: query " + std::to_string(q)
                    + " newly tracked, adopting membership silently");
        }
    }

    // 通知対象の抽出
    std::vector<NotifyTarget> targets;
    for (const auto& is : issues) {
        // 終了要求時は state.json を書かずに抜ける。前回のまま残るため、未通知分は
        // 次回ポーリングで再検知される。（通知は失われない）
        if (g_shutdownRequested) return 0;
        // 担当者フィルタで外れたチケットは通知しない
        if (!passesAssigneeFilter(is)) continue;
        auto it = prev.issues.find(is.id);
        if (it == prev.issues.end()) {
            // 新規流入（自分の起票は通知しない）
            if (g_myUserId != 0 && is.authorId == g_myUserId) continue;
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
            if (recentUpdate && g_myUserId != 0 && is.updaterId == g_myUserId) continue;
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
        // 自分の操作による更新は通知しない。（最終更新者は resolveUpdaters が確定済み）
        // updated_on が進んでいない純粋な流入では判定しない：時間経過による流入が典型で
        // 「自分の操作」ではないため。（クエリ流入の Toast は更新者名も出さない）
        if (updated && g_myUserId != 0 && is.updaterId == g_myUserId) continue;
        targets.push_back({&is, updated ? NotifyKind::Updated : NotifyKind::QueryEntered,
                           updated ? is.updaterName : std::string()});
    }

    if (!targets.empty()) {
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
                // 複数件：合計件数のみのサマリ 1 通とし、クリックで代表クエリ画面（query_ids の先頭）を開く
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

    // 保存に失敗すると次回も同じ更新を再検知して通知が重複するため、Toast で知らせる
    if (!saveState(exeDir, cfg, issues))
        showErrorToast(L"状態保存エラー", L"state.json を書き込めません。通知が重複する可能性があります");
    if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);

    return static_cast<int>(targets.size());
}

// 「今すぐ更新」の完了 Toast
//
// 明示のユーザ操作に対し、再取得が終わった時点を知らせる。（完了が分からない問題への対処）
// 更新を検知した回は通知 Toast 自体が完了の合図になるため、呼び出し側で出し分ける。
// 通知音は鳴らさない。（チケットの更新通知と違い、ユーザが待っている場面での応答のため）
static void showPollDoneToast()
{
    // 件数は tooltip と同じ根拠にするため buildListRows から得る。（行そのものは使わない）
    int visible = 0;
    buildListRows(visible);
    try {
        showToast3(L"更新が完了しました", L"新しい更新はありません",
                   L"未処理 " + std::to_wstring(visible) + L" 件",
                   L"", true);
    }
    catch (winrt::hresult_error const& e) {
        writeLog("poll done toast failed: " + winrt::to_string(e.message()));
    }
    catch (...) {
        writeLog("poll done toast failed: unknown exception");
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
    winrt::init_apartment();

    bool startupPoll = true;  // 起動直後の 1 回だけ schedule・クールダウンに関わらずポーリングする

    // グループ担当判定用の id 集合（起動後に 1 回確定する。本スレッド専用でロック不要）
    std::vector<int> ownGroups;        // 自分の所属グループ（fetchMyUserId が設定）
    std::vector<int> groupIds;         // 判定に使う集合（全グループ、権限不足時は所属グループ）
    bool groupIdsResolved = false;

    // ユーザ id → 姓のセッション内キャッシュ（一覧の最終更新者列。resolveLastName が使う）
    std::unordered_map<int, std::string> userLastNames;

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
                // waitInterruptible は forcePoll で即復帰するため使わない（NIC 変化の連発で
                // 周回してしまい「休止中は何もしない」が破れる）。shutdown と手動更新のみ監視して待つ
                DWORD sleepMs = calcSleepUntilNextPoll(0);
                for (DWORD waited = 0; waited < sleepMs && !g_shutdownRequested
                        && !g_manualPoll.load(); waited += 100)
                    Sleep(100);
                continue;
            }

            // 自分の user id を確定する。（失敗時 0 = 自分の操作の除外判定なし）
            // 自動起動直後などネットワーク未接続で失敗した場合に備え、取得できるまで毎回試みる。
            // 休止時間帯の判定より後に置き、休止中はネットワークに一切触れない
            if (g_myUserId == 0) {
                g_myUserId = fetchMyUserId(cfg, ownGroups);
                if (g_myUserId != 0) writeLog("my user id: " + std::to_string(g_myUserId.load()));
            }

            // グループ担当マーカーの判定集合を確定する（user id 取得後に 1 回）
            // /groups.json は admin 権限が必要なため、403 なら自分の所属グループへ縮退する。
            // （他グループ宛の判定は漏れるが誤判定はしない）接続エラーは次回ポーリングで再試行。
            if (!groupIdsResolved && g_myUserId != 0) {
                DWORD status = 0;
                if (auto all = fetchAllGroupIds(cfg, &status)) {
                    groupIds = std::move(*all);
                    groupIdsResolved = true;
                    writeLog("group ids: " + std::to_string(groupIds.size()) + " (all groups)");
                }
                else if (status == 403) {
                    groupIds = ownGroups;
                    groupIdsResolved = true;
                    writeLog("group ids: " + std::to_string(groupIds.size()) + " (own groups fallback)");
                }
            }

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
                // クールダウンの残り時間は forcePoll を無視して待つ。waitInterruptible は
                // forcePoll で即復帰するため、連発トリガー時に busy loop になり使えない。
                // 手動更新はクールダウンの対象外なので監視して即座に抜ける
                ULONGLONG remain = FORCE_POLL_COOLDOWN_MS - (tickNow - lastTick);
                for (ULONGLONG waited = 0; waited < remain && !g_shutdownRequested
                        && !g_manualPoll.load(); waited += 100)
                    Sleep(100);
                continue;
            }
            if (manualTriggered) writeLog("manual poll triggered");
            if (forceTriggered && !startupPoll) writeLog("force poll triggered");
            if (stale && !startupPoll)
                writeLog("stale poll triggered (" + std::to_string((tickNow - lastTick) / 1000) + "s since last poll)");

            std::vector<Issue> issues;
            ULONGLONG t0    = GetTickCount64();
            bool ok = fetchIssues(cfg, issues);
            ULONGLONG elapsed = GetTickCount64() - t0;

            // 取得試行をもって「起動直後の 1 回」は消費とする（成功を待たない）。
            // オフラインのまま休止時間帯に入った場合に、朝まで 60 秒間隔のリトライを続けないため
            startupPoll = false;

            if (!ok) {
                if (g_shutdownRequested) break;  // 終了による取得中断は接続エラーではない
                writeLog("HTTP request failed");
                // 手動更新の失敗はクールダウンを無視して必ず知らせる。無音のままだと
                // 操作が届いたのか失敗したのか区別できず、完了通知の目的を果たせない
                showErrorToast(L"接続エラー", L"Redmine API に接続できません", manualTriggered);
                waitInterruptible(RETRY_WAIT_MS);
                continue;
            }

            writeLog("poll: " + std::to_string(issues.size()) + " issues ("
                + std::to_string(elapsed) + "ms), next: " + nextPollTimeStr(pollsPerHour));

            // 担当がグループかを付与する（一覧の 👥 マーカー用。表示専用の属性のため取得後に一括で付ける）
            for (auto& is : issues)
                is.assignedToGroup = isGroupAssignee(groupIds, is.assignedToId);

            // 一覧・tooltip は最終更新者の解決を待たずに先行公開する。初回起動や移行直後は
            // resolveUpdaters が全件分の HTTP を打つため、完了を待つと一覧が空のまま待たされる。
            // （姓の列は解決後、deliverPollResults の再公開で入る）
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_issues = issues;
            }

            // 前回状態はここで 1 回だけ読み、最終更新者の解決と通知判定で共有する
            PollState prevState = loadState(exeDir);
            resolveUpdaters(cfg, issues, prevState, userLastNames);
            if (g_shutdownRequested) break;  // resolveUpdaters は HTTP を伴うため中断を確認する

            int notified = deliverPollResults(exeDir, cfg, issues, prevState);
            refreshPins(exeDir, cfg, issues, groupIds, groupIdsResolved);

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

    // シャットダウン前に通知音スレッドの完了を待機（ダッキング復元を保証）
    // g_shutdownRequested == true なので playWavToWasapi はすみやかに停止するはず
    if (g_soundThread) {
        DWORD r = WaitForSingleObject(g_soundThread, 5000);
        if (r != WAIT_TIMEOUT) {
            CloseHandle(g_soundThread);
            g_soundThread = nullptr;
        }
        else {
            // タイムアウト時はハンドルを閉じない（走行中スレッドが COM/WASAPI を使用中のため）
            writeLog("pollThreadFunc: sound thread did not finish within 5s on shutdown");
        }
    }

    winrt::uninit_apartment();
}

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

    try {
        winrt::init_apartment();
        SetCurrentProcessExplicitAppUserModelID(APP_AUMID);
        ensureShortcut();
        WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
        g_hWnd = createTrayWindow();
        WTSRegisterSessionNotification(g_hWnd, NOTIFY_FOR_THIS_SESSION);

        // NIC 変化（Wi-Fi 接続/切断、LAN 抜き差し等）の監視を登録
        // FALSE: 登録時に既存インターフェースの初期通知は不要
        HANDLE hNetNotify = nullptr;
        if (NotifyIpInterfaceChange(AF_UNSPEC, onNetworkChange, nullptr, FALSE, &hNetNotify) != NO_ERROR) {
            writeLog("NotifyIpInterfaceChange failed: " + std::to_string(GetLastError()));
            hNetNotify = nullptr;
        }

        auto cfg = loadConfig(exeDir);

        // 必須キーの検証。（欠けていれば設定エラーとして起動を中止する）
        // url のスキームもここで検証し、以降の全経路（API・ブラウザ起動・Toast ボタン）で
        // http(s) 以外が ShellExecuteW に渡らないことを保証する
        if (cfg.redmineUrl.empty() || !isHttpUrl(cfg.redmineUrl)
            || cfg.apiKey.empty() || cfg.queryIds.empty()) {
            writeLog("config error: [redmine] url (must start with http:// or https://) / api_key / query_ids must be set in redntfy.local.toml");
            try {
                showToast(L"設定エラー",
                          L"redntfy.local.toml の [redmine] url / api_key / query_ids を設定してください",
                          L"", false);
            }
            catch (...) {}
            return 1;
        }

        g_currentConfig = cfg;  // スレッド起動前に 1 回だけ設定（以降は不変・ロック不要）

        // 通知音を読み込みノーマライズしてキャッシュに格納（以降の再生はキャッシュを使用）
        loadWavAndNormalize(exeDir, cfg);

        addTrayIcon(g_hWnd);

        // レジストリから設定を復元（キー未作成時はデフォルト値）
        g_soundEnabled  = readRegDword(REG_SOUND_ENABLED, 1u) != 0;
        g_muteInMeeting = readRegDword(REG_MUTE_IN_MEETING, 1u) != 0;
        g_assignedToMeOnly = readRegDword(REG_ASSIGNED_TO_ME, 0u) != 0;
        g_sortByDue        = readRegDword(REG_SORT_BY_DUE, 0u) != 0;

        writeLog("started");
        logSchedule(cfg.schedule);
        // どのクエリを追跡しているかは state.json の記録と通知挙動に直結するため起動時に残す
        {
            std::string s;
            for (int q : cfg.queryIds) s += (s.empty() ? "" : ",") + std::to_string(q);
            writeLog("query_ids: [" + s + "]");
        }

        // 更新チェックスレッド起動（起動時に 1 回のみ実行、detach で分離）
        if (cfg.updateCheckEnabled) {
            try {
                std::thread(checkForUpdates).detach();
            }
            catch (const std::system_error& e) {
                writeLog(std::string("failed to start update check thread: ") + e.what());
            }
        }

        // メニュー描画用フォントを初期化（以降、WM_MEASUREITEM / WM_DRAWITEM で使用する）
        initMenuFonts();

        // ピン留めを復元する（起動直後のポーリング前でも一覧にピンを出せるようにする）
        loadPins(exeDir);

        // ポーリングスレッド起動
        // メインスレッドはメッセージループに専念させるため、Redmine API ポーリング（HTTP I/O）を別スレッドへ分離する。
        // これによりネットワーク状態にかかわらずトレイアイコン右クリック等の UI が常時応答する。
        std::thread pollThread(pollThreadFunc, exeDir, cfg);

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

        // バックグラウンドスレッドを停止（waitInterruptible が 100 ms 単位でフラグを監視している）
        pollThread.join();

        // ループ終了後のクリーンアップ
        WTSUnRegisterSessionNotification(g_hWnd);
        removeTrayIcon(g_hWnd);
        DestroyWindow(g_hWnd);

        writeLog("shutdown");
    }
    catch (...) {
        writeLog("unexpected initialization error");
        return 2;
    }

    return 0;
}
