// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * redmntfy - Redmine の更新チケットを Windows Toast 通知で知らせる常駐デーモン
 *
 * exe 同フォルダの redmntfy.toml から動作設定を、.env から接続情報（REDMINE_URL / REDMINE_API_KEY）を読み込み、
 * schedule に従って自律的にポーリングする。自分が関係する（担当・作成・ウォッチャー）チケットのうち
 * 前回チェック以降に更新されたものを Toast 通知と音声で知らせる。
 * schedule は 0 時〜 23 時の 24 要素配列（回/時、0 でその時間帯は休止）。
 * 通知済みチケットは「チケット id → 最終通知 updated_on」を state.json に永続化して重複を防ぐ。
 * 自分自身の操作による更新は最新 journal の更新者 id を見て除外する。
 *
 * 終了コード:
 *   0  - 正常終了（トレイメニューの「終了」による）
 *   1  - 設定エラー（.env / TOML 読み込み失敗・必須キー未設定）
 *   2  - 予期しない初期化エラー
 *   3  - 多重起動（既に起動済み）
 *
 * 依存ライブラリ: WinHTTP, WinRT (Windows.UI.Notifications, Windows.Data.Json), Propsys
 * 外部依存: libebur128（vcpkg: libebur128:x64-windows-static）
 * ビルド: rc /nologo resource.rc
 *         cl /nologo /utf-8 /std:c++20 /EHsc /O2 /Feredmntfy.exe main.cpp resource.res
 *             /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib
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

#include <winsock2.h>
#include <ws2tcpip.h>
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
#include <bcrypt.h>

#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#include <netioapi.h>
#pragma comment(lib, "iphlpapi.lib")

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

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

#include "resource.h"
#include "version.h"  // ビルド時生成（APP_VERSION を定義）
#include "oauth.h"    // ビルド時生成（OAUTH_CLIENT_ID / OAUTH_CLIENT_SECRET を定義）

// アプリケーション識別子（Toast 通知に使用）
static const wchar_t* APP_AUMID = L"com.gcalntfy";

// 通知リード時間のデフォルト（分）と有効範囲
static constexpr int DEFAULT_NOTIFY_MINUTES = 5;
static constexpr int MIN_NOTIFY_MINUTES = 0;
static constexpr int MAX_NOTIFY_MINUTES = 30;

// エラー時のリトライ待機時間（ミリ秒）
static constexpr DWORD RETRY_WAIT_MS = 60u * 1000u;

// トレイアイコン用メッセージ ID
static constexpr UINT WM_TRAYICON        = WM_USER + 1;
static constexpr UINT WM_UPDATE_TOOLTIP  = WM_USER + 2;
static constexpr UINT WM_AUTH_REQUESTED  = WM_USER + 3;  // ユーザ操作による認証フロー起動要求

// コンテキストメニューコマンド ID
static constexpr UINT IDM_EXIT             = 40002;
static constexpr UINT IDM_MUTE_IN_MEETING  = 40004;
static constexpr UINT IDM_SOUND_ENABLED       = 40005;
static constexpr UINT IDM_OPEN_CONFIG         = 40006;
static constexpr UINT IDM_OPEN_LOG            = 40007;
static constexpr UINT IDM_OPEN_GITHUB         = 40008; // GitHub リポジトリページを開く
static constexpr UINT IDM_OPEN_CALENDAR_TODAY = 40009; // Google Calendar 当日ページを開く
static constexpr UINT IDM_STARTUP             = 40010; // Windows スタートアップ登録トグル

static constexpr wchar_t GITHUB_URL[]                 = L"https://github.com/aviscaerulea/gcalntfy";
static constexpr wchar_t GITHUB_RELEASES_URL[]        = L"https://github.com/aviscaerulea/gcalntfy/releases";
static constexpr wchar_t GITHUB_API_RELEASES_LATEST[] = L"https://api.github.com/repos/aviscaerulea/gcalntfy/releases/latest";
static constexpr wchar_t CALENDAR_TODAY_URL[]         = L"https://calendar.google.com/calendar/r/week";

// イベントキャッシュファイル名（exe 同フォルダに保存）
static constexpr wchar_t CACHE_FILENAME[]        = L"events.json";
// 通知抑制リストキャッシュファイル名（exe 同フォルダに保存）
static constexpr wchar_t MUTED_CACHE_FILENAME[]  = L"muted_events.json";

// 左クリック予定一覧のイベント項目（IDM_EVENT_BASE + index で最大50件）
static constexpr UINT IDM_EVENT_BASE = 41000;
static constexpr UINT IDM_EVENT_MAX  = 41050;

// ツールチップ定期更新タイマー（1分間隔）
static constexpr UINT  IDT_TOOLTIP_REFRESH  = 1;
static constexpr DWORD TOOLTIP_REFRESH_MS   = 60000;

// 即時ポーリングの抑制間隔（前回ポーリングからこの時間内は即時ポーリングをスキップ）
static constexpr DWORD FORCE_POLL_COOLDOWN_MS = 60'000;

// 通知音のデフォルトファイル名（exe 同フォルダに配置）
static constexpr wchar_t DEFAULT_SOUND_FILE[] = L"sound.wav";

// 通知音 WAV ファイルの最大サイズ（バイト）。これを超えると不正ファイル扱いで読み込みを拒否する。
static constexpr DWORD MAX_WAV_FILE_BYTES = 16u * 1024 * 1024;

// 円周率（MSVC では M_PI に _USE_MATH_DEFINES が必要なため自前定義）
static constexpr double PI = 3.14159265358979323846;

// OAuth 認証コード待機タイムアウト（秒）
static constexpr int AUTH_CODE_TIMEOUT_SEC = 120;

// エラー Toast の最小間隔（30 分）
static constexpr ULONGLONG ERROR_TOAST_COOLDOWN_MS = 30uLL * 60 * 1000;

// 認証必要 Toast の最小間隔（30 分）
static constexpr ULONGLONG AUTH_TOAST_COOLDOWN_MS = 30uLL * 60 * 1000;

// 前回ポーリングからこの時間が経過したら即時ポーリング（1 時間）
static constexpr ULONGLONG STALE_POLL_THRESHOLD_MS = 3'600'000ULL;

// 予定なし時の表示文言（ツールチップ・左クリック一覧で共用）
static constexpr wchar_t NO_UPCOMING_EVENTS[] = L"本日の以降予定：なし";

// Google OAuth 2.0
static constexpr const wchar_t* OAUTH_AUTH_URL   = L"https://accounts.google.com/o/oauth2/v2/auth";
static constexpr const wchar_t* OAUTH_TOKEN_HOST = L"oauth2.googleapis.com";
static constexpr const wchar_t* OAUTH_TOKEN_PATH = L"/token";
static constexpr const wchar_t* OAUTH_SCOPE      = L"https://www.googleapis.com/auth/calendar.readonly";

// Google Calendar API v3
static constexpr const wchar_t* CALENDAR_API_HOST = L"www.googleapis.com";

// PKCE code_verifier のバイト数（Base64url で 86 文字）
static constexpr size_t PKCE_VERIFIER_BYTES = 64;

// OAuth state パラメータの乱数バイト数（CSRF 耐性のため十分なエントロピー）
static constexpr size_t OAUTH_STATE_BYTES = 32;

// レジストリ値名（refresh token）
static constexpr const wchar_t* REG_REFRESH_TOKEN = L"RefreshToken";

// シャットダウンフラグ（メインスレッド・WndProc・通知スレッドから参照）
static std::atomic<bool> g_shutdownRequested{false};

// 音声通知の有効/無効フラグ（レジストリで永続化、トレイメニューの親項目）
static std::atomic<bool> g_soundEnabled{true};

// マイク/カメラ使用中の音声自動ミュートフラグ（レジストリで永続化）
static std::atomic<bool> g_muteInMeeting{true};

// トレイウィンドウのハンドル（メインスレッドで作成し、ポーリングループと通知スレッドが参照）
static HWND g_hWnd = nullptr;

// トレイのポップアップメニュー表示中フラグ（ツールチップ更新抑制用）
static std::atomic<bool> g_popupShowing{false};

// スリープ復帰・ロック解除時の即時ポーリングフラグ
static std::atomic<bool> g_forcePoll{false};

// 前回ポーリング実行時刻（GetTickCount64、連続ポーリング抑制・stale 判定用）
static std::atomic<ULONGLONG> g_lastPollTick{0};

// 前回エラー Toast 表示時刻（GetTickCount64、スパム防止用。ポーリング成功時に 0 リセット）
static std::atomic<ULONGLONG> g_lastErrorToastTime{0};

// TaskbarCreated メッセージ ID（エクスプローラ再起動対策）
static UINT WM_TASKBAR_CREATED = 0;

// OAuth アクセストークンと有効期限（FILETIME 単位、100 ナノ秒）
// pollThread と切り離し認証スレッド（startInteractiveAuth）から並行アクセスされるため
// g_tokenMtx で保護する。読み書きは必ずロックを取得して行うこと。
static std::mutex     g_tokenMtx;
static std::wstring   g_accessToken;
static ULARGE_INTEGER g_tokenExpiry = {};

// 認証フロー状態フラグ
//
// g_authRequired:    refresh_token が無効・未設定で、ユーザ操作によるフル OAuth が必要な状態
// g_authInProgress:  startInteractiveAuth 実行中（二重起動防止用）
// g_lastAuthToastTime: 認証 Toast の最終表示時刻（クールダウン制御用、GetTickCount64）
static std::atomic<bool>      g_authRequired{false};
static std::atomic<bool>      g_authInProgress{false};
static std::atomic<ULONGLONG> g_lastAuthToastTime{0};

// 前方宣言（OAuth フロー内で Toast 通知・レジストリ操作を使用するため）
static void showToast(const std::wstring& timeJST, const std::wstring& title,
                      const std::wstring& permalink, bool silent = true);
static std::wstring readRegString(const wchar_t* valueName);
static void writeRegString(const wchar_t* valueName, const std::wstring& value);
static void notifyAuthRequired();

// ==================== データ構造 ====================

struct CalendarEvent {
    std::string      id;              // Google Calendar イベント id（通知重複防止キー）
    std::string      datetime;
    std::string      content;
    std::string      permalink;
    std::vector<int> reminderMinutes; // イベント個別の追加通知分数（popup のみ。空=追加通知なし）
};

// parseCalendarEvents の戻り値
struct ParseResult {
    std::vector<CalendarEvent> events;
    std::string errorMsg;
};

