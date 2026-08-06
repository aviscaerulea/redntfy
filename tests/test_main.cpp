// redntfy 単体テスト（自前ミニハーネス）
//
// src/main.cpp を丸ごとインクルードし、単一翻訳単位の static 関数へ直接アクセスする。
// （プロダクションコードを変更せず、単一翻訳単位の設計も崩さないため）
// エントリは本ファイルの main。リンカに /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup を
// 明示することで、main.cpp 側の wmain は未使用シンボルとして共存する。
// テストは時刻・環境に依存させない。「今日」を取る関数は直接テストせず、
// 基準日を引数で受ける下位関数へ固定日付を渡す。
#include "../src/main.cpp"

// 検査数と失敗数（main が集計を出力し、失敗ありなら終了コード 1）
static int g_checks = 0;
static int g_fails  = 0;

// 条件検査（失敗時に file:line と式を出力して続行する）
#define CHECK(cond) do { \
    ++g_checks; \
    if (!(cond)) { ++g_fails; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// wstring の一致検査（失敗時に実際値と期待値を UTF-8 で出力する）
static void checkWstr(const char* file, int line,
                      const std::wstring& actual, const std::wstring& expected) {
    ++g_checks;
    if (actual != expected) {
        ++g_fails;
        printf("FAIL %s:%d\n  actual  : %s\n  expected: %s\n",
               file, line, wideToUtf8(actual).c_str(), wideToUtf8(expected).c_str());
    }
}
#define CHECK_WSTR(actual, expected) checkWstr(__FILE__, __LINE__, (actual), (expected))

// UTC の年月日時分秒から JST 通算日を得る（テストの基準日を固定するため）
static long long dayOfUtc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
    SYSTEMTIME st = {};
    st.wYear   = static_cast<WORD>(y);
    st.wMonth  = static_cast<WORD>(mo);
    st.wDay    = static_cast<WORD>(d);
    st.wHour   = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    st.wSecond = static_cast<WORD>(s);
    return jstDaySerial(st);
}

// ==================== parseListFormat ====================

static void testParseListFormat() {
    // 空文字は空列（既定テンプレートへの置換は loadConfig 側の責務）
    CHECK(parseListFormat(L"").empty());

    // プレースホルダなしはリテラル 1 個
    {
        auto t = parseListFormat(L"abc");
        CHECK(t.size() == 1);
        CHECK(t[0].element == FMT_LITERAL);
        CHECK_WSTR(t[0].literal, L"abc");
    }

    // 既定テンプレートの全トークン（並び・要素・最大文字数）
    {
        auto t = parseListFormat(LIST_FORMAT_DEFAULT);
        CHECK(t.size() == 14);
        if (t.size() == 14) {
            CHECK(t[0].element == FMT_LITERAL);   CHECK_WSTR(t[0].literal, L"#");
            CHECK(t[1].element == FMT_ID);        CHECK(t[1].maxChars == 0);
            CHECK(t[2].element == FMT_LITERAL);   CHECK_WSTR(t[2].literal, L"  ");
            CHECK(t[3].element == FMT_LASTNAME);
            CHECK(t[4].element == FMT_LITERAL);   CHECK_WSTR(t[4].literal, L"  ");
            CHECK(t[5].element == FMT_GROUP);
            CHECK(t[6].element == FMT_LITERAL);   CHECK_WSTR(t[6].literal, L"[");
            CHECK(t[7].element == FMT_PROJECT);   CHECK(t[7].maxChars == 5);
            CHECK(t[8].element == FMT_LITERAL);   CHECK_WSTR(t[8].literal, L"] ");
            CHECK(t[9].element == FMT_DUE);
            CHECK(t[10].element == FMT_LITERAL);  CHECK_WSTR(t[10].literal, L" ");
            CHECK(t[11].element == FMT_BUG);
            CHECK(t[12].element == FMT_SUBJECT);  CHECK(t[12].maxChars == 40);
            CHECK(t[13].element == FMT_AGO);      CHECK(t[13].maxChars == 0);
        }
    }

    // 長さ指定の正常系
    {
        auto t = parseListFormat(L"{subject:40}");
        CHECK(t.size() == 1);
        CHECK(t[0].element == FMT_SUBJECT);
        CHECK(t[0].maxChars == 40);
    }

    // 不正な長さ指定はトークン全体をリテラル原文として残す
    for (const wchar_t* bad : { L"{subject:0}", L"{subject:abc}", L"{subject:}", L"{subject:12345}" }) {
        auto t = parseListFormat(bad);
        CHECK(t.size() == 1);
        CHECK(t[0].element == FMT_LITERAL);
        CHECK_WSTR(t[0].literal, bad);
    }

    // 未知の要素名・閉じ括弧なしもリテラル原文
    {
        auto t = parseListFormat(L"{foo}");
        CHECK(t.size() == 1 && t[0].element == FMT_LITERAL);
        CHECK_WSTR(t[0].literal, L"{foo}");
    }
    {
        auto t = parseListFormat(L"{subject");
        CHECK(t.size() == 1 && t[0].element == FMT_LITERAL);
        CHECK_WSTR(t[0].literal, L"{subject");
    }

    // リテラルと要素の混在
    {
        auto t = parseListFormat(L"a{id}b");
        CHECK(t.size() == 3);
        if (t.size() == 3) {
            CHECK_WSTR(t[0].literal, L"a");
            CHECK(t[1].element == FMT_ID);
            CHECK_WSTR(t[2].literal, L"b");
        }
    }
}

