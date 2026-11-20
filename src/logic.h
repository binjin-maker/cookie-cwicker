#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>

struct Word { std::string text; int x, y, w, h; };
struct Seg {
    std::string text; int x, y, w, h;
    int cx() const { return x + w / 2; }
    int cy() const { return y + h / 2; }
    int r() const { return x + w; }
    int b() const { return y + h; }
};
struct Rect { int x0, y0, x1, y1; bool ok() const { return x1 > x0 && y1 > y0; } };

static inline std::string lower(std::string s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = s[i];
        if (c == 0xC3 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0x97) { o += 'x'; i++; continue; }
        o += (char)tolower(c);
    }
    return o;
}
static inline std::string lettersOnly(const std::string &s) {
    std::string o;
    for (char c : lower(s)) if (c >= 'a' && c <= 'z') o += c;
    return o;
}
static inline std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == ' ') { if (!cur.empty()) v.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

// group OCR words into phrase segments (same line, small gap)
static inline std::vector<Seg> segment(std::vector<Word> ws) {
    std::sort(ws.begin(), ws.end(), [](const Word &a, const Word &b) { return a.x < b.x; });
    std::vector<Seg> segs;
    std::vector<bool> used(ws.size(), false);
    for (size_t i = 0; i < ws.size(); i++) {
        if (used[i]) continue;
        used[i] = true;
        Seg s{ws[i].text, ws[i].x, ws[i].y, ws[i].w, ws[i].h};
        bool grew = true;
        while (grew) {
            grew = false;
            for (size_t j = 0; j < ws.size(); j++) {
                if (used[j]) continue;
                const Word &w = ws[j];
                int h = std::max(1, std::min(s.h, w.h));
                int hmax = std::max(s.h, w.h);
                int sc = s.y + s.h / 2, wc = w.y + w.h / 2;
                if (std::abs(sc - wc) > h * 0.6 + 2) continue;
                if (hmax > h * 2.2 && hmax - h > 8) continue;
                int gap = w.x - s.r();
                if (gap < -h || gap > h * 1.3 + 4) continue;
                used[j] = true;
                s.text += " " + w.text;
                int x0 = std::min(s.x, w.x), y0 = std::min(s.y, w.y);
                int x1 = std::max(s.r(), w.x + w.w), y1 = std::max(s.b(), w.y + w.h);
                s.x = x0; s.y = y0; s.w = x1 - x0; s.h = y1 - y0;
                grew = true;
            }
        }
        segs.push_back(s);
    }
    return segs;
}

static inline double unitMult(const std::string &tok) {
    static const char *names[] = {"thousand", "million", "billion", "trillion", "quadrillion", "quintillion",
                                  "sextillion", "septillion", "octillion", "nonillion", "decillion", "undecillion"};
    std::string t = lettersOnly(tok);
    if (t.size() < 4) return 0;
    for (int i = 0; i < 12; i++) {
        std::string n = names[i];
        if (t == n || (t.size() >= n.size() - 1 && n.compare(0, t.size(), t) == 0) ||
            (t.size() > n.size() && t.compare(0, n.size(), n) == 0))
            return std::pow(10.0, 3 * (i + 1));
    }
    return 0;
}

static inline bool parseNum(const std::string &tokIn, double &out) {
    std::string tin = tokIn;
    // "98,56" -> OCR read the decimal point as a comma (thousands groups are always 3 digits)
    size_t lc = tin.rfind(',');
    if (lc != std::string::npos && tin.find('.') == std::string::npos) {
        size_t tail = tin.size() - lc - 1;
        if (tail == 1 || tail == 2) tin[lc] = '.';
    }
    std::string t;
    int digits = 0;
    for (char c : tin) {
        char d = c;
        if (c == 'O' || c == 'o') d = '0';
        else if (c == 'l' || c == 'I' || c == '|') d = '1';
        else if (c == 'S') d = '5';
        else if (c == 'B') d = '8';
        if (isdigit((unsigned char)d)) { t += d; digits++; }
        else if (d == '.') t += '.';
        else if (d == ',') continue;
        else return false;
    }
    if (digits == 0) return false;
    if (std::count(t.begin(), t.end(), '.') > 1) return false;
    out = atof(t.c_str());
    return true;
}

// amount anywhere in text, prefers a number followed by a unit word
static inline bool findAmount(const std::string &text, double &out) {
    auto toks = split(text);
    bool found = false, foundUnit = false;
    for (size_t i = 0; i < toks.size(); i++) {
        std::string tk = toks[i];
        size_t k = 0;
        while (k < tk.size() && !isdigit((unsigned char)tk[k])) k++;
        if (k == tk.size() || k > 1) continue;
        size_t e = k;
        while (e < tk.size() && (isdigit((unsigned char)tk[e]) || tk[e] == '.' || tk[e] == ',')) e++;
        std::string num = tk.substr(k, e - k), rest = tk.substr(e);
        double v;
        if (!parseNum(num, v)) continue;
        double m = unitMult(rest);
        if (m == 0 && !rest.empty() && rest[0] != '/' && rest[0] != '%') continue;
        if (m == 0 && rest.empty() && i + 1 < toks.size()) m = unitMult(toks[i + 1]);
        if (foundUnit && m == 0) continue;
        out = v * (m ? m : 1);
        found = true;
        if (m) foundUnit = true;
    }
    return found;
}

// whole segment is a price: [icon junk] number [unit]
static inline bool isPrice(const std::string &text, double &out, bool &hasUnit, bool allowBareDecimal = false) {
    auto toks = split(text);
    while (!toks.empty()) {
        const std::string &t = toks.front();
        bool anyDigit = false;
        for (char c : t) if (isdigit((unsigned char)c)) anyDigit = true;
        if (!anyDigit && t.size() <= 2 && unitMult(t) == 0) toks.erase(toks.begin());
        else break;
    }
    if (toks.empty() || toks.size() > 3) return false;
    std::string t0 = toks[0];
    while (!t0.empty() && !isdigit((unsigned char)t0[0])) t0.erase(0, 1);
    size_t e = 0;
    while (e < t0.size() && (isdigit((unsigned char)t0[e]) || t0[e] == '.' || t0[e] == ',')) e++;
    std::string glued = t0.substr(e);
    t0 = t0.substr(0, e);
    double v;
    if (!parseNum(t0, v)) return false;
    double m = 0;
    if (!glued.empty()) { m = unitMult(glued); if (m == 0 && glued.size() > 1) return false; }
    if (toks.size() >= 2) {
        double m2 = unitMult(toks[1]);
        if (m2 == 0) { if (toks[1].size() > 2) return false; }
        else m = m2;
        if (toks.size() == 3 && toks[2].size() > 2) return false;
    }
    hasUnit = m != 0;
    if (!hasUnit && !allowBareDecimal && t0.find('.') != std::string::npos) return false;
    out = v * (m ? m : 1);
    return true;
}

static inline bool isInt(const std::string &text, int &out) {
    std::string t;
    for (char c : text) { if (c == ' ' || c == ',') continue; t += c; }
    if (t.empty() || t.size() > 6) return false;
    for (char &c : t) {
        if (c == 'O' || c == 'o') c = '0';
        if (c == 'l' || c == 'I' || c == '|') c = '1';
        if (!isdigit((unsigned char)c)) return false;
    }
    out = atoi(t.c_str());
    return true;
}

// ---------------- page layout ----------------
struct Layout {
    bool ok = false;
    Rect left, bakery, store, game;
    int cookieX = 0, cookieY = 0, cookieR = 0;
};

static inline Layout findLayout(const std::vector<Seg> &segs, int scrW, int scrH) {
    Layout L;
    const Seg *store = nullptr, *bake = nullptr, *sell = nullptr, *foot = nullptr, *clickS = nullptr;
    std::vector<const Seg *> stores, bakes;
    for (auto &s : segs) {
        std::string l = lower(s.text);
        if (l == "store" || l.rfind("store ", 0) == 0) stores.push_back(&s);
        if (l.find("your bakery") != std::string::npos) bakes.push_back(&s);
        if (!foot && (l.find("reset my game") != std::string::npos || l.find("is inspired") != std::string::npos)) foot = &s;
    }
    // the bakery heading sits left of the store heading, on roughly the same row
    int bestGap = 1 << 30;
    for (auto st : stores)
        for (auto bk : bakes) {
            if (bk->x >= st->x - 100) continue;
            int gap = st->x - bk->x;
            if (gap < bestGap) { bestGap = gap; store = st; bake = bk; }
        }
    if (!store || !bake) return L;
    for (auto &s : segs) {
        std::string l = lower(s.text);
        if (std::abs(s.cy() - store->cy()) < store->h * 1.5 && s.x > store->x && l.find("sell") != std::string::npos) sell = &s;
        if (s.x < bake->x && l.find("/click") != std::string::npos) clickS = &s;
    }
    int panelTop = std::min(store->y, bake->y) - store->h * 2;
    int bottom = foot ? foot->y - 10 : scrH - 60;
    int storeR = sell ? sell->r() + 40 : store->x + (store->x - bake->x) * 9 / 10;
    L.store = {store->x - 25, panelTop, std::min(storeR, scrW - 1), bottom};
    L.bakery = {bake->x - 20, panelTop, store->x - 40, bottom};
    int leftW = (bake->x - 20) - std::max(0, bake->x - (store->x - bake->x) - 40);
    L.left = {std::max(0, bake->x - 20 - leftW), panelTop, bake->x - 30, bottom};
    L.game = {std::max(0, L.left.x0 - 300), panelTop - 10, std::min(scrW - 1, L.store.x1 + 300), bottom};
    if (clickS) { L.cookieX = clickS->cx(); L.cookieY = clickS->b() + 200; }
    else { L.cookieX = (L.left.x0 + L.left.x1) / 2; L.cookieY = panelTop + 450; }
    L.ok = true;
    return L;
}

// ---------------- economy ----------------
struct Econ { double bank = -1, cps = -1, perClick = -1; bool guess = false; };

static inline Econ readEcon(const std::vector<Seg> &segs, const Rect &left) {
    Econ e;
    const Seg *cookiesLbl = nullptr;
    for (auto &s : segs) {
        if (s.cx() < left.x0 || s.cx() > left.x1 || s.cy() < left.y0 || s.cy() > left.y1) continue;
        std::string l = lower(s.text);
        double v;
        bool secLike = l.find("sec") != std::string::npos || (!l.empty() && l.back() == '/');
        if (secLike && l.find("click") == std::string::npos && e.cps < 0 && findAmount(s.text, v)) e.cps = v;
        else if (l.find("/click") != std::string::npos && e.perClick < 0 && findAmount(s.text, v)) e.perClick = v;
        else if (!cookiesLbl && l.find("cookies") != std::string::npos && l.find("meter") == std::string::npos &&
                 l.find("real") == std::string::npos && s.text.size() < 40) cookiesLbl = &s;
    }
    if (cookiesLbl) {
        double inLbl;
        std::string l = lower(cookiesLbl->text);
        if (findAmount(cookiesLbl->text, inLbl)) e.bank = inLbl;
        else {
            double mult = 1;
            for (auto &t : split(l)) { double m = unitMult(t); if (m) mult = m; }
            const Seg *best = nullptr;
            for (auto &s : segs) {
                if (s.b() > cookiesLbl->y + 4 || cookiesLbl->y - s.b() > cookiesLbl->h * 6) continue;
                if (std::abs(s.cx() - cookiesLbl->cx()) > cookiesLbl->w) continue;
                double v; bool u;
                if (isPrice(s.text, v, u, true) && (!best || s.y > best->y)) best = &s;
            }
            double v; bool u;
            if (best && isPrice(best->text, v, u, true)) e.bank = u ? v : v * mult;
        }
    }
    return e;
}

// ---------------- store / bakery ----------------
struct Item {
    std::string name, key, desc;
    bool research = false;
    double cost = 0;
    int owned = -1;
    int scroll = 0;  // wheel notches from top where it was seen
    int descY = 0;
    int nx = 0, ny = 0, px = 0, py = 0;  // name / price positions on screen
    double tip = -1;  // exact per-unit value from the hover tooltip
    int tipKind = 0;  // 1 = cookies/sec, 2 = cookies per click
};

static inline std::vector<Item> parseStore(const std::vector<Seg> &segs, const Rect &r, int scroll) {
    std::vector<Item> out;
    std::vector<const Seg *> in;
    for (auto &s : segs) if (s.cx() >= r.x0 && s.cx() <= r.x1 && s.cy() >= r.y0 && s.cy() <= r.y1) in.push_back(&s);
    int W = r.x1 - r.x0;
    const Seg *resHdr = nullptr, *showAll = nullptr;
    for (auto s : in) {
        std::string l = lower(s->text);
        if (!resHdr && l.find("research") != std::string::npos) resHdr = s;
        if (!showAll && (l.rfind("show all", 0) == 0 || l.rfind("show less", 0) == 0 || l.rfind("show fewer", 0) == 0)) showAll = s;
    }
    int resTop = resHdr ? resHdr->y : -1;
    int resBot = showAll ? showAll->y : (resHdr ? r.y1 : -1);
    auto isText = [](const std::string &t) {
        int letters = 0, other = 0;
        for (char c : t) { if (isalpha((unsigned char)c)) letters++; else if (c != ' ') other++; }
        return letters >= 3 && letters > other * 2;
    };
    for (auto p : in) {
        double cost; bool unit;
        if (!isPrice(p->text, cost, unit)) continue;
        bool inRes = resHdr && p->y > resTop && p->y < resBot;
        int relX = p->x - r.x0;
        if (inRes) {
            if (relX < W * 0.45) continue;
            const Seg *name = nullptr;
            for (auto s : in) {
                if (s == p || s->r() > p->x + 5 || !isText(s->text)) continue;
                if (s->y < p->y - p->h * 3 || s->y > p->y + p->h * 0.5) continue;
                if (s->y <= resTop + 4) continue;
                if (!name || s->y < name->y) name = s;
            }
            if (!name) continue;
            Item it; it.research = true; it.cost = cost; it.name = name->text; it.scroll = scroll;
            std::vector<const Seg *> d;
            for (auto s : in)
                if (s != name && s != p && s->r() <= p->x + 5 && s->y > name->y && s->y < name->y + name->h * 5.5 && isText(s->text)) d.push_back(s);
            std::sort(d.begin(), d.end(), [](const Seg *a, const Seg *b) { return a->y < b->y || (a->y == b->y && a->x < b->x); });
            for (auto s : d) it.desc += s->text + " ";
            it.key = "R:" + lettersOnly(it.name);
            it.descY = name->y;
            it.nx = name->cx(); it.ny = name->cy(); it.px = p->cx(); it.py = p->cy();
            out.push_back(it);
        } else {
            if (relX > W * 0.6) continue;
            if (resHdr && p->y <= resTop) continue;
            const Seg *name = nullptr;
            for (auto s : in) {
                if (s == p || !isText(s->text)) continue;
                if (s->b() > p->y + 3 || p->y - s->b() > p->h * 3) continue;
                if (std::abs(s->x - p->x) > W * 0.2) continue;
                if (!name || s->y > name->y) name = s;
            }
            if (!name) continue;
            Item it; it.cost = cost; it.name = name->text; it.scroll = scroll;
            for (auto s : in) {
                int n;
                if (s->x - r.x0 > W * 0.7 && s->cy() > name->y - name->h && s->cy() < p->b() + p->h && isInt(s->text, n)) it.owned = n;
            }
            it.key = "B:" + lettersOnly(it.name);
            it.nx = name->cx(); it.ny = name->cy(); it.px = p->cx(); it.py = p->cy();
            out.push_back(it);
        }
    }
    for (auto &a : out) {
        if (!a.research) continue;
        for (auto &b : out) {
            if (!b.research || b.descY <= a.descY) continue;
            size_t p = a.desc.find(b.name);
            if (p != std::string::npos) a.desc = a.desc.substr(0, p);
        }
    }
    std::vector<Item> uniq;
    for (auto &i : out) {
        bool dup = false;
        for (auto &u : uniq) if (u.key == i.key) dup = true;
        if (!dup) uniq.push_back(i);
    }
    return uniq;
}

struct BakeryLine { std::string key; int count; double rate; };

static inline std::vector<BakeryLine> parseBakery(const std::vector<Seg> &segs, const Rect &r) {
    std::vector<BakeryLine> out;
    std::vector<const Seg *> in;
    for (auto &s : segs) if (s.cx() >= r.x0 && s.cx() <= r.x1 && s.cy() >= r.y0 && s.cy() <= r.y1) in.push_back(&s);
    for (auto s : in) {
        std::string l = lower(s->text);
        auto toks = split(l);
        if (toks.size() < 2) continue;
        int count = -1; size_t nameEnd = 0;
        for (size_t i = 1; i < toks.size(); i++) {
            std::string t = toks[i]; int n;
            if (t == "x" && i + 1 < toks.size() && isInt(toks[i + 1], n)) { count = n; nameEnd = i; break; }
            if (t.size() > 1 && (t[0] == 'x' || t[0] == '*') && isInt(t.substr(1), n)) { count = n; nameEnd = i; break; }
        }
        if (count <= 0) continue;
        std::string name;
        for (size_t i = 0; i < nameEnd; i++) name += toks[i];
        name = lettersOnly(name);
        if (name.size() < 4) continue;
        const Seg *rate = nullptr;
        for (auto t : in) {
            if (t == s || t->x < s->r()) continue;
            std::string tl = lower(t->text);
            if (tl.find("sec") == std::string::npos) continue;
            if (t->cy() < s->y - s->h || t->cy() > s->b() + s->h * 4) continue;
            if (!rate || std::abs(t->cy() - s->cy()) < std::abs(rate->cy() - s->cy())) rate = t;
        }
        double v;
        if (!rate || !findAmount(rate->text, v)) continue;
        out.push_back({"B:" + name, count, v});
    }
    return out;
}


// hover tooltip: "each grandma makes 68.5 cookies per second" / "each mini clicker adds 16 cookies to every click"
static inline bool parseTip(const std::vector<Seg> &segs, const std::string &itemName, double &val, int &kind) {
    std::vector<const Seg *> v;
    for (auto &s : segs) v.push_back(&s);
    std::sort(v.begin(), v.end(), [](const Seg *a, const Seg *b) {
        if (std::abs(a->cy() - b->cy()) > std::min(a->h, b->h) / 2) return a->cy() < b->cy();
        return a->x < b->x;
    });
    std::string all;
    for (auto s : v) all += lower(s->text) + " ";
    std::string want = lettersOnly(lower(itemName));
    if (want.size() > 5) want = want.substr(0, 5);
    size_t pos = 0;
    while ((pos = all.find("each ", pos)) != std::string::npos) {
        size_t verb = std::string::npos, vlen = 0;
        for (const char *w : {" makes ", " adds ", " make ", " add "}) {
            size_t f = all.find(w, pos);
            if (f != std::string::npos && f - pos < 60 && f < verb) { verb = f; vlen = strlen(w); }
        }
        if (verb == std::string::npos) { pos += 5; continue; }
        std::string who = lettersOnly(all.substr(pos + 5, verb - pos - 5));
        size_t ck = all.find("cookie", verb);
        if (ck == std::string::npos || ck - verb > 40 || who.compare(0, want.size(), want) != 0) { pos += 5; continue; }
        double x;
        if (!findAmount(all.substr(verb + vlen, ck - verb - vlen), x) || x <= 0) { pos += 5; continue; }
        std::string after = all.substr(ck, 40);
        kind = after.find("click") != std::string::npos ? 2 : 1;
        val = x;
        return true;
    }
    return false;
}

static inline int prefixMatch(const std::string &a, const std::string &b) {
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) n++;
    return (int)n;
}