// loadConfig の戻り値
struct Config {
    std::vector<int>          schedule;          // 24 要素（0 時〜 23 時の 1 時間あたりポーリング回数、最低 1）
    std::vector<std::wstring> duckTargets;        // 通知音再生中にミュートするプロセス名
    long long                 notifyLeadMs;       // 通知リード時間（ミリ秒、TOML では分で指定）
    std::vector<std::string>  extCalendarIds;     // 追加でポーリングするカレンダー ID（primary は常に有効）

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

// メインスレッド→通知スレッド: 予定リスト・設定の受け渡し（g_mtx で保護）
static std::mutex              g_mtx;
static std::condition_variable g_cv;
static std::vector<CalendarEvent> g_pendingEvents;
static Config                  g_currentConfig;
static bool                    g_eventsUpdated = false;
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
// アクセスは notifyThreadFunc 1 スレッドに限定する。launchSound（呼び出し元は notifyThreadFunc）と、
// notifyThreadFunc 末尾のシャットダウン処理がすべての書き換え箇所であり、
// 並行アクセスがないためミューテックス保護は不要。新たな呼び出し箇所を追加する場合は
// 必ず notifyThreadFunc コンテキスト内であることを確認すること。
static HANDLE g_soundThread = nullptr;

// exe ディレクトリパス（wmain 起動時に確定し、WndProc スレッドからも参照する）
static std::wstring g_exeDir;

// 通知抑制リスト：eventKey → JST 日付（YYYY-MM-DD）（g_mtx で保護）
static std::unordered_map<std::string, std::string> g_mutedEvents;

// 左クリックポップアップの予定項目描画用フォント（initMenuFonts で初期化）
static HFONT g_hMenuFont = nullptr;

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

// JST SYSTEMTIME を UTC SYSTEMTIME に変換する
static SYSTEMTIME jstToUtc(SYSTEMTIME st) { return shiftSystemTime(st, -JST_OFFSET_HNS); }

// 現在日時を JST "YYYY-MM-DD HH:MM" 形式で取得する
static std::wstring getCurrentDateTimeJST() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    st = utcToJst(st);
    wchar_t buf[32];
    swprintf_s(buf, _countof(buf), L"%04d-%02d-%02d %02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// 現在 UTC 時刻を ISO 8601 形式 "YYYY-MM-DDTHH:MM:SS.000Z" で取得する
static std::string getCurrentUtcISO() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    sprintf_s(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ISO 8601 UTC 文字列 "YYYY-MM-DDTHH:MM:SS...Z" を SYSTEMTIME にパースする
// パース失敗時は false を返す
static bool parseIsoToSystemTime(const std::string& iso, SYSTEMTIME& out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6) {
        return false;
    }
    out = {};
    out.wYear   = static_cast<WORD>(y);
    out.wMonth  = static_cast<WORD>(mo);
    out.wDay    = static_cast<WORD>(d);
    out.wHour   = static_cast<WORD>(h);
    out.wMinute = static_cast<WORD>(mi);
    out.wSecond = static_cast<WORD>(s);
    return true;
}

// UTC ISO 文字列を JST の SYSTEMTIME に変換する
// パース失敗時は nullopt を返す。
static std::optional<SYSTEMTIME> utcIsoToJstSt(const std::string& utcIso) {
    SYSTEMTIME st;
    if (!parseIsoToSystemTime(utcIso, st)) return std::nullopt;
    return utcToJst(st);
}

// UTC RFC3339 "YYYY-MM-DDTHH:MM:SS...Z" を JST "HH:MM" に変換する
static std::wstring utcToJstHHMM(const std::string& utcIso) {
    auto jst = utcIsoToJstSt(utcIso);
    if (!jst) return L"??:??";
    wchar_t buf[8];
    swprintf_s(buf, _countof(buf), L"%02d:%02d", jst->wHour, jst->wMinute);
    return buf;
}

// UTC ISO 文字列を JST の "M/D HH:MM" 形式に変換する
static std::wstring utcToJstMDHHMM(const std::string& utcIso) {
    auto jst = utcIsoToJstSt(utcIso);
    if (!jst) return L"?/? ??:??";
    wchar_t buf[16];
    swprintf_s(buf, _countof(buf), L"%d/%d %02d:%02d",
               jst->wMonth, jst->wDay, jst->wHour, jst->wMinute);
    return buf;
}

// SYSTEMTIME を ISO 8601 文字列 "YYYY-MM-DDTHH:MM:SS" に変換する
static std::string systemTimeToIso(const SYSTEMTIME& st) {
    char buf[24];
    sprintf_s(buf, "%04d-%02d-%02dT%02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// UTC ISO 8601 文字列を JST ISO 8601 文字列に変換する
// 入力: "2026-03-07T10:00:00.000Z" → 出力: "2026-03-07T19:00:00"
static std::string utcIsoToJst(const std::string& utcIso) {
    SYSTEMTIME st = {};
    if (!parseIsoToSystemTime(utcIso, st)) return utcIso;
    return systemTimeToIso(utcToJst(st));
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
    s += "] (" + std::to_string(total) + " polls/day)";
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
// authHeader: 空でなければ Authorization: Bearer ヘッダとして付与
// outStatusCode が非 null の場合、最終 HTTP ステータスコードを書き込む（失敗時は 0）
static std::string httpRequest(const wchar_t* method, const std::wstring& url,
    const std::string& body, const wchar_t* contentType,
    const std::wstring& authHeader, DWORD* outStatusCode = nullptr)
{
    if (outStatusCode) *outStatusCode = 0;
    HINTERNET hSession = WinHttpOpen(L"gcalntfy/1.0",
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
    if (!authHeader.empty())           headers += L"Authorization: Bearer " + authHeader + L"\r\n";

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

// Calendar API 用 GET（Bearer トークン付き）
static std::string httpGet(const std::wstring& url, const std::wstring& accessToken,
    DWORD* outStatusCode = nullptr)
{
    return httpRequest(L"GET", url, "", nullptr, accessToken, outStatusCode);
}

// トークンエンドポイント用 POST（application/x-www-form-urlencoded）
static std::string httpPostForm(const std::wstring& url, const std::string& formBody,
    DWORD* outStatusCode = nullptr)
{
    return httpRequest(L"POST", url, formBody, L"application/x-www-form-urlencoded", {}, outStatusCode);
}

// ==================== PKCE・エンコードユーティリティ ====================

// バイト列を Base64url エンコード（パディングなし、RFC 7636 準拠）
static std::string base64urlEncode(const unsigned char* data, size_t len) {
    static constexpr char TABLE[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int b = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < len) b |= data[i + 2];
        out += TABLE[(b >> 18) & 0x3F];
        out += TABLE[(b >> 12) & 0x3F];
        if (i + 1 < len) out += TABLE[(b >> 6) & 0x3F];
        if (i + 2 < len) out += TABLE[b & 0x3F];
    }
    return out;
}

// PKCE code_verifier 生成（BCryptGenRandom + Base64url）
// BCryptGenRandom 失敗時は空文字列を返す（安全でない rand() へのフォールバックは行わない）
static std::string generateCodeVerifier() {
    unsigned char buf[PKCE_VERIFIER_BYTES];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        writeLog("BCryptGenRandom failed");
        return {};
    }
    return base64urlEncode(buf, sizeof(buf));
}

// OAuth state パラメータ生成（BCryptGenRandom + Base64url）
// 認可リクエストとリダイレクト応答の対応関係を検証して CSRF 攻撃を防ぐ。
// 失敗時は空文字列を返し、呼び出し元で認証フローを中止する。
static std::string generateOAuthState() {
    unsigned char buf[OAUTH_STATE_BYTES];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        writeLog("BCryptGenRandom failed (state)");
        return {};
    }
    return base64urlEncode(buf, sizeof(buf));
}

// PKCE code_challenge 生成（SHA-256 + Base64url）
// 各 BCrypt API の失敗時はリソースを解放して空文字列を返す
static std::string generateCodeChallenge(const std::string& verifier) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return {};
    BCRYPT_HASH_HANDLE hHash = nullptr;
    unsigned char hash[32] = {};
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))
        && BCRYPT_SUCCESS(BCryptHashData(hHash,
               reinterpret_cast<PUCHAR>(const_cast<char*>(verifier.c_str())),
               static_cast<ULONG>(verifier.size()), 0))
        && BCRYPT_SUCCESS(BCryptFinishHash(hHash, hash, sizeof(hash), 0));
    if (hHash) BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!ok) {
        writeLog("BCrypt SHA-256 failed");
        return {};
    }
    return base64urlEncode(hash, sizeof(hash));
}

// URL クエリ値のパーセントエンコード
static std::string urlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        }
        else {
            char buf[4];
            sprintf_s(buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ==================== OAuth 2.0 フロー ====================

// ループバック HTTP サーバをランダムポートで起動する
// 戻り値: 実際のポート番号（失敗時 0）
static int startLoopbackServer(SOCKET& serverSocket) {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) { WSACleanup(); return 0; }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // OS にポートを割り当てさせる
    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || listen(serverSocket, 1) != 0) {
        closesocket(serverSocket);
        WSACleanup();
        serverSocket = INVALID_SOCKET;
        return 0;
    }

    int addrLen = sizeof(addr);
    if (getsockname(serverSocket, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        closesocket(serverSocket);
        WSACleanup();
        serverSocket = INVALID_SOCKET;
        return 0;
    }
    return ntohs(addr.sin_port);
}

// OAuth 認証 URL を構築してブラウザを開く
static void openBrowserForAuth(int redirectPort, const std::string& codeVerifier,
    const std::string& state)
{
    auto challenge = generateCodeChallenge(codeVerifier);
    std::string redirectUri = "http://127.0.0.1:" + std::to_string(redirectPort);
    std::string url = wideToUtf8(OAUTH_AUTH_URL);
    url += "?client_id="             + urlEncode(wideToUtf8(OAUTH_CLIENT_ID));
    url += "&redirect_uri="          + urlEncode(redirectUri);
    url += "&response_type=code";
    url += "&scope="                 + urlEncode(wideToUtf8(OAUTH_SCOPE));
    url += "&code_challenge="        + urlEncode(challenge);
    url += "&code_challenge_method=S256";
    url += "&access_type=offline";
    url += "&prompt=consent";
    url += "&state="                 + urlEncode(state);
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// クエリ文字列から指定キーの値を抽出する
// req は HTTP リクエストの Request-Line（"GET /path?... HTTP/1.1"）を想定する。
// ヘッダ部（Referer 等）に細工された code=/state= を誤マッチしないよう、
// 呼び出し側で最初の "\r\n" 以前に限定して渡すこと。
// key は "code" や "state"（"=" は不要）。
// "?" または "&" の直後に出現する key=value のみを対象とし、
// "scope=" が "code=" にマッチするような部分一致を回避する。
// 見つからない場合は空文字列。値の URL デコードまでは行わない（ASCII 範囲のみ想定）。
static std::string extractQueryValue(const std::string& req, const std::string& key) {
    std::string pattern = key + "=";
    size_t pos = 0;
    while ((pos = req.find(pattern, pos)) != std::string::npos) {
        // 直前の文字が "?" または "&" の場合のみ正当なキーと判定する
        if (pos > 0 && (req[pos - 1] == '?' || req[pos - 1] == '&')) break;
        pos += pattern.size();
    }
    if (pos == std::string::npos) return {};
    pos += pattern.size();
    size_t end = req.find_first_of("& \r\n", pos);
    if (end == std::string::npos) end = req.size();
    return req.substr(pos, end - pos);
}

// ループバックサーバで認証コードを待ち受ける（120秒タイムアウト）
// select() で accept タイムアウトを制御し、client ソケットで recv タイムアウトを設定する。
// \r\n\r\n 受信まで recv をループし、auth_code を抽出して返す（失敗時は空文字列）。
// expectedState が非空の場合、受信した state と一致しなければ空文字列を返す（CSRF 対策）。
// シャットダウン要求時は 1 秒以内にループから抜けて空文字列を返す（プロセス停止を阻害しないため）。
static std::string waitForAuthCode(SOCKET serverSocket, const std::string& expectedState) {
    // accept のタイムアウトは select() で実現する（SO_RCVTIMEO は accept に効かない）。
    // 1 秒ごとに g_shutdownRequested を確認し、必要なら早期に脱出する。
    // タイムアウトは「ちょうど AUTH_CODE_TIMEOUT_SEC 経過後」の判定で打ち切る
    // （厳密には 1 ループ分早く打ち切られる可能性があるが許容差）。
    int waited = 0;
    while (true) {
        if (waited >= AUTH_CODE_TIMEOUT_SEC) return {};
        if (g_shutdownRequested.load()) return {};
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverSocket, &readSet);
        timeval tv = { 1, 0 };
        int ready = select(0, &readSet, nullptr, nullptr, &tv);
        if (ready < 0) return {};
        if (ready > 0) break;
        ++waited;
    }

    SOCKET client = accept(serverSocket, nullptr, nullptr);
    if (client == INVALID_SOCKET) return {};

    // recv タイムアウトは client ソケットに設定する
    DWORD recvTimeout = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&recvTimeout), sizeof(recvTimeout));

    // 受信エラー応答（UTF-8 で「認証中にエラーが発生しました。再度お試しください。」を埋め込む）
    // ブラウザのタブが永久にローディング状態になるのを防ぐため、不完全リクエスト検知時もレスポンスを返す。
    static const char* RESPONSE_RECV_ERROR =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><p>\xE8\xAA\x8D\xE8\xA8\xBC\xE4\xB8\xAD\xE3\x81\xAB"
        "\xE3\x82\xA8\xE3\x83\xA9\xE3\x83\xBC\xE3\x81\x8C\xE7\x99\xBA\xE7\x94\x9F"
        "\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F\xE3\x80\x82"
        "\xE5\x86\x8D\xE5\xBA\xA6\xE3\x81\x8A\xE8\xA9\xA6\xE3\x81\x97\xE3\x81\x8F"
        "\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80\x82</p></body></html>";

    std::string req;
    // HTTP リクエスト読み出しループ
    // recv の戻り値: 正値=受信バイト数、0=ピア close、SOCKET_ERROR(-1)=エラー
    // ループ離脱後、"\r\n\r\n" が未受信なら不完全リクエスト検知で弾かれる（後段を参照）。
    char chunk[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
        int n = recv(client, chunk, sizeof(chunk), 0);
        if (n == 0) {
            writeLog("waitForAuthCode: peer closed before request complete");
            break;
        }
        if (n == SOCKET_ERROR) {
            writeLog("waitForAuthCode: recv failed, WSA error " + std::to_string(WSAGetLastError()));
            send(client, RESPONSE_RECV_ERROR, static_cast<int>(strlen(RESPONSE_RECV_ERROR)), 0);
            closesocket(client);
            return {};
        }
        req.append(chunk, static_cast<size_t>(n));
    }

    // 不完全リクエスト検知（peer close または 65536 バイト上限到達でヘッダ終端 "\r\n\r\n" 未受信）
    // n==0 と recv 上限超過の両方のケースを一律で弾き、後段の解析を停止する。
    if (req.find("\r\n\r\n") == std::string::npos) {
        writeLog("waitForAuthCode: incomplete request, abort");
        send(client, RESPONSE_RECV_ERROR, static_cast<int>(strlen(RESPONSE_RECV_ERROR)), 0);
        closesocket(client);
        return {};
    }

    // 認証完了応答（UTF-8 バイト列で「認証完了。このタブは閉じてください。」を埋め込む）
    // 完成後の HTML をブラウザに返してタブのクローズを促す。
    static const char* RESPONSE_OK =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><p>\xE8\xAA\x8D\xE8\xA8\xBC\xE5\xAE\x8C\xE4\xBA\x86\xE3\x80\x82"
        "\xE3\x81\x93\xE3\x81\xAE\xE3\x82\xBF\xE3\x83\x96\xE3\x81\xAF\xE9\x96\x89\xE3"
        "\x81\x98\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80"
        "\x82</p></body></html>";

    // state ミスマッチ応答（UTF-8 で「認証情報が一致しません。再度お試しください。」を埋め込む）
    // ユーザに認証完了と誤認させないため、state 検証失敗時はこちらを返す。
    static const char* RESPONSE_STATE_MISMATCH =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><p>\xE8\xAA\x8D\xE8\xA8\xBC\xE6\x83\x85\xE5\xA0\xB1\xE3\x81\x8C"
        "\xE4\xB8\x80\xE8\x87\xB4\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93\xE3"
        "\x80\x82\xE5\x86\x8D\xE5\xBA\xA6\xE3\x81\x8A\xE8\xA9\xA6\xE3\x81\x97\xE3\x81"
        "\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80\x82</p></body></html>";

    // クエリ抽出範囲の Request-Line 限定
    // ヘッダ部の Referer 等に細工された code=/state= の誤マッチを防ぐため、
    // 最初の "\r\n" 以前のみを抽出対象とする（"\r\n\r\n" を含む前提のため必ず見つかる）。
    std::string requestLine = req.substr(0, req.find("\r\n"));

    // state 検証を最初に行う（CSRF 対策）。失敗時はエラー応答を返してから終了する。
    if (!expectedState.empty()) {
        auto receivedState = extractQueryValue(requestLine, "state");
        if (receivedState != expectedState) {
            writeLog("OAuth state mismatch: ignoring callback");
            send(client, RESPONSE_STATE_MISMATCH, static_cast<int>(strlen(RESPONSE_STATE_MISMATCH)), 0);
            closesocket(client);
            return {};
        }
    }

    send(client, RESPONSE_OK, static_cast<int>(strlen(RESPONSE_OK)), 0);
    closesocket(client);

    return extractQueryValue(requestLine, "code");
}

