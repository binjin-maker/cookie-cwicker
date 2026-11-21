// CookieBot - learning auto-player for COOKIES FOR U
// F8 = pause/resume, F9 = quit. Touching the mouse pauses it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#include <mmsystem.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <windows.storage.streams.h>
#include <robuffer.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <cstdarg>
#include <ctime>
#include "logic.h"
#include <climits>

using namespace ABI::Windows::Storage::Streams;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif

// ---------- config ----------
static double cfgCps = 12.0, cfgHours = 8.0;
static bool cfgGolden = true, cfgBuy = true, cfgFx = true;
static const char *BRAIN = "cookiebot_brain.txt";
static const int VERSION = 13;
static const char *REPO_RAW = "https://raw.githubusercontent.com/binjin-maker/cookie-cwicker/main/";

// ---------- state ----------
static std::atomic<bool> gPaused{false}, gQuit{false}, gOverlayOn{false};
static std::atomic<double> gResumeAt{0};
static HWND gHwnd = nullptr, gPanel = nullptr;
static RECT gMon{}, gWork{};
static FILE *gLog = nullptr, *gDbg = nullptr;
static LARGE_INTEGER gFreq;
static double gStart = 0;
static Brain gBrain;
static std::atomic<bool> gUpdateCheckReq{false};
static HANDLE gUpdEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
static void requestUpdateCheck() { gUpdateCheckReq = true; SetEvent(gUpdEvent); }

// prompt bar: typed orders
static std::mutex gPromptMu;
static std::string gPromptText, gReply;
static std::atomic<bool> gPromptOpen{false};
static double gPromptLast = 0, gReplyAt = -100;
static std::deque<std::string> gCmds;
struct Goal { bool on = false; std::string key, name; int target = 0, start = 0, count = 1, bought = 0; double since = 0; };
static Goal gGoal;
static void updateGoalHud();
namespace cc {
void onKey(DWORD vk, bool down); void onPhys(); bool asking(); HWND target(); void answer(const std::string &s); bool active();
void start(int m); void stop(); void renderHud(); bool hudWanted(); void init();
}
static void processCmds();

static double now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / gFreq.QuadPart; }

struct HudState {
    std::mutex mu;
    std::string state = "STARTING", task = "Starting up", want, wantType, why, order;
    double wantCost = 0, wantGain = 0;
    std::vector<std::pair<std::string, double>> alts;
    std::deque<std::string> lines, log;
    long long clicks = 0;
    int buysRun = 0, bonusRun = 0;
    int logSerial = 0;
} gHud;

static void setTask(const char *fmt, ...) {
    char buf[256];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    std::lock_guard<std::mutex> lk(gHud.mu);
    gHud.task = buf;
}
static void setState(const char *st) { std::lock_guard<std::mutex> lk(gHud.mu); gHud.state = st; }

static void logf(const char *fmt, ...) {
    char buf[1024];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    time_t t = time(nullptr); char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    if (gLog) { fprintf(gLog, "[%s] %s\n", ts, buf); fflush(gLog); }
    std::lock_guard<std::mutex> lk(gHud.mu);
    gHud.lines.push_back(buf);
    while (gHud.lines.size() > 4) gHud.lines.pop_front();
    gHud.log.push_back(std::string("[") + ts + "] " + buf);
    while (gHud.log.size() > 300) gHud.log.pop_front();
    gHud.logSerial++;
}
static void dbgf(const char *fmt, ...) {
    if (!gDbg) return;
    va_list a; va_start(a, fmt); vfprintf(gDbg, fmt, a); va_end(a);
    fputc('\n', gDbg); fflush(gDbg);
}
static std::string fmtNum(double v) {
    static const char *u[] = {"", "K", "M", "B", "T", "Qa", "Qi", "Sx", "Sp", "Oc", "No", "Dc"};
    int i = 0; while (v >= 1000 && i < 11) { v /= 1000; i++; }
    char b[32]; snprintf(b, sizeof b, "%.2f%s", v, u[i]); return b;
}
static bool nap(int ms) {
    double end = now() + ms / 1000.0;
    while (now() < end) {
        if (gQuit || gPaused) return false;
        Sleep(std::min(20, std::max(1, (int)((end - now()) * 1000))));
    }
    return true;
}

// ---------- window safety ----------
static std::string titleOf(HWND h) {
    wchar_t w[512]; int n = GetWindowTextW(h, w, 512);
    char b[1024]; int m = WideCharToMultiByte(CP_UTF8, 0, w, n, b, sizeof b - 1, nullptr, nullptr); b[m] = 0;
    return lower(b);
}
static bool gameFocused() { return GetForegroundWindow() == gHwnd; }
static bool gameTitleOk() { return titleOf(gHwnd).find("cookies") != std::string::npos; }
static bool canAct() { return !gPaused && !gQuit && now() > gResumeAt && gameFocused() && gameTitleOk(); }

// ================= overlay: blue vignette + cursor effects =================
static HWND gVig = nullptr, gFx = nullptr, gHudWnd = nullptr;
static void renderHud();
static std::atomic<bool> gHiddenFromCapture{false}, gVigVisible{false};
struct Burst { int x, y; double t; bool big; };
static std::mutex gFxMu;
static std::vector<Burst> gBursts;
static const int FXS = 260;
static HWND gPath = nullptr, gRead = nullptr, gScan = nullptr, gDead = nullptr;
static std::atomic<bool> gPuzzle{false};
static std::atomic<int> gPuzzleHits{0};
static int gPuzzleX = 0, gPuzzleY = 0;
static Rect gReadRect{0, 0, 0, 0};
static double gReadAt = -100;
static std::atomic<bool> gCapturing{false};
struct PathT { bool glide = false; int sx = 0, sy = 0, tx = 0, ty = 0; double amp = 0; bool next = false; int nx = 0, ny = 0, dir = 0; };
static PathT gPathT;
static void setNext(bool on, int x = 0, int y = 0, int dir = 0) {
    std::lock_guard<std::mutex> lk(gFxMu);
    gPathT.next = on; gPathT.nx = x; gPathT.ny = y; gPathT.dir = dir;
}

static void fxClick(int x, int y, bool big = false) {
    std::lock_guard<std::mutex> lk(gFxMu);
    gBursts.push_back({x, y, now(), big});
    if (gBursts.size() > 40) gBursts.erase(gBursts.begin());
}

struct Canvas {
    int w, h; uint32_t *px;
    void blend(int x, int y, int r, int g, int b, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        if (a > 1) a = 1;
        uint32_t &d = px[y * w + x];
        float da = ((d >> 24) & 255) / 255.f;
        float oa = a + da * (1 - a);
        auto ch = [&](int sh, int c) { float dc = ((d >> sh) & 255) / 255.f; return (int)(255 * (c / 255.f * a + dc * (1 - a))); };
        d = ((uint32_t)(oa * 255) << 24) | (ch(16, r) << 16) | (ch(8, g) << 8) | ch(0, b);
    }
    void disc(float cx, float cy, float rad, int r, int g, int b, float a, float soft = 1.5f) {
        int x0 = (int)(cx - rad - soft), x1 = (int)(cx + rad + soft), y0 = (int)(cy - rad - soft), y1 = (int)(cy + rad + soft);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float d = std::hypot(x - cx, y - cy);
                float k = std::min(1.f, std::max(0.f, (rad + soft - d) / (2 * soft)));
                if (k > 0) blend(x, y, r * k, g * k, b * k, a * k);
            }
    }
    void ring(float cx, float cy, float rad, float th, int r, int g, int b, float a) {
        int R = (int)(rad + th + 2);
        for (int y = (int)cy - R; y <= (int)cy + R; y++)
            for (int x = (int)cx - R; x <= (int)cx + R; x++) {
                float d = std::fabs(std::hypot(x - cx, y - cy) - rad);
                float k = std::min(1.f, std::max(0.f, (th - d) / th));
                if (k > 0) blend(x, y, r * k, g * k, b * k, a * k);
            }
    }
};

static void pushLayered(HWND h, int x, int y, int w, int hgt, const uint32_t *src, bool keepPos = false) {
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -hgt; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, src, (size_t)w * hgt * 4);
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT pos{x, y}, zero{0, 0}; SIZE sz{w, hgt};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(h, scr, keepPos ? nullptr : &pos, &sz, mem, &zero, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, scr);
}

static void renderVignette(const RECT &m) {
    int w = m.right - m.left, h = m.bottom - m.top;
    std::vector<uint32_t> px((size_t)w * h, 0);
    float R = std::min(w, h) * 0.11f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float d = (float)std::min(std::min(x, y), std::min(w - 1 - x, h - 1 - y));
            if (d >= R) continue;
            float k = 1 - d / R; k = k * k;
            float a = k * 0.62f;
            px[(size_t)y * w + x] = ((uint32_t)(a * 255) << 24) | ((uint32_t)(a * 35) << 16) | ((uint32_t)(a * 120) << 8) | (uint32_t)(a * 255);
        }
    pushLayered(gVig, m.left, m.top, w, h, px.data());
}

static HWND makeOverlay(const wchar_t *cls, bool &excluded) {
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             cls, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    excluded = h && SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
    return h;
}

