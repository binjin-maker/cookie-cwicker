// ================= DETECT: a real trained object detector (YOLOX-S, 80 everyday classes) =================
// Runs on the GPU through ONNX Runtime + DirectML (falls back to CPU). Files next to the exe:
// onnxruntime.dll, DirectML.dll, onnxruntime_providers_shared.dll, yolox_s.onnx (the updater fetches them).
namespace det {

static const char *COCO[80] = {"person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light", "fire hydrant",
                               "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
                               "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
                               "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
                               "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut",
                               "cake", "chair", "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote",
                               "keyboard", "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors",
                               "teddy bear", "hair drier", "toothbrush"};

struct Box { int x0, y0, x1, y1; int cls; float conf; bool own = false; };

// ---- minimal ONNX Runtime C API, loaded at runtime (OrtApi is a stable table of function pointers) ----
typedef void *Status;
struct ApiBase { const void *const *(__stdcall *GetApi)(uint32_t); const char *(__stdcall *GetVersionString)(); };
static const void *const *api = nullptr;
template <class F> static F fn(int i) { return (F)api[i]; }
enum { I_GetErrorMessage = 2, I_CreateEnv = 3, I_CreateSession = 7, I_Run = 9, I_CreateSessionOptions = 10, I_SetExecMode = 13,
       I_DisableMemPattern = 17, I_SetOptLevel = 23, I_SetIntraThreads = 24, I_CreateTensor = 49, I_GetTensorData = 51,
       I_GetTypeShape = 65, I_GetDimCount = 61, I_GetDims = 62, I_CreateCpuMemInfo = 69, I_ReleaseStatus = 93, I_ReleaseValue = 96,
       I_ReleaseTypeShape = 99 };

static void *env = nullptr, *session = nullptr, *memInfo = nullptr;
static bool ready = false, triedLoad = false, onGpu = false;
static std::string status = "not loaded";
static std::mutex mu;
static std::vector<Box> boxes;
static double boxesAt = 0, ms = 0, rate = 0;
static int S = 640;

static bool ok(Status s, const char *what) {
    if (!s) return true;
    status = std::string(what) + ": " + fn<const char *(__stdcall *)(Status)>(I_GetErrorMessage)(s);
    fn<void(__stdcall *)(Status)>(I_ReleaseStatus)(s);
    return false;
}

static std::wstring exeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"" : s.substr(0, k + 1);
}

