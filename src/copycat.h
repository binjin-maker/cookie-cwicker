// ================= COPYCAT: watch you play any game, copy you, keep learning =================
// F6 = watch me (records you), F7 = copy me (plays, and keeps learning from its own runs).
namespace cc {

// eyes: 64x36 grey view + 8 colour tiles + a sharp 24x24 crosshair close-up (from the full-res screen)
const int FW = 64, FH = 36, FG = FW * FH, FC = FG + 24, CPW = 24, FD = ((FC + CPW * CPW + 15) / 16) * 16;
const int TW = 16, TH = 9, TN = TW * TH;  // tiny thumbnail for a fast first-pass search
const int K = 12, MAXSTAT = 32, MAXPLACE = 64;
const size_t MAXFRAMES = 40000;

struct Frame {
    uint8_t f[FD];
    uint8_t thumb[TN];
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
    int seen = 0, changes = 0, samples = 0, passiveUps = 0, activeUps = 0;
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

std::atomic<const char *> stepLoop{"-"}, stepOcr{"-"}, stepHud{"-"};
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
std::string goal = "-", confusedTxt = "-", actionTxt = "-", watchTxt = "-", placeTxt = "-", moverTxt = "-";
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
    if ((vk >= VK_F1 && vk <= VK_F24) || vk == VK_ESCAPE || vk == VK_LWIN || vk == VK_RWIN || vk > 255) return;
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
static uint8_t banned[32];
static int bannedBtn = 0;
static double keyChanged[256], btnChanged[3];
static bool neverPress(int vk) {
    return vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_ESCAPE ||
           (vk >= VK_F1 && vk <= VK_F24) || vk == VK_SNAPSHOT || vk == VK_SLEEP || vk == VK_PAUSE || (vk >= 0xA6 && vk <= 0xB7) || vk == VK_DELETE ||
           hasKey(banned, vk);
}
// presses at most ~6 times a second per key/button, never system keys
static void applyOut(const uint8_t *keys, int btn, bool force = false) {
    double t = now();
    for (int vk = 1; vk < 256; vk++) {
        bool want = hasKey(keys, vk) && !neverPress(vk), have = hasKey(heldOut, vk);
        if (want == have) continue;
        if (!force && want && t - keyChanged[vk] < 0.16) continue;
        if (!force && !want && t - keyChanged[vk] < 0.1) continue;
        sendKey(vk, want); keyChanged[vk] = t;
        if (want) heldOut[vk >> 3] |= 1 << (vk & 7); else heldOut[vk >> 3] &= ~(1 << (vk & 7));
    }
    btn &= ~bannedBtn;
    for (int i = 0; i < 3; i++) {
        int b = 1 << i;
        bool want = btn & b, have = btnOut & b;
        if (want == have) continue;
        if (!force && t - btnChanged[i] < (want ? 0.16 : 0.08)) continue;
        sendBtn(b, want); btnChanged[i] = t;
        if (want) btnOut |= b; else btnOut &= ~b;
    }
}
static void releaseAll() { uint8_t z[32] = {}; applyOut(z, 0, true); }

// ---------- seeing ----------
static bool targetRect(Rect &r) {
    RECT c; if (!tgt || !GetClientRect(tgt, &c)) return false;
    POINT p{0, 0}; ClientToScreen(tgt, &p);
    r = {p.x, p.y, p.x + c.right, p.y + c.bottom};
    return c.right > 40 && c.bottom > 40;
}
const int MW = 64, MH = 36;
static uint32_t mini[MW * MH];
static int16_t center[256];
static int centerRGB[3];
static int grayOf0(uint32_t p) { return (((p >> 16) & 255) * 3 + ((p >> 8) & 255) * 6 + (p & 255)) / 10; }
static bool capture(uint8_t *f, uint8_t *thumb, float &red) {
    Rect r; if (!targetRect(r)) return false;
    memset(f, 0, FD);
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
    // sharp close-up of the crosshair area, straight from the full-res screen
    {
        static HDC cm = nullptr; static uint32_t *cb = nullptr;
        HDC scr2 = GetDC(nullptr);
        if (!cm) {
            cm = CreateCompatibleDC(scr2);
            BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = CPW * 2; bi.bmiHeader.biHeight = -CPW * 2; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
            HBITMAP b2 = CreateDIBSection(cm, &bi, DIB_RGB_COLORS, (void **)&cb, nullptr, 0);
            SelectObject(cm, b2);
        }
        int side = std::max(48, std::min(r.x1 - r.x0, r.y1 - r.y0) / 4);
        int cx0 = (r.x0 + r.x1) / 2 - side / 2, cy0 = (r.y0 + r.y1) / 2 - side / 2;
        SetStretchBltMode(cm, HALFTONE); SetBrushOrgEx(cm, 0, 0, nullptr);
        StretchBlt(cm, 0, 0, CPW * 2, CPW * 2, scr2, cx0, cy0, side, side, SRCCOPY);
        GdiFlush();
        ReleaseDC(nullptr, scr2);
        for (int y = 0; y < CPW; y++)
            for (int x = 0; x < CPW; x++) {
                int g = 0;
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        uint32_t p = cb[(y * 2 + j) * CPW * 2 + x * 2 + i];
                        g += (((p >> 16) & 255) * 3 + ((p >> 8) & 255) * 6 + (p & 255)) / 10;
                    }
                f[FC + y * CPW + x] = (uint8_t)(g / 4);
            }
    }
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            int g = 0;
            for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) g += f[(y * 4 + j) * FW + x * 4 + i];
            thumb[y * TW + x] = (uint8_t)(g / 16);
        }
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            int rr = 0, gg = 0, bb = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) { uint32_t p = bits[(y * 4 + j) * SW + x * 4 + i]; rr += (p >> 16) & 255; gg += (p >> 8) & 255; bb += p & 255; }
            mini[y * MW + x] = ((uint32_t)(rr / 16) << 16) | ((uint32_t)(gg / 16) << 8) | (uint32_t)(bb / 16);
        }
    {
        int sum = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                int g = 0;
                for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) g += grayOf0(bits[(56 + y * 2 + j) * SW + 112 + x * 2 + i]);
                center[y * 16 + x] = (int16_t)(g / 4); sum += g / 4;
            }
        int mean = sum / 256;
        for (int i = 0; i < 256; i++) center[i] = (int16_t)(center[i] - mean);
        uint32_t c = bits[72 * SW + 128]; centerRGB[0] = (c >> 16) & 255; centerRGB[1] = (c >> 8) & 255; centerRGB[2] = c & 255;
    }
    return true;
}
static int grayOf(uint32_t p) { return (((p >> 16) & 255) * 3 + ((p >> 8) & 255) * 6 + (p & 255)) / 10; }

// ---------- moving things (NPCs, animals, players, vehicles) ----------
struct Mover { int x, y, w, h, n; int r, g, b; };
static std::vector<Mover> movers;
static int moverGood[24], moverBad[24];
static double lastMoverSeen[24];
static int colourBin(int r, int g, int b) {
    int mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    int light = mx < 70 ? 0 : mx > 180 ? 2 : 1;
    int hue;
    if (mx - mn < 25) hue = 6;  // grey
    else if (mx == r) hue = g > b ? (g > r * 0.6 ? 1 : 0) : 5;  // red / orange-yellow / magenta
    else if (mx == g) hue = b > r ? 3 : 2;                         // green / teal
    else hue = r > g ? 5 : 4;                                      // blue / purple
    return std::min(23, hue * 3 + light);
}
static std::string colourName(int bin) {
    static const char *hues[] = {"red", "orange/yellow", "green", "teal", "blue", "purple", "grey"};
    static const char *lights[] = {"dark ", "", "bright "};
    return std::string(lights[bin % 3]) + hues[std::min(6, bin / 3)];
}
static std::string shapeOf(const Mover &m) {
    double ar = (double)m.h / std::max(1, m.w);
    std::string sz = m.n > 120 ? "big " : m.n < 12 ? "small " : "";
    return sz + (ar > 1.6 ? "tall (person-like)" : ar < 0.6 ? "wide (animal/vehicle-like)" : "chunky");
}
static void findMovers(const uint32_t *a, const uint32_t *b, int sx, int sy) {
    // sx,sy is the whole-view shift on the 32x18 grid -> x2 here
    int dx = sx * MW / FW, dy = sy * MH / FH;
    static uint8_t mask[MW * MH];
    memset(mask, 0, sizeof mask);
    for (int y = 3; y < MH - 3; y++)
        for (int x = 3; x < MW - 3; x++) {
            int yy = y + dy, xx = x + dx;
            if (xx < 0 || yy < 0 || xx >= MW || yy >= MH) continue;
            if (std::abs(grayOf(a[y * MW + x]) - grayOf(b[yy * MW + xx])) > 45) mask[yy * MW + xx] = 1;
        }
    movers.clear();
    static int st[MW * MH];
    for (int i = 0; i < MW * MH; i++) {
        if (mask[i] != 1) continue;
        Mover m{i % MW, i / MW, 0, 0, 0, 0, 0, 0};
        int x0 = MW, y0 = MH, x1 = 0, y1 = 0, sp = 0;
        st[sp++] = i; mask[i] = 2;
        long rr = 0, gg = 0, bb = 0;
        while (sp) {
            int k = st[--sp], kx = k % MW, ky = k / MW;
            m.n++; x0 = std::min(x0, kx); x1 = std::max(x1, kx); y0 = std::min(y0, ky); y1 = std::max(y1, ky);
            uint32_t p = b[k]; rr += (p >> 16) & 255; gg += (p >> 8) & 255; bb += p & 255;
            const int nb[4] = {k - 1, k + 1, k - MW, k + MW};
            for (int q : nb) if (q >= 0 && q < MW * MH && std::abs(q % MW - kx) <= 1 && mask[q] == 1) { mask[q] = 2; st[sp++] = q; }
        }
        if (m.n < 4 || m.n > MW * MH / 4) continue;  // noise, or the whole view changing
        m.x = (x0 + x1) / 2; m.y = (y0 + y1) / 2; m.w = x1 - x0 + 1; m.h = y1 - y0 + 1;
        m.r = (int)(rr / m.n); m.g = (int)(gg / m.n); m.b = (int)(bb / m.n);
        movers.push_back(m);
    }
    static float movePres[48];
    for (auto &v : movePres) v *= 0.98f;
    std::vector<Mover> kept;
    for (auto &m : movers) {
        int cell = std::min(5, m.y * 6 / MH) * 8 + std::min(7, m.x * 8 / MW);
        movePres[cell] = std::min(1.f, movePres[cell] + 0.02f);
        if (movePres[cell] > 0.6f && m.y > MH / 2) continue;  // always moving down there = your own gun/arms
        kept.push_back(m);
    }
    movers.swap(kept);
    std::sort(movers.begin(), movers.end(), [](const Mover &p, const Mover &q) { return p.n > q.n; });
    if (movers.size() > 6) movers.resize(6);
    double t = now();
    for (auto &m : movers) lastMoverSeen[colourBin(m.r, m.g, m.b)] = t;
}
static bool hostileBin(int bin) { return moverBad[bin] >= 3 && moverBad[bin] > moverGood[bin] * 2; }

