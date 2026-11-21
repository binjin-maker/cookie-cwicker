// ================= BRAIN VIEW: live neuron picture of what CookieBot is thinking =================
// Columns: what it SEES -> what it REMEMBERS -> what it WANTS -> what it DOES.
// Every neuron is a real signal from the bot (0..1), not decoration.
namespace brain {

struct Layer { const char *title; std::vector<std::string> names; std::vector<float> v; };

static HWND wnd = nullptr;

static void line(Canvas &cv, float x0, float y0, float x1, float y1, int r, int g, int b, float a) {
    float dx = x1 - x0, dy = y1 - y0;
    int n = (int)std::max(std::fabs(dx), std::fabs(dy));
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / n : 0, x = x0 + dx * t, y = y0 + dy * t;
        int xi = (int)x, yi = (int)y;
        float fy = y - yi;
        cv.blend(xi, yi, r, g, b, a * (1 - fy));
        cv.blend(xi, yi + 1, r, g, b, a * fy);
    }
}

static void fillCookie(std::vector<Layer> &L) {
    std::string task, want; double wantCost; std::vector<std::pair<std::string, double>> alts;
    { std::lock_guard<std::mutex> lk(gHud.mu); task = gHud.task; want = gHud.want; wantCost = gHud.wantCost; alts = gHud.alts; }
    double bank = gEconTime > 0 ? bankNow() : 0;
    auto has = [&](const char *w) { return task.find(w) != std::string::npos ? 1.f : 0.f; };
    L.push_back({"SEES", {"cookie count", "cookies/sec", "per click", "bonus cookie", "puzzle", "your order", "store", "popups"},
                 {now() - gEconTime < 1.5f ? 1.f : 0.3f, gEcon.cps > 0 ? 1.f : 0.f, gEcon.perClick > 0 ? 0.8f : 0.f, now() - gLastBonus < 3 ? 1.f : 0.f,
                  gPuzzle ? 1.f : 0.f, gGoal.on ? 1.f : 0.f, now() - gLastFull < 8 ? 1.f : has("store"), has("popup")}});
    Layer mem{"OPTIONS", {}, {}};
    mem.names.push_back(want.empty() ? "-" : want); mem.v.push_back(want.empty() ? 0.f : 1.f);
    for (auto &a : alts) { mem.names.push_back(a.first); mem.v.push_back((float)std::max(0.05, 1.0 + a.second / 100.0)); }
    while (mem.names.size() < 4) { mem.names.push_back("-"); mem.v.push_back(0); }
    L.push_back(mem);
    float afford = wantCost > 0 ? (float)std::min(1.0, bank / wantCost) : 0;
    L.push_back({"WANTS", {"buy now", "save up", "click", "bonus", "order", "solve puzzle", "read screen", "rest"},
                 {afford >= 1 ? 1.f : 0.f, afford < 1 && wantCost > 0 ? afford : 0.f, has("Clicking"), now() - gLastBonus < 2 ? 1.f : 0.f, gGoal.on ? 1.f : 0.f,
                  gPuzzle ? 1.f : 0.f, has("Reading"), gPaused ? 1.f : 0.f}});
    L.push_back({"DOES", {"click cookie", "buy", "scroll", "OCR read", "bonus click", "close popup"},
                 {has("Clicking"), has("Buying"), has("Reading the store") * 0.8f, std::max(has("Reading"), now() - gEconTime < 0.4 ? 0.7f : 0.f),
                  now() - gLastBonus < 1 ? 1.f : 0.f, has("popup")}});
}

