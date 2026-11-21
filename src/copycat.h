// ================= COPYCAT: watch you play any game, copy you, keep learning =================
// F6 = watch me (records you), F7 = copy me (plays, and keeps learning from its own runs).
namespace cc {

const int FW = 32, FH = 18, FG = FW * FH, FD = FG + 24;
const int K = 12, MAXSTAT = 32, MAXPLACE = 64;
const size_t MAXFRAMES = 60000;

struct Frame {
    uint8_t f[FD];
    uint8_t keys[32];
    uint8_t btn;
    uint8_t self;
    int16_t dx, dy;
    uint16_t place;
    float value;
    uint32_t up, down;  // stats that went up / down within 3 s after this frame
    double t;
};

struct Stat {
    std::string label;
    int bx = 0, by = 0;
    double last = 0, maxSeen = 0, lastT = 0, trend = 0;
    int seen = 0, changes = 0, samples = 0;
    float weight = 1;
    bool need = false;
};

struct Place {
    float c[FD];
    int n = 0;
    std::string look;
    int up[MAXSTAT] = {}, down[MAXSTAT] = {};
    int good = 0, bad = 0;
};

struct KeyStat { int held = 0; double change = 0, sx = 0, sy = 0; int good = 0, bad = 0, presses = 0; };

struct Plan { std::vector<WORD> keys; int btn = 0; float mdx = 0; double until = 0; bool show = false; std::string text; };
struct Memory { uint8_t f[FD]; std::string text; };

std::atomic<int> mode{0};  // 0 off, 1 watching, 2 copying, 3 asking
std::atomic<bool> takeover{false};
HWND tgt = nullptr;
std::string name = "game";
std::mutex mu;
std::vector<Frame> data;
std::vector<Stat> stats;
std::vector<Place> places;
std::vector<Memory> mems;
std::map<int, KeyStat> keyStats;
KeyStat idle;
double mouseCorrXY = 0, mouseXX = 0, mouseYY = 0, aimSlowSum = 0, aimAllSum = 0;
int aimSlowN = 0, aimAllN = 0;
double badColor[3] = {}, allColor[3] = {};
int badColorN = 0, allColorN = 0;

std::atomic<uint64_t> keyBits[4];
std::atomic<int> rawDx{0}, rawDy{0}, btnState{0};
std::atomic<double> physAt{0};
double dRef = 0;

// analysis shown on the HUD
std::mutex amu;
std::deque<std::string> notes;
std::vector<std::string> findings;
std::string goal = "-", confusedTxt = "-", actionTxt = "-", watchTxt = "-", placeTxt = "-";
double fps = 0, decideMs = 0, ocrMs = 0, confusion = 0, matchPct = 0;
int demoN = 0, selfN = 0;
Plan plan;
double showUntil = 0, confusedSince = 0, lastAsk = 0;
std::deque<std::string> answers;

static void note(const char *fmt, ...) {
    char b[256];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    { std::lock_guard<std::mutex> lk(amu); notes.push_back(b); while (notes.size() > 6) notes.pop_front(); }
    CreateDirectoryA("copycat", nullptr);
    std::string p = "copycat\\" + name + "_journal.txt";
    if (FILE *f = fopen(p.c_str(), "a")) {
        time_t t = time(nullptr); char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
        fprintf(f, "[%s] %s\n", ts, b); fclose(f);
    }
}

static std::string keyName(int vk) {
    if (vk == 1001) return "LMB"; if (vk == 1002) return "RMB"; if (vk == 1004) return "MMB";
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LONG lp = sc << 16;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_INSERT || vk == VK_DELETE) lp |= 1 << 24;
    char b[32];
    if (GetKeyNameTextA(lp, b, 32) > 0) return b;
    snprintf(b, 32, "key%d", vk); return b;
}

static bool hasKey(const uint8_t *k, int vk) { return k[vk >> 3] & (1 << (vk & 7)); }

void onKey(DWORD vk, bool down) {
    if (vk == VK_F6 || vk == VK_F7 || vk == VK_F8 || vk == VK_F9 || vk > 255) return;
    uint64_t bit = 1ull << (vk & 63);
    if (down) keyBits[vk >> 6] |= bit; else keyBits[vk >> 6] &= ~bit;
}
void onPhys() { if (mode == 2) physAt = now(); }
bool asking() { return mode == 3; }
HWND target() { return tgt; }
void answer(const std::string &s) { std::lock_guard<std::mutex> lk(amu); answers.push_back(s); }
bool active() { return mode != 0; }

// ---------- raw mouse (works even when games lock the cursor) ----------
static LRESULT CALLBACK rawProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        RAWINPUT ri; UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE) {
            auto &mm = ri.data.mouse;
            if (!(mm.usFlags & MOUSE_MOVE_ABSOLUTE)) { rawDx += mm.lLastX; rawDy += mm.lLastY; }
            USHORT f = mm.usButtonFlags;
            int b = btnState;
            if (f & RI_MOUSE_LEFT_BUTTON_DOWN) b |= 1; if (f & RI_MOUSE_LEFT_BUTTON_UP) b &= ~1;
            if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) b |= 2; if (f & RI_MOUSE_RIGHT_BUTTON_UP) b &= ~2;
            if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) b |= 4; if (f & RI_MOUSE_MIDDLE_BUTTON_UP) b &= ~4;
            btnState = b;
        }
    }
    return DefWindowProcW(h, m, w, l);
}
static void rawThread() {
    WNDCLASSW wc{}; wc.lpfnWndProc = rawProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"CookieBotRaw";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"CookieBotRaw", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE d{0x01, 0x02, RIDEV_INPUTSINK, h};
    RegisterRawInputDevices(&d, 1, sizeof d);
    MSG msg;
    while (!gQuit && GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
}

// ---------- output ----------
static uint8_t heldOut[32];
static int btnOut = 0;
static void sendKey(int vk, bool down) {
    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_INSERT || vk == VK_DELETE)
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof in);
}
static void sendBtn(int bit, bool down) {
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dwFlags = bit == 1 ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP) : bit == 2 ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
                                                                                         : (down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP);
    SendInput(1, &in, sizeof in);
}
static void sendMove(int dx, int dy) {
    if (!dx && !dy) return;
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dx = dx; in.mi.dy = dy; in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof in);
}
static void applyOut(const uint8_t *keys, int btn) {
    for (int vk = 1; vk < 256; vk++) {
        bool want = hasKey(keys, vk), have = hasKey(heldOut, vk);
        if (want != have) { sendKey(vk, want); if (want) heldOut[vk >> 3] |= 1 << (vk & 7); else heldOut[vk >> 3] &= ~(1 << (vk & 7)); }
    }
    for (int b : {1, 2, 4}) if ((btn & b) != (btnOut & b)) sendBtn(b, btn & b);
    btnOut = btn;
}
static void releaseAll() { uint8_t z[32] = {}; applyOut(z, 0); }