// HUD can be dragged while paused; position is kept in cookiebot_hud.txt
static std::atomic<bool> gHudDrag{false};
static int gHudDX = -1, gHudDY = -1;
static void loadHudPos() { if (FILE *f = fopen("cookiebot_hud.txt", "r")) { if (fscanf(f, "%d %d", &gHudDX, &gHudDY) != 2) gHudDX = gHudDY = -1; fclose(f); } }
static LRESULT CALLBACK hudProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTCAPTION;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ENTERSIZEMOVE: gHudDrag = true; return 0;
    case WM_EXITSIZEMOVE: {
        RECT r; GetWindowRect(h, &r);
        gHudDX = std::max(0, (int)(r.left - gWork.left)); gHudDY = std::max(0, (int)(gWork.bottom - r.bottom));
        if (FILE *f = fopen("cookiebot_hud.txt", "w")) { fprintf(f, "%d %d\n", gHudDX, gHudDY); fclose(f); }
        gHudDrag = false;
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void overlayThread() {
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"CookieBotFx";
    RegisterClassW(&wc);
    bool e1 = false, e2 = false;
    gVig = makeOverlay(L"CookieBotFx", e1);
    gFx = makeOverlay(L"CookieBotFx", e2);
    bool e3 = false, e4 = false;
    WNDCLASSW hc{}; hc.lpfnWndProc = hudProc; hc.hInstance = GetModuleHandleW(nullptr); hc.lpszClassName = L"CookieBotHud";
    hc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
    RegisterClassW(&hc);
    gHudWnd = makeOverlay(L"CookieBotHud", e3);
    loadHudPos();
    bool hudGrab = false;
    gPath = makeOverlay(L"CookieBotFx", e4);
    bool e5 = false, e6 = false, e7 = false;
    gRead = makeOverlay(L"CookieBotFx", e5);
    gScan = makeOverlay(L"CookieBotFx", e6);
    gDead = makeOverlay(L"CookieBotFx", e7);
    gHiddenFromCapture = e1 && e2 && e3 && e4 && e5 && e6 && e7;
    bool scanShown = false, deadShown = false;
    std::vector<uint32_t> sbuf, dbuf;
    int scanW = 0;
    struct Shard { float x, y, vx, vy, life, age; int r, g, b; };
    std::vector<Shard> shards;
    int lastHits = 0;
    double lastFrame = now(), shakeUntil = 0;
    std::vector<std::vector<std::pair<float, float>>> cracks;
    bool readShown = false;
    Rect readDrawn{0, 0, 0, 0};
    std::vector<uint32_t> rbuf;
    bool pathShown = false;
    std::vector<uint32_t> pbuf;
    bool hudShown = false;
    double lastHud = 0;
    RECT lastMon{};
    bool shown = false;
    std::vector<uint32_t> buf(FXS * FXS);
    double t0 = now();
    while (!gQuit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        bool on = cfgFx && gOverlayOn && !gPaused && gameFocused();
        if (on != shown) {
            if (on) {
                if (memcmp(&lastMon, &gMon, sizeof(RECT)) != 0) { renderVignette(gMon); lastMon = gMon; }
                ShowWindow(gVig, SW_SHOWNOACTIVATE); ShowWindow(gFx, SW_SHOWNOACTIVATE);
            } else { ShowWindow(gVig, SW_HIDE); ShowWindow(gFx, SW_HIDE); ShowWindow(gPath, SW_HIDE); pathShown = false; ShowWindow(gRead, SW_HIDE); readShown = false; ShowWindow(gScan, SW_HIDE); scanShown = false; scanW = 0; ShowWindow(gDead, SW_HIDE); deadShown = false; }
            shown = on; gVigVisible = on;
        }
        bool hudOn = ((cfgFx || gPromptOpen) && gHwnd && gameFocused() && !gQuit) || cc::hudWanted();
        if (hudOn != hudShown) { ShowWindow(gHudWnd, hudOn ? SW_SHOWNOACTIVATE : SW_HIDE); hudShown = hudOn; }
        if (hudOn && now() - lastHud > 0.1) { renderHud(); lastHud = now(); }
        bool grab = gPaused && !cc::active();
        if (grab != hudGrab) {
            LONG ex = GetWindowLongW(gHudWnd, GWL_EXSTYLE);
            SetWindowLongW(gHudWnd, GWL_EXSTYLE, grab ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT));
            hudGrab = grab;
        }
        if (shown) {
            POINT c; GetCursorPos(&c);
            int wx = c.x - FXS / 2, wy = c.y - FXS / 2;
            std::fill(buf.begin(), buf.end(), 0);
            Canvas cv{FXS, FXS, buf.data()};
            float t = (float)(now() - t0), cx = FXS / 2.f, cy = FXS / 2.f;
            cv.disc(cx, cy, 30, 40, 140, 255, 0.10f, 14);
            for (int i = 0; i < 3; i++) {
                float ang = t * 3.2f + i * 2.094f;
                cv.disc(cx + std::cos(ang) * 30, cy + std::sin(ang) * 30, 3.2f, 120, 200, 255, 0.85f);
                for (int k = 1; k <= 5; k++) {
                    float aa = ang - k * 0.12f;
                    cv.disc(cx + std::cos(aa) * 30, cy + std::sin(aa) * 30, 2.6f - k * 0.35f, 80, 170, 255, 0.5f - k * 0.08f);
                }
            }
            std::vector<Burst> bs;
            { std::lock_guard<std::mutex> lk(gFxMu); bs = gBursts; }
            for (auto &b : bs) {
                float age = (float)(now() - b.t), life = b.big ? 0.8f : 0.35f;
                if (age > life) continue;
                float k = age / life, bx = (float)(b.x - wx), by = (float)(b.y - wy);
                float maxR = b.big ? 110.f : 46.f;
                cv.ring(bx, by, 8 + k * maxR, 2.5f, 150, 215, 255, (1 - k) * 0.9f);
                int n = b.big ? 14 : 6;
                for (int i = 0; i < n; i++) {
                    float ang = i * 6.2832f / n + b.t * 7;
                    float r = 6 + k * maxR * 1.1f;
                    cv.disc(bx + std::cos(ang) * r, by + std::sin(ang) * r, 2.4f * (1 - k) + 0.5f, 210, 240, 255, (1 - k));
                }
            }
            pushLayered(gFx, wx, wy, FXS, FXS, buf.data());

            // dotted path to where the cursor is headed
            PathT pt; { std::lock_guard<std::mutex> lk(gFxMu); pt = gPathT; }
            bool draw = false;
            double ax = c.x, ay = c.y, bx = 0, by = 0, amp = 0;
            if (pt.glide) { ax = pt.sx; ay = pt.sy; bx = pt.tx; by = pt.ty; amp = pt.amp; draw = true; }
            else if (pt.next) { bx = pt.nx; by = pt.ny; draw = true; }
            double dd = std::hypot(bx - ax, by - ay);
            if (draw && dd > 70 && !gCapturing) {
                if (!pt.glide) amp = dd * 0.08;
                double ux = (bx - ax) / dd, uy = (by - ay) / dd, px = -uy, py = ux;
                auto at = [&](double u, double &x, double &y) { double b = std::sin(3.14159 * u) * amp; x = ax + (bx - ax) * u + px * b; y = ay + (by - ay) * u + py * b; };
                int M = 40;
                double x0 = std::min(ax, bx) - std::fabs(amp) - M, y0 = std::min(ay, by) - std::fabs(amp) - M;
                double x1 = std::max(ax, bx) + std::fabs(amp) + M, y1 = std::max(ay, by) + std::fabs(amp) + M;
                int pw = (int)(x1 - x0), ph = (int)(y1 - y0);
                pbuf.assign((size_t)pw * ph, 0);
                Canvas pc{pw, ph, pbuf.data()};
                int n = std::max(3, (int)(dd / 15));
                double ph0 = std::fmod(t * 1.4, 1.0);
                for (int i = 0; i <= n; i++) {
                    double u = (i + ph0) / n;
                    if (u > 1) continue;
                    double x, y; at(u, x, y);
                    if (std::hypot(x - c.x, y - c.y) < 38 || std::hypot(x - bx, y - by) < 24) continue;
                    float a = (float)(0.6 + 0.4 * u);
                    pc.disc((float)(x - x0), (float)(y - y0), 5.2f, 5, 15, 40, a * 0.7f);
                    pc.disc((float)(x - x0), (float)(y - y0), 3.8f, 110, 210, 255, a);
                }
                float tx = (float)(bx - x0), ty = (float)(by - y0), pulse = 14 + 3 * std::sin(t * 6);
                pc.ring(tx, ty, pulse, 5.5f, 5, 15, 40, 0.7f);
                pc.ring(tx, ty, pulse, 3.5f, 150, 220, 255, 1.f);
                for (int i = 0; i < 4; i++) {
                    float ang = t * 1.5f + i * 1.5708f;
                    pc.disc(tx + std::cos(ang) * 24, ty + std::sin(ang) * 24, 3.4f, 150, 220, 255, 1.f);
                }
                if (pt.dir) for (int i = 0; i < 3; i++) {
                    float yy = ty + pt.dir * (24 + i * 7 + std::fmod(t * 20, 7.f));
                    pc.disc(tx - 6 + i * 0.f, yy - pt.dir * 4, 1.8f, 150, 215, 255, 0.7f - i * 0.2f);
                    pc.disc(tx + 6, yy - pt.dir * 4, 1.8f, 150, 215, 255, 0.7f - i * 0.2f);
                    pc.disc(tx, yy, 1.8f, 150, 215, 255, 0.7f - i * 0.2f);
                }
                pushLayered(gPath, (int)x0, (int)y0, pw, ph, pbuf.data());
                if (!pathShown) { ShowWindow(gPath, SW_SHOWNOACTIVATE); pathShown = true; }
            } else if (pathShown) { ShowWindow(gPath, SW_HIDE); pathShown = false; }

            // green box around whatever it's reading
            Rect rr; double rat;
            { std::lock_guard<std::mutex> lk(gFxMu); rr = gReadRect; rat = gReadAt; }
            if (now() - rat < 1.2 && rr.x1 > rr.x0 && !gCapturing) {
                if (memcmp(&rr, &readDrawn, sizeof rr) != 0 || !readShown) {
                    int m = 8, w = rr.x1 - rr.x0 + 2 * m, hh = rr.y1 - rr.y0 + 2 * m;
                    rbuf.assign((size_t)w * hh, 0);
                    auto put = [&](int x, int y, int r, int g, int b, float a) {
                        if (x < 0 || y < 0 || x >= w || y >= hh) return;
                        rbuf[(size_t)y * w + x] = ((uint32_t)(a * 255) << 24) | ((uint32_t)(r * a) << 16) | ((uint32_t)(g * a) << 8) | (uint32_t)(b * a);
                    };
                    for (int y = m; y < hh - m; y++) for (int x = m; x < w - m; x++) put(x, y, 40, 255, 90, 0.07f);
                    auto band = [&](int t0, int t1, int r, int g, int b, float a) {
                        for (int t = t0; t < t1; t++) {
                            for (int x = t; x < w - t; x++) { put(x, t, r, g, b, a); put(x, hh - 1 - t, r, g, b, a); }
                            for (int y = t; y < hh - t; y++) { put(t, y, r, g, b, a); put(w - 1 - t, y, r, g, b, a); }
                        }
                    };
                    band(2, 3, 0, 40, 10, 0.8f);
                    band(3, 7, 50, 255, 100, 1.f);
                    band(7, 8, 0, 40, 10, 0.8f);
                    int cl = std::min(40, std::min(w, hh) / 3);
                    for (int i = 0; i < cl; i++) for (int t = 0; t < 3; t++) {
                        put(i, t, 170, 255, 190, 1); put(t, i, 170, 255, 190, 1); put(w - 1 - i, t, 170, 255, 190, 1); put(w - 1 - t, i, 170, 255, 190, 1);
                        put(i, hh - 1 - t, 170, 255, 190, 1); put(t, hh - 1 - i, 170, 255, 190, 1);
                        put(w - 1 - i, hh - 1 - t, 170, 255, 190, 1); put(w - 1 - t, hh - 1 - i, 170, 255, 190, 1);
                    }
                    pushLayered(gRead, rr.x0 - m, rr.y0 - m, w, hh, rbuf.data());
                    readDrawn = rr;
                }
                if (!readShown) { ShowWindow(gRead, SW_SHOWNOACTIVATE); readShown = true; }
            } else if (readShown) { ShowWindow(gRead, SW_HIDE); readShown = false; }

            // scanning sweep inside the read box
            if (readShown && rr.y1 - rr.y0 > 30) {
                int w = rr.x1 - rr.x0, sh = 26;
                if (w != scanW) {
                    scanW = w;
                    sbuf.assign((size_t)w * sh, 0);
                    for (int y = 0; y < sh; y++) {
                        float k = 1 - std::fabs(y - sh * 0.75f) / (sh * 0.75f);
                        if (y > sh * 0.75f) k = 1 - (y - sh * 0.75f) / (sh * 0.25f);
                        k = std::max(0.f, k); k = k * k;
                        for (int x = 0; x < w; x++) {
                            float edge = std::min(1.f, std::min(x, w - 1 - x) / 30.f);
                            float a = k * edge * (y >= sh - 3 ? 1.f : 0.45f);
                            sbuf[(size_t)y * w + x] = ((uint32_t)(a * 255) << 24) | ((uint32_t)(a * 120) << 16) | ((uint32_t)(a * 255) << 8) | (uint32_t)(a * 150);
                        }
                    }
                    pushLayered(gScan, rr.x0, rr.y0, w, sh, sbuf.data());
                }
                float ph = (float)std::fmod((now() - rat) * 1.6, 1.0);
                int sy = rr.y0 + (int)((rr.y1 - rr.y0 - sh) * ph);
                SetWindowPos(gScan, HWND_TOPMOST, rr.x0, sy, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                if (!scanShown) { ShowWindow(gScan, SW_SHOWNOACTIVATE); scanShown = true; }
            } else if (scanShown) { ShowWindow(gScan, SW_HIDE); scanShown = false; scanW = 0; }

            // deadeye on the baker puzzle
            double dt = now() - lastFrame; lastFrame = now();
            if (gPuzzle && !gCapturing) {
                const int D = 600; const float C = D / 2.f;
                int hits = gPuzzleHits;
                float prog = std::min(1.f, hits / 530.f);
                if (hits < lastHits || cracks.empty()) {
                    cracks.clear();
                    for (int i = 0; i < 9; i++) {
                        std::vector<std::pair<float, float>> c;
                        float ang = i * 0.698f + (rand() % 100) / 200.f, x = 0, y = 0;
                        for (int j = 0; j < 14; j++) {
                            ang += ((rand() % 100) - 50) / 160.f;
                            x += std::cos(ang) * 18; y += std::sin(ang) * 18;
                            c.push_back({x, y});
                        }
                        cracks.push_back(c);
                    }
                    lastHits = 0;
                }
                for (int h = lastHits; h < hits; h++) {
                    for (int i = 0; i < 7; i++) {
                        float a = (rand() % 628) / 100.f, sp = 160 + rand() % 260;
                        int pick = rand() % 3;
                        shards.push_back({0, 0, std::cos(a) * sp, std::sin(a) * sp - 80, 0.5f + (rand() % 40) / 100.f, 0,
                                          255, pick == 0 ? 60 : pick == 1 ? 140 : 210, pick == 2 ? 120 : 30});
                    }
                    shakeUntil = now() + 0.07;
                }
                lastHits = hits;
                if (shards.size() > 500) shards.erase(shards.begin(), shards.begin() + (shards.size() - 500));
                dbuf.assign((size_t)D * D, 0);
                Canvas dc{D, D, dbuf.data()};
                float tt = (float)(now() - t0);
                // red tint disc
                dc.disc(C, C, 250, 120, 0, 0, 0.10f + 0.06f * prog, 60);
                // cracks grow with progress
                for (auto &c : cracks) {
                    int n = (int)(c.size() * std::min(1.f, prog * 1.6f + 0.05f));
                    for (int j = 0; j < n; j++) {
                        float px = C + c[j].first, py = C + c[j].second;
                        float qx = j ? C + c[j - 1].first : C, qy = j ? C + c[j - 1].second : C;
                        for (int s2 = 0; s2 <= 6; s2++) {
                            float u = s2 / 6.f;
                            dc.disc(qx + (px - qx) * u, qy + (py - qy) * u, 2.2f - j * 0.1f, 255, 70, 50, 0.85f);
                        }
                    }
                }
                // outer segmented ring, spinning
                for (int i = 0; i < 48; i++) {
                    if (i % 4 == 3) continue;
                    float a = tt * 0.9f + i * 6.2832f / 48;
                    dc.disc(C + std::cos(a) * 250, C + std::sin(a) * 250, 4.5f, 30, 0, 0, 0.7f);
                    dc.disc(C + std::cos(a) * 250, C + std::sin(a) * 250, 3.2f, 255, 45, 45, 1.f);
                }
                // progress arc
                int segs2 = (int)(160 * prog);
                for (int i = 0; i < segs2; i++) {
                    float a = -1.5708f + i * 6.2832f / 160;
                    dc.disc(C + std::cos(a) * 222, C + std::sin(a) * 222, 6.f, 255, 210, 60, 0.95f);
                }
                // inner rings
                float pulse = 1 + 0.08f * std::sin(tt * 14);
                dc.ring(C, C, 150 * pulse, 6, 30, 0, 0, 0.7f);
                dc.ring(C, C, 150 * pulse, 4, 255, 40, 40, 1.f);
                dc.ring(C, C, 70, 3, 255, 90, 90, 0.9f);
                // crosshair arms with a gap, counter-spinning ticks
                for (int arm = 0; arm < 4; arm++) {
                    float a = arm * 1.5708f;
                    for (float r = 45; r < 290; r += 2.5f) {
                        if (r > 155 && r < 205) continue;
                        float x = C + std::cos(a) * r, y = C + std::sin(a) * r;
                        dc.disc(x, y, 4.2f, 30, 0, 0, 0.6f);
                        dc.disc(x, y, 2.8f, 255, 50, 50, 1.f);
                    }
                    float b = -tt * 2.2f + arm * 1.5708f + 0.785f;
                    for (float r = 100; r < 130; r += 2.5f) dc.disc(C + std::cos(b) * r, C + std::sin(b) * r, 3.f, 255, 120, 120, 0.95f);
                }
                dc.disc(C, C, 7 + 2 * std::sin(tt * 20), 255, 230, 230, 1.f);
                // shards
                for (auto &sd : shards) {
                    sd.age += (float)dt; sd.x += sd.vx * (float)dt; sd.y += sd.vy * (float)dt; sd.vy += 520 * (float)dt;
                    float k = 1 - sd.age / sd.life;
                    if (k > 0) dc.disc(C + sd.x, C + sd.y, 1.5f + 3 * k, sd.r, sd.g, sd.b, k);
                }
                shards.erase(std::remove_if(shards.begin(), shards.end(), [](const Shard &a) { return a.age > a.life; }), shards.end());
                int jx = now() < shakeUntil ? (rand() % 9) - 4 : 0, jy = now() < shakeUntil ? (rand() % 9) - 4 : 0;
                pushLayered(gDead, gPuzzleX - D / 2 + jx, gPuzzleY - D / 2 + jy, D, D, dbuf.data());
                if (!deadShown) { ShowWindow(gDead, SW_SHOWNOACTIVATE); deadShown = true; }
            } else if (deadShown) { ShowWindow(gDead, SW_HIDE); deadShown = false; shards.clear(); cracks.clear(); lastHits = 0; }
            { std::lock_guard<std::mutex> lk(gFxMu);
              gBursts.erase(std::remove_if(gBursts.begin(), gBursts.end(), [](const Burst &b) { return now() - b.t > 1; }), gBursts.end()); }
        }
        Sleep(16);
    }
    ShowWindow(gVig, SW_HIDE); ShowWindow(gFx, SW_HIDE); ShowWindow(gHudWnd, SW_HIDE); ShowWindow(gPath, SW_HIDE); ShowWindow(gRead, SW_HIDE); ShowWindow(gScan, SW_HIDE); ShowWindow(gDead, SW_HIDE);
}

// ================= input =================
static void sendMouse(int x, int y, DWORD flags, int data = 0) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((x - vx) * 65535.0 / (vw - 1) + 0.5);
    in.mi.dy = (LONG)((y - vy) * 65535.0 / (vh - 1) + 0.5);
    in.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof in);
}

static double gLastClick = 0;
static Layout L;
static std::vector<Rect> gDanger;
static bool inRect(int x, int y, const Rect &r) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; }

static bool pointSafe(int x, int y) {
    if (!L.ok || !inRect(x, y, L.game)) return false;
    if (!inRect(x, y, Rect{gMon.left, gMon.top, gMon.right - 1, gMon.bottom - 1})) return false;
    for (auto &d : gDanger) if (inRect(x, y, d)) return false;
    return true;
}