// ================= typed orders from the prompt bar =================
enum OrdKind { O_NONE, O_BUY, O_CANCEL, O_PAUSE, O_BUYON, O_BUYOFF, O_GOLDON, O_GOLDOFF, O_FXON, O_FXOFF,
               O_CPS, O_HOURS, O_RESCAN, O_UPDATE, O_SKIP, O_UNSKIP, O_HELP };
struct Order { OrdKind kind = O_NONE; std::string key, name; int count = 1; double val = 0; };

static inline bool hasWord(const std::string &t, const char *w) {
    std::string s = " " + t + " ", k = std::string(" ") + w;
    size_t f = s.find(k);
    while (f != std::string::npos) {
        char c = s[f + k.size()];
        if (!isalpha((unsigned char)c)) return true;
        f = s.find(k, f + 1);
    }
    return false;
}

static inline int editDist(const std::string &a, const std::string &b) {
    std::vector<int> d(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) d[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        int prev = d[0]; d[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) {
            int t = d[j];
            d[j] = std::min(std::min(d[j] + 1, d[j - 1] + 1), prev + (a[i - 1] == b[j - 1] ? 0 : 1));
            prev = t;
        }
    }
    return d[b.size()];
}
// does text contain f, allowing a typo or two
static inline bool fuzzyHas(const std::string &tl, const std::string &f) {
    if (tl.find(f) != std::string::npos) return true;
    if (f.size() < 5) return false;
    int tol = f.size() >= 9 ? 2 : 1;
    for (int L = (int)f.size() - 1; L <= (int)f.size() + 1; L++)
        for (int i = 0; i + L <= (int)tl.size(); i++)
            if (editDist(tl.substr(i, L), f) <= tol) return true;
    return false;
}