// ---------- seeing ----------
static bool targetRect(Rect &r) {
    RECT c; if (!tgt || !GetClientRect(tgt, &c)) return false;
    POINT p{0, 0}; ClientToScreen(tgt, &p);
    r = {p.x, p.y, p.x + c.right, p.y + c.bottom};
    return c.right > 40 && c.bottom > 40;
}
static bool capture(uint8_t *f, float &red) {
    Rect r; if (!targetRect(r)) return false;
    const int SW = 256, SH = 144;
    static HDC mem = nullptr; static HBITMAP bmp = nullptr; static uint32_t *bits = nullptr;
    HDC scr = GetDC(nullptr);
    if (!mem) {
        mem = CreateCompatibleDC(scr);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = SW; bi.bmiHeader.biHeight = -SH; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&bits, nullptr, 0);
        SelectObject(mem, bmp);
    }
    SetStretchBltMode(mem, COLORONCOLOR);
    StretchBlt(mem, 0, 0, SW, SH, scr, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, SRCCOPY);
    GdiFlush();
    ReleaseDC(nullptr, scr);
    int cx = SW / FW, cy = SH / FH;
    double tr = 0, tg = 0, tb = 0;
    double tile[8][3] = {};
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) {
            int s = 0, rr = 0, gg = 0, bb = 0;
            for (int j = 0; j < cy; j++)
                for (int i = 0; i < cx; i++) {
                    uint32_t p = bits[(y * cy + j) * SW + x * cx + i];
                    rr += (p >> 16) & 255; gg += (p >> 8) & 255; bb += p & 255; s++;
                }
            rr /= s; gg /= s; bb /= s;
            f[y * FW + x] = (uint8_t)((rr * 3 + gg * 6 + bb) / 10);
            int ti = (y * 2 / FH) * 4 + x * 4 / FW;
            tile[ti][0] += rr; tile[ti][1] += gg; tile[ti][2] += bb;
            tr += rr; tg += gg; tb += bb;
        }
    int per = FG / 8;
    for (int i = 0; i < 8; i++) for (int c = 0; c < 3; c++) f[FG + i * 3 + c] = (uint8_t)(tile[i][c] / per);
    red = (float)((tr - (tg + tb) / 2) / FG);
    return true;
}
static int dist(const uint8_t *a, const uint8_t *b) {
    int s = 0;
    for (int i = 0; i < FD; i++) s += std::abs((int)a[i] - (int)b[i]);
    return s;
}
static void shiftOf(const uint8_t *a, const uint8_t *b, int &sx, int &sy, double &change) {
    int best = INT_MAX; sx = sy = 0;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -3; dx <= 3; dx++) {
            int s = 0, n = 0;
            for (int y = 3; y < FH - 3; y++)
                for (int x = 4; x < FW - 4; x++) { s += std::abs((int)a[y * FW + x] - (int)b[(y + dy) * FW + x + dx]); n++; }
            s = s * 100 / n;
            if (s < best) { best = s; sx = dx; sy = dy; }
        }
    int c = 0; for (int i = 0; i < FG; i++) c += std::abs((int)a[i] - (int)b[i]);
    change = (double)c / FG;
}
static std::string lookOf(const uint8_t *f) {
    double r = 0, g = 0, b = 0; for (int i = 0; i < 8; i++) { r += f[FG + i * 3]; g += f[FG + i * 3 + 1]; b += f[FG + i * 3 + 2]; }
    r /= 8; g /= 8; b /= 8;
    double l = (r + g + b) / 3;
    std::string s = l < 60 ? "dark" : l > 170 ? "bright" : "mid";
    if (g > r + 15 && g > b + 10) s += " green";
    else if (b > r + 15 && b > g) s += " blue";
    else if (r > g + 20 && r > b + 20) s += " red";
    else if (r > b + 25 && g > b + 15) s += " sandy";
    else if (std::abs(r - g) < 15 && std::abs(g - b) < 15) s += " grey";
    return s;
}
static int placeFor(const uint8_t *f) {
    int best = -1; double bd = 1e18;
    for (size_t i = 0; i < places.size(); i++) {
        double d = 0;
        for (int k = 0; k < FD; k++) d += std::fabs(places[i].c[k] - f[k]);
        if (d < bd) { bd = d; best = (int)i; }
    }
    double th = FD * 38.0;
    if ((best < 0 || bd > th) && places.size() < MAXPLACE) {
        Place p; for (int k = 0; k < FD; k++) p.c[k] = f[k];
        p.n = 1; p.look = lookOf(f);
        places.push_back(p);
        note("found a new place: Place %d (%s)", (int)places.size(), p.look.c_str());
        return (int)places.size() - 1;
    }
    Place &p = places[best];
    p.n++;
    float a = std::max(0.02f, 1.f / p.n);
    for (int k = 0; k < FD; k++) p.c[k] += (f[k] - p.c[k]) * a;
    return best;
}