// ==================== buildIssueLabel ====================

static void testBuildIssueLabel() {
    // 基準日は 2026-08-03（JST）固定。UTC 0:00 は JST 9:00 で同日
    const long long today     = dayOfUtc(2026, 8, 3);
    const int       todayYmd  = 20260803;

    auto makeRow = [] {
        ListRow row;
        row.id              = 12345;
        row.subject         = "テスト件名";
        row.projectName     = "ロケモニプロジェクト";
        row.updater         = "山田";
        row.updaterFirst    = "太郎";
        row.dueDate         = "2026-07-28";
        row.updatedOn       = "2026-07-31T12:00:00Z";  // 基準日から 3 日前
        row.assignedToGroup = true;
        row.isBugTracker    = true;
        return row;
    };

    // 全要素あり＋既定テンプレート：従来の固定並びと一致すること
    g_currentConfig.listFormat = parseListFormat(LIST_FORMAT_DEFAULT);
    {
        auto row = makeRow();
        auto lbl = buildIssueLabel(row, makeDueDateView(row.dueDate, todayYmd), today);
        CHECK_WSTR(lbl.text, L"#12345  山田  👥 [ロケモニプ] 7/28 💥 テスト件名（3 日前）");
        // ranges は期日（半太字・期限切れ赤）と 💥（赤、末尾空白を含まない）の 2 件で昇順
        CHECK(lbl.ranges.size() == 2);
        if (lbl.ranges.size() == 2) {
            CHECK(lbl.ranges[0].offset == 23);  // "#12345  山田  👥 [ロケモニプ] " の直後
            CHECK(lbl.ranges[0].len == 4);      // "7/28"
            CHECK(lbl.ranges[0].bold);
            CHECK(!lbl.ranges[0].keepColor);    // 期限切れ（7/28 ≦ 8/3）は赤
            CHECK(lbl.ranges[1].offset == 28);  // "7/28 " の直後
            CHECK(lbl.ranges[1].len == 2);      // 💥（サロゲートペア）
            CHECK(lbl.ranges[1].color == ALERT_TEXT_COLOR);
            CHECK(lbl.ranges[0].offset < lbl.ranges[1].offset);
        }
    }

    // 期日なし：前後のリテラル空白が詰まる
    {
        auto row = makeRow();
        row.dueDate.clear();
        auto lbl = buildIssueLabel(row, makeDueDateView(row.dueDate, todayYmd), today);
        CHECK_WSTR(lbl.text, L"#12345  山田  👥 [ロケモニプ] 💥 テスト件名（3 日前）");
    }

    // 更新者空：番号の後の空白が二重にならない
    {
        auto row = makeRow();
        row.updater.clear();
        auto lbl = buildIssueLabel(row, makeDueDateView(row.dueDate, todayYmd), today);
        CHECK_WSTR(lbl.text, L"#12345  👥 [ロケモニプ] 7/28 💥 テスト件名（3 日前）");
    }

    // updatedOn 空（旧形式ピン相当）：経過日数が付かない
    {
        auto row = makeRow();
        row.updatedOn.clear();
        auto lbl = buildIssueLabel(row, makeDueDateView(row.dueDate, todayYmd), today);
        CHECK_WSTR(lbl.text, L"#12345  山田  👥 [ロケモニプ] 7/28 💥 テスト件名");
    }

    // 期日が未来：半太字のまま色は据え置き（keepColor）
    {
        auto row = makeRow();
        row.dueDate = "2026-08-04";
        auto lbl = buildIssueLabel(row, makeDueDateView(row.dueDate, todayYmd), today);
        CHECK(lbl.ranges.size() == 2);
        if (!lbl.ranges.empty()) {
            CHECK(lbl.ranges[0].bold);
            CHECK(lbl.ranges[0].keepColor);
        }
    }

    // 件名の切り詰め：{subject:5} で 5 文字＋「…」
    g_currentConfig.listFormat = parseListFormat(L"{subject:5}");
    {
        auto row = makeRow();
        row.subject = "あいうえおかきく";
        auto lbl = buildIssueLabel(row, makeDueDateView("", todayYmd), today);
        CHECK_WSTR(lbl.text, L"あいうえお…");
    }

    // 行末のリテラル空白は残らない。（{id} は番号のみで「#」はリテラル側の責務）
    g_currentConfig.listFormat = parseListFormat(L"{id} ");
    {
        auto row = makeRow();
        auto lbl = buildIssueLabel(row, makeDueDateView("", todayYmd), today);
        CHECK_WSTR(lbl.text, L"12345");
    }

    // {firstname}：姓名の分離表示
    g_currentConfig.listFormat = parseListFormat(L"{lastname}{firstname}");
    {
        auto row = makeRow();
        auto lbl = buildIssueLabel(row, makeDueDateView("", todayYmd), today);
        CHECK_WSTR(lbl.text, L"山田太郎");
    }

    // 後始末：既定テンプレートへ戻す（他テストへの影響防止）
    g_currentConfig.listFormat = parseListFormat(LIST_FORMAT_DEFAULT);
}

