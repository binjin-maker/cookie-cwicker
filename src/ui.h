// CookieBot main window: always-on-top script editor (tabs, line numbers, Execute/Clear/Open/Save),
// live Output, and a Settings page with the old control-panel buttons.
// It's hidden from the bot's own screenshots, and Copycat holds still while you're in it.
#include <commdlg.h>
#include <uxtheme.h>

enum { ID_PAUSE = 101, ID_QUIT, ID_BUY, ID_GOLD, ID_FX, ID_HOURS, ID_CPS, ID_APPLY, ID_FORGET, ID_LOG, ID_STATUS, ID_STATS, ID_UPDATE, ID_WATCH, ID_COPY, ID_WIPE,
       ID_LBLH, ID_LBLC,
       ID_EDIT = 200, ID_GUTTER, ID_EXEC, ID_CLEAR, ID_OPEN, ID_SAVE, ID_TASKS, ID_SETTINGS, ID_PLAY, ID_OUTCLR, ID_OUTCOPY, ID_MIN, ID_CLOSE };
static std::atomic<bool> gForgetReq{false};
static std::atomic<bool> gWipeAll{false};  // wipe every file it learned, then restart

namespace ui {
const int WW = 480, WH = 640;
const int TITLE_H = 32, TABS_Y = 32, TABS_H = 30, ED_Y = 64, ED_B = 404, BAR_Y = 408, BAR_H = 32, OUT_Y = 446, OUTLIST_Y = 472;
const COLORREF C_BG = RGB(18, 18, 20), C_TITLE = RGB(13, 13, 15), C_TABS = RGB(22, 22, 25), C_TAB_ON = RGB(34, 34, 38), C_ED = RGB(12, 12, 14),
               C_LINE = RGB(42, 42, 48), C_TXT = RGB(222, 222, 228), C_DIM = RGB(130, 130, 140), C_ACC = RGB(90, 150, 240);
static HBRUSH brBg, brTitle, brEd, brSmallEd;
static HFONT fUi, fUiB, fMono, fMonoS, fIcon, fTitle;
static HWND hEdit, hGutter, hOut;
static bool settingsOn = false;
static int lineH = 16;

struct Tab { std::wstring title, path, text; bool tasks = false; };
static std::vector<Tab> tabs;
static int cur = -1;
static std::vector<RECT> tabRc, tabX;
static RECT plusRc{};
static bool wantTasks = false;

static std::wstring W(const std::string &s) {
    std::wstring w(s.size() + 1, 0);
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size()); w.resize(n); return w;
}
static std::string U(const std::wstring &w) {
    std::string s(w.size() * 3 + 1, 0);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], (int)s.size(), nullptr, nullptr); s.resize(n); return s;
}
static std::wstring toCRLF(const std::wstring &in) {
    std::wstring o; o.reserve(in.size() + 64);
    for (size_t i = 0; i < in.size(); i++) { if (in[i] == L'\n' && (i == 0 || in[i - 1] != L'\r')) o += L'\r'; o += in[i]; }
    return o;
}
static std::wstring readFile(const std::wstring &p) {
    FILE *f = _wfopen(p.c_str(), L"rb"); if (!f) return L"";
    std::string s; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) s.erase(0, 3);
    return toCRLF(W(s));
}
static void writeFile(const std::wstring &p, const std::wstring &t) {
    CreateDirectoryW(L"scripts", nullptr);
    if (FILE *f = _wfopen(p.c_str(), L"wb")) { std::string s = U(t); fwrite(s.data(), 1, s.size(), f); fclose(f); }
}
static std::wstring editText() {
    int n = GetWindowTextLengthW(hEdit);
    std::wstring t(n + 1, 0); GetWindowTextW(hEdit, &t[0], n + 1); t.resize(n); return t;
}
static bool isTasksTitle(const std::wstring &t) { return t.size() > 6 && t.compare(t.size() - 6, 6, L".tasks") == 0; }