// トークンレスポンス JSON からアクセストークンと有効期限を更新する
// access_token が含まれない場合は false を返す
// g_tokenMtx を取得してから書き込む（並行スレッドからの読み出しと競合しないため）
static bool applyTokenResponse(const winrt::Windows::Data::Json::JsonObject& obj) {
    if (!obj.HasKey(L"access_token")) return false;
    std::wstring token = obj.GetNamedString(L"access_token", L"").c_str();

    double expiresIn = obj.GetNamedNumber(L"expires_in", 3600);
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER expiry;
    expiry.LowPart  = ft.dwLowDateTime;
    expiry.HighPart = ft.dwHighDateTime;
    expiry.QuadPart += static_cast<ULONGLONG>(expiresIn * 10'000'000.0);

    std::lock_guard<std::mutex> lk(g_tokenMtx);
    g_accessToken = token;
    g_tokenExpiry = expiry;
    return true;
}

// 認証コードをアクセストークン・リフレッシュトークンに交換する
// 成功時: g_accessToken / g_tokenExpiry を更新し、refresh_token をレジストリに保存
static bool exchangeCodeForTokens(const std::string& authCode,
    int redirectPort, const std::string& codeVerifier)
{
    std::string redirectUri = "http://127.0.0.1:" + std::to_string(redirectPort);
    std::string body =
        "grant_type=authorization_code"
        "&code="          + urlEncode(authCode) +
        "&client_id="     + urlEncode(wideToUtf8(OAUTH_CLIENT_ID)) +
        "&client_secret=" + urlEncode(wideToUtf8(OAUTH_CLIENT_SECRET)) +
        "&redirect_uri="  + urlEncode(redirectUri) +
        "&code_verifier=" + urlEncode(codeVerifier);

    DWORD httpStatus = 0;
    std::wstring url = std::wstring(L"https://") + OAUTH_TOKEN_HOST + OAUTH_TOKEN_PATH;
    auto resp = httpPostForm(url, body, &httpStatus);
    if (resp.empty() || httpStatus != 200) {
        writeLog("token exchange failed: status " + std::to_string(httpStatus) + " body=" + resp);
        return false;
    }

    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(resp));
        if (!applyTokenResponse(obj)) {
            writeLog("token exchange: no access_token in response");
            return false;
        }

        if (obj.HasKey(L"refresh_token")) {
            std::wstring rt = obj.GetNamedString(L"refresh_token", L"").c_str();
            writeRegString(REG_REFRESH_TOKEN, rt);
            writeLog("refresh_token saved to registry");
        }
        else {
            writeLog("warning: no refresh_token in response");
        }
        writeLog("token exchange succeeded");
        return true;
    }
    catch (...) {
        writeLog("token exchange: JSON parse error");
        return false;
    }
}

// リフレッシュ結果
//
// Ok:           アクセストークン取得成功（有効期限内 or refresh 成功）
// NetworkError: ネットワーク不通・タイムアウト・5xx 等の一時エラー（認証 Toast を出さない）
// AuthRequired: refresh_token なし or 4xx（invalid_grant 等）。フル OAuth が必要
enum class RefreshResult { Ok, NetworkError, AuthRequired };

// リフレッシュトークンでアクセストークンを更新する
//
// 戻り値: Ok / NetworkError / AuthRequired（呼び出し側で使い分ける）
static RefreshResult refreshAccessToken(const std::wstring& refreshToken) {
    std::string body =
        "grant_type=refresh_token"
        "&refresh_token=" + urlEncode(wideToUtf8(refreshToken)) +
        "&client_id="     + urlEncode(wideToUtf8(OAUTH_CLIENT_ID)) +
        "&client_secret=" + urlEncode(wideToUtf8(OAUTH_CLIENT_SECRET));

    DWORD httpStatus = 0;
    std::wstring url = std::wstring(L"https://") + OAUTH_TOKEN_HOST + OAUTH_TOKEN_PATH;
    auto resp = httpPostForm(url, body, &httpStatus);

    // status==0（接続失敗・タイムアウト・DNS 解決失敗）または 5xx は一時エラー扱い
    if (httpStatus == 0 || (httpStatus >= 500 && httpStatus < 600)) {
        writeLog("refresh token network error: status " + std::to_string(httpStatus));
        return RefreshResult::NetworkError;
    }

    if (httpStatus != 200) {
        // 4xx（invalid_grant など）はフル認証が必要
        writeLog("refresh token rejected: status " + std::to_string(httpStatus));
        return RefreshResult::AuthRequired;
    }

    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(resp));
        if (!applyTokenResponse(obj)) return RefreshResult::AuthRequired;

        writeLog("access token refreshed");
        return RefreshResult::Ok;
    }
    catch (...) {
        writeLog("refresh token: JSON parse error");
        return RefreshResult::AuthRequired;
    }
}

// アクセストークン確保（非対話）
//
// Toast もブラウザも起動しない。ポーリングループから呼び出される。
// 1. 有効期限内（5 分マージン）なら即 Ok
// 2. レジストリの refresh_token でリフレッシュを試みる
// 3. refresh_token がなければ AuthRequired
static RefreshResult tryRefreshAccessToken() {
    // 有効期限確認（5 分のマージンを持たせる）
    {
        std::lock_guard<std::mutex> lk(g_tokenMtx);
        if (!g_accessToken.empty()) {
            FILETIME ft = {};
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER now;
            now.LowPart  = ft.dwLowDateTime;
            now.HighPart = ft.dwHighDateTime;
            if (now.QuadPart + 5uLL * 60 * 10'000'000 < g_tokenExpiry.QuadPart)
                return RefreshResult::Ok;
        }
    }

    auto refreshToken = readRegString(REG_REFRESH_TOKEN);
    if (refreshToken.empty()) return RefreshResult::AuthRequired;

    return refreshAccessToken(refreshToken);
}

// 対話的 OAuth フロー
//
// ユーザアクション（Toast クリック・未認証時のトレイ左クリック）からのみ起動される。
// ループバックサーバを起動し、ブラウザで Google 認証画面を開いて authorization code を待ち受ける。
// 二重起動は g_authInProgress で防止する。別スレッドで実行される想定。
// 成功時: g_authRequired をクリアし、g_forcePoll をセットして即時ポーリングを誘発する
static void startInteractiveAuth() {
    // 二重起動防止（CAS）
    bool expected = false;
    if (!g_authInProgress.compare_exchange_strong(expected, true)) {
        writeLog("interactive auth already in progress, skip");
        return;
    }

    // 切り離しスレッドで動作するため、ここで COM/WinRT アパートメントを初期化する。
    // ShellExecuteA（ブラウザ起動）と applyTokenResponse 経由の WinRT JSON 解析が COM に依存するため、
    // 失敗時は対話認証を断念して g_authInProgress を解放する（不整合状態で続行しない）。
    bool comInitialized = false;
    try {
        winrt::init_apartment();
        comInitialized = true;
    }
    catch (...) {
        writeLog("interactive auth: winrt::init_apartment failed, abort");
        g_authInProgress.store(false);
        return;
    }

    bool succeeded = false;
    try {
        writeLog("starting OAuth authorization flow (user-initiated)");

        SOCKET serverSocket = INVALID_SOCKET;
        int port = startLoopbackServer(serverSocket);
        if (port == 0) {
            // startLoopbackServer 内で WSACleanup 済み（呼び出し側での後始末は不要）
            writeLog("failed to start loopback server");
        }
        else {
            // ループバックサーバ起動成功時はここで closesocket + WSACleanup を一括処理する
            auto codeVerifier = generateCodeVerifier();
            auto stateValue   = generateOAuthState();
            if (codeVerifier.empty() || stateValue.empty()) {
                writeLog("failed to generate PKCE/state");
            }
            else {
                openBrowserForAuth(port, codeVerifier, stateValue);

                auto authCode = waitForAuthCode(serverSocket, stateValue);
                if (authCode.empty()) {
                    writeLog("OAuth auth code not received (timeout/state mismatch)");
                }
                else if (exchangeCodeForTokens(authCode, port, codeVerifier)) {
                    succeeded = true;
                }
            }
            closesocket(serverSocket);
            WSACleanup();
        }
    }
    catch (...) {
        writeLog("interactive auth: unexpected exception");
    }

    if (succeeded) {
        g_authRequired.store(false);
        g_forcePoll.store(true);  // 認証成功直後に即時ポーリングを誘発
    }
    if (comInitialized) winrt::uninit_apartment();
    g_authInProgress.store(false);
}

// ==================== Calendar イベント処理 ====================