// ---------- tools / hotbar ----------
struct Tool { float sig[MW * 6]; int n = 0; std::string name; int up[MAXSTAT] = {}, uses = 0; };
static Tool tools[10];
static int curTool = -1;
static void hotbarSig(float *out) {
    for (int y = 0; y < 6; y++) for (int x = 0; x < MW; x++) out[y * MW + x] = (float)grayOf(mini[(MH - 6 + y) * MW + x]);
}
static int whichTool() {
    float s[MW * 6]; hotbarSig(s);
    int best = -1; double bd = 1e18;
    for (int i = 1; i <= 9; i++) {
        if (tools[i].n < 2) continue;
        double d = 0; for (int k = 0; k < MW * 6; k++) d += std::fabs(tools[i].sig[k] - s[k]);
        if (d < bd) { bd = d; best = i; }
    }
    double second = 1e18;
    for (int i = 1; i <= 9; i++) {
        if (i == best || tools[i].n < 2) continue;
        double d = 0; for (int k = 0; k < MW * 6; k++) d += std::fabs(tools[i].sig[k] - s[k]);
        second = std::min(second, d);
    }
    return (best > 0 && (second == 1e18 || bd < second * 0.85)) ? best : -1;
}
// things you looked at (trees, rocks, crates...) learned from what happened when you hit them, or from what you told it
struct Look { int16_t p[256]; int rgb[3]; std::string label; int n = 1; int stat = -1; };
static std::vector<Look> looks;
static int lookingAt = -1;
static int lookDist(const Look &l) {
    int d = 0; for (int i = 0; i < 256; i++) d += std::abs(l.p[i] - center[i]);
    for (int c = 0; c < 3; c++) d += std::abs(l.rgb[c] - centerRGB[c]) * 8;
    return d;
}
static void learnLook(const std::string &label, int stat) {
    int best = -1, bd = INT_MAX;
    for (size_t i = 0; i < looks.size(); i++) if (looks[i].label == label) { int d = lookDist(looks[i]); if (d < bd) { bd = d; best = (int)i; } }
    if (best >= 0 && bd < 256 * 14) { Look &l = looks[best]; l.n++; for (int i = 0; i < 256; i++) l.p[i] = (int16_t)(l.p[i] + (center[i] - l.p[i]) / std::min(l.n, 8)); return; }
    if (looks.size() >= 300) return;
    Look l; memcpy(l.p, center, sizeof l.p); for (int c = 0; c < 3; c++) l.rgb[c] = centerRGB[c]; l.label = label; l.stat = stat;
    looks.push_back(l);
}
static int recognizeLook() {
    int best = -1, bd = INT_MAX;
    for (size_t i = 0; i < looks.size(); i++) { int d = lookDist(looks[i]); if (d < bd) { bd = d; best = (int)i; } }
    return bd < 256 * 11 ? best : -1;
}
static int classGood[80], classBad[80];
static double classSeen[80];
static bool firstSeenNoted[80];
// what you told it beats what it worked out: -1 enemy, 1 friendly, 2 yours (your own gun/arms/HUD)
static int userClass[80];
static std::string classAlias[80];
static bool hostileClass(int c) { return userClass[c] == -1 || (userClass[c] == 0 && classBad[c] >= 3 && classBad[c] > classGood[c] * 2); }
static bool friendlyClass(int c) { return userClass[c] == 1 || (userClass[c] == 0 && classGood[c] >= 3 && classGood[c] > classBad[c] * 2); }
static float sigEnemy = 0, sigResource = 0, sigNeed = 0, sigGood = 0, sigBad = 0;
static std::string className(int c) { return classAlias[c].empty() ? det::COCO[c] : classAlias[c]; }
// things that sit in the same spot at the bottom of the screen most of the time (your gun / arms) are yours, not enemies
static float presence[80][48];
static void markOwn(std::vector<det::Box> &bs, const Rect &r) {
    int W = std::max(1, r.x1 - r.x0), H = std::max(1, r.y1 - r.y0);
    for (auto &row : presence) for (auto &v : row) v *= 0.985f;
    for (auto &b : bs) {
        int cx = ((b.x0 + b.x1) / 2 - r.x0) * 8 / W, cy = ((b.y0 + b.y1) / 2 - r.y0) * 6 / H;
        int cell = std::max(0, std::min(5, cy)) * 8 + std::max(0, std::min(7, cx));
        float &p = presence[b.cls][cell];
        p = std::min(1.f, p + 0.015f);
        bool bottom = b.y1 > r.y0 + H * 0.85;
        b.own = userClass[b.cls] == 2 || (bottom && p > 0.5f);
    }
}
static double lastHitAt = 0;

static std::string toolName(int i) { return i <= 0 ? "nothing known" : !tools[i].name.empty() ? tools[i].name : "slot " + std::to_string(i); }
static int dist(const uint8_t *a, const uint8_t *b) {
    __m128i acc = _mm_setzero_si128();
    for (int i = 0; i < FD; i += 16)
        acc = _mm_add_epi64(acc, _mm_sad_epu8(_mm_loadu_si128((const __m128i *)(a + i)), _mm_loadu_si128((const __m128i *)(b + i))));
    return (int)(_mm_cvtsi128_si64(acc) + _mm_cvtsi128_si64(_mm_unpackhi_epi64(acc, acc)));
}
static int distThumb(const uint8_t *a, const uint8_t *b) {
    __m128i acc = _mm_setzero_si128();
    for (int i = 0; i < TN; i += 16)
        acc = _mm_add_epi64(acc, _mm_sad_epu8(_mm_loadu_si128((const __m128i *)(a + i)), _mm_loadu_si128((const __m128i *)(b + i))));
    return (int)(_mm_cvtsi128_si64(acc) + _mm_cvtsi128_si64(_mm_unpackhi_epi64(acc, acc)));
}
static void shiftOf(const uint8_t *a, const uint8_t *b, int &sx, int &sy, double &change) {
    int best = INT_MAX; sx = sy = 0;
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -6; dx <= 6; dx++) {
            int s = 0, n = 0;
            for (int y = 5; y < FH - 5; y += 2)
                for (int x = 7; x < FW - 7; x++) { s += std::abs((int)a[y * FW + x] - (int)b[(y + dy) * FW + x + dx]); n++; }
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
static std::map<std::string, float> statOverride;  // what you told it a number is worth (0 = ignore)
static float weightFor(const std::string &l, bool &needLike) {
    needLike = false;
    auto ov = statOverride.find(l);
    if (ov != statOverride.end()) return ov->second;
    for (const char *w : {"wet", "comfort", "temperature", "temp", "weight", "fps", "ping"}) if (l.find(w) != std::string::npos) return 0;
    for (const char *w : {"health", "hp", "life", "lives", "shield", "armor", "armour"}) if (l.find(w) != std::string::npos) { needLike = true; return 3; }
    for (const char *w : {"food", "hunger", "water", "thirst", "stamina", "energy", "calories", "hydration"}) if (l.find(w) != std::string::npos) { needLike = true; return 2; }
    for (const char *w : {"radiation", "rad", "cold", "heat", "poison", "bleed", "wanted"}) if (l.find(w) != std::string::npos) return -2;
    for (const char *w : {"ammo", "time", "fps", "ping", "ms", "x", "y", "z", "day", "clock"}) if (l == w || l.find(w) == 0) return 0;
    for (const char *w : {"score", "kill", "coin", "gold", "xp", "point", "money", "scrap", "wood", "stone", "metal", "ore", "cloth", "sulfur", "fuel", "loot"})
        if (l.find(w) != std::string::npos) return 2;
    return 1;
}
struct Used { size_t idx; size_t gen; double t; };
static std::deque<Used> usedMem;  // remembered moments it copied recently
static size_t eraseGenR = 0;
static double lastRewardAt = 0;
static void reward(float r, int statIdx, bool up, const char *why) {
    double t = now();
    std::lock_guard<std::mutex> lk(mu);
    if (r != 0) lastRewardAt = t;
    for (int i = (int)data.size() - 1; i >= 0 && t - data[i].t < 5.0; i--) {
        data[i].value += r * (float)std::exp(-(t - data[i].t) / 2.5);
        if (statIdx >= 0 && statIdx < 32) { if (up) data[i].up |= 1u << statIdx; else data[i].down |= 1u << statIdx; }
    }
    // the memories it chose to copy get the credit/blame too, so next time it picks better ones
    for (auto &u : usedMem)
        if (u.gen == eraseGenR && u.idx < data.size() && t - u.t < 5.0) {
            data[u.idx].value += r * 0.8f * (float)std::exp(-(t - u.t) / 2.5);
            if (statIdx >= 0 && statIdx < 32) { if (up) data[u.idx].up |= 1u << statIdx; else data[u.idx].down |= 1u << statIdx; }
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
        for (int bin = 0; bin < 24; bin++) if (t - lastMoverSeen[bin] < 1.5) { if (r < 0) moverBad[bin]++; else if (r > 0) moverGood[bin]++; }
        for (int c = 0; c < 80; c++) if (t - classSeen[c] < 1.5) { if (r < 0) classBad[c]++; else if (r > 0) classGood[c]++; }
        if (r > 0 && up && statIdx >= 0 && statIdx < (int)stats.size() && t - lastHitAt < 1.2) {
            learnLook("gives " + stats[statIdx].label, statIdx);
        }
        if (curTool > 0 && r > 0) { tools[curTool].uses++; if (statIdx >= 0 && statIdx < MAXSTAT && up) tools[curTool].up[statIdx]++; }
        note("%s %s at Place %d%s%s", r > 0 ? "GOOD:" : r < 0 ? "BAD:" : "", why, pl + 1, held.empty() ? "" : " while pressing ", held.c_str());
    }
}

static void ocrThread() {
    RoInitialize(RO_INIT_MULTITHREADED);
    while (!gQuit) {
        stepOcr = "idle";
        Sleep(120);
        if (!mode || mode == 3 || GetForegroundWindow() != tgt) continue;
        Rect r; if (!targetRect(r)) continue;
        double t0 = now();
        double sc = std::min(1.0, 1800.0 / std::max(1, r.x1 - r.x0));
        stepOcr = "reading the screen";
        auto words = ocrRect(r, sc);
        ocrMs = (now() - t0) * 1000;
        stepOcr = "parsing numbers";
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
                bool idleNow = false;
                {
                    // was anything pressed / moved in the last 2 s?
                    int act = 0, n = 0; double tn = now();
                    for (int i = (int)data.size() - 1; i >= 0 && tn - data[i].t < 2.0; i--, n++) {
                        bool any = data[i].btn || std::abs(data[i].dx) + std::abs(data[i].dy) > 2;
                        for (int b = 0; b < 32 && !any; b++) if (data[i].keys[b]) any = true;
                        if (any) act++;
                    }
                    idleNow = n > 10 && act < n / 10;
                }
                if (!clock && s.weight > 0 && d > 0) {
                    if (idleNow) s.passiveUps++; else s.activeUps++;
                    if (s.passiveUps >= 5 && s.passiveUps > s.activeUps * 2) {
                        s.weight = 0;
                        statOverride[s.label] = 0;
                        note("%s goes up by itself while nothing's happening - ignoring it from now on", s.label.c_str());
                    }
                }
                if (!clock && s.weight != 0 && s.seen > 2) {
                    float r2 = (d > 0 ? 1.f : -1.f) * s.weight;
                    if (idleNow && r2 > 0) r2 *= 0.2f;  // don't learn "sit still = good"
                    char why[128]; snprintf(why, sizeof why, "%s %s -> %s", s.label.c_str(), fmtNum(s.last).c_str(), fmtNum(v).c_str());
                    pend.push_back({r2, idx, d > 0, why});
                }
                s.last = v; s.maxSeen = std::max(s.maxSeen, v);
            }
        }
        for (auto &s : stats) if (s.seen > 2 && s.weight != 0) watch += s.label + " " + fmtNum(s.last) + "  ";
        statLock.unlock();
        stepOcr = "giving rewards";
        for (auto &p : pend) reward(p.r, p.idx, p.up, p.why.c_str());
        std::lock_guard<std::mutex> lk(amu);
        watchTxt = watch.empty() ? "no numbers on screen yet" : watch;
    }
}