static void storeCur() { if (cur >= 0 && cur < (int)tabs.size()) tabs[cur].text = editText(); }
static void saveTab(int i, bool quiet = true) {
    if (i < 0 || i >= (int)tabs.size()) return;
    if (i == cur) storeCur();
    writeFile(tabs[i].path, tabs[i].text);
    if (!quiet) logf("saved %s", U(tabs[i].title).c_str());
}
static void showTab(HWND h, int i) {
    storeCur();
    if (cur >= 0) saveTab(cur);
    cur = i;
    SetWindowTextW(hEdit, tabs[i].text.c_str());
    SendMessageW(hEdit, EM_SETSEL, 0, 0); SendMessageW(hEdit, EM_SCROLLCARET, 0, 0);
    InvalidateRect(h, nullptr, FALSE); InvalidateRect(hGutter, nullptr, FALSE);
    if (!settingsOn) SetFocus(hEdit);
}
static int addTab(const std::wstring &title, const std::wstring &path, const std::wstring &text) {
    for (size_t i = 0; i < tabs.size(); i++) if (tabs[i].path == path) { tabs[i].text = text; return (int)i; }
    Tab t; t.title = title; t.path = path; t.text = text; t.tasks = isTasksTitle(title);
    tabs.push_back(t); return (int)tabs.size() - 1;
}
static void loadTabs() {
    CreateDirectoryW(L"scripts", nullptr);
    WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW(L"scripts\\*.txt", &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            std::wstring nm = fd.cFileName, title = nm.substr(0, nm.size() - 4);
            addTab(title, L"scripts\\" + nm, readFile(L"scripts\\" + nm));
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    if (tabs.empty()) {
        std::wstring ex =
            L"// Copycat script: one command per line, same as F10. Execute runs them.\r\n"
            L"// Lines starting with // or # are comments. \"=>\" chains a line to the next one.\r\n"
            L"// Execute queues them if Copycat isn't running yet (F6 / F7 / Play).\r\n\r\n"
            L"// person is enemy\r\n"
            L"// if detect enemy then aim at person =>\r\n"
            L"//     if looking at enemy then hold lmb for 200ms\r\n"
            L"// set task flee if health < 30 then aim away from enemy and hold w and shift\r\n";
        int i = addTab(L"Tasks", L"scripts\\Tasks.txt", ex);
        saveTab(i);
    }
}

// Execute: comments dropped, "=>" joins a line with the next, everything else goes to Copycat
static void execute() {
    storeCur();
    if (cur < 0) return;
    std::string all = U(tabs[cur].text);
    std::vector<std::string> raw, lines;
    { std::string ln; for (char c : all) { if (c == '\n') { raw.push_back(ln); ln.clear(); } else if (c != '\r') ln += c; } raw.push_back(ln); }
    std::string acc;
    for (auto l : raw) {
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.erase(0, 1);
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t')) l.pop_back();
        if (l.empty() || l.rfind("//", 0) == 0 || l[0] == '#' || l.rfind("--", 0) == 0) continue;
        bool startsArrow = l.rfind("=>", 0) == 0;
        if (!acc.empty() && !startsArrow && !(acc.size() >= 2 && acc.compare(acc.size() - 2, 2, "=>") == 0)) { lines.push_back(acc); acc.clear(); }
        acc += (acc.empty() ? "" : " ") + l;
    }
    if (!acc.empty()) lines.push_back(acc);
    for (auto &l : lines) while (l.size() >= 2 && l.compare(l.size() - 2, 2, "=>") == 0) { l.erase(l.size() - 2); while (!l.empty() && l.back() == ' ') l.pop_back(); }
    if (lines.empty() && !tabs[cur].tasks) { logf("nothing to run"); return; }
    saveTab(cur);
    cc::runScript(lines, tabs[cur].tasks);
    if (cc::active()) logf("running %d line%s from %s", (int)lines.size(), lines.size() == 1 ? "" : "s", U(tabs[cur].title).c_str());
    else logf("queued %d line%s - they run when Copycat starts (F6 / F7 / Play)", (int)lines.size(), lines.size() == 1 ? "" : "s");
}

static void openFile(HWND h) {
    wchar_t fn[MAX_PATH] = L"";
    OPENFILENAMEW of{sizeof of}; of.hwndOwner = h; of.lpstrFilter = L"Scripts (*.txt)\0*.txt\0All files\0*.*\0";
    of.lpstrFile = fn; of.nMaxFile = MAX_PATH; of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    std::wstring p = fn, nm = p.substr(p.find_last_of(L"\\/") + 1);
    std::wstring title = nm.size() > 4 && nm.compare(nm.size() - 4, 4, L".txt") == 0 ? nm.substr(0, nm.size() - 4) : nm;
    storeCur();
    int i = addTab(title, L"scripts\\" + title + L".txt", readFile(p));
    showTab(h, i);
    saveTab(i);
    logf("opened %s", U(nm).c_str());
}

// ---------- drawing ----------
static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c) { RECT r{x0, y0, x1, y1}; HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void txt(HDC dc, HFONT f, COLORREF c, RECT r, const std::wstring &s, UINT fl) { SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT); DrawTextW(dc, s.c_str(), -1, &r, fl); }