static inline Order parseOrder(const std::string &raw, const std::vector<std::pair<std::string, std::string>> &items) {
    Order o;
    std::string t = lower(raw);
    for (auto &c : t) if (!isalnum((unsigned char)c) && c != '.') c = ' ';
    double num = -1;
    {
        static const char *words[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                      "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty"};
        for (auto &tk : split(t)) {
            if (isdigit((unsigned char)tk[0])) { num = atof(tk.c_str()); break; }
            for (int i = 0; i <= 20; i++) if (tk == words[i]) { num = i; break; }
            if (num >= 0) break;
        }
        if (num < 0 && (hasWord(t, "a") || hasWord(t, "an") || hasWord(t, "another"))) num = 1;
    }
    bool off = hasWord(t, "off") || hasWord(t, "stop") || hasWord(t, "no") || hasWord(t, "dont") || hasWord(t, "don") ||
               hasWord(t, "disable") || hasWord(t, "hide") || hasWord(t, "never");
    bool on = hasWord(t, "on") || hasWord(t, "start") || hasWord(t, "enable") || hasWord(t, "show") || hasWord(t, "resume");

    // best item match: longest name (or its singular) that appears in the text
    std::string tl = lettersOnly(t);
    size_t best = 0;
    for (auto &it : items) {
        std::string n = lettersOnly(lower(it.second));
        std::vector<std::pair<std::string, int>> forms{{n, 3}};
        if (n.size() > 4 && n.back() == 's') forms.push_back({n.substr(0, n.size() - 1), 3});
        if (n.size() > 5 && n.back() == 'y') forms.push_back({n.substr(0, n.size() - 1), 3});
        auto ws = split(lower(it.second));
        if (ws.size() > 1 && lettersOnly(ws.back()).size() >= 5) forms.push_back({lettersOnly(ws.back()), 1});
        int bonus = it.first.rfind("B:", 0) == 0 ? 1 : 0;
        for (auto &f : forms) {
            size_t sc = f.first.size() * f.second * 2 + bonus;
            if (f.first.size() < 4 || sc <= best) continue;
            if (tl.find(f.first) != std::string::npos) { best = sc; o.key = it.first; o.name = it.second; }
            else if (fuzzyHas(tl, f.first)) { best = sc - 1; o.key = it.first; o.name = it.second; }
        }
    }

    if (hasWord(t, "help") || t.find("what can") != std::string::npos) { o.kind = O_HELP; return o; }
    if (hasWord(t, "cancel") || hasWord(t, "nevermind") || hasWord(t, "forget it") || t.find("never mind") != std::string::npos ||
        (hasWord(t, "clear") && o.key.empty())) { o.kind = O_CANCEL; return o; }
    if (hasWord(t, "update") || hasWord(t, "updates")) { o.kind = O_UPDATE; return o; }
    if (hasWord(t, "pause")) { o.kind = O_PAUSE; return o; }
    if (hasWord(t, "rescan") || hasWord(t, "scan") || hasWord(t, "reread") || (hasWord(t, "refresh") && hasWord(t, "store"))) { o.kind = O_RESCAN; return o; }
    if (hasWord(t, "golden") || hasWord(t, "bonus")) { o.kind = off ? O_GOLDOFF : O_GOLDON; return o; }
    if (hasWord(t, "hud") || hasWord(t, "effects") || hasWord(t, "fx") || hasWord(t, "vignette")) { o.kind = off ? O_FXOFF : O_FXON; return o; }
    if ((hasWord(t, "cps") || hasWord(t, "clicks") || t.find("click speed") != std::string::npos || hasWord(t, "speed")) && num > 0 && o.key.empty()) {
        o.kind = O_CPS; o.val = num; return o;
    }
    if ((hasWord(t, "hours") || hasWord(t, "hour") || hasWord(t, "session")) && num > 0) { o.kind = O_HOURS; o.val = num; return o; }
    if (!o.key.empty()) {
        if (hasWord(t, "skip") || hasWord(t, "ignore") || hasWord(t, "avoid") || (off && (hasWord(t, "buy") || hasWord(t, "buying")))) { o.kind = O_SKIP; return o; }
        if (hasWord(t, "unskip") || hasWord(t, "allow")) { o.kind = O_UNSKIP; return o; }
        o.kind = O_BUY; o.count = num >= 1 ? std::min(500, (int)num) : 1; return o;
    }
    if (hasWord(t, "buying") || hasWord(t, "buy") || hasWord(t, "upgrades") || hasWord(t, "purchases")) {
        if (off) { o.kind = O_BUYOFF; return o; }
        if (on || hasWord(t, "buying")) { o.kind = O_BUYON; return o; }
    }
    if (hasWord(t, "buy") || hasWord(t, "get") || hasWord(t, "save") || hasWord(t, "purchase") || hasWord(t, "grab")) {
        // wants something we can't see yet: keep what they named
        static const char *skip[] = {"buy", "get", "save", "up", "for", "and", "me", "a", "an", "the", "some", "more", "purchase", "grab", "please", "pls", "to", "another", "of"};
        std::string nm;
        for (auto &tk : split(t)) {
            bool sk = isdigit((unsigned char)tk[0]);
            for (auto w : skip) if (tk == w) sk = true;
            if (!sk) nm += (nm.empty() ? "" : " ") + tk;
        }
        if (!nm.empty()) { o.kind = O_BUY; o.name = nm; o.count = num >= 1 ? std::min(500, (int)num) : 1; }
    }
    return o;
}