// ---------- object detector thread ----------
std::string seesTxt = "-";
static void detThread() {
    RoInitialize(RO_INIT_MULTITHREADED);
    double lastTry = -100, lastSec = now(); int n = 0;
    std::vector<det::Box> bs;
    while (!gQuit) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (!det::ready) {
            if (now() - lastTry > 10) { lastTry = now(); if (det::load()) logf("object detector ready (%s)", det::status.c_str()); }
            std::lock_guard<std::mutex> lk(amu); seesTxt = "detector: " + det::status;
            Sleep(200); continue;
        }
        Rect r;
        if (!mode || mode == 3 || GetForegroundWindow() != tgt || !targetRect(r)) { det::hide(); Sleep(50); continue; }
        { std::vector<det::Box> raw;
          if (!det::run(r, raw)) { det::hide(); Sleep(200); continue; }
          det::track(raw, bs); }
        n++;
        if (now() - lastSec >= 1) { det::rate = n / (now() - lastSec); n = 0; lastSec = now(); }
        std::vector<std::string> labels; std::vector<int> kind;
        std::string txt;
        double t = now();
        int cxs = (r.x0 + r.x1) / 2, cys = (r.y0 + r.y1) / 2;
        {
            std::lock_guard<std::mutex> lk(mu);
            markOwn(bs, r);
            for (auto &b : bs) {
                if (b.own) {
                    labels.push_back(" " + className(b.cls) + " (yours) "); kind.push_back(2);
                    continue;
                }
                classSeen[b.cls] = t;
                if (!firstSeenNoted[b.cls]) { firstSeenNoted[b.cls] = true; note("first time seeing a %s", className(b.cls).c_str()); }
                int k = hostileClass(b.cls) ? -1 : friendlyClass(b.cls) ? 1 : 0;
                char l[96];
                snprintf(l, sizeof l, " %s %d%%%s ", className(b.cls).c_str(), (int)(b.conf * 100), k < 0 ? (userClass[b.cls] == -1 ? " ENEMY" : " ENEMY?") : k > 0 ? " friendly" : "");
                labels.push_back(l); kind.push_back(k);
                if (txt.size() < 90) {
                    int bx = (b.x0 + b.x1) / 2;
                    bool mid = cxs > b.x0 && cxs < b.x1 && cys > b.y0 && cys < b.y1;
                    txt += std::string(txt.empty() ? "" : ", ") + className(b.cls) + (mid ? " (ahead)" : bx < cxs ? " (left)" : " (right)") + (k < 0 ? " [ENEMY]" : "");
                }
            }
        }
        if (cfgFx) det::draw(r, bs, labels, kind); else det::hide();
        { std::lock_guard<std::mutex> lk(det::mu); det::boxes = bs; det::boxesAt = t; }
        char hdr[96]; snprintf(hdr, sizeof hdr, "   [%s %.0f ms, %.0f/s]", det::onGpu ? "GPU" : "CPU", det::ms, det::rate);
        std::lock_guard<std::mutex> lk(amu);
        seesTxt = (txt.empty() ? std::string("nothing it knows") : txt) + hdr;
    }
    det::hide();
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
        else if (ch > base * 1.6) what = std::abs(sx) > 0.8 ? (sx > 0 ? "moves/turns you (view slides right)" : "moves/turns you (view slides left)") : "moves you or changes the view a lot";
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
    for (int c = 0; c < 80 && out.size() < 10; c++) {
        if (userClass[c] == 2) continue;
        if (userClass[c] == -1) out.push_back(className(c) + "s are enemies (you said so)");
        else if (userClass[c] == 1) out.push_back(className(c) + "s are friendly (you said so)");
        else if (hostileClass(c)) out.push_back(className(c) + "s show up before bad stuff - probably enemies (" + std::to_string(classBad[c]) + "x)");
        else if (friendlyClass(c)) out.push_back(className(c) + "s come with good stuff");
    }
    for (size_t i = 0; i < looks.size() && out.size() < 10; i++) if (looks[i].n >= 3 && looks[i].label.rfind("gives ", 0) == 0) out.push_back("learned what things that " + looks[i].label + " look like (" + std::to_string(looks[i].n) + " hits)");
    for (int bin = 0; bin < 24 && out.size() < 10; bin++) {
        if (hostileBin(bin)) out.push_back(colourName(bin) + " moving things show up before bad stuff - probably enemies (" + std::to_string(moverBad[bin]) + "x)");
        else if (moverGood[bin] >= 3 && moverGood[bin] > moverBad[bin] * 2) out.push_back(colourName(bin) + " moving things come with good stuff (animals to hunt? loot?)");
    }
    for (int i = 1; i <= 9 && out.size() < 12; i++) {
        if (tools[i].n < 2) continue;
        int bs = -1, bn = 0;
        for (size_t s2 = 0; s2 < stats.size() && s2 < MAXSTAT; s2++) if (tools[i].up[s2] > bn) { bn = tools[i].up[s2]; bs = (int)s2; }
        if (bs >= 0) out.push_back(toolName(i) + " gets " + stats[bs].label + " (" + std::to_string(bn) + "x)");
    }
    for (size_t s = 0; s < stats.size() && out.size() < 12; s++) {
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
        uint32_t magic = 0x32424343, n = (uint32_t)data.size(), np = (uint32_t)places.size(), nm = (uint32_t)mems.size();
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
    if (fread(&magic, 4, 1, f) == 1 && magic == 0x31424343) {
        fclose(f);
        std::string old = p + ".old";
        MoveFileExA(p.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        logf("Copycat: your old recording was made with the small eyes - saved it as %s, starting a fresh one", old.c_str());
        return;
    }
    if (magic == 0x32424343 && fread(&n, 4, 1, f) == 1 && n <= MAXFRAMES * 2) {
        data.resize(n);
        if (n && fread(data.data(), sizeof(Frame), n, f) != n) data.clear();
        double base = now() - 1e6;
        for (auto &fr : data) fr.t += base;
        size_t before = data.size();
        data.erase(std::remove_if(data.begin(), data.end(), [](const Frame &fr) {
            for (int vk = VK_F1; vk <= VK_F24; vk++) if (hasKey(fr.keys, vk)) return true;
            return false;
        }), data.end());
        if (data.size() != before) logf("Copycat: removed %d frames recorded while F10 was stuck", (int)(before - data.size()));
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

static std::atomic<bool> telling{false};
static std::atomic<int> resumeMode{2};
static size_t eraseGen = 0;
static int vkFromWord(const std::string &w) {
    if (w.size() == 1 && isalnum((unsigned char)w[0])) return toupper(w[0]);
    if (w == "space" || w == "spacebar") return VK_SPACE;
    if (w == "shift") return VK_SHIFT;
    if (w == "ctrl" || w == "control") return VK_CONTROL;
    if (w == "tab") return VK_TAB;
    if (w == "enter") return VK_RETURN;
    if (w == "click" || w == "lmb" || w == "shoot" || w == "fire" || w == "mouse") return 1001;
    if (w == "rmb" || w == "aim") return 1002;
    return 0;
}
static void saveRules() {
    CreateDirectoryA("copycat", nullptr);
    std::string p = "copycat\\" + name + ".rules";
    if (FILE *f = fopen(p.c_str(), "w")) {
        for (int vk = 1; vk < 256; vk++) if (hasKey(banned, vk)) fprintf(f, "ban %d\n", vk);
        if (bannedBtn) fprintf(f, "banbtn %d\n", bannedBtn);
        for (size_t i = 0; i < places.size(); i++) if (places[i].look.rfind("\"", 0) == 0) fprintf(f, "place %d %s\n", (int)i, places[i].look.c_str());
        for (int i = 1; i <= 9; i++) if (!tools[i].name.empty()) fprintf(f, "toolname %d %s\n", i, tools[i].name.c_str());
        for (int b = 0; b < 24; b++) if (moverGood[b] || moverBad[b]) fprintf(f, "mover %d %d %d\n", b, moverGood[b], moverBad[b]);
        for (int c = 0; c < 80; c++) if (classGood[c] || classBad[c]) fprintf(f, "class %d %d %d\n", c, classGood[c], classBad[c]);
        for (int c = 0; c < 80; c++) if (userClass[c]) fprintf(f, "userclass %d %d\n", c, userClass[c]);
        for (auto &kv : statOverride) fprintf(f, "statweight %g %s\n", kv.second, kv.first.c_str());
        for (int c = 0; c < 80; c++) if (!classAlias[c].empty()) fprintf(f, "alias %d %s\n", c, classAlias[c].c_str());
        fclose(f);
    }
    p = "copycat\\" + name + ".looks";
    if (FILE *f = fopen(p.c_str(), "wb")) {
        for (auto &l : looks) {
            fwrite(l.p, sizeof l.p, 1, f); fwrite(l.rgb, sizeof l.rgb, 1, f); fwrite(&l.n, 4, 1, f); fwrite(&l.stat, 4, 1, f);
            uint32_t L = (uint32_t)l.label.size(); fwrite(&L, 4, 1, f); fwrite(l.label.data(), 1, L, f);
        }
        fclose(f);
    }
    p = "copycat\\" + name + ".tools";
    if (FILE *f = fopen(p.c_str(), "wb")) {
        for (int i = 0; i < 10; i++) { fwrite(tools[i].sig, sizeof tools[i].sig, 1, f); fwrite(&tools[i].n, 4, 1, f); fwrite(tools[i].up, sizeof tools[i].up, 1, f); fwrite(&tools[i].uses, 4, 1, f); }
        fclose(f);
    }
}
static void loadRules() {
    memset(banned, 0, sizeof banned); bannedBtn = 0;
    memset(moverGood, 0, sizeof moverGood); memset(moverBad, 0, sizeof moverBad);
    memset(classGood, 0, sizeof classGood); memset(classBad, 0, sizeof classBad);
    memset(userClass, 0, sizeof userClass); for (auto &a : classAlias) a.clear();
    statOverride.clear();
    looks.clear();
    {
        std::string lp = "copycat\\" + name + ".looks";
        if (FILE *lf = fopen(lp.c_str(), "rb")) {
            for (;;) {
                Look l; uint32_t L = 0;
                if (fread(l.p, sizeof l.p, 1, lf) != 1 || fread(l.rgb, sizeof l.rgb, 1, lf) != 1 || fread(&l.n, 4, 1, lf) != 1 || fread(&l.stat, 4, 1, lf) != 1 ||
                    fread(&L, 4, 1, lf) != 1 || L > 200) break;
                l.label.resize(L); if (L && fread(&l.label[0], 1, L, lf) != L) break;
                looks.push_back(l);
                if (looks.size() >= 300) break;
            }
            fclose(lf);
        }
    }
    for (auto &tl : tools) tl = Tool{};
    std::string p = "copycat\\" + name + ".tools";
    if (FILE *tf = fopen(p.c_str(), "rb")) {
        for (int i = 0; i < 10; i++)
            if (fread(tools[i].sig, sizeof tools[i].sig, 1, tf) != 1 || fread(&tools[i].n, 4, 1, tf) != 1 || fread(tools[i].up, sizeof tools[i].up, 1, tf) != 1 ||
                fread(&tools[i].uses, 4, 1, tf) != 1) { tools[i] = Tool{}; break; }
        fclose(tf);
    }
    p = "copycat\\" + name + ".rules";
    FILE *f = fopen(p.c_str(), "r");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof line, f)) {
        int v; char nm[260];
        if (sscanf(line, "ban %d", &v) == 1 && v > 0 && v < 256) banned[v >> 3] |= 1 << (v & 7);
        else if (sscanf(line, "banbtn %d", &v) == 1) bannedBtn = v;
        else if (sscanf(line, "place %d %259[^\n]", &v, nm) == 2 && v >= 0 && v < (int)places.size()) places[v].look = nm;
        else if (sscanf(line, "toolname %d %259[^\n]", &v, nm) == 2 && v >= 1 && v <= 9) tools[v].name = nm;
        else {
            int g2, b2;
            if (sscanf(line, "mover %d %d %d", &v, &g2, &b2) == 3 && v >= 0 && v < 24) { moverGood[v] = g2; moverBad[v] = b2; }
            else if (sscanf(line, "class %d %d %d", &v, &g2, &b2) == 3 && v >= 0 && v < 80) { classGood[v] = g2; classBad[v] = b2; }
            else if (sscanf(line, "userclass %d %d", &v, &g2) == 2 && v >= 0 && v < 80) userClass[v] = g2;
            else if (sscanf(line, "alias %d %259[^\n]", &v, nm) == 2 && v >= 0 && v < 80) classAlias[v] = nm;
            else { float wv; if (sscanf(line, "statweight %f %259[^\n]", &wv, nm) == 2) statOverride[nm] = wv; }
        }
    }
    fclose(f);
}
// "don't press e", "this is the base", "this is dangerous" ... returns true if it understood something
static bool explain(const std::string &raw, int place) {
    std::string t = lower(raw);
    for (auto &c : t) if (!isalnum((unsigned char)c) && c != ' ') c = ' ';
    auto toks = split(t);
    bool did = false, good = false, bad = false;
    static const char *banV[] = {"press", "pressing", "use", "using", "click", "clicking", "hit", "hitting", "touch", "push"};
    bool neg = t.find("don t") != std::string::npos || t.find("dont") != std::string::npos || t.find("never") != std::string::npos ||
               t.find("stop") != std::string::npos || t.find("no more") != std::string::npos;
    bool pos = t.find("you can") != std::string::npos || t.find("ok to") != std::string::npos || t.find("allowed") != std::string::npos;
    for (size_t i = 0; i + 1 < toks.size(); i++)
        for (auto v : banV)
            if (toks[i] == v) {
                int vk = vkFromWord(toks[i + 1]);
                if (!vk && toks[i + 1] == "left" && i + 2 < toks.size() && toks[i + 2] == "click") vk = 1001;
                if (!vk && toks[i + 1] == "right" && i + 2 < toks.size() && toks[i + 2] == "click") vk = 1002;
                if (!vk) continue;
                if (neg) { if (vk >= 1001) bannedBtn |= vk == 1001 ? 1 : 2; else banned[vk >> 3] |= 1 << (vk & 7); note("rule: never press %s", keyName(vk).c_str()); did = true; }
                else if (pos) { if (vk >= 1001) bannedBtn &= ~(vk == 1001 ? 1 : 2); else banned[vk >> 3] &= ~(1 << (vk & 7)); note("rule: allowed to press %s again", keyName(vk).c_str()); did = true; }
            }
    // "3 is the hatchet", "slot 2 is a pickaxe"
    for (size_t i = 0; i + 2 < toks.size(); i++) {
        size_t j = i;
        if (toks[j] == "slot" && j + 1 < toks.size()) j++;
        if (toks[j].size() == 1 && toks[j][0] >= '1' && toks[j][0] <= '9' && toks[j + 1] == "is") {
            size_t k = j + 2;
            if (k < toks.size() && (toks[k] == "a" || toks[k] == "the" || toks[k] == "my" || toks[k] == "an")) k++;
            std::string nm;
            for (; k < toks.size(); k++) nm += (nm.empty() ? "" : " ") + toks[k];
            if (!nm.empty()) { tools[toks[j][0] - '0'].name = nm; note("tool %c is the %s", toks[j][0], nm.c_str()); did = true; break; }
        }
    }
    {
        static const char *cw[][2] = {{"red", "0"}, {"orange", "1"}, {"yellow", "1"}, {"green", "2"}, {"teal", "3"}, {"cyan", "3"}, {"blue", "4"},
                                      {"purple", "5"}, {"pink", "5"}, {"grey", "6"}, {"gray", "6"}, {"white", "6"}, {"black", "6"}};
        bool enemy = t.find("enem") != std::string::npos || t.find("hostile") != std::string::npos || t.find("bad guy") != std::string::npos ||
                     t.find("shoot") != std::string::npos || t.find("attack") != std::string::npos || t.find("danger") != std::string::npos;
        bool friendly = t.find("friend") != std::string::npos || t.find("ally") != std::string::npos || t.find("teammate") != std::string::npos ||
                        t.find("safe") != std::string::npos || t.find("animal") != std::string::npos;
        if (enemy != friendly)
            for (auto &c : cw)
                if (t.find(c[0]) != std::string::npos) {
                    int h = c[1][0] - '0';
                    for (int l = 0; l < 3; l++) { if (enemy) moverBad[h * 3 + l] += 10; else moverGood[h * 3 + l] += 10; }
                    note("%s moving things are %s", c[0], enemy ? "ENEMIES" : "friendly");
                    did = true; good = bad = false;
                }
    }
    {
        static const char *cw[][2] = {{"person", "0"}, {"people", "0"}, {"player", "0"}, {"guy", "0"}, {"npc", "0"}, {"scientist", "0"}, {"human", "0"},
                                      {"bandit", "0"}, {"soldier", "0"}, {"zombie", "0"}, {"bear", "21"}, {"wolf", "16"}, {"wolves", "16"}, {"dog", "16"},
                                      {"deer", "17"}, {"horse", "17"}, {"cow", "19"}, {"boar", "18"}, {"pig", "18"}, {"sheep", "18"}, {"bird", "14"},
                                      {"chicken", "14"}, {"car", "2"}, {"truck", "7"}, {"boat", "8"}, {"heli", "4"}, {"plane", "4"}, {"bike", "3"}};
        bool enemy = t.find("enem") != std::string::npos || t.find("hostile") != std::string::npos || t.find("danger") != std::string::npos ||
                     t.find("kill") != std::string::npos || t.find("attack") != std::string::npos || t.find("shoot") != std::string::npos;
        bool friendly = t.find("friend") != std::string::npos || t.find("ally") != std::string::npos || t.find("safe") != std::string::npos ||
                        t.find("food") != std::string::npos || t.find("hunt") != std::string::npos || t.find("ride") != std::string::npos;
        if (enemy != friendly)
            for (auto &c : cw)
                if ((" " + t).find(std::string(" ") + c[0]) != std::string::npos) {
                    int k = atoi(c[1]);
                    userClass[k] = enemy ? -1 : 1;
                    if (enemy) classBad[k] += 10; else classGood[k] += 10;
                    note("%ss are %s (your call - it'll stick)", det::COCO[k], enemy ? "ENEMIES" : "good / friendly");
                    did = true; break;
                }
    }
    // "wet doesn't matter" / "ignore wet" / "wet is bad" / "radiation is bad"
    {
        bool nothing = t.find("doesn t matter") != std::string::npos || t.find("dont matter") != std::string::npos || t.find("don t matter") != std::string::npos ||
                       t.find("doesnt matter") != std::string::npos || t.find("ignore") != std::string::npos || t.find("means nothing") != std::string::npos ||
                       t.find("useless") != std::string::npos || t.find("not important") != std::string::npos || t.find("who cares") != std::string::npos ||
                       t.find("pointless") != std::string::npos;
        bool isBad = t.find(" is bad") != std::string::npos || t.find(" are bad") != std::string::npos || t.find("lower is better") != std::string::npos;
        bool isGood = t.find(" is good") != std::string::npos || t.find(" are good") != std::string::npos || t.find("higher is better") != std::string::npos;
        std::vector<std::string> hit;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto &st : stats)
                if (st.label.size() >= 2 && (" " + t + " ").find(" " + st.label) != std::string::npos) {
                    float w = nothing ? 0.f : isBad ? -2.f : isGood ? 2.f : st.weight;
                    if (w == st.weight && !nothing && !isBad && !isGood) continue;
                    st.weight = w; statOverride[st.label] = w;
                    hit.push_back(st.label);
                }
            if (hit.empty() && (nothing || isBad || isGood))
                for (auto &tk : toks)
                    if (tk.size() >= 3 && tk != "doesn" && tk != "matter" && tk != "ignore" && tk != "the" && tk != "meter" && tk != "bar" && tk != "bad" && tk != "good") {
                        statOverride[tk] = nothing ? 0.f : isBad ? -2.f : 2.f;
                        hit.push_back(tk); break;
                    }
        }
        for (auto &h : hit) note("%s: %s", h.c_str(), nothing ? "doesn't matter - ignoring it" : isBad ? "going up is BAD" : "going up is good");
        if (!hit.empty()) did = true;
    }
    // "that's my gun" / "airplane is my gun" / "airplanes are guns"
    {
        bool mineWord = t.find("my gun") != std::string::npos || t.find("my weapon") != std::string::npos || t.find("my rifle") != std::string::npos ||
                        t.find("my pistol") != std::string::npos || t.find("my arm") != std::string::npos || t.find("my hand") != std::string::npos ||
                        t.find("my tool") != std::string::npos || t.find("s mine") != std::string::npos || t.find("is mine") != std::string::npos ||
                        t.find("my own") != std::string::npos || t.find("its me") != std::string::npos || t.find("that s me") != std::string::npos;
        int named = -1;
        for (int c = 0; c < 80; c++) {
            std::string n = det::COCO[c];
            if ((" " + t + " ").find(" " + n + " ") != std::string::npos || (" " + t + " ").find(" " + n + "s ") != std::string::npos) { named = c; break; }
        }
        std::string aliasTo;
        for (const char *ph : {" are ", " is a ", " is an ", " is my ", " is "}) {
            size_t k = t.find(ph);
            if (named >= 0 && k != std::string::npos) {
                aliasTo = t.substr(k + strlen(ph));
                for (const char *dropw : {"my ", "a ", "an ", "the "}) if (aliasTo.rfind(dropw, 0) == 0) aliasTo = aliasTo.substr(strlen(dropw));
                while (!aliasTo.empty() && aliasTo.back() == ' ') aliasTo.pop_back();
                if (!aliasTo.empty() && aliasTo.back() == 's' && aliasTo.size() > 3) aliasTo.pop_back();
                break;
            }
        }
        bool gunish = aliasTo.find("gun") != std::string::npos || aliasTo.find("weapon") != std::string::npos || aliasTo.find("rifle") != std::string::npos ||
                      aliasTo.find("pistol") != std::string::npos || aliasTo.find("arm") != std::string::npos || aliasTo.find("hand") != std::string::npos ||
                      aliasTo.find("tool") != std::string::npos;
        if (named >= 0 && (mineWord || gunish)) {
            userClass[named] = 2; classBad[named] = classGood[named] = 0;
            if (!aliasTo.empty()) classAlias[named] = aliasTo;
            note("%s = %s, it's yours - won't count it as good or bad", det::COCO[named], className(named).c_str());
            did = true;
        } else if (mineWord && named < 0) {
            std::lock_guard<std::mutex> dl(det::mu);
            Rect r; std::string what;
            if (targetRect(r))
                for (auto &b : det::boxes)
                    if (b.y1 > r.y0 + (r.y1 - r.y0) * 0.6) { userClass[b.cls] = 2; classBad[b.cls] = classGood[b.cls] = 0; what += std::string(what.empty() ? "" : ", ") + det::COCO[b.cls]; }
            note(what.empty() ? "couldn't see what you mean - try \"airplane is my gun\"" : "marked as yours: %s", what.c_str());
            did = true;
        } else if (named >= 0 && !aliasTo.empty() && !did) {
            static const char *skipw[] = {"enem", "hostile", "danger", "friend", "safe", "good", "bad", "food"};
            bool plain = true; for (auto w : skipw) if (aliasTo.find(w) != std::string::npos) plain = false;
            if (plain) { classAlias[named] = aliasTo; note("calling %ss \"%s\" from now on", det::COCO[named], aliasTo.c_str()); did = true; }
        }
    }
    for (const char *ph : {"that s a ", "that s an ", "thats a ", "thats an ", "that is a ", "that is an ", "looking at a ", "looking at an ", "looking at "}) {
        size_t k = t.find(ph);
        if (k == std::string::npos) continue;
        std::string nm = t.substr(k + strlen(ph));
        while (!nm.empty() && nm.back() == ' ') nm.pop_back();
        if (nm.empty()) break;
        { std::lock_guard<std::mutex> lk(mu); learnLook(nm, -1); }
        note("remembered what a %s looks like (the thing in the middle of the screen)", nm.c_str());
        did = true; break;
    }
    size_t at = std::string::npos;
    for (const char *ph : {"this place is ", "this is ", "here is ", "call this ", "this area is "}) if ((at = t.find(ph)) != std::string::npos) { at += strlen(ph); break; }
    for (const char *w : {"good", "great", "nice", "safe", "loot", "food", "water", "resources", "home", "base", "friendly", "friend"}) if (t.find(w) != std::string::npos) good = true;
    for (const char *w : {"bad", "danger", "enemy", "enemies", "avoid", "die", "death", "kill you", "hostile", "trap", "radiation", "hurts"}) if (t.find(w) != std::string::npos) bad = true;
    if (at != std::string::npos && place >= 0 && place < (int)places.size() && !good && !bad) {
        std::string nm = raw.substr(std::min(raw.size(), at));
        if (!nm.empty()) { places[place].look = "\"" + nm + "\""; note("named Place %d: %s", place + 1, nm.c_str()); did = true; }
    }
    if (good != bad && !did) {
        reward(good ? 2.5f : -2.5f, -1, good, (std::string("you said: ") + raw).c_str());
        if (at != std::string::npos && place >= 0 && place < (int)places.size()) places[place].look = "\"" + raw.substr(std::min(raw.size(), at)) + "\"";
        did = true;
    }
    if (did) saveRules();
    return did;
}
void openTell() {
    if (mode != 1 && mode != 2) return;
    for (auto &b : keyBits) b = 0;
    resumeMode = (int)mode;
    telling = true;
    mode = 3;
    releaseAll();
    { std::lock_guard<std::mutex> lk(gPromptMu); gPromptText.clear(); gPromptOpen = true; gPromptLast = now(); }
}