static void fillCopycat(std::vector<Layer> &L) {
    std::lock_guard<std::mutex> lk(cc::bmu);
    L.push_back({"SEES", {"motion", "enemy", "objects", "movers", "target", "tool", "need", "you"}, std::vector<float>(cc::bIn, cc::bIn + 8)});
    L.push_back({"MEMORIES", {"memory 1", "memory 2", "memory 3", "memory 4", "memory 5", "memory 6"}, std::vector<float>(cc::bMem, cc::bMem + 6)});
    L.push_back({"WANTS", {"fight", "gather", "survive", "go for good", "be careful", "your order", "confused", "copy you"}, std::vector<float>(cc::bDrv, cc::bDrv + 8)});
    // DOES: every key this game actually uses (most used first), mouse buttons and turning
    Layer out{"DOES", {}, {}};
    {
        std::vector<std::pair<int, int>> used;
        {
            std::lock_guard<std::mutex> dl(cc::mu);
            for (auto &kv : cc::keyStats) if (kv.first < 256 && kv.second.held > 5) used.push_back({kv.second.held, kv.first});
        }
        std::sort(used.rbegin(), used.rend());
        if (used.empty()) for (int vk : {'W', 'A', 'S', 'D', (int)VK_SPACE, (int)VK_SHIFT, (int)VK_CONTROL, 'E'}) used.push_back({0, vk});
        for (size_t i = 0; i < used.size() && i < 14; i++) { out.names.push_back(cc::keyName(used[i].second)); out.v.push_back(cc::outAll[used[i].second]); }
        out.names.push_back("shoot"); out.v.push_back(cc::outAll[256]);
        out.names.push_back("aim"); out.v.push_back(cc::outAll[257]);
        out.names.push_back("turn left"); out.v.push_back(cc::bOut[10]);
        out.names.push_back("turn right"); out.v.push_back(cc::bOut[11]);
    }
    L.push_back(out);
}