// "+09:00" や "Z" 付き日時を UTC ISO 8601 "YYYY-MM-DDTHH:MM:SS.000Z" に正規化する
// 終日イベント（"YYYY-MM-DD" 形式）は JST 00:00 として UTC 変換する
static std::string normalizeToUtcIso(const std::string& dt) {
    if (dt.empty()) return dt;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;

    // 終日イベント： "YYYY-MM-DD" 形式（10 文字）
    if (dt.size() == 10) {
        if (sscanf_s(dt.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return dt;
        SYSTEMTIME st = {};
        st.wYear = static_cast<WORD>(y); st.wMonth = static_cast<WORD>(mo); st.wDay = static_cast<WORD>(d);
        st = jstToUtc(st);
        char buf[32];
        sprintf_s(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    // 時刻あり: "YYYY-MM-DDTHH:MM:SS..." 形式
    if (sscanf_s(dt.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6) return dt;

    // "Z" は UTC
    if (dt.back() == 'Z') {
        char buf[32];
        sprintf_s(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", y, mo, d, h, mi, s);
        return buf;
    }

    // "+HH:MM" または "-HH:MM" を 10 文字以降で探す
    int tzH = 0, tzM = 0;
    bool negative = false;
    size_t plusPos  = dt.rfind('+');
    size_t minusPos = dt.rfind('-');
    if (plusPos != std::string::npos && plusPos > 10) {
        sscanf_s(dt.c_str() + plusPos + 1, "%d:%d", &tzH, &tzM);
    }
    else if (minusPos != std::string::npos && minusPos > 10) {
        sscanf_s(dt.c_str() + minusPos + 1, "%d:%d", &tzH, &tzM);
        negative = true;
    }
    // タイムゾーンオフセットの妥当性検証（有効範囲: ±14 時間以内）
    if (tzH > 14 || tzM > 59) return dt;

    SYSTEMTIME st = {};
    st.wYear = static_cast<WORD>(y); st.wMonth = static_cast<WORD>(mo); st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(h); st.wMinute = static_cast<WORD>(mi); st.wSecond = static_cast<WORD>(s);

    // UTC に変換（タイムゾーンオフセット分を引く）
    long long offsetHns = ((long long)tzH * 60 + tzM) * 60LL * 10'000'000LL;
    if (negative) offsetHns = -offsetHns;
    auto uli = systemTimeToUli(st);
    uli.QuadPart -= offsetHns;
    st = uliToSystemTime(uli);

    char buf[32];
    sprintf_s(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Calendar API v3 JSON レスポンスを CalendarEvent 配列に変換する
// "error" フィールドがある場合は errorMsg に "API error" をセット
// パースエラーの場合は errorMsg に "JSON parse error" をセット
static ParseResult parseCalendarEvents(const std::string& json) {
    ParseResult result;
    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json));

        if (obj.HasKey(L"error")) {
            result.errorMsg = "API error";
            return result;
        }

        auto arr = obj.GetNamedArray(L"items");
        for (auto item : arr) {
            auto ev = item.GetObject();

            // イベントタイプフィルタ（outOfOffice / workingLocation / focusTime を除外）
            auto evType = winrt::to_string(ev.GetNamedString(L"eventType", L"default"));
            if (evType == "outOfOffice" || evType == "workingLocation" || evType == "focusTime")
                continue;

            // キャンセル済みを除外
            if (winrt::to_string(ev.GetNamedString(L"status", L"")) == "cancelled") continue;

            // 自分が欠席（declined）のイベントを除外
            if (ev.HasKey(L"attendees")) {
                bool declined = false;
                for (auto att : ev.GetNamedArray(L"attendees")) {
                    auto a = att.GetObject();
                    if (a.GetNamedBoolean(L"self", false)
                        && winrt::to_string(a.GetNamedString(L"responseStatus", L"")) == "declined") {
                        declined = true;
                        break;
                    }
                }
                if (declined) continue;
            }

            CalendarEvent e;
            e.id = winrt::to_string(ev.GetNamedString(L"id", L""));

            // 開始日時の UTC 正規化（dateTime または date）
            if (ev.HasKey(L"start")) {
                auto startObj = ev.GetNamedObject(L"start");
                if (startObj.HasKey(L"dateTime"))
                    e.datetime = normalizeToUtcIso(
                        winrt::to_string(startObj.GetNamedString(L"dateTime", L"")));
                else if (startObj.HasKey(L"date"))
                    e.datetime = normalizeToUtcIso(
                        winrt::to_string(startObj.GetNamedString(L"date", L"")));
            }

            e.content   = winrt::to_string(ev.GetNamedString(L"summary",  L""));
            e.permalink = winrt::to_string(ev.GetNamedString(L"htmlLink", L""));

            // reminders.overrides の popup エントリを通知分数として収集（useDefault は無視）
            if (ev.HasKey(L"reminders")) {
                auto rem = ev.GetNamedObject(L"reminders");
                if (!rem.GetNamedBoolean(L"useDefault", true) && rem.HasKey(L"overrides")) {
                    for (auto ov : rem.GetNamedArray(L"overrides")) {
                        auto o = ov.GetObject();
                        if (winrt::to_string(o.GetNamedString(L"method", L"")) == "popup") {
                            auto mins = static_cast<int>(o.GetNamedNumber(L"minutes", 0));
                            if (mins > 0) e.reminderMinutes.push_back(mins);
                        }
                    }
                }
            }

            if (!e.datetime.empty() && !e.content.empty()) result.events.push_back(std::move(e));
        }
    }
    catch (winrt::hresult_error const& e) {
        result.errorMsg = "JSON parse error: " + winrt::to_string(e.message());
    }
    catch (...) {
        result.errorMsg = "JSON parse error: unknown exception";
    }
    return result;
}

// ==================== イベントキャッシュ ====================

// JSON 文字列をアトミックにファイルへ書き出す（"<path>.tmp" 経由で MoveFileEx 置換）
// 電源断・クラッシュで本体ファイルが壊れる可能性を避ける。
// logTag はエラー出力用の識別子（"cache" / "muted" 等）。成功時 true、失敗時 false。
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

// イベントキャッシュの保存
// ポーリング成功時に呼び出し、イベントリストを JSON ファイルに上書き保存する。
// ファイル I/O はロック外で呼ぶこと（events は呼び出し元のローカルコピー）。
static void saveCacheFile(const std::wstring& dir, const std::vector<CalendarEvent>& events) {
    using namespace winrt::Windows::Data::Json;
    try {
        JsonArray arr;
        for (const auto& e : events) {
            JsonObject obj;
            obj.Insert(L"id",        JsonValue::CreateStringValue(winrt::to_hstring(e.id)));
            obj.Insert(L"datetime",  JsonValue::CreateStringValue(winrt::to_hstring(e.datetime)));
            obj.Insert(L"content",   JsonValue::CreateStringValue(winrt::to_hstring(e.content)));
            obj.Insert(L"permalink", JsonValue::CreateStringValue(winrt::to_hstring(e.permalink)));
            JsonArray remArr;
            for (int m : e.reminderMinutes) remArr.Append(JsonValue::CreateNumberValue(m));
            obj.Insert(L"reminderMinutes", remArr);
            arr.Append(obj);
        }
        auto json = winrt::to_string(arr.Stringify());
        atomicWriteJson(dir + L"\\" + CACHE_FILENAME, json, "cache");
    }
    catch (...) {
        writeLog("cache: save failed (exception)");
    }
}

// イベントキャッシュの読み込み
// 起動時に呼び出し、キャッシュファイルからイベントリストを復元する。
// 全イベントの datetime が現在 UTC 時刻より前の場合は空ベクタを返す（古いデータの破棄）。
// ファイル未存在・パースエラー時も空ベクタを返す。
static std::vector<CalendarEvent> loadCacheFile(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    auto path = dir + L"\\" + CACHE_FILENAME;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    SetLastError(0);
    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE) {
        DWORD err = GetLastError();
        CloseHandle(hFile);
        writeLog(err != NO_ERROR
            ? "cache: GetFileSize failed (error " + std::to_string(err) + ")"
            : "cache: unexpected file size (4GB+)");
        return {};
    }
    if (fileSize == 0 || fileSize > 1024 * 1024) {
        CloseHandle(hFile);
        if (fileSize != 0) writeLog("cache: unexpected file size (" + std::to_string(fileSize) + ")");
        return {};
    }
    std::string buf(fileSize, '\0');
    DWORD readBytes = 0;
    BOOL ok = ReadFile(hFile, buf.data(), fileSize, &readBytes, nullptr);
    CloseHandle(hFile);

    if (!ok || readBytes != fileSize) {
        writeLog("cache: read failed (" + std::to_string(readBytes) + "/" + std::to_string(fileSize) + " bytes)");
        return {};
    }

    try {
        auto arr = JsonArray::Parse(winrt::to_hstring(buf));
        std::vector<CalendarEvent> events;
        events.reserve(arr.Size());
        for (auto item : arr) {
            auto obj = item.GetObject();
            CalendarEvent e;
            e.id        = winrt::to_string(obj.GetNamedString(L"id",        L""));
            e.datetime  = winrt::to_string(obj.GetNamedString(L"datetime",  L""));
            e.content   = winrt::to_string(obj.GetNamedString(L"content",   L""));
            e.permalink = winrt::to_string(obj.GetNamedString(L"permalink", L""));
            // reminderMinutes の復元（旧キャッシュ互換: キーなし → 空ベクタ）
            if (obj.HasKey(L"reminderMinutes")) {
                for (auto mv : obj.GetNamedArray(L"reminderMinutes"))
                    e.reminderMinutes.push_back(static_cast<int>(mv.GetNumber()));
            }
            if (!e.datetime.empty() && !e.content.empty()) events.push_back(std::move(e));
        }

        // 全イベントが過去なら破棄（ISO 8601 UTC 文字列の辞書順で比較可能）
        auto nowUtc = getCurrentUtcISO();
        bool allPast = !events.empty() && std::all_of(events.begin(), events.end(),
            [&](const CalendarEvent& e) { return e.datetime <= nowUtc; });
        if (allPast) {
            writeLog("cache: all events are past, discarding");
            return {};
        }

        return events;
    }
    catch (...) {
        writeLog("cache: parse failed");
        return {};
    }
}

// 通知抑制リストの保存
// トグル操作のたびに呼び出し、g_mutedEvents を JSON ファイルに上書き保存する。
// g_mtx ロック外で呼ぶこと。
static void saveMutedEvents(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    try {
        JsonArray arr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (const auto& [key, date] : g_mutedEvents) {
                JsonObject obj;
                obj.Insert(L"key",  JsonValue::CreateStringValue(winrt::to_hstring(key)));
                obj.Insert(L"date", JsonValue::CreateStringValue(winrt::to_hstring(date)));
                arr.Append(obj);
            }
        }
        auto json = winrt::to_string(arr.Stringify());
        atomicWriteJson(dir + L"\\" + MUTED_CACHE_FILENAME, json, "muted");
    }
    catch (...) {
        writeLog("muted: save failed (exception)");
    }
}

// 通知抑制リストの読み込み
// 起動時に呼び出し、当日以降のエントリのみ g_mutedEvents に格納する（過去分を自動プルーニング）。
// ファイル未存在・パースエラー時は何もしない。
static void loadMutedEvents(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    auto path = dir + L"\\" + MUTED_CACHE_FILENAME;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    SetLastError(0);
    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0 || fileSize > 1024 * 1024) {
        CloseHandle(hFile);
        return;
    }
    std::string buf(fileSize, '\0');
    DWORD readBytes = 0;
    BOOL ok = ReadFile(hFile, buf.data(), fileSize, &readBytes, nullptr);
    CloseHandle(hFile);
    if (!ok || readBytes != fileSize) return;

    try {
        auto arr = JsonArray::Parse(winrt::to_hstring(buf));

        SYSTEMTIME utcNow;
        GetSystemTime(&utcNow);
        auto jstNow = utcToJst(utcNow);
        std::string today = systemTimeToIso(jstNow).substr(0, 10);

        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto item : arr) {
            auto obj  = item.GetObject();
            auto key  = winrt::to_string(obj.GetNamedString(L"key",  L""));
            auto date = winrt::to_string(obj.GetNamedString(L"date", L""));
            if (!key.empty() && date >= today)
                g_mutedEvents[key] = date;
        }
        writeLog("muted: loaded " + std::to_string(g_mutedEvents.size()) + " entries");
    }
    catch (...) {
        writeLog("muted: load failed (exception)");
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
static std::optional<std::vector<int>> readSchedule(const std::optional<toml::table>& tbl) {
    if (!tbl) return std::nullopt;
    const auto* arr = (*tbl)["schedule"].as_array();
    if (!arr) return std::nullopt;
    std::vector<int> sched;
    for (const auto& el : *arr) {
        if (sched.size() >= 24) break;
        sched.push_back((std::min)(60, (std::max)(1, el.value_or(1))));
    }
    while (sched.size() < 24) sched.push_back(1);
    return sched;
}

// gcalntfy.toml と gcalntfy.local.toml を読み込んで Config を構築する
//
// local.toml のキーが優先（キー単位でオーバーライド）。
// schedule は local があれば local 全体を使用、なければ base を使用。
static Config loadConfig(const std::wstring& exeDir) {
    auto base  = loadToml(exeDir + L"\\gcalntfy.toml");
    auto local = loadToml(exeDir + L"\\gcalntfy.local.toml");
    if (local) writeLog("Loaded gcalntfy.local.toml (override active)");

    // duck_targets 配列の読み込み（local 優先、なければ base）
    auto readDuckTargets = [&](const std::optional<toml::table>& tbl) -> std::vector<std::wstring> {
        if (!tbl) return {};
        const auto* arr = (*tbl)["duck_targets"].as_array();
        if (!arr) return {};
        std::vector<std::wstring> targets;
        for (const auto& el : *arr) {
            if (auto s = el.value<std::string>()) targets.push_back(toWide(*s));
        }
        return targets;
    };

    // ext_calendar_ids 配列の読み込み（local 優先、なければ base）
    auto readExtCalendarIds = [&](const std::optional<toml::table>& tbl) -> std::vector<std::string> {
        if (!tbl) return {};
        const auto* arr = (*tbl)["ext_calendar_ids"].as_array();
        if (!arr) return {};
        std::vector<std::string> ids;
        for (const auto& el : *arr) {
            if (auto s = el.value<std::string>()) ids.push_back(*s);
        }
        return ids;
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

    cfg.duckTargets = readDuckTargets(local);
    if (cfg.duckTargets.empty()) cfg.duckTargets = readDuckTargets(base);

    cfg.extCalendarIds = readExtCalendarIds(local);
    if (cfg.extCalendarIds.empty()) cfg.extCalendarIds = readExtCalendarIds(base);

    // notify_minutes（通知リード時間、分単位。デフォルト 5 分、0〜30 にクランプ）
    long long notifyMin = DEFAULT_NOTIFY_MINUTES;
    if (local && (*local)["notify_minutes"].is_integer())
        notifyMin = **(*local)["notify_minutes"].as_integer();
    else if (base && (*base)["notify_minutes"].is_integer())
        notifyMin = **(*base)["notify_minutes"].as_integer();
    notifyMin = (std::max)((long long)MIN_NOTIFY_MINUTES, (std::min)((long long)MAX_NOTIFY_MINUTES, notifyMin));
    cfg.notifyLeadMs = notifyMin * 60LL * 1000LL;

    // [guard] / [loudness] セクション読み込みヘルパー
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

// ==================== 時刻ユーティリティ ====================

// ISO 8601 UTC 文字列 "YYYY-MM-DDTHH:MM:SS...Z" を ULARGE_INTEGER（100 ナノ秒単位）に変換する
static ULARGE_INTEGER parseIsoToUli(const std::string& iso) {
    SYSTEMTIME st;
    if (!parseIsoToSystemTime(iso, st)) {
        return ULARGE_INTEGER{};
    }
    return systemTimeToUli(st);
}

// 2 つの UTC ISO 8601 文字列の差をミリ秒で返す（isoTarget - isoNow、負の場合は 0）
static long long calcDiffMs(const std::string& isoTarget, const std::string& isoNow) {
    auto target = parseIsoToUli(isoTarget);
    auto now    = parseIsoToUli(isoNow);
    if (target.QuadPart <= now.QuadPart) return 0LL;
    return static_cast<long long>((target.QuadPart - now.QuadPart) / 10000LL);
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
// targets が空の場合は空リストを返す（ダッキング無効）。
// COM デバイス取得失敗時はログ出力して空リストを返す。
// 元々ミュート済みのセッションはスキップする（復元時にアンミュートしない）。
// 呼び出し元は COM が初期化済みであること（STA/MTA 問わず）。
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
static constexpr const wchar_t* REG_KEY_PATH        = L"SOFTWARE\\gcalntfy";
static constexpr const wchar_t* REG_SOUND_ENABLED     = L"SoundEnabled";
static constexpr const wchar_t* REG_MUTE_IN_MEETING   = L"MuteInMeeting";
static constexpr const wchar_t* REG_NOTIFIED_VERSION  = L"NotifiedUpdateVersion";

// Windows スタートアップ登録用レジストリ（HKCU Run キー）
static constexpr const wchar_t* REG_RUN_KEY_PATH    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr const wchar_t* REG_RUN_VALUE_NAME  = L"gcalntfy";

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
// HKCU Run キーに gcalntfy 値が存在すれば登録済みとみなす
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
// HKCU Run キーから gcalntfy 値を削除する。値が存在しない場合はエラーを無視
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
// LastUsedTimeStop == 0 のサブキーがあれば使用中（UWP 配下 + NonPackaged 配下の両方を走査）。
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
// 通知スレッドの MTA COM を利用（呼び出し元スレッドで CoInitializeEx 済み前提）。
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
// WASAPI 補完（isMicCaptureActive）はマイクのみ対象（カメラはレジストリ検出のみ）。
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
// g_wavCache.valid == false の場合は音声を再生せずに終了する（Toast 通知は呼び出し側で別途表示）。
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
            // WAIT_TIMEOUT は上記で処理済みなので、ここに来るのは WAIT_FAILED のみ（実質）。
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
// AUMID 変更に伴い旧 gcal-notify.lnk が残る場合は削除する。
static void ensureShortcut() {
    wchar_t appData[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return;

    // 旧ショートカット削除（AUMID 変更による残留ショートカット対処）
    std::wstring oldLink = std::wstring(appData)
        + L"\\Microsoft\\Windows\\Start Menu\\Programs\\gcal-notify.lnk";
    DeleteFileW(oldLink.c_str());

    std::wstring linkPath = std::wstring(appData)
        + L"\\Microsoft\\Windows\\Start Menu\\Programs\\gcalntfy.lnk";

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
// 初回呼び出し時に結果をキャッシュする（app.ico は起動後に変化しない）。
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
// xml は </visual> まで構築済みの文字列を渡す（</toast> は内部で付加する）。
static void dispatchToastXml(std::wstring xml, const std::wstring& permalink) {
    if (!permalink.empty() && isHttpUrl(permalink)) {
        xml += L"<actions>"
               L"<action activationType=\"protocol\" content=\"Calendar\""
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
// OS に通知を登録して即 return する（コールバック待機なし）。
// アプリアイコン（exe 同フォルダの app.ico）・Calendar を開くボタンを含むリッチな通知を表示する。
// silent=true（デフォルト）: OS 通知音を無効化する。
// silent=false: <audio> タグを省略し OS 標準通知音を鳴らす。
static void showToast(const std::wstring& timeJST, const std::wstring& title,
                      const std::wstring& permalink, bool silent)
{
    std::wstring xml =
        L"<toast>"
        L"<visual><binding template=\"ToastGeneric\">"
        + buildIconTag() +
        L"<text>" + escapeXml(timeJST) + L"</text>"
        L"<text>" + escapeXml(title)   + L"</text>"
        L"</binding></visual>"
        + (silent ? L"<audio silent=\"true\"/>" : L"");

    dispatchToastXml(std::move(xml), permalink);
}

// 3 行 Toast 通知を表示する（変更・キャンセル通知用）
//
// line1 を title スタイル（太字大）で表示し、OS 標準通知音を鳴らす。
static void showToast3(const std::wstring& line1, const std::wstring& line2,
                       const std::wstring& line3, const std::wstring& permalink)
{
    std::wstring xml =
        L"<toast>"
        L"<visual><binding template=\"ToastGeneric\">"
        + buildIconTag() +
        L"<text hint-style=\"title\">" + escapeXml(line1) + L"</text>"
        L"<text>" + escapeXml(line2) + L"</text>"
        L"<text>" + escapeXml(line3) + L"</text>"
        L"</binding></visual>";

    dispatchToastXml(std::move(xml), permalink);
}

// エラー Toast 表示（クールダウン制御付き）
//
// 前回通知から ERROR_TOAST_COOLDOWN_MS 以内は抑制する。
// showToast の第 1 引数（時刻欄）にエラー種別を流用して表示する。
static void showErrorToast(const std::wstring& title, const std::wstring& body)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastErrorToastTime.load() < ERROR_TOAST_COOLDOWN_MS) return;
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

// 認証必要 Toast を表示する
//
// XML に launch="auth" を付与し、Toast 本体クリックで Activated イベントが発火するようにする。
// Activated ハンドラから WM_AUTH_REQUESTED を WndProc に送り、UI スレッド経由で startInteractiveAuth を起動する。
//
// ライフタイム対策: ToastNotification がスコープを抜けるとイベントが発火しないため、
// プロセス寿命の static vector に保持して延命する（直近 4 件まで保持）。
static void showAuthRequiredToast() {
    static std::mutex                                                   tokensMtx;
    static std::vector<winrt::Windows::UI::Notifications::ToastNotification> tokens;

    std::wstring xml =
        L"<toast launch=\"auth\" activationType=\"foreground\">"
        L"<visual><binding template=\"ToastGeneric\">"
        + buildIconTag() +
        L"<text>Google 認証が必要です</text>"
        L"<text>クリックしてブラウザで認証してください</text>"
        L"</binding></visual>"
        L"<audio silent=\"true\"/>"
        L"</toast>";

    try {
        winrt::Windows::Data::Xml::Dom::XmlDocument doc;
        doc.LoadXml(xml);

        auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager
            ::CreateToastNotifier(APP_AUMID);
        winrt::Windows::UI::Notifications::ToastNotification notification(doc);

        notification.Activated([](
            winrt::Windows::UI::Notifications::ToastNotification const&,
            winrt::Windows::Foundation::IInspectable const& args)
        {
            // Toast 本体クリック時は ToastActivatedEventArgs::Arguments() == launch 属性値（"auth"）
            try {
                auto e = args.try_as<winrt::Windows::UI::Notifications::ToastActivatedEventArgs>();
                if (e && e.Arguments() != L"auth") return;
            }
            catch (...) { /* try_as 失敗は本体クリック扱いで続行 */ }
            if (g_hWnd) PostMessage(g_hWnd, WM_AUTH_REQUESTED, 0, 0);
        });

        notifier.Show(notification);

        std::lock_guard<std::mutex> lk(tokensMtx);
        tokens.push_back(notification);
        if (tokens.size() > 4) tokens.erase(tokens.begin());
    }
    catch (winrt::hresult_error const& e) {
        writeLog("showAuthRequiredToast failed: " + winrt::to_string(e.message()));
    }
    catch (...) {
        writeLog("showAuthRequiredToast failed: unknown exception");
    }
}

// 認証必要状態の通知（状態遷移＋クールダウン制御つき）
//
// ポーリングループから呼び出される。認証フロー実行中は何もしない。
// g_authRequired が false→true へ遷移したタイミング、または前回 Toast から
// AUTH_TOAST_COOLDOWN_MS 以上経過した場合のみ Toast を表示する。
static void notifyAuthRequired() {
    if (g_authInProgress.load()) return;

    bool wasFalse = !g_authRequired.exchange(true);
    ULONGLONG now = GetTickCount64();
    bool cooldownPassed = (now - g_lastAuthToastTime.load() >= AUTH_TOAST_COOLDOWN_MS);

    if (wasFalse || cooldownPassed) {
        g_lastAuthToastTime.store(now);
        showAuthRequiredToast();
    }
}

// ==================== トレイアイコン ====================

// バックグラウンドスレッド用の中断可能 Sleep
//
// メッセージは処理しない（呼び出し元がメインスレッドではないため）。
// g_shutdownRequested または g_forcePoll が true になった時点で即座にリターンする。
// 100 ms 単位で各フラグをポーリングするため、最大 100 ms の中断遅延が発生する。
static void waitInterruptible(DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (!g_shutdownRequested && !g_forcePoll.load()) {
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
    SetTimer(hWnd, IDT_TOOLTIP_REFRESH, TOOLTIP_REFRESH_MS, nullptr);
}

// バッジ付きトレイアイコンの生成
// ベースアイコンの右下に赤い円バッジを合成した HICON を返す。
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

    // バッジ円のパラメータ（アイコンを十字 4 等分した右下領域にマージン 1px で収める）
    int badgeSize = (std::max)(cx / 2 - 2, 3);
    int ox   = cx / 2;
    int oy   = cy / 2 + 1;
    float midX = ox + badgeSize / 2.0f;
    float midY = oy + badgeSize / 2.0f;
    float r    = badgeSize / 2.0f;

    // 距離ベースのアルファブレンドで円エッジを滑らかに描画（アンチエイリアス）
    int scanPad = static_cast<int>(r) + 1;
    for (int y = oy - scanPad; y < oy + scanPad + badgeSize; ++y) {
        if (y < 0 || y >= cy) continue;
        for (int x = ox - scanPad; x < ox + scanPad + badgeSize; ++x) {
            if (x < 0 || x >= cx) continue;
            float d     = sqrtf((x - midX) * (x - midX) + (y - midY) * (y - midY));
            float alpha = (d <= r - 0.5f) ? 1.0f : (d <= r + 0.5f) ? (r + 0.5f - d) : 0.0f;
            if (alpha <= 0.0f) continue;
            UINT32 a = static_cast<UINT32>(alpha * 255.0f + 0.5f);
            pixels[y * cx + x] = (a << 24) | 0x00FF0000u;
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

// トレイアイコンのバッジ切り替え
// hasUpcoming が g_trayBadgeActive（前回状態）と同じなら NIM_MODIFY をスキップする。
static void updateTrayIcon(HWND hWnd, bool hasUpcoming) {
    if (hasUpcoming == g_trayBadgeActive) return;
    g_trayBadgeActive = hasUpcoming;

    auto nid   = makeTrayNid(hWnd);
    nid.uFlags = NIF_ICON;
    if (hasUpcoming) {
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

// トレイアイコンのツールチップを更新する
// 現在JST時刻以降の当日イベント件数を「本日の以降予定：N 件」として表示する。
// ポップアップメニュー表示中は更新しない
static void updateTrayTooltip(HWND hWnd) {
    if (g_popupShowing.load()) return;
    if (g_tooltipUpdating) return;
    g_tooltipUpdating = true;

    // 未認証時はその旨を最優先で表示する（左クリックで認証フローを起動できる）
    if (g_authRequired.load()) {
        auto nid = makeTrayNid(hWnd);
        nid.uFlags = NIF_TIP;
        wcscpy_s(nid.szTip, L"Google 認証が必要です（クリックで開始）");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        updateTrayIcon(hWnd, false);
        g_tooltipUpdating = false;
        return;
    }

    std::vector<CalendarEvent> events;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        events = g_pendingEvents;
    }
    SYSTEMTIME utcNow;
    GetSystemTime(&utcNow);
    auto jstNow = utcToJst(utcNow);
    std::string nowJst = systemTimeToIso(jstNow);
    std::string today  = nowJst.substr(0, 10);
    int count = 0;
    for (const auto& ev : events) {
        auto jst = utcIsoToJst(ev.datetime);
        if (jst.substr(0, 10) == today && jst >= nowJst) ++count;
    }
    auto nid = makeTrayNid(hWnd);
    nid.uFlags = NIF_TIP;
    if (count > 0)
        swprintf_s(nid.szTip, _countof(nid.szTip), L"本日の以降予定：%d 件", count);
    else
        wcscpy_s(nid.szTip, NO_UPCOMING_EVENTS);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    updateTrayIcon(hWnd, count > 0);
    g_tooltipUpdating = false;
}

// トレイアイコンを除去する
static void removeTrayIcon(HWND hWnd) {
    KillTimer(hWnd, IDT_TOOLTIP_REFRESH);
    auto nid = makeTrayNid(hWnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}


// イベントの通知済み判定キーを生成する
// Google Calendar API の id フィールドを優先使用し、未取得時は datetime+content にフォールバックする。
// id ベースにすることで、ユーザがイベントタイトルを編集しても重複通知を防止できる。
static inline std::string eventKey(const CalendarEvent& e) {
    return e.id.empty() ? (e.datetime + "|" + e.content) : e.id;
}

// メニュー描画用フォントの初期化
// OS のメニューフォント設定を取得して左クリックポップアップの予定項目描画用フォントを作成する。
static void initMenuFonts() {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hMenuFont = CreateFontIndirectW(&ncm.lfMenuFont);
}

// 左クリックポップアップの予定項目（IDM_EVENT_BASE + index に対応、WndProc スレッドのみ使用）
struct ScheduleItem {
    std::wstring permalink;
    std::string  key;    // eventKey(e)：右クリック抑制トグル用
    std::string  date;   // JST YYYY-MM-DD：抑制リスト保存用
    std::wstring label;  // 描画テキスト（WM_DRAWITEM / WM_MEASUREITEM で使用）
    bool         muted;  // 抑制中フラグ（右クリックトグル時にもその場で更新する）
};
static std::vector<ScheduleItem> g_scheduleItems;

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

// 左クリック時の予定一覧ポップアップ表示
// g_pendingEvents から現在時刻以降の当日（JST）イベントを抽出してメニューに表示する。
// 左クリックで予定ページを開き、右クリックで通知抑制をトグルする。
static void showSchedulePopup(HWND hWnd) {
    std::vector<CalendarEvent> events;
    std::unordered_map<std::string, std::string> mutedSnapshot;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        events       = g_pendingEvents;
        mutedSnapshot = g_mutedEvents;
    }

    SYSTEMTIME utcNow;
    GetSystemTime(&utcNow);
    auto jstNow = utcToJst(utcNow);
    std::string nowJst = systemTimeToIso(jstNow);
    std::string today  = nowJst.substr(0, 10);

    std::vector<ScheduleItem> todayEvents;
    for (const auto& ev : events) {
        auto jst = utcIsoToJst(ev.datetime);
        if (jst.substr(0, 10) != today) continue;
        if (jst < nowJst) continue;
        auto key   = eventKey(ev);
        auto date  = jst.substr(0, 10);
        bool muted = mutedSnapshot.count(key) != 0;
        // "HH:MM タイトル" 形式（MF_UNCHECKED/MF_CHECKED がアイコン列を確保するためプレフィックス不要）
        std::wstring label = toWide((jst.size() >= 16 ? jst.substr(11, 5) : "??:??") + " " + ev.content);
        todayEvents.push_back({toWide(ev.permalink), key, date, label, muted});
    }

    g_scheduleItems.clear();
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        writeLog("showSchedulePopup: CreatePopupMenu failed");
        return;
    }
    if (todayEvents.empty()) {
        AppendMenuW(hMenu, MF_STRING, IDM_OPEN_CALENDAR_TODAY, NO_UPCOMING_EVENTS);
    }
    else {
        UINT idx = 0;
        for (const auto& te : todayEvents) {
            if (idx >= (IDM_EVENT_MAX - IDM_EVENT_BASE)) break;
            // MFT_OWNERDRAW で WM_MEASUREITEM / WM_DRAWITEM に描画を委譲する。
            // dwItemData にインデックスを渡し、描画時に g_scheduleItems から参照する。
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask     = MIIM_FTYPE | MIIM_ID | MIIM_DATA;
            mii.fType     = MFT_OWNERDRAW;
            mii.wID       = IDM_EVENT_BASE + idx;
            mii.dwItemData = static_cast<ULONG_PTR>(idx);
            InsertMenuItemW(hMenu, idx, TRUE, &mii);
            g_scheduleItems.push_back(te);
            ++idx;
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        std::wstring footer = L"本日の以降予定：" + std::to_wstring(g_scheduleItems.size())
                + (todayEvents.size() > g_scheduleItems.size() ? L" 件（超過分省略）" : L" 件")
                + L"（右クリックで通知抑制）";
        AppendMenuW(hMenu, MF_STRING, IDM_OPEN_CALENDAR_TODAY, footer.c_str());
    }

    POINT pt;
    GetCursorPos(&pt);
    forceForeground(hWnd);
    // TPM_LEFTBUTTON のみ指定する（TPM_RIGHTBUTTON を加えると右クリックも WM_COMMAND
    // を発火してしまい、抑制トグル用の WM_MENURBUTTONUP が届かなくなる）
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
            std::string body = httpGet(GITHUB_API_RELEASES_LATEST, L"", &status);
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
    std::wstring prefix = std::wstring(L"Gcalntfy v") + APP_VERSION + L" → ";
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
    std::wstring prefix = std::wstring(L"Gcalntfy v") + APP_VERSION + L" → ";
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
// その場で構築する（チェック状態は呼び出し時の最新値を反映）。
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
        // 新版あり: オーナードローで "Gcalntfy vX.Y.Z → vNew" を赤文字で表示する
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_FTYPE | MIIM_ID;
        mii.fType = MFT_OWNERDRAW;
        mii.wID   = IDM_OPEN_GITHUB;
        InsertMenuItemW(hMenu, 0, TRUE, &mii);
    }
    else {
        AppendMenuW(hMenu, MF_STRING, IDM_OPEN_GITHUB, L"Gcalntfy v" APP_VERSION);
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 音声通知（親: レジストリ永続化）
    AppendMenuW(hMenu, MF_STRING | (g_soundEnabled ? MF_CHECKED : MF_UNCHECKED),
        IDM_SOUND_ENABLED, L"通知音を鳴らす");

    // 子項目: 親が OFF なら非活性
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
// 未認証時は対話的認証フローを起動、それ以外は予定一覧ポップアップを表示する。
static void handleTrayLeftClick(HWND hWnd) {
    // 未認証時はメニューを挟まず即フロー起動。tooltip で事前にユーザに告知済み
    if (g_authRequired.load()) {
        if (!g_authInProgress.load()) {
            try {
                std::thread(startInteractiveAuth).detach();
            }
            catch (const std::system_error& e) {
                writeLog(std::string("failed to start auth thread: ") + e.what());
            }
        }
        return;
    }
    g_popupShowing.store(true);
    clearTrayTooltip(hWnd);
    showSchedulePopup(hWnd);
    g_popupShowing.store(false);
    updateTrayTooltip(hWnd);
}

// 当日ログファイルのパスを取得し、存在しなければ logs フォルダのパスを返す
//
// 「当日」は JST 基準で判定する（writeLog の日付ロールオーバ判定と同じ基準）。
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

// WM_COMMAND ディスパッチ
// メニュー選択（IDM_*）と予定一覧クリック（IDM_EVENT_BASE 以降）を処理する。
static void handleTrayCommand(UINT id) {
    if (id == IDM_EXIT) {
        g_shutdownRequested = true;
        PostQuitMessage(0);
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
    if (id == IDM_OPEN_CALENDAR_TODAY) {
        ShellExecuteW(nullptr, L"open", CALENDAR_TODAY_URL, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_CONFIG) {
        // 設定ファイルを OS デフォルトのエディタで開く（変更反映には再起動が必要）
        std::wstring toml = getExeDir() + L"\\gcalntfy.toml";
        ShellExecuteW(nullptr, L"open", toml.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_LOG) {
        auto target = getCurrentLogTarget();
        if (!target.empty())
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id >= IDM_EVENT_BASE && id < IDM_EVENT_MAX) {
        UINT idx = id - IDM_EVENT_BASE;
        if (idx < g_scheduleItems.size() && isHttpUrl(g_scheduleItems[idx].permalink)) {
            ShellExecuteW(nullptr, L"open", g_scheduleItems[idx].permalink.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

// 左クリックポップアップの owner-draw 項目サイズ計算
// 戻り値: TRUE で処理済み、FALSE で未処理（DefWindowProcW へ）
static BOOL measureScheduleMenuItem(HWND hWnd, MEASUREITEMSTRUCT* mis) {
    if (mis->CtlType != ODT_MENU) return FALSE;
    UINT eidx = static_cast<UINT>(mis->itemData);
    if (eidx >= g_scheduleItems.size()) return FALSE;
    const auto& item = g_scheduleItems[eidx];
    HDC   hdc = GetDC(hWnd);
    HFONT old = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
    SIZE  sz  = {};
    GetTextExtentPoint32W(hdc, item.label.c_str(),
        static_cast<int>(item.label.size()), &sz);
    SelectObject(hdc, old);
    ReleaseDC(hWnd, hdc);
    // 左右パディングとして 16 px ずつ確保する
    mis->itemWidth  = static_cast<UINT>(sz.cx) + 32;
    mis->itemHeight = static_cast<UINT>(sz.cy) + 6;
    return TRUE;
}

// 左クリックポップアップの owner-draw 項目描画
// ODS_SELECTED に応じた背景色・テキスト色を切り替え、muted フラグが立つ項目には
// DrawTextW 後に 2 px の取消線を手動で重ね描画する。
static BOOL drawScheduleMenuItem(DRAWITEMSTRUCT* dis) {
    if (dis->CtlType != ODT_MENU) return FALSE;
    UINT eidx = static_cast<UINT>(dis->itemData);
    if (eidx >= g_scheduleItems.size()) return FALSE;
    const auto& item     = g_scheduleItems[eidx];
    bool        selected = (dis->itemState & ODS_SELECTED) != 0;

    FillRect(dis->hDC, &dis->rcItem,
        reinterpret_cast<HBRUSH>(
            static_cast<INT_PTR>(selected ? COLOR_HIGHLIGHT + 1 : COLOR_MENU + 1)));

    RECT textRect  = dis->rcItem;
    textRect.left += 16;
    SetBkMode(dis->hDC, TRANSPARENT);
    COLORREF textColor = GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
    SetTextColor(dis->hDC, textColor);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, g_hMenuFont));
    DrawTextW(dis->hDC, item.label.c_str(), -1, &textRect,
        DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    if (item.muted) {
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

// 予定項目の通知抑制をトグルする（左クリックポップアップ上の右クリック）
// g_mutedEvents と item.muted をトグルし、自スレッド所有のメニューウィンドウを再描画する。
static void toggleScheduleItemMute(UINT itemIdx, HMENU hm) {
    UINT id = GetMenuItemID(hm, static_cast<int>(itemIdx));
    if (id < IDM_EVENT_BASE || id >= IDM_EVENT_MAX) return;
    UINT eidx = id - IDM_EVENT_BASE;
    if (eidx >= g_scheduleItems.size()) return;

    auto& item = g_scheduleItems[eidx];
    bool nowMuted;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_mutedEvents.find(item.key);
        if (it != g_mutedEvents.end()) {
            g_mutedEvents.erase(it);
            nowMuted = false;
        }
        else {
            g_mutedEvents[item.key] = item.date;
            nowMuted = true;
        }
    }
    item.muted = nowMuted;
    // 自スレッド所有のポップアップメニューウィンドウ（クラス名 "#32768"）を全て再描画する
    // FindWindowW はグローバル検索でタイミング依存・他プロセスの誤ヒットがあるため EnumThreadWindows を用いる
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t className[16] = {};
        if (GetClassNameW(hwnd, className, ARRAYSIZE(className))
            && wcscmp(className, L"#32768") == 0) {
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);
        }
        return TRUE;
    }, 0);
    saveMutedEvents(g_exeDir);
    g_forcePoll.store(true);
    writeLog(std::string("muted: ") + (nowMuted ? "added " : "removed ") + item.key);
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
    if (msg == WM_AUTH_REQUESTED) {
        if (!g_authInProgress.load()) {
            try {
                std::thread(startInteractiveAuth).detach();
            }
            catch (const std::system_error& e) {
                writeLog(std::string("failed to start auth thread: ") + e.what());
            }
        }
        return 0;
    }
    if (msg == WM_TIMER && wParam == IDT_TOOLTIP_REFRESH) {
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
        if (measureScheduleMenuItem(hWnd, mis)) return TRUE;
    }
    if (msg == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlType == ODT_MENU && dis->itemID == IDM_OPEN_GITHUB)
            return drawVersionMenuItem(dis) ? TRUE : DefWindowProcW(hWnd, msg, wParam, lParam);
        if (drawScheduleMenuItem(dis)) return TRUE;
    }
    // 左クリックポップアップ上の右クリック: 通知抑制をトグルする
    // WM_MENURBUTTONUP は TPM_RIGHTBUTTON 指定なしでも右クリックで届く（選択は発生しない）。
    if (msg == WM_MENURBUTTONUP) {
        toggleScheduleItemMute(static_cast<UINT>(wParam), reinterpret_cast<HMENU>(lParam));
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
    wc.lpszClassName = L"gcalntfy_tray";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, L"gcalntfy_tray", nullptr, 0,
        0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
}

// ==================== 予定変更検知 ====================

// 予定変更の種別
enum class EventChangeType { TimeChanged, Cancelled, Added };

// 検出した変更 1 件分
struct EventChange {
    EventChangeType type;
    std::string     oldDatetime;  // TimeChanged: 旧日時、Cancelled: 通知表示用日時
    std::string     newDatetime;  // TimeChanged / Added 時に使用（Cancelled 時は空）
    std::string     content;      // イベント名
    std::string     permalink;    // Calendar URL（空でもよい）
};

// イベントリストの変更を検出する
//
// oldEvents と newEvents を id で突合し、日時変更・追加・キャンセルを検出する。
// Added 検知はポーリングウィンドウへの新規進入を検知するものであり、ユーザが Calendar
// に実際に追加した予定との区別は行わない（firstPoll スキップで起動直後の誤検知を抑制）。
// id が空のイベントは比較対象から除外する。
// oldEvents が空の場合は空のベクタを返す（変更検知の開始前状態）。
static std::vector<EventChange> collectEventChanges(
    const std::vector<CalendarEvent>& oldEvents,
    const std::vector<CalendarEvent>& newEvents)
{
    if (oldEvents.empty()) return {};

    std::unordered_map<std::string, const CalendarEvent*> oldMap;
    for (const auto& e : oldEvents) {
        if (!e.id.empty()) oldMap[e.id] = &e;
    }

    std::unordered_set<std::string> newIds;
    std::vector<EventChange> changes;

    for (const auto& e : newEvents) {
        if (e.id.empty()) continue;
        newIds.insert(e.id);
        auto it = oldMap.find(e.id);
        if (it == oldMap.end()) {
            changes.push_back({EventChangeType::Added,
                               {}, e.datetime,
                               e.content, e.permalink});
        }
        else if (it->second->datetime != e.datetime) {
            changes.push_back({EventChangeType::TimeChanged,
                               it->second->datetime, e.datetime,
                               e.content, e.permalink});
        }
    }

    auto nowUtc = getCurrentUtcISO();
    for (const auto& [id, old] : oldMap) {
        if (newIds.find(id) == newIds.end()) {
            // 開始済みのイベントはポーリングウィンドウから自然消失しただけなのでスキップ
            if (old->datetime <= nowUtc) continue;
            changes.push_back({EventChangeType::Cancelled,
                               old->datetime, {},
                               old->content, old->permalink});
        }
    }

    return changes;
}

// 検出した変更に対して Toast 通知を送信する
//
// g_mtx のロック外から呼ぶこと（Toast 送信中に通知スレッドがロックを取得できるようにするため）。
static void notifyEventChanges(const std::vector<EventChange>& changes)
{
    for (const auto& c : changes) {
        auto wContent   = toWide(c.content);
        auto wPermalink = toWide(c.permalink);
        try {
            switch (c.type) {
            case EventChangeType::TimeChanged: {
                auto line2 = utcToJstMDHHMM(c.oldDatetime) + L" → " + utcToJstMDHHMM(c.newDatetime);
                showToast3(L"予定変更", line2, wContent, wPermalink);
                break;
            }
            case EventChangeType::Cancelled:
                showToast3(L"予定キャンセル", utcToJstMDHHMM(c.oldDatetime), wContent, wPermalink);
                break;
            case EventChangeType::Added:
                showToast3(L"予定追加", utcToJstMDHHMM(c.newDatetime), wContent, wPermalink);
                break;
            }
        }
        catch (...) {
            writeLog("notifyEventChanges: toast failed for " + c.content);
        }
    }
}

// ==================== 通知スレッド ====================

// 通知済みセット用キーを生成する
// leadMsVal はミリ秒単位。eventKey に "@分数" サフィックスを付けた形式で返す
static inline std::string notifyKey(const CalendarEvent& e, long long leadMsVal) {
    return eventKey(e) + "@" + std::to_string(leadMsVal / 60000);
}

// notifiedSet の自然失効: 新リストに含まれないキーを削除する
//
// notifiedSet のキーは "eventKey@minutes" 形式。
// イベントが削除・変更されたとき、対応するすべての "@minutes" エントリを失効させる。
static void pruneNotifiedSet(std::set<std::string>& notifiedSet,
                             const std::vector<CalendarEvent>& events)
{
    std::set<std::string> validBaseKeys;
    for (const auto& e : events) validBaseKeys.insert(eventKey(e));

    for (auto it = notifiedSet.begin(); it != notifiedSet.end(); ) {
        // "@minutes" サフィックスを除いたベースキーで照合する
        auto sep = it->rfind('@');
        auto base = (sep != std::string::npos) ? it->substr(0, sep) : *it;
        it = validBaseKeys.count(base) ? std::next(it) : notifiedSet.erase(it);
    }
}

// 通知発火: Toast 表示と音声再生を実行し、notifiedSet を更新する
// 音声スキップ判定（音声 OFF・会議中）はここで行い、Toast はグループ全件に出す。
static void fireNotificationGroup(const std::vector<const CalendarEvent*>& group,
    const std::string& targetDatetime, long long targetLeadMs,
    const Config& localConfig, std::set<std::string>& notifiedSet)
{
    auto jstTimeW = utcToJstHHMM(targetDatetime);
    auto jstTime  = wideToUtf8(jstTimeW);
    writeLog("notify: " + jstTime + " (" + std::to_string(group.size()) + " event(s), "
        + std::to_string(targetLeadMs / 60000) + "min before)");
    // 音声スキップ判定: 音声通知OFF > マイク/カメラ使用中ミュート > 通常再生
    if (!g_soundEnabled) {
        writeLog("sound skipped (sound disabled)");
    }
    else if (g_muteInMeeting && isMeetingActive()) {
        writeLog("sound skipped (mic/camera in use)");
    }
    else {
        launchSound(localConfig);
    }
    for (const auto* ev : group) {
        showToast(jstTimeW, toWide(ev->content), toWide(ev->permalink));
        notifiedSet.insert(notifyKey(*ev, targetLeadMs));
    }
}

// 発火対象を特定する: 通知タイミングが到来しかつ未通知のイベントから最初の (datetime, leadMs) を返す
// 見つからなければ targetDatetime を空のまま返す。
static void selectFireTarget(const std::vector<CalendarEvent>& localEvents,
    const std::string& nowUtc, const std::set<std::string>& notifiedSet,
    const std::unordered_set<std::string>& mutedKeys, long long leadMs,
    std::string& targetDatetime, long long& targetLeadMs)
{
    targetDatetime.clear();
    targetLeadMs = 0;
    for (const auto& e : localEvents) {
        long long diffMs = calcDiffMs(e.datetime, nowUtc);
        if (diffMs <= 0) continue;
        if (mutedKeys.count(eventKey(e))) continue;

        auto tryLead = [&](long long lv) -> bool {
            auto key = notifyKey(e, lv);
            if (!notifiedSet.count(key) && diffMs - lv <= 0) {
                targetDatetime = e.datetime;
                targetLeadMs   = lv;
                return true;
            }
            return false;
        };
        if (tryLead(leadMs)) return;
        for (int m : e.reminderMinutes)
            if (tryLead(static_cast<long long>(m) * 60000)) return;
    }
}

// 通知スレッド: メインスレッドから予定リストを受け取り、通知を実行する
//
// MTA で COM/WinRT を初期化し（winrt::init_apartment は既定で MTA）、g_cv で予定リスト更新を待機する。
// notify_minutes 前を基本通知タイミングとし、イベントの reminders.overrides に popup が
// 設定されていれば、そのタイミングでも追加通知する（重複分数は 1 回のみ通知）。
// notifiedSet のキーは "eventKey@minutes" 形式で、同一イベントの異なるタイミングを区別する。
// 全イベント × 全通知分数を走査して最小発火時間を求めてから wait_until で待機する。
static void notifyThreadFunc(const std::wstring& exeDir) {
    winrt::init_apartment();

    std::set<std::string>      notifiedSet;
    std::vector<CalendarEvent> localEvents;
    Config                     localConfig;

    while (!g_shutdownRequested) {
        // 予定リスト更新を待機
        // g_mutedEvents のキーセットをスナップショットして内側ループのロック取得を O(1) 回に削減する
        std::unordered_set<std::string> mutedKeys;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return g_eventsUpdated || g_shutdownRequested.load(); });
            if (g_shutdownRequested) break;
            localEvents     = g_pendingEvents;
            localConfig     = g_currentConfig;
            g_eventsUpdated = false;
            for (const auto& kv : g_mutedEvents)
                mutedKeys.insert(kv.first);
        }
        pruneNotifiedSet(notifiedSet, localEvents);

        // 直近未通知イベントを順次通知する内側ループ
        while (!g_shutdownRequested) {
            auto nowUtc = getCurrentUtcISO();
            long long leadMs = localConfig.notifyLeadMs;

            // 全イベント × 全通知分数を走査して最小発火待機時間を計算
            // notifiedSet キーは "eventKey@minutes" 形式
            long long minFireMs = LLONG_MAX;
            for (const auto& e : localEvents) {
                long long diffMs = calcDiffMs(e.datetime, nowUtc);
                if (diffMs <= 0) {
                    // 開始済みイベント: 全通知タイミングを通知済みとしてマーク
                    notifiedSet.insert(notifyKey(e, leadMs));
                    for (int m : e.reminderMinutes)
                        notifiedSet.insert(notifyKey(e, static_cast<long long>(m) * 60000));
                    continue;
                }
                // 通知抑制中のイベントは minFireMs 計算から除外する
                if (mutedKeys.count(eventKey(e))) continue;
                // notify_minutes（ベースライン）+ reminders のすべてのタイミングをチェック
                auto checkLead = [&](long long leadMsVal) {
                    auto key = notifyKey(e, leadMsVal);
                    if (!notifiedSet.count(key))
                        minFireMs = (std::min)(minFireMs, diffMs - leadMsVal);
                };
                checkLead(leadMs);
                // 遡及発火を防ぐため、通知タイミング経過済みの reminders は通知済みとみなす
                for (int m : e.reminderMinutes) {
                    long long rmMs = static_cast<long long>(m) * 60000;
                    if (diffMs - rmMs < 0) {
                        notifiedSet.insert(notifyKey(e, static_cast<long long>(m) * 60000));
                    }
                    else {
                        checkLead(rmMs);
                    }
                }
            }
            if (minFireMs == LLONG_MAX) break; // 通知すべき予定なし → 外側ループへ

            // 発火時刻まで待機（途中でイベント更新があれば再評価）
            if (minFireMs > 0) {
                std::unique_lock<std::mutex> lk(g_mtx);
                auto wakeAt = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(minFireMs);
                g_cv.wait_until(lk, wakeAt,
                    [] { return g_eventsUpdated || g_shutdownRequested.load(); });
                if (g_eventsUpdated) {
                    localEvents     = g_pendingEvents;
                    localConfig     = g_currentConfig;
                    leadMs          = localConfig.notifyLeadMs;
                    g_eventsUpdated = false;
                    mutedKeys.clear();
                    for (const auto& kv : g_mutedEvents)
                        mutedKeys.insert(kv.first);
                    pruneNotifiedSet(notifiedSet, localEvents);
                    continue;
                }
                if (g_shutdownRequested) break;
                nowUtc = getCurrentUtcISO();
            }

            // 発火対象の (datetime, leadMsVal) を特定
            std::string targetDatetime;
            long long   targetLeadMs = 0;
            selectFireTarget(localEvents, nowUtc, notifiedSet, mutedKeys, leadMs,
                targetDatetime, targetLeadMs);
            if (targetDatetime.empty()) continue; // 発火対象なし（稀なケース）

            // 同 datetime かつ同 targetLeadMs のイベントをグループ化
            std::vector<const CalendarEvent*> group;
            for (const auto& e : localEvents) {
                if (e.datetime != targetDatetime) continue;
                // notify_minutes タイミングか、reminders に含まれるタイミングかをチェック
                bool isTarget = (targetLeadMs == leadMs);
                if (!isTarget) {
                    for (int m : e.reminderMinutes)
                        if (static_cast<long long>(m) * 60000 == targetLeadMs) { isTarget = true; break; }
                }
                if (!isTarget) continue;
                if (mutedKeys.count(eventKey(e))) continue;
                auto key = notifyKey(e, targetLeadMs);
                if (!notifiedSet.count(key)) group.push_back(&e);
            }

            fireNotificationGroup(group, targetDatetime, targetLeadMs, localConfig, notifiedSet);
            g_forcePoll.store(true);
            writeLog("notification fired, requesting poll");
        }
    }

    // シャットダウン前に通知音スレッドの完了を待機（ダッキング復元を保証）
    // g_shutdownRequested == true なので playWavToWasapi がすみやかに停止するはず
    if (g_soundThread) {
        DWORD r = WaitForSingleObject(g_soundThread, 5000);
        if (r != WAIT_TIMEOUT) {
            CloseHandle(g_soundThread);
            g_soundThread = nullptr;
        }
        else {
            // タイムアウト時はハンドルを閉じない（走行中スレッドが COM/WASAPI を使用中のため）
            writeLog("notifyThreadFunc: sound thread did not finish within 5s on shutdown");
        }
    }

    winrt::uninit_apartment();
}

// ==================== エントリポイント ====================

// ネットワークインターフェース変化コールバック
//
// MibAddInstance（新規追加）と MibParameterNotification（パラメータ変更）で発火する。
// MibParameterNotification はルーティング変更等でも頻発するが、60 秒クールダウン期間中の
// トリガーはポーリングループでスキップされる（クールダウン後に 1 回ポーリングが走る）。
// MibDeleteInstance（切断）は無視する。後続の MibAddInstance で対応されるため不要。
// ※ システムスレッドプールから呼ばれるため、ここでは atomic 操作のみ行う。
static VOID WINAPI onNetworkChange(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE type) {
    if (type != MibAddInstance && type != MibParameterNotification) return;
    g_forcePoll.store(true);
}

// Calendar API クエリパラメータの構築
//
// timeMin に現在時刻、timeMax に「JST 翌日 23:59:59」を設定して
// 当日と翌日の予定をまとめて取得するためのクエリ文字列を返す。
static std::wstring buildCalendarQueryParams(const SYSTEMTIME& utcNow) {
    auto nowUtc = systemTimeToIso(utcNow) + ".000Z";
    SYSTEMTIME jstMidnight = utcToJst(utcNow);
    jstMidnight.wHour = jstMidnight.wMinute = jstMidnight.wSecond = jstMidnight.wMilliseconds = 0;
    auto tomorrowEndJst = shiftSystemTime(jstMidnight, 2LL * 24 * 60 * 60 * 10'000'000LL - 10'000'000LL);
    auto tomorrowEndUtc = jstToUtc(tomorrowEndJst);
    auto endUtc = systemTimeToIso(tomorrowEndUtc) + ".000Z";

    std::wstring queryParams = L"?singleEvents=true&orderBy=startTime&maxResults=50";
    queryParams += L"&fields=items(id,summary,start,htmlLink,eventType,status,attendees(self,responseStatus),reminders)";
    queryParams += L"&timeMin=" + toWide(urlEncode(nowUtc));
    queryParams += L"&timeMax=" + toWide(urlEncode(endUtc));
    return queryParams;
}

// 全カレンダーのイベント取得
//
// primary と ext_calendar_ids の各カレンダーに対して Calendar API を呼び、
// 取得したイベントを events に追加する。401 を検出した場合は
// アクセストークンをクリアしてリフレッシュ後に 1 回だけリトライする。
// outAnySuccess: 1 件以上取得できれば true。
// outAuthFailed: リフレッシュ後も認証エラーで全停止する場合に true。
//
// ※ ID プレフィックス付与（"<calId>/<eventId>"）はこの関数内で行う。
// カレンダー ID をまたいだ ID 衝突防止のため、events 取得直後にカレンダー ID を
// 付与する責務をここに集約する。後段の deliverPollResults 等は付与済み ID を前提とする。
static void fetchAllCalendarEvents(
    const std::vector<std::string>& calendarIds,
    const std::wstring& queryParams,
    std::vector<CalendarEvent>& events,
    bool& outAnySuccess,
    bool& outAuthFailed)
{
    outAnySuccess = false;
    outAuthFailed = false;

    for (const auto& calId : calendarIds) {
        std::wstring calUrl = L"https://";
        calUrl += CALENDAR_API_HOST;
        calUrl += L"/calendar/v3/calendars/" + toWide(urlEncode(calId)) + L"/events";
        calUrl += queryParams;

        DWORD httpStatus = 0;
        std::wstring tokenSnapshot;
        {
            std::lock_guard<std::mutex> lk(g_tokenMtx);
            tokenSnapshot = g_accessToken;
        }
        auto body = httpGet(calUrl, tokenSnapshot, &httpStatus);

        // 401: アクセストークン失効 → リフレッシュしてリトライ（非対話）
        if (httpStatus == 401) {
            writeLog("access token expired (401), refreshing...");
            {
                std::lock_guard<std::mutex> lk(g_tokenMtx);
                g_accessToken.clear();
                g_tokenExpiry = {};
            }
            auto rr = tryRefreshAccessToken();
            if (rr != RefreshResult::Ok) {
                if (rr == RefreshResult::AuthRequired) notifyAuthRequired();
                outAuthFailed = true;
                return;
            }
            {
                std::lock_guard<std::mutex> lk(g_tokenMtx);
                tokenSnapshot = g_accessToken;
            }
            body = httpGet(calUrl, tokenSnapshot, &httpStatus);
        }

        if (body.empty()) {
            writeLog("poll: calendar " + calId + " failed"
                + (httpStatus != 0 ? " (status " + std::to_string(httpStatus) + ")" : ""));
            continue;
        }

        auto [calEvents, errorMsg] = parseCalendarEvents(body);
        if (!errorMsg.empty()) {
            writeLog("poll: calendar " + calId + ": " + errorMsg);
            continue;
        }

        // カレンダー ID をプレフィックスとして付与（カレンダー間の ID 衝突を防ぐ）
        for (auto& ev : calEvents) {
            if (!ev.id.empty()) ev.id = calId + "/" + ev.id;
        }
        events.insert(events.end(), calEvents.begin(), calEvents.end());
        outAnySuccess = true;
    }
}

// ポーリング結果の引き渡しと変更検知
//
// 取得したイベントを通知スレッドへ受け渡し、当日分について
// ベースラインからの差分を検出して Toast 通知する。
// キャッシュファイル更新とトレイのツールチップ更新もここで実行する。
static void deliverPollResults(
    const std::wstring& exeDir,
    std::vector<CalendarEvent> events,
    const SYSTEMTIME& jstNow,
    bool baselineEstablished)
{
    std::vector<CalendarEvent> prevEvents;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        prevEvents      = std::move(g_pendingEvents);
        g_pendingEvents = events;
        g_eventsUpdated = true;
    }
    g_cv.notify_one();

    // 変更検知は当日の予定のみを対象とする
    // 翌日分を含めると日付変更時にポーリングウィンドウの変化で誤検知する
    auto isToday = [&](const CalendarEvent& e) {
        auto jst = utcIsoToJstSt(e.datetime);
        return jst && jst->wYear == jstNow.wYear
                   && jst->wMonth == jstNow.wMonth
                   && jst->wDay == jstNow.wDay;
    };
    std::vector<CalendarEvent> todayPrev, todayNew;
    for (const auto& e : prevEvents) {
        if (isToday(e)) todayPrev.push_back(e);
    }
    for (const auto& e : events) {
        if (isToday(e)) todayNew.push_back(e);
    }

    std::vector<EventChange> changes;
    if (baselineEstablished) {
        changes = collectEventChanges(todayPrev, todayNew);
    }
    notifyEventChanges(changes);
    saveCacheFile(exeDir, events);
    if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
}

// ポーリングスレッド本体
//
// メインスレッドからポーリング処理（HTTP I/O）を分離し、UI（右クリックメニュー等）の
// 応答性をネットワーク状態に依存させないことが目的。
// 実行内容：トークンリフレッシュ → Calendar API ポーリング → 結果を通知スレッドへ受け渡し。
// 中断は g_shutdownRequested の atomic フラグ経由（waitInterruptible が 100 ms 単位で監視）。
static void pollThreadFunc(std::wstring exeDir, Config cfg) {
    // WinRT アパートメント初期化
    // 本スレッドは認証失効・接続エラーの Toast 表示経路（showErrorToast / notifyAuthRequired）を
    // 持つため、WinRT 呼び出しに先立ってアパートメントを初期化する。
    winrt::init_apartment();

    int  lastJstDay          = -1;
    bool firstPoll           = true;  // 起動時は schedule に関わらず必ず1回ポーリング
    bool baselineEstablished = false; // 変更検知ベースラインが確立済みか

    while (!g_shutdownRequested) {
        try {
            // 対話的認証フロー実行中はトークン操作を一切行わない。
            // これにより g_accessToken / g_tokenExpiry の data race と
            // notifyAuthRequired の TOCTOU を回避する。
            if (g_authInProgress.load()) {
                waitInterruptible(RETRY_WAIT_MS);
                continue;
            }

            // 即時ポーリング判定（forcePoll フラグ or 1 時間以上未ポーリング）
            bool forceTriggered = g_forcePoll.exchange(false);
            ULONGLONG tickNow   = GetTickCount64();
            ULONGLONG lastTick  = g_lastPollTick.load();
            bool stale = (lastTick > 0) && (tickNow - lastTick >= STALE_POLL_THRESHOLD_MS);

            if ((forceTriggered || stale) && !firstPoll) {
                if (lastTick > 0 && (tickNow - lastTick < FORCE_POLL_COOLDOWN_MS)) {
                    if (forceTriggered) writeLog("force poll skipped (cooldown)");
                }
                else {
                    if (forceTriggered) writeLog("force poll triggered");
                    if (stale) writeLog("stale poll triggered (" + std::to_string((tickNow - lastTick) / 1000) + "s since last poll)");
                    firstPoll = true;
                }
            }

            SYSTEMTIME utcNow;
            GetSystemTime(&utcNow);
            auto jstNow = utcToJst(utcNow);

            // 日付変更: 強制ポーリングと変更検知ベースラインをリセットする
            // notifiedSet は通知スレッドが自然失効で管理する
            if (static_cast<int>(jstNow.wDay) != lastJstDay) {
                lastJstDay          = static_cast<int>(jstNow.wDay);
                firstPoll           = true;
                baselineEstablished = false;
            }

            int pollsPerHour = cfg.schedule[jstNow.wHour];

            // アクセストークン確保（非対話）。認証失敗時もブラウザは自動起動しない。
            {
                auto rr = tryRefreshAccessToken();
                if (rr == RefreshResult::NetworkError) {
                    // ネットワーク不通は接続エラー扱い。認証 Toast は出さない。
                    // 後段の Calendar API 呼び出しでも失敗するため、そちらの「接続エラー」Toast に任せる。
                    waitInterruptible(RETRY_WAIT_MS);
                    continue;
                }
                if (rr == RefreshResult::AuthRequired) {
                    notifyAuthRequired();  // Toast 表示（クールダウンつき）。ブラウザは開かない。
                    waitInterruptible(RETRY_WAIT_MS);
                    continue;
                }
                g_authRequired.store(false);  // 認証復旧時にフラグをクリア（tooltip も次更新で通常表示へ）
            }

            // Calendar API v3 クエリパラメータ（全カレンダー共通）
            auto queryParams = buildCalendarQueryParams(utcNow);

            // ポーリング対象カレンダー（primary + ext_calendar_ids）
            std::vector<std::string> calendarIds = {"primary"};
            for (const auto& id : cfg.extCalendarIds) calendarIds.push_back(id);

            std::vector<CalendarEvent> events;
            bool anySuccess = false;
            bool authFailed = false;
            ULONGLONG t0    = GetTickCount64();

            fetchAllCalendarEvents(calendarIds, queryParams, events, anySuccess, authFailed);
            ULONGLONG elapsed = GetTickCount64() - t0;

            if (authFailed) {
                // notifyAuthRequired で認証 Toast、または NetworkError 扱いで通知済み
                waitInterruptible(RETRY_WAIT_MS);
                continue;
            }

            if (!anySuccess) {
                writeLog("HTTP request failed");
                showErrorToast(L"接続エラー", L"Google Calendar API に接続できません");
                waitInterruptible(RETRY_WAIT_MS);
                continue;
            }

            // 複数カレンダーのマージ結果を開始時刻でソート
            std::sort(events.begin(), events.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
                return a.datetime < b.datetime;
            });

            g_lastErrorToastTime.store(0);
            writeLog("poll: " + std::to_string(events.size()) + " events ("
                + std::to_string(elapsed) + "ms), next: " + nextPollTimeStr(pollsPerHour));

            // ポーリング結果を通知スレッドへ渡す
            deliverPollResults(exeDir, events, jstNow, baselineEstablished);

            firstPoll           = false;
            baselineEstablished = true;
            g_lastPollTick.store(GetTickCount64());
            waitInterruptible(calcSleepUntilNextPoll(pollsPerHour));
        }
        catch (...) {
            writeLog("unexpected error in polling loop");
            waitInterruptible(RETRY_WAIT_MS);
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
    HANDLE hJob = CreateJobObjectW(nullptr, L"Local\\gcalntfy_job");
    if (hJob && GetLastError() == ERROR_ALREADY_EXISTS) {
        writeLog("terminating previous instance");
        TerminateJobObject(hJob, 0);
        CloseHandle(hJob);
        // カーネルが Job Object 名を解放するまで待機
        Sleep(100);
        hJob = CreateJobObjectW(nullptr, L"Local\\gcalntfy_job");
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
        g_currentConfig = cfg;  // 通知スレッドへの初期設定（起動時のみ）

        // 通知音を読み込みノーマライズしてキャッシュに格納（以降の再生はキャッシュを使用）
        loadWavAndNormalize(exeDir, cfg);

        addTrayIcon(g_hWnd);

        // レジストリから設定を復元（キー未作成時はデフォルト値）
        g_soundEnabled  = readRegDword(REG_SOUND_ENABLED, 1u) != 0;
        g_muteInMeeting = readRegDword(REG_MUTE_IN_MEETING, 1u) != 0;

        writeLog("started");
        logSchedule(cfg.schedule);

        // 更新チェックスレッド起動（起動時に 1 回のみ実行、detach で分離）
        if (cfg.updateCheckEnabled) {
            try {
                std::thread(checkForUpdates).detach();
            }
            catch (const std::system_error& e) {
                writeLog(std::string("failed to start update check thread: ") + e.what());
            }
        }

        // 通知スレッド起動
        std::thread notifyThread(notifyThreadFunc, exeDir);

        // 通知抑制リストを復元（過去分のエントリは自動プルーニング）
        loadMutedEvents(exeDir);

        // メニュー描画用フォントを初期化（以降、WM_MEASUREITEM / WM_DRAWITEM で使用する）
        initMenuFonts();

        // キャッシュからイベントデータを復元（起動直後のポーリング失敗に備える）
        auto cachedEvents = loadCacheFile(exeDir);
        if (!cachedEvents.empty()) {
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_pendingEvents = cachedEvents;
                g_eventsUpdated = true;
            }
            // g_eventsUpdated = true が条件変数の述語になっているため、
            // notify_one が通知スレッドの wait 到達前に呼ばれても次の wait 時に述語が true で即解放される
            g_cv.notify_one();
            if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
            writeLog("cache: loaded " + std::to_string(cachedEvents.size()) + " events from cache");
        }

        // ポーリングスレッド起動
        // メインスレッドはメッセージループに専念させるため、Calendar API ポーリング（HTTP I/O）を別スレッドへ分離する。
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

        // バックグラウンドスレッドを停止
        // 通知スレッドは条件変数で待機中の可能性があるため notify_one で起こす
        g_cv.notify_one();
        pollThread.join();
        notifyThread.join();

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