// ---------- rewards from numbers on screen ----------
static float weightFor(const std::string &l, bool &needLike) {
    needLike = false;
    for (const char *w : {"health", "hp", "life", "lives", "shield", "armor", "armour"}) if (l.find(w) != std::string::npos) { needLike = true; return 3; }
    for (const char *w : {"food", "hunger", "water", "thirst", "stamina", "energy", "calories", "hydration"}) if (l.find(w) != std::string::npos) { needLike = true; return 2; }
    for (const char *w : {"radiation", "rad", "cold", "heat", "poison", "bleed", "wanted"}) if (l.find(w) != std::string::npos) return -2;
    for (const char *w : {"ammo", "time", "fps", "ping", "ms", "x", "y", "z", "day", "clock"}) if (l == w || l.find(w) == 0) return 0;
    for (const char *w : {"score", "kill", "coin", "gold", "xp", "point", "money", "scrap", "wood", "stone", "metal", "ore", "cloth", "sulfur", "fuel", "loot"})
        if (l.find(w) != std::string::npos) return 2;
    return 1;
}
static void reward(float r, int statIdx, bool up, const char *why) {
    double t = now();
    std::lock_guard<std::mutex> lk(mu);
    for (int i = (int)data.size() - 1; i >= 0 && t - data[i].t < 3.0; i--) {
        data[i].value += r * (float)std::exp(-(t - data[i].t) / 1.5);
        if (statIdx >= 0 && statIdx < 32) { if (up) data[i].up |= 1u << statIdx; else data[i].down |= 1u << statIdx; }
    }
    if (!data.empty()) {
        int pl = data.back().place;
        if (pl < (int)places.size()) {
            if (statIdx >= 0 && statIdx < MAXSTAT) { if (up) places[pl].up[statIdx]++; else places[pl].down[statIdx]++; }
            if (r > 0) places[pl].good++; else if (r < 0) places[pl].bad++;
        }
        const Frame &f = data.back();
        std::string held;
        for (int vk = 1; vk < 256; vk++) if (hasKey(f.keys, vk)) { held += (held.empty() ? "" : "+") + keyName(vk); auto &ks = keyStats[vk]; if (r > 0) ks.good++; else if (r < 0) ks.bad++; }
        if (f.btn & 1) { held += (held.empty() ? "" : "+") + std::string("LMB"); auto &ks = keyStats[1001]; if (r > 0) ks.good++; else if (r < 0) ks.bad++; }
        if (r < 0) { for (int c = 0; c < 3; c++) { double s = 0; for (int i = 0; i < 8; i++) s += f.f[FG + i * 3 + c]; badColor[c] += s / 8; } badColorN++; }
        note("%s %s at Place %d%s%s", r > 0 ? "GOOD:" : r < 0 ? "BAD:" : "", why, pl + 1, held.empty() ? "" : " while pressing ", held.c_str());
    }
}

static void ocrThread() {
    while (!gQuit) {
        Sleep(120);
        if (!mode || mode == 3 || GetForegroundWindow() != tgt) continue;
        Rect r; if (!targetRect(r)) continue;
        double t0 = now();
        double sc = std::min(1.0, 1800.0 / std::max(1, r.x1 - r.x0));
        auto words = ocrRect(r, sc);
        ocrMs = (now() - t0) * 1000;
        struct Pend { float r; int idx; bool up; std::string why; };
        std::vector<Pend> pend;
        std::string watch;
        std::unique_lock<std::mutex> statLock(mu);
        for (size_t i = 0; i < words.size(); i++) {
            const Word &w = words[i];
            std::string digits; size_t k = 0;
            while (k < w.text.size() && !isdigit((unsigned char)w.text[k])) k++;
            while (k < w.text.size() && (isdigit((unsigned char)w.text[k]) || w.text[k] == '.' || w.text[k] == ',')) digits += w.text[k++];
            double v;
            if (digits.empty() || !parseNum(digits, v)) continue;
            std::string label = lettersOnly(lower(w.text));
            if (label.size() < 2) {
                int bestD = 260;
                for (size_t j = 0; j < words.size(); j++) {
                    if (j == i) continue;
                    const Word &o = words[j];
                    std::string ol = lettersOnly(lower(o.text));
                    if (ol.size() < 2) continue;
                    int dy = std::abs((o.y + o.h / 2) - (w.y + w.h / 2)), dx = w.x - (o.x + o.w);
                    int d = (dy < w.h && dx > -10 && dx < 250) ? dx : (dy < 60 && std::abs(o.x - w.x) < 80 && o.y < w.y) ? 120 + dy : 99999;
                    if (d < bestD) { bestD = d; label = ol; }
                }
            }
            if (label.empty()) label = "number";
            if (label == "x" || label == "y" || label == "z" || label == "pos" || label == "grid") continue;
            int bx = (w.x - r.x0) / 60, by = (w.y - r.y0) / 60;
            int idx = -1;
            for (size_t s = 0; s < stats.size(); s++)
                if (stats[s].label == label && std::abs(stats[s].bx - bx) <= 1 && std::abs(stats[s].by - by) <= 1) idx = (int)s;
            if (idx < 0) {
                if (stats.size() >= MAXSTAT) continue;
                Stat s; s.label = label; s.bx = bx; s.by = by; s.last = v; s.maxSeen = v; s.lastT = now();
                bool nl; s.weight = weightFor(label, nl); s.need = nl;
                stats.push_back(s); idx = (int)stats.size() - 1;
                continue;
            }
            Stat &s = stats[idx];
            s.seen++; s.samples++;
            if (v != s.last) {
                s.changes++;
                bool clock = s.samples > 12 && s.changes > s.samples * 0.7;
                double d = v - s.last;
                if (std::fabs(d) > std::max(1.0, std::fabs(s.last)) * 50) { s.last = v; continue; }  // misread
                s.trend = s.trend * 0.7 + (d > 0 ? 1 : -1) * 0.3;
                if (!clock && s.weight != 0 && s.seen > 2) {
                    float r2 = (d > 0 ? 1.f : -1.f) * s.weight;
                    char why[128]; snprintf(why, sizeof why, "%s %s -> %s", s.label.c_str(), fmtNum(s.last).c_str(), fmtNum(v).c_str());
                    pend.push_back({r2, idx, d > 0, why});
                }
                s.last = v; s.maxSeen = std::max(s.maxSeen, v);
            }
        }
        for (auto &s : stats) if (s.seen > 2 && s.weight != 0) watch += s.label + " " + fmtNum(s.last) + "  ";
        statLock.unlock();
        for (auto &p : pend) reward(p.r, p.idx, p.up, p.why.c_str());
        std::lock_guard<std::mutex> lk(amu);
        watchTxt = watch.empty() ? "no numbers on screen yet" : watch;
    }
}