// ==================== makeUpdatedAgoText ====================

static void testMakeUpdatedAgoText() {
    const long long today = dayOfUtc(2026, 8, 3);  // JST 2026-08-03

    // 同日と JST 日境界（UTC 15:00 = JST 翌日 0:00）
    CHECK_WSTR(makeUpdatedAgoText("2026-08-03T00:00:00Z", today), L"（今日）");
    CHECK_WSTR(makeUpdatedAgoText("2026-08-02T15:00:00Z", today), L"（今日）");
    CHECK_WSTR(makeUpdatedAgoText("2026-08-02T14:59:59Z", today), L"（昨日）");

    // 日数表示と未来の丸め
    CHECK_WSTR(makeUpdatedAgoText("2026-07-31T12:00:00Z", today), L"（3 日前）");
    CHECK_WSTR(makeUpdatedAgoText("2026-08-04T00:00:00Z", today), L"（今日）");

    // 解釈不能は空（何も表示しない）
    CHECK_WSTR(makeUpdatedAgoText("", today), L"");
    CHECK_WSTR(makeUpdatedAgoText("garbage", today), L"");
    CHECK_WSTR(makeUpdatedAgoText("2026-02-31T00:00:00Z", today), L"");  // 実在しない日付
}

// ==================== makeDueDateView ====================

static void testMakeDueDateView() {
    const int todayYmd = 20260803;

    // 今年は月日のみ、期限切れは当日を含む
    {
        auto v = makeDueDateView("2026-07-28", todayYmd);
        CHECK_WSTR(v.text, L"7/28");
        CHECK(v.overdue);
    }
    {
        auto v = makeDueDateView("2026-08-03", todayYmd);
        CHECK(v.overdue);  // 当日は期限切れ扱い
    }
    {
        auto v = makeDueDateView("2026-08-04", todayYmd);
        CHECK_WSTR(v.text, L"8/4");
        CHECK(!v.overdue);
    }

    // 他年は年付き
    CHECK_WSTR(makeDueDateView("2025-06-30", todayYmd).text, L"2025/6/30");

    // 解釈不能は空
    CHECK_WSTR(makeDueDateView("", todayYmd).text, L"");
    CHECK_WSTR(makeDueDateView("abc", todayYmd).text, L"");
}

// ==================== jstDaySerial ====================