// eased glide along a slight curve
static bool glideTo(int x, int y) {
    POINT p; GetCursorPos(&p);
    double dx = x - p.x, dy = y - p.y, d = std::hypot(dx, dy);
    if (d < 25) { sendMouse(x, y, 0); return true; }
    double dur = std::min(0.26, std::max(0.07, d / 3800.0 + 0.06));
    double amp = d * 0.08 * ((rand() & 1) ? 1 : -1), nx = -dy / d, ny = dx / d;
    double t0 = now();
    { std::lock_guard<std::mutex> lk(gFxMu); gPathT.glide = true; gPathT.sx = p.x; gPathT.sy = p.y; gPathT.tx = x; gPathT.ty = y; gPathT.amp = amp; }
    struct End { ~End() { std::lock_guard<std::mutex> lk(gFxMu); gPathT.glide = false; } } endGlide;
    for (;;) {
        if (!canAct()) return false;
        double t = std::min(1.0, (now() - t0) / dur);
        double e = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
        double bend = std::sin(3.14159 * t) * amp;
        sendMouse((int)(p.x + dx * e + nx * bend), (int)(p.y + dy * e + ny * bend), 0);
        if (t >= 1) break;
        Sleep(5);
    }
    sendMouse(x, y, 0);
    return true;
}

static bool safeClick(int x, int y, bool big = false) {
    if (!canAct() || !pointSafe(x, y)) return false;
    if (!glideTo(x, y)) return false;
    double gap = 1.0 / std::min(12.0, cfgCps) + 0.0004;
    for (;;) {
        double left = gLastClick + gap - now();
        if (left <= 0) break;
        if (left > 0.003) Sleep(1); else YieldProcessor();
    }
    if (!canAct() || !pointSafe(x, y)) return false;
    sendMouse(x, y, MOUSEEVENTF_LEFTDOWN);
    sendMouse(x, y, MOUSEEVENTF_LEFTUP);
    gLastClick = now();
    fxClick(x, y, big);
    { std::lock_guard<std::mutex> lk(gHud.mu); gHud.clicks++; }
    return true;
}

static bool safeWheel(int x, int y, int notches) {
    if (!canAct() || !pointSafe(x, y)) return false;
    if (!glideTo(x, y)) return false;
    int step = notches > 0 ? -WHEEL_DELTA : WHEEL_DELTA;
    for (int i = 0; i < std::abs(notches); i++) {
        if (!canAct()) return false;
        sendMouse(x, y, MOUSEEVENTF_WHEEL, step);
        Sleep(15);
    }
    return true;
}

static void pressF5() {
    if (gPaused || gQuit || !gameFocused()) return;
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_F5;
    in[1] = in[0]; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static std::atomic<bool> gCcBusy{false};
static void ccStartAsync(int m, double delay) {
    if (gCcBusy.exchange(true)) return;
    std::thread([m, delay] {
        if (delay > 0) Sleep((DWORD)(delay * 1000));
        cc::start(m);
        gCcBusy = false;
    }).detach();
}

// ---------- user input watcher ----------
static LRESULT CALLBACK mouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto *m = (MSLLHOOKSTRUCT *)lp;
        if (!(m->flags & LLMHF_INJECTED)) cc::onPhys();
        if (!(m->flags & LLMHF_INJECTED) && !cc::active() && !gPaused && now() > gResumeAt) {
            gPaused = true;
            logf("You touched the mouse - paused. F8 or Resume to continue.");
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void promptClose() { std::lock_guard<std::mutex> lk(gPromptMu); gPromptText.clear(); gPromptOpen = false; }

static LRESULT CALLBACK keyHook(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    auto *k = (KBDLLHOOKSTRUCT *)lp;
    if (k->flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);
    DWORD vk = k->vkCode;
    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    if (cc::asking()) {
        if (GetForegroundWindow() != cc::target() || vk == VK_F6 || vk == VK_F7 || vk == VK_F8 || vk == VK_F9) return CallNextHookEx(nullptr, code, wp, lp);
        if (!down) return 1;
        std::lock_guard<std::mutex> lk(gPromptMu);
        gPromptLast = now();
        if (vk == VK_RETURN) { if (!gPromptText.empty()) cc::answer(gPromptText); gPromptText.clear(); gPromptOpen = false; }
        else if (vk == VK_ESCAPE) { cc::answer("let me show you"); gPromptText.clear(); gPromptOpen = false; }
        else if (vk == VK_BACK) { if (!gPromptText.empty()) gPromptText.pop_back(); }
        else if (gPromptText.size() < 80) {
            if (vk >= 'A' && vk <= 'Z') gPromptText += (char)(vk - 'A' + 'a');
            else if (vk >= '0' && vk <= '9') gPromptText += (char)vk;
            else if (vk == VK_SPACE) gPromptText += ' ';
            else if (vk == VK_OEM_7) gPromptText += '\'';
        }
        return 1;
    }
    cc::onKey(vk, down);
    if (vk != VK_F6 && vk != VK_F7 && vk != VK_F8 && vk != VK_F9) cc::onPhys();
    bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_LWIN) & 0x8000);
    if (vk == VK_F8 || vk == VK_F9 || mods) return CallNextHookEx(nullptr, code, wp, lp);
    if (!gPromptOpen) {
        if (down && vk >= 'A' && vk <= 'Z' && gOverlayOn && !gPaused && gHwnd && GetForegroundWindow() == gHwnd) {
            std::lock_guard<std::mutex> lk(gPromptMu);
            gPromptText = std::string(1, (char)(vk - 'A' + 'a'));
            gPromptOpen = true; gPromptLast = now();
            return 1;
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (GetForegroundWindow() != gHwnd) { promptClose(); return CallNextHookEx(nullptr, code, wp, lp); }
    if (!down) return 1;
    std::lock_guard<std::mutex> lk(gPromptMu);
    gPromptLast = now();
    bool shift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
    if (vk == VK_ESCAPE) { gPromptText.clear(); gPromptOpen = false; }
    else if (vk == VK_RETURN) {
        if (!gPromptText.empty()) gCmds.push_back(gPromptText);
        gPromptText.clear(); gPromptOpen = false;
    } else if (vk == VK_BACK) { if (!gPromptText.empty()) gPromptText.pop_back(); if (gPromptText.empty()) gPromptOpen = false; }
    else if (gPromptText.size() < 80) {
        if (vk >= 'A' && vk <= 'Z') gPromptText += (char)(vk - 'A' + 'a');
        else if (vk >= '0' && vk <= '9' && !shift) gPromptText += (char)vk;
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) gPromptText += (char)('0' + vk - VK_NUMPAD0);
        else if (vk == VK_SPACE) gPromptText += ' ';
        else if (vk == VK_OEM_PERIOD || vk == VK_DECIMAL) gPromptText += '.';
        else if (vk == VK_OEM_7) gPromptText += '\'';
    }
    return 1;
}

static void inputThread() {
    HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(nullptr), 0);
    HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, keyHook, GetModuleHandleW(nullptr), 0);
    bool f8 = false, f9 = false, f6 = false, f7 = false;
    while (!gQuit) {
        bool k6 = GetAsyncKeyState(VK_F6) & 0x8000, k7 = GetAsyncKeyState(VK_F7) & 0x8000;
        if (k6 && !f6) ccStartAsync(1, 0);
        if (k7 && !f7) ccStartAsync(2, 0);
        f6 = k6; f7 = k7;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        bool k8 = GetAsyncKeyState(VK_F8) & 0x8000, k9 = GetAsyncKeyState(VK_F9) & 0x8000;
        if (k8 && !f8 && !cc::active()) {
            if (gPaused) { gResumeAt = now() + 3; gPaused = false; logf("Resuming in 3 s - hands off the mouse."); }
            else { gPaused = true; logf("Paused (F8 to resume)"); }
        }
        if (k9 && !f9) { gQuit = true; logf("Quitting."); }
        f8 = k8; f9 = k9;
        if (gPromptOpen && !cc::asking() && now() - gPromptLast > 30) promptClose();
        Sleep(10);
    }
    if (h) UnhookWindowsHookEx(h);
    if (hk) UnhookWindowsHookEx(hk);
}

// ================= screen capture =================
struct Img { int w = 0, h = 0; std::vector<uint32_t> px; };