// ---------- analysis ----------
static void analyze() {
    std::vector<std::string> out;
    std::unique_lock<std::mutex> dataLock(mu);
    double base = idle.held ? idle.change / idle.held : 1;
    std::vector<std::pair<int, KeyStat>> ks(keyStats.begin(), keyStats.end());
    std::sort(ks.begin(), ks.end(), [](auto &a, auto &b) { return a.second.held > b.second.held; });
    for (auto &kv : ks) {
        const KeyStat &k = kv.second;
        if (k.held < 15 && k.presses < 4) continue;
        std::string n = keyName(kv.first);
        double ch = k.held ? k.change / k.held : 0, sx = k.held ? k.sx / k.held : 0;
        char b[200];
        std::string what;
        if (kv.first >= 1000) what = "";
        else if (ch > base * 1.6) what = std::abs(sx) > 0.4 ? (sx > 0 ? "moves/turns you (view slides right)" : "moves/turns you (view slides left)") : "moves you or changes the view a lot";
        else if (ch < base * 0.8) what = "doesn't change the view much (action/menu?)";
        else what = "small effect on the view";
        snprintf(b, sizeof b, "%s: %s%s%s good %d / bad %d", n.c_str(), what.c_str(), what.empty() ? "" : " -", k.presses ? (" pressed " + std::to_string(k.presses) + "x,").c_str() : "", k.good, k.bad);
        out.push_back(b);
        if (out.size() >= 5) break;
    }
    if (mouseXX > 0) {
        double c = mouseCorrXY / std::sqrt(mouseXX * mouseYY + 1e-9);
        if (std::fabs(c) > 0.25) out.push_back(std::string("mouse turns the camera") + (c < 0 ? "" : " (inverted)"));
    }
    if (aimSlowN > 5 && aimAllN > 50) {
        double r = (aimSlowSum / aimSlowN) / (aimAllSum / aimAllN + 1e-9);
        char b[120]; snprintf(b, sizeof b, r < 0.7 ? "you slow your aim before clicking (x%.2f speed) - precision" : "you click while still moving the mouse (x%.2f)", r);
        out.push_back(b);
    }
    if (badColorN > 2 && allColorN > 50) {
        const char *nm[3] = {"red", "green", "blue"};
        int bi = 0; double bd = -1e9;
        for (int c = 0; c < 3; c++) { double d = badColor[c] / badColorN - allColor[c] / allColorN; if (d > bd) { bd = d; bi = c; } }
        if (bd > 8) { char b[120]; snprintf(b, sizeof b, "danger: screens get more %s right before bad stuff", nm[bi]); out.push_back(b); }
    }
    for (size_t s = 0; s < stats.size() && out.size() < 10; s++) {
        int bestP = -1, bestN = 0;
        for (size_t p = 0; p < places.size(); p++) if (places[p].up[s] > bestN) { bestN = places[p].up[s]; bestP = (int)p; }
        if (bestP >= 0 && bestN >= 2) {
            char b[160]; snprintf(b, sizeof b, "%s goes up most at Place %d (%s)", stats[s].label.c_str(), bestP + 1, places[bestP].look.c_str());
            out.push_back(b);
        }
    }
    dataLock.unlock();
    std::lock_guard<std::mutex> lk(amu);
    findings = out;
}

// ---------- understanding your answer ----------
static Plan parseAnswer(const std::string &raw) {
    Plan p; p.text = raw;
    std::string t = " " + lower(raw) + " ";
    auto has = [&](const char *w) { return t.find(w) != std::string::npos; };
    double secs = 3;
    for (size_t i = 0; i < t.size(); i++) if (isdigit((unsigned char)t[i])) { secs = std::max(0.5, std::min(30.0, atof(t.c_str() + i))); break; }
    if (has("show") || has("watch") || has("let me") || has("i'll do") || has("ill do")) { p.show = true; p.until = now() + 15; return p; }
    if (has("wait") || has("nothing") || has("idle") || has("afk") || has("stand")) { p.until = now() + secs; return p; }
    if (has("forward") || has(" run") || has(" go ") || has("walk") || has("move") || has("explore") || has("sprint") || has("drive")) p.keys.push_back('W');
    if (has("back") || has("retreat") || has("away") || has("reverse")) { p.keys.erase(std::remove(p.keys.begin(), p.keys.end(), 'W'), p.keys.end()); p.keys.push_back('S'); }
    if (has(" left")) p.keys.push_back('A');
    if (has(" right")) p.keys.push_back('D');
    if (has("jump") || has("climb") || has(" up ")) p.keys.push_back(VK_SPACE);
    if (has("crouch") || has("duck") || has("sneak")) p.keys.push_back(VK_CONTROL);
    if (has("sprint") || has("fast") || has("run away")) p.keys.push_back(VK_SHIFT);
    if (has("shoot") || has("fire") || has("attack") || has("kill") || has("hit") || has("mine") || has("chop") || has("gather") || has("farm") ||
        has("break") || has("punch"))
        p.btn |= 1;
    if (has("aim") || has("scope") || has("block")) p.btn |= 2;
    if (has("reload")) p.keys.push_back('R');
    if (has("use") || has("interact") || has("open") || has("pick") || has("loot") || has("grab") || has("eat") || has("drink") || has("enter") || has("craft"))
        p.keys.push_back('E');
    if (has("inventory") || has("bag")) p.keys.push_back(VK_TAB);
    if (has("turn around")) p.mdx = 60;
    else if (has("look around") || has("search") || has("explore") || has("find")) p.mdx = 12;
    else if (has("turn left")) p.mdx = -25;
    else if (has("turn right")) p.mdx = 25;
    p.until = now() + secs;
    return p;
}