static void paint(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT cr; GetClientRect(h, &cr);
    fill(dc, 0, 0, cr.right, cr.bottom, C_BG);
    fill(dc, 0, 0, cr.right, TITLE_H, C_TITLE);
    wchar_t t[64]; swprintf(t, 64, L"CookieBot  v%d", VERSION);
    txt(dc, fTitle, C_TXT, RECT{14, 0, 300, TITLE_H}, t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::wstring st = cc::active() ? L"Copycat on" : gPaused ? L"paused" : L"";
    txt(dc, fUi, C_DIM, RECT{150, 0, cr.right - 80, TITLE_H}, st, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    // tabs
    fill(dc, 0, TABS_Y, cr.right, TABS_Y + TABS_H, C_TABS);
    tabRc.clear(); tabX.clear();
    int x = 6;
    if (!settingsOn) {
        for (size_t i = 0; i < tabs.size(); i++) {
            SIZE sz; SelectObject(dc, fUi); std::wstring lbl = tabs[i].title + L".txt";
            if (tabs[i].tasks) lbl = tabs[i].title;
            GetTextExtentPoint32W(dc, lbl.c_str(), (int)lbl.size(), &sz);
            int w = sz.cx + 44;
            if (x + w > cr.right - 40) break;
            RECT r{x, TABS_Y + 3, x + w, TABS_Y + TABS_H};
            bool on = (int)i == cur;
            if (on) { fill(dc, r.left, r.top, r.right, r.bottom, C_TAB_ON); fill(dc, r.left, r.bottom - 2, r.right, r.bottom, C_ACC); }
            txt(dc, fIcon, tabs[i].tasks ? C_ACC : C_DIM, RECT{r.left + 8, r.top, r.left + 24, r.bottom}, tabs[i].tasks ? L"\xE8FD" : L"\xE8A5", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            txt(dc, fUi, on ? C_TXT : C_DIM, RECT{r.left + 26, r.top, r.right - 18, r.bottom}, lbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT xr{r.right - 18, r.top, r.right - 4, r.bottom};
            txt(dc, fIcon, C_DIM, xr, L"\xE711", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            tabRc.push_back(r); tabX.push_back(xr);
            x += w + 2;
        }
        plusRc = RECT{cr.right - 34, TABS_Y + 3, cr.right - 8, TABS_Y + TABS_H};
        txt(dc, fIcon, C_DIM, plusRc, L"\xE710", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        txt(dc, fUiB, C_TXT, RECT{14, TABS_Y, cr.right, TABS_Y + TABS_H}, L"Settings", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    fill(dc, 0, TABS_Y + TABS_H, cr.right, TABS_Y + TABS_H + 1, C_LINE);
    if (!settingsOn) fill(dc, 0, ED_Y, cr.right, ED_B, C_ED);
    fill(dc, 0, ED_B + 1, cr.right, ED_B + 2, C_LINE);
    fill(dc, 0, OUT_Y - 2, cr.right, OUT_Y - 1, C_LINE);
    txt(dc, fUiB, C_TXT, RECT{12, OUT_Y, 200, OUTLIST_Y}, L"Output", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    fill(dc, 0, OUTLIST_Y, cr.right, cr.bottom, C_ED);
    EndPaint(h, &ps);
}

// line numbers next to the editor
static LRESULT CALLBACK gutterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT cr; GetClientRect(h, &cr);
        HDC md = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, cr.right, cr.bottom); HGDIOBJ ob = SelectObject(md, bm);
        fill(md, 0, 0, cr.right, cr.bottom, C_ED);
        int first = (int)SendMessageW(hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
        int count = (int)SendMessageW(hEdit, EM_GETLINECOUNT, 0, 0);
        int caret = (int)SendMessageW(hEdit, EM_LINEFROMCHAR, (WPARAM)-1, 0);
        LRESULT p = SendMessageW(hEdit, EM_POSFROMCHAR, SendMessageW(hEdit, EM_LINEINDEX, first, 0), 0);
        int y0 = p == -1 ? 2 : (short)HIWORD(p);
        RECT er; GetWindowRect(hEdit, &er); RECT gr; GetWindowRect(h, &gr); y0 += er.top - gr.top;
        for (int i = first; i < count; i++) {
            int y = y0 + (i - first) * lineH;
            if (y > cr.bottom) break;
            wchar_t b[16]; swprintf(b, 16, L"%d", i + 1);
            txt(md, fMono, i == caret ? C_TXT : C_DIM, RECT{0, y, cr.right - 8, y + lineH}, b, DT_RIGHT | DT_SINGLELINE);
        }
        BitBlt(dc, 0, 0, cr.right, cr.bottom, md, 0, 0, SRCCOPY);
        SelectObject(md, ob); DeleteObject(bm); DeleteDC(md);
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

// editor keys: Ctrl+A select all, Ctrl+S save, Ctrl+Enter execute
static WNDPROC editOrig;
static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    if (m == WM_KEYDOWN && ctrl) {
        if (w == 'A') { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        if (w == 'S') { saveTab(cur, false); return 0; }
        if (w == VK_RETURN) { execute(); return 0; }
    }
    if (m == WM_CHAR && ctrl && (w == 1 || w == 19 || w == 10 || w == 13)) return 0;  // swallow the beeps
    LRESULT r = CallWindowProcW(editOrig, h, m, w, l);
    if (m == WM_KEYDOWN || m == WM_CHAR || m == WM_MOUSEWHEEL || m == WM_VSCROLL || m == WM_LBUTTONDOWN || m == WM_MOUSEMOVE) InvalidateRect(hGutter, nullptr, FALSE);
    return r;
}

static void drawFlat(DRAWITEMSTRUCT *d, const wchar_t *glyph, const std::wstring &label, COLORREF col) {
    RECT r = d->rcItem;
    bool pressed = d->itemState & ODS_SELECTED;
    fill(d->hDC, r.left, r.top, r.right, r.bottom, pressed ? C_TAB_ON : C_BG);
    if (label.empty()) { txt(d->hDC, fIcon, col, r, glyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return; }
    txt(d->hDC, fIcon, col, RECT{r.left + 6, r.top, r.left + 24, r.bottom}, glyph, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    txt(d->hDC, fUi, col, RECT{r.left + 26, r.top, r.right, r.bottom}, label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void drawButton(DRAWITEMSTRUCT *d) {
    int id = d->CtlID;
    switch (id) {
    case ID_EXEC: drawFlat(d, L"\xE768", L"Execute", C_TXT); return;
    case ID_CLEAR: drawFlat(d, L"\xE75C", L"Clear", C_TXT); return;
    case ID_OPEN: drawFlat(d, L"\xE8E5", L"Open", C_TXT); return;
    case ID_SAVE: drawFlat(d, L"\xE74E", L"Save", C_TXT); return;
    case ID_TASKS: drawFlat(d, L"\xE8FD", L"Tasks", C_TXT); return;
    case ID_SETTINGS: drawFlat(d, settingsOn ? L"\xE70F" : L"\xE713", L"", settingsOn ? C_ACC : C_TXT); return;
    case ID_PLAY: drawFlat(d, cc::active() ? L"\xE71A" : L"\xE768", cc::active() ? L"Stop" : L"Play (F7)", cc::active() ? RGB(240, 120, 120) : C_ACC); return;
    case ID_OUTCLR: drawFlat(d, L"\xE74D", L"", C_DIM); return;
    case ID_OUTCOPY: drawFlat(d, L"\xE8C8", L"", C_DIM); return;
    case ID_MIN: drawFlat(d, L"\xE921", L"", C_DIM); return;
    case ID_CLOSE: drawFlat(d, L"\xE8BB", L"", C_DIM); return;
    }
    bool toggle = id == ID_BUY || id == ID_GOLD || id == ID_FX;
    bool on = id == ID_BUY ? cfgBuy : id == ID_GOLD ? cfgGolden : id == ID_FX ? cfgFx : false;
    bool pressed = d->itemState & ODS_SELECTED;
    RECT r = d->rcItem;
    fill(d->hDC, r.left, r.top, r.right, r.bottom, C_BG);
    COLORREF f = id == ID_QUIT || id == ID_WIPE ? RGB(120, 38, 46) : toggle ? (on ? RGB(40, 90, 170) : RGB(38, 38, 44)) : RGB(38, 38, 44);
    if (id == ID_PAUSE && !gPaused) f = RGB(150, 105, 30);
    if (pressed) f = RGB(GetRValue(f) * 3 / 4, GetGValue(f) * 3 / 4, GetBValue(f) * 3 / 4);
    HBRUSH b = CreateSolidBrush(f); HPEN pn = CreatePen(PS_SOLID, 1, RGB(60, 60, 68));
    HGDIOBJ ob = SelectObject(d->hDC, b), op = SelectObject(d->hDC, pn);
    RoundRect(d->hDC, r.left, r.top, r.right, r.bottom, 8, 8);
    SelectObject(d->hDC, ob); SelectObject(d->hDC, op); DeleteObject(b); DeleteObject(pn);
    wchar_t t[80]; GetWindowTextW(d->hwndItem, t, 80);
    std::wstring label = t;
    if (toggle) label += on ? L":  ON" : L":  OFF";
    if (id == ID_PAUSE) label = gPaused ? L"Resume  (F8)" : L"Pause  (F8)";
    if (id == ID_WATCH && cc::active()) label = L"Stop Copycat";
    txt(d->hDC, fUiB, C_TXT, r, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static const int settingIds[] = {ID_STATUS, ID_PAUSE, ID_QUIT, ID_BUY, ID_GOLD, ID_FX, ID_LBLH, ID_HOURS, ID_LBLC, ID_CPS, ID_APPLY, ID_STATS,
                                 ID_WATCH, ID_COPY, ID_FORGET, ID_UPDATE, ID_WIPE};
static void showSettings(HWND h, bool on) {
    settingsOn = on;
    for (int id : settingIds) ShowWindow(GetDlgItem(h, id), on ? SW_SHOW : SW_HIDE);
    ShowWindow(hEdit, on ? SW_HIDE : SW_SHOW); ShowWindow(hGutter, on ? SW_HIDE : SW_SHOW);
    for (int id : {ID_EXEC, ID_CLEAR, ID_OPEN, ID_SAVE, ID_TASKS}) EnableWindow(GetDlgItem(h, id), !on);
    InvalidateRect(h, nullptr, TRUE);
    if (!on) SetFocus(hEdit);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static int seenLog = 0;
    switch (m) {
    case WM_CREATE: {
        auto mk = [&](const wchar_t *cls, const wchar_t *t, DWORD st, int x, int y, int cw, int ch, int id, HFONT f, DWORD ex = 0) {
            HWND c = CreateWindowExW(ex, cls, t, WS_CHILD | WS_VISIBLE | st, x, y, cw, ch, h, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
            return c;
        };
        // title bar buttons
        mk(L"BUTTON", L"", BS_OWNERDRAW, WW - 68, 0, 34, TITLE_H, ID_MIN, fUi);
        mk(L"BUTTON", L"", BS_OWNERDRAW, WW - 34, 0, 34, TITLE_H, ID_CLOSE, fUi);
        // editor + gutter
        hGutter = CreateWindowExW(0, L"CookieBotGutter", L"", WS_CHILD | WS_VISIBLE, 0, ED_Y, 40, ED_B - ED_Y, h, (HMENU)ID_GUTTER, nullptr, nullptr);
        hEdit = mk(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN | ES_NOHIDESEL | WS_VSCROLL | WS_HSCROLL,
                   42, ED_Y + 4, WW - 42, ED_B - ED_Y - 4, ID_EDIT, fMono);
        SendMessageW(hEdit, EM_SETLIMITTEXT, 1 << 20, 0);
        SendMessageW(hEdit, EM_SETMARGINS, EC_LEFTMARGIN, 4);
        { int ts = 16; SendMessageW(hEdit, EM_SETTABSTOPS, 1, (LPARAM)&ts); }
        SetWindowTheme(hEdit, L"DarkMode_Explorer", nullptr);
        editOrig = (WNDPROC)SetWindowLongPtrW(hEdit, GWLP_WNDPROC, (LONG_PTR)editProc);
        { HDC dc = GetDC(hEdit); SelectObject(dc, fMono); TEXTMETRICW tm; GetTextMetricsW(dc, &tm); lineH = tm.tmHeight; ReleaseDC(hEdit, dc); }
        // toolbar
        mk(L"BUTTON", L"", BS_OWNERDRAW, 6, BAR_Y, 86, BAR_H, ID_EXEC, fUi);
        mk(L"BUTTON", L"", BS_OWNERDRAW, 94, BAR_Y, 70, BAR_H, ID_CLEAR, fUi);
        mk(L"BUTTON", L"", BS_OWNERDRAW, 166, BAR_Y, 70, BAR_H, ID_OPEN, fUi);
        mk(L"BUTTON", L"", BS_OWNERDRAW, 238, BAR_Y, 66, BAR_H, ID_SAVE, fUi);
        mk(L"BUTTON", L"", BS_OWNERDRAW, 306, BAR_Y, 66, BAR_H, ID_TASKS, fUi);
        mk(L"BUTTON", L"", BS_OWNERDRAW, 374, BAR_Y, 32, BAR_H, ID_SETTINGS, fUi);
        mk(L"BUTTON", L"", BS_OWNERDRAW, 404, BAR_Y, 74, BAR_H, ID_PLAY, fUi);
        // output
        mk(L"BUTTON", L"", BS_OWNERDRAW, WW - 64, OUT_Y, 28, 24, ID_OUTCLR, fUi);
        mk(L"BUTTON", L"", BS_OWNERDRAW, WW - 34, OUT_Y, 28, 24, ID_OUTCOPY, fUi);
        hOut = mk(L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL | LBS_NOSEL, 0, OUTLIST_Y, WW, WH - OUTLIST_Y, ID_LOG, fMonoS);
        SendMessageW(hOut, LB_SETHORIZONTALEXTENT, 1600, 0);
        SetWindowTheme(hOut, L"DarkMode_Explorer", nullptr);
        // settings page (hidden until the gear is clicked)
        int y = ED_Y + 8;
        mk(L"STATIC", L"", SS_LEFT, 16, y, WW - 32, 36, ID_STATUS, fUi); y += 40;
        mk(L"BUTTON", L"Pause", BS_OWNERDRAW, 16, y, 218, 32, ID_PAUSE, fUi);
        mk(L"BUTTON", L"Quit  (F9)", BS_OWNERDRAW, 246, y, 218, 32, ID_QUIT, fUi); y += 40;
        mk(L"BUTTON", L"Buy upgrades", BS_OWNERDRAW, 16, y, 144, 30, ID_BUY, fUi);
        mk(L"BUTTON", L"Bonus cookies", BS_OWNERDRAW, 168, y, 144, 30, ID_GOLD, fUi);
        mk(L"BUTTON", L"Effects + HUD", BS_OWNERDRAW, 320, y, 144, 30, ID_FX, fUi); y += 38;
        wchar_t b[32];
        mk(L"STATIC", L"Session hours", SS_LEFT, 16, y + 4, 90, 20, ID_LBLH, fUi);
        swprintf(b, 32, L"%.1f", cfgHours); mk(L"EDIT", b, ES_LEFT, 108, y + 2, 56, 22, ID_HOURS, fUi);
        mk(L"STATIC", L"Clicks/s (max 12)", SS_LEFT, 180, y + 4, 112, 20, ID_LBLC, fUi);
        swprintf(b, 32, L"%.1f", cfgCps); mk(L"EDIT", b, ES_LEFT, 294, y + 2, 50, 22, ID_CPS, fUi);
        mk(L"BUTTON", L"Apply", BS_OWNERDRAW, 384, y, 80, 28, ID_APPLY, fUi); y += 36;
        mk(L"STATIC", L"", SS_LEFT, 16, y, WW - 32, 54, ID_STATS, fUi); y += 58;
        mk(L"BUTTON", L"Watch me play (F6)", BS_OWNERDRAW, 16, y, 218, 28, ID_WATCH, fUi);
        mk(L"BUTTON", L"Let it play (F7)", BS_OWNERDRAW, 246, y, 218, 28, ID_COPY, fUi); y += 34;
        mk(L"BUTTON", L"Forget cookie brain", BS_OWNERDRAW, 16, y, 218, 28, ID_FORGET, fUi);
        mk(L"BUTTON", L"Check for updates", BS_OWNERDRAW, 246, y, 218, 28, ID_UPDATE, fUi); y += 34;
        mk(L"BUTTON", L"Wipe all  -  restart with no knowledge", BS_OWNERDRAW, 16, y, 448, 28, ID_WIPE, fUi);
        loadTabs();
        showSettings(h, false);
        showTab(h, 0);
        SetTimer(h, 1, 250, nullptr);
        SetTimer(h, 2, 60, nullptr);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT p{(short)LOWORD(l), (short)HIWORD(l)}; ScreenToClient(h, &p);
        if (p.y >= 0 && p.y < TITLE_H && p.x < WW - 70) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_PAINT: paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        if (settingsOn) return 0;
        for (size_t i = 0; i < tabRc.size(); i++) {
            if (PtInRect(&tabX[i], p)) {
                if (tabs.size() <= 1) { logf("can't close the last tab"); return 0; }
                std::wstring q = L"Close and delete \"" + tabs[i].title + L"\"?";
                if (MessageBoxW(h, q.c_str(), L"CookieBot", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                if ((int)i == cur) cur = -1; else storeCur();
                DeleteFileW(tabs[i].path.c_str());
                tabs.erase(tabs.begin() + i);
                int ni = std::min<int>((int)i, (int)tabs.size() - 1);
                if (cur > (int)i) cur--;
                if (cur < 0) { cur = -1; SetWindowTextW(hEdit, L""); }
                showTab(h, cur >= 0 ? cur : ni);
                return 0;
            }
            if (PtInRect(&tabRc[i], p)) { if ((int)i != cur) showTab(h, (int)i); return 0; }
        }
        if (PtInRect(&plusRc, p)) {
            int n = 1; std::wstring t;
            for (;; n++) { t = L"Script " + std::to_wstring(n); bool used = false; for (auto &tb : tabs) if (tb.title == t) used = true; if (!used) break; }
            int i = addTab(t, L"scripts\\" + t + L".txt", L"");
            saveTab(i);
            showTab(h, i);
        }
        return 0;
    }
    case WM_DRAWITEM: drawButton((DRAWITEMSTRUCT *)l); return TRUE;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)w, C_TXT); SetBkColor((HDC)w, C_BG); return (LRESULT)brBg;
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)w, RGB(200, 200, 208)); SetBkColor((HDC)w, C_ED); return (LRESULT)brEd;
    case WM_CTLCOLOREDIT:
        if ((HWND)l == hEdit) { SetTextColor((HDC)w, C_TXT); SetBkColor((HDC)w, C_ED); return (LRESULT)brEd; }
        SetTextColor((HDC)w, C_TXT); SetBkColor((HDC)w, RGB(34, 34, 40)); return (LRESULT)brSmallEd;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_EDIT) { if (HIWORD(w) == EN_VSCROLL || HIWORD(w) == EN_CHANGE) InvalidateRect(hGutter, nullptr, FALSE); return 0; }
        if (id == ID_MIN) ShowWindow(h, SW_MINIMIZE);
        else if (id == ID_CLOSE) { if (MessageBoxW(h, L"Quit CookieBot?", L"CookieBot", MB_YESNO | MB_ICONQUESTION) == IDYES) gQuit = true; }
        else if (id == ID_EXEC) execute();
        else if (id == ID_CLEAR) { SetWindowTextW(hEdit, L""); SetFocus(hEdit); }
        else if (id == ID_OPEN) openFile(h);
        else if (id == ID_SAVE) saveTab(cur, false);
        else if (id == ID_TASKS) { cc::requestTasks(); wantTasks = true; if (!cc::active()) logf("showing the tasks Copycat has loaded (%s)", cc::gameName().c_str()); }
        else if (id == ID_SETTINGS) showSettings(h, !settingsOn);
        else if (id == ID_PLAY) {
            if (cc::active()) std::thread(cc::stop).detach();
            else { logf("Copycat: click your game window now - starting in 3 s"); ccStartAsync(2, 3); }
        } else if (id == ID_OUTCLR) SendMessageW(hOut, LB_RESETCONTENT, 0, 0);
        else if (id == ID_OUTCOPY) {
            std::wstring all; int n = (int)SendMessageW(hOut, LB_GETCOUNT, 0, 0);
            for (int i = 0; i < n; i++) { int len = (int)SendMessageW(hOut, LB_GETTEXTLEN, i, 0); std::wstring s(len + 1, 0); SendMessageW(hOut, LB_GETTEXT, i, (LPARAM)&s[0]); s.resize(len); all += s + L"\r\n"; }
            if (OpenClipboard(h)) {
                EmptyClipboard();
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (all.size() + 1) * sizeof(wchar_t));
                if (g) { memcpy(GlobalLock(g), all.c_str(), (all.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
                CloseClipboard();
                logf("output copied");
            }
        } else if (id == ID_PAUSE) {
            if (gPaused) { gPaused = false; gResumeAt = now() + 2.5; if (gHwnd) SetForegroundWindow(gHwnd); logf("Resuming - switching to the game. Hands off the mouse."); }
            else { gPaused = true; logf("Paused from the menu"); }
        } else if (id == ID_QUIT) gQuit = true;
        else if (id == ID_BUY) { cfgBuy = !cfgBuy; logf("Buying upgrades %s", cfgBuy ? "on" : "off"); }
        else if (id == ID_GOLD) { cfgGolden = !cfgGolden; logf("Bonus cookie clicking %s", cfgGolden ? "on" : "off"); }
        else if (id == ID_FX) { cfgFx = !cfgFx; logf("Effects + HUD %s", cfgFx ? "on" : "off"); }
        else if (id == ID_APPLY) {
            wchar_t b[32];
            GetDlgItemTextW(h, ID_HOURS, b, 32); double hrs = _wtof(b);
            GetDlgItemTextW(h, ID_CPS, b, 32); double c = _wtof(b);
            if (hrs >= 0.25 && hrs <= 72) cfgHours = hrs;
            if (c >= 1 && c <= 12) cfgCps = c;
            logf("Settings: %.1f h sessions, %.1f clicks/s", cfgHours, cfgCps);
        } else if (id == ID_FORGET) {
            if (MessageBoxW(h, L"Wipe what the cookie bot learned and start its brain from scratch?", L"CookieBot", MB_YESNO | MB_ICONQUESTION) == IDYES) gForgetReq = true;
        } else if (id == ID_WIPE) {
            if (MessageBoxW(h, L"Delete EVERYTHING it learned - cookie brain plus every Copycat recording, rule, task, name, hotbar and camera setting for every game - and restart from zero?\n\nYour scripts in the editor are kept.\n\nThis can't be undone.",
                            L"CookieBot - wipe all", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                logf("Wiping everything and restarting...");
                gWipeAll = true; gQuit = true;
            }
        } else if (id == ID_WATCH || id == ID_COPY) {
            if (cc::active()) std::thread(cc::stop).detach();
            else { logf("Copycat: click your game window now - starting in 3 s"); ccStartAsync(id == ID_WATCH ? 1 : 2, 3); }
        } else if (id == ID_UPDATE) { static double last = 0; if (now() - last > 5) { last = now(); requestUpdateCheck(); logf("Checking GitHub for updates..."); } }
        if (id != ID_EDIT) InvalidateRect(GetDlgItem(h, id), nullptr, TRUE);
        return 0;
    }
    case WM_TIMER: {
        if (w == 2) {
            static int lf = -1, lc = -1, lk = -1;
            int f = (int)SendMessageW(hEdit, EM_GETFIRSTVISIBLELINE, 0, 0), c = (int)SendMessageW(hEdit, EM_GETLINECOUNT, 0, 0), k = (int)SendMessageW(hEdit, EM_LINEFROMCHAR, (WPARAM)-1, 0);
            if (f != lf || c != lc || k != lk) { lf = f; lc = c; lk = k; InvalidateRect(hGutter, nullptr, FALSE); }
            return 0;
        }
        std::string st, task; int serial; std::vector<std::string> newLines;
        {
            std::lock_guard<std::mutex> lk(gHud.mu);
            st = gPaused ? "PAUSED" : gHud.state; task = gHud.task; serial = gHud.logSerial;
            int fresh = std::min<int>(serial - seenLog, (int)gHud.log.size());
            for (int i = (int)gHud.log.size() - fresh; i < (int)gHud.log.size(); i++) newLines.push_back(gHud.log[i]);
            seenLog = serial;
        }
        SetDlgItemTextA(h, ID_STATUS, (st + "  -  " + task).c_str());
        char b[400];
        double up = gStart > 0 ? now() - gStart : 0;
        snprintf(b, sizeof b, "Cookies %s   |   %s/s   |   %s/click\nBought %d this run, %d all-time   |   uptime %s\nBrain: %d upgrade types, %d buildings, %d price curves learned",
                 fmtNum(gEconTime > 0 ? bankNow() : 0).c_str(), fmtNum(gEcon.cps).c_str(), fmtNum(gEcon.perClick).c_str(), gHud.buysRun, gBrain.purchases,
                 fmtDur(up).c_str(), (int)gBrain.corr.size(), (int)gBrain.unitCps.size(), (int)gBrain.growth.size());
        SetDlgItemTextA(h, ID_STATS, b);
        for (auto &ln : newLines) {
            std::string s2 = ln;
            size_t q = s2.find("] ");
            if (q != std::string::npos && q < 12) s2.insert(q + 1, " -");  // "[12:00:01] - message"
            SendMessageW(hOut, LB_ADDSTRING, 0, (LPARAM)W(s2).c_str());
        }
        int cnt = (int)SendMessageW(hOut, LB_GETCOUNT, 0, 0);
        while (cnt > 400) { SendMessageW(hOut, LB_DELETESTRING, 0, 0); cnt--; }
        if (!newLines.empty()) SendMessageW(hOut, LB_SETTOPINDEX, cnt - 1, 0);
        std::string tt;
        if (wantTasks && cc::takeTasks(tt)) {
            wantTasks = false;
            std::wstring title = W(cc::gameName()) + L".tasks";
            storeCur();
            int i = addTab(title, L"scripts\\" + title + L".txt", toCRLF(W(tt)));
            tabs[i].tasks = true;
            showTab(h, i); saveTab(i);
            logf("loaded %s - edit and Execute to replace its explained tasks", U(title).c_str());
        }
        static bool lastActive = false, lastPaused = false;
        if (cc::active() != lastActive || gPaused != lastPaused) {
            lastActive = cc::active(); lastPaused = gPaused;
            InvalidateRect(GetDlgItem(h, ID_PLAY), nullptr, FALSE); InvalidateRect(GetDlgItem(h, ID_WATCH), nullptr, FALSE);
            RECT tr{0, 0, WW, TITLE_H}; InvalidateRect(h, &tr, FALSE);
        }
        InvalidateRect(GetDlgItem(h, ID_PAUSE), nullptr, FALSE);
        if (gQuit) DestroyWindow(h);
        return 0;
    }
    case WM_CLOSE: gQuit = true; DestroyWindow(h); return 0;
    case WM_DESTROY: storeCur(); for (size_t i = 0; i < tabs.size(); i++) saveTab((int)i); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace ui

static void panelThread() {
    using namespace ui;
    brBg = CreateSolidBrush(C_BG); brEd = CreateSolidBrush(C_ED); brSmallEd = CreateSolidBrush(RGB(34, 34, 40)); brTitle = CreateSolidBrush(C_TITLE);
    fUi = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fUiB = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fTitle = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fMono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    fMonoS = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    fIcon = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW gc{}; gc.lpfnWndProc = gutterProc; gc.hInstance = hi; gc.lpszClassName = L"CookieBotGutter"; gc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&gc);
    WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = L"CookieBotPanel";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); wc.hbrBackground = brBg; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    wchar_t title[64]; swprintf(title, 64, L"CookieBot  v%d", VERSION);
    gPanel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, L"CookieBotPanel", title, WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                             wa.right - WW - 24, wa.top + 60, WW, WH, nullptr, nullptr, hi, nullptr);
    typedef HRESULT(WINAPI * DwmSet)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
        if (auto f = (DwmSet)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            BOOL dark = TRUE; f(gPanel, 20, &dark, sizeof dark);
            int round = 2; f(gPanel, 33, &round, sizeof round);  // rounded corners on Windows 11
        }
    SetWindowDisplayAffinity(gPanel, 0x11);  // WDA_EXCLUDEFROMCAPTURE: the bot's screenshots see the game, not this window
    ShowWindow(gPanel, SW_SHOWNORMAL);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    gQuit = true;
}