static void testJstDaySerial() {
    // UTC 15:00 が JST の日境界（+9 時間で翌日 0:00）
    CHECK(dayOfUtc(2026, 8, 2, 14, 59, 59) + 1 == dayOfUtc(2026, 8, 2, 15, 0, 0));
    CHECK(dayOfUtc(2026, 8, 2, 15, 0, 0) == dayOfUtc(2026, 8, 3, 0, 0, 0));
    // 月またぎの差もカレンダー日数どおり
    CHECK(dayOfUtc(2026, 8, 1) - dayOfUtc(2026, 7, 31) == 1);
}

// ==================== truncateText ====================

static void testTruncateText() {
    CHECK_WSTR(truncateText(L"abc", 3), L"abc");           // ちょうどは切らない
    CHECK_WSTR(truncateText(L"ab", 3), L"ab");             // 未満は素通し
    CHECK_WSTR(truncateText(L"abcd", 3), L"abc…");         // 超過は切り詰め＋…
    CHECK_WSTR(truncateText(L"abcd", 3, false), L"abc");   // ellipsis=false は…なし

    // サロゲートペア（😀 = 2 コードユニット）の途中で切らない
    std::wstring s = L"ab\U0001F600cd";  // a b 😀(2) c d = 6 units
    CHECK_WSTR(truncateText(s, 3), L"ab…");     // 上位サロゲート終わりは 1 つ手前で切る
    CHECK_WSTR(truncateText(s, 4), L"ab\U0001F600…");
}

// ==================== matchWildcard ====================

static void testMatchWildcard() {
    CHECK(matchWildcard("abc", "abc"));
    CHECK(matchWildcard("abc", "a*"));
    CHECK(matchWildcard("abc", "*c"));
    CHECK(matchWildcard("abc", "*b*"));
    CHECK(matchWildcard("abc", "*"));
    CHECK(!matchWildcard("abc", "x*"));
    CHECK(!matchWildcard("abc", ""));
    CHECK(matchWildcard("", "*"));
    // UTF-8 マルチバイト（`*` は継続バイトに現れないため文字境界を跨がない）
    CHECK(matchWildcard("重大バグ修正", "*バグ*"));
    CHECK(!matchWildcard("機能追加", "*バグ*"));
}

// ==================== parseVersion / isNewerVersion ====================

static void testVersionCompare() {
    // 出力引数は検査ごとにリセットする。（前の検査の書き込みが残ると恒真になり回帰を見逃す）
    int ma = 0, mi = 0, pa = 0;
    CHECK(parseVersion(L"v2.7.4", ma, mi, pa) && ma == 2 && mi == 7 && pa == 4);
    ma = mi = pa = 0;
    CHECK(parseVersion(L"2.7.4-dirty", ma, mi, pa) && ma == 2 && mi == 7 && pa == 4);
    ma = mi = pa = 0;
    CHECK(parseVersion(L"2.7.4-5-gABC", ma, mi, pa) && ma == 2 && mi == 7 && pa == 4);
    CHECK(!parseVersion(L"abc", ma, mi, pa));

    CHECK(isNewerVersion(L"v2.0.0", L"v1.9.9"));
    CHECK(isNewerVersion(L"v1.10.0", L"v1.9.0"));
    CHECK(isNewerVersion(L"v1.0.10", L"v1.0.9"));
    CHECK(!isNewerVersion(L"v1.0.0", L"v1.0.0"));
    CHECK(!isNewerVersion(L"v1.9.9", L"v2.0.0"));
    CHECK(!isNewerVersion(L"abc", L"v1.0.0"));  // 解釈不能は「新しくない」へ倒す
}

// ==================== escapeXml / isHttpUrl / normalizeSpaces ====================

static void testEscapeXml() {
    CHECK_WSTR(escapeXml(L"a&b<c>d\"e"), L"a&amp;b&lt;c&gt;d&quot;e");
    CHECK_WSTR(escapeXml(L"plain"), L"plain");
}

static void testIsHttpUrl() {
    CHECK(isHttpUrl(L"https://example.com"));
    CHECK(isHttpUrl(L"http://example.com"));
    CHECK(!isHttpUrl(L"ftp://example.com"));
    CHECK(!isHttpUrl(L""));
    CHECK(!isHttpUrl(L"example.com"));
    CHECK(!isHttpUrl(L"HTTPS://example.com"));  // 大文字スキームは不許可（starts_with は大小区別）
}