// ---------- the main loop ----------
static void fileSave() {
    CreateDirectoryA("copycat", nullptr);
    std::string p = "copycat\\" + name + ".rec";
    std::lock_guard<std::mutex> lk(mu);
    if (FILE *f = fopen(p.c_str(), "wb")) {
        uint32_t magic = 0x31424343, n = (uint32_t)data.size(), np = (uint32_t)places.size(), nm = (uint32_t)mems.size();
        fwrite(&magic, 4, 1, f); fwrite(&n, 4, 1, f);
        double t0 = data.empty() ? 0 : data.front().t;
        for (auto fr : data) { fr.t -= t0; fwrite(&fr, sizeof fr, 1, f); }
        fwrite(&np, 4, 1, f);
        for (auto &pl : places) { fwrite(pl.c, sizeof pl.c, 1, f); fwrite(&pl.n, 4, 1, f); }
        fwrite(&nm, 4, 1, f);
        for (auto &m : mems) { fwrite(m.f, FD, 1, f); uint32_t L = (uint32_t)m.text.size(); fwrite(&L, 4, 1, f); fwrite(m.text.data(), 1, L, f); }
        fclose(f);
    }
}
static void fileLoad() {
    std::string p = "copycat\\" + name + ".rec";
    std::lock_guard<std::mutex> lk(mu);
    data.clear(); places.clear(); mems.clear();
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return;
    uint32_t magic = 0, n = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == 0x31424343 && fread(&n, 4, 1, f) == 1 && n <= MAXFRAMES * 2) {
        data.resize(n);
        if (n && fread(data.data(), sizeof(Frame), n, f) != n) data.clear();
        double base = now() - 1e6;
        for (auto &fr : data) fr.t += base;
        uint32_t np = 0;
        if (fread(&np, 4, 1, f) == 1 && np <= MAXPLACE)
            for (uint32_t i = 0; i < np; i++) { Place pl; if (fread(pl.c, sizeof pl.c, 1, f) != 1 || fread(&pl.n, 4, 1, f) != 1) break; uint8_t tmp[FD]; for (int k = 0; k < FD; k++) tmp[k] = (uint8_t)pl.c[k]; pl.look = lookOf(tmp); places.push_back(pl); }
        uint32_t nm = 0;
        if (fread(&nm, 4, 1, f) == 1 && nm < 10000)
            for (uint32_t i = 0; i < nm; i++) {
                Memory m; uint32_t L = 0;
                if (fread(m.f, FD, 1, f) != 1 || fread(&L, 4, 1, f) != 1 || L > 500) break;
                m.text.resize(L); if (L && fread(&m.text[0], 1, L, f) != L) break;
                mems.push_back(m);
            }
    }
    fclose(f);
}
static std::string exeOf(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    std::string out = "game";
    if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t b[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(ph, 0, b, &n)) {
            std::wstring w(b, n);
            size_t s = w.find_last_of(L"\\/"); if (s != std::wstring::npos) w = w.substr(s + 1);
            size_t d = w.rfind(L'.'); if (d != std::wstring::npos) w = w.substr(0, d);
            out.clear(); for (wchar_t c : w) if (c < 128 && (isalnum(c) || c == '-' || c == '_')) out += (char)c;
            if (out.empty()) out = "game";
        }
        CloseHandle(ph);
    }
    return out;
}
static void computeRef() {
    std::lock_guard<std::mutex> lk(mu);
    if (data.size() < 100) { dRef = FD * 12.0; return; }
    std::vector<int> ds;
    for (int s = 0; s < 120; s++) {
        size_t i = rand() % data.size();
        int best = INT_MAX;
        for (int r = 0; r < 2500; r++) {
            size_t j = ((size_t)rand() * 32768 + rand()) % data.size();
            if (std::fabs(data[j].t - data[i].t) < 1.0) continue;
            best = std::min(best, dist(data[i].f, data[j].f));
        }
        if (best < INT_MAX) ds.push_back(best);
    }
    std::sort(ds.begin(), ds.end());
    dRef = ds.empty() ? FD * 12.0 : std::max(FD * 2.0, (double)ds[ds.size() / 2]);
}

static void openAsk() {
    mode = 3;
    releaseAll();
    { std::lock_guard<std::mutex> lk(gPromptMu); gPromptText.clear(); gPromptOpen = true; gPromptLast = now(); }
    MessageBeep(MB_ICONQUESTION);
    note("CONFUSED: nothing I've seen looks like this - asking you");
    lastAsk = now();
}