// ================= BRAIN: everything it learns, saved between runs =================
struct Brain {
    std::map<std::string, double> unitCps;              // building -> cps per unit
    std::map<std::string, double> growth;               // building -> price multiplier per purchase
    std::map<std::string, std::pair<double, int>> word; // word -> (avg relative gain, samples) for unknown research
    std::map<std::string, double> corr;                 // effect type -> observed/predicted correction
    std::map<std::string, std::pair<int, int>> spot;    // "B:name" / "R:price" etc -> (successes, attempts)
    int notchesPerPage = 3;
    int settleMs = 450;
    double buyOverhead = 4.0;  // seconds of not-clicking per purchase (measured)
    int purchases = 0, failedBuys = 0, bonusClicks = 0, bonusHelped = 0;

    double growthFor(const std::string &k) const {
        auto f = growth.find(k);
        if (f != growth.end()) return f->second;
        double s = 0; int n = 0;
        for (auto &g : growth) { s += g.second; n++; }
        return n ? s / n : 1.15;  // base training: classic idle-game 15% growth
    }
    double corrFor(const std::string &t) const { auto f = corr.find(t); return f == corr.end() ? 1.0 : f->second; }

    void learnCorr(const std::string &t, double ratio) {
        ratio = std::max(0.05, std::min(20.0, ratio));
        double &c = corr.emplace(t, 1.0).first->second;
        c = std::exp(0.7 * std::log(c) + 0.3 * std::log(ratio));
    }
    void learnGrowth(const std::string &k, double g) {
        if (g < 1.0 || g > 3.0) return;
        auto f = growth.find(k);
        if (f == growth.end()) growth[k] = g; else f->second = 0.6 * f->second + 0.4 * g;
    }
    void learnUnit(const std::string &k, double v) {
        if (v <= 0) return;
        auto f = unitCps.find(k);
        if (f == unitCps.end()) unitCps[k] = v; else f->second = 0.5 * f->second + 0.5 * v;
    }
    static std::vector<std::string> keyWords(const std::string &s) {
        static const std::set<std::string> stop = {"your", "make", "makes", "each", "also", "with", "that", "this", "they",
                                                   "until", "once", "many", "much", "more", "than", "from", "the", "and", "cookie", "cookies"};
        std::vector<std::string> o;
        for (auto &t : split(lower(s))) { auto w = lettersOnly(t); if (w.size() >= 4 && !stop.count(w)) o.push_back(w); }
        return o;
    }
    double predictUnknown(const std::string &text) const {
        double s = 0; int n = 0;
        for (auto &w : keyWords(text)) { auto f = word.find(w); if (f != word.end() && f->second.second > 0) { s += f->second.first; n++; } }
        return n ? s / n : 0.03;  // base training: unknown upgrades are usually a few %
    }
    void learnUnknown(const std::string &text, double rel) {
        for (auto &w : keyWords(text)) {
            auto &p = word[w];
            p.first = (p.first * p.second + rel) / (p.second + 1);
            p.second = std::min(p.second + 1, 20);
        }
    }
    double spotScore(const std::string &k) const {
        auto f = spot.find(k);
        int s = f == spot.end() ? 0 : f->second.first, a = f == spot.end() ? 0 : f->second.second;
        return (s + 1.0) / (a + 2.0);
    }