static void render(const std::vector<Layer> &L, const RECT &wa, double k, bool copy) {
    size_t most = 0; for (auto &l : L) most = std::max(most, l.v.size());
    int W = (int)(660 * k), H = (int)((most > 12 ? 360 + (most - 12) * 22 : 360) * k);
    std::vector<uint32_t> bg((size_t)W * H, 0);
    Canvas cv{W, H, bg.data()};
    float R = 16 * (float)k;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float dx = std::max(0.f, std::max(R - x, x - (W - 1 - R))), dy = std::max(0.f, std::max(R - y, y - (H - 1 - R)));
            float d = std::sqrt(dx * dx + dy * dy);
            if (d > R) continue;
            float cy = (float)y / H;
            if (R - d < 2 * (float)k) cv.blend(x, y, 40, 190, 230, 0.9f);
            else cv.blend(x, y, (int)(6 + 8 * cy), (int)(12 + 14 * cy), (int)(30 + 26 * cy), 0.9f);
        }
    float t = (float)now();
    int nl = (int)L.size();
    float left = 92 * (float)k, right = W - 92 * (float)k, top = 48 * (float)k, bot = H - 22 * (float)k;
    std::vector<std::vector<std::pair<float, float>>> pos(nl);
    for (int c = 0; c < nl; c++) {
        int n = (int)L[c].v.size();
        float x = nl > 1 ? left + (right - left) * c / (nl - 1) : W / 2.f;
        for (int i = 0; i < n; i++) pos[c].push_back({x, top + (bot - top) * (i + 0.5f) / n});
    }
    // links: brighter where both ends are active, with pulses running along the strong ones
    for (int c = 0; c + 1 < nl; c++)
        for (size_t i = 0; i < pos[c].size(); i++)
            for (size_t j = 0; j < pos[c + 1].size(); j++) {
                float a = L[c].v[i] * L[c + 1].v[j];
                float al = 0.05f + 0.55f * a;
                auto p = pos[c][i], q = pos[c + 1][j];
                line(cv, p.first, p.second, q.first, q.second, 60, (int)(170 + 60 * a), 230, al);
                if (a > 0.25f) {
                    float ph = std::fmod(t * 1.3f + i * 0.37f + j * 0.61f + c * 0.13f, 1.f);
                    cv.disc(p.first + (q.first - p.first) * ph, p.second + (q.second - p.second) * ph, 2.2f * (float)k, 200, 250, 255, a);
                }
            }
    for (int c = 0; c < nl; c++)
        for (size_t i = 0; i < pos[c].size(); i++) {
            float a = std::max(0.f, std::min(1.f, L[c].v[i]));
            auto p = pos[c][i];
            float rad = (6.5f + 3.5f * a) * (float)k;
            if (a > 0.05f) cv.disc(p.first, p.second, rad + 9 * (float)k * a, 40, 200, 255, 0.22f * a, 8 * (float)k);
            cv.disc(p.first, p.second, rad, (int)(25 + 60 * a), (int)(120 + 110 * a), (int)(160 + 95 * a), 0.55f + 0.45f * a);
            cv.disc(p.first - rad * 0.3f, p.second - rad * 0.3f, rad * 0.35f, 220, 250, 255, 0.25f + 0.5f * a);
        }
    // labels
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memset(bits, 0, (size_t)W * H * 4);
    HGDIOBJ oldB = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    HFONT fT = CreateFontW(-(int)(16 * k), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT fS = CreateFontW(-(int)(11 * k), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    auto text = [&](HFONT f, COLORREF col, int x, int y, const std::string &s, UINT align) {
        SelectObject(dc, f); SetTextColor(dc, col);
        std::wstring w(s.begin(), s.end());
        RECT r{x, y, x, y};
        if (align == DT_RIGHT) { r.left = x - (int)(84 * k); r.right = x; }
        else if (align == DT_CENTER) { r.left = x - (int)(70 * k); r.right = x + (int)(70 * k); }
        else { r.left = x; r.right = x + (int)(84 * k); }
        r.bottom = y + (int)(16 * k);
        DrawTextW(dc, w.c_str(), -1, &r, align | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    };
    SelectObject(dc, fT); SetTextColor(dc, RGB(170, 235, 255));
    RECT tr{(int)(18 * k), (int)(8 * k), W, (int)(30 * k)};
    DrawTextW(dc, copy ? L"COOKIEBOT BRAIN  -  copycat" : L"COOKIEBOT BRAIN  -  cookie mode", -1, &tr, DT_LEFT | DT_SINGLELINE);
    for (int c = 0; c < nl; c++) {
        if (pos[c].empty()) continue;
        text(fS, RGB(110, 160, 220), (int)pos[c][0].first, (int)(30 * k), L[c].title, DT_CENTER);
        for (size_t i = 0; i < pos[c].size(); i++) {
            float a = std::max(0.f, std::min(1.f, L[c].v[i]));
            COLORREF col = RGB((int)(120 + 120 * a), (int)(150 + 100 * a), (int)(180 + 75 * a));
            int y = (int)(pos[c][i].second - 7 * k);
            if (c == 0) text(fS, col, (int)(pos[c][i].first - 14 * k), y, L[c].names[i], DT_RIGHT);
            else if (c == nl - 1) text(fS, col, (int)(pos[c][i].first + 14 * k), y, L[c].names[i], DT_LEFT);
            else if (a > 0.3f) text(fS, col, (int)(pos[c][i].first + 12 * k), (int)(y - 9 * k), L[c].names[i], DT_LEFT);
        }
    }
    GdiFlush();
    const uint32_t *tp = (const uint32_t *)bits;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint32_t v = tp[i];
        int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
        int m = std::max(r, std::max(g, b));
        if (!m) continue;
        cv.blend((int)(i % W), (int)(i / W), r * 255 / m, g * 255 / m, b * 255 / m, m / 255.f);
    }
    SelectObject(dc, oldB);
    DeleteObject(fT); DeleteObject(fS); DeleteObject(bmp); DeleteDC(dc);
    int x = wa.right - W - (int)(26 * k), y = wa.bottom - H - (int)(26 * k);
    pushLayered(wnd, x, y, W, H, bg.data());
}

static void thread() {
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"CookieBotBrain";
    RegisterClassW(&wc);
    wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"CookieBotBrain", L"", WS_POPUP, 0, 0, 1, 1,
                          nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowDisplayAffinity(wnd, WDA_EXCLUDEFROMCAPTURE);
    bool shown = false;
    while (!gQuit) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        bool copy = cc::active();
        bool on = cfgFx && (copy ? cc::hudWanted() : (gOverlayOn && gHwnd && gameFocused() && !gPaused));
        if (!on) { if (shown) { ShowWindow(wnd, SW_HIDE); shown = false; } Sleep(100); continue; }
        std::vector<Layer> L;
        if (copy) fillCopycat(L); else fillCookie(L);
        HWND ref = copy ? cc::target() : gHwnd;
        HMONITOR hm = MonitorFromWindow(ref, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof mi}; GetMonitorInfo(hm, &mi);
        double k = 1.0; UINT d = GetDpiForWindow(ref); if (d) k = d / 96.0 * 0.9;
        render(L, mi.rcWork, k, copy);
        if (!shown) { ShowWindow(wnd, SW_SHOWNOACTIVATE); shown = true; }
        Sleep(45);
    }
    ShowWindow(wnd, SW_HIDE);
}

}  // namespace brain