static void decide(const uint8_t *f, uint8_t *keysOut, int &btn, int &mdx, int &mdy) {
    memset(keysOut, 0, 32); btn = 0; mdx = mdy = 0;
    double t0 = now();
    struct Hit { int d; size_t i; };
    std::vector<Hit> top;
    std::vector<int> needIdx;
    uint32_t needMask = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t s = 0; s < stats.size(); s++)
            if (stats[s].need && stats[s].seen > 2 && stats[s].maxSeen > 0 && (stats[s].last < stats[s].maxSeen * 0.55 || stats[s].trend < -0.5)) needIdx.push_back((int)s);
        for (int s : needIdx) if (s < 32) needMask |= 1u << s;
        if (data.size() < 60) { std::lock_guard<std::mutex> l2(amu); goal = "need more recording - press F6 and play for a bit"; return; }
        double tNow = now();
        for (size_t i = 0; i < data.size(); i++) {
            if (data[i].self && tNow - data[i].t < 2.0) continue;
            int d = dist(f, data[i].f);
            if ((int)top.size() < K) { top.push_back({d, i}); std::push_heap(top.begin(), top.end(), [](const Hit &a, const Hit &b) { return a.d < b.d; }); }
            else if (d < top.front().d) {
                std::pop_heap(top.begin(), top.end(), [](const Hit &a, const Hit &b) { return a.d < b.d; });
                top.back() = {d, i};
                std::push_heap(top.begin(), top.end(), [](const Hit &a, const Hit &b) { return a.d < b.d; });
            }
        }
        if (top.empty()) return;
        int dmin = INT_MAX; for (auto &h : top) dmin = std::min(dmin, h.d);
        confusion = dmin / std::max(1.0, dRef);
        matchPct = std::max(0.0, 100.0 * (1.0 - dmin / (FD * 60.0)));
        double ks[256] = {}, bs[3] = {}, W = 0, mx = 0, my = 0, val = 0;
        for (auto &h : top) {
            const Frame &fr = data[h.i];
            double w = std::exp(-(h.d - dmin) / (dRef * 0.5 + 1)) * std::max(0.1, std::min(3.0, 1.0 + fr.value)) * (fr.self ? 0.6 : 1.0);
            if (needMask && (fr.up & needMask)) w *= 3;
            if (needMask && (fr.down & needMask)) w *= 0.4;
            W += w; val += w * fr.value;
            for (int vk = 1; vk < 256; vk++) if (hasKey(fr.keys, vk)) ks[vk] += w;
            for (int b = 0; b < 3; b++) if (fr.btn & (1 << b)) bs[b] += w;
            mx += w * fr.dx; my += w * fr.dy;
        }
        if (W <= 0) return;
        static double prevKs[256], prevBs[3];
        std::string act;
        for (int vk = 1; vk < 256; vk++) {
            double p = ks[vk] / W, th = prevKs[vk] > 0.5 ? 0.42 : 0.58;
            prevKs[vk] = p;
            if (p > th) { keysOut[vk >> 3] |= 1 << (vk & 7); act += keyName(vk) + " "; }
        }
        for (int b = 0; b < 3; b++) {
            double p = bs[b] / W, th = prevBs[b] > 0.5 ? 0.42 : 0.58;
            prevBs[b] = p;
            if (p > th) { btn |= 1 << b; act += b == 0 ? "LMB " : b == 1 ? "RMB " : "MMB "; }
        }
        mdx = (int)std::lround(mx / W); mdy = (int)std::lround(my / W);
        if (rand() % 100 < 4) { mdx += (rand() % 41) - 20; act += "(trying something) "; }
        std::string g;
        if (!needIdx.empty()) {
            const Stat &s = stats[needIdx[0]];
            int bestP = -1, bestN = 0;
            for (size_t p = 0; p < places.size(); p++) if (places[p].up[needIdx[0]] > bestN) { bestN = places[p].up[needIdx[0]]; bestP = (int)p; }
            char b[200];
            snprintf(b, sizeof b, "%s is low (%s) - doing what raised it before%s", s.label.c_str(), fmtNum(s.last).c_str(),
                     bestP >= 0 ? (" (Place " + std::to_string(bestP + 1) + ")").c_str() : "");
            g = b;
        } else if (val / W > 0.3) g = "heading for something good (similar moments paid off)";
        else if (val / W < -0.3) g = "careful - similar moments went badly";
        else g = "copying what you did here";
        std::lock_guard<std::mutex> l2(amu);
        goal = g; actionTxt = act.empty() ? "nothing" : act;
        confusedTxt = confusion > 2.2 ? "very - this doesn't look like anything I've seen" : confusion > 1.5 ? "a bit - closest memory is a loose match" : "no";
    }
    decideMs = (now() - t0) * 1000;
}

static void loop() {
    uint8_t prev[FD] = {}; bool havePrev = false;
    float redAvg = 0;
    double next = now(), lastSec = now(), lastAnalyze = 0, lastSave = now();
    int frames = 0;
    std::vector<double> recentDx;
    while (!gQuit && mode) {
        next += 1.0 / 30;
        double wait = next - now();
        if (wait > 0.002) Sleep((DWORD)(wait * 1000 - 1));
        while (now() < next) YieldProcessor();
        if (now() - next > 0.2) next = now();
        if (now() - lastSec >= 1) { fps = frames / (now() - lastSec); frames = 0; lastSec = now(); }
        if (!IsWindow(tgt)) { note("the game window closed - stopping"); mode = 0; break; }
        if (GetForegroundWindow() != tgt) { releaseAll(); continue; }
        uint8_t f[FD]; float red;
        if (!capture(f, red)) continue;
        frames++;
        {
            std::lock_guard<std::mutex> lk(amu);
            for (int c = 0; c < 3; c++) { double s = 0; for (int i = 0; i < 8; i++) s += f[FG + i * 3 + c]; allColor[c] += s / 8; }
            allColorN++;
        }
        if (havePrev && red - redAvg > 18) reward(-1.5f, -1, false, "screen flashed red (damage?)");
        redAvg = redAvg * 0.95f + red * 0.05f;
        int sx = 0, sy = 0; double change = 0;
        if (havePrev) shiftOf(prev, f, sx, sy, change);
        memcpy(prev, f, FD); havePrev = true;

        int m = mode;
        if (m == 3) {
            std::string a;
            { std::lock_guard<std::mutex> lk(amu); if (!answers.empty()) { a = answers.front(); answers.pop_front(); } }
            if (!a.empty()) {
                plan = parseAnswer(a);
                Memory mm; memcpy(mm.f, f, FD); mm.text = a;
                { std::lock_guard<std::mutex> lk(mu); mems.push_back(mm); }
                note("you said: \"%s\" -> %s", a.c_str(), plan.show ? "watching you for 15 s" : "doing that now");
                if (plan.show) showUntil = now() + 15;
                confusedSince = 0;
                mode = 2;
            }
            continue;
        }
        bool watching = m == 1 || (m == 2 && (now() - physAt < 2.0 || now() < showUntil));
        if (m == 2 && watching && !takeover) { takeover = true; releaseAll(); note("you took over - watching you"); }
        if (m == 2 && !watching && takeover) { takeover = false; note("back to playing on my own"); }

        Frame fr{}; memcpy(fr.f, f, FD); fr.t = now();
        int dxr = rawDx.exchange(0), dyr = rawDy.exchange(0);
        if (watching) {
            for (int i = 0; i < 4; i++) { uint64_t b = keyBits[i]; memcpy(fr.keys + i * 8, &b, 8); }
            fr.btn = (uint8_t)btnState; fr.dx = (int16_t)std::max(-3000, std::min(3000, dxr)); fr.dy = (int16_t)std::max(-3000, std::min(3000, dyr));
            fr.self = 0;
        } else {
            uint8_t ko[32]; int b, mx, my;
            bool planOn = now() < plan.until && !plan.show;
            if (planOn) {
                memset(ko, 0, 32);
                for (WORD vk : plan.keys) ko[vk >> 3] |= 1 << (vk & 7);
                b = plan.btn; mx = (int)plan.mdx; my = 0;
                std::lock_guard<std::mutex> lk(amu); goal = "doing what you said: \"" + plan.text + "\""; actionTxt = "following your answer";
            } else {
                decide(f, ko, b, mx, my);
                if (confusion > 2.2) {
                    if (confusedSince == 0) confusedSince = now();
                    if (now() - confusedSince > 1.5) {
                        int bm = -1, bd = INT_MAX;
                        { std::lock_guard<std::mutex> lk(mu); for (size_t i = 0; i < mems.size(); i++) { int d = dist(f, mems[i].f); if (d < bd) { bd = d; bm = (int)i; } } }
                        if (bm >= 0 && bd < dRef * 1.6) {
                            plan = parseAnswer(mems[bm].text);
                            note("this looks like when you said \"%s\" - doing that", mems[bm].text.c_str());
                            confusedSince = 0;
                        } else if (now() - lastAsk > 20) { openAsk(); continue; }
                    }
                } else confusedSince = 0;
            }
            applyOut(ko, b);
            sendMove(mx, my);
            memcpy(fr.keys, ko, 32); fr.btn = (uint8_t)b; fr.dx = (int16_t)mx; fr.dy = (int16_t)my; fr.self = 1;
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            fr.place = (uint16_t)std::max(0, placeFor(f));
            data.push_back(fr);
            if (fr.self) selfN++; else demoN++;
            if (data.size() > MAXFRAMES) {
                auto it = std::find_if(data.begin(), data.end(), [](const Frame &x) { return x.self; });
                data.erase(it != data.end() ? it : data.begin());
            }
            if (watching) {
                int held = 0;
                for (int vk = 1; vk < 256; vk++)
                    if (hasKey(fr.keys, vk)) {
                        auto &k = keyStats[vk]; k.held++; k.change += change; k.sx += sx; k.sy += sy; held++;
                        if (data.size() > 1 && !hasKey(data[data.size() - 2].keys, vk)) k.presses++;
                    }
                for (int b = 0; b < 3; b++)
                    if (fr.btn & (1 << b)) {
                        auto &k = keyStats[1001 + (b == 0 ? 0 : b == 1 ? 1 : 3)]; k.held++;
                        if (data.size() > 1 && !(data[data.size() - 2].btn & (1 << b))) {
                            k.presses++;
                            if (b == 0 && recentDx.size() >= 9) { double s = 0; for (double v : recentDx) s += v; aimSlowSum += s / recentDx.size(); aimSlowN++; }
                        }
                    }
                if (!held && !fr.btn && std::abs(fr.dx) + std::abs(fr.dy) < 3) { idle.held++; idle.change += change; }
                if (std::abs(fr.dx) > 2) { mouseCorrXY += fr.dx * sx; mouseXX += (double)fr.dx * fr.dx; mouseYY += (double)sx * sx; }
                double sp = std::hypot((double)fr.dx, (double)fr.dy);
                recentDx.push_back(sp); if (recentDx.size() > 9) recentDx.erase(recentDx.begin());
                aimAllSum += sp; aimAllN++;
            }
            std::lock_guard<std::mutex> l2(amu);
            placeTxt = "Place " + std::to_string(fr.place + 1) + " (" + places[fr.place].look + ")";
            if (watching) { actionTxt = "recording you"; if (m == 1) goal = "learning what you do"; }
        }
        if (now() - lastAnalyze > 2) { analyze(); lastAnalyze = now(); }
        if (now() - lastSave > 120) { fileSave(); lastSave = now(); }
    }
    releaseAll();
}