    void save(const char *path) const {
        std::ofstream f(path);
        f << "O " << buyOverhead << "\nN " << notchesPerPage << "\nS " << settleMs << "\nP " << purchases << " " << failedBuys << " " << bonusClicks << " " << bonusHelped << "\n";
        for (auto &kv : unitCps) f << "U " << kv.first << " " << kv.second << "\n";
        for (auto &kv : growth) f << "G " << kv.first << " " << kv.second << "\n";
        for (auto &kv : word) f << "W " << kv.first << " " << kv.second.first << " " << kv.second.second << "\n";
        for (auto &kv : corr) f << "C " << kv.first << " " << kv.second << "\n";
        for (auto &kv : spot) f << "K " << kv.first << " " << kv.second.first << " " << kv.second.second << "\n";
    }
    void load(const char *path) {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string t, k; ss >> t;
            if (t == "O") ss >> buyOverhead;
            else if (t == "N") ss >> notchesPerPage;
            else if (t == "S") ss >> settleMs;
            else if (t == "P") ss >> purchases >> failedBuys >> bonusClicks >> bonusHelped;
            else if (t == "U") { double v; ss >> k >> v; unitCps[k] = v; }
            else if (t == "G") { double v; ss >> k >> v; growth[k] = v; }
            else if (t == "W") { double v; int n; ss >> k >> v >> n; word[k] = {v, n}; }
            else if (t == "C") { double v; ss >> k >> v; if (v >= 0.2 && v <= 5) corr[k] = v; }
            else if (t == "K") { int s, a; ss >> k >> s >> a; spot[k] = {s, a}; }
        }
        notchesPerPage = std::max(1, std::min(10, notchesPerPage));
        buyOverhead = std::max(1.0, std::min(20.0, buyOverhead));
        settleMs = std::max(250, std::min(1500, settleMs));
    }
};