static Img grab(const Rect &r, double scale, bool smooth) {
    Img im;
    int sw = r.x1 - r.x0, sh = r.y1 - r.y0;
    if (sw <= 0 || sh <= 0) return im;
    bool hide = !gHiddenFromCapture && (gVigVisible || IsWindowVisible(gHudWnd));
    bool hudWas = hide && IsWindowVisible(gHudWnd), vigWas = hide && gVigVisible;
    if (hide) { gCapturing = true; ShowWindow(gVig, SW_HIDE); ShowWindow(gFx, SW_HIDE); ShowWindow(gHudWnd, SW_HIDE); ShowWindow(gPath, SW_HIDE); ShowWindow(gRead, SW_HIDE); ShowWindow(gScan, SW_HIDE); ShowWindow(gDead, SW_HIDE); Sleep(40); }
    im.w = std::max(1, (int)(sw * scale)); im.h = std::max(1, (int)(sh * scale));
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = im.w; bi.bmiHeader.biHeight = -im.h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    SetStretchBltMode(mem, smooth ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(mem, 0, 0, nullptr);
    StretchBlt(mem, 0, 0, im.w, im.h, scr, r.x0, r.y0, sw, sh, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    im.px.assign((uint32_t *)bits, (uint32_t *)bits + (size_t)im.w * im.h);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, scr);
    if (vigWas) { ShowWindow(gVig, SW_SHOWNOACTIVATE); ShowWindow(gFx, SW_SHOWNOACTIVATE); }
    if (hudWas) ShowWindow(gHudWnd, SW_SHOWNOACTIVATE);
    gCapturing = false;
    return im;
}

// ================= Windows OCR (WinRT, raw vtables) =================
struct RectF { float X, Y, W, H; };
struct IVecView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32 *) = 0;
};
struct IAsyncOp : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void **) = 0;
};
struct IAsyncInf : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct IOcrWordX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_BoundingRect(RectF *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING *) = 0;
};
struct IOcrLineX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Words(IVecView **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING *) = 0;
};
struct IOcrResultX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Lines(IVecView **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TextAngle(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING *) = 0;
};
struct IOcrEngineX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RecognizeAsync(IInspectable *, IAsyncOp **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RecognizerLanguage(void **) = 0;
};
struct IOcrEngineStaticsX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MaxImageDimension(UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvailableRecognizerLanguages(IVecView **) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLanguageSupported(void *, boolean *) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromLanguage(void *, IOcrEngineX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromUserProfileLanguages(IOcrEngineX **) = 0;
};
struct ISoftwareBitmapStaticsX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Copy(void *, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Convert(void *, int, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ConvertWithAlpha(void *, int, int, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCopyFromBuffer(IInspectable *, int, int, int, IInspectable **) = 0;
};

static const GUID IID_OcrStatics = {0x5bffa85a, 0x3384, 0x3540, {0x99, 0x40, 0x69, 0x91, 0x20, 0xd4, 0x28, 0xa8}};
static const GUID IID_SBStatics = {0xdf0385db, 0x672f, 0x4a9d, {0x80, 0x6e, 0xc2, 0x44, 0x2f, 0x34, 0x3e, 0x86}};
static const GUID IID_AsyncInf = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID IID_Insp = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
static const GUID IID_ActFactory = {0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID IID_BufFactory = {0x71af914d, 0xc10f, 0x484b, {0xbc, 0x50, 0x14, 0xbc, 0x62, 0x3b, 0x3a, 0x27}};
static const GUID IID_BufByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

static IOcrEngineX *gOcr = nullptr;
static ISoftwareBitmapStaticsX *gSB = nullptr;
static IBufferFactory *gBufF = nullptr;
static UINT32 gOcrMax = 2600;

static HSTRING hs(const wchar_t *s) { HSTRING h; WindowsCreateString(s, (UINT32)wcslen(s), &h); return h; }
static std::string fromHs(HSTRING h) {
    UINT32 n; const wchar_t *w = WindowsGetStringRawBuffer(h, &n);
    std::string o(n * 3 + 1, 0);
    int m = WideCharToMultiByte(CP_UTF8, 0, w, n, &o[0], (int)o.size(), nullptr, nullptr);
    o.resize(m); return o;
}

static bool ocrInit() {
    RoInitialize(RO_INIT_MULTITHREADED);
    IOcrEngineStaticsX *st = nullptr;
    HSTRING c = hs(L"Windows.Media.Ocr.OcrEngine");
    HRESULT hr = RoGetActivationFactory(c, IID_OcrStatics, (void **)&st);
    if (FAILED(hr)) {
        IInspectable *f = nullptr;
        if (SUCCEEDED(RoGetActivationFactory(c, IID_Insp, (void **)&f))) {
            ULONG n = 0; IID *ids = nullptr;
            if (SUCCEEDED(f->GetIids(&n, &ids))) {
                for (ULONG i = 0; i < n && !st; i++)
                    if (!IsEqualGUID(ids[i], IID_ActFactory)) f->QueryInterface(ids[i], (void **)&st);
                CoTaskMemFree(ids);
            }
            f->Release();
        }
    }
    WindowsDeleteString(c);
    if (!st) { logf("ERROR: Windows OCR not available (hr=0x%08lx). Needs Windows 10/11.", hr); return false; }
    st->get_MaxImageDimension(&gOcrMax);
    st->TryCreateFromUserProfileLanguages(&gOcr);
    if (!gOcr) {
        IVecView *langs = nullptr;
        if (SUCCEEDED(st->get_AvailableRecognizerLanguages(&langs)) && langs) {
            UINT32 n = 0; langs->get_Size(&n);
            if (n) { void *lang = nullptr; langs->GetAt(0, &lang); if (lang) { st->TryCreateFromLanguage(lang, &gOcr); ((IUnknown *)lang)->Release(); } }
            langs->Release();
        }
    }
    st->Release();
    if (!gOcr) { logf("ERROR: no OCR language installed. Settings > Time & Language > Language > add English."); return false; }
    c = hs(L"Windows.Graphics.Imaging.SoftwareBitmap");
    hr = RoGetActivationFactory(c, IID_SBStatics, (void **)&gSB);
    WindowsDeleteString(c);
    if (FAILED(hr)) { logf("ERROR: SoftwareBitmap factory failed 0x%08lx", hr); return false; }
    c = hs(L"Windows.Storage.Streams.Buffer");
    hr = RoGetActivationFactory(c, IID_BufFactory, (void **)&gBufF);
    WindowsDeleteString(c);
    if (FAILED(hr)) { logf("ERROR: Buffer factory failed 0x%08lx", hr); return false; }
    logf("Windows OCR ready (max image %u px)", gOcrMax);
    return true;
}

static std::mutex gOcrMu;
static std::vector<Word> ocrImage(const Img &im, double scale, int ox, int oy) {
    std::lock_guard<std::mutex> ocrLock(gOcrMu);
    std::vector<Word> out;
    if (!gOcr || im.w < 8 || im.h < 8) return out;
    UINT32 bytes = (UINT32)(im.w * im.h * 4);
    IBuffer *buf = nullptr;
    if (FAILED(gBufF->Create(bytes, &buf)) || !buf) return out;
    buf->put_Length(bytes);
    IUnknown *ba = nullptr;
    if (SUCCEEDED(buf->QueryInterface(IID_BufByteAccess, (void **)&ba))) {
        byte *p = nullptr;
        ((Windows::Storage::Streams::IBufferByteAccess *)ba)->Buffer(&p);
        for (size_t i = 0; i < (size_t)im.w * im.h; i++) ((uint32_t *)p)[i] = im.px[i] | 0xFF000000u;
        ba->Release();
    }
    IInspectable *bmp = nullptr;
    HRESULT hr = gSB->CreateCopyFromBuffer((IInspectable *)buf, 87, im.w, im.h, &bmp);
    buf->Release();
    if (FAILED(hr) || !bmp) { logf("SoftwareBitmap create failed 0x%08lx", hr); return out; }
    IAsyncOp *op = nullptr;
    hr = gOcr->RecognizeAsync(bmp, &op);
    bmp->Release();
    if (FAILED(hr) || !op) return out;
    IAsyncInf *info = nullptr;
    op->QueryInterface(IID_AsyncInf, (void **)&info);
    int status = 0; double t0 = now();
    while (info && SUCCEEDED(info->get_Status(&status)) && status == 0 && now() - t0 < 8) Sleep(2);
    if (info) info->Release();
    IOcrResultX *res = nullptr;
    if (status == 1) op->GetResults((void **)&res);
    op->Release();
    if (!res) return out;
    IVecView *lines = nullptr;
    res->get_Lines(&lines);
    UINT32 nl = 0; if (lines) lines->get_Size(&nl);
    for (UINT32 i = 0; i < nl; i++) {
        IOcrLineX *ln = nullptr; lines->GetAt(i, (void **)&ln);
        if (!ln) continue;
        IVecView *words = nullptr; ln->get_Words(&words);
        UINT32 nw = 0; if (words) words->get_Size(&nw);
        for (UINT32 j = 0; j < nw; j++) {
            IOcrWordX *w = nullptr; words->GetAt(j, (void **)&w);
            if (!w) continue;
            RectF r; HSTRING t = nullptr;
            w->get_BoundingRect(&r); w->get_Text(&t);
            out.push_back({fromHs(t), ox + (int)(r.X / scale), oy + (int)(r.Y / scale), std::max(1, (int)(r.W / scale)), std::max(1, (int)(r.H / scale))});
            WindowsDeleteString(t);
            w->Release();
        }
        if (words) words->Release();
        ln->Release();
    }
    if (lines) lines->Release();
    res->Release();
    return out;
}

static void invert(Img &im) { for (auto &p : im.px) p = (~p) & 0x00FFFFFFu; }
static void addUnique(std::vector<Word> &dst, const std::vector<Word> &src) {
    for (auto &w : src) {
        bool dup = false;
        for (auto &d : dst)
            if (std::abs(d.x - w.x) < 8 && std::abs(d.y - w.y) < 8 && (d.text == w.text || std::abs(d.w - w.w) < 6)) { dup = true; break; }
        if (!dup) dst.push_back(w);
    }
}

static thread_local bool tlNoBox = false;
static std::vector<Word> ocrRect(const Rect &r, double want = 2.0, bool alsoInverted = false) {
    static thread_local int depth = 0;
    if (depth == 0 && !tlNoBox) { std::lock_guard<std::mutex> lk(gFxMu); gReadRect = r; gReadAt = now(); }
    depth++;
    struct Dec { ~Dec() { depth--; } } dec;
    std::vector<Word> out;
    int sw = r.x1 - r.x0, sh = r.y1 - r.y0;
    if (sw <= 0 || sh <= 0) return out;
    double maxd = gOcrMax - 8;
    double s = std::min(want, maxd / std::max(sw, sh));
    if (s < 1.0) {
        int tile = (int)(maxd / want), ov = 80;
        for (int y = r.y0; y < r.y1; y += tile - ov) {
            for (int x = r.x0; x < r.x1; x += tile - ov) {
                Rect t{x, y, std::min(r.x1, x + tile), std::min(r.y1, y + tile)};
                addUnique(out, ocrRect(t, want, alsoInverted));
                if (x + tile >= r.x1) break;
            }
            if (y + tile >= r.y1) break;
        }
        return out;
    }
    Img im = grab(r, s, true);
    out = ocrImage(im, s, r.x0, r.y0);
    if (alsoInverted) { invert(im); addUnique(out, ocrImage(im, s, r.x0, r.y0)); }
    return out;
}

static void updateDanger(const std::vector<Seg> &segs) {
    static const char *bad[] = {"reset", "shop", "cart", "visit", "rewards", "collection", "sell", "netlify", "delete",
                                "logout", "log out", "sign out", "wipe", "ascend", "prestige", "fresh start", "checkout", "address", "claim"};
    for (auto &s : segs) {
        if (s.text.size() > 30) continue;
        std::string l = lower(s.text);
        auto toks = split(l);
        for (auto b : bad) {
            bool m = false;
            if (strchr(b, ' ')) m = l.find(b) != std::string::npos;
            else for (auto &t : toks) if (lettersOnly(t) == b) m = true;
            if (!m) continue;
            Rect d{s.x - 12, s.y - 10, s.r() + 12, s.b() + 10};
            bool have = false;
            for (auto &e : gDanger) if (std::abs(e.x0 - d.x0) < 6 && std::abs(e.y0 - d.y0) < 6) have = true;
            if (!have) gDanger.push_back(d);
        }
    }
    if (gDanger.size() > 200) gDanger.erase(gDanger.begin(), gDanger.begin() + 100);
}

// ================= pixel blobs (main cookie + bonus cookies) =================
static inline bool cookieColor(uint32_t p) {
    int b = p & 255, g = (p >> 8) & 255, r = (p >> 16) & 255;
    return r >= 120 && r - b >= 70 && g >= 60 && g <= r - 15 && b <= 140 && r - g <= 120;
}
struct Blob { int x0, y0, x1, y1, n; int cx() const { return (x0 + x1) / 2; } int cy() const { return (y0 + y1) / 2; } };

template <class F> static std::vector<Blob> findBlobsIf(const Img &im, F pred);
static std::vector<Blob> findBlobs(const Img &im) { return findBlobsIf(im, [](uint32_t p) { return cookieColor(p); }); }
template <class F> static std::vector<Blob> findBlobsIf(const Img &im, F cookieColor) {
    std::vector<Blob> out;
    std::vector<uint8_t> seen(im.px.size(), 0);
    std::vector<int> st;
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            size_t i = (size_t)y * im.w + x;
            if (seen[i] || !cookieColor(im.px[i])) continue;
            Blob b{x, y, x, y, 0};
            st.clear(); st.push_back((int)i); seen[i] = 1;
            while (!st.empty()) {
                int k = st.back(); st.pop_back();
                int kx = k % im.w, ky = k / im.w; b.n++;
                b.x0 = std::min(b.x0, kx); b.x1 = std::max(b.x1, kx); b.y0 = std::min(b.y0, ky); b.y1 = std::max(b.y1, ky);
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = kx + dx, ny = ky + dy;
                        if (nx < 0 || ny < 0 || nx >= im.w || ny >= im.h) continue;
                        size_t j = (size_t)ny * im.w + nx;
                        if (!seen[j] && cookieColor(im.px[j])) { seen[j] = 1; st.push_back((int)j); }
                    }
            }
            out.push_back(b);
        }
    return out;
}

// the panel's background colour = most common colour around the image border
static uint32_t gPanelBg = 0;
static int colDist(uint32_t a, uint32_t b) {
    return std::abs((int)((a >> 16) & 255) - (int)((b >> 16) & 255)) + std::abs((int)((a >> 8) & 255) - (int)((b >> 8) & 255)) +
           std::abs((int)(a & 255) - (int)(b & 255));
}
static uint32_t borderMode(const Img &im) {
    std::map<uint32_t, int> h;
    auto add = [&](int x, int y) { h[im.px[(size_t)y * im.w + x] & 0xF8F8F8]++; };
    for (int x = 0; x < im.w; x += 2) { add(x, 0); add(x, im.h - 1); }
    for (int y = 0; y < im.h; y += 2) { add(0, y); add(im.w - 1, y); }
    uint32_t best = 0; int n = -1;
    for (auto &kv : h) if (kv.second > n) { n = kv.second; best = kv.first; }
    return best;
}

static bool locateMainCookie() {
    Rect r = L.left;
    double s = 0.5;
    Img im = grab(r, s, false);
    if (im.w < 20 || im.h < 20) return false;
    auto bl = findBlobs(im);
    const Blob *best = nullptr;
    for (auto &b : bl) if (!best || b.n > best->n) best = &b;
    if (!best || best->n < 400) {
        // cookie skin changed (new flavour etc): take the biggest round thing that isn't background
        uint32_t bg = borderMode(im);
        bl = findBlobsIf(im, [bg](uint32_t p) { return colDist(p & 0xF8F8F8, bg) > 70; });
        best = nullptr;
        for (auto &b : bl) {
            int w = b.x1 - b.x0 + 1, h = b.y1 - b.y0 + 1;
            double ar = (double)w / h;
            if (ar < 0.7 || ar > 1.45 || b.n < 0.45 * w * h || b.n < 400) continue;
            if (b.x0 <= 1 || b.y0 <= 1 || b.x1 >= im.w - 2 || b.y1 >= im.h - 2) continue;
            if (!best || b.n > best->n) best = &b;
        }
        if (!best) { dbgf("cookie not found: %d blobs, panel bg %06X", (int)bl.size(), bg); return false; }
        gPanelBg = bg;
        logf("found the cookie by shape (its colour changed)");
    } else gPanelBg = 0;
    L.cookieX = r.x0 + (int)(best->cx() / s);
    L.cookieY = r.y0 + (int)(best->cy() / s);
    L.cookieR = (int)(std::max(best->x1 - best->x0, best->y1 - best->y0) / s / 2);
    return true;
}

static bool cookieStillThere() {
    Img im = grab(Rect{L.cookieX - 20, L.cookieY - 20, L.cookieX + 20, L.cookieY + 20}, 1, false);
    int n = 0;
    for (auto p : im.px) if (gPanelBg ? colDist(p & 0xF8F8F8, gPanelBg) > 70 : cookieColor(p)) n++;
    return n > (int)im.px.size() / 4;
}

struct Spot { int x, y, hits; double last; };
static std::vector<Spot> gIgnore, gTried;

static std::vector<std::pair<int, int>> spawnedCookies() {
    std::vector<std::pair<int, int>> out;
    if (!L.ok) return out;
    double s = 0.5;
    Img im = grab(L.game, s, false);
    for (auto &b : findBlobs(im)) {
        int w = (int)((b.x1 - b.x0 + 1) / s), h = (int)((b.y1 - b.y0 + 1) / s);
        if (w < 14 || h < 14 || w > 190 || h > 190) continue;
        double ar = (double)w / h;
        if (ar < 0.6 || ar > 1.65) continue;
        if (b.n < 0.35 * (b.x1 - b.x0 + 1) * (b.y1 - b.y0 + 1)) continue;
        int x = L.game.x0 + (int)(b.cx() / s), y = L.game.y0 + (int)(b.cy() / s);
        if (inRect(x, y, L.store) || inRect(x, y, L.bakery)) continue;
        int R = (int)(L.cookieR * 1.25) + 10;
        if (std::abs(x - L.cookieX) < R && std::abs(y - L.cookieY) < R) continue;
        bool ign = false;
        for (auto &g : gIgnore) if (std::abs(g.x - x) < 25 && std::abs(g.y - y) < 25) ign = true;
        if (!ign) out.push_back({x, y});
    }
    return out;
}

static void baselineStatic() {
    gIgnore.clear();
    auto a = spawnedCookies();
    Sleep(700);
    auto b = spawnedCookies();
    for (auto &p : a)
        for (auto &q : b)
            if (std::abs(p.first - q.first) < 10 && std::abs(p.second - q.second) < 10) gIgnore.push_back({p.first, p.second, 0, 0});
}

static double gLastBonus = -1000, gBonusP = 0;
static int gGoldenClicks = 0;

// ================= game reading =================
static std::string gHostKey = "cookiesforu";
static int gMiss = 0;
static double gLastRefresh = -100;
static Econ gEcon;
static double gEconTime = 0;


static std::atomic<long long> gCookieClicks{0};
static long long gClicksAtRead = 0;
static double effRate() {
    double up = gStart > 0 ? now() - gStart : 0;
    if (up < 60 || gCookieClicks < 100) return cfgCps * 0.8;
    return std::min(cfgCps, gCookieClicks / up);
}
static double prodNow(const Econ &e) { return std::max(0.0, e.cps) + std::max(0.0, e.perClick) * effRate(); }
static double bankNow() {
    return gEcon.bank + std::max(0.0, gEcon.cps) * (now() - gEconTime) + std::max(0.0, gEcon.perClick) * (double)(gCookieClicks - gClicksAtRead);
}

static std::mutex gEconMu;
static bool readEconNow(bool quiet = false) {
    std::lock_guard<std::mutex> econLock(gEconMu);
    Rect r = L.left;
    r.y1 = std::min(r.y1, L.cookieY);
    auto segs = segment(ocrRect(r, 2.0));
    Econ e = readEcon(segs, L.left);
    // "0 /sec" in the pixel font often reads as junk ("O @ /"). If the count read fine, fall back to the last cps (or 0).
    if (e.bank >= 0 && e.cps < 0) {
        bool zeroish = false;
        for (auto &sg : segs) { std::string l = lower(sg.text); if (l.find("sec") != std::string::npos || l.find("o @") != std::string::npos) zeroish = true; }
        if (gEconTime > 0 && now() - gEconTime < 120 && gEcon.cps > 0) e.cps = gEcon.cps;
        else if (zeroish || gEcon.cps <= 0) e.cps = 0;
        e.guess = true;
    }
    if (e.bank < 0 || e.cps < 0) {
        if (quiet) return false;
        dbgf("--- econ read FAILED:");
        for (auto &sg : segs) dbgf("  seg (%d,%d) '%s'", sg.x, sg.y, sg.text.c_str());
        return false;
    }
    if (e.perClick < 0) e.perClick = gEcon.perClick > 0 ? gEcon.perClick : 0;
    gEcon = e; gEconTime = now(); gClicksAtRead = gCookieClicks;
    // did a bonus cookie do something?
    if (gLastBonus > 0 && now() - gLastBonus < 8 && gBonusP > 0) {
        if (prodNow(e) > gBonusP * 1.1) { gBrain.bonusHelped++; logf("bonus cookie boosted production %.1fx", prodNow(e) / gBonusP); }
        gLastBonus = -1000;
    }
    return true;
}

static void clickSpawned() {
    if (!cfgGolden) return;
    for (auto &p : spawnedCookies()) {
        Spot *t = nullptr;
        for (auto &s : gTried) if (std::abs(s.x - p.first) < 25 && std::abs(s.y - p.second) < 25) t = &s;
        if (!t) { gTried.push_back({p.first, p.second, 0, now()}); t = &gTried.back(); }
        if (now() - t->last > 20) t->hits = 0;
        if (++t->hits > 3) { gIgnore.push_back(*t); logf("learned: thing at %d,%d isn't a bonus cookie, ignoring it", t->x, t->y); continue; }
        t->last = now();
        if (safeClick(p.first, p.second, true)) {
            gGoldenClicks++; gBrain.bonusClicks++;
            { std::lock_guard<std::mutex> lk(gHud.mu); gHud.bonusRun++; }
            gLastBonus = now(); gBonusP = prodNow(gEcon);
            logf("clicked a bonus cookie at %d,%d", p.first, p.second);
        }
    }
    if (gTried.size() > 100) gTried.erase(gTried.begin(), gTried.begin() + 50);
}

static double gFailSince = 0;
static bool detectLayout() {
    Rect mon{gMon.left, gMon.top, gMon.right, gMon.bottom};
    auto words = ocrRect(mon, 1.5);
    auto allWords = words;
    // only text that is actually part of the game's browser window (not this chat, the panel, other apps)
    words.erase(std::remove_if(words.begin(), words.end(), [&](const Word &w) {
        POINT pt{w.x + w.w / 2, w.y + w.h / 2};
        HWND hw = WindowFromPoint(pt);
        return !hw || GetAncestor(hw, GA_ROOT) != gHwnd;
    }), words.end());
    auto segs = segment(words);
    Layout nl = findLayout(segs, gMon.right, gMon.bottom);
    if (!nl.ok) {
        static double lastMsg = 0;
        int ns = 0, nb = 0;
        for (auto &sg : segs) { std::string l = lower(sg.text); if (l == "store" || l.rfind("store ", 0) == 0) ns++; if (l.find("bakery") != std::string::npos) nb++; }
        if (now() - lastMsg > 15) {
            logf("can't find the game layout: %d words on screen, %d store headings, %d bakery headings (in the game window)", (int)allWords.size(), ns, nb);
            lastMsg = now();
        }
        dbgf("--- layout FAILED (%d segs)", (int)segs.size());
        for (auto &sg : segs) dbgf("  seg (%d,%d) '%s'", sg.x, sg.y, sg.text.c_str());
        return false;
    }
    L = nl;
    gFailSince = 0;
    gDanger.clear();
    updateDanger(segs);
    if (!locateMainCookie()) logf("couldn't find the big cookie by color, using text-based guess");
    baselineStatic();
    logf("layout found: store x%d-%d, bakery x%d-%d, cookie at %d,%d", L.store.x0, L.store.x1, L.bakery.x0, L.bakery.x1, L.cookieX, L.cookieY);
    return true;
}

static void connectionWatch() {
    if (!gameFocused() || gPaused) return;
    if (gFailSince == 0) gFailSince = now();
    if (now() - gFailSince < 20) return;
    if (now() - gLastRefresh < 10) return;
    Rect top{gMon.left, gMon.top, gMon.right, gMon.top + std::min(260L, (gMon.bottom - gMon.top) / 6)};
    bool ours = false;
    for (auto &w : ocrRect(top, 1.5)) if (lower(w.text).find(gHostKey) != std::string::npos) ours = true;
    std::string t = titleOf(gHwnd);
    bool errTitle = t.find("problem loading") != std::string::npos || t.find("not found") != std::string::npos ||
                    t.find("can't be reached") != std::string::npos || t.find("offline") != std::string::npos ||
                    t.find("error") != std::string::npos;
    bool blank = false;
    if (ours && !errTitle) {
        Rect body{gMon.left, top.y1, gMon.right, gMon.bottom - 40};
        int n = 0;
        for (auto &w : ocrRect(body, 1.0)) if (w.text.size() >= 3) n++;
        blank = n < 6;
    }
    if (blank || errTitle) {
        logf("game not readable (%s) - refreshing", errTitle ? "error page" : "page is blank");
        setTask("Game isn't loading - refreshing every 10 s");
        pressF5();
        gLastRefresh = now();
        L.ok = false;
    }
}

// ---------- full-screen check: captcha (wait for human) and harmless popups (dismiss) ----------
static double gLastScreenCheck = 0;
static int screenState(std::vector<Seg> &segs) {  // 0 normal, 1 captcha, 2 popup, 3 baker puzzle
    static const char *cap[] = {"captcha", "not a robot", "are you human", "are you a human", "verify you", "prove you",
                                "human verification", "quick check", "are you a robot", "still there"};
    static const char *pop[] = {"welcome back", "cookie unlocked", "new cookie", "while you were away", "flavor unlocked"};
    Rect mon{gMon.left, gMon.top, gMon.right, gMon.bottom};
    if (L.ok) mon = Rect{L.left.x0, L.game.y0, L.store.x1, L.game.y1};
    segs = segment(ocrRect(mon, 1.0));
    int st = 0;
    for (auto &sg : segs) {
        std::string l = lower(sg.text);
        if (l.find("real baker") != std::string::npos) return 3;
        for (auto k : cap) if (l.find(k) != std::string::npos) return 1;
        for (auto k : pop) if (l.find(k) != std::string::npos) st = 2;
    }
    return st;
}

static void dismissPopup(const std::vector<Seg> &segs) {
    static const char *ok[] = {"collect", "nice", "ok", "okay", "continue", "got it", "yum", "awesome", "close", "sweet",
                               "thanks", "back to baking", "keep baking", "great", "cool", "done", "x"};
    updateDanger(segs);
    for (auto k : ok)
        for (auto &sg : segs) {
            std::string l = lower(sg.text);
            if (sg.text.size() > 20) continue;
            bool m = (strlen(k) <= 2) ? (l == k) : (l.find(k) != std::string::npos);
            if (!m) continue;
            int x = sg.cx(), y = sg.cy(), H = gMon.bottom - gMon.top;
            if (y < gMon.top + H * 0.15 || y > gMon.bottom - H * 0.06) continue;
            bool bad = false;
            for (auto &d : gDanger) if (inRect(x, y, d)) bad = true;
            if (bad || !canAct() || !glideTo(x, y)) continue;
            sendMouse(x, y, MOUSEEVENTF_LEFTDOWN); sendMouse(x, y, MOUSEEVENTF_LEFTUP);
            gLastClick = now(); fxClick(x, y);
            logf("closed a popup ('%s')", sg.text.c_str()); nap(600); return;
        }
}

static void screenGuard() {
    if (now() - gLastScreenCheck < 15) return;
    gLastScreenCheck = now();
    std::vector<Seg> segs;
    int st = screenState(segs);
    if (st == 2) { setTask("Closing a popup"); dismissPopup(segs); L.ok = false; return; }
    if (st == 3) {
        updateDanger(segs);
        int x = (gMon.left + gMon.right) / 2, y = (gMon.top + gMon.bottom) / 2;
        bool bad = false;
        for (auto &d : gDanger) if (inRect(x, y, d)) bad = true;
        if (bad) { logf("baker puzzle: screen center is a no-click zone, waiting instead"); st = 1; }
        else {
            logf("baker puzzle - clicking screen center 530 times");
            setState("PUZZLE");
            gPuzzleX = x; gPuzzleY = y; gPuzzleHits = 0; gPuzzle = true;
            struct Off { ~Off() { gPuzzle = false; } } puzzleOff;
            int done = 0;
            bool gone = false;
            if (glideTo(x, y)) {
                double gap = 1.0 / std::min(12.0, cfgCps) + 0.0004;
                for (; done < 530 && canAct(); done++) {
                    while (gLastClick + gap - now() > 0) Sleep(1);
                    if (!canAct()) break;
                    sendMouse(x, y, MOUSEEVENTF_LEFTDOWN); sendMouse(x, y, MOUSEEVENTF_LEFTUP);
                    gLastClick = now(); fxClick(x, y, true); gPuzzleHits++;
                    { std::lock_guard<std::mutex> lk(gHud.mu); gHud.clicks++; }
                    if (done % 10 == 0) setTask("Baker puzzle: %d / 530 clicks", done);
                    if (done % 12 == 11 && screenState(segs) != 3) { done++; gone = true; break; }
                }
            }
            logf(gone ? "baker puzzle gone after %d clicks - stopping" : "baker puzzle: %d clicks done", done);
            setState("RUNNING");
            L.ok = false;
            nap(800);
            if (gone || done < 530 || screenState(segs) != 3) return;
            logf("baker puzzle still up after 530 clicks - waiting for you");
            st = 1;
        }
    }
    if (st != 1) return;
    logf("captcha on screen - not touching anything until you clear it");
    setState("CAPTCHA"); setTask("Waiting for you to clear the captcha");
    gOverlayOn = false;
    MessageBeep(MB_ICONEXCLAMATION);
    double lastBeep = now();
    while (!gQuit && !gPaused) {
        Sleep(1000);
        if (gameFocused()) { int s2 = screenState(segs); if (s2 != 1 && s2 != 3) break; }
        if (now() - lastBeep > 30) { MessageBeep(MB_ICONEXCLAMATION); lastBeep = now(); }
    }
    logf("captcha gone - carrying on");
    L.ok = false;
}

// ================= store scanning =================
static std::map<std::string, Item> gItems;
static std::map<std::string, BakeryLine> gBakery;
static std::map<std::string, double> gBlack;
static double gLastFull = -1000, gLastBakery = -1000;
static bool gClickedX1 = false;

static int gStoreScroll = -1;  // notches from top, -1 = unknown
static void storeTop() { safeWheel((L.store.x0 + L.store.x1) / 2, (L.store.y0 + L.store.y1) / 2, -80); nap(gBrain.settleMs); gStoreScroll = 0; }

static std::vector<Item> readStorePage(int scroll) {
    auto segs = segment(ocrRect(L.store, 2.0));
    updateDanger(segs);
    auto items = parseStore(segs, L.store, scroll);
    dbgf("--- store @%d notches: %d segs", scroll, (int)segs.size());
    for (auto &sg : segs) dbgf("  seg (%d,%d %dx%d) '%s'", sg.x, sg.y, sg.w, sg.h, sg.text.c_str());
    for (auto &it : items) dbgf("  ITEM %s '%s' cost=%s owned=%d desc='%s'", it.research ? "R" : "B", it.name.c_str(), fmtNum(it.cost).c_str(), it.owned, it.desc.c_str());
    return items;
}

static bool readTip(Item &it) {
    if (it.research || !it.nx) return false;
    if (!canAct() || !pointSafe(it.nx, it.ny) || !glideTo(it.nx, it.ny)) return false;
    nap(280);
    Rect r{std::max(L.game.x0, L.store.x0 - 760), std::max(L.game.y0, it.ny - 380), L.store.x0 + 12, std::min(L.game.y1, it.ny + 380)};
    double v; int kind;
    for (int pass = 0; pass < 2; pass++) {
        auto segs = segment(ocrRect(r, 1.6, pass == 1));
        if (parseTip(segs, it.name, v, kind)) {
            it.tip = v; it.tipKind = kind;
            dbgf("  TIP %s: %s %s", it.name.c_str(), fmtNum(v).c_str(), kind == 2 ? "per click" : "per sec");
            return true;
        }
        if (pass == 0) for (auto &sg : segs) dbgf("  tipseg '%s'", sg.text.c_str());
    }
    dbgf("  TIP %s: not found", it.name.c_str());
    return false;
}

static void fullScan(bool store = true) {
    setTask("Reading cookie count");
    if (!readEconNow()) { logf("couldn't read cookie count"); gMiss++; return; }
    std::map<std::string, Item> items;
    std::string prevSig;
    if (!store) goto bakery;
    {
    int sx = (L.store.x0 + L.store.x1) / 2, sy = (L.store.y0 + L.store.y1) / 2;
    storeTop();
    auto first = segment(ocrRect(L.store, 2.0));
    updateDanger(first);
    if (!gClickedX1) {
        for (auto &s : first) {
            std::string l = lower(s.text);
            if ((l == "x1" || l == "*1" || l == "x 1") && s.cy() < L.store.y0 + 200) { safeClick(s.cx(), s.cy()); nap(300); break; }
        }
        gClickedX1 = true;
    }
    for (auto &s : first) {
        std::string l = lower(s.text);
        if (l.rfind("show all", 0) == 0 && s.text.size() < 16) { safeClick(s.cx(), s.cy()); nap(500); break; }
    }
    std::vector<Item> prev;
    int step = gBrain.notchesPerPage, scroll = 0;
    std::vector<double> shifts;
    for (int page = 0; page < 40 && !gQuit && !gPaused; page++) {
        processCmds();
        setTask("Reading the store (page %d)", page + 1);
        auto its = readStorePage(scroll);
        std::string sig;
        for (auto &i : its) sig += i.key + "|";
        if (page > 0 && sig == prevSig) break;
        // learn how far one page of scrolling moves things
        for (auto &a : its) for (auto &b : prev) if (a.key == b.key) shifts.push_back((double)(b.ny - a.ny) / (L.store.y1 - L.store.y0));
        prevSig = sig; prev = its;
        for (auto &i : its) {
            if (items.count(i.key)) continue;
            if (!i.research) {
                setTask("Reading the %s tooltip", i.name.c_str());
                if (!readTip(i)) { auto f = gItems.find(i.key); if (f != gItems.end()) { i.tip = f->second.tip; i.tipKind = f->second.tipKind; } }
            }
            items[i.key] = i;
        }
        if (gQuit || gPaused) return;
        if (!safeWheel(sx, sy, step)) return;
        scroll += step;
        nap(gBrain.settleMs);
    }
    if (!shifts.empty()) {
        std::sort(shifts.begin(), shifts.end());
        double med = shifts[shifts.size() / 2];
        int old = gBrain.notchesPerPage;
        if (med < 0.3 && gBrain.notchesPerPage < 10) gBrain.notchesPerPage++;
        else if (med > 0.75 && gBrain.notchesPerPage > 1) gBrain.notchesPerPage--;
        if (old != gBrain.notchesPerPage) logf("learned: scrolling %d notches per page (moved %.0f%% of the view)", gBrain.notchesPerPage, med * 100);
    }
    gStoreScroll = -1;
    if (gPaused || gQuit) return;
    if (now() - gLastBakery < 300 && !gBakery.empty() && !items.empty()) {
        gItems = items; gLastFull = now(); gMiss = 0;
        logf("scan: %s cookies, %s/s | %d store items", fmtNum(gEcon.bank).c_str(), fmtNum(gEcon.cps).c_str(), (int)items.size());
        return;
    }
    }
bakery:
    gLastBakery = now();
    int bx = (L.bakery.x0 + L.bakery.x1) / 2, by = (L.bakery.y0 + L.bakery.y1) / 2;
    std::map<std::string, BakeryLine> bake;
    safeWheel(bx, by, -80); nap(gBrain.settleMs);
    prevSig.clear();
    for (int page = 0; page < 40 && !gQuit && !gPaused; page++) {
        processCmds();
        setTask("Reading the bakery (page %d)", page + 1);
        auto bl = parseBakery(segment(ocrRect(L.bakery, 2.0, true)), L.bakery);
        std::string sig;
        for (auto &b : bl) sig += b.key + "|";
        if (page > 0 && sig == prevSig) break;
        prevSig = sig;
        for (auto &b : bl) bake[b.key] = b;
        if (!safeWheel(bx, by, gBrain.notchesPerPage)) return;
        nap(gBrain.settleMs);
    }
    safeWheel(bx, by, -80);
    if (!store) {
        if (!bake.empty()) gBakery = bake;
        logf("bakery re-read after an upgrade: %d rows", (int)bake.size());
        return;
    }
    if (items.empty()) { logf("store unreadable"); gMiss++; return; }
    gItems = items;
    if (!bake.empty()) gBakery = bake;
    gLastFull = now();
    gMiss = 0;
    int nr = 0; for (auto &kv : items) if (kv.second.research) nr++;
    logf("scan: %s cookies, %s/s, %s/click | %d store items (%d research), %d bakery rows", fmtNum(gEcon.bank).c_str(),
         fmtNum(gEcon.cps).c_str(), fmtNum(gEcon.perClick).c_str(), (int)items.size(), nr, (int)bake.size());
    gBrain.save(BRAIN);
}

// ================= buying, with learning =================
static int gBuyFails = 0;
static double gBuyPauseUntil = 0;

static std::vector<std::pair<std::string, POINT>> clickSpots(const Item &it) {
    std::string k = it.research ? "R:" : "B:";
    std::vector<std::pair<std::string, POINT>> v = {
        {k + "name", POINT{it.nx, it.ny}},
        {k + "price", POINT{it.px, it.py}},
        {k + "card", POINT{(it.nx + it.px) / 2, (it.ny + it.py) / 2}},
        {k + "left", POINT{std::max(L.store.x0 + 20, it.nx - (it.px - it.nx) / 2 - 30), (it.ny + it.py) / 2}},
    };
    std::stable_sort(v.begin(), v.end(), [](auto &a, auto &b) { return gBrain.spotScore(a.first) > gBrain.spotScore(b.first); });
    return v;
}

static SimState currentState(std::vector<Item> &list) {
    list.clear();
    for (auto &kv : gItems) if (!gBlack.count(kv.first) || gBlack[kv.first] < now()) list.push_back(kv.second);
    std::vector<BakeryLine> bl;
    for (auto &kv : gBakery) bl.push_back(kv.second);
    Econ e = gEcon; e.bank = bankNow();
    SimState st = makeState(e, bl, list, effRate(), gBrain);
    static double lastFix = 1;
    if (std::abs(st.fix / lastFix - 1) > 0.25) {
        if (st.fix != 1) logf("building readings only add up to 1/%.3g of real cps - scaling them x%.3g", st.fix, st.fix);
        lastFix = st.fix;
    }
    return st;
}

static bool findOnStore(const Item &target, Item &found, int &atScroll) {
    int sx = (L.store.x0 + L.store.x1) / 2, sy = (L.store.y0 + L.store.y1) / 2;
    int step = gBrain.notchesPerPage;
    int tries[] = {target.scroll, target.scroll + step, target.scroll - step, target.scroll + 2 * step};
    for (int sc : tries) {
        if (sc < 0) continue;
        if (gStoreScroll < 0 || sc < gStoreScroll - 4 * step) storeTop();
        if (sc != gStoreScroll) {
            if (!safeWheel(sx, sy, sc - gStoreScroll)) { gStoreScroll = -1; return false; }
            gStoreScroll = sc;
            nap(gBrain.settleMs);
        }
        for (auto &i : readStorePage(sc)) if (i.key == target.key) { found = i; atScroll = sc; return true; }
    }
    gStoreScroll = -1;
    return false;
}

static bool buy(const Item &target, double predictedGain, const std::string &effType) {
    double buyStart = now();
    Item cur; int sc = 0;
    if (!findOnStore(target, cur, sc)) {
        logf("couldn't find '%s' after scrolling - rescanning", target.name.c_str());
        if (gBrain.settleMs < 1200) { gBrain.settleMs += 100; logf("learned: waiting %d ms after scrolling", gBrain.settleMs); }
        gLastFull = -1000; return false;
    }
    // cached price was wrong? fix the cache and re-plan instead of clicking
    {
        auto f = gItems.find(cur.key);
        if (f != gItems.end() && std::fabs(f->second.cost - cur.cost) > f->second.cost * 0.02) {
            dbgf("price cache fix '%s': %s -> %s", cur.name.c_str(), fmtNum(f->second.cost).c_str(), fmtNum(cur.cost).c_str());
            f->second.cost = cur.cost; f->second.owned = cur.owned; f->second.scroll = sc;
            if (bankNow() < cur.cost) return false;
        }
    }
    double before = bankNow(), P0 = prodNow(gEcon);
    bool econGuess0 = gEcon.guess;
    auto spots = clickSpots(cur);
    for (size_t attempt = 0; attempt < 2 && attempt < spots.size(); attempt++) {
        auto &sp = spots[attempt];
        double t0 = now();
        if (!safeClick(sp.second.x, sp.second.y)) return false;
        nap(300);
        auto after = readStorePage(sc);
        const Item *a0 = nullptr;
        for (auto &i : after) if (i.key == cur.key) a0 = &i;
        bool countKnown = (cur.research && !a0) || (cur.owned >= 0 && a0 && a0->owned >= 0);
        bool econOk = true, spent = false;
        if (!countKnown) {
            econOk = false;
            for (int k = 0; k < 2 && !econOk; k++) { econOk = readEconNow(); if (!econOk) nap(150); }
            spent = econOk && gEcon.bank < before + P0 * (now() - t0) - cur.cost * 0.4;
        }
        const Item *a = nullptr;
        for (auto &i : after) if (i.key == cur.key) a = &i;
        if (!cur.research && cur.owned >= 0 && a && a->owned >= 0 && a->owned < cur.owned) {
            gPaused = true;
            logf("!!! '%s' count went DOWN - store looks like it's in Sell mode. Turn Sell off, then press F8.", cur.name.c_str());
            return false;
        }
        bool ok = spent || (cur.owned >= 0 && a && a->owned > cur.owned) || (cur.research && !a);
        if (ok) { gEcon.bank = std::max(0.0, bankNow() - cur.cost); gEconTime = now(); }
        dbgf("BUY '%s' via %s (%ld,%ld) bank %s -> %s econOk %d owned %d -> %d => %s", cur.name.c_str(), sp.first.c_str(),
             sp.second.x, sp.second.y, fmtNum(before).c_str(), fmtNum(gEcon.bank).c_str(), econOk, cur.owned, a ? a->owned : -2, ok ? "OK" : "no");
        auto &st = gBrain.spot[sp.first];
        if (ok) {
            st.first++; st.second++;
            gBrain.purchases++; gBuyFails = 0;
            if (gGoal.on && cur.key == gGoal.key) { gGoal.bought++; updateGoalHud(); }
            logf("BOUGHT %s for %s", cur.name.c_str(), fmtNum(cur.cost).c_str());
            { std::lock_guard<std::mutex> lk(gHud.mu); gHud.buysRun++; }
            // learn price growth
            if (!cur.research && a && a->cost > cur.cost) {
                gBrain.learnGrowth(cur.key, a->cost / cur.cost);
            }
            if (cur.research) gItems.erase(cur.key);
            for (auto &i : after) { auto f = gItems.find(i.key); if (f != gItems.end()) { f->second.cost = i.cost; f->second.owned = i.owned; f->second.scroll = sc; } }
            gBrain.buyOverhead = 0.8 * gBrain.buyOverhead + 0.2 * std::min(20.0, now() - buyStart);
            // learn what it actually did (only when there's something to learn)
            bool needLearn = cur.research || !gBakery.count(cur.key);
            if (cur.research) {
                gLastBakery = -1000;
                for (auto &kv : gItems) kv.second.tipKind = 0;  // multipliers changed: trust the bakery panel until the next full scan
            }
            bool clean = now() - gLastBonus > 40;
            if (needLearn && clean && nap(1200) && readEconNow()) {
                double dP = prodNow(gEcon) - P0;
                bool trust = !gEcon.guess && !econGuess0 && dP > 0;
                if (trust && !cur.research && !gBakery.count(cur.key) && gEcon.cps > 0) gBrain.learnUnit(cur.key, dP);
                if (trust && cur.research && predictedGain > 0 && P0 > 0) {
                    if (effType == "unknown") gBrain.learnUnknown(cur.name + " " + cur.desc, dP / P0);
                    gBrain.learnCorr(effType, dP / predictedGain);
                    logf("learned: '%s' was worth +%s/s (expected +%s/s)", cur.name.c_str(), fmtNum(dP).c_str(), fmtNum(predictedGain).c_str());
                }
            }
            gBrain.save(BRAIN);
            return true;
        }
        if (!econOk) { gLastFull = -1000; return false; }
        if (readEconNow() && gEcon.bank < cur.cost) {
            logf("couldn't afford '%s' yet (estimate was high) - waiting", cur.name.c_str());
            return false;
        }
        st.second++;
        logf("click on '%s' (%s) didn't buy it - trying another spot", cur.name.c_str(), sp.first.c_str());
    }
    gBrain.failedBuys++;
    gBlack[cur.key] = now() + (gGoal.on && gGoal.key == cur.key ? 20 : 180);
    logf("couldn't buy '%s', skipping it for 3 min", cur.name.c_str());
    if (++gBuyFails >= 5) { gBuyPauseUntil = now() + 600; gBuyFails = 0; logf("too many failed buys - just clicking for 10 min"); }
    gBrain.save(BRAIN);
    return false;
}

// ================= HUD (lower-left of the overlay) =================
static std::string fmtDur(double s) {
    char b[32];
    if (s < 0) s = 0;
    if (s < 60) snprintf(b, sizeof b, "%.0fs", s);
    else if (s < 3600) snprintf(b, sizeof b, "%dm %02ds", (int)s / 60, (int)s % 60);
    else snprintf(b, sizeof b, "%dh %02dm", (int)s / 3600, ((int)s % 3600) / 60);
    return b;
}

static void renderHud() {
    if (cc::active()) { cc::renderHud(); return; }
    double k = 1.0;
    if (gHwnd) { UINT d = GetDpiForWindow(gHwnd); if (d) k = d / 96.0 * 0.9; }
    int W = (int)(470 * k), H = (int)(412 * k);
    std::string state, task, want, wantType, why;
    double wantCost, wantGain; long long clicks; int buysRun, bonusRun;
    std::vector<std::pair<std::string, double>> alts;
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(gHud.mu);
        state = gHud.state; task = gHud.task; want = gHud.want; wantType = gHud.wantType; why = gHud.why;
        wantCost = gHud.wantCost; wantGain = gHud.wantGain; clicks = gHud.clicks; buysRun = gHud.buysRun; bonusRun = gHud.bonusRun;
        alts = gHud.alts; lines = gHud.lines;
    }
    std::string order; { std::lock_guard<std::mutex> lk(gHud.mu); order = gHud.order; }
    std::string ptext, reply; bool popen = gPromptOpen; double replyAt;
    { std::lock_guard<std::mutex> lk(gPromptMu); ptext = gPromptText; reply = gReply; replyAt = gReplyAt; }
    if (gPaused) state = "PAUSED";
    if (!gPaused && now() < gResumeAt) { char b[64]; snprintf(b, sizeof b, "Starting in %.1fs - hands off the mouse", gResumeAt - now()); task = b; }
    double bank = gEconTime > 0 ? bankNow() : 0, P = prodNow(gEcon);

    std::vector<uint32_t> bg((size_t)W * H, 0);
    Canvas cv{W, H, bg.data()};
    float R = 14 * (float)k;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float dx = std::max(0.f, std::max(R - x, x - (W - 1 - R))), dy = std::max(0.f, std::max(R - y, y - (H - 1 - R)));
            float d = std::sqrt(dx * dx + dy * dy);
            if (d > R) continue;
            float edge = R - d;
            if (edge < 2 * (float)k) cv.blend(x, y, 80, 150, 255, 0.95f);
            else cv.blend(x, y, 8, 16, 34, 0.84f);
        }
    int barX = (int)(86 * k), barY = (int)(102 * k), barW = W - barX - (int)(18 * k), barH = (int)(9 * k);
    float frac = wantCost > 0 ? (float)std::min(1.0, bank / wantCost) : 0;
    for (int y = barY; y < barY + barH; y++)
        for (int x = barX; x < barX + barW; x++) {
            bool fill = x < barX + barW * frac;
            if (fill) cv.blend(x, y, 70, 160, 255, 0.95f); else cv.blend(x, y, 40, 60, 100, 0.6f);
        }
    for (int x = (int)(16 * k); x < W - (int)(16 * k); x++) { cv.blend(x, (int)(204 * k), 70, 110, 180, 0.6f); cv.blend(x, (int)(282 * k), 70, 110, 180, 0.6f); }
    {
        int bx0 = (int)(14 * k), bx1 = W - (int)(14 * k), by0 = (int)(366 * k), by1 = (int)(398 * k);
        float glow = popen ? 0.95f : 0.45f;
        for (int y = by0; y < by1; y++)
            for (int x = bx0; x < bx1; x++) {
                bool edge = y < by0 + (int)(1.5 * k) || y >= by1 - (int)(1.5 * k) || x < bx0 + (int)(1.5 * k) || x >= bx1 - (int)(1.5 * k);
                if (edge) cv.blend(x, y, 90, 170, 255, glow); else cv.blend(x, y, 14, 30, 64, popen ? 0.95f : 0.7f);
            }
    }

    // text layer (grayscale-antialiased, colour comes from the text itself)
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memset(bits, 0, (size_t)W * H * 4);
    HGDIOBJ oldB = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    auto font = [&](int px, int weight) { return CreateFontW(-(int)(px * k), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI"); };
    HFONT fTitle = font(19, FW_BOLD), fBody = font(14, FW_NORMAL), fBold = font(14, FW_SEMIBOLD), fSmall = font(12, FW_NORMAL), fLabel = font(11, FW_BOLD);
    auto text = [&](HFONT f, COLORREF c, int x, int y, const std::string &str, int maxW = 0, bool right = false) {
        SelectObject(dc, f); SetTextColor(dc, c);
        std::wstring w(str.size() + 1, 0);
        int n = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &w[0], (int)w.size()); w.resize(n);
        RECT r{(int)(x * k), (int)(y * k), maxW ? (int)((x + maxW) * k) : W - (int)(14 * k), H};
        DrawTextW(dc, w.c_str(), (int)w.size(), &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | (right ? DT_RIGHT : DT_LEFT));
    };
    COLORREF cyan = RGB(120, 200, 255), white = RGB(235, 242, 255), dim = RGB(130, 150, 185), lab = RGB(95, 140, 210);
    COLORREF stc = state == "RUNNING" ? RGB(90, 230, 140) : state == "CAPTCHA" ? RGB(255, 90, 90) : RGB(255, 200, 80);
    text(fTitle, cyan, 18, 12, "COOKIEBOT  AI");
    text(fBold, stc, 18, 15, std::string("\xE2\x97\x8F ") + state, 0, true);
    text(fLabel, lab, 18, 50, "TASK");      text(fBody, white, 86, 47, task);
    text(fLabel, lab, 18, 76, "WANTS");
    if (!want.empty()) {
        text(fBold, white, 86, 73, want + "  \xC2\xB7  " + fmtNum(wantCost), 250);
        double eta = bank >= wantCost ? 0 : (wantCost - bank) / std::max(1.0, P);
        text(fSmall, dim, 86, 75, bank >= wantCost ? "buying now" : "ready in " + fmtDur(eta), 0, true);
    } else text(fBody, dim, 86, 73, "nothing worth buying right now");
    text(fLabel, lab, 18, 120, "WHY");
    if (!want.empty() && wantGain > 0) {
        char b[200];
        snprintf(b, sizeof b, "+%s/s  \xC2\xB7  pays back in %s  \xC2\xB7  %s", fmtNum(wantGain).c_str(), fmtDur(wantCost / wantGain).c_str(), wantType.c_str());
        text(fBody, white, 86, 117, b);
    } else text(fBody, dim, 86, 117, why.empty() ? "-" : why);
    text(fLabel, lab, 18, 146, "ALSO");
    std::string al;
    for (auto &a : alts) { char b[96]; snprintf(b, sizeof b, "%s%s %+.1f%%", al.empty() ? "" : "  \xC2\xB7  ", a.first.c_str(), a.second); al += b; }
    text(fSmall, dim, 86, 145, al.empty() ? "-" : al);

    {
        text(fLabel, lab, 18, 172, "NEXT");
        auto cd = [&](double t) { return t <= 0.5 ? std::string("now") : fmtDur(t); };
        std::string nx;
        if (!want.empty() && cfgBuy) nx += "buy " + cd(bank >= wantCost ? 0 : (wantCost - bank) / std::max(1.0, P)) + "  \xC2\xB7  ";
        if (cfgBuy) nx += "store " + cd(gLastFull + 3600 - now()) + "  \xC2\xB7  ";
        nx += "count " + cd(gEconTime + 1 - now()) + "  \xC2\xB7  captcha " + cd(gLastScreenCheck + 15 - now());
        text(fSmall, white, 86, 171, nx);
    }
    auto stat = [&](int col, int row, const char *label, const std::string &v) {
        int x = 18 + col * 150, y = 214 + row * 22;
        text(fSmall, dim, x, y, label, 70); text(fBold, white, x + 66, y - 1, v, 80);
    };
    double up = gStart > 0 ? now() - gStart : 0;
    stat(0, 0, "Cookies", fmtNum(bank)); stat(1, 0, "Per sec", fmtNum(gEcon.cps)); stat(2, 0, "Per click", fmtNum(gEcon.perClick));
    stat(0, 1, "Clicks", fmtNum((double)clicks)); stat(1, 1, "Bought", std::to_string(buysRun) + " / " + std::to_string(gBrain.purchases));
    stat(2, 1, "Bonus", std::to_string(bonusRun));
    stat(0, 2, "Uptime", fmtDur(up)); stat(1, 2, "Click/s", up > 0 ? std::to_string((int)(clicks / up * 10) / 10.0).substr(0, 4) : "0");
    stat(2, 2, "Learned", std::to_string(gBrain.corr.size() + gBrain.growth.size() + gBrain.unitCps.size()));
    text(fLabel, lab, 18, 290, "THOUGHTS");
    int y = 306;
    for (auto &l : lines) { text(fSmall, dim, 18, y, l); y += 15; if (y > 342) break; }
    {
        std::string row;
        COLORREF col = dim;
        if (popen) { row = "> " + ptext + (std::fmod(now(), 1.0) < 0.55 ? "_" : " "); col = white; }
        else if (now() - replyAt < 6 && !reply.empty()) { row = reply; col = cyan; }
        else if (!order.empty()) { row = "ORDER  " + order; col = cyan; }
        else row = "type to give an order  (e.g. \"buy 10 grandmas\")";
        text(fBody, col, 26, 373, row, 420);
    }

    GdiFlush();
    const uint32_t *tp = (const uint32_t *)bits;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint32_t t = tp[i];
        int r = (t >> 16) & 255, g = (t >> 8) & 255, b = t & 255;
        int m = std::max(r, std::max(g, b));
        if (!m) continue;
        float a = m / 255.f;
        cv.blend((int)(i % W), (int)(i / W), r * 255 / m, g * 255 / m, b * 255 / m, a);
    }
    SelectObject(dc, oldB);
    for (HFONT f : {fTitle, fBody, fBold, fSmall, fLabel}) DeleteObject(f);
    DeleteObject(bmp); DeleteDC(dc);
    RECT wa = gWork.right > gWork.left ? gWork : gMon;
    int hx = wa.left + (gHudDX >= 0 ? gHudDX : (int)(26 * k)), hy = wa.bottom - H - (gHudDY >= 0 ? gHudDY : (int)(26 * k));
    hx = std::max((int)wa.left, std::min((int)wa.right - W, hx)); hy = std::max((int)wa.top, std::min((int)wa.bottom - H, hy));
    pushLayered(gHudWnd, hx, hy, W, H, bg.data(), gHudDrag);
}