static std::thread worker;
void stop() {
    if (!mode) return;
    int was = mode;
    mode = 0;
    if (worker.joinable()) worker.join();
    releaseAll();
    analyze();
    fileSave();
    std::vector<std::string> fs;
    { std::lock_guard<std::mutex> lk(amu); fs = findings; }
    note("stopped %s - %d frames of you, %d of me, %d places, %d things noted", was == 1 ? "watching" : "copying", demoN, selfN, (int)places.size(), (int)fs.size());
    for (auto &x : fs) note("finding: %s", x.c_str());
    { std::lock_guard<std::mutex> lk(gPromptMu); gPromptOpen = false; gPromptText.clear(); }
    logf("Copycat stopped. Journal: copycat\\%s_journal.txt", name.c_str());
}
void start(int m) {
    HWND fg = GetForegroundWindow();
    if (mode) { int was = mode; stop(); if (was == m || (was >= 2 && m == 2)) return; }
    if (!fg || fg == gPanel) { logf("Copycat: click the game window first, then press F6/F7"); return; }
    if (worker.joinable()) worker.join();
    tgt = fg;
    std::string n = exeOf(fg);
    if (n != name || data.empty()) {
        name = n; fileLoad();
        std::lock_guard<std::mutex> lk(mu);
        stats.clear(); stats.reserve(MAXSTAT); keyStats.clear(); idle = KeyStat{};
    }
    { std::lock_guard<std::mutex> lk(mu); demoN = 0; selfN = 0; for (auto &d : data) (d.self ? selfN : demoN)++; }
    computeRef();
    memset(heldOut, 0, sizeof heldOut); btnOut = 0;
    for (auto &b : keyBits) b = 0;
    rawDx = 0; rawDy = 0; takeover = false; plan = Plan{}; showUntil = 0; confusedSince = 0;
    gPaused = true;
    mode = m;
    note("%s %s (%d frames remembered)", m == 1 ? "watching you play" : "playing", name.c_str(), (int)data.size());
    logf("Copycat %s '%s' - %s", m == 1 ? "watching" : "copying", name.c_str(), m == 1 ? "play normally, F6 to stop" : "touch the mouse/keys to take over, F7 to stop");
    worker = std::thread(loop);
}