static void testNormalizeSpaces() {
    CHECK(normalizeSpaces("あ\xE3\x80\x80い") == "あ い");
    CHECK(normalizeSpaces("あ\xE3\x80\x80\xE3\x80\x80い") == "あ  い");
    CHECK(normalizeSpaces("\xE3\x80\x80" "ab" "\xE3\x80\x80") == " ab ");
    CHECK(normalizeSpaces("no wide") == "no wide");
}

// ==================== toWide / wideToUtf8 ====================

static void testStringConversion() {
    // 往復変換（ASCII・日本語・サロゲートペア）
    CHECK_WSTR(toWide("abc"), L"abc");
    CHECK(wideToUtf8(L"abc") == "abc");
    CHECK_WSTR(toWide(wideToUtf8(L"日本語テスト")), L"日本語テスト");
    CHECK_WSTR(toWide(wideToUtf8(L"a\U0001F600b")), L"a\U0001F600b");

    // 空入力は空を返す
    CHECK(toWide("").empty());
    CHECK(wideToUtf8(L"").empty());

    // 不正な UTF-8 バイト列でもクラッシュ・巨大確保せず安全に返る。
    // （CP_UTF8 の既定動作では不正シーケンスは U+FFFD に置換される。
    // 変換 API が 0 を返す環境・入力でも n - 1 のアンダーフローで落ちないことが契約）
    std::string broken = "\xFF\xFE\x80";
    auto w = toWide(broken);
    CHECK(w.size() <= broken.size());
}

// ==================== issuesFilterQuery / trackedQueryIds / queryUrl ====================

static void testIssuesFilterQuery() {
    CHECK_WSTR(issuesFilterQuery(12), L"query_id=12");
    // フォールバックモード：me は Redmine がサーバ側で自分＋所属グループに展開する
    CHECK_WSTR(issuesFilterQuery(FALLBACK_QUERY_ID), L"assigned_to_id=me");
}

static void testTrackedQueryIds() {
    Config cfg;
    cfg.queryIds = {12, 34};
    CHECK(trackedQueryIds(cfg) == (std::vector<int>{12, 34}));
    // 省略時は擬似クエリ 1 件
    cfg.queryIds.clear();
    CHECK(trackedQueryIds(cfg) == (std::vector<int>{FALLBACK_QUERY_ID}));
}

static void testQueryUrl() {
    Config cfg;
    cfg.redmineUrl = L"https://r.example";
    cfg.queryIds = {12, 34};
    CHECK_WSTR(queryUrl(cfg), L"https://r.example/issues?query_id=12");
    // フォールバックモードは担当フィルタ付き一覧
    cfg.queryIds.clear();
    CHECK_WSTR(queryUrl(cfg), L"https://r.example/issues?set_filter=1&assigned_to_id=me");
}

// ==================== passesVersionFilter ====================

static void testPassesVersionFilter() {
    Issue is;
    is.hasFixedVersion = false;
    is.dueDate.clear();
    is.hasVersionField = true;

    // フィルタ OFF は常に通す
    g_excludeNoVersion.store(false);
    CHECK(passesVersionFilter(is));

    // フィルタ ON：バージョンなし・期日なし・欄ありは弾く
    g_excludeNoVersion.store(true);
    CHECK(!passesVersionFilter(is));

    // バージョンあり・期日あり・欄なしのいずれかで通す
    is.hasFixedVersion = true;
    CHECK(passesVersionFilter(is));
    is.hasFixedVersion = false;
    is.dueDate = "2026-08-03";
    CHECK(passesVersionFilter(is));
    is.dueDate.clear();
    is.hasVersionField = false;
    CHECK(passesVersionFilter(is));

    // 後始末（他テストへの影響防止）
    g_excludeNoVersion.store(false);
}

// ==================== selectNotifyTargets ====================

// 通知対象選定の検証用に前回状態を組み立てる（baseline 確立済み・クエリ 1 追跡）
static PollState makePrevState() {
    PollState prev;
    prev.baseline     = true;
    prev.knownQueries = {1};
    prev.polledOn     = "2026-08-01T00:00:00Z";
    return prev;
}