#include "copycat.h"

// ================= control panel window =================
enum { ID_PAUSE = 101, ID_QUIT, ID_BUY, ID_GOLD, ID_FX, ID_HOURS, ID_CPS, ID_APPLY, ID_FORGET, ID_LOG, ID_STATUS, ID_STATS, ID_UPDATE, ID_WATCH, ID_COPY };
static std::atomic<bool> gForgetReq{false};
static HBRUSH gBrBg, gBrEdit;
static HFONT gUiFont, gUiBold;

static void drawButton(DRAWITEMSTRUCT *d) {
    int id = d->CtlID;
    bool toggle = id == ID_BUY || id == ID_GOLD || id == ID_FX;
    bool on = id == ID_BUY ? cfgBuy : id == ID_GOLD ? cfgGolden : id == ID_FX ? cfgFx : false;
    bool pressed = d->itemState & ODS_SELECTED;
    RECT r = d->rcItem;
    FillRect(d->hDC, &r, gBrBg);
    COLORREF fill = id == ID_QUIT ? RGB(150, 45, 55) : id == ID_FORGET ? RGB(60, 60, 80) : toggle ? (on ? RGB(35, 110, 210) : RGB(45, 52, 70)) : RGB(35, 110, 210);
    if (id == ID_PAUSE && !gPaused) fill = RGB(200, 140, 30);
    if (pressed) fill = RGB(GetRValue(fill) * 3 / 4, GetGValue(fill) * 3 / 4, GetBValue(fill) * 3 / 4);
    HBRUSH b = CreateSolidBrush(fill);
    HPEN pn = CreatePen(PS_SOLID, 1, RGB(90, 150, 240));
    HGDIOBJ ob = SelectObject(d->hDC, b), op = SelectObject(d->hDC, pn);
    RoundRect(d->hDC, r.left, r.top, r.right, r.bottom, 12, 12);
    SelectObject(d->hDC, ob); SelectObject(d->hDC, op); DeleteObject(b); DeleteObject(pn);
    wchar_t t[64]; GetWindowTextW(d->hwndItem, t, 64);
    std::wstring label = t;
    if (toggle) label += on ? L":  ON" : L":  OFF";
    if (id == ID_PAUSE) label = gPaused ? L"▶  Resume  (F8)" : L"⏸  Pause  (F8)";
    SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, RGB(240, 245, 255));
    SelectObject(d->hDC, gUiBold);
    DrawTextW(d->hDC, label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static LRESULT CALLBACK panelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static int seenLog = 0;
    switch (m) {
    case WM_CREATE: {
        auto mk = [&](const wchar_t *cls, const wchar_t *txt, DWORD st, int x, int y, int cw, int ch, int id) {
            HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, cw, ch, h, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)gUiFont, TRUE);
            return c;
        };
        mk(L"STATIC", L"", SS_LEFT, 16, 12, 420, 40, ID_STATUS);
        mk(L"BUTTON", L"Pause", BS_OWNERDRAW, 16, 58, 205, 40, ID_PAUSE);
        mk(L"BUTTON", L"✖  Quit  (F9)", BS_OWNERDRAW, 231, 58, 205, 40, ID_QUIT);
        mk(L"BUTTON", L"Buy upgrades", BS_OWNERDRAW, 16, 108, 135, 34, ID_BUY);
        mk(L"BUTTON", L"Bonus cookies", BS_OWNERDRAW, 159, 108, 135, 34, ID_GOLD);
        mk(L"BUTTON", L"Effects + HUD", BS_OWNERDRAW, 302, 108, 134, 34, ID_FX);
        mk(L"STATIC", L"Session hours", SS_LEFT, 16, 158, 90, 20, 0);
        wchar_t b[32];
        swprintf(b, 32, L"%.1f", cfgHours);
        mk(L"EDIT", b, ES_LEFT | WS_BORDER, 110, 155, 60, 24, ID_HOURS);
        mk(L"STATIC", L"Clicks/s (max 12)", SS_LEFT, 186, 158, 110, 20, 0);
        swprintf(b, 32, L"%.1f", cfgCps);
        mk(L"EDIT", b, ES_LEFT | WS_BORDER, 300, 155, 50, 24, ID_CPS);
        mk(L"BUTTON", L"Apply", BS_OWNERDRAW, 360, 152, 76, 30, ID_APPLY);
        mk(L"STATIC", L"", SS_LEFT, 16, 194, 420, 58, ID_STATS);
        mk(L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | WS_VSCROLL | LBS_NOSEL, 16, 258, 420, 250, ID_LOG);
        mk(L"BUTTON", L"Forget what it learned", BS_OWNERDRAW, 16, 516, 205, 30, ID_FORGET);
        mk(L"BUTTON", L"Check for updates", BS_OWNERDRAW, 231, 516, 205, 30, ID_UPDATE);
        mk(L"BUTTON", L"Watch me play (F6)", BS_OWNERDRAW, 16, 554, 205, 30, ID_WATCH);
        mk(L"BUTTON", L"Let it play (F7)", BS_OWNERDRAW, 231, 554, 205, 30, ID_COPY);
        SetTimer(h, 1, 250, nullptr);
        return 0;
    }
    case WM_DRAWITEM: drawButton((DRAWITEMSTRUCT *)l); return TRUE;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)w, RGB(215, 225, 245)); SetBkColor((HDC)w, RGB(14, 20, 34)); return (LRESULT)gBrBg;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, RGB(240, 245, 255)); SetBkColor((HDC)w, RGB(28, 38, 60)); return (LRESULT)gBrEdit;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_PAUSE) {
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
            if (MessageBoxW(h, L"Wipe everything the bot has learned and start its brain from scratch?", L"CookieBot", MB_YESNO | MB_ICONQUESTION) == IDYES)
                gForgetReq = true;
        } else if (id == ID_WATCH || id == ID_COPY) {
            if (cc::active()) std::thread(cc::stop).detach();
            else { logf("Copycat: click your game window now - starting in 3 s"); ccStartAsync(id == ID_WATCH ? 1 : 2, 3); }
        } else if (id == ID_UPDATE) { static double last = 0; if (now() - last > 5) { last = now(); requestUpdateCheck(); logf("Checking GitHub for updates..."); } }
        InvalidateRect(GetDlgItem(h, id), nullptr, TRUE);
        return 0;
    }
    case WM_TIMER: {
        std::string st, task; int serial; std::vector<std::string> newLines;
        {
            std::lock_guard<std::mutex> lk(gHud.mu);
            st = gPaused ? "PAUSED" : gHud.state; task = gHud.task; serial = gHud.logSerial;
            int fresh = std::min<int>(serial - seenLog, (int)gHud.log.size());
            for (int i = (int)gHud.log.size() - fresh; i < (int)gHud.log.size(); i++) newLines.push_back(gHud.log[i]);
            seenLog = serial;
        }
        std::string s1 = st + "  -  " + task;
        SetDlgItemTextA(h, ID_STATUS, s1.c_str());
        char b[400];
        double up = gStart > 0 ? now() - gStart : 0;
        snprintf(b, sizeof b, "Cookies %s   |   %s/s   |   %s/click\nBought %d this run, %d all-time   |   uptime %s\nBrain: %d upgrade types, %d buildings, %d price curves learned",
                 fmtNum(gEconTime > 0 ? bankNow() : 0).c_str(), fmtNum(gEcon.cps).c_str(), fmtNum(gEcon.perClick).c_str(), gHud.buysRun, gBrain.purchases,
                 fmtDur(up).c_str(), (int)gBrain.corr.size(), (int)gBrain.unitCps.size(), (int)gBrain.growth.size());
        SetDlgItemTextA(h, ID_STATS, b);
        HWND lb = GetDlgItem(h, ID_LOG);
        for (auto &ln : newLines) {
            std::wstring wl(ln.size() + 1, 0);
            int n = MultiByteToWideChar(CP_UTF8, 0, ln.c_str(), (int)ln.size(), &wl[0], (int)wl.size()); wl.resize(n);
            SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)wl.c_str());
        }
        int cnt = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
        while (cnt > 300) { SendMessageW(lb, LB_DELETESTRING, 0, 0); cnt--; }
        if (!newLines.empty()) SendMessageW(lb, LB_SETTOPINDEX, cnt - 1, 0);
        InvalidateRect(GetDlgItem(h, ID_PAUSE), nullptr, FALSE);
        if (gQuit) DestroyWindow(h);
        return 0;
    }
    case WM_CLOSE: gQuit = true; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void panelThread() {
    gBrBg = CreateSolidBrush(RGB(14, 20, 34));
    gBrEdit = CreateSolidBrush(RGB(28, 38, 60));
    gUiFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    gUiBold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    WNDCLASSW wc{}; wc.lpfnWndProc = panelProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"CookieBotPanel";
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hbrBackground = gBrBg; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, 452, 600};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    wchar_t title[64]; swprintf(title, 64, L"CookieBot  v%d", VERSION);
    gPanel = CreateWindowExW(0, L"CookieBotPanel", title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    BOOL dark = TRUE;
    typedef HRESULT(WINAPI * DwmSet)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
        if (auto f = (DwmSet)GetProcAddress(dwm, "DwmSetWindowAttribute")) f(gPanel, 20, &dark, sizeof dark);
    ShowWindow(gPanel, SW_SHOWNORMAL);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    gQuit = true;
}