// ================= upgrade understanding (base training) =================
enum EffKind { E_BUILD, E_BMULT, E_CLICKPCT, E_CLICKMULT, E_GLOBAL, E_UNKNOWN, E_NONE };
struct Effect { EffKind kind = E_NONE; std::string target; double v = 0; std::string type() const {
    static const char *n[] = {"build", "bmult", "clickpct", "clickmult", "global", "unknown", "none"}; return n[kind]; } };

static inline double numberBefore(const std::string &d, size_t pos) {
    size_t s = pos; while (s > 0 && (isdigit((unsigned char)d[s - 1]) || d[s - 1] == '.')) s--;
    return atof(d.substr(s, pos - s).c_str());
}

static inline std::string matchBuilding(const std::string &text, const std::vector<std::string> &buildings) {
    std::string nm = lettersOnly(text), best; int bestN = 0;
    for (auto &bk : buildings) {
        std::string b = bk.substr(2);
        for (size_t start = 0; start + 4 <= nm.size(); start++) {
            int n = prefixMatch(nm.substr(start), b);
            if (n >= (int)b.size() * 0.7 && n > bestN) { bestN = n; best = bk; }
        }
    }
    return best;
}

static inline Effect understand(const Item &it, const std::vector<std::string> &buildings) {
    Effect e;
    if (!it.research) { e.kind = E_BUILD; e.target = it.key; return e; }
    std::string d = lower(it.name + " " + it.desc);
    size_t pc = d.find("% of your cookies per second");
    if (pc == std::string::npos) pc = d.find("% of your cps");
    if (pc != std::string::npos) { e.kind = E_CLICKPCT; e.v = numberBefore(d, pc); if (e.v <= 0) e.v = 1; return e; }
    double mult = 0;
    if (d.find("x2") != std::string::npos || d.find("twice") != std::string::npos || d.find("double") != std::string::npos) mult = 2;
    if (d.find("x3") != std::string::npos || d.find("three times") != std::string::npos || d.find("triple") != std::string::npos) mult = 3;
    if (d.find("x4") != std::string::npos || d.find("four times") != std::string::npos) mult = 4;
    if (mult > 0) {
        std::string b = matchBuilding(it.name, buildings);
        if (b.empty()) b = matchBuilding(it.desc, buildings);
        if (!b.empty()) { e.kind = E_BMULT; e.target = b; e.v = mult; return e; }
        std::string nm = lettersOnly(it.name);
        if (nm.find("click") != std::string::npos || d.find("per click") != std::string::npos || d.find("clicking") != std::string::npos) {
            e.kind = E_CLICKMULT; e.v = mult; return e;
        }
    }
    size_t pp = d.find('%');
    if (pp != std::string::npos) {
        double pct = numberBefore(d, pp);
        if (pct > 0) {
            if (d.find("click") != std::string::npos) { e.kind = E_CLICKMULT; e.v = 1 + pct / 100; return e; }
            e.kind = E_GLOBAL; e.v = pct / 100; return e;
        }
    }
    e.kind = E_UNKNOWN;
    return e;
}