// 検証用チケットを組み立てる（クエリ 1 所属・prev.polledOn より新しい更新）
static Issue makeIssue(int id, const std::string& updatedOn = "2026-08-02T00:00:00Z") {
    Issue is;
    is.id        = id;
    is.updatedOn = updatedOn;
    is.queryIds  = {1};
    return is;
}

static void testSelectNotifyTargets() {
    const int ME = 5;

    // 新規流入は New として通知し、直近更新の更新者名を添える
    {
        auto prev = makePrevState();
        auto is   = makeIssue(101);
        is.updaterId   = 9;
        is.updaterName = "山田太郎";
        // NotifyTarget::issue は渡した vector 内を指すため、一時 vector を渡すと
        // 戻り値のポインタが即座にダングリングになる。必ず名前付きローカルで渡す（以下同様）
        std::vector<Issue> issues{is};
        auto r = selectNotifyTargets(issues, prev, true, ME);
        CHECK(r && r->size() == 1);
        CHECK((*r)[0].kind == NotifyKind::New);
        CHECK((*r)[0].updaterName == "山田太郎");
    }
    // 時間経過による流入（prev.polledOn より古い更新）は起票者名を出す
    {
        auto prev = makePrevState();
        auto is   = makeIssue(102, "2026-07-01T00:00:00Z");
        is.authorName  = "起票者";
        is.updaterName = "更新者";
        std::vector<Issue> issues{is};
        auto r = selectNotifyTargets(issues, prev, true, ME);
        CHECK(r && r->size() == 1 && (*r)[0].updaterName == "起票者");
    }
    // 自分の起票・自分の直近更新による流入は抑止し、抑止 OFF なら通知する
    {
        auto prev = makePrevState();
        auto mine = makeIssue(103);
        mine.authorId = ME;
        auto updatedByMe = makeIssue(104);
        updatedByMe.updaterId = ME;
        std::vector<Issue> issues{mine, updatedByMe};
        auto r = selectNotifyTargets(issues, prev, true, ME);
        CHECK(r && r->empty());
        r = selectNotifyTargets(issues, prev, false, ME);
        CHECK(r && r->size() == 2);
    }
    // 前回追跡していないクエリだけに属する新規は黙って採用する（通知しない）
    {
        auto prev = makePrevState();
        auto is   = makeIssue(105);
        is.queryIds = {2};
        std::vector<Issue> issues{is};
        auto r = selectNotifyTargets(issues, prev, true, ME);
        CHECK(r && r->empty());
    }
    // 既知チケット：updated_on の進行は Updated、変化なしは通知なし
    {
        auto prev = makePrevState();
        prev.issues[106] = {"2026-08-01T12:00:00Z", {1}, true};
        auto moved  = makeIssue(106, "2026-08-02T00:00:00Z");
        moved.updaterName = "更新者";
        auto stable = makeIssue(106, "2026-08-01T12:00:00Z");
        std::vector<Issue> movedIssues{moved};
        auto r = selectNotifyTargets(movedIssues, prev, true, ME);
        CHECK(r && r->size() == 1);
        CHECK((*r)[0].kind == NotifyKind::Updated);
        CHECK((*r)[0].updaterName == "更新者");
        std::vector<Issue> stableIssues{stable};
        r = selectNotifyTargets(stableIssues, prev, true, ME);
        CHECK(r && r->empty());
    }
    // 既知チケットの新クエリ流入は QueryEntered（更新者名なし）。未追跡クエリへの流入は対象外
    {
        auto prev = makePrevState();
        prev.knownQueries = {1, 2};
        prev.issues[107]  = {"2026-08-01T12:00:00Z", {1}, true};
        auto entered = makeIssue(107, "2026-08-01T12:00:00Z");
        entered.queryIds = {1, 2};
        std::vector<Issue> issues{entered};
        auto r = selectNotifyTargets(issues, prev, true, ME);
        CHECK(r && r->size() == 1);
        CHECK((*r)[0].kind == NotifyKind::QueryEntered);
        CHECK((*r)[0].updaterName.empty());
        entered.queryIds = {1, 3};  // クエリ 3 は前回未追跡
        issues = {entered};
        r = selectNotifyTargets(issues, prev, true, ME);
        CHECK(r && r->empty());
    }
    // v1 移行（knownQueries 空）：新規は通知側に倒す
    {
        auto prev = makePrevState();
        prev.knownQueries.clear();
        std::vector<Issue> issues{makeIssue(108)};
        auto r = selectNotifyTargets(issues, prev, true, ME);
        CHECK(r && r->size() == 1 && (*r)[0].kind == NotifyKind::New);
    }
    // 非表示チケットは新規・更新とも通知しない（非表示でないチケットは通知する）
    {
        auto prev = makePrevState();
        prev.issues[110] = {"2026-08-01T12:00:00Z", {1}, true};
        std::vector<Issue> issues{makeIssue(109), makeIssue(110, "2026-08-02T00:00:00Z")};
        std::unordered_set<int> hidden{109, 110};
        auto r = selectNotifyTargets(issues, prev, true, ME, hidden);
        CHECK(r && r->empty());
        r = selectNotifyTargets(issues, prev, true, ME, {111});
        CHECK(r && r->size() == 2);
    }
}