// ---------- analysis HUD ----------
void renderHud() {
    double k = 1.0;
    if (tgt) { UINT d = GetDpiForWindow(tgt); if (d) k = d / 96.0 * 0.9; }
    int W = (int)(560 * k), H = (int)(520 * k);
    std::vector<uint32_t> bg((size_t)W * H, 0);
    Canvas cv{W, H, bg.data()};
    int m = mode;
    int ar = m == 1 ? 60 : m == 3 ? 255 : 255, ag = m == 1 ? 220 : m == 3 ? 200 : 120, ab = m == 1 ? 120 : m == 3 ? 60 : 60;
    float R = 14 * (float)k;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float dx = std::max(0.f, std::max(R - x, x - (W - 1 - R))), dy = std::max(0.f, std::max(R - y, y - (H - 1 - R)));
            float d = std::sqrt(dx * dx + dy * dy);
            if (d > R) continue;
            if (R - d < 2.5f * (float)k) cv.blend(x, y, ar, ag, ab, 0.95f); else cv.blend(x, y, 10, 14, 24, 0.88f);
        }
    double conf = confusion;
    int bx0 = (int)(130 * k), bx1 = W - (int)(20 * k), by0 = (int)(282 * k), by1 = by0 + (int)(8 * k);
    float fr = (float)std::min(1.0, conf / 3.0);
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            bool fill = x < bx0 + (bx1 - bx0) * fr;
            if (fill) cv.blend(x, y, 255, (int)(220 - 180 * fr), 60, 0.95f); else cv.blend(x, y, 50, 50, 70, 0.6f);
        }
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memset(bits, 0, (size_t)W * H * 4);
    HGDIOBJ oldB = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    auto font = [&](int px, int weight) { return CreateFontW(-(int)(px * k), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI"); };
    HFONT fT = font(19, FW_BOLD), fB = font(13, FW_SEMIBOLD), fS = font(12, FW_NORMAL), fL = font(11, FW_BOLD);
    auto text = [&](HFONT f, COLORREF c, int x, int y, const std::string &s) {
        SelectObject(dc, f); SetTextColor(dc, c);
        RECT r{(int)(x * k), (int)(y * k), W - (int)(14 * k), H};
        std::wstring w(s.size(), L' ');
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size()); w.resize(std::max(0, n));
        DrawTextW(dc, w.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    };
    COLORREF white = RGB(240, 244, 255), dim = RGB(150, 160, 185), lab = RGB(110, 150, 220), hot = RGB(ar, ag, ab);
    std::string g, ct, at, wt, pt; std::deque<std::string> ns; std::vector<std::string> fs;
    { std::lock_guard<std::mutex> lk(amu); g = goal; ct = confusedTxt; at = actionTxt; wt = watchTxt; pt = placeTxt; ns = notes; fs = findings; }
    std::string modeTxt = m == 1 ? "WATCHING YOU" : m == 3 ? "ASKING YOU" : takeover ? "YOU'RE DRIVING - RECORDING" : "PLAYING ON ITS OWN";
    text(fT, white, 18, 12, "COOKIEBOT COPYCAT");
    text(fB, hot, 220, 16, modeTxt);
    char b[256];
    snprintf(b, sizeof b, "game: %s   %d frames of you / %d of me   %d places", name.c_str(), demoN, selfN, (int)places.size());
    text(fS, dim, 18, 40, b);
    text(fL, lab, 18, 64, "SPEED");
    snprintf(b, sizeof b, "%.0f fps   decide %.1f ms   read numbers %.0f ms", fps, decideMs, ocrMs);
    text(fB, white, 130, 62, b);
    text(fL, lab, 18, 88, "WATCHING");
    text(fS, white, 130, 87, wt);
    text(fL, lab, 18, 108, "WHERE");
    text(fS, white, 130, 107, pt);
    text(fL, lab, 18, 128, "ANALYZING");
    int y = 127;
    if (fs.empty()) { text(fS, dim, 130, y, "not enough data yet"); y += 16; }
    for (auto &s : fs) { text(fS, white, 130, y, s); y += 16; if (y > 240) break; }
    text(fL, lab, 18, 250, "TRYING TO GET");
    text(fS, white, 130, 249, g);
    text(fL, lab, 18, 270, "DOING");
    text(fS, white, 130, 269, at);
    text(fL, lab, 18, 293, "CONFUSED");
    snprintf(b, sizeof b, "%s   (match %.0f%%)", ct.c_str(), matchPct);
    text(fS, white, 130, 293, b);
    text(fL, lab, 18, 318, "WRITING DOWN");
    y = 317;
    for (auto it = ns.rbegin(); it != ns.rend() && y < 440; ++it) { text(fS, dim, 130, y, *it); y += 16; }
    std::string pr; bool po;
    { std::lock_guard<std::mutex> lk(gPromptMu); pr = gPromptText; po = gPromptOpen; }
    if (m == 3) text(fB, hot, 18, 470, "What are you doing?  > " + pr + (std::fmod(now(), 1.0) < 0.55 ? "_" : " "));
    else text(fS, dim, 18, 474, m == 1 ? "F6 stop watching  -  F7 let it play" : "touch mouse/keys to take over  -  F7 stop  -  F6 just watch");
    (void)po;
    GdiFlush();
    const uint32_t *tp = (const uint32_t *)bits;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint32_t t = tp[i];
        int r = (t >> 16) & 255, gg = (t >> 8) & 255, bb = t & 255;
        int mx = std::max(r, std::max(gg, bb));
        if (!mx) continue;
        cv.blend((int)(i % W), (int)(i / W), r * 255 / mx, gg * 255 / mx, bb * 255 / mx, mx / 255.f);
    }
    SelectObject(dc, oldB);
    for (HFONT f : {fT, fB, fS, fL}) DeleteObject(f);
    DeleteObject(bmp); DeleteDC(dc);
    HMONITOR hm = MonitorFromWindow(tgt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi}; GetMonitorInfo(hm, &mi);
    RECT wa = mi.rcWork;
    int hx = wa.left + (gHudDX >= 0 ? gHudDX : (int)(26 * k)), hy = wa.bottom - H - (gHudDY >= 0 ? gHudDY : (int)(26 * k));
    hx = std::max((int)wa.left, std::min((int)wa.right - W, hx)); hy = std::max((int)wa.top, std::min((int)wa.bottom - H, hy));
    pushLayered(gHudWnd, hx, hy, W, H, bg.data(), gHudDrag);
}
bool hudWanted() { return mode && tgt && GetForegroundWindow() == tgt; }

void init() {
    std::thread(rawThread).detach();
    std::thread(ocrThread).detach();
}

}  // namespace cc