// ================= simulator + planner =================
struct SimItem { std::string key, name; bool research; double cost, growth; Effect eff; double corr = 1, unk = 0.03; };
struct SimState {
    double bank, cps, perClick, rate;
    std::map<std::string, double> unit, total;  // per building (current multipliers baked in)
    std::map<std::string, int> owned;
    double fix = 1;  // how much the building readings had to be scaled to match the real cps
    double P() const { return cps + perClick * rate; }
};

static inline double effGain(const SimItem &it, const SimState &s, const Brain &B, const std::string &text) {
    const Effect &e = it.eff;
    double c = it.corr;
    (void)B; (void)text;
    switch (e.kind) {
    case E_BUILD: { auto f = s.unit.find(e.target); return f == s.unit.end() ? 0 : f->second; }
    case E_BMULT: {
        auto f = s.total.find(e.target);
        if (f == s.total.end()) return 0;
        double tot = s.cps > 0 ? std::min(f->second, s.cps) : f->second;
        return tot * (e.v - 1) * c;
    }
    case E_CLICKPCT: return s.cps * e.v / 100 * s.rate * c;
    case E_CLICKMULT: return s.perClick * (e.v - 1) * s.rate * c;
    case E_GLOBAL: return s.cps * e.v * c;
    case E_UNKNOWN: return s.P() * it.unk * c;
    default: return 0;
    }
}

static inline void applyEff(const SimItem &it, SimState &s, const Brain &B, const std::string &text) {
    const Effect &e = it.eff;
    double g = effGain(it, s, B, text);
    switch (e.kind) {
    case E_BUILD: s.cps += g; s.total[e.target] += g; s.owned[e.target]++; break;
    case E_BMULT: s.cps += g; s.total[e.target] *= e.v; s.unit[e.target] *= e.v; break;
    case E_CLICKPCT: case E_CLICKMULT: s.perClick += g / std::max(0.1, s.rate); break;
    case E_GLOBAL: case E_UNKNOWN: {
        double f = 1 + g / std::max(1.0, s.cps + (e.kind == E_UNKNOWN ? s.perClick * s.rate : 0));
        s.cps *= f; for (auto &kv : s.unit) kv.second *= f; for (auto &kv : s.total) kv.second *= f; break;
    }
    default: break;
    }
}

struct Plan { int first = -1; double value = 0, wait = 0, gain = 0; std::string why, effType; std::vector<std::pair<std::string, double>> alts; };

// greedy-by-payback rollout, used inside the planner. every purchase costs buyOverhead seconds of no clicking.
static inline double rollout(std::vector<SimItem> items, std::vector<std::string> texts, SimState s, int forced, double T, double tail,
                             const Brain &B, int maxSteps = 600) {
    double t = 0, oh = B.buyOverhead;
    for (int step = 0; step < maxSteps; step++) {
        double ohLoss = oh * s.perClick * s.rate;
        int pick = -1; double best = 1e300;
        if (step == 0 && forced >= 0) pick = forced;
        else {
            for (size_t i = 0; i < items.size(); i++) {
                double g = effGain(items[i], s, B, texts[i]);
                if (g <= 0) continue;
                double w = std::max(0.0, (items[i].cost - s.bank) / std::max(1.0, s.P()));
                if (t + w + oh > T) continue;
                double pay = (items[i].cost + ohLoss) / g;
                if (pay > (T - t - w) + tail) continue;
                double sc = w + pay;
                if (sc < best) { best = sc; pick = (int)i; }
            }
        }
        if (pick < 0) break;
        double w = std::max(0.0, (items[pick].cost - s.bank) / std::max(1.0, s.P()));
        if (t + w + oh > T) break;
        t += w + oh;
        s.bank += s.P() * w + s.cps * oh - items[pick].cost;
        applyEff(items[pick], s, B, texts[pick]);
        if (items[pick].research) { items.erase(items.begin() + pick); texts.erase(texts.begin() + pick); }
        else items[pick].cost *= items[pick].growth;
    }
    s.bank += s.P() * (T - t);
    return s.bank + s.P() * tail;
}