// ================= main =================
static void clickFor(double seconds) {
    setState("RUNNING");
    {
        std::string w; { std::lock_guard<std::mutex> lk(gHud.mu); w = gHud.want; }
        if (w.empty()) setTask("Clicking the cookie"); else setTask("Clicking the cookie - saving up for %s", w.c_str());
    }
    double end = now() + seconds, nextSpawn = now();
    while (now() < end && canAct()) {
        { std::lock_guard<std::mutex> lk(gPromptMu); if (!gCmds.empty()) break; }
        if (cfgGolden && now() >= nextSpawn) { clickSpawned(); nextSpawn = now() + 0.4; }
        if (!safeClick(L.cookieX + (rand() % 11 - 5), L.cookieY + (rand() % 11 - 5))) break;
        gCookieClicks++;
    }
}

// reads the real cookie count every second on its own thread, so clicking never waits for it
static void econThread() {
    RoInitialize(RO_INIT_MULTITHREADED);
    tlNoBox = true;
    while (!gQuit) {
        Sleep(250);
        if (!L.ok || !gOverlayOn || gPaused || now() < gResumeAt || !gameFocused()) continue;
        if (now() - gEconTime < 1.0) continue;
        readEconNow(true);
    }
}

// ================= prompt bar orders =================
static void reply(const char *fmt, ...) {
    char buf[256];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    { std::lock_guard<std::mutex> lk(gPromptMu); gReply = buf; gReplyAt = now(); }
    logf("order: %s", buf);
}