// ---------- taught tasks: you show it something, name it, say when to do it ----------
struct Step { uint8_t keys[32]; uint8_t btn; int16_t dx, dy; };
struct Task {
    std::string name, when;
    int kind = 0;  // 0 only when asked, 1 stat low, 2 stat high, 3 see a thing, 4 enemy ahead, 5 looking at, 6 every N seconds
    std::string what; double num = -1;
    std::vector<Step> steps;
    double lastRun = -1e9; int runs = 0;
};
static std::vector<Task> tasks;
static int teaching = -1;          // task being recorded
static double teachStart = 0;
static int runningTask = -1; static size_t runStep = 0;
static std::string taskWhy;

static void parseWhen(Task &k) {
    std::string t = " " + k.when + " ";
    auto num = [&](size_t from) { for (size_t i = from; i < t.size(); i++) if (isdigit((unsigned char)t[i])) return atof(t.c_str() + i); return -1.0; };
    size_t p;
    if ((p = t.find(" every ")) != std::string::npos) {
        double n = num(p); if (n <= 0) n = 1;
        if (t.find("min") != std::string::npos) n *= 60;
        k.kind = 6; k.num = n; return;
    }
    if (t.find("enem") != std::string::npos || t.find("attacked") != std::string::npos || t.find("getting shot") != std::string::npos) { k.kind = 4; return; }
    for (const char *ph : {" looking at ", " facing ", " in front of "})
        if ((p = t.find(ph)) != std::string::npos) {
            k.kind = 5; k.what = t.substr(p + strlen(ph));
            for (const char *dropw : {"a ", "an ", "the "}) if (k.what.rfind(dropw, 0) == 0) k.what = k.what.substr(strlen(dropw));
            while (!k.what.empty() && k.what.back() == ' ') k.what.pop_back();
            return;
        }
    for (const char *ph : {" see ", " there is ", " there s ", " spot "})
        if ((p = t.find(ph)) != std::string::npos) {
            for (int c = 0; c < 80; c++) {
                std::string n = className(c), n2 = det::COCO[c];
                if (t.find(" " + n, p) != std::string::npos || t.find(" " + n2, p) != std::string::npos) { k.kind = 3; k.num = c; k.what = n; return; }
            }
        }
    bool low = t.find(" low") != std::string::npos || t.find(" below") != std::string::npos || t.find(" under") != std::string::npos ||
               t.find(" drops") != std::string::npos || t.find(" less than") != std::string::npos || t.find(" empty") != std::string::npos ||
               t.find(" hungry") != std::string::npos || t.find(" thirsty") != std::string::npos || t.find(" hurt") != std::string::npos;
    bool high = t.find(" high") != std::string::npos || t.find(" above") != std::string::npos || t.find(" over ") != std::string::npos ||
                t.find(" full") != std::string::npos || t.find(" more than") != std::string::npos;
    if (low || high) {
        k.kind = low ? 1 : 2; k.num = num(0);
        static const char *skipw[] = {"when", "my", "is", "the", "gets", "get", "low", "below", "under", "drops", "high", "above", "over", "full", "less", "more",
                                      "than", "empty", "it", "i", "m", "am", "you", "are", "bar", "meter"};
        auto toks = split(t);
        for (auto &w : toks) {
            bool sk = isdigit((unsigned char)w[0]);
            for (auto s2 : skipw) if (w == s2) sk = true;
            if (!sk) { k.what = w; break; }
        }
        if (k.what == "hungry") k.what = "food";
        if (k.what == "thirsty") k.what = "water";
        if (k.what == "hurt") k.what = "health";
        return;
    }
    k.kind = 0;
}
static bool taskDue(Task &k, std::string &why) {
    double t = now();
    if (k.steps.empty()) return false;
    if (k.kind == 6) { if (t - k.lastRun > k.num) { why = "it's been " + std::to_string((int)k.num) + " s"; return true; } return false; }
    if (t - k.lastRun < 10) return false;
    if (k.kind == 1 || k.kind == 2) {
        for (auto &st : stats) {
            if (st.seen < 2 || st.label.find(k.what) == std::string::npos) continue;
            double th = k.num >= 0 ? k.num : st.maxSeen * (k.kind == 1 ? 0.4 : 0.9);
            if (k.kind == 1 ? st.last < th : st.last > th) { why = st.label + " is " + fmtNum(st.last); return true; }
        }
        return false;
    }
    if (k.kind == 3 && k.num >= 0 && t - classSeen[(int)k.num] < 0.5) { why = "saw a " + k.what; return true; }
    if (k.kind == 4) {
        if (sigEnemy > 0.5f) { why = "enemy ahead"; return true; }
        bool anyHostile = false; for (int c = 0; c < 80; c++) if (hostileClass(c)) anyHostile = true;
        std::lock_guard<std::mutex> dl(det::mu);
        if (now() - det::boxesAt < 0.5)
            for (auto &b : det::boxes)
                if (!b.own && (hostileClass(b.cls) || (!anyHostile && b.cls == 0))) { why = std::string("saw ") + className(b.cls); return true; }
        return false;
    }
    if (k.kind == 5 && lookingAt >= 0 && looks[lookingAt].label.find(k.what) != std::string::npos) { why = "looking at " + looks[lookingAt].label; return true; }
    return false;
}
static void saveTasks() {
    CreateDirectoryA("copycat", nullptr);
    std::string p = "copycat\\" + name + ".tasks";
    if (FILE *f = fopen(p.c_str(), "wb")) {
        uint32_t n = (uint32_t)tasks.size(); fwrite(&n, 4, 1, f);
        for (auto &k : tasks) {
            uint32_t L = (uint32_t)k.name.size(); fwrite(&L, 4, 1, f); fwrite(k.name.data(), 1, L, f);
            L = (uint32_t)k.when.size(); fwrite(&L, 4, 1, f); fwrite(k.when.data(), 1, L, f);
            uint32_t S = (uint32_t)k.steps.size(); fwrite(&S, 4, 1, f); if (S) fwrite(k.steps.data(), sizeof(Step), S, f);
            fwrite(&k.runs, 4, 1, f);
        }
        fclose(f);
    }
}
static void loadTasks() {
    tasks.clear();
    std::string p = "copycat\\" + name + ".tasks";
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return;
    uint32_t n = 0;
    if (fread(&n, 4, 1, f) == 1 && n < 500)
        for (uint32_t i = 0; i < n; i++) {
            Task k; uint32_t L = 0, S = 0;
            if (fread(&L, 4, 1, f) != 1 || L > 200) break; k.name.resize(L); if (L && fread(&k.name[0], 1, L, f) != L) break;
            if (fread(&L, 4, 1, f) != 1 || L > 400) break; k.when.resize(L); if (L && fread(&k.when[0], 1, L, f) != L) break;
            if (fread(&S, 4, 1, f) != 1 || S > 30 * 120) break; k.steps.resize(S); if (S && fread(k.steps.data(), sizeof(Step), S, f) != S) break;
            if (fread(&k.runs, 4, 1, f) != 1) break;
            parseWhen(k);
            tasks.push_back(k);
        }
    fclose(f);
}
// "teach heal when health is low", "learn: reload when ammo is empty", "this is how to loot when looking at a crate", "done", "do heal"
static bool taskCommand(const std::string &raw) {
    std::string t = lower(raw);
    for (auto &c : t) if (!isalnum((unsigned char)c) && c != ' ') c = ' ';
    std::string tt = " " + t + " ";
    if (teaching >= 0 && (tt.find(" done ") != std::string::npos || tt.find(" finished ") != std::string::npos || tt.find(" end ") != std::string::npos ||
                          tt.find(" stop ") != std::string::npos || tt.find(" that s it ") != std::string::npos || tt.find(" thats it ") != std::string::npos)) {
        Task &k = tasks[teaching];
        // trim idle frames at the start and end
        auto idleStep = [](const Step &s) { if (s.btn || std::abs(s.dx) + std::abs(s.dy) > 2) return false; for (int i = 0; i < 32; i++) if (s.keys[i]) return false; return true; };
        while (!k.steps.empty() && idleStep(k.steps.back())) k.steps.pop_back();
        size_t lead = 0; while (lead < k.steps.size() && idleStep(k.steps[lead])) lead++;
        k.steps.erase(k.steps.begin(), k.steps.begin() + lead);
        note("learned task \"%s\" (%.1f s of actions) - it'll do it %s", k.name.c_str(), k.steps.size() / 30.0, k.kind ? ("when " + k.when).c_str() : "when you say \"do " + k.name + "\"");
        if (k.steps.empty()) { note("...but you didn't press anything, so it's empty - teach it again"); tasks.erase(tasks.begin() + teaching); }
        teaching = -1; showUntil = 0;
        saveTasks();
        return true;
    }
    size_t p = std::string::npos; size_t len = 0;
    for (const char *ph : {" teach ", " learn ", " this is how to ", " this is how you ", " watch me ", " task "})
        if ((p = tt.find(ph)) != std::string::npos) { len = strlen(ph); break; }
    if (p != std::string::npos) {
        std::string rest = tt.substr(p + len);
        Task k;
        size_t w = rest.find(" when ");
        if (w == std::string::npos) w = rest.find(" every ");
        if (w == std::string::npos) w = rest.find(" if ");
        k.name = rest.substr(0, w == std::string::npos ? rest.size() : w);
        if (w != std::string::npos) k.when = rest.substr(w + 1);
        while (!k.name.empty() && k.name.front() == ' ') k.name.erase(0, 1);
        while (!k.name.empty() && k.name.back() == ' ') k.name.pop_back();
        while (!k.when.empty() && k.when.back() == ' ') k.when.pop_back();
        if (k.name.empty()) k.name = "task " + std::to_string(tasks.size() + 1);
        parseWhen(k);
        for (size_t i = 0; i < tasks.size(); i++) if (tasks[i].name == k.name) { tasks.erase(tasks.begin() + i); break; }
        tasks.push_back(k);
        teaching = (int)tasks.size() - 1; teachStart = now();
        showUntil = now() + 3600;
        note("TEACHING \"%s\" - do it now, then F10 \"done\"%s", k.name.c_str(), k.kind ? "" : " (no 'when' - it'll only do it when asked)");
        if (k.kind == 4) {
            bool anyHostile = false; for (int c = 0; c < 80; c++) if (hostileClass(c)) anyHostile = true;
            if (!anyHostile) note("(no enemy type set yet - any detected person will count. F10 \"person is enemy\" to be sure)");
        }
        return true;
    }
    for (const char *ph : {" do ", " run ", " go "})
        if ((p = tt.find(ph)) != std::string::npos) {
            std::string nm = tt.substr(p + strlen(ph));
            while (!nm.empty() && nm.back() == ' ') nm.pop_back();
            for (size_t i = 0; i < tasks.size(); i++)
                if (!tasks[i].name.empty() && (nm.find(tasks[i].name) != std::string::npos || tasks[i].name.find(nm) == 0)) {
                    runningTask = (int)i; runStep = 0; taskWhy = "you asked";
                    note("doing task \"%s\"", tasks[i].name.c_str());
                    return true;
                }
        }
    return false;
}