static inline Plan plan(const std::vector<Item> &list, SimState s, const Brain &B, double sessionLeft) {
    std::vector<std::string> bks;
    for (auto &kv : s.unit) bks.push_back(kv.first);
    for (auto &it : list) if (!it.research) bks.push_back(it.key);
    std::vector<SimItem> items; std::vector<std::string> texts;
    for (auto &it : list) {
        SimItem si{it.key, it.name, it.research, it.cost, B.growthFor(it.key), understand(it, bks)};
        si.corr = B.corrFor(si.eff.type());
        if (si.eff.kind == E_UNKNOWN) si.unk = B.predictUnknown(it.name + " " + it.desc);
        items.push_back(si); texts.push_back(it.name + " " + it.desc);
    }
    double T = std::min(sessionLeft, 4 * 3600.0), tail = 3600;
    Plan p;
    struct Cand { int i; double v, w, g; };
    std::vector<Cand> cs;
    double best = -1e300;
    for (size_t i = 0; i < items.size(); i++) {
        double g = effGain(items[i], s, B, texts[i]);
        if (g <= 0) continue;
        double w = std::max(0.0, (items[i].cost - s.bank) / std::max(1.0, s.P()));
        if (w > sessionLeft) continue;  // can't get it this session -> don't save for it
        double v = rollout(items, texts, s, (int)i, T, tail, B);
        cs.push_back({(int)i, v, w, g});
        best = std::max(best, v);
    }
    for (auto &c : cs)
        if (p.first < 0 || c.v > p.value || (c.v == p.value && c.w < p.wait)) { p.first = c.i; p.value = c.v; p.wait = c.w; p.gain = c.g; }
    std::sort(cs.begin(), cs.end(), [](const Cand &a, const Cand &b) { return a.v > b.v; });
    for (auto &c : cs) {
        if (c.i == p.first || p.alts.size() >= 3) continue;
        p.alts.push_back({list[c.i].name, p.value > 0 ? (c.v / p.value - 1) * 100 : 0});
    }
    if (p.first >= 0) {
        char buf[300];
        snprintf(buf, sizeof buf, "%s [%s] cost %.3g, +%.3g/s, ready in %.0fs", list[p.first].name.c_str(),
                 items[p.first].eff.type().c_str(), list[p.first].cost, p.gain, p.wait);
        p.effType = items[p.first].eff.type();
        p.why = buf;
    }
    return p;
}

static inline SimState makeState(const Econ &e, const std::vector<BakeryLine> &bl, const std::vector<Item> &items, double rate, Brain &B) {
    SimState s;
    s.bank = std::max(0.0, e.bank); s.cps = std::max(0.0, e.cps); s.perClick = std::max(0.0, e.perClick); s.rate = rate;
    double sum = 0;
    for (auto &b : bl) {
        s.unit[b.key] = b.rate / b.count; s.total[b.key] = b.rate; s.owned[b.key] = b.count; sum += b.rate;
    }
    bool anyTip = false;
    for (auto &it : items) {
        if (it.research || it.tipKind == 0) continue;
        anyTip = true;
        double u = it.tipKind == 2 ? it.tip * rate : it.tip;
        sum -= s.total.count(it.key) ? s.total[it.key] : 0;
        s.unit[it.key] = u;
        if (it.owned > 0) { s.total[it.key] = u * it.owned; s.owned[it.key] = it.owned; sum += u * it.owned; }
    }
    std::vector<const Item *> missing;
    for (auto &it : items) if (!it.research && it.owned > 0 && !s.unit.count(it.key)) missing.push_back(&it);
    double leftover = s.cps - sum;
    if (missing.size() == 1 && leftover > 0 && !bl.empty()) {
        s.unit[missing[0]->key] = leftover / missing[0]->owned; s.total[missing[0]->key] = leftover;
    }
    for (auto &it : items) {
        if (it.research || s.unit.count(it.key)) continue;
        auto f = B.unitCps.find(it.key);
        if (f != B.unitCps.end()) {
            s.unit[it.key] = f->second;
            if (it.owned > 0) s.total[it.key] = f->second * it.owned;
        }
    }
    // building totals must add up to the real cps. if they don't, the readings are off (missed "million" etc) -> rescale
    double tot = 0; bool allKnown = true;
    for (auto &it : items) {
        if (it.research || it.owned <= 0) continue;
        auto f = s.total.find(it.key);
        if (f == s.total.end() || f->second <= 0) allKnown = false; else tot += f->second;
    }
    if (!anyTip && allKnown && tot > 0 && s.cps > 0) {
        double r = s.cps / tot;
        if (r > 1.5 || r < 0.67) {
            s.fix = r;
            for (auto &kv : s.unit) kv.second *= r;
            for (auto &kv : s.total) kv.second *= r;
        }
    }
    for (auto &b : bl) B.learnUnit(b.key, s.unit[b.key]);
    for (auto &it : items) if (!it.research && it.tipKind) B.learnUnit(it.key, s.unit[it.key]);
    // never-seen buildings: estimate from the efficiency of the priciest known one
    double bestCost = 0, eff = 0;
    for (auto &it : items) {
        if (it.research) continue;
        auto f = s.unit.find(it.key);
        if (f != s.unit.end() && f->second > 0 && it.cost > bestCost) { bestCost = it.cost; eff = f->second / it.cost; }
    }
    for (auto &it : items) {
        if (it.research || s.unit.count(it.key)) continue;
        s.unit[it.key] = eff > 0 ? it.cost * eff * 0.6 : it.cost / 1800.0;
    }
    return s;
}