static void updateGoalHud() {
    std::string o;
    if (gGoal.on) {
        char b[160];
        if (gGoal.key.rfind("R:", 0) == 0) snprintf(b, sizeof b, "buy %s", gGoal.name.c_str());
        else snprintf(b, sizeof b, "buy %d %s  -  %d / %d bought", gGoal.count, gGoal.name.c_str(), gGoal.bought, gGoal.count);
        o = b;
    }
    std::lock_guard<std::mutex> lk(gHud.mu); gHud.order = o;
}

static void checkGoal() {
    if (!gGoal.on) return;
    auto f = gItems.find(gGoal.key);
    bool research = gGoal.key.rfind("R:", 0) == 0;
    if ((research && f == gItems.end()) || (!research && gGoal.bought >= gGoal.count)) {
        gGoal.on = false;
        reply("done: %s - back to normal play", gGoal.name.c_str());
    }
    updateGoalHud();
}

struct Pending { std::string text; double until; };
static std::vector<Pending> gPending;
static double gPendingTried = 0;

static void processCmds() {
    std::deque<std::string> q;
    { std::lock_guard<std::mutex> lk(gPromptMu); q.swap(gCmds); }
    std::set<std::string> retry;
    if (!gPending.empty() && gLastFull > gPendingTried) {
        gPendingTried = gLastFull;
        for (auto &p : gPending) if (now() < p.until) { q.push_back(p.text); retry.insert(p.text); }
        gPending.clear();
    }
    for (auto &c : q) {
        bool again = retry.count(c) > 0;
        if (!again) logf("you typed: \"%s\"", c.c_str());
        std::vector<std::pair<std::string, std::string>> names;
        for (auto &kv : gItems) names.push_back({kv.first, kv.second.name});
        Order o = parseOrder(c, names);
        switch (o.kind) {
        case O_BUY: {
            if (o.key.empty()) {
                if (!again) {
                    gPending.push_back({c, now() + 1200});
                    if (!gItems.empty()) gLastFull = -1000;
                    reply("can't see \"%s\" in the store yet - rescanning, will buy it once it shows up", o.name.c_str());
                } else gPending.push_back({c, now() + 1200});
                break;
            }
            auto &it = gItems[o.key];
            gGoal = Goal{};
            gGoal.on = true; gGoal.key = o.key; gGoal.name = o.name; gGoal.since = now();
            gGoal.start = std::max(0, it.owned);
            gGoal.count = it.research ? 1 : o.count;
            gGoal.target = gGoal.start + gGoal.count;
            gBlack.erase(o.key);
            gBuyPauseUntil = 0;
            if (it.research) reply("on it: saving up for %s", o.name.c_str());
            else reply("on it: buying %d %s (%d -> %d)", o.count, o.name.c_str(), gGoal.start, gGoal.target);
            break;
        }
        case O_CANCEL:
            if (gGoal.on) { reply("cancelled: %s - back to normal play", gGoal.name.c_str()); gGoal.on = false; }
            else reply("nothing to cancel");
            break;
        case O_SKIP: gBlack[o.key] = now() + 1e9; reply("won't buy %s (say \"allow %s\" to undo)", o.name.c_str(), o.name.c_str()); break;
        case O_UNSKIP: gBlack.erase(o.key); reply("%s is allowed again", o.name.c_str()); break;
        case O_PAUSE: gPaused = true; reply("paused - F8 to resume"); break;
        case O_BUYON: cfgBuy = true; reply("buying turned on"); break;
        case O_BUYOFF: cfgBuy = false; reply("buying turned off - just clicking"); break;
        case O_GOLDON: cfgGolden = true; reply("clicking bonus cookies"); break;
        case O_GOLDOFF: cfgGolden = false; reply("ignoring bonus cookies"); break;
        case O_FXON: cfgFx = true; reply("effects + HUD on"); break;
        case O_FXOFF: cfgFx = false; reply("effects + HUD off"); break;
        case O_CPS: cfgCps = std::max(1.0, std::min(12.0, o.val)); reply("clicking at %.1f/s", cfgCps); break;
        case O_HOURS: cfgHours = std::max(0.25, std::min(72.0, o.val)); reply("planning for %.1f h sessions", cfgHours); break;
        case O_RESCAN: gLastFull = -1000; gLastBakery = -1000; reply("rescanning the store and bakery"); break;
        case O_UPDATE: requestUpdateCheck(); reply("checking GitHub for updates"); break;
        case O_HELP: reply("try: buy 10 grandmas / skip bakery / cancel / bonus off / clicks 8 / rescan / pause"); break;
        default:
            if (gItems.empty()) { gPending.push_back({c, now() + 1200}); reply("got it - will do that once the store is read"); }
            else if (!again) reply("didn't get that - type \"help\"");
        }
        if (gPanel) InvalidateRect(gPanel, nullptr, TRUE);
    }
    updateGoalHud();
}