// ==================== buildListRows（非表示チケット） ====================

static void testBuildListRowsHidden() {
    // 検証用の共有状態を組み立てる（フィルタは全て OFF・limit は十分大きく）
    auto setup = [](std::initializer_list<int> issueIds, std::initializer_list<int> hiddenIds) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_issues.clear();
        for (int id : issueIds) {
            Issue is;
            is.id        = id;
            is.updatedOn = "2026-08-0" + std::to_string(id % 9 + 1) + "T00:00:00Z";
            g_issues.push_back(is);
        }
        g_pins.clear();
        g_unreadIds = {issueIds.begin(), issueIds.end()};
        g_hiddenIds = {hiddenIds.begin(), hiddenIds.end()};
    };
    int savedLimit = g_currentConfig.listLimit;
    g_currentConfig.listLimit = 10;

    // トグル OFF：非表示行も hidden フラグ付きで出るが、件数・未読には数えない
    {
        setup({1, 2, 3}, {2});
        g_excludeHidden.store(false);
        int visible = 0;
        auto rows = buildListRows(visible);
        CHECK(rows.size() == 3);
        CHECK(visible == 2);
        int hiddenRows = 0, unreadRows = 0;
        for (const auto& r : rows) {
            if (r.hidden) ++hiddenRows;
            if (r.unread) ++unreadRows;
        }
        CHECK(hiddenRows == 1);
        CHECK(unreadRows == 2);  // 非表示行の未読は数えない
    }
    // トグル ON：非表示行は行ごと出ない（件数は OFF と変わらない）
    {
        setup({1, 2, 3}, {2});
        g_excludeHidden.store(true);
        int visible = 0;
        auto rows = buildListRows(visible);
        CHECK(rows.size() == 2);
        CHECK(visible == 2);
        for (const auto& r : rows) CHECK(r.id != 2);
    }
    // 非表示行は list_limit の予算を消費しない（通常行は常に limit 件見える）
    {
        setup({1, 2, 3, 4}, {2});
        g_excludeHidden.store(false);
        g_currentConfig.listLimit = 3;
        int visible = 0;
        auto rows = buildListRows(visible);
        CHECK(rows.size() == 4);  // 通常 3 行＋非表示 1 行
        CHECK(visible == 3);
        g_currentConfig.listLimit = 10;
    }

    // 後始末（他テストへの影響防止）
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_issues.clear();
        g_unreadIds.clear();
        g_hiddenIds.clear();
    }
    g_excludeHidden.store(false);
    g_currentConfig.listLimit = savedLimit;
}

// テストの列挙実行と集計出力（失敗ありなら終了コード 1）
int main() {
    // 失敗診断は日本語（UTF-8）を含むため、コンソールの出力コードページを合わせる
    // （既定の CP932 のままだと wideToUtf8 の出力が文字化けして actual/expected が読めない）
    SetConsoleOutputCP(CP_UTF8);
    testParseListFormat();
    testBuildIssueLabel();
    testMakeUpdatedAgoText();
    testMakeDueDateView();
    testJstDaySerial();
    testTruncateText();
    testMatchWildcard();
    testVersionCompare();
    testEscapeXml();
    testIsHttpUrl();
    testNormalizeSpaces();
    testStringConversion();
    testIssuesFilterQuery();
    testTrackedQueryIds();
    testQueryUrl();
    testPassesVersionFilter();
    testSelectNotifyTargets();
    testBuildListRowsHidden();
    printf("checks: %d, failed: %d\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