static bool load() {
    triedLoad = true;
    std::wstring dir = exeDir();
    std::wstring modelFile = L"yolox_tiny.onnx"; S = 416;
    if (GetFileAttributesW((dir + modelFile).c_str()) == INVALID_FILE_ATTRIBUTES) { modelFile = L"yolox_s.onnx"; S = 640; }
    if (GetFileAttributesW((dir + modelFile).c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW((dir + L"onnxruntime.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        status = "detector files not downloaded yet";
        return false;
    }
    HMODULE m = LoadLibraryExW((dir + L"onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { status = "couldn't load onnxruntime.dll"; return false; }
    auto getBase = (const ApiBase *(__stdcall *)())GetProcAddress(m, "OrtGetApiBase");
    if (!getBase) { status = "onnxruntime.dll is wrong"; return false; }
    api = getBase()->GetApi(17);
    if (!api) { status = "onnxruntime.dll too old"; return false; }
    if (!ok(fn<Status(__stdcall *)(int, const char *, void **)>(I_CreateEnv)(3, "cookiebot", &env), "env")) return false;
    auto makeSession = [&](bool gpu) -> bool {
        void *opts = nullptr;
        if (!ok(fn<Status(__stdcall *)(void **)>(I_CreateSessionOptions)(&opts), "options")) return false;
        fn<Status(__stdcall *)(void *, int)>(I_SetOptLevel)(opts, 99);
        if (gpu) {
            fn<Status(__stdcall *)(void *, int)>(I_SetExecMode)(opts, 0);
            fn<Status(__stdcall *)(void *)>(I_DisableMemPattern)(opts);
            auto addDml = (Status(__stdcall *)(void *, int))GetProcAddress(m, "OrtSessionOptionsAppendExecutionProvider_DML");
            if (!addDml || !ok(addDml(opts, 0), "gpu")) return false;
        } else fn<Status(__stdcall *)(void *, int)>(I_SetIntraThreads)(opts, 4);
        std::wstring model = dir + modelFile;
        return ok(fn<Status(__stdcall *)(void *, const wchar_t *, void *, void **)>(I_CreateSession)(env, model.c_str(), opts, &session), "model");
    };
    onGpu = makeSession(true);
    if (!onGpu) {
        std::string why = status;
        if (!makeSession(false)) return false;
        status = "running on CPU (" + why + ")";
    } else status = "running on GPU";
    status += S == 416 ? " (fast model)" : " (big model)";
    if (!ok(fn<Status(__stdcall *)(int, int, void **)>(I_CreateCpuMemInfo)(1, 0, &memInfo), "meminfo")) return false;
    ready = true;
    return true;
}

static float iou(const Box &a, const Box &b) {
    int x0 = std::max(a.x0, b.x0), y0 = std::max(a.y0, b.y0), x1 = std::min(a.x1, b.x1), y1 = std::min(a.y1, b.y1);
    float in = (float)std::max(0, x1 - x0) * std::max(0, y1 - y0);
    float u = (float)(a.x1 - a.x0) * (a.y1 - a.y0) + (float)(b.x1 - b.x0) * (b.y1 - b.y0) - in;
    return u > 0 ? in / u : 0;
}

// r = screen rect to look at. Returns boxes in screen coordinates.
static bool run(const Rect &r, std::vector<Box> &out) {
    out.clear();
    if (!ready) return false;
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (w < 32 || h < 32) return false;
    float sc = std::min((float)S / w, (float)S / h);
    int dw = std::max(1, (int)(w * sc)), dh = std::max(1, (int)(h * sc));
    static HDC mem = nullptr; static uint32_t *bits = nullptr;
    HDC scr = GetDC(nullptr);
    if (!mem) {
        mem = CreateCompatibleDC(scr);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 640; bi.bmiHeader.biHeight = -640; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&bits, nullptr, 0);
        SelectObject(mem, bmp);
    }
    SetStretchBltMode(mem, COLORONCOLOR);
    StretchBlt(mem, 0, 0, dw, dh, scr, r.x0, r.y0, w, h, SRCCOPY);
    GdiFlush();
    ReleaseDC(nullptr, scr);
    static std::vector<float> input;
    input.resize((size_t)3 * S * S);
    const size_t plane = (size_t)S * S;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            size_t i = (size_t)y * S + x;
            float b = 114, g = 114, rr = 114;
            if (x < dw && y < dh) { uint32_t p = bits[(size_t)y * 640 + x]; b = (float)(p & 255); g = (float)((p >> 8) & 255); rr = (float)((p >> 16) & 255); }
            input[i] = b; input[plane + i] = g; input[2 * plane + i] = rr;  // model wants BGR, 0-255
        }
    int64_t shape[4] = {1, 3, S, S};
    void *inVal = nullptr, *outVal = nullptr;
    if (!ok(fn<Status(__stdcall *)(void *, void *, size_t, const int64_t *, size_t, int, void **)>(I_CreateTensor)(memInfo, input.data(), input.size() * 4, shape, 4, 1, &inVal), "tensor")) return false;
    const char *inNames[1] = {"images"}, *outNames[1] = {"output"};
    double t0 = now();
    bool good = ok(fn<Status(__stdcall *)(void *, void *, const char *const *, const void *const *, size_t, const char *const *, size_t, void **)>(I_Run)(
                       session, nullptr, inNames, (const void *const *)&inVal, 1, outNames, 1, &outVal), "run");
    ms = (now() - t0) * 1000;
    fn<void(__stdcall *)(void *)>(I_ReleaseValue)(inVal);
    if (!good) return false;
    float *o = nullptr;
    fn<Status(__stdcall *)(void *, void **)>(I_GetTensorData)(outVal, (void **)&o);
    std::vector<Box> cand;
    int idx = 0;
    for (int st : {8, 16, 32}) {
        int n = S / st;
        for (int gy = 0; gy < n; gy++)
            for (int gx = 0; gx < n; gx++, idx++) {
                const float *p = o + (size_t)idx * 85;
                float obj = p[4];
                if (obj < 0.2f) continue;
                int best = 0; float bc = 0;
                for (int c = 0; c < 80; c++) if (p[5 + c] > bc) { bc = p[5 + c]; best = c; }
                float conf = obj * bc;
                if (conf < 0.45f) continue;
                float cx = (p[0] + gx) * st, cy = (p[1] + gy) * st, bw = std::exp(p[2]) * st, bh = std::exp(p[3]) * st;
                Box b{r.x0 + (int)((cx - bw / 2) / sc), r.y0 + (int)((cy - bh / 2) / sc), r.x0 + (int)((cx + bw / 2) / sc), r.y0 + (int)((cy + bh / 2) / sc), best, conf};
                cand.push_back(b);
            }
    }
    fn<void(__stdcall *)(void *)>(I_ReleaseValue)(outVal);
    std::sort(cand.begin(), cand.end(), [](const Box &a, const Box &b) { return a.conf > b.conf; });
    for (auto &c : cand) {
        bool keep = true;
        for (auto &k : out) if (k.cls == c.cls && iou(k, c) > 0.45f) { keep = false; break; }
        if (keep) out.push_back(c);
        if (out.size() >= 20) break;
    }
    return true;
}

// ---- tracking: keeps boxes steady between frames instead of flickering ----
struct Track { float x0, y0, x1, y1; int cls; float conf; int hits; double last; };
static std::vector<Track> tracks;
static void track(const std::vector<Box> &in, std::vector<Box> &out) {
    double t = now();
    std::vector<bool> used(tracks.size(), false);
    for (auto &b : in) {
        int best = -1; float bi = 0.25f;
        for (size_t i = 0; i < tracks.size(); i++) {
            if (used[i] || tracks[i].cls != b.cls) continue;
            Box tb{(int)tracks[i].x0, (int)tracks[i].y0, (int)tracks[i].x1, (int)tracks[i].y1, b.cls, 0};
            float v = iou(tb, b);
            if (v > bi) { bi = v; best = (int)i; }
        }
        if (best >= 0) {
            Track &k = tracks[best]; used[best] = true;
            const float a = 0.45f;
            k.x0 += (b.x0 - k.x0) * a; k.y0 += (b.y0 - k.y0) * a; k.x1 += (b.x1 - k.x1) * a; k.y1 += (b.y1 - k.y1) * a;
            k.conf = k.conf * 0.6f + b.conf * 0.4f; k.hits++; k.last = t;
        } else {
            tracks.push_back({(float)b.x0, (float)b.y0, (float)b.x1, (float)b.y1, b.cls, b.conf, 1, t});
            used.push_back(true);
        }
    }
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [t](const Track &k) { return t - k.last > 0.4; }), tracks.end());
    out.clear();
    for (auto &k : tracks)
        if (k.hits >= 3 || (k.hits >= 2 && k.conf > 0.6f)) out.push_back({(int)k.x0, (int)k.y0, (int)k.x1, (int)k.y1, k.cls, k.conf});
}

// ---- overlay: boxes + labels drawn over the game (hidden from the bot's own screenshots) ----
static HWND wnd = nullptr;
static void ensureWnd() {
    if (wnd) return;
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"CookieBotDet";
    RegisterClassW(&wc);
    wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"CookieBotDet", L"", WS_POPUP, 0, 0, 1, 1,
                          nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowDisplayAffinity(wnd, WDA_EXCLUDEFROMCAPTURE);
}
static void draw(const Rect &r, const std::vector<Box> &bs, const std::vector<std::string> &labels, const std::vector<int> &kind) {
    ensureWnd();
    int W = r.x1 - r.x0, H = r.y1 - r.y0;
    if (W <= 0 || H <= 0) return;
    static HDC dc = nullptr; static HBITMAP bmp = nullptr; static uint32_t *px = nullptr; static int cw = 0, ch = 0;
    if (!dc) dc = CreateCompatibleDC(nullptr);
    if (W != cw || H != ch) {
        if (bmp) DeleteObject(bmp);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, nullptr, 0);
        if (!bmp) { cw = ch = 0; return; }
        SelectObject(dc, bmp);
        cw = W; ch = H;
    }
    memset(px, 0, (size_t)W * H * 4);
    auto put = [&](int x, int y, uint32_t c) { if (x >= 0 && y >= 0 && x < W && y < H) px[(size_t)y * W + x] = c; };
    HFONT f = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Segoe UI");
    SelectObject(dc, f);
    SetBkMode(dc, OPAQUE);
    for (size_t i = 0; i < bs.size(); i++) {
        const Box &b = bs[i];
        uint32_t col = kind[i] == 2 ? 0xFF8A93A0 : kind[i] < 0 ? 0xFFFF3030 : kind[i] > 0 ? 0xFF40E070 : 0xFFFFD040;
        int x0 = b.x0 - r.x0, y0 = b.y0 - r.y0, x1 = b.x1 - r.x0, y1 = b.y1 - r.y0;
        for (int t = 0; t < 3; t++) {
            for (int x = x0; x <= x1; x++) { put(x, y0 + t, col); put(x, y1 - t, col); }
            for (int y = y0; y <= y1; y++) { put(x0 + t, y, col); put(x1 - t, y, col); }
        }
        std::wstring w(labels[i].begin(), labels[i].end());
        SetBkColor(dc, RGB((col >> 16) & 255, (col >> 8) & 255, col & 255));
        SetTextColor(dc, RGB(10, 10, 10));
        TextOutW(dc, std::max(0, x0), std::max(0, y0 - 18), w.c_str(), (int)w.size());
    }
    GdiFlush();
    // GDI text leaves alpha at 0: make every drawn pixel opaque
    for (size_t i = 0; i < (size_t)W * H; i++) if (px[i] & 0x00FFFFFF) px[i] |= 0xFF000000;
    SelectObject(dc, GetStockObject(SYSTEM_FONT)); DeleteObject(f);
    HDC scr = GetDC(nullptr);
    POINT pos{r.x0, r.y0}, zero{0, 0}; SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, scr, &pos, &sz, dc, &zero, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    if (!IsWindowVisible(wnd)) ShowWindow(wnd, SW_SHOWNOACTIVATE);
}
static void hide() { if (wnd && IsWindowVisible(wnd)) ShowWindow(wnd, SW_HIDE); }

}  // namespace det