static void openAsk() {
    for (auto &b : keyBits) b = 0;
    telling = false;
    mode = 3;
    releaseAll();
    { std::lock_guard<std::mutex> lk(gPromptMu); gPromptText.clear(); gPromptOpen = true; gPromptLast = now(); }
    MessageBeep(MB_ICONQUESTION);
    note("CONFUSED: nothing I've seen looks like this - asking you");
    lastAsk = now();
}

static int wantTool = -1;
// live "brain" signals (0..1) for the neuron view
std::mutex bmu;
float bIn[8], bMem[6], bDrv[8], bOut[12];
float outAll[259];  // every key (0-255) + LMB/RMB/MMB, how much it's being pressed right now
static void feedOut(const uint8_t *keys, int btn) {
    for (int vk = 0; vk < 256; vk++) outAll[vk] = outAll[vk] * 0.6f + (hasKey(keys, vk) ? 0.4f : 0.f);
    for (int b = 0; b < 3; b++) outAll[256 + b] = outAll[256 + b] * 0.6f + ((btn >> b) & 1 ? 0.4f : 0.f);
}

static long decide(const uint8_t *f, const uint8_t *th) {
    double t0 = now();
    struct Hit { int d; size_t i; };
    std::vector<Hit> top;
    std::vector<int> needIdx;
    uint32_t needMask = 0;
    long pick = -1;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t s = 0; s < stats.size(); s++)
            if (stats[s].need && stats[s].seen > 2 && stats[s].maxSeen > 0 && (stats[s].last < stats[s].maxSeen * 0.55 || stats[s].trend < -0.5)) needIdx.push_back((int)s);
        for (int s : needIdx) if (s < 32) needMask |= 1u << s;
        if (data.size() < 60) { std::lock_guard<std::mutex> l2(amu); goal = "need more recording - press F6 and play for a bit"; return -1; }
        auto cmp = [](const Hit &a, const Hit &b) { return a.d < b.d; };
        // pass 1: tiny thumbnails, keep the 400 closest
        std::vector<Hit> rough;
        for (size_t i = 0; i + 1 < data.size(); i++) {
            const Frame &fr = data[i];
            if (fr.self == 1 && fr.value < 0.5) continue;  // only follow my own moves if they paid off
            if (data[i + 1].t - fr.t > 0.2) continue;  // needs a continuation to follow
            int d = distThumb(th, fr.thumb);
            if (rough.size() < 400) { rough.push_back({d, i}); std::push_heap(rough.begin(), rough.end(), cmp); }
            else if (d < rough.front().d) { std::pop_heap(rough.begin(), rough.end(), cmp); rough.back() = {d, i}; std::push_heap(rough.begin(), rough.end(), cmp); }
        }
        // pass 2: full sharp comparison on those
        for (auto &rh : rough) {
            int d = dist(f, data[rh.i].f);
            if ((int)top.size() < K) { top.push_back({d, rh.i}); std::push_heap(top.begin(), top.end(), cmp); }
            else if (d < top.front().d) { std::pop_heap(top.begin(), top.end(), cmp); top.back() = {d, rh.i}; std::push_heap(top.begin(), top.end(), cmp); }
        }
        if (top.empty()) return -1;
        int dmin = INT_MAX; for (auto &h : top) dmin = std::min(dmin, h.d);
        confusion = dmin / std::max(1.0, dRef);
        matchPct = std::max(0.0, 100.0 * (1.0 - dmin / (FD * 60.0)));
        bool enemyNear = false;
        for (auto &mv : movers)
            if (hostileBin(colourBin(mv.r, mv.g, mv.b)) && std::abs(mv.x - MW / 2) < MW / 4 && std::abs(mv.y - MH / 2) < MH / 3) enemyNear = true;
        {
            std::lock_guard<std::mutex> dl(det::mu);
            Rect r;
            if (now() - det::boxesAt < 0.5 && targetRect(r)) {
                int cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2, mx = (r.x1 - r.x0) / 4, my = (r.y1 - r.y0) / 4;
                for (auto &b : det::boxes)
                    if (!b.own && hostileClass(b.cls) && b.x1 > cx - mx && b.x0 < cx + mx && b.y1 > cy - my && b.y0 < cy + my) enemyNear = true;
            }
        }
        bool resourceAhead = lookingAt >= 0 && looks[lookingAt].stat >= 0 && needMask && (needMask & (1u << looks[lookingAt].stat));
        wantTool = -1;
        if (!needIdx.empty()) {
            int bn = 0;
            for (int i = 1; i <= 9; i++) if (tools[i].n >= 2 && tools[i].up[needIdx[0]] > bn) { bn = tools[i].up[needIdx[0]]; wantTool = i; }
        }
        double best = -1, val = 0, W = 0;
        for (auto &h : top) {
            const Frame &fr = data[h.i];
            double w = std::exp(-(h.d - dmin) / (dRef * 0.5 + 1)) * std::max(0.03, std::min(6.0, std::exp(0.9 * fr.value)));
            if (fr.self == 2) w *= 2;  // you corrected it here - trust that a lot
            {
                bool any = fr.btn || std::abs(fr.dx) + std::abs(fr.dy) > 2;
                for (int b2 = 0; b2 < 32 && !any; b2++) if (fr.keys[b2]) any = true;
                if (!any) w *= 0.25;  // standing still / typing / menus - copy those less
            }
            if (needMask && (fr.up & needMask)) w *= 3;
            if (needMask && (fr.down & needMask)) w *= 0.4;
            if (enemyNear && (fr.btn & 1)) w *= 2.5;
            if (resourceAhead && (fr.btn & 1)) w *= 3;
            W += w; val += w * fr.value;
            if (w > best) { best = w; pick = (long)h.i; }
        }
        // bored (nothing good or bad for a while): sometimes try the 2nd/3rd best memory instead
        if (now() - lastRewardAt > 25 && top.size() > 2 && rand() % 3 == 0) {
            std::vector<std::pair<double, size_t>> ws;
            for (auto &h : top) ws.push_back({std::exp(-(h.d - dmin) / (dRef * 0.5 + 1)), h.i});
            std::sort(ws.rbegin(), ws.rend());
            pick = (long)ws[1 + rand() % std::min<size_t>(2, ws.size() - 1)].second;
            { std::lock_guard<std::mutex> l3(amu); goal = "nothing's happening - trying something different"; }
        }
        {
            std::vector<double> ws;
            for (auto &h : top) ws.push_back(std::exp(-(h.d - dmin) / (dRef * 0.5 + 1)));
            std::sort(ws.rbegin(), ws.rend());
            std::lock_guard<std::mutex> bl(bmu);
            for (int i = 0; i < 6; i++) bMem[i] = i < (int)ws.size() ? (float)ws[i] : 0;
        }
        sigEnemy = enemyNear ? 1.f : 0.f; sigResource = resourceAhead ? 1.f : 0.f; sigNeed = needIdx.empty() ? 0.f : 1.f;
        sigGood = W > 0 ? (float)std::max(0.0, std::min(1.0, val / W)) : 0; sigBad = W > 0 ? (float)std::max(0.0, std::min(1.0, -val / W)) : 0;
        std::string g;
        if (!needIdx.empty()) {
            const Stat &st = stats[needIdx[0]];
            int bestP = -1, bestN = 0;
            for (size_t p = 0; p < places.size(); p++) if (places[p].up[needIdx[0]] > bestN) { bestN = places[p].up[needIdx[0]]; bestP = (int)p; }
            char b[200];
            snprintf(b, sizeof b, "%s is low (%s) - doing what raised it before%s", st.label.c_str(), fmtNum(st.last).c_str(),
                     bestP >= 0 ? (" (Place " + std::to_string(bestP + 1) + ")").c_str() : "");
            g = b;
            if (wantTool > 0) g += " with " + toolName(wantTool);
        } else if (enemyNear) g = "enemy-looking thing ahead - fighting like you did";
        if (resourceAhead) g = "looking at something that " + looks[lookingAt].label + " - hitting it";
        else if (W > 0 && val / W > 0.3) g = "heading for something good (similar moments paid off)";
        else if (W > 0 && val / W < -0.3) g = "careful - similar moments went badly";
        else g = "replaying what you did in a moment like this";
        std::lock_guard<std::mutex> l2(amu);
        goal = g;
        confusedTxt = confusion > 2.2 ? "very - this doesn't look like anything I've seen" : confusion > 1.5 ? "a bit - closest memory is a loose match" : "no";
    }
    decideMs = (now() - t0) * 1000;
    return pick;
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
        if (GetForegroundWindow() != tgt) {
            releaseAll();
            if (mode == 2 && !takeover) { note("the game isn't in front anymore - stopped playing for safety"); logf("Copycat stopped: game window lost focus"); mode = 0; break; }
            continue;
        }
        alignas(16) uint8_t f[FD]; uint8_t th[TN]; float red;
        stepLoop = "capture";
        if (!capture(f, th, red)) continue;
        frames++;
        stepLoop = "colours";
        {
            std::lock_guard<std::mutex> lk(amu);
            for (int c = 0; c < 3; c++) { double s = 0; for (int i = 0; i < 8; i++) s += f[FG + i * 3 + c]; allColor[c] += s / 8; }
            allColorN++;
        }
        if (!havePrev) redAvg = red;
        else if (red - redAvg > 18) { stepLoop = "red flash reward"; reward(-1.5f, -1, false, "screen flashed red (damage?)"); }
        redAvg = redAvg * 0.95f + red * 0.05f;
        stepLoop = "motion";
        int sx = 0, sy = 0; double change = 0;
        if (havePrev) shiftOf(prev, f, sx, sy, change);
        {
            static uint32_t prevMini[MW * MH];
            std::lock_guard<std::mutex> lk(mu);
            if (havePrev) findMovers(prevMini, mini, sx, sy);
            memcpy(prevMini, mini, sizeof prevMini);
            if (frames % 5 == 0) curTool = whichTool();
            if (frames % 3 == 0) lookingAt = recognizeLook();
            if (btnState & 1 || btnOut & 1) lastHitAt = now();
        }
        memcpy(prev, f, FD); havePrev = true;

        {
            static double stuckSince = 0;
            bool moving = hasKey(heldOut, 'W') || hasKey(heldOut, 'A') || hasKey(heldOut, 'S') || hasKey(heldOut, 'D');
            if (mode == 2 && !takeover && moving && change < 1.5) {
                if (stuckSince == 0) stuckSince = now();
                if (now() - stuckSince > 3) { reward(-0.7f, -1, false, "stuck - walking but going nowhere"); stuckSince = now(); }
            } else stuckSince = 0;
        }
        int m = mode;
        stepLoop = "mode";
        if (m == 3) {
            std::string a;
            { std::lock_guard<std::mutex> lk(amu); if (!answers.empty()) { a = answers.front(); answers.pop_front(); } }
            if (!a.empty()) {
                int pl = -1;
                { std::lock_guard<std::mutex> lk(mu); if (!data.empty()) pl = data.back().place; }
                if (taskCommand(a)) {
                    confusedSince = 0;
                    mode = telling ? (int)resumeMode : 2;
                    telling = false;
                    for (auto &b : keyBits) b = 0;
                    continue;
                }
                bool understood = explain(a, pl);
                Plan pp = parseAnswer(a);
                bool act = pp.show || !pp.keys.empty() || pp.btn || pp.mdx != 0 || (!understood && !telling);
                if (act) {
                    plan = pp;
                    Memory mm; memcpy(mm.f, f, FD); mm.text = a;
                    { std::lock_guard<std::mutex> lk(mu); mems.push_back(mm); }
                    if (plan.show) showUntil = now() + 15;
                }
                note("you said: \"%s\" -> %s", a.c_str(), plan.show && act ? "watching you for 15 s" : act ? "doing that now" : understood ? "noted" : "noted (didn't understand it as an action)");
                confusedSince = 0;
                mode = telling ? (int)resumeMode : 2;
                telling = false;
                for (auto &b : keyBits) b = 0;
            }
            continue;
        }
        bool watching = m == 1 || (m == 2 && (now() - physAt < 2.0 || now() < showUntil));
        if (m == 2 && watching && !takeover) { takeover = true; releaseAll(); if (runningTask >= 0) { tasks[runningTask].lastRun = now(); runningTask = -1; } if (teaching < 0) note("you took over - watching you"); }
        if (m == 2 && !watching && takeover) { takeover = false; note("back to playing on my own"); }

        Frame fr{}; memcpy(fr.f, f, FD); memcpy(fr.thumb, th, TN); fr.t = now();
        int dxr = rawDx.exchange(0), dyr = rawDy.exchange(0);
        if (watching) {
            for (int i = 0; i < 4; i++) { uint64_t b = keyBits[i]; memcpy(fr.keys + i * 8, &b, 8); }
            fr.btn = (uint8_t)btnState; fr.dx = (int16_t)std::max(-3000, std::min(3000, dxr)); fr.dy = (int16_t)std::max(-3000, std::min(3000, dyr));
            fr.self = (m == 2 && takeover) ? 2 : 0;
            if (teaching >= 0 && teaching < (int)tasks.size()) {
                Step st; memcpy(st.keys, fr.keys, 32); st.btn = fr.btn; st.dx = fr.dx; st.dy = fr.dy;
                tasks[teaching].steps.push_back(st);
                if (now() - teachStart > 120) { note("teaching ran 2 minutes - stopping it there"); taskCommand("done"); }
            }
            // you pressed a number key -> remember what the hotbar looks like with that slot picked
            static uint8_t lastKeys[32]; static int pendTool = 0; static double pendAt = 0;
            for (int d = 1; d <= 9; d++) if (hasKey(fr.keys, '0' + d) && !hasKey(lastKeys, '0' + d)) { pendTool = d; pendAt = now() + 0.35; }
            memcpy(lastKeys, fr.keys, 32);
            if (pendTool && now() > pendAt) {
                std::lock_guard<std::mutex> lk(mu);
                float sg[MW * 6]; hotbarSig(sg);
                Tool &tl = tools[pendTool];
                float a = tl.n ? std::max(0.1f, 1.f / (tl.n + 1)) : 1.f;
                for (int k = 0; k < MW * 6; k++) tl.sig[k] += (sg[k] - tl.sig[k]) * a;
                if (tl.n++ == 1) note("learned what the hotbar looks like with %s picked", toolName(pendTool).c_str());
                pendTool = 0;
            }
        } else {
            uint8_t ko[32]; int b = 0, mx = 0, my = 0;
            memset(ko, 0, 32);
            // never act if a visible cursor has wandered outside the game
            CURSORINFO ci{sizeof ci}; Rect cr;
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && targetRect(cr) && !inRect(ci.ptScreenPos.x, ci.ptScreenPos.y, cr)) {
                releaseAll();
                note("the mouse left the game window - stopped playing for safety");
                logf("Copycat stopped: cursor left the game window");
                mode = 0; break;
            }
            bool planOn = now() < plan.until && !plan.show;
            if (runningTask < 0 && !planOn)
                for (size_t i = 0; i < tasks.size(); i++) { std::string why; if (taskDue(tasks[i], why)) { runningTask = (int)i; runStep = 0; taskWhy = why; note("doing task \"%s\" (%s)", tasks[i].name.c_str(), why.c_str()); break; } }
            if (runningTask >= 0 && runningTask < (int)tasks.size()) {
                Task &k = tasks[runningTask];
                if (runStep < k.steps.size()) {
                    const Step &st = k.steps[runStep++];
                    memcpy(ko, st.keys, 32); b = st.btn; mx = std::max(-80, std::min(80, (int)st.dx)); my = std::max(-80, std::min(80, (int)st.dy));
                    std::lock_guard<std::mutex> lk(amu);
                    goal = "task \"" + k.name + "\" (" + taskWhy + ")"; actionTxt = "step " + std::to_string(runStep) + " / " + std::to_string(k.steps.size());
                    planOn = true;
                } else { k.lastRun = now(); k.runs++; runningTask = -1; }
            }
            if (planOn && runningTask >= 0) {
            } else if (planOn) {
                memset(ko, 0, 32);
                for (WORD vk : plan.keys) ko[vk >> 3] |= 1 << (vk & 7);
                b = plan.btn; mx = (int)plan.mdx; my = 0;
                std::lock_guard<std::mutex> lk(amu); goal = "doing what you said: \"" + plan.text + "\""; actionTxt = "following your answer";
            } else {
                stepLoop = "decide";
                static long clipIdx = -1; static int clipLeft = 0; static size_t clipGen = 0;
                bool repick = clipLeft <= 0 || clipIdx < 0 || clipGen != eraseGen;
                if (!repick) { std::lock_guard<std::mutex> lk(mu); repick = clipIdx >= (long)data.size() || dist(f, data[clipIdx].f) > dRef * 2.6; }
                if (repick || frames % 30 == 0) {
                    long p = decide(f, th);
                    if (repick) { clipIdx = p; clipLeft = 24; clipGen = eraseGen; }
                }
                std::string act;
                if (clipIdx >= 0) {
                    std::lock_guard<std::mutex> lk(mu);
                    if (clipIdx < (long)data.size()) {
                        const Frame &src = data[clipIdx];
                        usedMem.push_back({(size_t)clipIdx, eraseGenR, now()});
                        while (usedMem.size() > 200 || (!usedMem.empty() && now() - usedMem.front().t > 6)) usedMem.pop_front();
                        memcpy(ko, src.keys, 32); b = src.btn;
                        mx = std::max(-80, std::min(80, (int)src.dx)); my = std::max(-80, std::min(80, (int)src.dy));
                        for (int vk = 1; vk < 256; vk++) if (hasKey(ko, vk)) act += keyName(vk) + " ";
                        if (b & 1) act += "LMB "; if (b & 2) act += "RMB ";
                        long nx = clipIdx + 1;
                        if (nx < (long)data.size() && data[nx].t - src.t < 0.2 && (data[nx].self == 1) == (src.self == 1)) { clipIdx = nx; clipLeft--; }
                        else clipLeft = 0;
                    }
                }
                { std::lock_guard<std::mutex> lk(amu); actionTxt = act.empty() ? "nothing" : act + " (following a moment from memory)"; }
                {
                    static double idleSince = 0, lastIdleNote = 0;
                    if (act.empty()) {
                        if (idleSince == 0) idleSince = now();
                        if (now() - idleSince > 6 && now() - lastIdleNote > 30) {
                            size_t n; { std::lock_guard<std::mutex> lk(mu); n = data.size(); }
                            if (clipIdx < 0) note("not doing anything: no memory to follow (%d frames recorded - record more with F6)", (int)n);
                            else note("not doing anything: the memories that match this screen are moments where you stood still");
                            lastIdleNote = now();
                        }
                    } else idleSince = 0;
                }
                if (confusion > 2.2) {
                    if (confusedSince == 0) confusedSince = now();
                    if (now() - confusedSince > 4 && data.size() > 3000) {
                        int bm = -1, bd = INT_MAX;
                        { std::lock_guard<std::mutex> lk(mu); for (size_t i = 0; i < mems.size(); i++) { int d = dist(f, mems[i].f); if (d < bd) { bd = d; bm = (int)i; } } }
                        if (bm >= 0 && bd < dRef * 1.6) {
                            plan = parseAnswer(mems[bm].text);
                            note("this looks like when you said \"%s\" - doing that", mems[bm].text.c_str());
                            confusedSince = 0;
                        } else if (now() - lastAsk > 60) { openAsk(); continue; }
                    }
                } else confusedSince = 0;
            }
            static double lastSwitch = 0; static int switchFrames = 0, switchKey = 0;
            if (!planOn && wantTool > 0 && curTool != wantTool && now() - lastSwitch > 4 && !hasKey(banned, '0' + wantTool)) {
                switchKey = '0' + wantTool; switchFrames = 5; lastSwitch = now();
                note("switching to %s", toolName(wantTool).c_str());
            }
            if (switchFrames > 0) { switchFrames--; ko[switchKey >> 3] |= 1 << (switchKey & 7); }
            {
                std::lock_guard<std::mutex> bl(bmu);
                feedOut(ko, b);
                const int ok[8] = {'W', 'A', 'S', 'D', VK_SPACE, VK_SHIFT, VK_CONTROL, 'E'};
                for (int i = 0; i < 8; i++) bOut[i] = bOut[i] * 0.6f + (hasKey(ko, ok[i]) ? 0.4f : 0.f);
                bOut[8] = bOut[8] * 0.6f + ((b & 1) ? 0.4f : 0.f); bOut[9] = bOut[9] * 0.6f + ((b & 2) ? 0.4f : 0.f);
                bOut[10] = bOut[10] * 0.6f + (mx < -2 ? 0.4f : 0.f); bOut[11] = bOut[11] * 0.6f + (mx > 2 ? 0.4f : 0.f);
                bDrv[0] = sigEnemy; bDrv[1] = std::max(sigResource, wantTool > 0 ? 0.6f : 0.f); bDrv[2] = sigNeed;
                bDrv[3] = sigGood; bDrv[4] = sigBad; bDrv[5] = planOn ? 1.f : 0.f;
                bDrv[6] = (float)std::min(1.0, confusion / 3.0); bDrv[7] = std::max(0.f, 1.f - bDrv[6] - bDrv[0] * 0.5f);
            }
            stepLoop = "press keys";
            applyOut(ko, b);
            sendMove(mx, my);
            memcpy(fr.keys, ko, 32); fr.btn = (uint8_t)b; fr.dx = (int16_t)mx; fr.dy = (int16_t)my; fr.self = 1;
        }
        stepLoop = "store frame";
        {
            std::lock_guard<std::mutex> lk(mu);
            stepLoop = "place";
            fr.place = (uint16_t)std::max(0, placeFor(f));
            stepLoop = "key stats";
            data.push_back(fr);
            if (fr.self == 1) selfN++; else demoN++;
            if (data.size() > MAXFRAMES) {
                auto it = std::find_if(data.begin(), data.end(), [](const Frame &x) { return x.self == 1; });
                data.erase(it != data.end() ? it : data.begin());
                eraseGen++; eraseGenR++; usedMem.clear();
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
            placeTxt = "Place " + std::to_string(fr.place + 1) + " (" + places[fr.place].look + ")   holding: " + toolName(curTool) +
                       (lookingAt >= 0 ? "   looking at: " + looks[lookingAt].label : "");
            std::string mv;
            for (size_t i = 0; i < movers.size() && i < 3; i++) {
                const Mover &q = movers[i];
                int bin = colourBin(q.r, q.g, q.b);
                bool mid = std::abs(q.x - MW / 2) < MW / 4 && std::abs(q.y - MH / 2) < MH / 3;
                mv += (mv.empty() ? "" : ", ") + colourName(bin) + " " + shapeOf(q) + (mid ? " ahead" : q.x < MW / 2 ? " left" : " right") + (hostileBin(bin) ? " [ENEMY?]" : "");
            }
            moverTxt = mv.empty() ? "nothing moving" : mv;
            if (watching) { actionTxt = "recording you"; if (m == 1) goal = "learning what you do"; }
            {
                std::lock_guard<std::mutex> bl(bmu);
                int nDet = 0; { std::lock_guard<std::mutex> dl(det::mu); if (now() - det::boxesAt < 0.6) nDet = (int)det::boxes.size(); }
                float numFlash = 0;
                bIn[0] = (float)std::min(1.0, change / 25.0);
                bIn[1] = std::max(sigEnemy, 0.f);
                bIn[2] = std::min(1.f, nDet / 4.f);
                bIn[3] = std::min(1.f, movers.size() / 4.f);
                bIn[4] = lookingAt >= 0 ? 1.f : 0.f;
                bIn[5] = curTool > 0 ? 1.f : 0.f;
                bIn[6] = sigNeed; (void)numFlash;
                bIn[7] = watching ? 1.f : 0.f;
                if (watching) {
                    feedOut(fr.keys, fr.btn);
                    const int ok[8] = {'W', 'A', 'S', 'D', VK_SPACE, VK_SHIFT, VK_CONTROL, 'E'};
                    for (int i = 0; i < 8; i++) bOut[i] = bOut[i] * 0.6f + (hasKey(fr.keys, ok[i]) ? 0.4f : 0.f);
                    bOut[8] = bOut[8] * 0.6f + ((fr.btn & 1) ? 0.4f : 0.f); bOut[9] = bOut[9] * 0.6f + ((fr.btn & 2) ? 0.4f : 0.f);
                    bOut[10] = bOut[10] * 0.6f + (fr.dx < -2 ? 0.4f : 0.f); bOut[11] = bOut[11] * 0.6f + (fr.dx > 2 ? 0.4f : 0.f);
                    for (int i = 0; i < 6; i++) bMem[i] *= 0.9f;
                    bDrv[7] = 1.f; for (int i = 0; i < 7; i++) bDrv[i] *= 0.9f;
                }
            }
        }
        if (now() - lastAnalyze > 2) { stepLoop = "analyze"; analyze(); lastAnalyze = now(); }
        if (now() - lastSave > 600) { stepLoop = "save"; fileSave(); lastSave = now(); }
        stepLoop = "wait";
    }
    releaseAll();
    if (!mode) { analyze(); fileSave(); }
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
    { std::lock_guard<std::mutex> lk(mu); saveRules(); }
    if (teaching >= 0) taskCommand("done");
    saveTasks();
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
    { std::lock_guard<std::mutex> lk(mu); demoN = 0; selfN = 0; for (auto &d : data) (d.self == 1 ? selfN : demoN)++; }
    computeRef();
    { std::lock_guard<std::mutex> lk(mu); loadRules(); loadTasks(); }
    teaching = -1; runningTask = -1;
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
    stepHud = "drawing";
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
    std::string g, ct, at, wt, pt, mt, se; std::deque<std::string> ns; std::vector<std::string> fs;
    { std::lock_guard<std::mutex> lk(amu); g = goal; ct = confusedTxt; at = actionTxt; wt = watchTxt; pt = placeTxt; mt = moverTxt; se = seesTxt; ns = notes; fs = findings; }
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
    text(fL, lab, 18, 128, "SEES");
    text(fS, white, 130, 127, se);
    text(fL, lab, 18, 148, "MOVING");
    text(fS, white, 130, 147, mt);
    text(fL, lab, 18, 168, "ANALYZING");
    int y = 167;
    if (fs.empty()) { text(fS, dim, 130, y, "not enough data yet"); y += 16; }
    for (auto &s : fs) { text(fS, white, 130, y, s); y += 16; if (y > 232) break; }
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
    if (m == 3) text(fB, hot, 18, 470, std::string(telling ? "Tell me something" : "What are you doing?") + "  > " + pr + (std::fmod(now(), 1.0) < 0.55 ? "_" : " "));
    else text(fS, dim, 18, 474, m == 1 ? "F6 stop watching  -  F7 let it play  -  F10 tell it something" : "touch mouse/keys to take over  -  F7 stop  -  F10 tell it something");
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
    stepHud = "done";
}
bool hudWanted() { return mode && tgt && GetForegroundWindow() == tgt; }

void init() {
    std::thread(rawThread).detach();
    std::thread(ocrThread).detach();
    std::thread(detThread).detach();
}

}  // namespace cc