// ================= self-update from GitHub =================
static std::atomic<bool> gUpdateReady{false};
static std::wstring gSelf, gNew;

static bool httpGet(const std::string &url, std::string &out, size_t maxBytes) {
    static HINTERNET net = nullptr;
    if (!net) {
        net = InternetOpenA("CookieBot", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (!net) return false;
        DWORD to = 8000;
        InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof to);
        InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof to);
    }
    std::string u = url + "?t=" + std::to_string((long long)time(nullptr));
    HINTERNET h = InternetOpenUrlA(net, u.c_str(), nullptr, 0,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE | INTERNET_FLAG_KEEP_CONNECTION, 0);
    bool ok = false;
    if (h) {
        DWORD code = 0, len = sizeof code;
        HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, nullptr);
        if (code == 200) {
            char buf[65536]; DWORD n;
            out.clear(); ok = true;
            while (InternetReadFile(h, buf, sizeof buf, &n) && n) {
                out.append(buf, n);
                if (out.size() > maxBytes) { ok = false; break; }
            }
        }
        InternetCloseHandle(h);
    }
    return ok;
}

static bool checkUpdate(bool manual) {
    std::string v;
    if (!httpGet(std::string(REPO_RAW) + "version.txt", v, 64)) { if (manual) logf("couldn't reach GitHub to check for updates"); return false; }
    int remote = atoi(v.c_str());
    if (remote <= VERSION) { if (manual) logf("already up to date (v%d)", VERSION); return false; }
    logf("update found: v%d -> v%d, downloading", VERSION, remote);
    std::string exe;
    if (!httpGet(std::string(REPO_RAW) + "CookieBot.ex_", exe, 64u << 20) || exe.size() < 100000 || exe.compare(0, 2, "MZ") != 0) {
        logf("update download failed - staying on v%d", VERSION);
        return false;
    }
    HANDLE f = CreateFileW(gNew.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, exe.data(), (DWORD)exe.size(), &w, nullptr) && w == exe.size();
    CloseHandle(f);
    if (!ok) { DeleteFileW(gNew.c_str()); return false; }
    logf("v%d downloaded - restarting into it", remote);
    return true;
}

static void updateThread() {
    for (;;) {
        bool manual = gUpdateCheckReq.exchange(false);
        if (checkUpdate(manual)) { gUpdateReady = true; gQuit = true; return; }
        double until = now() + 3600;
        while (!gQuit && !gUpdateCheckReq && now() < until) WaitForSingleObject(gUpdEvent, 500);
        if (gQuit) return;
    }
}

static void applyUpdate() {
    std::wstring old = gSelf + L".old";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(gSelf.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { logf("update: couldn't move old exe"); return; }
    if (!MoveFileExW(gNew.c_str(), gSelf.c_str(), MOVEFILE_REPLACE_EXISTING)) { MoveFileExW(old.c_str(), gSelf.c_str(), 0); logf("update: couldn't swap exe"); return; }
    STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
    std::wstring cmd = GetCommandLineW();
    if (CreateProcessW(gSelf.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--hours" && i + 1 < argc) cfgHours = atof(argv[++i]);
        else if (a == "--cps" && i + 1 < argc) cfgCps = std::min(12.0, atof(argv[++i]));
        else if (a == "--nogolden") cfgGolden = false;
        else if (a == "--nobuy") cfgBuy = false;
        else if (a == "--nofx") cfgFx = false;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&gFreq);
    timeBeginPeriod(1);
    SetConsoleTitleA("CookieBot - F8 pause/resume, F9 quit");
    gLog = fopen("cookiebot_log.txt", "a");
    gDbg = fopen("cookiebot_debug.txt", "w");
    srand((unsigned)time(nullptr));
    gBrain.load(BRAIN);
    {
        wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
        gSelf = p; gNew = gSelf + L".new";
        DeleteFileW((gSelf + L".old").c_str()); DeleteFileW(gNew.c_str());
    }
    std::thread(updateThread).detach();
    std::thread(panelThread).detach();
    std::thread(overlayThread).detach();
    std::thread(inputThread).detach();
    logf("CookieBot v%d - brain has %d purchases and %d upgrade types learned", VERSION, gBrain.purchases, (int)gBrain.corr.size());
    if (!ocrInit()) { MessageBoxW(nullptr, L"Windows OCR isn't available. Check cookiebot_log.txt.", L"CookieBot", MB_ICONERROR); return 1; }
    cc::init();
    setState("WAITING"); setTask("Click on the game's browser tab to start");
    logf("Click on the browser tab with the game to start.");
    while (!gQuit) {
        HWND fg = GetForegroundWindow();
        if (fg && !cc::active() && titleOf(fg).find("cookies") != std::string::npos) { gHwnd = fg; break; }
        if (GetAsyncKeyState(VK_F9) & 0x8000) gQuit = true;
        Sleep(200);
    }
    if (gQuit) {
        if (gUpdateReady) applyUpdate();
        return 0;
    }
    logf("Found the game: \"%s\". Starting in 3 s - let go of the mouse.", titleOf(gHwnd).c_str());
    setTask("Starting in 3 s - hands off the mouse");
    {
        HMONITOR m = MonitorFromWindow(gHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof mi}; GetMonitorInfo(m, &mi); gMon = mi.rcMonitor; gWork = mi.rcWork;
    }
    gResumeAt = now() + 3;
    gPaused = false;
    std::thread(econThread).detach();
    gStart = now();
    {
        Rect top{gMon.left, gMon.top, gMon.right, gMon.top + (gMon.bottom - gMon.top) / 6};
        for (auto &w : ocrRect(top, 1.5)) {
            std::string l = lower(w.text);
            size_t sl = l.find("//"); if (sl != std::string::npos) l = l.substr(sl + 2);
            size_t d = l.find('.');
            if (l.find("cookie") != std::string::npos && d != std::string::npos && d > 4) { gHostKey = l.substr(0, d); break; }
        }
        logf("watching for host '%s'", gHostKey.c_str());
    }

    bool wasPaused = false;
    double lastStatus = 0;
    std::string lastPlan;
    while (!gQuit) {
        if (cc::active()) { gOverlayOn = false; Sleep(100); continue; }
        processCmds();
        checkGoal();
        if (gForgetReq) { gBrain = Brain(); DeleteFileA(BRAIN); gForgetReq = false; logf("Brain wiped - starting from base training"); }
        if (gPaused || now() < gResumeAt) {
            gOverlayOn = false; wasPaused = true;
            setState(gPaused ? "PAUSED" : "STARTING"); setTask(gPaused ? "Paused - F8 or Resume to continue" : "Resuming - hands off the mouse");
            Sleep(100); continue;
        }
        if (wasPaused) { L.ok = false; wasPaused = false; }
        if (!gameFocused()) {
            gOverlayOn = false;
            setState("WAITING"); setTask("Waiting for the game window to be in front");
            if (now() - lastStatus > 30) { logf("waiting - game window isn't in front"); lastStatus = now(); }
            Sleep(500); continue;
        }
        HMONITOR m = MonitorFromWindow(gHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof mi}; GetMonitorInfo(m, &mi); gMon = mi.rcMonitor; gWork = mi.rcWork;

        if (!gameTitleOk()) {
            gOverlayOn = false;
            connectionWatch();
            setState("WAITING"); setTask("Waiting - this tab doesn't look like the game");
            if (now() - lastStatus > 30) { logf("waiting - tab title doesn't look like the game"); lastStatus = now(); }
            Sleep(1000); continue;
        }
        if (!L.ok) {
            setState("RUNNING"); setTask("Finding the game on screen");
            gLastScreenCheck = 0;
            screenGuard();
            if (gQuit || gPaused) continue;
            gOverlayOn = true;
            if (!detectLayout()) {
                gMiss++;
                if (gMiss >= 2) connectionWatch();
                Sleep(1500); continue;
            }
            gLastFull = -1000;
        }
        gOverlayOn = true;
        if (gMiss >= 3) { L.ok = false; gMiss = 0; continue; }
        if (!cookieStillThere() && !locateMainCookie()) {
            static double lastMsg = 0;
            if (now() - lastMsg > 20) { logf("lost the big cookie - re-finding the layout (details in cookiebot_debug.txt)"); lastMsg = now(); }
            L.ok = false; continue;
        }

        screenGuard();
        if (gQuit || gPaused || !L.ok) continue;
        if ((cfgBuy || gGoal.on) && now() - gLastFull > 3600) fullScan();
        else if ((cfgBuy || gGoal.on) && gLastBakery < 0 && !gItems.empty()) fullScan(false);
        else if (now() - gEconTime > 15) { if (!readEconNow()) gMiss++; }
        if (gQuit || gPaused || !L.ok) continue;

        double clickSecs = 2.0;
        if ((cfgBuy || gGoal.on) && !gItems.empty() && (now() > gBuyPauseUntil || gGoal.on)) {
            std::vector<Item> list;
            SimState s = currentState(list);
            double sess = cfgHours * 3600;
            double left = std::max(1200.0, sess - fmod(now() - gStart, sess));
            Plan p = plan(list, s, gBrain, left);
            bool goalBlocked = false;
            if (gGoal.on) {
                int gi = -1;
                for (size_t i = 0; i < list.size(); i++) if (list[i].key == gGoal.key) gi = (int)i;
                if (gi >= 0) {
                    p.first = gi;
                    p.gain = s.unit.count(gGoal.key) ? s.unit[gGoal.key] : 0;
                    p.wait = std::max(0.0, (list[gi].cost - s.bank) / std::max(1.0, s.P()));
                    p.effType = "your order";
                    p.why = "order: " + gGoal.name;
                } else { p.first = -1; goalBlocked = true; }
            }
            if (p.first >= 0) {
                const Item &nt = list[p.first];
                if (nt.nx && gStoreScroll == nt.scroll) setNext(true, nt.nx, nt.ny, 0);
                else {
                    int dir = (gStoreScroll < 0 || nt.scroll < gStoreScroll) ? -1 : 1;
                    setNext(true, (L.store.x0 + L.store.x1) / 2, dir < 0 ? L.store.y0 + 50 : L.store.y1 - 50, dir);
                }
            } else setNext(false);
            {
                std::lock_guard<std::mutex> lk(gHud.mu);
                gHud.alts = p.alts;
                if (p.first >= 0) {
                    gHud.want = list[p.first].name; gHud.wantCost = list[p.first].cost; gHud.wantGain = p.gain; gHud.wantType = p.effType;
                    gHud.why = gGoal.on ? p.why : "";
                } else { gHud.want.clear(); gHud.wantCost = 0; gHud.wantGain = 0; gHud.why = "nothing pays off before the session ends"; }
            }
            if (p.first >= 0) {
                if (p.why != lastPlan) { logf("plan: %s", p.why.c_str()); lastPlan = p.why; }
                if (s.bank >= list[p.first].cost) {
                    std::vector<std::string> bks;
                    for (auto &kv : s.unit) bks.push_back(kv.first);
                    Effect e = understand(list[p.first], bks);
                    setTask("Buying %s", list[p.first].name.c_str());
                    if (buy(list[p.first], p.gain, e.type())) continue;
                } else clickSecs = std::max(2.0, std::min(10.0, p.wait - 0.5));
            } else clickSecs = goalBlocked ? 5.0 : 10.0;
        } else setNext(false);
        clickFor(clickSecs);
    }
    gOverlayOn = false;
    gBrain.save(BRAIN);
    timeEndPeriod(1);
    logf("stopped. bonus cookies clicked this run: %d", gGoldenClicks);
    if (gPanel) PostMessageW(gPanel, WM_CLOSE, 0, 0);
    Sleep(300);
    if (gUpdateReady) applyUpdate();
    if (gLog) fclose(gLog);
    return 0;
}
