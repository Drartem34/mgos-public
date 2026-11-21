// pixel_waves.h — піксельні хвилі (вид згори) для шапок Megumin OS:
// about_osd і GUI-інсталятор mgOS. Лише cairo + libm, без GTK.
//
//   pw::Waves w;  w.set_accent("#9929CF");  w.set_palette_css("~/.cache/wal/colors.css");
//   w.t = секунди;  w.reveal = 0..1 (поява);  w.draw(cr, width, height);
//   w.accent_now() — поточний «живий» акцент (для CSS елементів, що пливуть разом із хвилями).
#pragma once
#include <cairo.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <thread>

namespace pw {

struct RGB { double r, g, b; };

inline bool parse_hex(const std::string& h, RGB& out) {
    if (h.size() != 7 || h[0] != '#') return false;
    unsigned v = 0;
    if (sscanf(h.c_str() + 1, "%06x", &v) != 1) return false;
    out = {((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0};
    return true;
}

inline void rgb_hsl(RGB c, double& h, double& s, double& l) {
    double mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    l = (mx + mn) / 2; h = s = 0;
    if (mx == mn) return;
    double d = mx - mn;
    s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6 : 0);
    else if (mx == c.g) h = (c.b - c.r) / d + 2;
    else h = (c.r - c.g) / d + 4;
    h *= 60;
}

inline double hue2(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

inline RGB hsl_rgb(double h, double s, double l) {
    h = std::fmod(std::fmod(h, 360) + 360, 360) / 360;
    if (s <= 0) return {l, l, l};
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    return {hue2(p, q, h + 1.0 / 3), hue2(p, q, h), hue2(p, q, h - 1.0 / 3)};
}

inline std::string to_hex(RGB c) {
    char b[8];
    snprintf(b, sizeof b, "#%02x%02x%02x", (int)std::lround(std::clamp(c.r, 0.0, 1.0) * 255),
             (int)std::lround(std::clamp(c.g, 0.0, 1.0) * 255), (int)std::lround(std::clamp(c.b, 0.0, 1.0) * 255));
    return b;
}
// rgba() без локалі (uk_UA пише кому — CSS ламається)
inline std::string to_rgba(RGB c, double a) {
    int m = (int)std::lround(std::clamp(a, 0.0, 1.0) * 1000);
    char b[64];
    snprintf(b, sizeof b, "rgba(%d,%d,%d,%d.%03d)", (int)std::lround(c.r * 255), (int)std::lround(c.g * 255),
             (int)std::lround(c.b * 255), m / 1000, m % 1000);
    return b;
}

inline double ease_out_cubic(double x) { x = std::clamp(x, 0.0, 1.0); return 1 - std::pow(1 - x, 3); }
inline double smooth(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); }

struct Waves {
    double hue0 = 280, sat0 = 0.7, light0 = 0.55;
    double hue_lo = -15, hue_hi = 15;  // межі відтінку відносно акценту (з палітри pywal)
    double t = 0;            // час, с
    std::vector<float> eq_lv; // «Еквалайзер»: рівні смуг 0..1 зі звуку (заповнює pixel_wallpaper, коли грає музика)
    float eq_mix = 0;        // 0 — власна анімація, 1 — повністю звук
    std::vector<float> eq_top; // «Еквалайзер»: верх кожної смуги (0..1 від верху) — рахується раз на кадр у render()
    double reveal = 1;      // 0..1 — поява (дизер-розчинення згори вниз з гребенем піни)
    int cell = 5;            // розмір «пікселя», логічні px
    double fade_from = 0.5;  // з якої висоти шапка розсипається у фон (0..1, >1 — без згасання)

    // Стиль анімації (налаштування «Стиль піксельних шапок»):
    //   "waves" — хвилі (за замовчуванням), "virus" — вірус (збудливе середовище, кільця зараження),
    //   "eq" — еквалайзер (вертикальні смуги), "rain" — піксельний дощ, "life" — гра «Життя»,
    //   "aurora" — піксельні хвилі-сяйво (як playhorizon.dev), "horizon" — піксельний захід сонця з горами,
    //   "blackhole" — гравітаційна сингулярність: тінь, акреційний диск, лінзування (курсор і вікна «засмоктує»),
    //   "neoncity" — нічний неоновий мегаполіс: вивіски, голограма, летючі машини, дощ, мокра вулиця,
    //   "rooftop" — мінімалістичний нічний дах: дівчина з ноутом, місто, цифровий місяць, частинки,
    //   "vortex" — гіперпросторовий фрактальний тунель із гексагональними кільцями та варп-іскрами,
    //   "synthwave" — ретровейв-каньйон із 3D-сіткою, каркасними горами та планетою з кільцями,
    //   "lava" — плазма-лампа (ферофлюїд / метаболи з електричними дугами між краплями),
    //   "abyss" — глибоководна біолюмінесценція: промені каустики, водорості та сяючі медузи,
    //   "crystal" — кристалічна жеода (3D-грані Вороного, сяючі жили та рухоме світло),
    //   "attractor" — 3D дивний атрактор Лоренца з кометними шлейфами на радарній сітці.
    std::string style = "waves";
    enum { S_WAVES, S_VIRUS, S_EQ, S_RAIN, S_LIFE, S_HORIZON, S_AURORA, S_BLACKHOLE, S_CITY, S_ROOF,
           S_VORTEX, S_SYNTH, S_LAVA, S_ABYSS, S_CRYSTAL, S_ATTRACTOR, S_TOWER, S_RIFT, S_VOID, S_SKYLINE, S_TORUS, S_KNOT, S_CLOUDS };
    int sid = S_WAVES;   // style як число — рахується раз на кадр (порівнювати рядки на кожен піксель дорого)
    int style_id() const {
        static const char* N[] = {"waves", "virus", "eq", "rain", "life", "horizon", "aurora", "blackhole", "neoncity", "rooftop",
                                  "vortex", "synthwave", "lava", "abyss", "crystal", "attractor", "tower", "riftportal", "voidfield", "skyline", "torus", "knot", "clouds"};
        for (int i = 0; i < 23; i++) if (style == N[i]) return i;
        return S_WAVES;
    }
    // Світла тема: фон світлий, «світло» хвиль/сяйва — насичене темніше (як негатив за яскравістю)
    bool light = false;
    double lt(double l) const { return light ? 0.96 - l * 0.7 : l; }

    cairo_surface_t* surf = nullptr;
    int sw = 0, sh = 0;

    // Стан клітинкових стилів (virus/life). Маленька сітка cols×rows, крок за часом t.
    std::vector<uint8_t> cells, cells2;
    std::string cells_style;
    long step_done = -1;
    double last_t = -1;
    uint32_t rng = 0x9E3779B9u;
    uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
    double rndf() { return (rnd() & 0xFFFFFF) / double(0x1000000); }
    static uint32_t hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
    static double hashf(uint32_t x) { return (hash(x) & 0xFFFF) / 65536.0; }

    // ---- вірус: модель Грінберга–Гастінгса. 0 — здорова, 1..2 — заразна, 3..K — імунітет ----
    // Кільця мають розходитися рівно і не накладатися: зараження майже гарантоване (8 сусідів, діагональ слабша —
    // фронти круглі, без розривів, з яких ростуть спіралі), довгий імунітет лишає темний слід, нові спалахи —
    // рідко і лише на «спокійній» ділянці.
    static constexpr int VK = 34;
    bool virus_calm(int cols, int rows, int x, int y, int r) const {
        for (int dy = -r; dy <= r; dy += 2)
            for (int dx = -r; dx <= r; dx += 2) {
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < cols && ny < rows && cells[ny * cols + nx]) return false;
            }
        return true;
    }
    void virus_seed(int cols, int rows, int n) {
        for (int i = 0, tries = 0; i < n && tries < 40; tries++) {
            int x = rnd() % cols, y = rnd() % rows;
            if (!virus_calm(cols, rows, x, y, std::max(6, rows / 8))) continue;
            cells[y * cols + x] = 1; i++;
        }
    }
    void virus_step(int cols, int rows) {
        int infected = 0;
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                uint8_t c = cells[y * cols + x], o = 0;
                if (c > 0) o = c + 1 > VK ? 0 : c + 1;
                else {
                    double w = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            if (!dx && !dy) continue;
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
                            uint8_t q = cells[ny * cols + nx];
                            if (q >= 1 && q <= 2) w += (dx && dy) ? 0.22 : 0.4;   // < 1 — фронт трохи «живий» і круглий, не восьмикутник
                        }
                    if (w > 0 && rndf() < w) o = 1;
                }
                if (o >= 1 && o <= 2) infected++;
                cells2[y * cols + x] = o;
            }
        cells.swap(cells2);
        if (infected == 0) virus_seed(cols, rows, 1 + (rnd() & 1));     // епідемія згасла — новий спалах
        else if (rndf() < 0.03) virus_seed(cols, rows, 1);             // ~раз на 3 с — нове кільце (лише на спокійній ділянці)
    }

    // ---- «Життя»: біт 7 — жива, нижні біти — вік (живої) або згасання сліду (мертвої) ----
    void life_seed(int cols, int rows, double dens) {
        for (auto& c : cells) if (!(c & 0x80) && rndf() < dens) c = 0x80;
    }
    void life_step(int cols, int rows) {
        int alive = 0;
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        int nx = (x + dx + cols) % cols, ny = (y + dy + rows) % rows;   // тор — без «стін»
                        n += cells[ny * cols + nx] >> 7;
                    }
                uint8_t c = cells[y * cols + x], o;
                bool live = c & 0x80;
                if (live && (n == 2 || n == 3)) o = 0x80 | std::min(0x7F, (c & 0x7F) + 1);
                else if (!live && n == 3) o = 0x80;
                else o = live ? 6 : (c > 0 ? c - 1 : 0);   // помер — слід згасає за 6 кроків
                if (o & 0x80) alive++;
                cells2[y * cols + x] = o;
            }
        cells.swap(cells2);
        if (alive < cols * rows / 25) life_seed(cols, rows, 0.12);
        else if (rndf() < 0.05) { int x = rnd() % cols, y = rnd() % rows;   // «мутація» — не дає застигнути
            for (int k = 0; k < 5; k++) cells[((y + k / 3) % rows) * cols + (x + k % 3) % cols] = (rnd() & 1) ? 0x80 : 0; }
    }

    void cells_advance(int cols, int rows, double rate) {
        bool reset = cells_style != style || (int)cells.size() != cols * rows || t < last_t;
        last_t = t;
        if (reset) {
            cells.assign(cols * rows, 0); cells2.assign(cols * rows, 0);
            cells_style = style;
            rng ^= (uint32_t)(t * 1000) + 0x51ED27u;
            if (style == "virus") virus_seed(cols, rows, 3); else life_seed(cols, rows, 0.3);
            step_done = (long)(t * rate);
            return;
        }
        long want = (long)(t * rate);
        for (int i = 0; i < 4 && step_done < want; i++, step_done++)   // не більше 4 кроків за кадр
            style == "virus" ? virus_step(cols, rows) : life_step(cols, rows);
        step_done = want;
    }

    // ---- «Аврора» — піксельні хвилі з playhorizon.dev (PixelAurora), у відтінку акценту ----
    // Дві «завіси» світла, що повільно хвилюються (передня ~80% висоти, задня ~60%): над краєм
    // завіси — сяйво з променями, біля краю — гаусів пік. Сума квантується в 10-кольорову
    // палітру з дизерингом Байєра 8×8. Плюс мерехтливі зорі та піксельний місяць 12×12.
    std::vector<float> au_front, au_frays, au_back, au_brays;
    std::vector<uint32_t> au_pal;
    void aurora_prepare(int cols, int rows, double hue, double s, double dim) {
        double H = rows * cell;
        au_front.resize(cols); au_frays.resize(cols); au_back.resize(cols); au_brays.resize(cols);
        for (int c = 0; c < cols; c++) {
            double a = (c + 0.5) * cell;
            au_front[c] = H * (0.8 + 0.07 * std::sin(0.004 * a + 0.32 * t) + 0.035 * std::sin(0.011 * a - 0.55 * t));
            au_frays[c] = 0.55 + 0.45 * std::sin(0.03 * a + 2.6 * std::sin(0.005 * a + 0.38 * t) + 0.25 * t);
            au_back[c]  = H * (0.6 + 0.06 * std::sin(0.003 * a - 0.21 * t + 2) + 0.03 * std::sin(0.009 * a + 0.4 * t));
            au_brays[c] = 0.5 + 0.5 * std::sin(0.022 * a - 2 * std::sin(0.004 * a - 0.27 * t) - 0.18 * t);
        }
        // палітра як в оригіналі (#100a1f → #ab3a75: темний → насичений), але по відтінках акценту
        au_pal.resize(10);
        double h0 = hue + hue_lo - 10, h1 = hue + hue_hi + 8;
        for (int i = 0; i < 10; i++) {
            double k = i / 9.0;
            RGB c = hsl_rgb(h0 + (h1 - h0) * k, std::clamp(s * (0.5 + 0.45 * k), 0.0, 1.0),
                            light ? 0.94 - 0.5 * std::pow(k, 1.15) : 0.07 + 0.42 * std::pow(k, 1.15));
            au_pal[i] = 0xFF000000u | (uint32_t)(std::clamp(c.r * dim, 0.0, 1.0) * 255) << 16
                      | (uint32_t)(std::clamp(c.g * dim, 0.0, 1.0) * 255) << 8 | (uint32_t)(std::clamp(c.b * dim, 0.0, 1.0) * 255);
        }
    }
    uint32_t aurora_px(int x, int y, int cols, int rows, double hue) const {
        static const int B8[64] = {0,32,8,40,2,34,10,42,48,16,56,24,50,18,58,26,12,44,4,36,14,46,6,38,60,28,52,20,62,30,54,22,
                                   3,35,11,43,1,33,9,41,51,19,59,27,49,17,57,25,15,47,7,39,13,45,5,37,63,31,55,23,61,29,53,21};
        static const char* MOON[12] = {"aaaaaaaaaaab","abbaaaaaaabb","abccbaaaabcb","abccbaaaaabb","aabbaaaaaaab","aaaaaabbaaab",
                                       "aaaaabccbaab","abbaabccbaab","bccbaabbaabb","abbaaaaaabcb","aaaaaaaaaabb","abbbbbbbbbbb"};
        auto sm = [](double e) { return e <= 0 ? 0.0 : e >= 1 ? 1.0 : e * e * (3 - 2 * e); };
        double H = rows * cell;
        // місяць: справа вгорі, з ореолом
        int mx = (int)(cols * 0.78), my = std::max(2, (int)(32.0 / cell));
        if (x >= mx && x < mx + 12 && y >= my && y < my + 12) {
            char ch = MOON[y - my][x - mx];
            RGB c = ch == 'a' ? hsl_rgb(hue + 40, 0.9, 0.92) : ch == 'b' ? hsl_rgb(hue + 35, 0.45, 0.8) : hsl_rgb(hue + 30, 0.3, 0.65);
            return 0xFF000000u | (uint32_t)(c.r * 255) << 16 | (uint32_t)(c.g * 255) << 8 | (uint32_t)(c.b * 255);
        }
        double ty = (y + 0.5) * cell;
        double w = 0.24 * H, M = 2 * std::pow(0.03 * H, 2);
        double v1 = std::min(140.0, 0.2 * H), j1 = std::min(100.0, 0.15 * H);
        double base = 0.25 + ty / H * 0.35, edge = sm(ty / v1) * sm((H - ty) / j1);
        double f = au_front[x] - ty, hb = au_back[x] - ty;
        double p = (base + (f > 0 ? std::exp(-f / w) * au_frays[x] : std::exp(-(f * f) / M)) * 7.5
                         + (hb > 0 ? std::exp(-hb / w) * au_brays[x] : std::exp(-(hb * hb) / M)) * 2.5) * edge;
        // ореол місяця
        double cxm = mx + 6, cym = my + 6, R = 2.4 * 12;
        double dm = std::max(std::fabs(y + 0.5 - cym), std::fabs(x + 0.5 - cxm));
        if (dm < R) p += 5 * (1 + 0.08 * std::sin(0.7 * t)) * sm(ty / 40) * std::pow(1 - dm / R, 2);
        int v = (int)std::floor(p);
        int jv = p - v > (B8[(y & 7) * 8 + (x & 7)] + 0.5) / 64.0 ? v + 1 : v;
        uint32_t px = au_pal[std::clamp(jv, 0, 9)];
        // зорі (верхня частина), мерехтять
        if (!light && ty < std::min(0.3 * H, 160.0) + 32) {
            uint32_t hs = hash(x * 2654435761u ^ (y * 40503u + 7));
            if ((hs % 30u) == 0 && (hs >> 8) % 6 == 0) {
                double r = 0.5 + 0.5 * std::sin(t * (0.6 + 1.4 * ((hs >> 12) % 100) / 100.0) + ((hs >> 20) % 63) * 0.1);
                if (r >= 0.35) { RGB c = r > 0.8 ? hsl_rgb(hue + 20, 0.6, 0.94) : hsl_rgb(hue, 0.35, 0.64);
                    px = 0xFF000000u | (uint32_t)(c.r * 255) << 16 | (uint32_t)(c.g * 255) << 8 | (uint32_t)(c.b * 255); }
            }
        }
        return px;
    }

    // ---- «Горизонт» (як playhorizon.dev): піксельний захід сонця у відтінку акценту ----
    // Небо з дизерним градієнтом, ретро-сонце зі смугами, що повзуть, 3 шари гір із деревами
    // (паралакс), туман над обрієм, мерехтливі зорі, падаюча зоря раз на ~9 с, світлячки.
    // Повертає ARGB (0 — прозоро), колір рахується тут, а не через палітру рівнів.
    std::vector<int> hz_top[3], hz_tree[3];
    // кеш hsl→rgb на кадр: у «Заході сонця» кольорів ~30, а викликів — сотні тисяч
    struct HC { double h, s, l; RGB c; bool used; };
    mutable HC hc_tab[128] = {};
    RGB hc(double h, double s, double l) const {
        uint64_t k = (uint64_t)(int64_t)std::llround(h * 64) * 1000003u ^ (uint64_t)std::llround(s * 4096) * 7919u ^ (uint64_t)std::llround(l * 4096);
        HC& e = hc_tab[k & 127];
        if (e.used && e.h == h && e.s == s && e.l == l) return e.c;
        e = {h, s, l, hsl_rgb(h, s, lt(l)), true};
        return e.c;
    }
    void horizon_prepare(int cols, int rows) {
        int horizon = (int)(rows * 0.56);
        auto ridge_top = [&](int L, double xs) {
            static const double BASE[3] = {-0.05, 0.08, 0.22}, AMP[3] = {0.07, 0.09, 0.11}, FREQ[3] = {0.05, 0.07, 0.09};
            double hgt = std::sin(xs * FREQ[L] + L * 1.7) * 0.5 + std::sin(xs * FREQ[L] * 2.3 + L * 4.1) * 0.3
                       + std::sin(xs * FREQ[L] * 5.1 + L * 2.9) * 0.2;
            return (int)std::round(horizon + rows * BASE[L] - rows * AMP[L] * (hgt + 0.6));
        };
        for (int L = 0; L < 3; L++) {
            hz_top[L].assign(cols, 0); hz_tree[L].assign(cols, 1 << 30);
            double shift = t * (0.5 + 1.3 * L);
            for (int x = 0; x < cols; x++) hz_top[L][x] = ridge_top(L, x + shift);
            if (L == 0) continue;
            int c0 = (int)std::floor(shift) - 4, c1 = (int)std::floor(cols + shift) + 4;
            for (int c = c0; c <= c1; c++) {                             // ялинки — спрайти-трикутники
                if (hash(c * 977 + L * 31) % 17 != 0) continue;
                int ttop = ridge_top(L, c), th = 3 + L + (int)(hash(c * 13 + L) % 3);
                for (int dy = 1; dy <= th; dy++) {
                    int half = dy <= 1 ? 0 : (th - dy + 1) / 2;
                    for (int dx = -half; dx <= half; dx++) {
                        int x = (int)std::floor(c - shift) + dx;      // колонка екрана
                        if (x < 0 || x >= cols) continue;
                        hz_tree[L][x] = std::min(hz_tree[L][x], ttop - dy);
                    }
                }
            }
        }
    }
    uint32_t horizon_px(int x, int y, int cols, int rows, double bay, double hue, double s, double dim) const {
        static const double PI = 3.14159265358979;
        auto pk = [&](RGB c) {
            c.r = std::clamp(c.r * dim, 0.0, 1.0); c.g = std::clamp(c.g * dim, 0.0, 1.0); c.b = std::clamp(c.b * dim, 0.0, 1.0);
            return 0xFF000000u | (uint32_t)(c.r * 255) << 16 | (uint32_t)(c.g * 255) << 8 | (uint32_t)(c.b * 255);
        };
        auto mix = [](RGB a, RGB b, double k) { return RGB{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k}; };
        double h_top = hue + hue_lo - 8, h_hor = hue + hue_hi + 6;     // верх неба холодніший, обрій — теплий край палітри
        int horizon = (int)(rows * 0.56);
        double fy = (double)y / rows, fx = (double)x / std::max(1, cols - 1);

        // --- гори: 3 шари (від ближнього); вершини й ялинки пораховані per-column у horizon_prepare ---
        for (int L = 2; L >= 0; L--) {
            int top = hz_top[L][x];
            bool tree = y < top && y >= hz_tree[L][x];
            if (y < top && !tree) continue;
            double haze = L == 0 ? 0.62 : L == 1 ? 0.3 : 0.0;          // атмосферна перспектива
            RGB body = light ? hsl_rgb(h_top + 6 + 8 * (2 - L), s * (0.35 + 0.15 * L), 0.36 + 0.16 * (2 - L))   // день: ближні темніші
                             : mix(hc(h_top + 6, s * 0.5, 0.06 + 0.03 * (2 - L)), hc(h_hor, s * 0.55, 0.36), haze);
            if (tree) return pk(body);                                   // ялинки — силуети
            if (y == top) {
                RGB rim = hc(h_hor + 10, s * 0.8, 0.5 + 0.08 * (2 - L));
                double glow = std::exp(-std::pow((x - cols * 0.5) / (cols * 0.3), 2));
                return pk(mix(body, rim, 0.35 + 0.6 * glow));
            }
            if (L == 0) {                                               // туман біля підніжжя дальніх гір
                double fog = std::clamp((y - horizon + rows * 0.02) / (rows * 0.1), 0.0, 1.0) * 0.7;
                if (bay < fog) body = mix(body, hc(h_hor, s * 0.6, 0.55), 0.35);
            }
            return pk(body);
        }

        // --- небо: 9 смуг з дизерингом Байєра (пікселі, без гладкого градієнта) ---
        double g = std::clamp(fy / (horizon / (double)rows), 0.0, 1.0);
        g = std::pow(g, 1.6);
        const int NB = 9;
        double band = g * (NB - 1);
        int b0 = (int)band; double fr = band - b0;
        int b = std::min(NB - 1, b0 + (bay < fr ? 1 : 0));
        double k = (double)b / (NB - 1);
        RGB sky = hc(h_top + (h_hor - h_top) * k, s * (0.55 + 0.4 * k), 0.04 + 0.52 * std::pow(k, 1.6));

        // --- сонце: верх — майже білий, низ — у колір обрію; смуги внизу повзуть униз ---
        double sr = rows * 0.24, scx = cols * 0.5, scy = horizon - rows * 0.13;
        double ddx = (x + 0.5 - scx) / 1.0, ddy = y + 0.5 - scy;
        double dist = std::sqrt(ddx * ddx + ddy * ddy);
        if (dist < sr) {
            double o = (ddy + sr) / (2 * sr);                       // 0 — верх диска
            bool gap = false;
            if (o > 0.42) {
                double period = std::max(3.0, sr / 4.5), th = 1 + (o - 0.42) / 0.58 * period * 0.6;
                gap = std::fmod(std::fmod(y - scy - t * 2.2, period) + period, period) < th;
            }
            if (!gap) {
                RGB top = hsl_rgb(h_hor + 25, 0.9, light ? 0.97 : 0.9), bot = hsl_rgb(h_hor - 5, std::max(s, 0.75), light ? 0.72 : 0.6);
                return pk(mix(top, bot, std::clamp(o, 0.0, 1.0)));
            }
        } else if (dist < sr * 2.2) {                                // ореол
            double gl = (1 - (dist - sr) / (sr * 1.2)) * (0.9 + 0.1 * std::sin(t * 0.9));
            if (bay < gl * 0.55) sky = mix(sky, hc(h_hor + 15, 0.85, 0.7), 0.35 * gl);
        }

        // --- зорі (лише високо в небі) ---
        if (!light && fy < 0.42) {
            uint32_t hs = hash(x * 73856093u ^ y * 19349663u);
            if ((hs & 1023) < 9) {
                double tw = 0.5 + 0.5 * std::sin(t * (0.5 + (hs >> 10) % 20 * 0.1) + (hs >> 16) % 628 * 0.01);
                double br = (1 - fy / 0.42) * (0.35 + 0.65 * tw);
                if (br > 0.15) sky = mix(sky, ((hs >> 20) & 1) ? RGB{0.82, 0.78, 1.0} : RGB{1.0, 0.95, 0.89}, br);
            }
        }
        // --- падаюча зоря: раз на 9 с, ~0.9 с ---
        {
            long slot = (long)std::floor(t / 9.0);
            double ph = t - slot * 9.0 - (1 + 5 * hashf(slot * 7919 + 13));
            if (ph > 0 && ph < 0.9 && hashf(slot * 31 + 7) < 0.8) {
                int dir = hashf(slot * 17 + 3) < 0.5 ? -1 : 1;
                double sx = cols * (0.15 + 0.7 * hashf(slot * 101 + 5)) + dir * ph * cols * 0.4;
                double sy = rows * (0.05 + 0.2 * hashf(slot * 211 + 9)) + ph * rows * 0.25;
                double ex = x - sx, ey = y - sy;
                double along = -(ex * dir * 0.85 + ey * 0.5), across = std::fabs(ex * 0.5 - ey * dir * 0.85);
                if (along >= 0 && along < cols * 0.08 && across < 0.6) {
                    double a = std::sin(ph / 0.9 * PI) * (1 - along / (cols * 0.08));
                    sky = mix(sky, RGB{1.0, 0.95, 0.9}, a);
                }
            }
        }
        // --- хмаринки: піксельні смужки, що пливуть, з підсвіченим низом ---
        for (int c = 0; c < 3; c++) {
            double cy = rows * (0.12 + 0.11 * c), cw = cols * (0.22 + 0.08 * hashf(c * 5 + 1));
            double cx = std::fmod(cols * hashf(c * 9 + 2) + t * (1.2 + 0.7 * c), cols + cw) - cw * 0.5;
            double ddx2 = (x - cx) / (cw * 0.5), ddy2 = (y - cy) / 2.2;
            double shape = 1 - ddx2 * ddx2 - ddy2 * ddy2 * (1 + 0.6 * std::sin(x * 0.35 + c));
            if (shape > 0) { RGB cl = hc(h_top + 10, s * 0.4, 0.18 + 0.05 * c); if (ddy2 > 0.25) cl = hc(h_hor, s * 0.7, 0.42); sky = cl; }
        }
        (void)fx;
        return pk(sky);
    }


    // ---- «Чорна діра» — гравітаційна сингулярність у піксель-арті ----
    // Абсолютно чорна тінь, тонке фотонне кільце, майже ребром — акреційний диск (кеплерівське
    // обертання, ліва сторона яскравіша — доплерівське підсилення), над і під тінню — вигнуте
    // лінзою зображення дальнього боку диска. Фон (зорі, туманність) і «привиди» вікон та курсора
    // беруться через рівняння точкової лінзи β = θ − θE²/θ, тож біля діри вони викривляються,
    // розтягуються в дуги й з'являються з протилежного боку (кільце Ейнштейна).
    // Привиди видно лише там, де лінза помітно зсуває картинку — під самими вікнами їх немає.
    struct BHRect { float x0, y0, x1, y1; bool active; };
    std::vector<BHRect> bh_wins;           // у клітинках сітки
    double bh_cx = -1e9, bh_cy = -1e9;     // курсор, у клітинках
    std::vector<uint32_t> bh_pal;          // 12 рівнів світла диска
    RGB bh_bg[16][4], bh_star, bh_acc, bh_rim, bh_cur;   // кольори на кадр (hsl_rgb на кожен піксель дорогий)
    // «Засмоктування»: від країв вікон, звернених до діри, відриваються пікселі (справжні кольори
    // вікна — з маленького знімка екрана bh_shot, або синтетичні, якщо знімка нема) і по спіралі
    // падають у діру, нагріваючись до кольору диска та гаснучи за горизонтом подій.
    struct BHPart { float x, y, vx, vy, life; uint32_t c; };
    std::vector<BHPart> bh_parts;
    std::vector<uint32_t> bh_shot; int bh_shot_w = 0, bh_shot_h = 0;   // знімок екрана в сітці клітинок
    double bh_last_t = -1, bh_spawn_acc = 0;
    bool bh_draw_wins = false;   // прев'ю: малювати самі «вікна» (у системі на їхньому місці справжні)
    uint32_t bh_win_color(const BHRect& w, double px, double py, int cols, int rows) const {
        if (!bh_shot.empty() && cols > 0 && rows > 0) {
            int sx = std::clamp((int)(px * bh_shot_w / cols), 0, bh_shot_w - 1), sy = std::clamp((int)(py * bh_shot_h / rows), 0, bh_shot_h - 1);
            return bh_shot[sy * bh_shot_w + sx] | 0xFF000000u;
        }
        // синтетичне «вікно»: заголовок у кольорі акценту, тіло темне з рядками «тексту»
        auto pk = [](RGB c) { return 0xFF000000u | (uint32_t)(c.r * 255) << 16 | (uint32_t)(c.g * 255) << 8 | (uint32_t)(c.b * 255); };
        if (py - w.y0 < 2.5) return pk(bh_acc);
        int ly = (int)(py - w.y0), lx = (int)(px - w.x0);
        if (ly % 3 == 1 && lx > 2 && (hash(ly * 131 + (int)w.x0) % 40) + 5 > lx % 45) return pk(RGB{0.82, 0.82, 0.88});
        return pk(RGB{0.11, 0.11, 0.14});
    }
    void blackhole_particles(int cols, int rows) {
        double dt = t - bh_last_t;
        if (bh_last_t < 0 || dt < 0 || dt > 0.5) dt = 0.05;
        bh_last_t = t;
        if (dt <= 0) return;
        const double hx = cols * 0.5, hy = rows * 0.5, Rs = rows * 0.13, Rinf = rows * 1.1;
        // народження: точки периметра, повернуті до діри; ближче вікно — більше пікселів
        for (const auto& w : bh_wins) {
            double nx = std::clamp(hx, (double)w.x0, (double)w.x1), ny = std::clamp(hy, (double)w.y0, (double)w.y1);
            double d = std::hypot(nx - hx, ny - hy);
            if (d < Rs) continue;                                    // діра під вікном
            double strength = std::clamp((Rinf - d) / Rinf, 0.08, 1.0);
            double per = 2 * ((w.x1 - w.x0) + (w.y1 - w.y0));
            bh_spawn_acc += per * strength * strength * 2.8 * dt;
            int n = (int)bh_spawn_acc; bh_spawn_acc -= n;
            for (int i = 0; i < n * 3 && n > 0 && bh_parts.size() < 5000; i++) {
                double u = rndf() * per, px, py, ex, ey;   // точка на периметрі і зовнішня нормаль
                double W = w.x1 - w.x0, H = w.y1 - w.y0;
                if (u < W) { px = w.x0 + u; py = w.y0; ex = 0; ey = -1; }
                else if (u < W + H) { px = w.x1; py = w.y0 + (u - W); ex = 1; ey = 0; }
                else if (u < 2 * W + H) { px = w.x1 - (u - W - H); py = w.y1; ex = 0; ey = 1; }
                else { px = w.x0; py = w.y1 - (u - 2 * W - H); ex = -1; ey = 0; }
                double tx = hx - px, ty = hy - py, tl = std::sqrt(tx * tx + ty * ty) + 1e-6;
                double face = (ex * tx + ey * ty) / tl;
                if (face <= 0.05 || rndf() > face) continue;          // лише боки, звернені до діри
                if (--n < 0) break;
                uint32_t c = bh_win_color(w, px - ex * 0.8, py - ey * 0.8, cols, rows);   // колір з краю вікна
                double sp = 2 + 4 * rndf();
                bh_parts.push_back({(float)(px + ex * 0.6), (float)(py + ey * 0.6),
                                    (float)(tx / tl * sp - ty / tl * sp * 0.6), (float)(ty / tl * sp + tx / tl * sp * 0.6), 0, c});
            }
        }
        // рух: тяжіння ~1/r^1.5 + закручування, легке гальмування — спіраль
        int steps = dt > 0.06 ? 2 : 1; double h = dt / steps;
        for (int st = 0; st < steps; st++)
            for (auto& p : bh_parts) {
                double rx = hx - p.x, ry = hy - p.y, r = std::sqrt(rx * rx + ry * ry) + 1e-6;
                double a = 3800.0 / std::pow(std::max(r, Rs * 0.8), 1.5);
                double ux = rx / r, uy = ry / r;
                p.vx += (ux * a - uy * a * 0.55) * h; p.vy += (uy * a + ux * a * 0.55) * h;
                double k = 1 - 0.25 * h; p.vx *= k; p.vy *= k;
                double v = std::sqrt(p.vx * p.vx + p.vy * p.vy);
                if (v > 140) { p.vx *= 140 / v; p.vy *= 140 / v; }
                p.x += p.vx * h; p.y += p.vy * h; p.life += h;
            }
        bh_parts.erase(std::remove_if(bh_parts.begin(), bh_parts.end(), [&](const BHPart& p) {
            double r = std::hypot(p.x - hx, p.y - hy);
            return r < Rs || p.life > 14 || p.x < -20 || p.y < -20 || p.x > cols + 20 || p.y > rows + 20;
        }), bh_parts.end());
    }
    void blackhole_draw_particles(unsigned char* data, int stride, int cols, int rows) const {
        const double hx = cols * 0.5, hy = rows * 0.5, Rs = rows * 0.13;
        RGB hot = hsl_rgb(hue0 + 35, 0.6, 0.88);
        auto put = [&](int x, int y, uint32_t c, double a) {
            if (x < 0 || y < 0 || x >= cols || y >= rows) return;
            uint32_t* px = (uint32_t*)(data + y * stride) + x, o = *px;
            auto ch = [&](int sh) { return (uint32_t)std::lround(((o >> sh) & 255) * (1 - a) + ((c >> sh) & 255) * a) << sh; };
            *px = 0xFF000000u | ch(16) | ch(8) | ch(0);
        };
        if (bh_draw_wins)
            for (const auto& w : bh_wins)
                for (int y = std::max(0, (int)w.y0); y < std::min(rows, (int)w.y1); y++)
                    for (int x = std::max(0, (int)w.x0); x < std::min(cols, (int)w.x1); x++)
                        put(x, y, bh_win_color(w, x + 0.5, y + 0.5, cols, rows), 1);
        for (const auto& p : bh_parts) {
            double r = std::hypot(p.x - hx, p.y - hy);
            double heat = std::clamp((Rs * 3.2 - r) / (Rs * 2.2), 0.0, 1.0);   // біля діри — розпечені
            uint32_t c = p.c;
            if (heat > 0) {
                RGB o{((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0};
                RGB m{o.r + (hot.r - o.r) * heat, o.g + (hot.g - o.g) * heat, o.b + (hot.b - o.b) * heat};
                c = 0xFF000000u | (uint32_t)(m.r * 255) << 16 | (uint32_t)(m.g * 255) << 8 | (uint32_t)(m.b * 255);
            }
            double fade = std::clamp((r - Rs) / (Rs * 0.4), 0.0, 1.0) * std::min(1.0, (double)p.life * 4);
            put((int)p.x, (int)p.y, c, fade);
            // хвіст — спагетифікація: що швидше, то довший
            double v = std::hypot(p.vx, p.vy);
            int tail = std::min(4, (int)(v / 25));
            for (int k = 1; k <= tail; k++)
                put((int)(p.x - p.vx / v * k), (int)(p.y - p.vy / v * k), c, fade * (0.6 - 0.12 * k));
        }
    }
    void blackhole_prepare(double hue, double s, double dim) {
        for (int b = 0; b < 16; b++)
            for (int l = 0; l < 4; l++) bh_bg[b][l] = hsl_rgb(hue + hue_lo * 0.6 + 12 * (b / 15.0), s * 0.55, 0.012 + 0.03 * l);
        bh_star = hsl_rgb(hue + 30, 0.4, 0.9); bh_acc = hsl_rgb(hue, std::max(s, 0.6), 0.62);
        bh_rim = hsl_rgb(hue + 20, 0.5, 0.85); bh_cur = hsl_rgb(hue + 25, 0.8, 0.7);
        bh_pal.resize(12);
        for (int i = 0; i < 12; i++) {
            double k = i / 11.0;
            // зовні — глибокий акцент, всередині — тепліший і майже білий
            RGB c = hsl_rgb(hue + hue_lo + (40 - hue_lo) * std::pow(k, 1.4), std::clamp(s * (1.0 - 0.75 * std::pow(k, 3)), 0.0, 1.0),
                            0.08 + 0.88 * std::pow(k, 1.1));
            bh_pal[i] = 0xFF000000u | (uint32_t)(std::clamp(c.r * dim, 0.0, 1.0) * 255) << 16
                      | (uint32_t)(std::clamp(c.g * dim, 0.0, 1.0) * 255) << 8 | (uint32_t)(std::clamp(c.b * dim, 0.0, 1.0) * 255);
        }
    }
    // швидкі sin/atan2 для «Чорної діри» (точності ~1e-3 вистачає для пікселів, а коштують у ~5 разів менше)
    static double fsin(double x) {
        x = x * 0.15915494309189535; x -= std::floor(x + 0.5);           // → [-0.5, 0.5) обертів
        double y = x * (8 - 16 * std::fabs(x));                           // парабола
        return y * (0.775 + 0.225 * std::fabs(y));                        // уточнення
    }
    static double fcos(double x) { return fsin(x + 1.5707963267948966); }
    static double fatan2(double y, double x) {
        double ax = std::fabs(x), ay = std::fabs(y), mx = std::max(ax, ay);
        if (mx == 0) return 0;
        double a = std::min(ax, ay) / mx, s2 = a * a;
        double r = ((-0.0464964749 * s2 + 0.15931422) * s2 - 0.327622764) * s2 * a + a;
        if (ay > ax) r = 1.5707963267948966 - r;
        if (x < 0) r = 3.141592653589793 - r;
        return y < 0 ? -r : r;
    }
    static double fhyp(double a, double b) { return std::sqrt(a * a + b * b); }
    double swirl_t = 0;
    uint32_t blackhole_px(int x, int y, int cols, int rows, double bay, double hue, double s, double dim) const {
        auto pk = [&](RGB c) {
            return 0xFF000000u | (uint32_t)(std::clamp(c.r * dim, 0.0, 1.0) * 255) << 16
                 | (uint32_t)(std::clamp(c.g * dim, 0.0, 1.0) * 255) << 8 | (uint32_t)(std::clamp(c.b * dim, 0.0, 1.0) * 255);
        };
        auto mix = [](RGB a, RGB b, double k) { return RGB{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k}; };
        const double cx = cols * 0.5, cy = rows * 0.5;
        const double Rs = rows * 0.13;                       // радіус тіні
        double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
        double r = std::sqrt(dx * dx + dy * dy);

        // --- світло диска: прямий диск + лінзоване гало + фотонне кільце ---
        const double inc = 0.15, tilt = -0.07;               // майже ребром, трохи нахилений
        double dyb = dy - tilt * dx, dz = dyb / inc;
        double rd = std::sqrt(dx * dx + dz * dz);
        const double Rin = Rs * 1.3, Rout = Rs * 4.3;
        double I = 0;
        if (rd > Rin && rd < Rout && (r > Rs || dyb > 0)) {  // дальня половина за тінню схована
            double u = (rd - Rin) / (Rout - Rin);
            double ang = fatan2(dz, dx);
            double om = (Rin / rd) * std::sqrt(Rin / rd);              // Кеплер: всередині швидше
            double sw = 0.62 + 0.38 * fsin(rd * 1.25 - t * om * 2.2 + ang * 3) * fsin(rd * 0.45 + ang * 2 - t * om * 1.3);
            double edge = smooth(u / 0.05) * smooth((1 - u) / 0.3);
            I = (1 - u) * std::sqrt(1 - u) * sw * edge * (1 + 0.5 * (-dx / std::max(rd, 1.0))) * 1.25;
        }
        if (r > Rs * 0.97 && r < Rs * 2.1) {                 // дальній бік диска, вигнутий над і під тінню
            double a = fatan2(dy, dx), sa = std::fabs(fsin(a));
            double Rh = Rs * (1.2 + 0.12 * sa), w = Rs * (0.07 + 0.13 * sa);
            double d = (r - Rh) / w;
            double h = std::exp(-d * d) * (0.45 + 0.55 * sa) * (1 + 0.45 * (-fcos(a)));
            I += h * (0.82 + 0.18 * fsin(a * 6 - t * 1.6 + r * 0.7));
        }
        { double d = (r - Rs * 1.02) / 0.8; I += std::exp(-d * d) * 0.95; }   // фотонне кільце

        RGB bg{0, 0, 0};
        bool shadow = r < Rs;
        if (!shadow) {
            // --- фон через точкову лінзу + «закручування» простору біля діри ---
            const double E = Rs * 1.85;
            double k = (r - E * E / r) / r;                   // <0 — зображення з протилежного боку
            double sx = dx * k, sy = dy * k, qx, qy;
            if (r < Rs * 7) {                                  // далі закручування менше пікселя — не рахуємо
                double swirl = 1.1 * (Rs * Rs) / (r * r) + swirl_t * (Rs / r);
                double cs = fcos(swirl), sn = fsin(swirl);
                qx = cx + sx * cs - sy * sn; qy = cy + sx * sn + sy * cs;
            } else { qx = cx + sx; qy = cy + sy; }
            double shift = fhyp(qx - (x + 0.5), qy - (y + 0.5));
            double e2 = E * E / (r * r), mag = std::min(3.0, 1.0 / std::max(0.05, std::fabs(1 - e2 * e2)));

            // туманність — 3 рівні з дизерингом
            double neb = 0.5 + 0.25 * fsin(qx * 0.045 + t * 0.03) * fsin(qy * 0.06 - qx * 0.021)
                       + 0.25 * fsin(qx * 0.013 - qy * 0.029 + 1.3);
            int lv = std::clamp((int)(neb * 3 + bay - 0.5), 0, 3);
            bg = bh_bg[std::clamp((int)(qy * 15 / rows), 0, 15)][lv];   // завжди «нічна», і у світлій темі

            // зорі (сітка в координатах джерела — лінза їх тягне в дуги)
            int ix = (int)std::floor(qx), iy = (int)std::floor(qy);
            uint32_t hs = hash((uint32_t)ix * 73856093u ^ (uint32_t)iy * 19349663u ^ 0xB1ACu);
            if ((hs & 511) < 5) {
                double tw = 0.55 + 0.45 * fsin(t * (0.4 + (hs >> 10) % 17 * 0.12) + (hs >> 16) % 628 * 0.01);
                RGB sc = ((hs >> 20) & 1) ? RGB{0.85, 0.82, 1.0} : bh_star;
                bg = mix(bg, sc, std::min(1.0, tw * (0.55 + 0.25 * mag)));
            }

            // привиди вікон і курсора — лише там, де лінза помітно зсунула картинку
            double gw = std::clamp((shift - 1.5) / 4.0, 0.0, 1.0);
            if (gw > 0) {
                double cd = fhyp(qx - bh_cx, qy - bh_cy);
                if (cd < 3.2) bg = mix(bg, cd < 1.3 ? RGB{1, 0.97, 0.95} : bh_cur, (cd < 1.3 ? 1.0 : 0.5) * gw);
            }
            // тонке гравітаційне «затемнення» перед тінню
            if (r < Rs * 1.5) { double f = (r - Rs) / (Rs * 0.5); bg = mix(RGB{0, 0, 0}, bg, f * f); }
        }

        // світло диска поверх фону — 12 рівнів з дизерингом Байєра
        double q = std::clamp(I, 0.0, 1.35) / 1.35 * 11 + (bay - 0.5);
        int qi = (int)std::floor(q);
        if (qi >= 1 && !bh_pal.empty()) return bh_pal[std::min(qi, 11)];
        if (shadow) return 0xFF000000u;
        return pk(bg);
    }

    // ---- «Неонове місто» — кіберпанк-мегаполіс у піксель-арті (власний дизайн) ----
    // Палітра — різкий жовтий/ціан/червоний на брудно-бірюзовій темряві; смог, підсвічений містом знизу.
    // Статична основа (небо, смог, мегаблоки з поверховими смугами вікон, моноліт-вежа, естакада, ближні
    // громади, дроти, туман, віньєтка) — раз на розмір/тему в ct_base. Щокадру: прожектори в смогу, голограма-
    // емблема, вивіски з вигаданими гліфами, рекламні екрани (4 програми), летючі машини, потоки фар на
    // естакаді й вулиці, мокре дзеркало, косий дощ, зрідка — хроматичний глітч. Акцент — четвертий неон.
    enum { CD_SKY, CD_FAR, CD_MID, CD_HWY, CD_NEAR, CD_ROAD };
    std::vector<uint32_t> ct_base, ct_frame;
    std::vector<uint8_t> ct_dep, ct_vig;
    int ct_w = 0, ct_h = 0, ct_light = -1, ct_ground = 0, ct_refl = 0, ct_hwy = 0;
    // kind: 0 вертикальна вивіска, 1 бігуча, 2 неон-ребро, 3 неон-лінія даху, 4 рекламний екран
    struct CtSign { int x0, y0, w, h, col, depth, kind; uint32_t seed; };
    struct CtAnt { int x, y; float ph; };
    struct CtBeam { int x, y; float ph, sp, base; };
    std::vector<CtSign> ct_signs;
    std::vector<CtAnt> ct_ants;
    std::vector<CtBeam> ct_beams;
    int ct_holo[3] = {0, 0, 0};           // голограма-емблема: cx, cy, r
    static uint32_t ct_pk(RGB c) {
        return 0xFF000000u | (uint32_t)(std::clamp(c.r, 0.0, 1.0) * 255) << 16
             | (uint32_t)(std::clamp(c.g, 0.0, 1.0) * 255) << 8 | (uint32_t)(std::clamp(c.b, 0.0, 1.0) * 255);
    }
    static RGB ct_un(uint32_t v) { return {((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0}; }
    static RGB ct_mix(RGB a, RGB b, double k) { return {a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k}; }
    static bool ct_glyph(uint32_t seed, int gi, int a, int b, int gs) {     // вигадані «ієрогліфи»: дзеркальні + риски
        uint32_t bits = hash(seed * 131 + gi * 17 + 3);
        int sx = std::min(b, gs - 1 - b);
        bool lit = (bits >> ((a * 3 + sx) % 31)) & 1;
        if (a == 0 || a == gs - 1) lit = lit || ((bits >> 30) & 1);
        return lit;
    }
    void ct_blend(int x, int y, RGB c, double a, int maxdep = 9) {
        if (x < 0 || y < 0 || x >= ct_w || y >= ct_refl || a <= 0) return;
        size_t p = (size_t)y * ct_w + x;
        if (ct_dep[p] > maxdep) return;
        ct_frame[p] = ct_pk(ct_mix(ct_un(ct_frame[p]), c, std::min(1.0, a)));
    }

    void city_build(int cols, int rows) {
        static const double BY[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        auto bay = [&](int x, int y) { return (BY[y & 3][x & 3] + 0.5) / 16.0; };
        ct_w = cols; ct_h = rows; ct_light = light;
        size_t N = (size_t)cols * rows;
        ct_base.assign(N, 0xFF000000u); ct_dep.assign(N, 0); ct_vig.assign(N, 255);
        ct_signs.clear(); ct_ants.clear(); ct_beams.clear();
        int G = ct_ground = (int)(rows * 0.84); ct_refl = G + 3;
        int HW = ct_hwy = (int)(rows * 0.665);
        uint32_t sd = 0xC0FFEE77u;                     // фіксоване зерно — те саме місто при кожному запуску
        auto rf = [&]() { sd = hash(sd + 0x9E3779B9u); return (sd & 0xFFFFFF) / double(0x1000000); };
        auto ri = [&](int a, int b) { return a + (int)(rf() * (b - a + 1)); };
        auto put = [&](int x, int y, RGB c, int d) {
            if (x < 0 || y < 0 || x >= cols || y >= rows) return;
            size_t p = (size_t)y * cols + x; ct_base[p] = ct_pk(c); ct_dep[p] = d;
        };
        auto get = [&](int x, int y) { return ct_un(ct_base[(size_t)y * cols + x]); };
        auto dep = [&](int x, int y) { return (x < 0 || y < 0 || x >= cols || y >= rows) ? 99 : (int)ct_dep[(size_t)y * cols + x]; };

        RGB sT, sM, sH, sG, cDark, cLit, fog;
        if (light) { sT = {0.66, 0.66, 0.52}; sM = {0.88, 0.78, 0.45}; sH = {0.98, 0.64, 0.28}; sG = {1.0, 0.76, 0.42};
                     cDark = {0.80, 0.72, 0.50}; cLit = {0.97, 0.78, 0.48}; fog = {1.0, 0.80, 0.50}; }
        else       { sT = {0.012, 0.025, 0.045}; sM = {0.035, 0.13, 0.16}; sH = {0.50, 0.15, 0.11}; sG = {0.90, 0.36, 0.13};
                     cDark = {0.05, 0.10, 0.12}; cLit = {0.55, 0.20, 0.13}; fog = {0.55, 0.22, 0.14}; }
        // --- небо: бірюзова темрява → брудно-червоний смог → розпечене світло міста над обрієм ---
        for (int y = 0; y < ct_refl; y++) {
            const int NB = 14;
            double band = std::clamp((double)y / G, 0.0, 1.0) * (NB - 1);
            for (int x = 0; x < cols; x++) {
                int b = std::min(NB - 1, (int)band + (bay(x, y) < band - (int)band ? 1 : 0));
                double q = (double)b / (NB - 1);
                RGB c = q < 0.25 ? ct_mix(sT, sM, q / 0.25) : q < 0.5 ? ct_mix(sM, sH, (q - 0.25) / 0.25) : ct_mix(sH, sG, std::min(1.0, (q - 0.5) / 0.3));
                put(x, y, c, CD_SKY);
            }
        }
        // --- смог: дві хмарні смуги, підсвічені знизу ---
        for (int y = 0; y < (int)(rows * 0.5); y++)
            for (int x = 0; x < cols; x++) {
                double n = std::sin(x * 0.043 + y * 0.19) * 0.45 + std::sin(x * 0.107 - y * 0.13 + 1.7) * 0.3
                         + std::sin(x * 0.021 + 2.1) * 0.4 + (hashf(x * 7 + y * 131) - 0.5) * 0.25;
                double band = std::exp(-std::pow((y - rows * 0.10) / (rows * 0.055), 2))
                            + 0.9 * std::exp(-std::pow((y - rows * 0.29) / (rows * 0.065), 2));
                double dens = band * (n * 0.5 + 0.55);
                if (dens > 0.58 + 0.35 * bay(x, y)) {
                    double lk = std::clamp((double)y / (rows * 0.45), 0.0, 1.0);
                    RGB c = ct_mix(cDark, cLit, 0.2 + 0.7 * lk);
                    if (dens < 0.7) c = ct_mix(get(x, y), c, 0.55);                  // рвані краї
                    put(x, y, c, CD_SKY);
                }
            }
        // --- палітра будівель ---
        RGB bFar, bMid, bMega, bNear, bHwy;
        static const RGB WIN[4] = {{1.0, 0.70, 0.32}, {0.70, 0.92, 1.0}, {0.30, 0.90, 1.0}, {1.0, 0.85, 0.45}};
        if (light) { bFar = ct_mix(sH, {0.60, 0.58, 0.55}, 0.5); bMid = {0.40, 0.39, 0.40}; bMega = {0.22, 0.23, 0.27};
                     bNear = {0.15, 0.14, 0.16}; bHwy = {0.47, 0.45, 0.43}; }
        else       { bFar = ct_mix(sH, {0.05, 0.12, 0.14}, 0.72); bMid = {0.04, 0.055, 0.07}; bMega = {0.022, 0.026, 0.04};
                     bNear = {0.012, 0.012, 0.018}; bHwy = {0.075, 0.072, 0.085}; }
        // дальні мегаблоки: щільна стіна у серпанку, поверхові смуги тьмяних вікон
        auto far_block = [&](int x0, int x1, int top) {
            for (int y = std::max(0, top); y < G; y++)
                for (int x = std::max(0, x0); x <= std::min(cols - 1, x1); x++) {
                    RGB c = bFar; int ly = y - top;
                    if (ly == 0) c = ct_mix(c, sG, light ? 0.15 : 0.25);
                    else if (ly % 3 == 1 && x > x0 && x < x1 && hashf((x / 3) * 7919 + y * 104729 + x0) < (light ? 0.12 : 0.3))
                        c = ct_mix(c, light ? RGB{0.9, 0.88, 0.8} : WIN[hash(x / 3 + y) % 4], light ? 0.3 : 0.38);
                    put(x, y, c, CD_FAR);
                }
        };
        for (int x = -4; x < cols;) {
            int w = ri(6, 16), top = (int)(rows * (0.20 + 0.26 * rf()));
            far_block(x, x + w - 1, top);
            if (rf() < 0.4) far_block(x + w / 4, x + w - 1 - w / 4, top - ri(4, 10));
            x += w + ri(-3, 1);
        }
        // середні бруталістські блоки: карнизи кожні 4 ряди, між ними — смуги вікон, скошені кути даху
        auto block = [&](int x0, int x1, int top, bool chamfer) {
            int w = x1 - x0 + 1, ch = chamfer ? std::min(4, w / 4) : 0;
            for (int y = std::max(0, top); y < G; y++) {
                int cut = y - top < ch ? ch - (y - top) : 0;
                for (int x = std::max(0, x0 + cut); x <= std::min(cols - 1, x1 - cut); x++) {
                    RGB c = bMid; int lx = x - x0, ly = y - top;
                    if (ly == 0 || x == x0 + cut || x == x1 - cut) c = ct_mix(c, sG, light ? 0.12 : 0.18);   // підсвічений край
                    else if (ly % 4 == 0) c = ct_mix(c, light ? RGB{0.7, 0.68, 0.62} : RGB{0.10, 0.13, 0.15}, 0.5);  // карниз
                    else if (ly % 4 == 2 && lx >= 1 && x < x1 - cut && lx % 2 == 1) {
                        uint32_t seg = hash((uint32_t)(x0 + 1000) * 131 + (lx / 5) * 977 + (ly / 4) * 31);
                        if (!light && (seg & 255) < 90) c = ct_mix(WIN[(seg >> 8) % 4], c, 0.2 + 0.3 * hashf(x * 13 + y));
                        else if (light && (seg & 255) < 110) c = ct_mix(c, {0.70, 0.80, 0.85}, 0.35);
                        else c = ct_mix(c, light ? sT : RGB{0.10, 0.16, 0.2}, 0.3);
                    }
                    put(x, y, c, CD_MID);
                }
            }
        };
        auto mast = [&](int x, int y0, int h, int d, RGB col) {
            for (int y = y0 - h; y < y0; y++) put(x, y, col, d);
            ct_ants.push_back({x, y0 - h - 1, (float)(rf() * 6.28)});
        };
        for (int x = -6; x < cols;) {
            int w = ri(14, 32), top = (int)(rows * (0.26 + 0.28 * rf()));
            block(x, x + w - 1, top, rf() < 0.45);
            double r = rf();
            if (r < 0.35) mast(x + w / 2 + ri(-3, 3), top, ri(3, 9), CD_MID, ct_mix(bMid, sG, 0.2));
            else if (r < 0.5) ct_signs.push_back({rf() < 0.5 ? x : x + w - 1, top + 3, 1, G - top - 3, ri(0, 2), CD_MID, 2, hash(x + 77)});
            if (rf() < 0.25) ct_beams.push_back({x + w / 2, top - 1, (float)(rf() * 6.28), (float)(0.25 + 0.3 * rf()), (float)(rf() - 0.5) * 0.6f});
            x += w + ri(0, 7);
        }
        // моноліт-вежа корпорації: звужується уступами, вертикальні щілини світла, світлові пояси, шпиль
        {
            int cx = (int)(cols * 0.43), hw0 = std::max(16, cols / 11), top = (int)(rows * 0.035);
            for (int y = top; y < G; y++) {
                double f = (double)(y - top) / (G - top);
                int hw = (int)(hw0 * (0.42 + 0.58 * std::floor(f * 7) / 7.0));                 // 7 уступів
                for (int x = cx - hw; x <= cx + hw; x++) {
                    RGB c = bMega;
                    int lx = x - cx;
                    if (std::abs(lx) == hw) c = ct_mix(c, sG, light ? 0.15 : 0.22);
                    else if (lx % 4 == 0) c = ct_mix(c, light ? RGB{0.55, 0.6, 0.65} : RGB{0.0, 0.75, 0.9}, light ? 0.2 : 0.12 + 0.08 * hashf(y / 3 + x));
                    if ((y - top) % (rows / 9) == rows / 18) c = ct_mix(c, light ? RGB{0.9, 0.85, 0.6} : RGB{1.0, 0.82, 0.2}, 0.55);
                    if (y - top >= 3 && y - top <= 4) c = light ? RGB{0.5, 0.75, 0.8} : RGB{0.0, 0.85, 1.0};   // світний вінець
                    put(x, y, c, CD_MID);
                }
            }
            for (int y = 0; y < top; y++) put(cx, y, ct_mix(bMega, sG, 0.3), CD_MID);
            ct_ants.push_back({cx, 0, 1.0f});
            ct_holo[0] = cx; ct_holo[1] = top + (int)(rows * 0.13); ct_holo[2] = std::max(6, (int)(rows * 0.065));
            ct_beams.push_back({cx - hw0 / 2, top + 2, 0.0f, 0.22f, -0.35f});
            ct_beams.push_back({cx + hw0 / 2, top + 2, 2.0f, 0.18f, 0.35f});
        }
        // ще дві високі вежі по боках — щоб центр не був самотнім
        block((int)(cols * 0.62), (int)(cols * 0.62) + std::max(18, cols / 14), (int)(rows * 0.14), true);
        mast((int)(cols * 0.62) + 6, (int)(rows * 0.14), 7, CD_MID, ct_mix(bMid, sG, 0.2));
        block((int)(cols * 0.24), (int)(cols * 0.24) + std::max(16, cols / 18), (int)(rows * 0.2), false);
        // естакада: перила з ліхтарями, полотно, підсвічений низ, опори
        for (int x = 0; x < cols; x++) {
            put(x, HW - 1, (x % 3 == 0) ? ct_mix(bHwy, sG, 0.35) : bHwy, CD_HWY);
            for (int y = HW; y < HW + 3; y++) put(x, y, y == HW ? ct_mix(bHwy, {0.2, 0.2, 0.22}, 0.5) : bHwy, CD_HWY);
            put(x, HW + 3, ct_mix(bHwy, light ? RGB{0.6, 0.5, 0.4} : RGB{0.55, 0.25, 0.1}, 0.45), CD_HWY);
        }
        for (int x = 8; x < cols; x += 30)
            for (int y = HW + 4; y < G; y++) for (int dx = 0; dx < 3; dx++)
                put(x + dx, y, ct_mix(bHwy, light ? RGB{0.3, 0.3, 0.3} : RGB{0.02, 0.02, 0.03}, dx == 0 ? 0.2 : 0.5), CD_HWY);
        RGB lamp = {1.0, 0.80, 0.40};
        for (int x = 4; x < cols; x += 15) {
            put(x, HW - 2, light ? RGB{0.35, 0.35, 0.36} : RGB{0.12, 0.12, 0.14}, CD_HWY);
            put(x, HW - 3, lamp, CD_HWY);
            if (!light) for (int dy = -2; dy <= 3; dy++) for (int dx = -3; dx <= 3; dx++) {
                int xx = x + dx, yy = HW - 3 + dy;
                if ((dx || dy) && dep(xx, yy) <= CD_HWY && bay(xx, yy) < 0.45 - std::hypot(dx, dy) * 0.1)
                    put(xx, yy, ct_mix(get(xx, yy), lamp, 0.3), dep(xx, yy));
            }
        }
        // ближні громади по краях
        auto near_block = [&](int x0, int x1, int top) {
            for (int y = std::max(0, top); y < G; y++)
                for (int x = std::max(0, x0); x <= std::min(cols - 1, x1); x++) {
                    RGB c = bNear; int lx = x - x0, ly = y - top;
                    if (ly == 0) c = ct_mix(c, sG, 0.2);
                    else if (lx >= 2 && x < x1 - 1 && lx % 6 < 3 && ly >= 3 && ly % 5 == 0) {
                        uint32_t room = hash((uint32_t)(x0 + 1000) * 31 + lx / 6 * 7 + ly * 131);
                        c = (!light && (room & 255) < 55) ? ct_mix(WIN[(room >> 8) % 4], c, 0.45) : ct_mix(c, light ? sT : RGB{0.06, 0.08, 0.1}, 0.4);
                    } else if (ly % 11 == 7 && lx % 9 == 3) c = ct_mix(c, {0.25, 0.25, 0.28}, 0.5);   // кондиціонери
                    put(x, y, c, CD_NEAR);
                }
        };
        int lA = (int)(cols * 0.15), rA = (int)(cols * 0.83), rB0 = (int)(cols * 0.72);
        int tA = (int)(rows * 0.30), tB = (int)(rows * 0.50), tC = (int)(rows * 0.22), tD = (int)(rows * 0.54);
        near_block(-3, lA, tA);
        near_block(lA - 6, (int)(cols * 0.22), tB);
        near_block(rA, cols + 3, tC);
        near_block(rB0, rA + 2, tD);
        mast(lA - 5, tA, 6, CD_NEAR, ct_mix(bNear, sG, 0.2)); mast(rA + 8, tC, 10, CD_NEAR, ct_mix(bNear, sG, 0.2));
        int vs = std::max(5, cols / 60);
        // вивіски на ближніх: тісно, як у справжньому кварталі
        ct_signs.push_back({lA + 1, tA + 4, vs, (int)(rows * 0.19), 0, CD_NEAR, 0, 0x1234});
        ct_signs.push_back({lA + vs + 3, tA + 9, vs - 1, (int)(rows * 0.12), 2, CD_NEAR, 0, 0x2235});
        ct_signs.push_back({4, tA + 6, lA - 9, (int)(rows * 0.09), 0, CD_NEAR, 4, 0x10});
        ct_signs.push_back({lA + 2, tB + 4, (int)(cols * 0.22) - lA - 4, vs, 1, CD_NEAR, 1, 0x2468});
        ct_signs.push_back({4, tA + 6 + (int)(rows * 0.09) + 5, vs, (int)(rows * 0.15), 3, CD_NEAR, 0, 0x7777});
        ct_signs.push_back({rA - vs - 1, tC + 4, vs, tD - tC - 7, 0, CD_NEAR, 0, 0x5679});
        ct_signs.push_back({rA - 2 * vs - 3, tC + 10, vs - 1, (int)(rows * 0.14), 2, CD_NEAR, 0, 0x5681});
        ct_signs.push_back({rA + 4, tC + 16, cols - rA - 8, (int)(rows * 0.10), 0, CD_NEAR, 4, 0x13});
        ct_signs.push_back({rB0 + 2, tD + 4, rA - rB0 - vs - 6, (int)(rows * 0.08), 0, CD_NEAR, 4, 0x12});
        ct_signs.push_back({rB0 + 2, tD + (int)(rows * 0.08) + 7, rA - rB0 - 6, vs, 3, CD_NEAR, 1, 0x1357});
        ct_signs.push_back({rA + 4, tC + 16 + (int)(rows * 0.10) + 5, vs, (int)(rows * 0.16), 1, CD_NEAR, 0, 0x4444});
        // рекламні екрани й вивіски на середніх фасадах — лише там, де вони цілком на стіні
        auto fits = [&](int x0, int y0, int w, int h, int d) {
            for (auto& s : ct_signs)
                if (s.kind != 2 && s.kind != 3 && x0 + w + 2 > s.x0 && x0 - 2 < s.x0 + s.w && y0 + h + 2 > s.y0 && y0 - 2 < s.y0 + s.h) return false;
            for (int yy = y0 - 2; yy <= y0 + h + 1; yy++)
                for (int xx = x0 - 2; xx <= x0 + w + 1; xx++) if (dep(xx, yy) != d) return false;
            return true;
        };
        for (int tries = 0, n = 0; tries < 500 && n < 3; tries++) {
            int w = ri(12, 22), h = ri(7, 12), x0 = ri(2, cols - w - 3), y0 = ri((int)(rows * 0.18), HW - h - 4);
            if (fits(x0, y0, w, h, CD_MID)) { ct_signs.push_back({x0, y0, w, h, ri(0, 3), CD_MID, 4, hash(tries * 7 + 1)}); n++; }
        }
        for (int tries = 0, n = 0; tries < 600 && n < 7; tries++) {
            bool hz = rf() < 0.3;
            int w = hz ? ri(10, 18) : ri(3, 5), h = hz ? ri(4, 5) : ri(8, 18), x0 = ri(2, cols - w - 3), y0 = ri((int)(rows * 0.18), HW - h - 4);
            double r = rf(); int col = r < 0.42 ? 0 : r < 0.66 ? 1 : r < 0.86 ? 2 : 3;     // жовтий — головний
            if (fits(x0, y0, w, h, CD_MID)) { ct_signs.push_back({x0, y0, w, h, col, CD_MID, hz ? 1 : 0, hash(tries * 91 + 5)}); n++; }
        }
        // дроти, що провисають через передній план, з ліхтариками
        for (int k = 0; k < 2; k++) {
            double ax = cols * 0.15, ay = rows * 0.31 + k * 3, bx = cols * 0.83, by = rows * 0.23 + k * 4, sag = rows * (0.07 + 0.035 * k);
            for (int x = (int)ax; x <= (int)bx; x++) {
                double u = (x - ax) / (bx - ax);
                int y = (int)std::lround(ay + (by - ay) * u + sag * 4 * u * (1 - u));
                if (dep(x, y) < CD_NEAR) put(x, y, light ? RGB{0.12, 0.11, 0.12} : RGB{0.01, 0.01, 0.015}, CD_NEAR);
                if (k == 1 && x % 23 == 11) { put(x, y + 1, light ? RGB{0.2, 0.2, 0.2} : RGB{0.02, 0.02, 0.02}, CD_NEAR);
                                              ct_signs.push_back({x, y + 2, 1, 1, (x / 23) % 3, CD_NEAR, 3, (uint32_t)x}); }
            }
        }
        // туман біля підніжжя (і під естакадою)
        int f0 = G - (int)(rows * 0.2);
        for (int y = std::max(0, f0); y < G; y++) {
            double f = (double)(y - f0) / (G - f0);
            for (int x = 0; x < cols; x++) {
                int d = dep(x, y);
                if (d >= CD_FAR && d <= CD_HWY && bay(x, y) < f) put(x, y, ct_mix(get(x, y), fog, d == CD_FAR ? 0.3 : 0.12), d);
            }
        }
        // вулиця
        for (int y = G; y < ct_refl; y++)
            for (int x = 0; x < cols; x++)
                put(x, y, y == G ? (light ? RGB{0.3, 0.3, 0.33} : RGB{0.10, 0.09, 0.11}) : (light ? RGB{0.22, 0.22, 0.25} : RGB{0.03, 0.03, 0.04}), CD_ROAD);
        // віньєтка (множник яскравості, з дизером)
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                double vx = (double)x / cols - 0.5, vy = (double)y / rows - 0.45;
                double v = std::clamp((vx * vx * 1.2 + vy * vy * 1.4 - 0.16) * 2.0, 0.0, 1.0);
                ct_vig[(size_t)y * cols + x] = (uint8_t)(255 * (1 - (bay(x, y) < v ? 0.22 : 0.0) * (light ? 0.4 : 1)));
            }
    }

    void city_prepare(int cols, int rows, double hue, double /*dim*/) {
        static const double BY[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        if (cols != ct_w || rows != ct_h || (int)light != ct_light) city_build(cols, rows);
        ct_frame = ct_base;
        int G = ct_ground, R0 = ct_refl, HW = ct_hwy;
        double gk = light ? 0.45 : 1.0;                 // вдень неон світить слабше
        const RGB WHITE = {1, 1, 1}, DARK = {0.03, 0.025, 0.04};
        RGB neon[4] = {{1.0, 0.92, 0.04}, {0.0, 0.90, 1.0}, {1.0, 0.04, 0.24}, hsl_rgb(hue, 0.95, 0.58)};
        // прожектори, що гойдаються в смогу (лише над небом і дальніми блоками)
        for (auto& b : ct_beams) {
            double a = b.base + 0.45 * std::sin(t * b.sp + b.ph);
            double dx = std::sin(a), dy = -std::cos(a), len = rows * 1.1;
            RGB bc = light ? RGB{1, 1, 0.92} : RGB{0.75, 0.95, 1.0};
            for (int y = 0; y < b.y; y++)
                for (int x = 0; x < cols; x++) {
                    double vx = x + 0.5 - b.x, vy = y + 0.5 - b.y;
                    double along = vx * dx + vy * dy;
                    if (along <= 0) continue;
                    double perp = std::fabs(vx * dy - vy * dx), wdt = 0.6 + along * 0.05;
                    if (perp >= wdt) continue;
                    double k = (1 - perp / wdt) * (1 - along / len);
                    if (k > 0) ct_blend(x, y, bc, k * (light ? 0.12 : 0.2), CD_FAR);
                }
        }
        // далекі потоки машин між вежами
        for (int ln = 0; ln < 2; ln++) {
            int y = (int)(rows * (0.40 + 0.08 * ln));
            double sp = ln ? -7.0 : 6.0;
            for (int x = 0; x < cols; x++) {
                double u = x - t * sp; long c = (long)std::floor(u / 5);
                if (u - c * 5 < 1 && (hash((uint32_t)c * 31 + ln) & 3) == 0) ct_blend(x, y, ln ? RGB{1, 0.2, 0.25} : RGB{1, 0.95, 0.8}, 0.85, CD_FAR);
            }
        }
        // антени
        for (auto& a : ct_ants)
            if (std::sin(t * 2.2 + a.ph) > 0.4) {
                RGB r = {1, 0.1, 0.15};
                ct_blend(a.x, a.y, r, 1);
                ct_blend(a.x - 1, a.y, r, 0.3 * gk); ct_blend(a.x + 1, a.y, r, 0.3 * gk); ct_blend(a.x, a.y - 1, r, 0.3 * gk);
            }
        // голограма-емблема: обертове кільце + вигаданий знак, напівпрозора, з мерехтінням рядків
        {
            int cx = ct_holo[0], cy = ct_holo[1], R = ct_holo[2];
            double ca = std::cos(t * 0.7), rx = R * std::max(0.08, std::fabs(ca));
            RGB hc = (long)std::floor(t / 8) & 1 ? neon[1] : neon[0];
            for (int y = cy - R - 1; y <= cy + R + 1; y++) {
                if (hashf((uint32_t)y * 7 + (uint32_t)(long)std::floor(t * 10)) < 0.08) continue;   // «зламані» рядки
                for (int x = cx - R - 1; x <= cx + R + 1; x++) {
                    double ex = (x + 0.5 - cx) / rx, ey = (y + 0.5 - cy) / R, d = std::sqrt(ex * ex + ey * ey);
                    double ring = std::fabs(d - 1) * R;
                    double a = 0;
                    if (ring < 0.9) a = 0.75;
                    else if (ring < 1.8) a = 0.2;
                    // знак: шестикутник із косою рискою, сплющується разом з кільцем
                    double ix = (x + 0.5 - cx) / std::max(0.5, std::fabs(ca)), iy = y + 0.5 - cy, r2 = R * 0.55;
                    double hex = std::max(std::fabs(iy) * 1.15 + std::fabs(ix) * 0.6, std::fabs(ix) * 1.15);
                    if (std::fabs(hex - r2) < 0.8 || (std::fabs(ix + iy * 0.8) < 0.7 && hex < r2)) a = std::max(a, 0.8);
                    else if (hex < r2) a = std::max(a, 0.12);
                    if (a > 0) ct_blend(x, y, ct_mix(hc, WHITE, a > 0.7 ? 0.25 : 0), a * (light ? 0.75 : 1), CD_MID);
                }
            }
        }
        // вивіски та екрани
        for (auto& s : ct_signs) {
            double on = 1;
            if ((s.seed & 3) == 0) {                     // кожна четверта — несправна, мерехтить
                double h = hashf((uint32_t)(long)std::floor(t * 9) * 7 + s.seed);
                on = h < 0.1 ? 0.15 : h < 0.18 ? 0.6 : 1;
            }
            RGB c = neon[s.col], cc = ct_mix(ct_mix(c, DARK, 0.75), c, on);
            double ga = on * gk;
            if (s.kind == 2 || s.kind == 3) {            // неонові ребра, лінії, ліхтарики на дротах
                for (int y = s.y0; y < s.y0 + s.h; y++)
                    for (int x = s.x0; x < s.x0 + s.w; x++) {
                        if (x < 0 || x >= cols || y < 0 || y >= G) continue;
                        if (s.kind == 2 && ct_dep[(size_t)y * cols + x] != s.depth) continue;
                        ct_blend(x, y, cc, 0.95, s.depth);
                        ct_blend(x - 1, y, c, 0.22 * ga, s.depth); ct_blend(x + 1, y, c, 0.22 * ga, s.depth);
                        if (s.kind == 3) { ct_blend(x, y - 1, c, 0.22 * ga, s.depth); ct_blend(x, y + 1, c, 0.22 * ga, s.depth); }
                    }
                continue;
            }
            RGB halo = c;
            if (s.kind == 4) {
                int prog = s.seed % 4;
                halo = prog == 0 ? neon[0] : prog == 1 ? neon[2] : prog == 2 ? neon[1] : neon[3];
                if (hashf((uint32_t)(long)std::floor(t * 12) * 3 + s.seed) < 0.025) on = 0.1;   // екран «смикається»
                ga = on * gk;
            }
            for (int y = s.y0 - 2; y < s.y0 + s.h + 2; y++)       // ореол
                for (int x = s.x0 - 2; x < s.x0 + s.w + 2; x++) {
                    int o = std::max(std::max(s.x0 - x, x - (s.x0 + s.w - 1)), std::max(s.y0 - y, y - (s.y0 + s.h - 1)));
                    if (o > 0) ct_blend(x, y, halo, (o == 1 ? 0.28 : 0.08) * ga, s.depth);
                }
            if (s.kind == 4) {                           // рекламний екран
                int prog = s.seed % 4, iw = s.w - 2, ih = s.h - 2;
                long ph = (long)std::floor(t / 2.5);
                for (int y = 0; y < s.h; y++)
                    for (int x = 0; x < s.w; x++) {
                        int X = s.x0 + x, Y = s.y0 + y;
                        if (x == 0 || y == 0 || x == s.w - 1 || y == s.h - 1) { ct_blend(X, Y, light ? RGB{0.2, 0.2, 0.22} : RGB{0.1, 0.1, 0.12}, 1, 9); continue; }
                        int ix = x - 1, iy = y - 1;
                        RGB p;
                        if (prog == 0) {                 // жовто-чорні смуги «hazard» + чорна плашка з бігучими гліфами
                            bool band = iy >= ih / 3 && iy < ih - ih / 3;
                            if (!band) p = ((ix + iy + (long)std::floor(t * 6)) / 2) % 2 ? neon[0] : DARK;
                            else {
                                int gs = std::max(2, ih - 2 * (ih / 3) - 1), a = ix + (int)std::floor(t * 5), b = iy - ih / 3;
                                p = (a % (gs + 1) < gs && b < gs && ct_glyph(s.seed, a / (gs + 1), a % (gs + 1), b, gs)) ? neon[0] : DARK;
                            }
                        } else if (prog == 1) {          // червоний пульс із великим гліфом
                            double pu = 0.55 + 0.45 * std::fabs(std::sin(t * 2.6));
                            int gs = std::min(iw, ih), gx = ix - (iw - gs) / 2;
                            bool lit = gx >= 0 && gx < gs && ct_glyph(s.seed + ph, 0, iy * 5 / std::max(1, ih), gx * 5 / std::max(1, gs), 5);
                            p = lit ? WHITE : ct_mix(DARK, neon[2], pu * (iy % 2 ? 0.8 : 1));
                        } else if (prog == 2) {          // «еквалайзер» ціан → акцент
                            double lv = 0.5 + 0.4 * std::sin(t * 4 + ix * 0.8 + s.seed) * std::sin(t * 1.3 + ix * 0.3);
                            bool lit = iy >= ih * (1 - lv);
                            p = lit ? ct_mix(neon[1], neon[3], (double)iy / std::max(1, ih)) : RGB{0.02, 0.05, 0.08};
                        } else {                         // текст, що прокручується вгору на акценті
                            int row = iy + (int)std::floor(t * 4);
                            bool lit = row % 3 != 2 && (hash((uint32_t)(row / 3) * 131 + ix / 2 + s.seed) & 3) != 0 && ix > 0 && ix < iw - 1;
                            p = lit ? ct_mix(neon[3], WHITE, 0.6) : ct_mix(DARK, neon[3], 0.35);
                        }
                        ct_blend(X, Y, ct_mix(DARK, p, on), 1, 9);
                    }
                continue;
            }
            int iw = s.w - 2, ih = s.h - 2;
            int gs = s.kind == 0 ? iw : ih;
            int ng = std::max(1, (s.kind == 0 ? ih : iw) / (gs + 1));
            double seq = std::fmod(t * 1.6 + (s.seed % 13), ng + 3.0);
            long scroll = s.kind == 1 ? (long)std::floor(t * 7) : 0;
            for (int y = 0; y < s.h; y++)
                for (int x = 0; x < s.w; x++) {
                    int X = s.x0 + x, Y = s.y0 + y;
                    if (x == 0 || y == 0 || x == s.w - 1 || y == s.h - 1) { ct_blend(X, Y, cc, 1, 9); continue; }
                    int a = s.kind == 0 ? y - 1 : x - 1 + (int)scroll, b = s.kind == 0 ? x - 1 : y - 1;
                    int gi = a / (gs + 1), ga2 = a % (gs + 1);
                    bool lit = ga2 < gs && gs >= 2 && ct_glyph(s.seed, gi, ga2, b, gs);
                    if (s.kind == 0 && (s.seed & 4) && gi > seq) lit = false;
                    ct_blend(X, Y, lit ? ct_mix(cc, WHITE, 0.3 * on) : ct_mix(DARK, c, 0.12 * on), 1, 9);
                }
        }
        // летючі машини: нижчі лінії — ближчі й більші; частина за вежами, дві — перед середніми блоками
        RGB carb = light ? RGB{0.22, 0.22, 0.26} : RGB{0.09, 0.085, 0.11};
        for (int ln = 0; ln < 8; ln++) {
            double ly = rows * (0.12 + 0.065 * ln);
            int dir = (ln & 1) ? -1 : 1, len = ln >= 5 ? 7 : ln >= 3 ? 5 : 4;
            int md = ln % 3 == 0 ? CD_FAR : ln >= 6 ? CD_HWY : CD_MID;
            double sp = (ln >= 5 ? 11 : 5) + 4 * hashf(ln * 11 + 2), span = cols + 40;
            for (int k = 0; k < 2; k++) {
                double p = std::fmod(t * sp + hashf(ln * 37 + k * 101) * span, span) - 20;
                int x0 = dir > 0 ? (int)p : cols - 1 - (int)p;
                int y0 = (int)(ly + 1.5 * std::sin(t * 0.8 + ln + k * 2));
                for (int i = 0; i < len; i++) {
                    int x = x0 - dir * i;
                    if (i >= 1 && i <= len - 2) ct_blend(x, y0, i == 1 ? RGB{0.35, 0.6, 0.75} : carb, 1, md);
                    ct_blend(x, y0 + 1, carb, 1, md);
                    if (i >= 1 && i <= len - 2) ct_blend(x, y0 + 2, neon[(ln + k) % 4], 0.35 * gk, md);
                }
                ct_blend(x0, y0 + 1, {1, 0.97, 0.8}, 1, md);
                for (int b = 1; b <= 4; b++) ct_blend(x0 + dir * b, y0 + 1, {1, 0.95, 0.7}, (0.6 - b * 0.13) * gk, md);
                int tx = x0 - dir * (len - 1);
                ct_blend(tx, y0 + 1, {1, 0.05, 0.12}, 1, md);
                ct_blend(tx - dir, y0 + 1, {1, 0.05, 0.12}, 0.3 * gk, md);
            }
        }
        // потоки фар: на естакаді й на вулиці
        auto stream = [&](int y, double sp, int per, RGB col, uint32_t salt, int d) {
            for (int x = 0; x < cols; x++) {
                double u = x - t * sp; long c = (long)std::floor(u / per);
                if (u - c * per < 2 && (hash((uint32_t)c * 7 + salt) & 3)) {
                    size_t p = (size_t)y * cols + x;
                    if (ct_dep[p] == d) ct_blend(x, y, col, 0.95, d);
                }
            }
        };
        stream(HW + 1, 24, 8, {1, 0.08, 0.15}, 1, CD_HWY);
        stream(HW + 2, -29, 10, {1, 0.95, 0.75}, 5, CD_HWY);
        stream(G + 1, 18, 11, {1, 0.08, 0.15}, 9, CD_ROAD);
        stream(G + 2, -22, 13, {1, 0.95, 0.75}, 13, CD_ROAD);
        // мокра вулиця: дзеркало з хвилястим зсувом, смуги калюж і сплески дощу
        RGB wet = light ? RGB{0.30, 0.30, 0.33} : RGB{0.015, 0.015, 0.025};
        uint32_t rs = (uint32_t)(long)std::floor(t * 6);
        for (int y = R0; y < rows; y++) {
            double dd = (double)(y - R0) / std::max(1, rows - R0);
            int sy = std::max(0, R0 - 1 - (int)((y - R0) * 1.3));
            for (int x = 0; x < cols; x++) {
                int wob = (int)std::lround(std::sin(y * 1.3 + t * 2.6 + x * 0.05) * (0.5 + 1.8 * dd));
                int sx = std::clamp(x + wob, 0, cols - 1);
                RGB c = ct_mix(wet, ct_un(ct_frame[(size_t)sy * cols + sx]), light ? 0.55 : 0.6 - 0.25 * dd);
                if ((y + x / 7) % 4 == 0) c = ct_mix(c, wet, 0.3);
                uint32_t hs = hash(x * 73856093u ^ y * 19349663u ^ rs * 83492791u);
                if ((hs & 1023) < 3) c = ct_mix(c, {0.7, 0.8, 1.0}, 0.6);
                ct_frame[(size_t)y * cols + x] = ct_pk(c);
            }
        }
        // косий дощ
        RGB rc = light ? RGB{0.55, 0.6, 0.65} : RGB{0.5, 0.62, 0.75};
        double ra = light ? 0.28 : 0.34;
        for (int y = 0; y < R0; y++)
            for (int x = 0; x < cols; x++) {
                uint32_t lane = x + y / 3, h = hash(lane * 2654435761u + 77);
                if (h % 5) continue;
                double sp = 45 + h % 30, len = 3 + (h >> 8) % 4, span = rows + len + (h >> 12) % 40;
                double d = std::fmod(t * sp + hashf(lane * 31 + 9) * span, span) - y;
                if (d >= 0 && d < len) ct_blend(x, y, rc, ra * (1 - d / len));
            }
        // глітч: раз на ~7 с на чверть секунди смуга рядків зсувається з розщепленням каналів
        {
            long slot = (long)std::floor(t / 7);
            double ph = t - slot * 7 - 2 - 3 * hashf((uint32_t)slot * 13 + 1);
            if (ph > 0 && ph < 0.25) {
                int y0 = (int)(hashf((uint32_t)slot * 7 + 3) * rows * 0.8), hh = 2 + (int)(hashf((uint32_t)slot * 5) * rows * 0.12);
                std::vector<uint32_t> line(cols);
                uint32_t fs = (uint32_t)(long)std::floor(t * 30);
                for (int y = y0; y < std::min(rows, y0 + hh); y++) {
                    int off = (int)(hashf((uint32_t)y * 31 + fs) * 7) - 3;
                    uint32_t* row = &ct_frame[(size_t)y * cols];
                    std::copy(row, row + cols, line.begin());
                    for (int x = 0; x < cols; x++) {
                        uint32_t a = line[std::clamp(x - off, 0, cols - 1)], r = line[std::clamp(x - off - 2, 0, cols - 1)];
                        row[x] = (a & 0xFF00FFFFu) | (r & 0x00FF0000u);
                    }
                }
            }
        }
        (void)BY;
    }
    uint32_t city_px(int x, int y, double dim) const {
        size_t p = (size_t)y * ct_w + x;
        uint32_t v = ct_frame[p];
        double k = dim * ct_vig[p] / 255.0;
        if (k > 0.999) return v;
        RGB c = ct_un(v);
        return ct_pk({c.r * k, c.g * k, c.b * k});
    }

    // ---- «Дах» — мінімалістична нічна сцена (власний персонаж) ----
    // Дах хмарочоса з антенами й кабелями; знизу ліворуч на краю сидить дівчина в худі й навушниках із
    // вушками (силует спиною до глядача з контровим світлом місяця), поруч ноут із логами, що біжать.
    // Праворуч — місто до обрію, збоку від центру — великий цифровий місяць зі скан-лініями в хмарах,
    // довкола дівчини левітують темні частинки. Усе в кількох відтінках акценту, як інші піксельні шпалери.
    std::vector<uint32_t> rt_base, rt_frame;
    std::vector<uint8_t> rt_dep;            // 0 небо, 1 місяць, 2 місто, 3 дах і антени, 4 дівчина/ноут
    int rt_w = 0, rt_h = 0, rt_light = -1, rt_hue = -9999;
    int rt_gx = 0, rt_gy = 0, rt_s = 1, rt_edge = 0, rt_roof = 0, rt_hor = 0;
    int rt_sx = 0, rt_sy = 0, rt_sw = 0, rt_sh = 0;          // екран ноута
    int rt_mx = 0, rt_my = 0, rt_mr = 0;                     // місяць
    struct RtWin { int x, y; float ph; };
    std::vector<RtWin> rt_wins;
    std::vector<CtAnt> rt_ants;
    RGB rt_c[16];
    // 0 небо верх, 1 небо обрій, 2 місяць, 3 скан-лінія місяця, 4 хмара, 5 підсвічений край хмари,
    // 6 дальнє місто, 7 ближнє місто, 8 вікно, 9 дах, 10 край даху, 11 силует, 12 волосся, 13 контрове світло,
    // 14 світлодіод/акцент, 15 світло екрана
    void roof_palette(double H, double s) {
        double lo = hue_lo, hi = hue_hi;
        if (!light) {
            rt_c[0] = hsl_rgb(H + lo - 10, s * 0.55, 0.045); rt_c[1] = hsl_rgb(H + hi, s * 0.6, 0.24);
            rt_c[2] = hsl_rgb(H + hi + 10, 0.5, 0.88);       rt_c[3] = hsl_rgb(H + hi + 10, 0.45, 0.6);
            rt_c[4] = hsl_rgb(H - 5, s * 0.45, 0.10);        rt_c[5] = hsl_rgb(H + hi, s * 0.55, 0.32);
            rt_c[6] = hsl_rgb(H, s * 0.45, 0.13);            rt_c[7] = hsl_rgb(H - 5, s * 0.4, 0.075);
            rt_c[8] = hsl_rgb(H + hi + 15, 0.85, 0.68);      rt_c[9] = hsl_rgb(H - 8, s * 0.3, 0.05);
            rt_c[10] = hsl_rgb(H + hi, s * 0.6, 0.28);       rt_c[11] = hsl_rgb(H - 10, 0.35, 0.03);
            rt_c[12] = hsl_rgb(H - 10, 0.3, 0.06);           rt_c[13] = hsl_rgb(H + hi + 10, 0.7, 0.64);
            rt_c[14] = hsl_rgb(H, 0.95, 0.66);               rt_c[15] = hsl_rgb(H + hi + 20, 0.6, 0.74);
        } else {
            rt_c[0] = hsl_rgb(H + lo, s * 0.35, 0.78);       rt_c[1] = hsl_rgb(H + hi, s * 0.45, 0.93);
            rt_c[2] = hsl_rgb(H + hi, 0.4, 0.99);            rt_c[3] = hsl_rgb(H + hi, 0.35, 0.87);
            rt_c[4] = hsl_rgb(H, s * 0.3, 0.86);             rt_c[5] = hsl_rgb(H + hi, s * 0.3, 0.96);
            rt_c[6] = hsl_rgb(H, s * 0.25, 0.68);            rt_c[7] = hsl_rgb(H - 5, s * 0.25, 0.52);
            rt_c[8] = hsl_rgb(H, 0.55, 0.88);                rt_c[9] = hsl_rgb(H - 8, s * 0.2, 0.30);
            rt_c[10] = hsl_rgb(H + hi, s * 0.3, 0.62);       rt_c[11] = hsl_rgb(H - 10, 0.25, 0.13);
            rt_c[12] = hsl_rgb(H - 10, 0.22, 0.18);          rt_c[13] = hsl_rgb(H + hi, 0.5, 0.80);
            rt_c[14] = hsl_rgb(H, 0.95, 0.55);               rt_c[15] = hsl_rgb(H + hi, 0.4, 0.93);
        }
    }
    void roof_build(int cols, int rows, double hue, double s) {
        static const double BY[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        // дівчина спиною до глядача, дивиться праворуч на місто; ряд 26 — стегна на даху, нижче — ноги за краєм
        static const char* SPR[44] = {"...........K......K...........", "..........KK.....KK...........", "..........KkK...KkK...........", ".........KbbbbbbbbbK..........", "........hhhhhhhhhgghh.........", ".......hhhhhhhhhhhggghr.......", "......hhhhhhhhhhhhhhhhhr......", "......hhhhhhhhhhhhhhhhhr......", "......hhhhhhhhhhhhhhhEEr......", "......hhhhhhhhhhhhhhhELr......", "......hhhhhhhhhhhhhhhELr......", "......hhhhhhhhhhhhhhhEEr......", "......hhhhhhhhhhhhhhhhr.......", "......hhhhhhhhhhhhhhhhr.......", ".......hhhhhhhhhhhhhhr........", ".......hhhhhhhhhhhhhh.........", "......hhhh.hhhhhhhhh..........", ".....HHHHHHhhhhhhHHHH.........", "....HHHDDDDHHHHDDDHHHr........", "...HHHHHHHHHHHHHHHHHHHr.......", "...HHHHHHHHHHHHHHHHHHHHr......", "..HHHHHHHDHHHHHHHHHHHHHr......", "..HHHHHHHDHHHHHHHHHHHHHHr.....", "..HHHHHHHDHHHHHHHHHHHHHHr.....", "..HHHHHHHDHHHHHHHHHHHHHHr.....", "..HHHHHHHDHHHHHHHHHHHHHHHr....", "..HHHHHHHDHHHHHHHHHHHHHHHr....", "..HHHHHHHDHHHHHHHHHHHHHHHr....", "..HHHHHHHDHHHHHHHHHHHHHHHr....", "..HHHHHHHDHHHHHHHHHHHHHHHHr...", "..HHHHHHHDHHHHHHHHHHHHHHHHr...", "...HHHHHHDHHHHHHHHHHHHHHHHr...", "...HHHHHHHHHHHHHHHHHHHPPPPPr..", "...HHHHHHHHHHHHHHHHHPPPPPPPPr.", "...HHHHHHHHHHHHHHHHPPPPPPPPPPr", "....HHHHHHHHHHHHHHPPPPPPPPPPPP", "....PPPPPPPPPPPPPPPPPPPPPPPPPP", ".........................PPPP.", ".........................PPP..", ".........................PPP..", "........................PPP...", "........................PPP...", "........................PPP...", "........................SSSS.."};
        auto bay = [&](int x, int y) { return (BY[y & 3][x & 3] + 0.5) / 16.0; };
        rt_w = cols; rt_h = rows; rt_light = light; rt_hue = (int)std::lround(hue);
        roof_palette(rt_hue, s);
        const RGB* C = rt_c;
        size_t N = (size_t)cols * rows;
        rt_base.assign(N, ct_pk(C[0])); rt_dep.assign(N, 0); rt_wins.clear(); rt_ants.clear();
        auto put = [&](int x, int y, RGB c, int d) {
            if (x < 0 || y < 0 || x >= cols || y >= rows) return;
            size_t p = (size_t)y * cols + x; rt_base[p] = ct_pk(c); rt_dep[p] = d;
        };
        auto get = [&](int x, int y) { return ct_un(rt_base[(size_t)y * cols + x]); };
        uint32_t sd = 0xD00F5EEDu;
        auto rf = [&]() { sd = hash(sd + 0x9E3779B9u); return (sd & 0xFFFFFF) / double(0x1000000); };
        auto ri = [&](int a, int b) { return a + (int)(rf() * (b - a + 1)); };
        int S = rt_s = std::max(1, (int)std::lround(rows / 200.0));   // дівчина ~25% висоти кадру
        int hor = rt_hor = (int)(rows * 0.64);
        // небо: дизерні смуги
        for (int y = 0; y < rows; y++) {
            const int NB = 10;
            double band = std::pow(std::clamp((double)y / hor, 0.0, 1.0), 1.4) * (NB - 1);
            for (int x = 0; x < cols; x++) {
                int b = std::min(NB - 1, (int)band + (bay(x, y) < band - (int)band ? 1 : 0));
                put(x, y, ct_mix(C[0], C[1], (double)b / (NB - 1)), 0);
            }
        }
        // цифровий місяць: скан-лінії, легка тінь, ореол з дизером
        int mx = rt_mx = (int)(cols * 0.63), my = rt_my = (int)(rows * 0.27), mr = rt_mr = std::max(8, (int)(rows * 0.17));
        for (int y = my - mr * 2; y <= my + mr * 2; y++)
            for (int x = mx - mr * 2; x <= mx + mr * 2; x++) {
                if (x < 0 || y < 0 || x >= cols || y >= rows) continue;
                double dx = x + 0.5 - mx, dy = y + 0.5 - my, d = std::sqrt(dx * dx + dy * dy);
                if (d < mr) {
                    RGB c = ((y - my + mr) % 3 == 2) ? C[3] : C[2];
                    if ((dx + dy) / mr > 0.55 && bay(x, y) < ((dx + dy) / mr - 0.55) * 1.5) c = C[3];   // тінь знизу праворуч
                    put(x, y, c, 1);
                } else if (d < mr * 1.7) {
                    double g = 1 - (d - mr) / (mr * 0.7);
                    if (bay(x, y) < g * 0.45) put(x, y, ct_mix(get(x, y), C[2], light ? 0.25 : 0.12), 0);
                }
            }
        // місто: дальній шар до обрію і ближній, що йде донизу (ми на даху — місто під нами)
        for (int x = (int)(cols * 0.22); x < cols;) {
            int w = ri(4, 10), top = hor - (int)(rows * (0.03 + 0.11 * rf()));
            for (int y = top; y < rows; y++)
                for (int xx = x; xx < std::min(cols, x + w); xx++) {
                    RGB c = C[6];
                    if (y == top) c = ct_mix(c, C[1], 0.35);
                    else if ((xx - x) % 2 == 1 && (y - top) % 3 == 1 && hashf(xx * 131 + y * 977) < 0.07) c = ct_mix(c, C[8], 0.5);
                    put(xx, y, c, 2);
                }
            x += w + ri(0, 2);
        }
        for (int x = (int)(cols * 0.26); x < cols;) {
            int w = ri(8, 18), top = hor - (int)(rows * 0.10) + ri(0, (int)(rows * 0.18));
            for (int y = top; y < rows; y++)
                for (int xx = x; xx < std::min(cols, x + w); xx++) {
                    RGB c = C[7];
                    int lx = xx - x, ly = y - top;
                    if (ly == 0) c = ct_mix(c, C[1], 0.3);
                    else if (lx % 3 == 1 && ly % 3 == 1 && lx < w - 1) {
                        double h = hashf(xx * 7919 + y * 104729);
                        if (h < 0.025) rt_wins.push_back({xx, y, (float)(h * 2513)});      // ці мигають
                        else if (h < 0.1) c = ct_mix(c, C[8], 0.75);
                    }
                    double fd = std::clamp((double)(y - hor) / (rows - hor), 0.0, 1.0);   // глибина: донизу темніше
                    if (bay(xx, y) < fd * 0.8) c = ct_mix(c, C[light ? 7 : 11], 0.5);
                    put(xx, y, c, 2);
                }
            x += w + ri(1, 4);
        }
        // дах, на краю якого сидить дівчина
        int roof = rt_roof = (int)(rows * 0.80);
        rt_gy = roof - 37 * S; rt_gx = (int)(cols * 0.15); int edge = rt_edge = rt_gx + 25 * S;
        for (int y = roof; y < rows; y++)
            for (int x = 0; x < edge; x++) {
                RGB c = C[9];
                if (y == roof) c = C[10];
                else if (y == roof + 1) c = ct_mix(C[9], C[10], 0.3);
                else if (x == edge - 1) c = ct_mix(C[9], C[10], 0.45);
                put(x, y, c, 3);
            }
        // кондиціонер на даху
        {
            int ax = (int)(cols * 0.025), aw = 9 * S, ah = 5 * S;
            for (int y = roof - ah; y < roof; y++)
                for (int x = ax; x < ax + aw; x++) put(x, y, y == roof - ah ? C[10] : ((x - ax) % (2 * S) == 0 ? ct_mix(C[9], C[10], 0.2) : C[9]), 3);
        }
        // антени з перекладинами і кабелі
        auto mast = [&](int x, int h) {
            for (int y = roof - h; y < roof; y++) put(x, y, C[11], 3);
            for (int k = 1; k <= 3; k++) {
                int y = roof - h + h * k / 5, hw = 3 - k / 2;
                for (int dx = -hw; dx <= hw; dx++) put(x + dx, y, C[11], 3);
            }
            rt_ants.push_back({x, roof - h - 1, (float)(rf() * 6.28)});
        };
        auto cable = [&](double x0, double y0, double x1, double y1, double sag) {
            int a = (int)std::floor(std::min(x0, x1)), b = (int)std::ceil(std::max(x0, x1));
            int py = -1;
            for (int x = a; x <= b; x++) {
                double u = (x - x0) / (x1 - x0);
                int y = (int)std::lround(y0 + (y1 - y0) * u + sag * 4 * u * (1 - u));
                int ys = py < 0 ? y : std::min(y, py + 1), ye = py < 0 ? y : std::max(y, py - 1);
                for (int yy = std::min(ys, ye); yy <= std::max(ys, ye); yy++)
                    if (yy >= 0 && yy < rows && x >= 0 && x < cols && rt_dep[(size_t)yy * cols + x] < 3) put(x, yy, C[11], 3);
                py = y;
            }
        };
        int m1 = (int)(cols * 0.05), h1 = (int)(rows * 0.42), m2 = (int)(cols * 0.105), h2 = (int)(rows * 0.30);
        mast(m1, h1); mast(m2, h2);
        cable(m1, roof - h1, -2, rows * 0.08, rows * 0.04);
        cable(m1, roof - h1, m2, roof - h2, rows * 0.05);
        cable(m2, roof - h2, cols * 0.5, -1, rows * 0.10);
        cable(m1, roof - h1 * 2 / 3, edge - 2, roof, rows * 0.05);
        // ноут ліворуч від дівчини: основа і відкритий екран
        rt_sw = 12 * S; rt_sh = 8 * S; rt_sx = rt_gx - 13 * S; rt_sy = roof - S - rt_sh;
        for (int y = roof - S; y < roof; y++) for (int x = rt_gx - 14 * S; x < rt_gx + S; x++) put(x, y, ct_mix(C[11], C[10], 0.35), 4);
        for (int y = rt_sy; y < rt_sy + rt_sh; y++) for (int x = rt_sx; x < rt_sx + rt_sw; x++) put(x, y, C[11], 4);
        // світло екрана на даху (дизер)
        int scx = rt_sx + rt_sw / 2, scy = rt_sy + rt_sh / 2, R = 14 * S;
        for (int y = scy - R; y <= scy + R; y++)
            for (int x = scx - R; x <= scx + R; x++) {
                if (x < 0 || y < 0 || x >= cols || y >= rows) continue;
                size_t p = (size_t)y * cols + x;
                if (rt_dep[p] != 3) continue;
                double d = std::hypot(x - scx, (y - scy) * 1.4) / R;
                if (d < 1 && bay(x, y) < (1 - d) * 0.8) put(x, y, ct_mix(get(x, y), C[15], light ? 0.15 : 0.22), 3);
            }
        // дівчина
        for (int r = 0; r < 44; r++)
            for (int c = 0; c < 30; c++) {
                char ch = SPR[r][c];
                if (ch == '.') continue;
                RGB col;
                switch (ch) {
                    case 'K': case 'b': col = ct_mix(C[11], C[12], 0.6); break;
                    case 'k': col = ct_mix(C[11], C[14], 0.45); break;
                    case 'h': col = C[12]; break;
                    case 'E': col = ct_mix(C[11], C[13], 0.22); break;
                    case 'L': col = C[14]; break;
                    case 'r': col = C[13]; break;
                    case 'S': col = C[12]; break;
                    case 'D': col = ct_mix(C[11], {0, 0, 0}, 0.5); break;
                    case 'g': col = ct_mix(C[12], C[13], 0.3); break;
                    case 'P': col = ct_mix(C[11], {0, 0, 0}, 0.2); break;
                    default: col = C[11];
                }
                for (int dy = 0; dy < S; dy++) for (int dx = 0; dx < S; dx++) put(rt_gx + c * S + dx, rt_gy + r * S + dy, col, 4);
            }
    }
    void roof_prepare(int cols, int rows, double hue, double s) {
        if (cols != rt_w || rows != rt_h || (int)light != rt_light || std::lround(hue) != rt_hue) roof_build(cols, rows, hue, s);
        rt_frame = rt_base;
        const RGB* C = rt_c;
        int S = rt_s, hor = rt_hor;
        auto bl = [&](int x, int y, RGB c, double a, int maxdep) {
            if (x < 0 || y < 0 || x >= cols || y >= rows || a <= 0) return;
            size_t p = (size_t)y * cols + x;
            if (rt_dep[p] > maxdep) return;
            rt_frame[p] = ct_pk(ct_mix(ct_un(rt_frame[p]), c, std::min(1.0, a)));
        };
        // зорі
        if (!light)
            for (int y = 0; y < hor * 3 / 4; y++)
                for (int x = 0; x < cols; x++) {
                    uint32_t hs = hash(x * 73856093u ^ y * 19349663u);
                    if ((hs & 1023) >= 5) continue;
                    double tw = 0.5 + 0.5 * std::sin(t * (0.6 + (hs >> 10) % 20 * 0.08) + (hs >> 16) % 628 * 0.01);
                    bl(x, y, C[2], (1 - y / (hor * 0.75)) * (0.25 + 0.75 * tw), 0);
                }
        // хмари: пігулки, що повільно пливуть; край, ближчий до місяця, підсвічений
        for (int k = 0; k < 3; k++) {
            static const double CY[3] = {-0.15, 0.5, -1.2};     // відносно місяця: через центр, по нижній третині, високо
            double cy = rt_my + rt_mr * CY[k], cw = cols * (0.22 + 0.08 * hashf(k * 5 + 1)), chh = (k == 2 ? 1.5 : 2.6 + 0.8 * k) * S;
            double span = cols + cw * 1.2;
            double cx = std::fmod(cols * (0.45 + 0.4 * k) + t * (0.3 + 0.15 * k) * S, span) - cw * 0.6;
            for (int y = (int)(cy - chh - 2); y <= (int)(cy + chh + 2); y++)
                for (int x = (int)(cx - cw * 0.55); x <= (int)(cx + cw * 0.55); x++) {
                    double ex = std::max(0.0, std::fabs(x - cx) - cw * 0.32) / (cw * 0.18);
                    double ey = (y - cy) / (chh + 0.6 * std::sin(x * 0.21 / S + k * 2));
                    if (ex * ex + ey * ey >= 1) continue;
                    double mp = std::clamp(1 - std::hypot(x - rt_mx, y - rt_my) / (rt_mr * 2.2), 0.0, 1.0);
                    RGB c = ey < -0.45 ? ct_mix(C[4], C[5], 0.3 + 0.7 * mp) : C[4];
                    bl(x, y, c, 1, 1);
                }
        }
        // глітч місяця: раз на ~6 с смуга рядків зсувається
        {
            long slot = (long)std::floor(t / 6);
            double ph = t - slot * 6 - 1 - 3 * hashf((uint32_t)slot * 13 + 1);
            if (ph > 0 && ph < 0.3) {
                int y0 = rt_my - rt_mr + (int)(hashf((uint32_t)slot * 7) * rt_mr * 1.6), hh = (2 + (int)(hashf((uint32_t)slot * 3) * 4)) * S;
                int off = ((int)(hashf((uint32_t)(long)std::floor(t * 20)) * 5) - 2) * S;
                std::vector<uint32_t> line(cols);
                for (int y = std::max(0, y0); y < std::min(rows, y0 + hh); y++) {
                    uint32_t* row = &rt_frame[(size_t)y * cols];
                    std::copy(row, row + cols, line.begin());
                    for (int x = std::max(0, rt_mx - rt_mr - 6); x < std::min(cols, rt_mx + rt_mr + 6); x++) {
                        if (rt_dep[(size_t)y * cols + x] > 1) continue;
                        int sx = std::clamp(x - off, 0, cols - 1);
                        if (rt_dep[(size_t)y * cols + sx] <= 1) row[x] = line[sx];
                    }
                }
            }
        }
        // вікна, що мигають
        for (auto& w : rt_wins)
            if (std::sin(t * 0.35 + w.ph) > 0.1) bl(w.x, w.y, C[8], 0.8, 2);
        // вогні антен
        for (auto& a : rt_ants)
            if (std::sin(t * 1.6 + a.ph) > 0.3) {
                bl(a.x, a.y, C[14], 1, 9);
                bl(a.x - 1, a.y, C[14], 0.3, 3); bl(a.x + 1, a.y, C[14], 0.3, 3); bl(a.x, a.y - 1, C[14], 0.3, 3);
            }
        // логи на екрані ноута
        {
            int ix0 = rt_sx + S, iy0 = rt_sy + S, iw = rt_sw - 2 * S, ih = rt_sh - 2 * S;
            long scroll = (long)std::floor(t * 2.5);
            RGB bg = ct_mix(C[11], C[15], 0.12);
            for (int y = 0; y < ih; y++) {
                long row = (y / S) + scroll;
                bool line = row % 2 == 0;
                uint32_t h = hash((uint32_t)(row / 2) * 2654435761u + 7);
                int len = 2 + (int)(h % std::max(1, iw / S - 2));
                bool last = y / S >= ih / S - 1;
                for (int x = 0; x < iw; x++) {
                    RGB c = bg;
                    if (line && x / S < len) c = (h >> 9) % 5 == 0 ? C[14] : C[15];
                    if (last && x / S == 1 && std::fmod(t, 1.0) < 0.5) c = C[15];      // курсор
                    bl(ix0 + x, iy0 + y, c, 1, 9);
                }
            }
        }
        // світлодіод навушників «дихає»
        {
            double pu = 0.5 + 0.5 * std::sin(t * 1.8);
            for (int dy = 0; dy < S; dy++) for (int dx = 0; dx < S; dx++)
                bl(rt_gx + 22 * S + dx, rt_gy + 9 * S + dy, ct_mix(C[14], {1, 1, 1}, 0.45 * pu), 1, 9);
        }
        // темні частинки левітують довкола
        {
            double cx = rt_gx + 14 * S, cy = rt_gy + 17 * S;
            for (int i = 0; i < 9; i++) {
                double ang = hashf(i * 17 + 3) * 6.283 + t * (0.08 + 0.05 * hashf(i)) * ((i & 1) ? 1 : -1);
                double R = (15 + 14 * hashf(i * 3 + 1)) * S;
                int px = (int)std::lround(cx + std::cos(ang) * R * 1.3);
                int py = (int)std::lround(cy + std::sin(ang) * R * 0.55 + std::sin(t * 0.9 + i * 1.7) * 2 * S);
                int sz = (i % 3 == 0 ? 2 : 1) * S;
                for (int dy = 0; dy < sz; dy++)
                    for (int dx = 0; dx < sz; dx++) {
                        bool rim = sz > S && (dx >= sz - S || dy < S);
                        bl(px + dx, py + dy, rim ? C[13] : ct_mix(C[11], C[13], sz > S ? 0.05 : 0.35), rim ? 0.55 : 1, 9);
                    }
            }
        }
    }
    uint32_t roof_px(int x, int y, double dim) const {
        uint32_t v = rt_frame[(size_t)y * rt_w + x];
        if (dim > 0.999) return v;
        RGB c = ct_un(v);
        return ct_pk({c.r * dim, c.g * dim, c.b * dim});
    }

    // ---- «Гіперпростір» (vortex): 3D фрактальний гексагональний тунель із варп-іскрами ----
    uint32_t vx_pal[16] = {};
    void vortex_prepare(double hue, double s, double dim) {
        double h0 = hue + hue_lo - 18, h1 = hue + hue_hi + 28;
        for (int i = 0; i < 16; i++) {
            double k = i / 15.0;
            double hh = h0 + (h1 - h0) * std::pow(k, 0.9) + (i >= 13 ? 25 * (k - 0.8) : 0);
            double ss = std::clamp(s * (0.45 + 0.55 * std::sin(k * 3.14159)), 0.15, 1.0);
            double ll = light ? 0.96 - 0.78 * std::pow(k, 1.15) : 0.025 + 0.90 * std::pow(k, 1.35);
            RGB c = hsl_rgb(hh, ss, ll);
            vx_pal[i] = ct_pk({c.r * dim, c.g * dim, c.b * dim});
        }
    }
    uint32_t vortex_px(int x, int y, int cols, int rows, double bay) const {
        double cx = cols * (0.5 + 0.08 * fsin(t * 0.42)), cy = rows * (0.5 + 0.07 * fcos(t * 0.33));
        double sc = 2.0 / std::max(1, rows);
        double dx = (x + 0.5 - cx) * sc, dy = (y + 0.5 - cy) * sc;
        double r = std::sqrt(dx * dx + dy * dy) + 1e-4;
        double ang = fatan2(dy, dx);
        // гексагональна + октагональна деформація перерізу тунелю
        double hex = 1.0 + 0.12 * fcos(ang * 6.0 - t * 0.7 + 1.2 / (r + 0.18))
                         + 0.05 * fsin(ang * 12.0 + t * 1.1);
        double rp = r * hex;
        double z = 1.35 / (rp + 0.045) + t * 2.6;
        double twist = ang + 0.85 / (rp + 0.11) + 0.32 * fsin(z * 0.3 - t * 0.8);
        // поздовжні енергетичні ребра та поперечні кільця глибини
        double ribs = std::fabs(fsin(twist * 6.0));
        double rings = fsin(z * 2.4);
        double cells_v = fsin(twist * 12.0 + z * 1.5) * fcos(twist * 6.0 - z * 2.4);
        double E = std::pow(ribs, 3.0) * 0.58 + std::max(0.0, rings - 0.45) * 1.35
                 + std::max(0.0, cells_v) * 0.42;
        // глибинне загасання біля центру + сяюче ядро сингулярності
        double fog = std::clamp(rp * 1.45, 0.0, 1.0);
        E *= fog;
        double core = std::exp(-rp * rp * 42.0) * (0.85 + 0.15 * fsin(t * 3.5));
        E = std::max(E, core * 1.15);
        // варп-іскри, що летять на глядача
        int sec = (int)((ang + 3.14159265) * 19.1) & 127;
        uint32_t sh = hash(sec * 7919u + 17u);
        if ((sh & 3) == 0 && rp > 0.08) {
            double sp = 1.8 + (sh % 11) * 0.18;
            double head = std::fmod(t * sp + (sh >> 8) * 0.013, 1.6);
            double dr = rp - head;
            if (dr > 0 && dr < 0.22 * rp + 0.03) E = std::max(E, 0.92 + 0.15 * (1 - dr / (0.22 * rp + 0.03)));
        }
        int qi = std::clamp((int)std::floor(E * 13.5 + bay), 0, 15);
        return vx_pal[qi];
    }

    // ---- «Ретровейв» (synthwave): 3D-перспектива, каркасні гори, лазерне шосе та планета з кільцем ----
    std::vector<int> sw_m1, sw_m2;
    uint32_t sw_sky[10] = {}, sw_grid_c = 0, sw_road_c = 0, sw_mtn_c1 = 0, sw_mtn_c2 = 0;
    void synth_prepare(int cols, int rows, double hue, double s, double dim) {
        double h0 = hue + hue_lo - 15, h1 = hue + hue_hi + 22;
        for (int i = 0; i < 10; i++) {
            double k = i / 9.0;
            RGB c = hsl_rgb(h0 + (h1 - h0) * k, std::clamp(s * (0.55 + 0.4 * k), 0.2, 1.0),
                            light ? 0.95 - 0.42 * std::pow(k, 1.2) : 0.03 + 0.46 * std::pow(k, 1.5));
            sw_sky[i] = ct_pk({c.r * dim, c.g * dim, c.b * dim});
        }
        RGB gc = hsl_rgb(hue + 28, 0.95, light ? 0.38 : 0.68);
        RGB rc = hsl_rgb(hue - 18, 0.92, light ? 0.42 : 0.62);
        RGB m1 = hsl_rgb(hue + 10, std::max(s, 0.65), light ? 0.45 : 0.56);
        RGB m2 = hsl_rgb(hue - 12, std::max(s, 0.55), light ? 0.62 : 0.36);
        sw_grid_c = ct_pk({gc.r * dim, gc.g * dim, gc.b * dim});
        sw_road_c = ct_pk({rc.r * dim, rc.g * dim, rc.b * dim});
        sw_mtn_c1 = ct_pk({m1.r * dim, m1.g * dim, m1.b * dim});
        sw_mtn_c2 = ct_pk({m2.r * dim, m2.g * dim, m2.b * dim});
        int hor = (int)(rows * 0.50);
        sw_m1.resize(cols); sw_m2.resize(cols);
        for (int x = 0; x < cols; x++) {
            double fx = (double)x / std::max(1, cols - 1);
            double flank = std::clamp((std::fabs(fx - 0.5) - 0.08) / 0.42, 0.0, 1.0);
            double w1 = 0.55 + 0.3 * std::sin(x * 0.075 + t * 0.25) + 0.2 * std::sin(x * 0.19 - t * 0.15);
            double w2 = 0.5 + 0.35 * std::sin(x * 0.045 - t * 0.12 + 2.1) + 0.2 * std::cos(x * 0.13);
            sw_m1[x] = hor - (int)std::lround(rows * 0.22 * flank * w1);
            sw_m2[x] = hor - (int)std::lround(rows * 0.14 * std::pow(flank, 0.7) * w2);
        }
    }
    uint32_t synth_px(int x, int y, int cols, int rows, double bay, double hue, double s, double dim) const {
        auto pk = [&](RGB c) { return ct_pk({c.r * dim, c.g * dim, c.b * dim}); };
        int hor = (int)(rows * 0.50);
        // --- 3D-земля та неонова сітка нижче горизонту ---
        if (y >= hor) {
            double p = (double)(y - hor + 1) / std::max(1, rows - hor);
            double z = 1.6 / (p + 0.035) + t * 3.6;
            double wx = (x + 0.5 - cols * 0.5) / (rows * 0.42 * (p + 0.035));
            RGB bg = hsl_rgb(hue + hue_lo - 10, s * 0.55, light ? 0.90 - 0.25 * p : 0.025 + 0.06 * (1 - p));
            // бічні каньйони: рельєф підіймає лінії сітки
            double canyon = std::max(0.0, std::fabs(wx) - 1.25);
            double z_mod = z + canyon * 0.9 * fsin(wx * 1.4 + z * 0.5);
            double gx = std::fabs(wx - std::round(wx));
            double gz = std::fabs(z_mod - std::round(z_mod));
            double line_w = 0.045 + 0.04 * (1 - p);
            // центральна магістраль: подвійні рейки та пунктир посередині
            if (std::fabs(std::fabs(wx) - 1.22) < line_w * 1.4) return sw_road_c;
            if (std::fabs(wx) < 0.06 && std::fmod(z * 0.5, 1.0) < 0.45) return sw_grid_c;
            if (gx < line_w || gz < line_w * 1.2) {
                double pulse = 0.65 + 0.35 * fsin(z * 0.5 - t * 4.2);
                RGB lc = ct_un(canyon > 0.01 ? sw_mtn_c1 : sw_grid_c);
                return ct_pk(ct_mix(bg, lc, std::clamp(pulse * (0.4 + 0.6 * p), 0.0, 1.0)));
            }
            // неонове сяйво самого обрію
            if (y <= hor + 1) return sw_road_c;
            return pk(bg);
        }
        // --- каркасні гори над обрієм (два шари з діагональною полігональною сіткою) ---
        if (y >= sw_m1[x]) {
            int top = sw_m1[x];
            RGB body = hsl_rgb(hue - 12, s * 0.5, light ? 0.80 : 0.055);
            if (y <= top + 1) return sw_mtn_c1;
            int d1 = (x + y - top) % 7, d2 = (x - (y - top) + 700) % 9;
            if (d1 == 0 || d2 == 0 || (y - top) % 5 == 0) return ct_pk(ct_mix(body, ct_un(sw_mtn_c1), 0.65));
            return pk(body);
        }
        if (y >= sw_m2[x]) {
            int top = sw_m2[x];
            RGB body = hsl_rgb(hue - 5, s * 0.45, light ? 0.86 : 0.085);
            if (y == top) return sw_mtn_c2;
            if ((x + y) % 8 == 0 || (y - top) % 6 == 0) return ct_pk(ct_mix(body, ct_un(sw_mtn_c2), 0.5));
            return pk(body);
        }
        // --- небо + ретро-планета з орбітальним кільцем ---
        double fy = (double)y / std::max(1, hor);
        double band = std::pow(fy, 1.35) * 9.0;
        int bi = std::clamp((int)band + (bay < band - (int)band ? 1 : 0), 0, 9);
        uint32_t sky_px = sw_sky[bi];
        double scx = cols * 0.5, scy = hor * 0.52, sr = rows * 0.21;
        double dx = x + 0.5 - scx, dy = y + 0.5 - scy;
        double dist = std::sqrt(dx * dx + dy * dy);
        // орбітальне кільце (нахилений еліпс): передня дуга перекриває планету
        double rx = dx * 0.96 - dy * 0.26, ry = (dx * 0.26 + dy * 0.96) * 4.2;
        double rd = std::sqrt(rx * rx + ry * ry);
        bool in_ring = rd > sr * 1.22 && rd < sr * 1.88;
        bool ring_front = in_ring && ry > 0;
        if (ring_front) {
            double ru = (rd - sr * 1.22) / (sr * 0.66);
            if (std::fmod(ru * 4.0, 1.0) < 0.72)
                return pk(hsl_rgb(hue + 32, 0.9, light ? 0.48 : 0.78));
        }
        if (dist < sr) {
            double o = (dy + sr) / (2 * sr);
            // смуги ретро-сонця + меридіани каркасної сфери
            bool gap = o > 0.46 && std::fmod(std::fmod(y - scy - t * 1.8, 4.5) + 4.5, 4.5) < (o - 0.42) * 4.2;
            if (!gap) {
                double z_sph = std::sqrt(std::max(0.0, sr * sr - dx * dx - dy * dy));
                double lon = fatan2(dx, z_sph) + t * 0.28, lat = fatan2(dy, std::hypot(dx, z_sph));
                bool wire = std::fabs(fsin(lon * 5.0)) > 0.95 || std::fabs(fsin(lat * 6.0)) > 0.95;
                RGB c1 = hsl_rgb(hue + 38, 0.95, light ? 0.65 : 0.88);
                RGB c2 = hsl_rgb(hue - 10, 0.90, light ? 0.42 : 0.54);
                RGB pc = ct_mix(c1, c2, std::clamp(o, 0.0, 1.0));
                if (wire) pc = ct_mix(pc, {1, 1, 1}, 0.45);
                return pk(pc);
            }
        }
        if (in_ring && !ring_front) {
            double ru = (rd - sr * 1.22) / (sr * 0.66);
            if (std::fmod(ru * 4.0, 1.0) < 0.72)
                return pk(hsl_rgb(hue + 22, 0.75, light ? 0.58 : 0.58));
        }
        // зорі у верхній частині неба
        if (!light && fy < 0.65) {
            uint32_t hs = hash(x * 73856093u ^ y * 19349663u ^ 0x5177u);
            if ((hs & 511) < 4) {
                double tw = 0.5 + 0.5 * fsin(t * (0.7 + (hs >> 9) % 15 * 0.1) + (hs >> 15));
                if (tw > 0.35) return pk(hsl_rgb(hue + 25, 0.4, 0.65 + 0.3 * tw));
            }
        }
        return sky_px;
    }

    // ---- «Плазма-лампа» (lava): метаболи / ферофлюїд із відблисками та електричними дугами ----
    struct LvBall { float x, y, r2; };
    LvBall lv_balls[10] = {};
    uint32_t lv_pal[16] = {};
    void lava_prepare(int cols, int rows, double hue, double s, double dim) {
        double mr = std::min(cols, rows);
        for (int i = 0; i < 10; i++) {
            double fi = i * 0.6283 + 0.4;
            double spx = 0.28 + 0.07 * (i % 4), spy = 0.22 + 0.09 * ((i + 1) % 5);
            double bx = cols * (0.5 + 0.36 * std::sin(t * spx + fi * 2.1) * std::cos(t * 0.13 + i));
            double by = rows * (0.5 + 0.38 * std::sin(t * spy + fi * 1.3) + 0.08 * std::cos(t * 0.45 - i * 1.7));
            double rad = mr * (0.095 + 0.045 * ((i * 7) % 5) / 4.0) * (0.9 + 0.1 * std::sin(t * 0.9 + i));
            lv_balls[i] = {(float)bx, (float)by, (float)(rad * rad)};
        }
        double h0 = hue + hue_lo - 12, h1 = hue + hue_hi + 36;
        for (int i = 0; i < 16; i++) {
            double k = i / 15.0;
            double hh = h0 + (h1 - h0) * std::pow(k, 1.1);
            double ss = std::clamp(s * (0.5 + 0.5 * std::sin(k * 3.14)), 0.2, 1.0);
            double ll = light ? 0.95 - 0.72 * std::pow(k, 1.2) : 0.03 + 0.90 * std::pow(k, 1.3);
            RGB c = hsl_rgb(hh, ss, ll);
            lv_pal[i] = ct_pk({c.r * dim, c.g * dim, c.b * dim});
        }
    }
    uint32_t lava_px(int x, int y, int cols, int rows, double bay) const {
        float px = x + 0.5f, py = y + 0.5f;
        float field = 0.0f, gx = 0.0f, gy = 0.0f;
        for (int i = 0; i < 10; i++) {
            float dx = px - lv_balls[i].x, dy = py - lv_balls[i].y;
            float d2 = dx * dx + dy * dy + 6.0f;
            float w = lv_balls[i].r2 / d2;
            field += w;
            float g = w / d2;
            gx -= dx * g; gy -= dy * g;
        }
        // всередині краплі метабола: об'ємне освітлення + відблиск Френеля
        if (field >= 1.0f) {
            float gl = std::sqrt(gx * gx + gy * gy) + 1e-5f;
            float nx = gx / gl, ny = gy / gl;
            float spec = std::max(0.0f, (-0.55f * nx - 0.75f * ny));   // світло згори-ліворуч
            double core = std::clamp((field - 1.0) * 0.55, 0.0, 1.0);
            double rim = std::exp(-std::pow((field - 1.08) * 7.0, 2));
            double v = 0.45 + 0.38 * core + 0.28 * std::pow(spec, 3.0) + 0.22 * rim;
            int qi = std::clamp((int)std::floor(v * 14.0 + bay), 6, 15);
            return lv_pal[qi];
        }
        // зовні: електричні плазмові нитки вздовж силових ліній між близькими краплями
        double aura = std::pow(std::clamp((double)field, 0.0, 1.0), 1.8) * 0.42;
        if (field > 0.38f) {
            double ang = fatan2(gy, gx);
            double arc = fsin(ang * 7.0 + field * 9.5 - t * 3.8) * fcos(ang * 5.0 - field * 6.0 + t * 2.4);
            if (arc > 0.62) aura += (arc - 0.62) * 2.4 * (field - 0.35);
        }
        // легка фонова сітка капсули
        if ((x % 8 == 0 && y % 8 == 0) && aura < 0.12) aura += 0.08;
        int qi = std::clamp((int)std::floor(aura * 14.0 + bay), 0, 14);
        return lv_pal[qi];
    }

    // ---- «Біолюмінесценція» (abyss): океанська безодня, промені каустики, водорості та сяючі медузи ----
    std::vector<uint32_t> ab_frame;
    int ab_w = 0, ab_h = 0;
    void abyss_prepare(int cols, int rows, double hue, double s, double dim) {
        static const double BY[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        auto bay = [&](int x, int y) { return (BY[y & 3][x & 3] + 0.5) / 16.0; };
        ab_w = cols; ab_h = rows;
        ab_frame.resize((size_t)cols * rows);
        double h_deep = hue + hue_lo - 16, h_surf = hue + hue_hi + 12;
        // 1. Фон безодні + сонячні/місячні промені каустики згори
        for (int y = 0; y < rows; y++) {
            double fy = (double)y / std::max(1, rows - 1);
            double depth = 1.0 - fy;
            for (int x = 0; x < cols; x++) {
                double b = bay(x, y);
                // діагональні промені води
                double rx = (x - y * 0.38) * 0.055;
                double ray = std::max(0.0, fsin(rx + t * 0.45) * 0.6 + fsin(rx * 2.3 - t * 0.7) * 0.4);
                double caus = std::pow(ray, 2.2) * std::pow(depth, 1.5) * 0.38;
                double lv = std::clamp(depth * 0.32 + caus + (b - 0.5) * 0.06, 0.0, 1.0);
                RGB c = hsl_rgb(h_deep + (h_surf - h_deep) * lv, std::clamp(s * (0.6 + 0.35 * lv), 0.25, 0.95),
                                light ? 0.94 - 0.55 * (1 - lv) : 0.025 + 0.38 * std::pow(lv, 1.25));
                ab_frame[(size_t)y * cols + x] = ct_pk({c.r * dim, c.g * dim, c.b * dim});
            }
        }
        auto blend = [&](int x, int y, RGB c, double a) {
            if (x < 0 || y < 0 || x >= cols || y >= rows || a <= 0) return;
            size_t p = (size_t)y * cols + x;
            RGB cur = ct_un(ab_frame[p]);
            RGB dst = {c.r * dim, c.g * dim, c.b * dim};
            ab_frame[p] = ct_pk(ct_mix(cur, dst, std::min(1.0, a)));
        };
        // 2. Водорості на дні, що плавно гойдаються течією, зі світними бруньками
        RGB kelp_dark = hsl_rgb(h_deep - 8, s * 0.5, light ? 0.42 : 0.05);
        RGB kelp_glow = hsl_rgb(hue + 35, 0.95, light ? 0.55 : 0.72);
        for (int k = 0; k < 18; k++) {
            uint32_t kh = hash(k * 131u + 29u);
            int bx = (int)((kh % 1000) / 1000.0 * cols);
            int kh_len = (int)(rows * (0.16 + 0.24 * ((kh >> 10) % 100) / 100.0));
            double ph = (kh >> 18) * 0.01;
            for (int dy = 0; dy < kh_len; dy++) {
                int y = rows - 1 - dy;
                double u = (double)dy / kh_len;
                int x = bx + (int)std::lround( std::sin(t * 1.1 + u * 3.5 + ph) * (1.5 + 4.5 * u) );
                int w = u < 0.7 ? 1 : 0;
                for (int dx = -w; dx <= w; dx++) blend(x + dx, y, kelp_dark, 0.85);
                if (dy % 7 == 4) {
                    double pulse = 0.5 + 0.5 * fsin(t * 2.0 + k + dy * 0.3);
                    blend(x + (dy % 2 ? 2 : -2), y, kelp_glow, 0.45 + 0.45 * pulse);
                }
            }
        }
        // 3. Сяючий планктон, що дрейфує у воді
        for (int i = 0; i < 45; i++) {
            uint32_t ph = hash(i * 977u + 7u);
            double px = std::fmod((ph % 1000) * 0.001 * cols + t * (1.2 + (ph % 7) * 0.3) + std::sin(t * 0.8 + i) * 4.0, (double)cols);
            double py = std::fmod(rows * 2.0 + ((ph >> 10) % 1000) * 0.001 * rows - t * (1.5 + (ph % 5) * 0.4), (double)rows);
            double tw = 0.4 + 0.6 * fsin(t * (1.5 + (i % 7) * 0.25) + i * 1.9);
            RGB spc = hsl_rgb(hue + ((i & 1) ? 32 : -18), 0.92, light ? 0.48 : 0.82);
            int ix = (int)px, iy = (int)py;
            blend(ix, iy, spc, tw);
            if (tw > 0.75) {
                blend(ix - 1, iy, spc, tw * 0.3); blend(ix + 1, iy, spc, tw * 0.3);
                blend(ix, iy - 1, spc, tw * 0.3); blend(ix, iy + 1, spc, tw * 0.3);
            }
        }
        // 4. П'ять біолюмінесцентних медуз із пульсуючими куполами та щупальцями
        for (int j = 0; j < 5; j++) {
            double jph = j * 1.37 + 0.5;
            double cycle = t * (1.9 + j * 0.15) + jph * 3.0;
            double pulse = std::sin(cycle);                     // скорочення купола
            double speed = 5.5 + j * 0.9;
            double span_y = rows + 50.0;
            double cy = rows + 25.0 - std::fmod(t * speed + j * span_y * 0.21 + std::max(0.0, pulse) * 2.2, span_y);
            double cx = cols * (0.14 + 0.18 * j) + std::sin(t * 0.45 + j * 2.1) * (cols * 0.06);
            double scale = (rows / 180.0) * (j % 2 == 0 ? 1.05 : 0.82);
            double rx = (9.5 - 1.8 * pulse) * scale;
            double ry = (6.8 + 1.4 * pulse) * scale;
            double jhue = hue + (j % 3 == 0 ? 0 : j % 3 == 1 ? 34 : -24);
            RGB c_rim = hsl_rgb(jhue + 15, 0.95, light ? 0.45 : 0.86);
            RGB c_body = hsl_rgb(jhue, 0.88, light ? 0.58 : 0.58);
            RGB c_core = hsl_rgb(jhue + 42, 0.95, light ? 0.40 : 0.92);
            // м'який ореол навколо медузи
            int hr = (int)(rx * 1.9);
            for (int dy = -hr; dy <= hr; dy++)
                for (int dx = -hr; dx <= hr; dx++) {
                    double d = std::hypot(dx, dy) / std::max(1.0, hr * 1.0);
                    if (d < 1.0) blend((int)cx + dx, (int)cy + dy, c_body, (1 - d) * (1 - d) * 0.22);
                }
            // щупальця (7 штук), що хвилясто тягнуться знизу купола
            int n_tent = 7;
            int t_len = (int)((16.0 + 4.0 * (j % 2)) * scale);
            for (int ti = 0; ti < n_tent; ti++) {
                double tx0 = cx + (ti - (n_tent - 1) * 0.5) * (rx * 0.24);
                for (int s_step = 0; s_step < t_len; s_step++) {
                    double u = (double)s_step / std::max(1, t_len);
                    double sway = std::sin(cycle - u * 4.2 + ti * 0.7) * (1.2 + 3.5 * u) * scale;
                    int tx = (int)std::lround(tx0 + sway);
                    int ty = (int)std::lround(cy + ry * 0.25 + s_step);
                    RGB tc = (ti == 3 || s_step % 3 == 0) ? c_core : c_rim;
                    blend(tx, ty, tc, (1.0 - u * 0.78) * 0.85);
                }
            }
            // купол медузи (сплюснута півсфера з фестончастим краєм)
            int irx = (int)std::ceil(rx + 2), iry = (int)std::ceil(ry + 2);
            for (int dy = -iry; dy <= (int)(ry * 0.45); dy++)
                for (int dx = -irx; dx <= irx; dx++) {
                    double nx = dx / std::max(0.5, rx), ny = dy / std::max(0.5, ry);
                    double skirt = 0.22 + 0.10 * std::cos(nx * 9.0 + cycle);
                    if (ny > skirt) continue;
                    double d2 = nx * nx + ny * ny;
                    if (d2 > 1.0) continue;
                    double d = std::sqrt(d2);
                    bool is_rim = d > 0.78 || ny > skirt - 0.12;
                    bool is_core = std::hypot(nx * 1.4, ny + 0.2) < 0.42;
                    RGB col = is_core ? c_core : is_rim ? c_rim : c_body;
                    double alpha = is_rim ? 0.95 : is_core ? 0.88 : 0.45 + 0.35 * d;
                    blend((int)cx + dx, (int)cy + dy, col, alpha);
                }
        }
    }
    uint32_t abyss_px(int x, int y) const {
        return ab_frame[(size_t)y * ab_w + x];
    }

    // ---- «Кристали» (crystal): 3D-грані Вороного (жеода), сяючі розломи та рухоме джерело світла ----
    static constexpr int CR_GX = 8, CR_GY = 5;
    struct CrSeed { float x, y, nx, ny, h_off; };
    CrSeed cr_seeds[CR_GX * CR_GY] = {};
    float cr_lx = 0, cr_ly = 0;
    void crystal_prepare(int cols, int rows) {
        double cw = (double)cols / CR_GX, ch = (double)rows / CR_GY;
        for (int gy = 0; gy < CR_GY; gy++)
            for (int gx = 0; gx < CR_GX; gx++) {
                int id = gy * CR_GX + gx;
                uint32_t h = hash(id * 7919u + 0xC215u);
                double ph1 = (h & 1023) * 0.00614, ph2 = ((h >> 10) & 1023) * 0.00614;
                double sx = (gx + 0.5 + 0.36 * std::sin(t * 0.35 + ph1)) * cw;
                double sy = (gy + 0.5 + 0.36 * std::cos(t * 0.29 + ph2)) * ch;
                double na = ph1 * 2.0 + 0.25 * std::sin(t * 0.4 + id);
                cr_seeds[id] = {(float)sx, (float)sy, (float)std::cos(na), (float)std::sin(na),
                                (float)(((int)(h >> 20) % 41) - 20)};
            }
        cr_lx = (float)(cols * (0.5 + 0.38 * std::cos(t * 0.52)));
        cr_ly = (float)(rows * (0.5 + 0.34 * std::sin(t * 0.39)));
    }
    uint32_t crystal_px(int x, int y, int cols, int rows, double bay, double hue, double s, double dim) const {
        float px = x + 0.5f, py = y + 0.5f;
        int gx0 = std::clamp((int)(px * CR_GX / cols), 0, CR_GX - 1);
        int gy0 = std::clamp((int)(py * CR_GY / rows), 0, CR_GY - 1);
        float d1 = 1e9f, d2 = 1e9f;
        int best = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int gx = std::clamp(gx0 + dx, 0, CR_GX - 1);
                int gy = std::clamp(gy0 + dy, 0, CR_GY - 1);
                int id = gy * CR_GX + gx;
                float d = std::hypot(px - cr_seeds[id].x, py - cr_seeds[id].y);
                if (d < d1) { d2 = d1; d1 = d; best = id; }
                else if (d < d2 && id != best) { d2 = d; }
            }
        const CrSeed& cs = cr_seeds[best];
        float edge = d2 - d1;
        // сяючі енергетичні жили вздовж стиків кристалів
        if (edge < 1.35f) {
            double pulse = 0.65 + 0.35 * fsin(px * 0.09 + py * 0.09 - t * 2.8);
            double core = std::clamp(1.0 - edge / 1.35, 0.0, 1.0);
            RGB vc = hsl_rgb(hue + 28, 0.95, light ? 0.35 + 0.25 * (1 - core) : 0.55 + 0.40 * core * pulse);
            return ct_pk({vc.r * dim, vc.g * dim, vc.b * dim});
        }
        // 3D-освітлення грані кристала + внутрішні смуги заломлення
        float lx = cr_lx - px, ly = cr_ly - py, lz = rows * 0.28f;
        float ll = std::sqrt(lx * lx + ly * ly + lz * lz) + 1e-5f;
        lx /= ll; ly /= ll; lz /= ll;
        float ndotl = std::max(0.0f, cs.nx * lx * 0.65f + cs.ny * ly * 0.65f + 0.76f * lz);
        float spec = std::pow(ndotl, 7.0f);
        double refract = 0.5 + 0.5 * fsin((px - cs.x) * cs.nx * 0.42 + (py - cs.y) * cs.ny * 0.42);
        double bev = std::clamp((edge - 1.35) / 4.5, 0.0, 1.0);
        double lv = (0.18 + 0.52 * ndotl + 0.14 * refract) * (0.75 + 0.25 * bev) + 0.35 * spec;
        int q = std::clamp((int)std::floor(lv * 8.0 + bay), 0, 8);
        double qk = q / 8.0;
        RGB fc = hsl_rgb(hue + cs.h_off * 0.7, std::clamp(s * (0.55 + 0.4 * (1 - spec * 0.6)), 0.15, 0.95),
                         light ? 0.94 - 0.58 * qk : 0.05 + 0.82 * std::pow(qk, 1.2));
        return ct_pk({fc.r * dim, fc.g * dim, fc.b * dim});
    }

    // ---- «Дивний атрактор» (attractor): 3D хаотичний атрактор Лоренца з кометними шлейфами ----
    std::vector<uint32_t> at_frame;
    int at_w = 0, at_h = 0;
    void attractor_prepare(int cols, int rows, double hue, double s, double dim) {
        at_w = cols; at_h = rows;
        at_frame.resize((size_t)cols * rows);
        RGB bg = hsl_rgb(hue + hue_lo - 12, s * 0.45, light ? 0.94 : 0.03);
        RGB grid_c = hsl_rgb(hue, s * 0.5, light ? 0.84 : 0.08);
        uint32_t bg_pk = ct_pk({bg.r * dim, bg.g * dim, bg.b * dim});
        uint32_t gr_pk = ct_pk({grid_c.r * dim, grid_c.g * dim, grid_c.b * dim});
        int cx = cols / 2, cy = rows / 2;
        double r_hud = std::min(cols, rows) * 0.42;
        // 1. Фон із делікатною радарною сіткою та прицільними мітками
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                bool grid = (x % 16 == 0 && y % 4 == 0) || (y % 16 == 0 && x % 4 == 0);
                double d = std::hypot(x - cx, y - cy);
                if (std::fabs(d - r_hud) < 0.6 && ((x + y) & 1)) grid = true;
                at_frame[(size_t)y * cols + x] = grid ? gr_pk : bg_pk;
            }
        auto add_px = [&](int x, int y, RGB c, double a) {
            if (x < 0 || y < 0 || x >= cols || y >= rows || a <= 0) return;
            size_t p = (size_t)y * cols + x;
            RGB cur = ct_un(at_frame[p]);
            RGB dst = {c.r * dim, c.g * dim, c.b * dim};
            if (light) {
                at_frame[p] = ct_pk(ct_mix(cur, dst, std::min(1.0, a)));
            } else {
                // адитивне світіння фосфору в темній темі
                at_frame[p] = ct_pk({std::min(1.0, cur.r + dst.r * a),
                                     std::min(1.0, cur.g + dst.g * a),
                                     std::min(1.0, cur.b + dst.b * a)});
            }
        };
        // 2. Інтегруємо 75 потоків на 3D-атракторі Лоренца (dx=10(y-x), dy=x(28-z)-y, dz=xy-8/3z)
        double ang_y = t * 0.28, ang_x = 0.35 * std::sin(t * 0.17);
        double cy_a = std::cos(ang_y), sy_a = std::sin(ang_y);
        double cx_a = std::cos(ang_x), sx_a = std::sin(ang_x);
        double scale = std::min((double)cols, rows * 1.45) * 0.021;
        for (int k = 0; k < 75; k++) {
            // детермінована початкова точка + фазовий зсув уздовж траєкторії
            double lx = 0.1 + 12.0 * std::sin(k * 1.73), ly = 0.1 + 12.0 * std::cos(k * 2.19), lz = 25.0 + 8.0 * std::sin(k * 0.91);
            int warmup = 90 + (int)(std::fmod(t * 28.0 + k * 19.3, 180.0));
            const double dt = 0.0065;
            for (int s_i = 0; s_i < warmup; s_i++) {
                double dx = 10.0 * (ly - lx), dy = lx * (28.0 - lz) - ly, dz = lx * ly - (8.0 / 3.0) * lz;
                lx += dx * dt; ly += dy * dt; lz += dz * dt;
            }
            // малюємо кометний шлейф із 42 кроків
            const int TAIL = 42;
            double px0 = -1e9, py0 = -1e9;
            for (int s_i = 0; s_i < TAIL; s_i++) {
                double dx = 10.0 * (ly - lx), dy = lx * (28.0 - lz) - ly, dz = lx * ly - (8.0 / 3.0) * lz;
                lx += dx * dt; ly += dy * dt; lz += dz * dt;
                double cz = lz - 25.0;
                // 3D-обертання
                double rx = lx * cy_a + cz * sy_a;
                double rz = -lx * sy_a + cz * cy_a;
                double ry = ly * cx_a - rz * sx_a;
                double rz2 = ly * sx_a + rz * cx_a;
                double persp = 1.0 / (1.0 + rz2 * 0.012);
                double sx = cx + rx * scale * persp * 1.25;
                double sy = cy + ry * scale * persp * 0.95;
                double u = (double)s_i / (TAIL - 1);          // 0 — хвіст, 1 — голова комети
                double h_pt = hue + (k % 3 == 0 ? 0 : k % 3 == 1 ? 30 : -22) + u * 18.0;
                RGB col = hsl_rgb(h_pt, u > 0.88 ? 0.55 : std::max(s, 0.78),
                                  light ? 0.35 + 0.25 * (1 - u) : 0.42 + 0.52 * std::pow(u, 1.6));
                double alpha = std::pow(u, 1.5) * (light ? 0.75 : 0.62);
                int ix = (int)std::lround(sx), iy = (int)std::lround(sy);
                add_px(ix, iy, col, alpha);
                if (px0 > -1e8 && std::hypot(sx - px0, sy - py0) > 1.2) {
                    add_px((int)std::lround((sx + px0) * 0.5), (int)std::lround((sy + py0) * 0.5), col, alpha * 0.7);
                }
                if (s_i == TAIL - 1) {
                    add_px(ix - 1, iy, col, 0.25); add_px(ix + 1, iy, col, 0.25);
                    add_px(ix, iy - 1, col, 0.25); add_px(ix, iy + 1, col, 0.25);
                }
                px0 = sx; py0 = sy;
            }
        }
    }
    uint32_t attractor_px(int x, int y) const {
        return at_frame[(size_t)y * at_w + x];
    }

    // ---- «Вежа» — тиха lo-fi сцена (власний персонаж) ----
    // Знизу праворуч на уламку руїн сидить дівчина в навушниках спиною до глядача й дивиться на далеку
    // вежу, зарослу квітами, у серпанку. З боків — квітучі гілки, що ледь гойдаються, падають пелюстки,
    // пливуть смуги туману, біля вершини кружляють птахи. Верх кадру — порожнє небо з дизерингом.
    // Темна тема — мрійливі сутінки, світла — пастельний день; усе у відтінках акценту.
    static const char* const* girl_spr() {      // 30×44, дивиться праворуч; ряд 36 — стегна на опорі
        static const char* S[44] = {
            "...........K......K...........", "..........KK.....KK...........", "..........KkK...KkK...........",
            ".........KbbbbbbbbbK..........", "........hhhhhhhhhgghh.........", ".......hhhhhhhhhhhggghr.......",
            "......hhhhhhhhhhhhhhhhhr......", "......hhhhhhhhhhhhhhhhhr......", "......hhhhhhhhhhhhhhhEEr......",
            "......hhhhhhhhhhhhhhhELr......", "......hhhhhhhhhhhhhhhELr......", "......hhhhhhhhhhhhhhhEEr......",
            "......hhhhhhhhhhhhhhhhr.......", "......hhhhhhhhhhhhhhhhr.......", ".......hhhhhhhhhhhhhhr........",
            ".......hhhhhhhhhhhhhh.........", "......hhhh.hhhhhhhhh..........", ".....HHHHHHhhhhhhHHHH.........",
            "....HHHDDDDHHHHDDDHHHr........", "...HHHHHHHHHHHHHHHHHHHr.......", "...HHHHHHHHHHHHHHHHHHHHr......",
            "..HHHHHHHDHHHHHHHHHHHHHr......", "..HHHHHHHDHHHHHHHHHHHHHHr.....", "..HHHHHHHDHHHHHHHHHHHHHHr.....",
            "..HHHHHHHDHHHHHHHHHHHHHHr.....", "..HHHHHHHDHHHHHHHHHHHHHHHr....", "..HHHHHHHDHHHHHHHHHHHHHHHr....",
            "..HHHHHHHDHHHHHHHHHHHHHHHr....", "..HHHHHHHDHHHHHHHHHHHHHHHr....", "..HHHHHHHDHHHHHHHHHHHHHHHHr...",
            "..HHHHHHHDHHHHHHHHHHHHHHHHr...", "...HHHHHHDHHHHHHHHHHHHHHHHr...", "...HHHHHHHHHHHHHHHHHHHPPPPPr..",
            "...HHHHHHHHHHHHHHHHHPPPPPPPPr.", "...HHHHHHHHHHHHHHHHPPPPPPPPPPr", "....HHHHHHHHHHHHHHPPPPPPPPPPPP",
            "....PPPPPPPPPPPPPPPPPPPPPPPPPP", ".........................PPPP.", ".........................PPP..",
            ".........................PPP..", "........................PPP...", "........................PPP...",
            "........................PPP...", "........................SSSS.."};
        return S;
    }
    std::vector<uint32_t> tw_base, tw_frame;
    std::vector<uint8_t> tw_dep;            // 0 небо, 1 вежа/далечінь, 2 земля, 3 уламок, 4 дівчина
    int tw_w = 0, tw_h = 0, tw_light = -1, tw_hue = -9999;
    int tw_s = 1, tw_gx = 0, tw_gy = 0, tw_hor = 0, tw_tx = 0, tw_ttop = 0, tw_ledx = 0, tw_ledy = 0;
    int tw_ring = 0, tw_hw0 = 5, tw_winx = 0, tw_winy = 0;
    struct TwSeg { float x0, y0, x1, y1, w, th; };        // гілка: відрізок, w — гнучкість, th — товщина
    struct TwBloom { float x, y, r, w; uint32_t seed; uint8_t kind; }; // 0 гроно цвіту, 1 бутон, 2 листочки
    struct TwTuft { int x, y, h; bool flower; uint32_t seed; };
    std::vector<TwSeg> tw_segs;
    std::vector<TwBloom> tw_blooms;
    struct TwPt { int16_t x, y; uint8_t tone; };
    std::vector<TwPt> tw_tree[3];                // дерева: 3 шари гойдання (стовбур / гілки / кінчики), тон 1–4 цвіт, 5–7 кора
    std::vector<std::vector<uint8_t>> tw_cspr;   // кеш форми кожної маси крони: тон 0..4 на клітинку (2R×2R)
    std::vector<TwBloom> tw_canopy;      // сакура: великі маси крони (x, y, r, гнучкість, seed, kind=0 ліве дерево / 1 права крона)
    std::vector<TwTuft> tw_tufts;
    RGB tw_c[16];
    // 0 небо верх, 1 небо обрій, 2 серпанок, 3 вежа, 4 тінь вежі, 5 лоза, 6 квітка, 7 світла квітка,
    // 8 земля, 9 уламок, 10 край уламка, 11 силует, 12 волосся, 13 контрове світло, 14 світлодіод, 15 гілка
    void tower_palette(double H, double s) {
        double lo = hue_lo, hi = hue_hi;
        bool D = !light;
        auto L = [&](double n, double d) { return D ? n : d; };
        tw_c[0] = hsl_rgb(H + lo - 20, s * L(0.45, 0.35), L(0.10, 0.82));
        tw_c[1] = hsl_rgb(H + hi + 12, s * L(0.5, 0.45), L(0.40, 0.95));
        tw_c[2] = hsl_rgb(H + hi + 5, s * L(0.38, 0.3), L(0.32, 0.90));
        tw_c[3] = hsl_rgb(H + lo - 10, s * L(0.3, 0.22), L(0.20, 0.70));
        tw_c[4] = hsl_rgb(H + lo - 12, s * L(0.3, 0.22), L(0.15, 0.62));
        tw_c[5] = ct_mix(tw_c[3], hsl_rgb(150, 0.35, L(0.22, 0.52)), 0.55);
        tw_c[6] = hsl_rgb(H + hi + 25, 0.7, L(0.68, 0.76));
        tw_c[7] = hsl_rgb(H + hi + 30, 0.6, L(0.84, 0.92));
        tw_c[8] = hsl_rgb(H + lo, s * L(0.3, 0.25), L(0.16, 0.74));
        tw_c[9] = hsl_rgb(H - 10, s * L(0.2, 0.15), L(0.09, 0.44));
        tw_c[10] = hsl_rgb(H + hi, s * L(0.3, 0.25), L(0.22, 0.62));
        tw_c[11] = hsl_rgb(H - 10, 0.3, L(0.05, 0.20));
        tw_c[12] = hsl_rgb(H - 10, 0.28, L(0.08, 0.26));
        tw_c[13] = hsl_rgb(H + hi + 10, 0.6, L(0.64, 0.86));
        tw_c[14] = hsl_rgb(H, 0.9, L(0.66, 0.56));
        tw_c[15] = hsl_rgb(H - 15, 0.25, L(0.07, 0.30));
    }
    void tower_build(int cols, int rows, double hue, double s) {
        static const double BY[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        auto bay = [&](int x, int y) { return (BY[y & 3][x & 3] + 0.5) / 16.0; };
        tw_w = cols; tw_h = rows; tw_light = light; tw_hue = (int)std::lround(hue);
        tower_palette(tw_hue, s);
        const RGB* C = tw_c;
        size_t N = (size_t)cols * rows;
        tw_base.assign(N, ct_pk(C[0])); tw_dep.assign(N, 0);
        tw_segs.clear(); tw_blooms.clear(); tw_tufts.clear(); tw_canopy.clear(); for (auto& L : tw_tree) L.clear(); tw_cspr.clear();
        auto put = [&](int x, int y, RGB c, int d) {
            if (x < 0 || y < 0 || x >= cols || y >= rows) return;
            size_t p = (size_t)y * cols + x; tw_base[p] = ct_pk(c); tw_dep[p] = d;
        };
        auto get = [&](int x, int y) { return ct_un(tw_base[(size_t)y * cols + x]); };
        uint32_t sd = 0x70BE12u;
        auto rf = [&]() { sd = hash(sd + 0x9E3779B9u); return (sd & 0xFFFFFF) / double(0x1000000); };
        int S = tw_s = std::max(1, (int)std::lround(rows / 200.0));
        int hor = tw_hor = (int)(rows * 0.68);
        // небо: м'які дизерні смуги, верх — спокійний
        for (int y = 0; y < rows; y++) {
            const int NB = 12;
            double band = std::pow(std::clamp((double)y / hor, 0.0, 1.0), 1.7) * (NB - 1);
            for (int x = 0; x < cols; x++) {
                int b = std::min(NB - 1, (int)band + (bay(x, y) < band - (int)band ? 1 : 0));
                put(x, y, ct_mix(C[0], C[1], (double)b / (NB - 1)), 0);
            }
        }
        // далекі руїни вздовж обрію, в серпанку
        for (int x = 0; x < cols;) {
            int w = 4 + (int)(rf() * 10), h = (int)(rows * (0.01 + 0.05 * rf()));
            bool ruin = rf() < 0.5;
            for (int xx = x; xx < std::min(cols, x + w); xx++) {
                int top = hor - h - (ruin ? (int)(hash(xx * 31) % 3) : 0);
                for (int y = top; y < hor + 2; y++) put(xx, y, ct_mix(C[2], C[3], 0.35), 1);
            }
            x += w + (int)(rf() * 8);
        }
        // вежа: стрункий стовбур з контрфорсами, балкон-кільце, пролом у стіні, арочні вікна, лоза та грона квітів
        int tx = tw_tx = (int)(cols * 0.34), ttop = tw_ttop = (int)(rows * 0.24), tbase = hor + 2;
        int hw0 = tw_hw0 = std::max(5, cols / 42);
        int ring = tw_ring = ttop + (tbase - ttop) / 3;
        tw_winx = tx - std::max(1, hw0 / 3);
        tw_winy = ring + (tbase - ring) / 4;
        for (int y = ttop - 3; y < tbase; y++) {
            double f = std::clamp((double)(y - ttop) / (tbase - ttop), 0.0, 1.0);
            int buttress = f > 0.74 ? (int)std::lround((f - 0.74) * 8 * S) : 0;
            int hw = (int)std::lround(hw0 * (0.7 + 0.3 * f)) + (std::abs(y - ring) <= 1 ? 2 * S : 0) + buttress;
            for (int x = tx - hw; x <= tx + hw; x++) {
                int jag = y < ttop + 3 * S ? (int)(hash((x / S) * 977 + 5) % (5 * S)) : 0;   // обламаний край
                if (y < ttop + 3 * S - jag) continue;
                // пролом у стіні справа нижче балкона
                if (f > 0.43 && f < 0.56) {
                    double bf = 1.0 - std::fabs(f - 0.495) / 0.065;
                    int bdepth = (int)std::lround(hw * 0.65 * bf) + (int)(hash((y / S) * 73) % (2 * S + 1));
                    if (x > tx + hw - bdepth) continue;
                }
                int lx = x - (tx - hw);
                RGB c = lx > hw * 2 * 0.60 ? C[4] : C[3];                                   // світло зліва
                if (lx == 0) c = ct_mix(C[3], C[1], 0.28);                                  // контровий відблиск зліва
                // карнизи та пілястри
                if (std::abs(y - ring) <= 1) c = (y == ring - 1) ? ct_mix(C[3], C[1], 0.22) : C[4];
                else if (y == ring + 2 || (y - ttop) % (12 * S) == 0) c = ct_mix(C[3], C[4], 0.65);
                else if (std::abs(x - tx) == hw - 2 * S && f > 0.12 && f < 0.85) c = ct_mix(c, C[4], 0.35);
                // край пролому — темніший сколений камінь
                if (f > 0.42 && f < 0.57 && x >= tx + hw - (int)std::lround(hw * 0.65 * (1.0 - std::fabs(f - 0.495) / 0.065)) - 2 * S)
                    c = ct_mix(C[4], {0, 0, 0}, 0.18);
                // арочні вікна (два ряди по вертикалі: вгорі та під балконом)
                int wy = (y - ttop) % (12 * S), wh = 5 * S;
                int wx1 = tx - std::max(1, hw0 / 3), wx2 = tx + std::max(1, hw0 / 3);
                for (int wx : {wx1, wx2}) {
                    int dxw = std::abs(x - wx);
                    bool in_arch = (wy >= 3 * S && wy < 3 * S + wh) &&
                                   ((wy == 3 * S && dxw == 0) || (wy > 3 * S && dxw <= S));
                    if (in_arch && std::abs(y - ring) > 3 * S && f > 0.08 && f < 0.88) {
                        c = ct_mix(C[4], {0, 0, 0}, 0.42);
                        if (wy == 3 * S + wh - 1) c = ct_mix(C[3], C[1], 0.20);             // підвіконня
                    }
                }
                // лоза та квіти на стінах вежі
                double vn = std::sin(x * 0.9 + y * 0.23) + std::sin(y * 0.11 + x * 0.3) * 0.8 + (hashf(x * 13 + y * 7) - 0.5);
                double vine = std::clamp((f - 0.42) * 2.2, 0.0, 1.0) + (y < ttop + (tbase - ttop) / 4 ? 0.78 : 0);
                if (vn + vine > 1.45) {
                    c = C[5];
                    if (hashf(x * 131 + y * 17) < 0.22) c = hashf(x + y * 3) < 0.5 ? C[6] : C[7];
                }
                c = ct_mix(c, C[2], 0.24 + 0.36 * f);                                       // серпанок густішає донизу
                put(x, y, c, 1);
            }
        }
        // пишні грона квітів на обламаній верхівці вежі
        for (int k = 0; k < 9; k++) {
            double cx = tx - hw0 * 0.95 + (k / 8.0) * hw0 * 1.9;
            double cy = ttop + 1.5 * S - hashf(k * 13 + 5) * 3.5 * S + (std::fabs(cx - tx) / hw0) * 1.5 * S;
            double cr = (1.6 + 1.5 * hashf(k * 29 + 3)) * S;
            for (int yy = (int)(cy - cr - 1); yy <= (int)(cy + cr + 1); yy++)
                for (int xx = (int)(cx - cr - 1); xx <= (int)(cx + cr + 1); xx++) {
                    double d = std::hypot(xx - cx, yy - cy) / cr;
                    if (d > 1.0 || (d > 0.65 && (hash(xx * 37 + yy * 13 + k) & 3) == 0)) continue;
                    uint32_t h = hash(xx * 131 + yy * 17 + k * 7);
                    RGB col = (h % 5 == 0) ? C[7] : (h % 5 < 3) ? C[6] : C[5];
                    put(xx, yy, ct_mix(col, C[2], 0.18), 1);
                }
        }
        // земля: луг у серпанку з квітковими цятками, густішими ближче
        for (int y = hor; y < rows; y++) {
            double f = (double)(y - hor) / (rows - hor);
            for (int x = 0; x < cols; x++) {
                if (tw_dep[(size_t)y * cols + x] == 1 && y < hor + 2) continue;
                RGB c = ct_mix(ct_mix(C[2], C[8], 0.4), C[8], f);
                if (bay(x, y) < 0.5 - f * 0.4 && (y - hor) % 3 == 0) c = ct_mix(c, C[2], 0.3);
                if (hashf(x * 7919 + y * 104729) < 0.003 + 0.016 * f) c = ct_mix(c, hashf(x * 3 + y) < 0.5 ? C[6] : C[7], 0.35 + 0.5 * f);
                put(x, y, c, 2);
            }
        }
        // проміжний план між обрієм і уламком: малі руїни та дерева в серпанку
        {
            static const double MX[6] = {0.09, 0.18, 0.25, 0.46, 0.55, 0.62};
            static const double MY[6] = {0.26, 0.40, 0.20, 0.30, 0.45, 0.24};
            for (int m = 0; m < 6; m++) {
                int mx = (int)(cols * MX[m]), my = hor + (int)((rows - hor) * MY[m]);
                double mf = MY[m];
                RGB mc = ct_mix(C[2], ct_mix(C[4], C[9], 0.55), 0.55 + 0.30 * mf);
                RGB ml = ct_mix(C[2], C[5], 0.48 + 0.32 * mf);
                if (m % 2 == 0) {
                    // маленька напівзруйнована арка / колона з мохом
                    int mw = (4 + (m % 3) * 2) * S, mh = (8 + (m % 2) * 3) * S;
                    for (int yy = my - mh; yy <= my; yy++)
                        for (int xx = mx - mw; xx <= mx + mw; xx++) {
                            int dy = yy - (my - mh), dx = std::abs(xx - mx);
                            if (dy < (int)(hash(xx * 19 + m) % (2 * S + 1))) continue;
                            if (dy > 3 * S && dx < mw / 2) continue;                         // отвір арки
                            double fog_k = std::clamp((double)(my - yy) / std::max(1, mh), 0.35, 1.0);
                            RGB col = dy <= 2 * S && hashf(xx * 13 + yy) < 0.5 ? ml : mc;
                            put(xx, yy, ct_mix(get(xx, yy), col, fog_k * 0.90), 2);
                        }
                } else {
                    // м'яке розлоге деревце в серпанку з кількома квітами
                    int tr_h = (5 + m) * S;
                    for (int yy = my - tr_h; yy <= my; yy++) put(mx + (my - yy) / (3 * S), yy, mc, 2);
                    double cr = (3.5 + (m % 2) * 1.2) * S, cx = mx + 1.2 * S, cy = my - tr_h;
                    for (int yy = (int)(cy - cr); yy <= (int)(cy + cr); yy++)
                        for (int xx = (int)(cx - cr * 1.3); xx <= (int)(cx + cr * 1.3); xx++) {
                            double d = std::hypot((xx - cx) / 1.25, yy - cy) / cr;
                            if (d > 1.0 || (d > 0.7 && (hash(xx * 23 + yy * 7) & 1))) continue;
                            RGB col = (hash(xx * 41 + yy * 13) % 7 == 0) ? ct_mix(C[2], C[6], 0.55) : ml;
                            put(xx, yy, ct_mix(get(xx, yy), col, 0.82), 2);
                        }
                }
            }
        }
        // уламок знизу праворуч: природний нерівний силует зі сколами, тріщини, мох, трава і квіти
        int gx = tw_gx = (int)(cols * 0.69), seat = rows - (int)(rows * 0.20);
        tw_gy = seat - 37 * S;
        int bx0 = gx + 3 * S;                                                  // ліве ребро під дівчиною
        std::vector<int> rtop(cols, rows);
        for (int x = bx0 - 4 * S; x < cols; x++) {
            int top = seat;
            if (x < bx0) {
                // нижній відколотий уступ зліва біля основи
                top = seat + (int)((rows - seat) * 0.55) + (bx0 - x) * 2 + (int)(hash((x / S) * 19) % (2 * S + 1));
            } else if (x > gx + 29 * S) {
                int dx = x - (gx + 29 * S);
                int step = (dx / (4 * S)) * S + (int)(hash((x / (2 * S)) * 37 + 11) % (2 * S + 1));
                top = seat + step - (x > cols - 11 * S ? (x - (cols - 11 * S)) / 3 : 0);
            }
            rtop[std::clamp(x, 0, cols - 1)] = top;
            for (int y = top; y < rows; y++) {
                // нерівний лівий край зі сколами та нависаючою плитою зверху
                if (x < bx0 + 4 * S) {
                    int dy = y - seat;
                    int indent = (dy < 4 * S) ? 0 : (dy < 10 * S) ? 2 * S : (dy < 18 * S) ? S : -2 * S;
                    indent += (int)(hash((y / (2 * S)) * 53 + 7) % (2 * S + 1)) - S;
                    if (x < bx0 + indent) continue;
                }
                RGB c = C[9];
                int dy = y - top;
                if (dy == 0) c = C[10];
                else if (dy == 1 && (hash(x * 13) & 1)) c = ct_mix(C[9], C[10], 0.55);
                else if (x <= bx0 + 2 * S && (tw_dep[(size_t)y * cols + std::max(0, x - 1)] < 3))
                    c = ct_mix(C[9], C[10], 0.45);                             // підсвічений скол зліва
                // природні кам'яні шви й гіллясті тріщини (без регулярної сітки крапок)
                int by_blk = (y - seat) / (6 * S), bx_blk = (x - bx0 + (by_blk & 1) * 7 * S) / (13 * S);
                if ((y - seat) % (6 * S) == 0 && (hash(bx_blk * 31 + by_blk) & 3) != 0 && dy > 2 * S)
                    c = ct_mix(C[9], {0, 0, 0}, 0.25);
                int crk1 = (x - (gx + 16 * S)) - dy / 2 - (int)(std::sin(dy * 0.35) * 1.5 * S);
                int crk2 = (x - (gx + 40 * S)) + dy / 2 + (int)(std::cos(dy * 0.28) * 1.5 * S);
                if ((std::abs(crk1) <= (S > 1 ? 1 : 0) && dy < 18 * S) || (std::abs(crk2) <= (S > 1 ? 1 : 0) && dy < 14 * S))
                    c = ct_mix(C[9], {0, 0, 0}, 0.38);
                // мох зверху уламка
                int moss_h = 1 + (int)(hash((x / S) * 23 + 5) % (3 * S));
                if (dy <= moss_h && hashf(x * 17 + y * 3) < 0.55)
                    c = dy == 0 ? ct_mix(C[5], C[10], 0.25) : C[5];
                put(x, y, c, 3);
            }
        }
        // пучки трави та квіточки на уламку (координати зберігаємо, анімуємо з вітром у tower_prepare)
        for (int x = bx0 + S; x < cols - 2 * S; x += 2 * S) {
            if (x >= gx + 4 * S && x <= gx + 27 * S) continue;                 // місце, де сидить дівчина
            uint32_t h = hash((x / S) * 131 + 19);
            if ((h % 5) < 2) {
                int ty = rtop[std::clamp(x, 0, cols - 1)];
                int gh = (2 + (h >> 4) % 4) * S;
                bool flw = ((h >> 8) % 3) == 0;
                tw_tufts.push_back({x, ty, gh, flw, h});
            }
        }
        for (int k = 0; k < 2; k++) {                                          // арматура — з верху уламка
            int ax = cols - (24 + 6 * k) * S, ay = 0;
            while (ay < rows && tw_dep[(size_t)ay * cols + ax] != 3) ay++;
            for (int i = 0; i < 8 * S; i++) put(ax + i / 3, ay - i, C[11], 3);
        }
        // дівчина — дзеркальний спрайт (дивиться ліворуч на вежу)
        const char* const* SPR = girl_spr();
        for (int r = 0; r < 44; r++)
            for (int c = 0; c < 30; c++) {
                char ch = SPR[r][29 - c];
                if (ch == '.') continue;
                RGB col;
                switch (ch) {
                    case 'K': case 'b': col = ct_mix(C[11], C[12], 0.6); break;
                    case 'k': col = ct_mix(C[11], C[14], 0.45); break;
                    case 'h': col = C[12]; break;
                    case 'g': col = ct_mix(C[12], C[13], 0.3); break;
                    case 'E': col = ct_mix(C[11], C[13], 0.22); break;
                    case 'L': col = C[14]; tw_ledx = gx + c * S; tw_ledy = tw_gy + r * S; break;
                    case 'r': col = C[13]; break;
                    case 'S': col = C[12]; break;
                    case 'D': col = ct_mix(C[11], {0, 0, 0}, 0.5); break;
                    case 'P': col = ct_mix(C[11], {0, 0, 0}, 0.2); break;
                    default: col = C[11];
                }
                for (int dy = 0; dy < S; dy++) for (int dx = 0; dx < S; dx++) put(gx + c * S + dx, tw_gy + r * S + dy, col, 4);
            }
        // пишні квітучі гілки з боків (глибина 4–5, грона вздовж усієї гілки, бутони й листочки; верх ~30% спокійний)
        auto branch = [&](double x0, double y0, double ang, double len, double wid, int depth, double flex0) {
            struct B { double x, y, a, l, w, f; int d; };
            std::vector<B> st = {{x0, y0, ang, len, wid, flex0, depth}};
            double min_y = rows * 0.285;
            while (!st.empty()) {
                B b = st.back(); st.pop_back();
                double a = b.a;
                if (b.y + std::sin(a) * b.l < min_y) {
                    a = (std::cos(a) >= 0) ? (0.10 + 0.22 * rf()) : (3.14159 - 0.10 - 0.22 * rf());
                }
                double x1 = b.x + std::cos(a) * b.l, y1 = b.y + std::sin(a) * b.l;
                double f1 = b.f + b.l / (len * 2.5);
                tw_segs.push_back({(float)b.x, (float)b.y, (float)x1, (float)y1, (float)b.f, (float)b.w});
                // густі грона цвіту та листочки вздовж усього сегмента гілки (від самого краю кадру до кінчиків)
                int n_along = (b.d >= 4) ? 4 : (b.d >= 2) ? 2 : 1;
                for (int k = 1; k <= n_along; k++) {
                    double u = (double)k / (n_along + 0.2);
                    double ort = (rf() - 0.5) * (b.d >= 3 ? 3.6 : 2.0) * S;
                    double bx = b.x + (x1 - b.x) * u - std::sin(a) * ort;
                    double by = b.y + (y1 - b.y) * u + std::cos(a) * ort;
                    float bf = (float)(b.f + (f1 - b.f) * u);
                    float br = (float)((b.d >= 3 ? 2.6 + 1.8 * rf() : (b.d == 2 ? 2.0 + 1.4 * rf() : 1.5 + 1.0 * rf())) * S);
                    tw_blooms.push_back({(float)bx, (float)by, br, bf, (uint32_t)(rf() * 1e6), 0});
                    if (rf() < 0.78) {
                        double la = a + (rf() < 0.5 ? 1.0 : -1.0);
                        tw_blooms.push_back({(float)(bx + std::cos(la) * br * 0.9), (float)(by + std::sin(la) * br * 0.9),
                                             (float)((1.4 + 0.8 * rf()) * S), bf, (uint32_t)(rf() * 1e6), 2});
                    }
                }
                if (b.d == 0 || b.l < 2.2 * S) {
                    tw_blooms.push_back({(float)x1, (float)y1, (float)((1.5 + 1.1 * rf()) * S), (float)f1, (uint32_t)(rf() * 1e6), 0});
                    for (int bk = 0; bk < 2; bk++) {
                        double ba = a + (bk == 0 ? 0.28 : -0.32) + (rf() - 0.5) * 0.18;
                        double bd = (2.0 + 1.3 * rf()) * S;
                        tw_blooms.push_back({(float)(x1 + std::cos(ba) * bd), (float)(y1 + std::sin(ba) * bd),
                                             (float)((1.1 + 0.5 * rf()) * S), (float)(f1 + 0.06), (uint32_t)(rf() * 1e6), 1});
                    }
                    continue;
                }
                double sp1 = 0.26 + 0.22 * rf(), sp2 = -0.24 - 0.22 * rf();
                st.push_back({x1, y1, a + sp1, b.l * (0.72 + 0.05 * rf()), std::max((double)S, b.w * 0.70), f1, b.d - 1});
                if (b.d >= 2 || rf() < 0.65)
                    st.push_back({x1, y1, a + sp2, b.l * (0.66 + 0.05 * rf()), std::max((double)S, b.w * 0.68), f1, b.d - 1});
                // короткі бічні пагони біля основи гілки (щоб біля краю кадру було пишно)
                if (b.d >= 4) {
                    double mx = b.x + (x1 - b.x) * 0.45, my = b.y + (y1 - b.y) * 0.45;
                    double sa = a + (rf() < 0.5 ? 0.55 : -0.50);
                    st.push_back({mx, my, sa, b.l * 0.55, std::max((double)S, b.w * 0.62), b.f + 0.08, 2});
                }
            }
        };
        // САКУРА: природні дерева — кривий стовбур, що звужується, рекурсивні криві гілки (5 рівнів, по черзі
        // ліво/право, асиметрія), цвіт нерівними гронцями на кінцях і вздовж верхніх гілок з просвітами неба.
        // Усе розкладено в 3 шари гойдання й кешовано тут; щокадру — лише зсув шарів.
        (void)branch;
        auto layer_of = [](double f) { return f < 0.25 ? 0 : f < 0.6 ? 1 : 2; };
        auto stamp_bark = [&](double x, double y, double th, double nxn, double nyn, double f) {
            int r = std::max(0, (int)std::floor(th / 2)), L = layer_of(f);
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (dx * dx + dy * dy > r * r + r) continue;
                    double side = (dx * nxn + dy * nyn) / std::max(1, r);        // -1 світлий бік (ліво-верх), +1 тіньовий
                    uint8_t tone = th >= 2.5 ? (side < -0.45 ? 6 : side > 0.45 ? 7 : 5) : 5;
                    tw_tree[L].push_back({(int16_t)std::lround(x + dx), (int16_t)std::lround(y + dy), tone});
                }
        };
        std::vector<std::array<double, 4>> clusters;     // x, y, r, flex
        double xmin_l = -1e9, xmax_l = cols * 0.25, xmin_r = cols * 0.74, xmax_r = 1e9;
        std::function<void(double, double, double, double, double, int, double, int)> grow =
            [&](double x, double y, double ang, double len, double th, int depth, double f, int side) {
            int steps = std::max(2, (int)std::lround(len));
            double th1 = th * 0.66;
            for (int i = 0; i < steps; i++) {
                double u = (double)i / steps, w = th + (th1 - th) * u;
                ang += (rf() - 0.5) * 0.16;                                              // кривизна
                if (depth <= 2) ang += 0.012 * (std::cos(ang) >= 0 ? 1 : -1) * (ang < 0 ? 1 : 0.3);   // кінці трохи никнуть
                if (y < rows * 0.06) ang += (std::cos(ang) >= 0 ? 0.12 : -0.12);         // не лізти в самий верх
                if (side == 0 && x > xmax_l) ang += (std::sin(ang) < 0 ? -0.15 : 0.15);  // ліве дерево не заходить на вежу
                if (side == 1 && x < xmin_r) ang += (std::sin(ang) < 0 ? 0.15 : -0.15);
                double nx = std::cos(ang), ny = std::sin(ang);
                double ff = f + u * len / (rows * 0.55);
                stamp_bark(x, y, w, -ny * (nx >= 0 ? 1 : -1), nx * (nx >= 0 ? 1 : -1), ff);
                if (depth <= 2 && i % 3 == 1 && rf() < 0.55)
                    clusters.push_back({x + (rf() - 0.5) * 3, y + (rf() - 0.5) * 3, (1.6 + 1.6 * rf()) * (depth == 0 ? 1.0 : 1.25), ff});
                x += nx; y += ny;
            }
            double fe = f + len / (rows * 0.55);
            if (depth == 0) { clusters.push_back({x, y, 2.6 + 2.4 * rf(), fe}); return; }
            double spread = 0.32 + 0.3 * rf(), longer = rf() < 0.5 ? 1 : -1;
            grow(x, y, ang + spread, len * (0.70 + 0.12 * longer), th1 * 0.82, depth - 1, fe, side);
            grow(x, y, ang - spread * (0.8 + 0.4 * rf()), len * (0.70 - 0.12 * longer), th1 * 0.74, depth - 1, fe, side);
            if (depth >= 3 && rf() < 0.6) grow(x, y, ang + (rf() - 0.5) * 0.3, len * 0.55, th1 * 0.6, depth - 2, fe, side);
        };
        (void)xmin_l; (void)xmax_r;
        double TH = std::max(4.0, rows * 0.040);
        grow(cols * 0.012, rows * 1.03, -1.40, rows * 0.30, TH, 5, 0.0, 0);              // ліве дерево
        grow(cols + 2.0, rows * 0.13, 3.14159 - 0.30, cols * 0.075, TH * 0.75, 4, 0.05, 1);   // права гілка над дівчиною
        // гронця цвіту: нерівні, з дірками, світло згори-зліва, окремі квіточки по краю
        std::sort(clusters.begin(), clusters.end(), [](auto& a, auto& b) { return a[1] > b[1]; });   // нижні — позаду
        uint32_t ck = 0;
        for (auto& q : clusters) {
            double cx = q[0], cy = q[1], R = q[2]; int L = layer_of(q[3] + 0.15); ck++;
            tw_canopy.push_back({(float)cx, (float)cy, (float)R, (float)q[3], ck * 2654435761u, 0});
            int H = (int)std::ceil(R + 2);
            for (int dy = -H; dy <= H; dy++)
                for (int dx = -H; dx <= H; dx++) {
                    double nx = dx / R, ny = dy / (R * 0.9), d = std::sqrt(nx * nx + ny * ny);
                    uint32_t h = hash(ck * 977 + (uint32_t)(dx + 32) * 131 + (uint32_t)(dy + 32) * 7919);
                    double edge = 1.0 + 0.28 * (hashf(ck * 31 + (uint32_t)std::lround(std::atan2(ny, nx) * 3 + 10)) - 0.5);
                    uint8_t tone = 0;
                    if (d <= edge) {
                        if (d > 0.45 && (h & 15) < 2) continue;                          // просвіти
                        double v = -(nx * 0.5 + ny * 0.8) * 0.55 + ((h >> 8 & 255) / 255.0 - 0.5) * 0.6;
                        tone = v > 0.40 ? 4 : v > 0.08 ? 3 : v > -0.30 ? 2 : 1;
                    } else if (d <= edge + 0.6 && (h & 31) < 4) tone = (h >> 6) & 1 ? 3 : 4;   // окремі квіточки
                    if (tone) tw_tree[L].push_back({(int16_t)std::lround(cx + dx), (int16_t)std::lround(cy + dy), tone});
                }
        }
    }
    void tower_prepare(int cols, int rows, double hue, double s) {
        if (cols != tw_w || rows != tw_h || (int)light != tw_light || std::lround(hue) != tw_hue) tower_build(cols, rows, hue, s);
        tw_frame = tw_base;
        static const double BY[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        auto bay = [&](int x, int y) { return (BY[y & 3][x & 3] + 0.5) / 16.0; };
        const RGB* C = tw_c;
        int S = tw_s, hor = tw_hor;
        auto bl = [&](int x, int y, RGB c, double a, int maxdep) {
            if (x < 0 || y < 0 || x >= cols || y >= rows || a <= 0) return;
            size_t p = (size_t)y * cols + x;
            if (tw_dep[p] > maxdep) return;
            tw_frame[p] = ct_pk(ct_mix(ct_un(tw_frame[p]), c, std::min(1.0, a)));
        };
        // рідкі зорі в сутінках (лише темна тема, дуже стримано)
        if (!light)
            for (int y = 0; y < rows / 3; y++)
                for (int x = 0; x < cols; x++) {
                    uint32_t hs = hash(x * 73856093u ^ y * 19349663u ^ 0x7077u);
                    if ((hs & 2047) >= 4) continue;
                    double tw = 0.5 + 0.5 * std::sin(t * (0.4 + (hs >> 11) % 13 * 0.07) + (hs >> 16) % 628 * 0.01);
                    bl(x, y, C[7], (1 - y / (rows / 3.0)) * 0.6 * tw, 0);
                }
        // у світлій темі — ледь помітні дизерні промені світла згори-збоку (нижче верхніх 28% неба)
        if (light) {
            RGB sun_c = ct_mix(C[1], {1.0, 0.99, 0.95}, 0.55);
            for (int y = (int)(rows * 0.25); y < (int)(rows * 0.78); y++) {
                double fy = (double)y / rows;
                double vfade = smooth((fy - 0.25) / 0.14) * (1.0 - smooth((fy - 0.50) / 0.28));
                if (vfade <= 0.02) continue;
                for (int x = 0; x < (int)(cols * 0.75); x++) {
                    double u = (x - y * 0.62) / (cols * 0.085);
                    double beam = 0.5 + 0.3 * fsin(u + t * 0.08) + 0.2 * fsin(u * 2.1 - t * 0.05 + 1.2);
                    double strength = std::max(0.0, beam - 0.62) * 2.6 * vfade * (1.0 - (double)x / (cols * 0.75));
                    if (strength > bay(x, y) * 0.85) bl(x, y, sun_c, 0.22, 2);
                }
            }
        }
        // 2 м'які хмаринки в середній зоні неба (верх ~30% залишається спокійним і чистим)
        for (int ck = 0; ck < 2; ck++) {
            double cy = rows * (0.37 + 0.11 * ck), cw = cols * (0.18 + 0.05 * ck), ch = (2.2 + 0.6 * ck) * S;
            double span = cols + cw * 2;
            double cx = std::fmod(cols * (0.22 + 0.48 * ck) + t * (0.35 + 0.15 * ck) * S, span) - cw;
            RGB cl_top = ct_mix(C[1], C[7], light ? 0.35 : 0.18);
            RGB cl_bot = ct_mix(C[0], C[2], 0.65);
            for (int y = (int)(cy - ch * 2); y <= (int)(cy + ch * 1.5); y++)
                for (int x = (int)(cx - cw * 0.6); x <= (int)(cx + cw * 0.6); x++) {
                    double dx = (x - cx) / (cw * 0.5), dy = (y - cy) / ch;
                    double puff = 0.35 * fsin(dx * 4.2 + ck * 2.1) + 0.2 * fcos(dx * 7.5 - ck);
                    double d = dx * dx + (dy + puff * (dy < 0 ? 0.7 : 0.2)) * (dy + puff * (dy < 0 ? 0.7 : 0.2));
                    if (d >= 1.0) continue;
                    if (d > 0.55 && bay(x, y) > (1.0 - d) * 2.2) continue;
                    bl(x, y, dy < 0.15 ? cl_top : cl_bot, light ? 0.55 : 0.38, 0);
                }
        }
        // вежа: у темній темі — тепле віконце, що м'яко мерехтить; лоза з балкона-кільця ледь гойдається
        if (!light) {
            double flk = 0.68 + 0.22 * std::sin(t * 2.1) + 0.10 * std::cos(t * 5.3 + 0.8);
            RGB w_core = {1.0, 0.82, 0.42}, w_halo = {0.95, 0.58, 0.25};
            for (int dy = -2 * S; dy <= 3 * S; dy++)
                for (int dx = -2 * S; dx <= 2 * S; dx++) {
                    double d = std::hypot(dx / (double)S, (dy - 0.5 * S) / (double)S);
                    if (std::abs(dx) <= (dy == -S ? 0 : S) && dy >= -S && dy <= 2 * S)
                        bl(tw_winx + dx, tw_winy + dy, w_core, 0.85 * flk, 1);
                    else if (d < 2.8)
                        bl(tw_winx + dx, tw_winy + dy, w_halo, (1.0 - d / 2.8) * 0.28 * flk, 1);
                }
        }
        for (int vk = 0; vk < 5; vk++) {
            int vx0 = tw_tx - tw_hw0 + (vk * tw_hw0 * 2) / 4;
            int vlen = (5 + (hash(vk * 37 + 3) % 7)) * S;
            for (int dy = 1; dy <= vlen; dy++) {
                double u = (double)dy / vlen;
                int sway = (int)std::lround(std::sin(t * 0.9 + vk * 1.4 + u * 2.2) * (1.2 * u * S));
                RGB vc = (dy == vlen || (dy % (3 * S) == 0 && (vk & 1))) ? C[6] : ct_mix(C[5], C[2], 0.30);
                bl(vx0 + sway, tw_ring + 1 + dy, vc, 0.9, 1);
            }
        }
        // туман: три смуги, що повільно пливуть (одна перетинає вежу)
        for (int k = 0; k < 3; k++) {
            static const double YY[3] = {0.50, 0.64, 0.71};
            double cy = rows * YY[k], hh = rows * (0.018 + 0.01 * k), sp = (0.4 + 0.25 * k) * S;
            for (int y = (int)(cy - hh * 2); y <= (int)(cy + hh * 2); y++)
                for (int x = 0; x < cols; x++) {
                    double n = 0.5 + 0.3 * std::sin((x + t * sp) * 0.035 + k * 2) + 0.2 * std::sin((x - t * sp * 0.6) * 0.09 + k);
                    double a = std::exp(-std::pow((y - cy) / hh, 2)) * n;
                    if (bay(x, y) < a * 0.75) bl(x, y, C[2], 0.45, 2);
                }
        }
        // птахи біля вершини: «v», що махають крилами
        for (int i = 0; i < 3; i++) {
            double a = t * (0.18 + 0.05 * i) + i * 2.1;
            int bx = (int)std::lround(tw_tx + std::cos(a) * rows * (0.10 + 0.03 * i));
            int by = (int)std::lround(tw_ttop - rows * 0.04 + std::sin(a * 1.7) * rows * 0.02 - i * 2);
            bool up = std::fmod(t * 3 + i, 1.0) < 0.5;
            RGB bc = ct_mix(C[11], C[2], 0.35);
            bl(bx, by, bc, 1, 1);
            bl(bx - 1, by + (up ? -1 : 0), bc, 1, 1); bl(bx + 1, by + (up ? -1 : 0), bc, 1, 1);
            if (S > 1) { bl(bx - 2, by + (up ? -2 : 1), bc, 1, 1); bl(bx + 2, by + (up ? -2 : 1), bc, 1, 1); }
        }
        // пучки трави та квіточки на уламку, що гойдаються від вітру
        for (auto& tf : tw_tufts) {
            double w_sway = std::sin(t * 1.3 + tf.x * 0.15) * 0.8 - 0.5;       // вітер ліворуч
            for (int b = -1; b <= 1; b++) {
                int bh = std::max(S, tf.h - std::abs(b) * S);
                for (int dy = 1; dy <= bh; dy++) {
                    double u = (double)dy / bh;
                    int gx_p = tf.x + b * S + (int)std::lround((w_sway + b * 0.4) * u * S);
                    int gy_p = tf.y - dy;
                    RGB gc = u > 0.65 ? ct_mix(C[5], C[10], 0.35) : C[5];
                    bl(gx_p, gy_p, gc, 0.95, 3);
                    if (b == 0 && dy == bh && tf.flower) {
                        RGB fc = (tf.seed & 1) ? C[6] : C[7];
                        bl(gx_p, gy_p - 1, fc, 1.0, 3);
                        if (S > 1) bl(gx_p - 1, gy_p - 1, fc, 0.85, 3);
                    }
                }
            }
        }
        // 3 пасма волосся дівчини ззаду/на потилиці, у кольорі волосся, ледь гойдаються від вітру (поверх girl_spr)
        for (int st = 0; st < 3; st++) {
            int hx0 = tw_gx + (st == 1 ? 23 : 22) * S, hy0 = tw_gy + (10 + st * 2) * S;
            int hlen = (3 + (st == 1 ? 1 : 0)) * S;
            double wave = std::sin(t * 1.6 + st * 1.2);
            for (int i = 1; i <= hlen; i++) {
                double u = (double)i / hlen;
                int px = hx0 + i;
                int py = hy0 + (int)std::lround((u * 1.4 + wave * 0.55 * u) * S);
                for (int dy = 0; dy < S; dy++) bl(px, py + dy, C[12], 0.98, 9);
            }
        }
        // гілки, листочки, бутони й цвіт із гойданням (гілки товщі біля краю кадру, проплітаються крізь цвіт)
        double sw = std::sin(t * 0.55), sw2 = std::sin(t * 0.9 + 1.3);
        auto off = [&](float f) { return std::make_pair((sw * 1.6 + sw2 * 0.5) * f * S, (sw2 * 0.8) * f * S); };
        RGB bark_hi = ct_mix(C[15], C[3], 0.28);
        RGB fl_mid = ct_mix(C[6], C[14], 0.40), fl_shd = ct_mix(C[6], C[15], 0.32), bud_c = ct_mix(C[6], C[14], 0.62);
        RGB leaf_hi = ct_mix(C[5], C[7], 0.18), leaf_dk = ct_mix(C[5], C[15], 0.35);
        auto draw_bloom = [&](const TwBloom& b) {
            auto [ox, oy] = off(b.w);
            double cx = b.x + ox, cy = b.y + oy;
            if (b.kind == 2) {
                double ang = (b.seed % 628) * 0.01, cs = std::cos(ang), sn = std::sin(ang);
                int r = (int)std::ceil(b.r) + 1;
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        double u = (dx * cs + dy * sn) / b.r, v = (-dx * sn + dy * cs) / (b.r * 0.52);
                        if (u * u + v * v <= 1.0)
                            bl((int)cx + dx, (int)cy + dy, v < 0 ? leaf_hi : (u > 0.3 ? leaf_dk : C[5]), 0.95, 9);
                    }
            } else if (b.kind == 1) {
                int r = std::max(S, (int)std::lround(b.r));
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        if (dx * dx + dy * dy > r * r + 1) continue;
                        RGB bc = (dy > 0) ? C[5] : (dx == 0 && dy == -r ? C[7] : bud_c);
                        bl((int)cx + dx, (int)cy + dy, bc, 1, 9);
                    }
            } else {
                for (int y = (int)(cy - b.r - 1); y <= (int)(cy + b.r + 1); y++)
                    for (int x = (int)(cx - b.r - 1); x <= (int)(cx + b.r + 1); x++) {
                        double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy) / b.r;
                        if (d >= 1.05) continue;
                        uint32_t h = hash(b.seed + (uint32_t)(x - (int)cx + 16) * 31 + (uint32_t)(y - (int)cy + 16) * 977);
                        if (d > 0.72 && (h & 3) == 0) continue;
                        if (d > 0.82 && (h % 5 == 0)) {
                            bl(x, y, (h & 4) ? C[5] : leaf_dk, 0.92, 9);
                            continue;
                        }
                        uint32_t sel = (h >> 3) % 8;
                        RGB c = (sel <= 1) ? C[7] : (sel <= 4) ? C[6] : (sel <= 6) ? fl_mid : bud_c;
                        if (x + 0.5 < cx && y + 0.5 > cy && d > 0.42) c = fl_shd;
                        bl(x, y, c, 1, 9);
                    }
            }
        };
        for (auto& b : tw_blooms) if (b.kind != 1 && (b.seed & 3) != 0) draw_bloom(b);
        for (auto& g : tw_segs) {
            double len = std::hypot(g.x1 - g.x0, g.y1 - g.y0);
            int n = std::max(1, (int)std::ceil(len));
            for (int i = 0; i <= n; i++) {
                double u = (double)i / n, f = g.w + u * len / (rows * 0.4);
                int wdt = std::max(1, (int)std::lround(g.th * (1.0 - 0.16 * u)));
                auto [ox, oy] = off((float)f);
                double x = g.x0 + (g.x1 - g.x0) * u + ox, y = g.y0 + (g.y1 - g.y0) * u + oy;
                for (int dy = 0; dy < wdt; dy++)
                    for (int dx = 0; dx < wdt; dx++)
                        bl((int)x + dx, (int)y + dy, (dy == 0 && wdt >= 2 * S) ? bark_hi : C[15], 1, 9);
            }
        }
        for (auto& b : tw_blooms) if (b.kind == 1 || (b.seed & 3) == 0) draw_bloom(b);
        // крона сакури: кожна маса — 4 тони (тінь, основний, світлий, відблиск), світло згори-зліва,
        // фактура «гронами» 2×2, рваний дизерний край, кілька пасм цвіту звисають униз
        RGB sk_sh = light ? hsl_rgb(330, 0.32, 0.66) : hsl_rgb(322, 0.30, 0.36);
        RGB sk_md = light ? hsl_rgb(340, 0.55, 0.80) : hsl_rgb(338, 0.45, 0.58);
        RGB sk_lt = light ? hsl_rgb(345, 0.62, 0.88) : hsl_rgb(342, 0.55, 0.72);
        RGB sk_hi = light ? hsl_rgb(350, 0.70, 0.95) : hsl_rgb(348, 0.60, 0.86);
        const RGB SK[5] = {{0, 0, 0}, sk_sh, sk_md, sk_lt, sk_hi};
        {   // дерева: 3 шари з різним гойданням (стовбур нерухомий, кінчики — найбільше)
            const RGB TT[8] = {{0, 0, 0}, sk_sh, sk_md, sk_lt, sk_hi, C[15], ct_mix(C[15], C[3], light ? 0.45 : 0.35), ct_mix(C[15], {0, 0, 0}, 0.35)};
            static const float LF[3] = {0.0f, 0.45f, 1.0f};
            for (int L = 0; L < 3; L++) {
                auto [ox, oy] = off(LF[L]);
                int dx = (int)std::lround(ox), dy = (int)std::lround(oy);
                for (const auto& q : tw_tree[L]) bl(q.x + dx, q.y + dy, TT[q.tone], 1, 9);
            }
        }
        for (size_t bi = 0; bi < tw_canopy.size() && bi < tw_cspr.size(); bi++) {
            const auto& b = tw_canopy[bi]; const auto& m = tw_cspr[bi];
            auto [ox, oy] = off(b.w);
            int H = (int)std::ceil(b.r * 1.2), D = 2 * H + 1;
            int x0 = (int)b.x - H + (int)std::lround(ox), y0 = (int)b.y - H + (int)std::lround(oy);
            for (int j = 0; j < D; j++)
                for (int i = 0; i < D; i++) { uint8_t tn = m[j * D + i]; if (tn) bl(x0 + i, y0 + j, SK[tn], 1, 9); }
            for (int k = 0; k < 2; k++) {                                              // 1–2 короткі звислі пасма
                if ((b.seed >> (k + 3)) & 1) continue;
                int hx = x0 + H + (int)(b.r * (k ? 0.35 : -0.3)), hy = y0 + H + (int)(b.r * 0.8);
                int len = (int)((1 + (b.seed >> (k * 3)) % 3) * S);
                for (int i = 0; i < len; i++) bl(hx, hy + i, i < S ? sk_md : sk_lt, 0.9, 9);
            }
        }
        // пелюстки: частина зривається безпосередньо з квітучих гілок, решта кружляє в потоці вітру
        for (int i = 0; i < 28; i++) {
            double x, y;
            if (i < 16 && !tw_canopy.empty()) {
                const auto& b = tw_canopy[(i * 5) % tw_canopy.size()];
                auto [ox, oy] = off(b.w);
                double dur = 7.0 + 3.0 * hashf(i * 23 + 5);
                double age = std::fmod(t + hashf(i * 41 + 9) * dur, dur);
                double vx = -(3.2 + 2.5 * hashf(i * 11 + 2)) * S;
                double vy = (4.2 + 3.0 * hashf(i * 7 + 1)) * S;
                x = b.x + ox + age * vx + std::sin(t * 1.6 + i * 1.3) * 2.2 * S;
                y = b.y + oy + b.r * 0.6 + age * vy + std::cos(t * 1.1 + i) * 1.0 * S;
            } else {
                double vy = (5 + 4 * hashf(i * 7 + 1)) * S, vx = -(2 + 3 * hashf(i * 11 + 2)) * S;
                double spanx = cols + 20, spany = rows + 10;
                x = std::fmod(hashf(i * 13 + 3) * spanx + t * vx, spanx); if (x < 0) x += spanx; x -= 10;
                y = std::fmod(hashf(i * 17 + 4) * spany + t * vy, spany) - 5;
                x += std::sin(t * (1.2 + hashf(i)) + i) * 2.5 * S;
            }
            RGB c = i % 3 ? (light ? hsl_rgb(340, 0.55, 0.82) : hsl_rgb(338, 0.45, 0.62)) : (light ? hsl_rgb(350, 0.7, 0.95) : hsl_rgb(348, 0.6, 0.84));
            int sz = (i % 4 == 0 ? 2 : 1) * S;
            bool flip = std::fmod(t * 2 + i * 0.37, 1.0) < 0.5;                     // пелюстка перевертається
            for (int dy = 0; dy < sz; dy++) for (int dx = 0; dx < (flip && sz > S ? S : sz); dx++) bl((int)x + dx, (int)y + dy, c, 0.9, 9);
        }
        // світлодіод навушників «дихає»
        {
            double pu = 0.5 + 0.5 * std::sin(t * 1.6);
            for (int dy = 0; dy < S; dy++) for (int dx = 0; dx < S; dx++)
                bl(tw_ledx + dx, tw_ledy + dy, ct_mix(C[14], {1, 1, 1}, 0.45 * pu), 1, 9);
        }
        (void)hor;
    }
    uint32_t tower_px(int x, int y, double dim) const {
        uint32_t v = tw_frame[(size_t)y * tw_w + x];
        if (dim > 0.999) return v;
        RGB c = ct_un(v);
        return ct_pk({c.r * dim, c.g * dim, c.b * dim});
    }

    // ---- «Розлом» (riftportal): крупнопіксельний смугастий вихор (5 відтінків) і квадратні частинки ----
    struct RpPart { float fx, fy; int lv; };
    RpPart rp_pts[14] = {};
    float rp_cx[3] = {}, rp_cy[3] = {}, rp_breathe = 0.0f;
    uint32_t rp_pal[5] = {};
    void rift_prepare(double hue, double s, double dim) {
        // РІВНО 5 квантованих відтінків фіолетового від акценту (темний -> світло-бузковий), без білих ліній і glow
        double d_violet = std::fmod(270.0 - hue + 540.0, 360.0) - 180.0;
        double base_h = hue + d_violet * 0.72;
        // кожні ~1 с смуги ледь «дихають» (період ~1 с -> кутова частота ~6.0 рад/с)
        double br = std::sin(t * 6.0);
        rp_breathe = (float)(0.065 * br);
        static const double DK_L[5] = {0.11, 0.22, 0.36, 0.52, 0.69};
        static const double LT_L[5] = {0.54, 0.64, 0.74, 0.83, 0.91};
        static const double H_OFF[5] = {-8.0, -4.0, 0.0, 4.0, 8.0};
        static const double DK_S[5] = {0.62, 0.67, 0.70, 0.66, 0.58};
        static const double LT_S[5] = {0.55, 0.53, 0.50, 0.46, 0.42};
        for (int i = 0; i < 5; i++) {
            double hh = base_h + H_OFF[i] + (hue_lo + hue_hi) * 0.12;
            double ss = (light ? LT_S[i] : DK_S[i]) * (0.65 + 0.35 * s);
            double ll = (light ? LT_L[i] : DK_L[i]) + (light ? -0.010 : 0.012) * br;
            RGB c = hsl_rgb(hh, std::clamp(ss, 0.35, 0.80), std::clamp(ll, 0.05, 0.95));
            rp_pal[i] = ct_pk({c.r * dim, c.g * dim, c.b * dim});
        }
        // 3 центри спірального вихору, рознесені по екрану
        rp_cx[0] = (float)(-0.82 + 0.14 * std::cos(t * 0.23));
        rp_cy[0] = (float)(-0.22 + 0.14 * std::sin(t * 0.29));
        rp_cx[1] = (float)( 0.80 + 0.14 * std::sin(t * 0.26 + 1.4));
        rp_cy[1] = (float)( 0.24 + 0.14 * std::cos(t * 0.21 + 0.9));
        rp_cx[2] = (float)( 0.02 + 0.15 * std::sin(t * 0.31 + 2.5));
        rp_cy[2] = (float)( 0.04 + 0.15 * std::cos(t * 0.27 + 2.0));
        // крупні квадратні частинки (2×2 «пікселі»), що повільно піднімаються й згасають сходинками палітри
        for (int p = 0; p < 14; p++) {
            uint32_t ph = hash(p * 7919u + 0x91F7u);
            double dur = 5.0 + (ph & 255) / 255.0 * 2.6;
            double u = std::fmod(t + ((ph >> 8) & 1023) / 1024.0 * dur, dur) / dur;
            double lane = (p + 0.22 + ((ph >> 18) & 255) / 255.0 * 0.56) / 14.0;
            double x0 = 0.04 + lane * 0.90;
            double y0 = 0.24 + ((ph % 997) / 997.0) * 0.70;
            double sway = 0.015 * std::sin(t * 0.85 + p * 1.7);
            double fy = y0 - u * 0.46;
            int lv = (u < 0.10) ? 3 : (u < 0.55) ? 4 : (u < 0.82) ? 3 : (u < 0.94) ? 2 : 0;
            rp_pts[p] = {(float)(x0 + sway), (float)fy, lv};
        }
    }
    uint32_t rift_px(int x, int y, int cols, int rows, double bay) const {
        (void)bay;
        // крупний піксель: власна сітка 4×4 клітинки на один «піксель» порталу, без згладжування і дизеру
        const int B = 4;
        int bx = x / B, by = y / B;
        int bcols = std::max(1, (cols + B - 1) / B);
        int brows = std::max(1, (rows + B - 1) / B);
        double sc = 2.0 / std::max(1, brows);
        double px = (bx + 0.5 - bcols * 0.5) * sc;
        double py = (by + 0.5 - brows * 0.5) * sc;
        // початковий domain-warp для плавної течії смуг
        double wx0 = px + 0.18 * fsin(py * 2.0 + t * 0.38) + 0.10 * fcos(px * 1.6 - t * 0.29);
        double wy0 = py + 0.18 * fcos(px * 1.8 - t * 0.34) + 0.10 * fsin(py * 2.2 + t * 0.32);
        // закручування в полярних координатах довкола 3 незалежних центрів спіралі
        static const double SWIRL[3] = {5.8, -5.4, 5.0};
        static const double RAD[3] = {1.35, 1.35, 1.45};
        double wx = wx0, wy = wy0;
        for (int k = 0; k < 3; k++) {
            double dx = wx0 - rp_cx[k], dy = wy0 - rp_cy[k];
            double r = std::sqrt(dx * dx + dy * dy);
            double ang = fatan2(dy, dx);
            double twist = SWIRL[k] * std::exp(-r * RAD[k]);
            wx += (rp_cx[k] + r * fcos(ang + twist)) - wx0;
            wy += (rp_cy[k] + r * fsin(ang + twist)) - wy0;
        }
        // широкі спіральні смуги з рівномірною шириною сходинок (трикутно-синусна хвиля у спільній фазі)
        double phase = wx * 3.2 + wy * 2.3 - t * 0.72 + 0.45 * fsin(wx * 1.8 - wy * 2.0 + t * 0.48);
        double cyc = phase * 0.15915494309189535 + 0.25;
        cyc -= std::floor(cyc);
        double tri = 1.0 - std::fabs(2.0 * cyc - 1.0);
        double v = 0.65 * tri + 0.35 * (0.5 + 0.5 * fsin(phase)) + rp_breathe;
        // квантування рівно в 5 рівнів (0..4) — чіткі «сходинки» широких смуг
        int qi = std::clamp((int)std::floor(v * 5.0), 0, 4);
        // поверх — крупні квадратні частинки 2×2 «пікселі» (на тій самій сітці bx, by)
        for (int p = 0; p < 14; p++) {
            if (rp_pts[p].lv <= 0) continue;
            int pbx = (int)std::floor(rp_pts[p].fx * bcols);
            int pby = (int)std::floor(rp_pts[p].fy * brows);
            if ((bx == pbx || bx == pbx + 1) && (by == pby || by == pby + 1)) {
                qi = (qi >= rp_pts[p].lv) ? std::min(4, qi + 1) : rp_pts[p].lv;
                break;
            }
        }
        return rp_pal[qi];
    }

    // ---- «Зоряна безодня» (voidfield): політ у безодню — 3D-зоряне поле з перспективою ----
    // Кожна зірка має (x, y, z); камера повільно летить уперед (z рівномірно зменшується), проєкція
    // sx = cx + (x − камера)/z·f. Стану немає: z і місце народження — функції часу та номера «циклу» зірки,
    // тож кадр детермінований для будь-якого t (постери). Яскравість і розмір ∝ 1/z (субклітинкове AA),
    // у близьких — радіальний шлейф назовні; центр — темна пустота з пилом, навколо — туманне кільце
    // (дизер Байєра, 3 тони), що повільно обертається; віньєтка. Буфери перевиділяються лише при зміні розміру.
    static constexpr int VF_N = 1000;
    struct VfStar { float ph, tw_sp, tw_ph; };
    VfStar vf_st[VF_N] = {};
    bool vf_built = false;
    int vf_w = 0, vf_h = 0;
    std::vector<float> vf_acc;        // на клітинку: r·a, g·a, b·a, a — накопичення зірок
    std::vector<uint32_t> vf_frame;
    void void_prepare(int cols, int rows, double hue, double s, double dim) {
        const double NEAR = 0.05, FAR = 2.4, V = 0.075, RNG = FAR - NEAR;   // V — одиниць глибини за секунду
        if (!vf_built) {
            vf_built = true;
            for (int i = 0; i < VF_N; i++) {
                uint32_t h = hash((uint32_t)i * 0x9E3779B1u + 0x27D4EB2Fu), h2 = hash(h ^ 0xA5A5A5A5u);
                vf_st[i].ph = (float)((h & 0xFFFFFF) / 16777216.0);
                vf_st[i].tw_sp = (float)(0.6 + (h2 & 255) / 255.0 * 1.6);
                vf_st[i].tw_ph = (float)(((h2 >> 8) & 1023) * 0.0061359);
            }
        }
        if (vf_w != cols || vf_h != rows || vf_frame.size() != (size_t)cols * rows) {
            vf_w = cols; vf_h = rows;
            vf_acc.assign((size_t)cols * rows * 4, 0.0f);
            vf_frame.assign((size_t)cols * rows, 0u);
        } else std::fill(vf_acc.begin(), vf_acc.end(), 0.0f);

        // Палітра глибини: 0 — найближчі (майже білі з відтінком ціану), 15 — найдальші (синьо-фіолетові)
        double acc_pull = std::clamp(std::fmod(hue - 216.0 + 540.0, 360.0) - 180.0, -30.0, 30.0) * 0.25;
        double h_near = 184.0 + acc_pull * 0.5 + hue_hi * 0.3, h_far = 252.0 + acc_pull + hue_lo * 0.3;
        RGB pal[16];
        for (int j = 0; j < 16; j++) {
            double k = j / 15.0;
            double hh = h_near + (h_far - h_near) * k;
            double sat = std::clamp(0.30 + 0.55 * std::min(1.0, k * 2.5) + (s - 0.65) * 0.2, 0.2, 0.95);
            double l = 0.93 - 0.30 * std::sqrt(k);
            pal[j] = light ? hsl_rgb(hh, std::min(0.95, sat + 0.15), lt(l)) : hsl_rgb(hh, sat, l);
        }
        RGB c_flare = light ? hsl_rgb(h_near - 8.0, 0.8, 0.14) : hsl_rgb(h_near - 8.0, 0.45, 0.97);

        float* A = vf_acc.data();
        // Квадрат півшириною hs з центром (px, py) — площа перекриття з клітинками (AA); великі — з округленими кутами
        auto splat = [&](double px, double py, double hs, const RGB& c, double w) {
            if (w <= 0.004) return;
            int x0 = (int)std::floor(px - hs), x1 = (int)std::floor(px + hs);
            int y0 = (int)std::floor(py - hs), y1 = (int)std::floor(py + hs);
            if (x1 < 0 || y1 < 0 || x0 >= cols || y0 >= rows) return;
            x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, cols - 1); y1 = std::min(y1, rows - 1);
            double rr = hs * 1.2 + 0.5;
            for (int yy = y0; yy <= y1; yy++) {
                double oy = std::min(py + hs, yy + 1.0) - std::max(py - hs, (double)yy);
                if (oy <= 0) continue;
                for (int xx = x0; xx <= x1; xx++) {
                    double ox = std::min(px + hs, xx + 1.0) - std::max(px - hs, (double)xx);
                    if (ox <= 0) continue;
                    double a = ox * oy * w;
                    if (hs > 0.9) {
                        double ddx = xx + 0.5 - px, ddy = yy + 0.5 - py;
                        a *= std::clamp(rr - std::sqrt(ddx * ddx + ddy * ddy), 0.0, 1.0);
                    }
                    float* p = A + ((size_t)yy * cols + xx) * 4;
                    p[0] += (float)(c.r * a); p[1] += (float)(c.g * a); p[2] += (float)(c.b * a); p[3] += (float)a;
                }
            }
        };

        // Точка сходу ледь пливе (ліссажу), камера трохи зсувається вбік — живий 3D-паралакс
        double vcx = cols * (0.5 + 0.05 * std::sin(t * 0.037) + 0.02 * std::sin(t * 0.061 + 2.0));
        double vcy = rows * (0.47 + 0.045 * std::sin(t * 0.029 + 1.3));
        double camx = 0.06 * std::sin(t * 0.021 + 0.7), camy = 0.04 * std::sin(t * 0.027);
        double f = rows * 0.42;
        for (int i = 0; i < VF_N; i++) {
            const VfStar& S = vf_st[i];
            double u = t * V / RNG + S.ph;
            double cyc = std::floor(u);
            double z = FAR - (u - cyc) * RNG;
            uint32_t h = hash((uint32_t)i * 0x9E3779B1u ^ (uint32_t)(int64_t)cyc * 0x85EBCA77u);
            uint32_t h2 = hash(h ^ 0x68E31DA4u);
            double wx = ((h & 0xFFFF) / 65535.0 - 0.5) * 3.0;
            double wy = (((h >> 16) & 0xFFFF) / 65535.0 - 0.5) * 1.8;
            if (std::fabs(wx) < 0.04 && std::fabs(wy) < 0.04) wx = wx < 0 ? -0.06 : 0.06;
            double iz = 1.0 / z;
            double hx = vcx + (wx - camx) * iz * f, hy = vcy + (wy - camy) * iz * f;
            double izt = 1.0 / std::min(FAR, z + V * 0.6);                 // де зірка була ~0.6 с тому
            double tx = vcx + (wx - camx) * izt * f, ty = vcy + (wy - camy) * izt * f;
            if ((hx < -3 && tx < -3) || (hx > cols + 3 && tx > cols + 3) || (hy < -3 && ty < -3) || (hy > rows + 3 && ty > rows + 3))
                continue;
            double fin = std::clamp((FAR - z) / 0.35, 0.0, 1.0);           // плавна поява вдалині
            double b = std::min(1.0, std::pow(0.30 * iz, 1.5)) * fin * fin;   // далекі — майже фон
            b *= 0.80 + 0.20 * fsin(t * S.tw_sp + S.tw_ph);
            double fl = 0.0;                                                // рідкісний м'який спалах
            if ((h2 & 1023) < 12) {
                double zc = 0.32 + ((h2 >> 10) & 255) / 255.0 * 0.4;
                double q = 1.0 - std::fabs(z - zc) / 0.06;
                if (q > 0) fl = q * q * (3.0 - 2.0 * q);
            }
            int pj = std::clamp((int)((z - 0.12) / 1.5 * 15.0 + 0.5), 0, 15);
            const RGB& c = pal[pj];
            double hs = std::clamp(0.30 + 0.12 * iz, 0.5, 1.55);
            // Радіальний шлейф назовні від точки сходу (хвіст — ближче до центру), згасає до кінця
            double tdx = tx - hx, tdy = ty - hy, len = std::sqrt(tdx * tdx + tdy * tdy);
            double tk = std::clamp((0.75 - z) / 0.4, 0.0, 1.0);
            if (len > 0.8 && tk > 0.0) {
                int n = std::min(80, (int)(len * 2.0) + 1);
                double wst = b * tk * 0.85 * len / n;
                for (int j = 1; j <= n; j++) {
                    double p = (double)j / n, g = 1.0 - p;
                    splat(hx + tdx * p, hy + tdy * p, 0.5, c, wst * g * g);
                }
            }
            splat(hx, hy, hs, fl > 0.0 ? ct_mix(c, c_flare, fl) : c, std::min(1.0, b + fl));
            if (fl > 0.02)
                for (int a = 1; a <= 4; a++) {
                    double o = hs + a - 0.5, w = fl * 0.7 * (1.0 - a / 5.0);
                    splat(hx + o, hy, 0.5, c_flare, w); splat(hx - o, hy, 0.5, c_flare, w);
                    splat(hx, hy + o, 0.5, c_flare, w); splat(hx, hy - o, 0.5, c_flare, w);
                }
        }

        // Фон: майже чорний з ледь бірюзовим; пустота в центрі, туманне кільце (3 тони дизеру), пил, віньєтка
        RGB tone[4];
        if (!light) {
            tone[0] = hsl_rgb(185.0 + acc_pull * 0.2, 0.55, 0.030);
            tone[1] = hsl_rgb(h_far - 8.0, 0.50, 0.052);
            tone[2] = hsl_rgb(h_far - 28.0, 0.50, 0.076);
            tone[3] = hsl_rgb(h_near + 18.0, 0.45, 0.104);
        } else {
            tone[0] = hsl_rgb(185.0 + acc_pull * 0.2, 0.22, 0.950);
            tone[1] = hsl_rgb(h_far - 8.0, 0.22, 0.938);
            tone[2] = hsl_rgb(h_far - 28.0, 0.24, 0.924);
            tone[3] = hsl_rgb(h_near + 18.0, 0.22, 0.908);
        }
        const RGB& dust_c = pal[13];
        double R0 = rows * 0.13, Rn = rows * 0.30, Rw = rows * 0.17;
        double iR0 = 1.0 / (R0 * 2.2), iRw = 1.0 / Rw, iRd = 1.0 / (R0 * 2.6);
        double ca = std::cos(t * 0.017), sa = std::sin(t * 0.017);
        for (int y = 0; y < rows; y++) {
            double dy = y + 0.5 - vcy, ey = (y + 0.5) / rows * 2.0 - 1.0;
            uint32_t* out = vf_frame.data() + (size_t)y * cols;
            const float* P = A + (size_t)y * cols * 4;
            for (int x = 0; x < cols; x++, P += 4) {
                double dx = x + 0.5 - vcx, d = std::sqrt(dx * dx + dy * dy);
                static const uint8_t VB[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
                double bay = (VB[y & 3][x & 3] + 0.5) / 16.0;
                int ti = 0;
                double rr = (d - Rn) * iRw;
                if (rr > -1.0 && rr < 1.0) {
                    double ring = 1.0 - rr * rr; ring *= ring;
                    double u = dx * ca - dy * sa, v = dx * sa + dy * ca;
                    double n = 0.5 + 0.5 * fsin(u * 0.05 + 1.8 * fsin(v * 0.034 + 0.7));
                    n *= 0.55 + 0.45 * fsin(v * 0.07 - u * 0.025 + 2.0);
                    ti = std::clamp((int)(ring * n * 3.2 + bay), 0, 3);
                }
                RGB c = tone[ti];
                double vd = std::min(1.0, d * iR0); vd = vd * vd * (3.0 - 2.0 * vd);
                double ex = (x + 0.5) / cols * 2.0 - 1.0;
                double vig = 1.0 - 0.30 * (ex * ex * 0.8 + ey * ey);
                if (!light) {
                    double k = (0.35 + 0.65 * vd) * vig;
                    c = {c.r * k, c.g * k, c.b * k};
                } else {
                    double k = 0.6 + 0.4 * vd, e = 0.92 + 0.08 * vig;
                    c = {(1.0 - (1.0 - c.r) * k) * e, (1.0 - (1.0 - c.g) * k) * e, (1.0 - (1.0 - c.b) * k) * e};
                }
                // Дрібний пил біля точки сходу: густішає до центру, ледь мерехтить
                double dz = 1.0 - std::min(1.0, d * iRd);
                if (dz > 0.0) {
                    uint32_t hh = hash((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ 0x5bd1e995u);
                    if ((hh & 255) < dz * 40.0) {
                        double tw = 0.5 + 0.5 * fsin(t * (0.4 + ((hh >> 8) & 15) * 0.08) + ((hh >> 12) & 1023) * 0.0061);
                        c = ct_mix(c, dust_c, (0.05 + 0.08 * tw) * dz);
                    }
                }
                double a = P[3];
                if (a > 0.003) {
                    double ia = 1.0 / a;
                    c = ct_mix(c, RGB{P[0] * ia, P[1] * ia, P[2] * ia}, std::min(1.0, a));
                }
                out[x] = ct_pk({c.r * dim, c.g * dim, c.b * dim});
            }
        }
    }
    uint32_t void_px(int x, int y, int cols, int rows, double bay) const {
        (void)cols; (void)rows; (void)bay;
        return vf_frame[(size_t)y * vf_w + x];
    }

    // ---- Skyline (sk_*): піксельний Nissan-подібний спорткупе їде дорогою (вид збоку); повз — ліхтарі, дерева, знаки,
    //      зупинки, будівлі та зустрічні авто. Без буферів: усе аналітично, колір — з малої палітри sk_c. ----
    enum { SKY0 = 0, SKFAR = 8, SKWIN = 9, SKWIN2 = 10, SKTREE = 11, SKTREE2 = 12, SKPOLE = 13, SKGLOW = 14, SKROAD = 15,
           SKROAD2 = 16, SKLANE = 17, SKKERB = 18, SKB0 = 19, SKB1 = 20, SKB2 = 21, SKB3 = 22, SKGLASS = 23, SKGLASS2 = 24,
           SKTIRE = 25, SKRIM = 26, SKTAIL = 27, SKHEAD = 28, SKBEAM = 29, SKSTAR = 30, SKNEON = 31, SKSIGN = 32,
           SKONC = 33, SKSHEL = 34 };
    uint32_t sk_c[36] = {};
    int sk_gy = 0; double sk_k = 1, sk_ws = 9;
    static uint32_t sk_pack(RGB c) {
        auto q = [](double v) { return (uint32_t)(std::clamp(v, 0.0, 1.0) * 255); };
        return 0xFF000000u | q(c.r) << 16 | q(c.g) << 8 | q(c.b);
    }
    void skyline_prepare(int cols, int rows, double hue, double s, double dim) {
        sk_gy = (int)(rows * 0.80);
        sk_k = std::max(0.25, std::min(cols * 0.44, rows * 1.1)) / 100.0;
        sk_ws = (cols * 0.26) / (6.8 * sk_k);
        double hs = std::max(s, 0.5);
        auto C = [&](double h, double ss, double l) { return sk_pack(hsl_rgb(h, ss, std::clamp(l, 0.0, 1.0))); };
        auto D = [&](double dk, double lg) { return light ? lg : dk * dim; };   // яскравість: ніч (з появою) / день
        for (int i = 0; i < 8; i++) {                                           // небо: від зеніту до горизонту
            double f = i / 7.0, g = std::pow(f, 1.3);
            sk_c[SKY0 + i] = C(hue - 18 + 46 * f, hs * (light ? 0.55 : 0.8), light ? 0.80 + 0.14 * g : (0.045 + 0.27 * g) * dim);
        }
        sk_c[SKFAR] = C(hue, 0.35, D(0.085, 0.60));  sk_c[SKWIN] = C(hue + 42, 0.8, D(0.72, 0.62)); sk_c[SKWIN2] = C(hue, 0.3, D(0.15, 0.68));
        sk_c[SKTREE] = C(hue + 95, 0.35, D(0.11, 0.33)); sk_c[SKTREE2] = C(hue + 95, 0.4, D(0.19, 0.45));
        sk_c[SKPOLE] = C(hue, 0.15, D(0.05, 0.22)); sk_c[SKGLOW] = C(46, 0.9, 0.78);
        sk_c[SKROAD] = C(hue, 0.18, D(0.085, 0.40)); sk_c[SKROAD2] = C(hue, 0.18, D(0.13, 0.46));
        sk_c[SKLANE] = C(hue, 0.2, D(0.72, 0.95)); sk_c[SKKERB] = C(hue, 0.2, D(0.2, 0.64));
        sk_c[SKB0] = C(hue, hs * 0.8, 0.22 * (0.4 + 0.6 * dim)); sk_c[SKB1] = C(hue, hs, 0.38 * (0.4 + 0.6 * dim));
        sk_c[SKB2] = C(hue, hs, 0.55 * (0.4 + 0.6 * dim)); sk_c[SKB3] = C(hue, hs * 0.7, 0.80 * (0.4 + 0.6 * dim));
        sk_c[SKGLASS] = C(hue + 10, 0.4, 0.07); sk_c[SKGLASS2] = C(hue + 10, 0.5, 0.36);
        sk_c[SKTIRE] = C(hue, 0.1, 0.04); sk_c[SKRIM] = C(hue, 0.1, 0.72);
        sk_c[SKTAIL] = C(2, 0.9, 0.55); sk_c[SKHEAD] = C(50, 0.5, 0.95); sk_c[SKBEAM] = C(50, 0.6, 0.78);
        sk_c[SKSTAR] = C(hue + 30, 0.3, 0.88); sk_c[SKNEON] = C(hue + 8, 0.95, 0.62);
        sk_c[SKSIGN] = C(hue + 150, 0.7, D(0.42, 0.5)); sk_c[SKONC] = C(hue + 180, 0.4, 0.15);
        sk_c[SKSHEL] = C(hue + 20, 0.5, D(0.5, 0.78));
    }
    // профіль даху/капота (у «одиницях авто»: u 0..100 вздовж, v вгору)
    static double sk_top(double u) {
        static const double P[][2] = {{2, 12}, {3, 16}, {27, 16.3}, {35, 26}, {58, 26}, {74, 16.8}, {95, 14.2}, {99, 10}, {99, 5}};
        for (int i = 0; i < 8; i++)
            if (u <= P[i + 1][0]) { double a = (u - P[i][0]) / (P[i + 1][0] - P[i][0]); return P[i][1] + a * (P[i + 1][1] - P[i][1]); }
        return 0;
    }
    // 0 — прозоро. flip: авто їде ліворуч (зустрічне, темне).
    uint32_t sk_car_px(double u, double v, double bay, bool flip) const {
        if (flip) u = 100 - u;
        static const double WX[2] = {22, 79};
        double bob = flip ? 0 : 0.35 * std::sin(t * 10.7) + 0.2 * std::sin(t * 4.3), vb = v - bob;
        for (int i = 0; i < 2; i++) {
            double dx = u - WX[i], dy = v - 6.8, d = std::sqrt(dx * dx + dy * dy);
            if (d < 6.8) {
                if (flip) return sk_c[SKTIRE];
                if (d < 4.3) {
                    if (d < 1.4) return sk_c[SKRIM];
                    double sp = std::fmod(std::atan2(dy, dx) * (5 / 6.2831853) - t * sk_ws * (1 / 6.2831853) * 5 + 40.0, 1.0);
                    return sp < 0.3 ? sk_c[SKRIM] : sk_c[SKTIRE];
                }
                return sk_c[SKTIRE];
            }
        }
        if (u < 2 || u > 99 || vb < 4.5) {
            if (!flip && u >= 2 && u <= 9 && vb >= 19 && vb <= 20.8) return sk_c[SKB0];            // спойлер
            return 0;
        }
        double top = sk_top(u);
        if (!flip && ((u > 3.8 && u < 5.2) || (u > 7.6 && u < 9)) && vb > top && vb < 19) return sk_c[SKB0];   // стійки спойлера
        if (!flip && u >= 2 && u <= 9 && vb >= 19 && vb <= 20.8) return sk_c[SKB1];
        if (vb > top) return 0;
        for (int i = 0; i < 2; i++) {
            double dx = u - WX[i], dy = v - 6.8;
            if (dx * dx + dy * dy < 8.4 * 8.4) return sk_c[SKTIRE];                                   // арка
        }
        if (flip) {                                                                                    // зустрічне: силует із вікном і фарою
            if (u > 94 && vb > 10.5 && vb < 13) return sk_c[SKHEAD];
            if (u < 5.2 && vb > 10 && vb < 13.5) return sk_c[SKTAIL];
            double xl = 27 + (vb - 16.3) / 9.7 * 8 + 2.5, xr = 58 + (26 - vb) / 9.2 * 16 - 2.5;
            if (vb > 17.8 && vb < 24.5 && u > xl && u < xr) return sk_c[SKWIN2];
            return sk_c[SKONC];
        }
        double xl = 27 + (vb - 16.3) / 9.7 * 8 + 2.5, xr = 58 + (26 - vb) / 9.2 * 16 - 2.5;
        if (vb > 17.8 && vb < 24.5 && u > xl && u < xr) {                                             // скло
            if (u > 47.5 && u < 49.5) return sk_c[SKB1];                                              // B-стійка
            return std::fmod(u + vb * 0.8 + 100, 13.0) < 2.2 ? sk_c[SKGLASS2] : sk_c[SKGLASS];
        }
        if (u < 5.4 && vb > 9.5 && vb < 13.8) return sk_c[SKTAIL];                                    // задні ліхтарі
        if (u > 93.5 && vb > 10.5 && vb < 13.2) return sk_c[SKHEAD];                                  // фара
        if (u > 96 && vb > 7 && vb < 9) return sk_c[SKB3];
        if (vb > 8.0 && vb < 8.9 && u > 12 && u < 90) return sk_c[SKB0];                              // лінія порогу
        if (u > 51 && u < 52 && vb > 8.9 && vb < 16.5) return sk_c[SKB0];                             // шов дверей
        if (u > 44 && u < 49.5 && vb > 14 && vb < 14.9) return sk_c[SKB3];                            // ручка
        double r = (vb - 4.5) / std::max(1.0, top - 4.5), q = r * 3.2 + (bay - 0.5) * 0.9;
        if (r > 0.9) return sk_c[SKB3];
        return q > 2.1 ? sk_c[SKB2] : q > 1.0 ? sk_c[SKB1] : sk_c[SKB0];
    }
    uint32_t sk_px(int x, int y, int cols, int rows, double bay) const {
        const int gy = sk_gy; const double k = sk_k, spd = cols * 0.26;
        const int yr0 = gy - (int)(rows * 0.085), ypv = yr0 - std::max(2, (int)(rows * 0.03));
        const double offm = t * spd, offf = t * spd * 2.1, offr = t * cols * 0.02;
        // 1. передній план: тонкі стовпи, що миготять перед авто
        {   double sp = cols * 0.66, wx = x + offf; long n = (long)std::floor(wx / sp);
            if (hash((uint32_t)n * 977u + 3) % 3 == 0) {
                double lx = wx - n * sp - sp * 0.35;
                if (lx >= 0 && lx < std::max(1.0, 2.4 * k) && y > rows * 0.1) return sk_c[SKPOLE];
            }
        }
        // 2. наше авто
        double cx0 = cols * 0.5 - 50 * k, u = (x + 0.5 - cx0) / k, v = (gy - y - 0.5) / k;
        if (u >= -1 && u <= 101 && v >= -1 && v <= 30) { uint32_t c = sk_car_px(u, v, bay, false); if (c) return c; }
        // 3. зустрічне авто (дальня смуга, менше), раз на ~6 с
        {   const double P = 6.0; long sl = (long)std::floor(t / P);
            if (hashf((uint32_t)sl * 31u + 7) < 0.7) {
                double kk = k * 0.78, vel = cols * 0.62, xp = cols * 1.1 - (t - sl * P) * vel;
                int gy2 = gy - (int)(rows * 0.05);
                double uu = (x + 0.5 - xp) / kk, vv = (gy2 - y - 0.5) / kk;
                if (uu >= 0 && uu <= 100 && vv >= -1 && vv <= 30) {
                    uint32_t c = sk_car_px(uu, vv, bay, true);
                    if (c) return c;
                }
            }
        }
        uint32_t out;
        if (y >= yr0) {                                                         // дорога
            out = (y > gy + (int)(rows * 0.09) && (bay < 0.5)) ? sk_c[SKROAD2] : sk_c[SKROAD];
            if (y <= yr0 + 1) out = sk_c[SKKERB];
            else {
                int th = std::max(1, (int)(0.8 * k));
                double wr = x + offm; int yc = gy - (int)(rows * 0.04);
                if (std::abs(y - yc) < th && std::fmod(wr, cols * 0.12) < cols * 0.06) out = sk_c[SKLANE];
                int ye = gy + (int)(rows * 0.12);
                if (std::abs(y - ye) < th && std::fmod(x + offf, cols * 0.16) < cols * 0.08) out = sk_c[SKLANE];
                if (hash(((uint32_t)((x + offf * 1.4) / 2) << 12) ^ (uint32_t)(y * 131 + 5)) % 41 == 0) out = sk_c[SKROAD2];
            }
        } else if (y >= ypv) out = sk_c[SKKERB];                                // тротуар
        else {
            // небо
            double fs = std::clamp((double)y / std::max(1, ypv), 0.0, 1.0);
            out = sk_c[SKY0 + std::clamp((int)(fs * 7.0 + (bay - 0.5) * 0.9 + 0.5), 0, 7)];
            // місяць / сонце, зорі
            double mx = cols * 0.8, my = rows * 0.2, mr = std::max(3.0, rows * 0.055), md = std::hypot(x - mx, y - my);
            if (md < mr) out = light ? sk_c[SKGLOW] : sk_c[SKSTAR];
            else if (!light && md < mr * 1.9 && bay < 0.2 * (1 - (md - mr) / (mr * 0.9))) out = sk_c[SKSTAR];
            else if (!light && y < ypv * 0.55 && hash((uint32_t)x * 7919u + (uint32_t)y * 104729u) % 400 == 0
                     && std::sin(t * 2 + x) > -0.3) out = sk_c[SKSTAR];
            // далекі будівлі
            double wxf = x + offr, fw = std::max(6.0, cols * 0.05); long m = (long)std::floor(wxf / (fw * 2));
            double hb = rows * (0.1 + 0.3 * hashf((uint32_t)m * 7 + 1));
            if (y >= ypv - (int)hb) {
                out = sk_c[SKFAR];
                int wi = (int)wxf;
                if (!light && wi % 4 == 1 && y % 5 == 2 && hash((uint32_t)(wi / 4) * 31u + (uint32_t)(y / 5) * 17u) % 3 == 0) out = sk_c[SKWIN];
                else if (light && wi % 4 == 1 && y % 5 == 2) out = sk_c[SKWIN2];
            }
        }
        // 4. об'єкти узбіччя (повз авто)
        {   double W = std::max(30.0, cols * 0.2), wx = x + offm; long n = (long)std::floor(wx / W);
            uint32_t h = hash((uint32_t)n * 2654435761u + 11);
            double lx = wx - n * W, cxm = W * (0.3 + 0.4 * ((h >> 8) & 255) / 255.0), dx = lx - cxm;
            int ty = h % 7, w1 = std::max(1, (int)std::lround(1.3 * k));
            if (ty <= 1) {                                                        // ліхтар
                int yt = yr0 - (int)(rows * 0.4);
                double armx = cols * 0.04;
                if (std::abs(dx) < w1 * 0.5 + 0.3 && y >= yt && y <= yr0) return sk_c[SKPOLE];
                if (dx > 0 && dx < armx && y >= yt && y < yt + w1) return sk_c[SKPOLE];
                double hx = dx - armx, hy = y - (yt + w1);
                if (hx > -2 && hx < 2 && hy >= 0 && hy < 2) return sk_c[SKHEAD];
                if (!light) { double d = std::hypot(hx, hy * 1.2), R = rows * 0.17; if (d < R && bay < 0.55 * (1 - d / R)) return sk_c[SKGLOW]; }
            } else if (ty <= 3) {                                                 // дерево
                double R = rows * 0.085; int yb = yr0 - (int)(rows * 0.11);
                if (std::abs(dx) < w1 * 0.5 + 0.5 && y >= yb && y <= yr0) return sk_c[SKPOLE];
                double d = std::hypot(dx, y - (yb - R * 0.55));
                if (d < R) return std::hypot(dx + R * 0.3, y - (yb - R * 0.55) + R * 0.3) < R * 0.55 + (bay - 0.5) * R * 0.3 ? sk_c[SKTREE2] : sk_c[SKTREE];
            } else if (ty == 4) {                                                 // дорожній знак
                int yt = yr0 - (int)(rows * 0.26); double sw = cols * 0.05, sh = rows * 0.08;
                if (std::abs(dx) < w1 * 0.5 + 0.3 && y >= yt && y <= yr0) return sk_c[SKPOLE];
                if (dx > -sw * 0.5 && dx < sw * 0.5 && y >= yt - sh && y < yt) {
                    bool edge = dx < -sw * 0.5 + 1.2 || dx > sw * 0.5 - 1.2 || y < yt - sh + 1.2 || y >= yt - 1.2;
                    return edge ? sk_c[SKLANE] : sk_c[SKSIGN];
                }
            } else if (ty == 5) {                                                 // автобусна зупинка
                double sw = cols * 0.11, sh = rows * 0.13;
                if (dx > -sw * 0.5 && dx < sw * 0.5 && y >= yr0 - sh && y <= yr0) {
                    if (y < yr0 - sh + 2 || dx < -sw * 0.5 + 1.5 || dx > sw * 0.5 - 1.5) return sk_c[SKPOLE];
                    return (!light && bay < 0.7) || light ? sk_c[SKSHEL] : out;
                }
            }
        }
        // 5. світло фар і неонова підсвітка під авто (поверх фону)
        if (!light) {
            if (u > 99 && u < 170 && std::abs(v - 11.6) < 1 + (u - 99) * 0.13 && bay < 0.6 * (1 - (u - 99) / 71.0)) return sk_c[SKBEAM];
            if (u > 8 && u < 92 && v < 0 && v > -6 && bay < 0.55 * (1 + v / 6.0)) return sk_c[SKNEON];
        }
        return out;
    }

    // ---- «Бублик» (torus) і «Вузол» (knot): обертовий 3D піксельний об'єкт по центру ----
    // Обидва — трубка радіуса tr_r уздовж замкненої кривої (коло для тора, трилисник для вузла).
    // Крива + ортонормальна рамка (паралельне перенесення) рахуються раз; щокадру рамку обертаємо
    // (по 9 множень на точку кривої), а точки поверхні — лінійні комбінації в просторі камери,
    // проєкція з перспективою і z-буфер (сплатинг з запасом щільності ~1.5 точки/клітинку — без дір).
    // Освітлення: дифуз + блік + контурне світло, туман за глибиною; тло — м'яке світіння і тінь
    // (розмита маска на півроздільності, тінь зсунута від світла). Дизер Байєра, палітра з акценту.
    struct TrNode { float c[3], a[3], b[3]; };
    std::vector<TrNode> tr_curve;
    int tr_kind = -1;
    float tr_r = 0.4f, tr_len = 6.28f;
    int tr_w = 0, tr_h = 0;
    std::vector<float> tr_z, tr_v, tr_m, tr_g, tr_s, tr_tmp;
    std::vector<uint32_t> tr_frame;
    static constexpr int TR_NL = 12, TR_NB = 8;
    uint32_t tr_pal[TR_NL] = {}, tr_bg[TR_NB] = {};
    void tr_build(int kind) {
        tr_kind = kind;
        const int N = 720;
        tr_curve.assign(N, {});
        auto C = [&](double u, double* p) {
            if (kind == 0) { p[0] = std::cos(u); p[1] = std::sin(u); p[2] = 0; return; }
            // трилисник: (sin u + 2 sin 2u, cos u − 2 cos 2u, −sin 3u) / 3
            p[0] = (std::sin(u) + 2 * std::sin(2 * u)) / 3.0;
            p[1] = (std::cos(u) - 2 * std::cos(2 * u)) / 3.0;
            p[2] = -std::sin(3 * u) * 0.42;
        };
        tr_r = kind == 0 ? 0.36f : 0.17f;
        double len = 0, pp[3], p0[3];
        C(0, p0);
        double prev[3] = {p0[0], p0[1], p0[2]};
        double a[3] = {0, 0, 1};
        for (int i = 0; i < N; i++) {
            double u = 6.283185307179586 * i / N;
            double p[3], q[3];
            C(u, p); C(u + 1e-3, q);
            double T[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
            double tl = std::sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]); T[0] /= tl; T[1] /= tl; T[2] /= tl;
            if (i == 0) { a[0] = -p[0]; a[1] = -p[1]; a[2] = 0.3; }
            // паралельне перенесення: прибираємо з a складову вздовж T
            double d = a[0] * T[0] + a[1] * T[1] + a[2] * T[2];
            a[0] -= d * T[0]; a[1] -= d * T[1]; a[2] -= d * T[2];
            double al = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); a[0] /= al; a[1] /= al; a[2] /= al;
            double b[3] = {T[1] * a[2] - T[2] * a[1], T[2] * a[0] - T[0] * a[2], T[0] * a[1] - T[1] * a[0]};
            TrNode& n = tr_curve[i];
            for (int k = 0; k < 3; k++) { n.c[k] = (float)p[k]; n.a[k] = (float)a[k]; n.b[k] = (float)b[k]; }
            len += std::sqrt((p[0] - prev[0]) * (p[0] - prev[0]) + (p[1] - prev[1]) * (p[1] - prev[1]) + (p[2] - prev[2]) * (p[2] - prev[2]));
            prev[0] = p[0]; prev[1] = p[1]; prev[2] = p[2];
        }
        (void)pp;
        tr_len = (float)len;
    }
    // коробкове розмиття радіуса r (рядки, потім стовпці), ковзна сума
    void tr_blur(std::vector<float>& a, int w, int h, int r) {
        if (r < 1) return;
        tr_tmp.resize((size_t)w * h);
        float k = 1.0f / (2 * r + 1);
        for (int y = 0; y < h; y++) {
            const float* s = &a[(size_t)y * w]; float* o = &tr_tmp[(size_t)y * w];
            float acc = 0;
            for (int i = -r; i <= r; i++) acc += s[std::clamp(i, 0, w - 1)];
            for (int x = 0; x < w; x++) {
                o[x] = acc * k;
                acc += s[std::min(x + r + 1, w - 1)] - s[std::max(x - r, 0)];
            }
        }
        for (int x = 0; x < w; x++) {
            float acc = 0;
            for (int i = -r; i <= r; i++) acc += tr_tmp[(size_t)std::clamp(i, 0, h - 1) * w + x];
            for (int y = 0; y < h; y++) {
                a[(size_t)y * w + x] = acc * k;
                acc += tr_tmp[(size_t)std::min(y + r + 1, h - 1) * w + x] - tr_tmp[(size_t)std::max(y - r, 0) * w + x];
            }
        }
    }
    void torus_prepare(int cols, int rows, double hue, double s, double dim, int kind) {
        if (tr_kind != kind || tr_curve.empty()) tr_build(kind);
        size_t n = (size_t)cols * rows;
        int hw = (cols + 1) / 2, hh = (rows + 1) / 2;
        if (tr_w != cols || tr_h != rows) {
            tr_w = cols; tr_h = rows;
            tr_z.assign(n, 0); tr_v.assign(n, 0); tr_frame.assign(n, 0);
            tr_m.assign((size_t)hw * hh, 0); tr_g.assign((size_t)hw * hh, 0); tr_s.assign((size_t)hw * hh, 0);
        }
        // ---- палітри ----
        double hc = hue + (hue_lo + hue_hi) * 0.1;
        for (int i = 0; i < TR_NL; i++) {
            double k = (double)i / (TR_NL - 1);                       // 0 — тінь, 1 — блік
            double hh2 = hc + (hue_lo - hc + hue) * 0.0 + (k - 0.5) * (hue_hi - hue_lo) * 0.9;
            RGB c;
            if (!light) c = hsl_rgb(hh2, std::clamp(s * (1.0 - 0.45 * k * k * k), 0.25, 0.95), 0.06 + 0.66 * std::pow(k, 1.1) + 0.18 * k * k * k * k);
            else c = hsl_rgb(hh2, std::clamp(s * (0.95 - 0.35 * k * k), 0.3, 0.95), 0.20 + 0.62 * k + 0.12 * k * k * k * k);
            tr_pal[i] = ct_pk({std::min(1.0, c.r) * dim, std::min(1.0, c.g) * dim, std::min(1.0, c.b) * dim});
        }
        for (int i = 0; i < TR_NB; i++) {
            double k = (double)i / (TR_NB - 1);                       // 0 — глибока тінь, 1 — найсильніше світіння
            RGB c;
            if (!light) c = hsl_rgb(hc + (k - 0.3) * 10, std::clamp(s * (0.55 + 0.35 * k), 0.2, 0.9), 0.025 + 0.20 * k * k + 0.03 * k);
            else c = hsl_rgb(hc - (k - 0.3) * 8, std::clamp(s * (0.35 + 0.35 * k), 0.15, 0.85), 0.80 + 0.12 * std::min(1.0, k * 3) - 0.0 * k);
            tr_bg[i] = ct_pk({c.r * dim, c.g * dim, c.b * dim});
        }
        // ---- обертання ----
        // повноцінне «кувиркання»: безперервне обертання навколо горизонтальної X, вертикальної Y і повільно Z
        // з несинхронними швидкостями (рад/с); кути ростуть лінійно — жодних стрибків. Світло нерухоме (у просторі камери).
        double ax, ay, az;
        if (kind == 0) { ax = 0.9 + t * 0.50; ay = 0.3 + t * 0.37; az = t * 0.21; }
        else { ax = 0.6 + t * 0.43; ay = 1.1 + t * 0.31; az = t * 0.19; }
        double cX = std::cos(ax), sX = std::sin(ax), cY = std::cos(ay), sY = std::sin(ay), cZ = std::cos(az), sZ = std::sin(az);
        // M = Rx · Ry · Rz
        double M[9] = {cY * cZ, -cY * sZ, sY,
                       sX * sY * cZ + cX * sZ, -sX * sY * sZ + cX * cZ, -sX * cY,
                       -cX * sY * cZ + sX * sZ, cX * sY * sZ + sX * cZ, cX * cY};
        double R = rows * (kind == 0 ? 0.25 : 0.34);                  // масштаб об'єкта в клітинках
        double D = 4.2;                                               // відстань камери (в одиницях об'єкта)
        double f = R * D;
        double ocx = cols * 0.5, ocy = rows * (0.49 + 0.012 * std::sin(t * 0.6));
        // світло зверху-зліва, трохи спереду (камера дивиться вздовж +z)
        double Lx = -0.52, Ly = -0.62, Lz = -0.58;
        { double l = std::sqrt(Lx * Lx + Ly * Ly + Lz * Lz); Lx /= l; Ly /= l; Lz /= l; }
        double Hx = Lx, Hy = Ly, Hz = Lz - 1; { double l = std::sqrt(Hx * Hx + Hy * Hy + Hz * Hz); Hx /= l; Hy /= l; Hz /= l; }
        // ---- сплатинг ----
        std::fill(tr_z.begin(), tr_z.end(), 1e9f);
        double scl = R * D / (D - 1.0);                               // найбільше збільшення (ближній край)
        int NU = std::clamp((int)(tr_len * scl * 1.15), 64, 4000);
        int NV = std::clamp((int)(6.283 * tr_r * scl * 1.2) + 8, 16, 800);
        static thread_local std::vector<float> cv, sv;
        if ((int)cv.size() != NV) { cv.resize(NV); sv.resize(NV); for (int j = 0; j < NV; j++) { cv[j] = (float)std::cos(6.283185307 * j / NV); sv[j] = (float)std::sin(6.283185307 * j / NV); } }
        int NC = (int)tr_curve.size();
        float rr = tr_r;
        float spin = (float)(t * 0.6);                                // «бігуча» смуга вздовж трубки
        for (int i = 0; i < NU; i++) {
            double fu = (double)i * NC / NU;
            int i0 = (int)fu; double w1 = fu - i0; int i1 = (i0 + 1) % NC;
            const TrNode& n0 = tr_curve[i0]; const TrNode& n1 = tr_curve[i1];
            double c[3], a[3], b[3];
            for (int k = 0; k < 3; k++) { c[k] = n0.c[k] + (n1.c[k] - n0.c[k]) * w1; a[k] = n0.a[k] + (n1.a[k] - n0.a[k]) * w1; b[k] = n0.b[k] + (n1.b[k] - n0.b[k]) * w1; }
            float C3[3], A3[3], B3[3];
            for (int k = 0; k < 3; k++) {
                C3[k] = (float)(M[k * 3] * c[0] + M[k * 3 + 1] * c[1] + M[k * 3 + 2] * c[2]);
                A3[k] = (float)(M[k * 3] * a[0] + M[k * 3 + 1] * a[1] + M[k * 3 + 2] * a[2]);
                B3[k] = (float)(M[k * 3] * b[0] + M[k * 3 + 1] * b[1] + M[k * 3 + 2] * b[2]);
            }
            double uu = 6.283185307 * i / NU;
            // ледь помітні смуги вздовж трубки — щоб обертання тора навколо осі було видно
            float band = (float)(kind == 0 ? 0.06 * fsin(uu * 12 + spin) : 0.05 * fsin(uu * 18 - spin));
            for (int j = 0; j < NV; j++) {
                float nx = cv[j] * A3[0] + sv[j] * B3[0], ny = cv[j] * A3[1] + sv[j] * B3[1], nz = cv[j] * A3[2] + sv[j] * B3[2];
                float pz = C3[2] + rr * nz;
                // відкидаємо звернені від камери точки (перспектива: вектор до камери ≈ (−p) з камерою в −D)
                float px = C3[0] + rr * nx, py = C3[1] + rr * ny;
                if (nx * px + ny * py + nz * (pz + (float)D) > 0.02f) continue;
                float iz = (float)(f / (D + pz));
                int sx = (int)(ocx + px * iz), sy = (int)(ocy + py * iz);
                if ((unsigned)sx >= (unsigned)cols || (unsigned)sy >= (unsigned)rows) continue;
                size_t p = (size_t)sy * cols + sx;
                if (pz >= tr_z[p]) continue;
                tr_z[p] = pz;
                float dif = std::max(0.0f, (nx * (float)Lx + ny * (float)Ly + nz * (float)Lz));
                float sp = std::max(0.0f, (nx * (float)Hx + ny * (float)Hy + nz * (float)Hz));
                sp *= sp; sp *= sp; sp *= sp; sp *= sp; sp *= sp;              // ^32
                float fill = std::max(0.0f, nx * 0.55f + ny * 0.62f - nz * 0.2f);     // слабке відбите світло знизу-справа
                float rim = 1.0f + nz; rim = rim * rim * 0.5f;                  // контурне світло (nz≈−1 — до камери)
                float fog = 1.0f - 0.22f * (pz + 1.0f);                         // далі — темніше
                float v = (0.07f + 0.17f * fill + 0.66f * dif + 0.12f * (1 - rim) * 0.0f + 0.22f * rim * (0.5f + 0.5f * (float)light) ) * fog + band + 0.55f * sp;
                tr_v[p] = std::clamp(v, 0.0f, 1.0f);
            }
        }
        // латання одиночних дірок сплатингу: порожня клітинка між двома зайнятими сусідами (по горизонталі/вертикалі)
        for (int y = 1; y + 1 < rows; y++) for (int x = 1; x + 1 < cols; x++) {
            size_t p = (size_t)y * cols + x;
            if (tr_z[p] < 1e8f) continue;
            size_t a = 0, b = 0;
            if (tr_z[p - 1] < 1e8f && tr_z[p + 1] < 1e8f) { a = p - 1; b = p + 1; }
            else if (tr_z[p - cols] < 1e8f && tr_z[p + cols] < 1e8f) { a = p - cols; b = p + cols; }
            else continue;
            if (std::fabs(tr_z[a] - tr_z[b]) > tr_r) continue;          // різні гілки трубки — справжній проміжок
            tr_z[p] = 0.5f * (tr_z[a] + tr_z[b]); tr_v[p] = 0.5f * (tr_v[a] + tr_v[b]);
        }
        // ---- маска на півроздільності -> світіння і тінь ----
        std::fill(tr_m.begin(), tr_m.end(), 0.0f);
        for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++)
            if (tr_z[(size_t)y * cols + x] < 1e8f) tr_m[(size_t)(y >> 1) * hw + (x >> 1)] += 0.25f;
        tr_g = tr_m; tr_s = tr_m;
        int rg = std::max(2, (int)(R * 0.16)), rs = std::max(1, (int)(R * 0.07));
        tr_blur(tr_g, hw, hh, rg); tr_blur(tr_g, hw, hh, rg);
        tr_blur(tr_s, hw, hh, rs); tr_blur(tr_s, hw, hh, rs);
        int shx = (int)std::lround(R * 0.20), shy = (int)std::lround(R * 0.24);   // тінь зсунута від світла (у півклітинках)
        double pulse = 0.85 + 0.15 * std::sin(t * 0.9);
        // ---- кадр ----
        static const double BY[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        double inv = 1.0 / std::max(1, rows);
        for (int y = 0; y < rows; y++) {
            uint32_t* o = &tr_frame[(size_t)y * cols];
            for (int x = 0; x < cols; x++) {
                double bay = (BY[y & 3][x & 3] + 0.5) / 16.0 - 0.5;
                size_t p = (size_t)y * cols + x;
                if (tr_z[p] < 1e8f) {
                    int q = (int)std::floor(tr_v[p] * (TR_NL - 1) + 0.5 + bay);
                    o[x] = tr_pal[std::clamp(q, 0, TR_NL - 1)];
                    continue;
                }
                // тло: білінійна вибірка півроздільних світіння і тіні
                double gx = x * 0.5 - 0.25, gy = y * 0.5 - 0.25;
                int x0 = std::clamp((int)gx, 0, hw - 1), y0 = std::clamp((int)gy, 0, hh - 1);
                int x1 = std::min(x0 + 1, hw - 1), y1 = std::min(y0 + 1, hh - 1);
                double fx = std::clamp(gx - x0, 0.0, 1.0), fy = std::clamp(gy - y0, 0.0, 1.0);
                auto bil = [&](const std::vector<float>& a, int ox, int oy) {
                    int ax0 = std::clamp(x0 - ox, 0, hw - 1), ax1 = std::clamp(x1 - ox, 0, hw - 1);
                    int ay0 = std::clamp(y0 - oy, 0, hh - 1), ay1 = std::clamp(y1 - oy, 0, hh - 1);
                    double t0 = a[(size_t)ay0 * hw + ax0] * (1 - fx) + a[(size_t)ay0 * hw + ax1] * fx;
                    double t1 = a[(size_t)ay1 * hw + ax0] * (1 - fx) + a[(size_t)ay1 * hw + ax1] * fx;
                    return t0 * (1 - fy) + t1 * fy;
                };
                double g = bil(tr_g, 0, 0) * (1.0 - 0.8 * bil(tr_s, 0, 0)), sh = bil(tr_s, shx, shy);   // світіння — лише назовні (не в отворі)
                // тонкий темний контур навколо силуету (темна тема) — чітко видно отвір і перехрестя
                if (!light && ((x > 0 && tr_z[p - 1] < 1e8f) || (x + 1 < cols && tr_z[p + 1] < 1e8f) ||
                               (y > 0 && tr_z[p - cols] < 1e8f) || (y + 1 < rows && tr_z[p + cols] < 1e8f))) { o[x] = tr_bg[0]; continue; }
                double dx = (x - ocx) * inv, dy = (y - ocy) * inv;
                double vig = std::max(0.0, 1.0 - std::sqrt(dx * dx + dy * dy) * 0.9);
                double bv = 0.16 + 0.16 * vig + 1.25 * g * pulse - 0.30 * sh;
                int q = (int)std::floor(bv * (TR_NB - 1) + 0.5 + bay);
                o[x] = tr_bg[std::clamp(q, 0, TR_NB - 1)];
            }
        }
    }
    uint32_t torus_px(int x, int y) const { return tr_frame[(size_t)y * tr_w + x]; }

    // ---- «Хмари» — псевдо-3D політ камери над/крізь хмарні шари ----
    // Два шари хмар під камерою (ближній яскравий і дальній темніший — паралакс у прогалинах), стеля хмар над
    // обрієм при похмурій погоді і кілька «пухнастиків», крізь які камера пролітає (розчиняються дизером).
    // Сонце рахується локально з широти/довготи і часу (схід/захід/ніч), світить спереду: краї хмар з боку
    // сонця підсвічені, на заході — теплі відтінки (усе — зсуви відтінку акценту). Погода й координати —
    // з кешу дашборда або власного кешу (wttr.in за IP, раз на 30 хв, у фоновому потоці, не блокує кадр).
    // Тест без мережі: MGOS_CLOUDS_WX=clear|partly|cloudy|rain|snow|storm|fog, MGOS_CLOUDS_HOUR=0..24.
    std::vector<float> cl_nhi, cl_nlo;           // тайлова 256×256 fbm-текстура шуму і її mip-рівні 128..8 (без мерехтіння вдалині)
    size_t cl_moff[6] = {0};
    uint32_t cl_sky[5][24], cl_cld[6][10], cl_rain = 0, cl_sun = 0, cl_moon = 0, cl_star = 0;
    double cl_hy = 0, cl_cov = 0.5, cl_dim = 1, cl_fogk = 0, cl_sunx = 0, cl_suny = 0, cl_sunr = 0, cl_suna = 0;
    double cl_day = 1, cl_gold = 0, cl_night = 0, cl_T = 0, cl_glowr = 1, cl_moona = 0;
    int cl_wx = 1;
    double cl_lat = 50.45, cl_lon = 30.52;       // офлайн-дефолт (Київ), поки немає кешу
    long long cl_poll = 0;
    struct ClPuff { float sx, sy, rr, al, pz; int seed; };
    ClPuff cl_pf[8]; int cl_np = 0;

    static int cl_code_wx(int c) {               // код wttr.in → 0 ясно,1 мінливо,2 хмарно,3 дощ,4 сніг,5 гроза,6 туман
        switch (c) {
            case 113: return 0; case 116: return 1; case 119: case 122: return 2;
            case 143: case 248: case 260: return 6;
            case 200: case 386: case 389: case 392: case 395: return 5;
            case 179: case 227: case 230: case 323: case 326: case 329: case 332: case 335: case 338: case 368: case 371: return 4;
            default: return c ? 3 : 1;
        }
    }
    static double cl_num_after(const std::string& s, const std::string& key, size_t from = 0) {
        size_t p = s.find("\"" + key + "\"", from);
        if (p == std::string::npos) return NAN;
        p = s.find_first_of("-0123456789", p + key.size() + 2);
        return p == std::string::npos ? NAN : atof(s.c_str() + p);
    }
    // Розбір j1-відповіді wttr.in: код погоди і координати найближчого місця
    static bool cl_parse_j1(const std::string& s, int& code, double& lat, double& lon) {
        size_t cc = s.find("current_condition");
        if (cc == std::string::npos) return false;
        double c = cl_num_after(s, "weatherCode", cc);
        if (std::isnan(c)) return false;
        code = (int)c;
        size_t na = s.find("nearest_area");
        if (na != std::string::npos) {
            double a = cl_num_after(s, "latitude", na), o = cl_num_after(s, "longitude", na);
            if (!std::isnan(a) && !std::isnan(o)) { lat = a; lon = o; }
        }
        return true;
    }
    // Місто з ~/.config/megumin-os/settings.json ("location"), url-encoded (безпечно для shell і URL); "" — за IP
    static std::string cl_url_enc(const std::string& v) {
        std::string r; char b[4];
        for (unsigned char ch : v) {
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') r += (char)ch;
            else if (ch == ' ') r += '+';
            else { snprintf(b, sizeof b, "%%%02X", ch); r += b; }
        }
        return r;
    }
    static std::string cl_location(const std::string& home) {
        std::ifstream f(home + "/.config/megumin-os/settings.json");
        if (!f) return "";
        std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str();
        size_t p = s.find("\"location\"");
        if (p == std::string::npos) return "";
        size_t a = s.find('"', s.find(':', p) + 1);
        if (a == std::string::npos) return "";
        std::string v; bool esc = false;
        for (size_t i = a + 1; i < s.size(); i++) {
            char c = s[i];
            if (esc) { v += c; esc = false; } else if (c == '\\') esc = true; else if (c == '"') break; else v += c;
        }
        while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
        while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
        return cl_url_enc(v);
    }
    // Раз на хвилину — дешеве читання кешу; застарів (>30 хв) — разовий фоновий потік з curl.
    void clouds_weather_poll() {
        long long now = (long long)time(nullptr);
        if (now - cl_poll < 60 && cl_poll) return;
        cl_poll = now;
        const char* hm = getenv("HOME");
        std::string home = hm ? hm : "/tmp";
        std::string mine = home + "/.cache/megumin-os/clouds_weather.txt";
        long long ts = 0; int code = 0; double la = cl_lat, lo = cl_lon;
        const std::string loc = cl_location(home);   // місто з Налаштувань ("" — за IP)
        {   std::ifstream f(mine); std::string floc;
            if (f >> ts >> la >> lo >> code) {
                cl_lat = la; cl_lon = lo; cl_wx = cl_code_wx(code);
                if (!(f >> floc)) floc = "";
                if (floc == "-") floc = "";
                if (floc != loc) ts = 0;               // місто змінили — кеш не наш
            } else ts = 0; }
        if (now - ts < 1800) return;
        {   // дашборд уже тягнув погоду (його кеш свіжий) — беремо звідти, без мережі
            std::ifstream f(home + "/.cache/mgos/dashboard_weather.json");
            if (f) {
                std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str();
                double dts = cl_num_after(s, "ts");
                size_t cp = s.find("\"city\"");
                std::string dcity;
                if (cp != std::string::npos) { size_t a = s.find('"', s.find(':', cp) + 1), b = a == std::string::npos ? a : s.find('"', a + 1);
                                               if (b != std::string::npos) dcity = s.substr(a + 1, b - a - 1); }
                if (!std::isnan(dts) && now - (long long)dts < 1800 && cl_url_enc(dcity) == loc && cl_parse_j1(s, code, la, lo)) {
                    cl_lat = la; cl_lon = lo; cl_wx = cl_code_wx(code);
                    std::ofstream o(mine); o << (long long)dts << ' ' << la << ' ' << lo << ' ' << code << ' ' << (loc.empty() ? "-" : loc) << '\n';
                    return;
                }
            }
        }
        if (t < 6) return;                         // постер/прев'ю — без мережі
        static std::atomic<bool> busy{false};
        if (busy.exchange(true)) return;
        std::thread([mine, la, lo, loc]() {
            std::string out; char buf[4096];
            std::string cmd = "curl -sf -m 15 -A curl 'https://wttr.in/" + loc + "?format=j1' 2>/dev/null";   // loc уже url-encoded
            if (FILE* p = popen(cmd.c_str(), "r")) {
                size_t n; while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
                pclose(p);
            }
            int code = 0; double a = la, o = lo;
            bool ok = cl_parse_j1(out, code, a, o);
            std::string tmp = mine + ".tmp";
            {   // невдача — теж пишемо (зі старим кодом/дефолтом), щоб не смикати мережу щохвилини
                std::ofstream f(tmp);
                f << (long long)time(nullptr) - (ok ? 0 : 1500) << ' ' << a << ' ' << o << ' ' << (ok ? code : 116) << ' ' << (loc.empty() ? "-" : loc) << '\n';
            }
            rename(tmp.c_str(), mine.c_str());
            busy = false;
        }).detach();
    }
    // Висота і азимут сонця (градуси) — спрощена формула NOAA, точність ~1°
    static void cl_sun_pos(double unix_t, double lat, double lon, double& elev, double& az) {
        const double R = M_PI / 180;
        double d = unix_t / 86400.0 - 10957.5;
        double g = (357.529 + 0.98560028 * d) * R, q = 280.459 + 0.98564736 * d;
        double L = (q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * R, e = (23.439 - 3.6e-7 * d) * R;
        double ra = std::atan2(std::cos(e) * std::sin(L), std::cos(L)), dec = std::asin(std::sin(e) * std::sin(L));
        double gmst = std::fmod(18.697374558 + 24.06570982441908 * d, 24.0);
        double ha = (gmst * 15 + lon) * R - ra, la = lat * R;
        elev = std::asin(std::sin(la) * std::sin(dec) + std::cos(la) * std::cos(dec) * std::cos(ha)) / R;
        az = std::atan2(-std::sin(ha), std::tan(dec) * std::cos(la) - std::sin(la) * std::cos(ha)) / R;
    }
    void clouds_noise() {
        const int N = 256;
        cl_nhi.assign(N * N, 0);
        auto vn = [](int x, int y, int per, uint32_t seed) {      // тайлова value-noise з періодом per
            auto at = [per, seed](int i, int j) { return hashf(((i % per + per) % per) * 73856093u ^ ((j % per + per) % per) * 19349663u ^ seed); };
            return std::function<double(double, double)>([=](double u, double v) {
                int i = (int)std::floor(u), j = (int)std::floor(v);
                double fx = u - i, fy = v - j; fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
                double a = at(i, j), b = at(i + 1, j), c = at(i, j + 1), d = at(i + 1, j + 1);
                return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
            });
        };
        std::vector<std::function<double(double, double)>> oc;
        for (int o = 0; o < 5; o++) oc.push_back(vn(0, 0, 4 << o, 0xC10D5u + o * 977u));
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                double hi = 0, a = 0.5, sum = 0;
                for (int o = 0; o < 5; o++) {
                    double f = (4 << o) / (double)N, v = oc[o](x * f, y * f);
                    hi += a * v;
                    sum += a; a *= 0.5;
                }
                cl_nhi[y * N + x] = (float)(hi / sum);
            }
        auto stretch = [](std::vector<float>& v) {                 // розтягнути контраст у 0..1
            float mn = 1, mx = 0; for (float f : v) { mn = std::min(mn, f); mx = std::max(mx, f); }
            for (float& f : v) f = (f - mn) / std::max(1e-3f, mx - mn);
        };
        stretch(cl_nhi);
        cl_nlo.clear();
        std::vector<float> pv = cl_nhi, lv;
        for (int k = 1; k < 6; k++) {                // box-фільтр 2×2, кожен рівень знову розтягнутий до 0..1
            int n = N >> k, pn = n * 2;
            lv.assign(n * n, 0);
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++)
                    lv[y * n + x] = 0.25f * (pv[2 * y * pn + 2 * x] + pv[2 * y * pn + 2 * x + 1] +
                                             pv[(2 * y + 1) * pn + 2 * x] + pv[(2 * y + 1) * pn + 2 * x + 1]);
            stretch(lv);
            cl_moff[k] = cl_nlo.size();
            cl_nlo.insert(cl_nlo.end(), lv.begin(), lv.end());
            pv.swap(lv);
        }
    }
    // Вибірка з mip-рівня k (u, v — у текселях рівня 0)
    float cl_mtex(int k, double u, double v) const {
        if (k <= 0) return cl_tex(cl_nhi, u, v);
        int n = 256 >> k, m = n - 1;
        u = u / (1 << k) - 0.5; v = v / (1 << k) - 0.5;
        double fu = std::floor(u), fv = std::floor(v);
        int i = (int)fu & m, j = (int)fv & m, i1 = (i + 1) & m, j1 = (j + 1) & m;
        float fx = (float)(u - fu), fy = (float)(v - fv);
        const float* T = cl_nlo.data() + cl_moff[k];
        float a = T[j * n + i], b = T[j * n + i1], c = T[j1 * n + i], d = T[j1 * n + i1];
        return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
    }
    static float cl_tex(const std::vector<float>& T, double u, double v) {
        double fu = std::floor(u), fv = std::floor(v);
        int i = (int)fu & 255, j = (int)fv & 255, i1 = (i + 1) & 255, j1 = (j + 1) & 255;
        float fx = (float)(u - fu), fy = (float)(v - fv);
        float a = T[j * 256 + i], b = T[j * 256 + i1], c = T[j1 * 256 + i], d = T[j1 * 256 + i1];
        return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
    }
    void clouds_prepare(int cols, int rows, double hue, double s, double dim) {
        if (cl_nhi.empty()) clouds_noise();
        clouds_weather_poll();
        double unix_t = (double)time(nullptr);
        const char* eh = getenv("MGOS_CLOUDS_HOUR");
        if (eh && *eh) {
            time_t n = time(nullptr); struct tm lt_; localtime_r(&n, &lt_);
            lt_.tm_hour = 0; lt_.tm_min = 0; lt_.tm_sec = 0;
            unix_t = (double)mktime(&lt_) + atof(eh) * 3600;
        }
        if (const char* ew = getenv("MGOS_CLOUDS_WX")) {
            static const char* W[] = {"clear", "partly", "cloudy", "rain", "snow", "storm", "fog"};
            for (int i = 0; i < 7; i++) if (!strcmp(ew, W[i])) cl_wx = i;
        }
        double elev, az; cl_sun_pos(unix_t, cl_lat, cl_lon, elev, az);
        (void)az;                                    // камера летить назустріч сонцю — азимут задає лише «курс»
        static const double COV[] = {0.34, 0.5, 0.7, 0.8, 0.74, 0.9, 0.62};
        static const double DIM[] = {1.0, 0.94, 0.76, 0.64, 0.84, 0.5, 0.74};
        static const double FOG[] = {0.0, 0.05, 0.15, 0.3, 0.3, 0.25, 0.8};
        cl_cov = COV[cl_wx]; cl_dim = DIM[cl_wx]; cl_fogk = FOG[cl_wx];
        cl_day = smooth((elev + 7) / 19);            // 0 — ніч (сонце нижче -7°), 1 — день (вище 12°)
        cl_gold = std::exp(-std::pow((elev - 3) / 8, 2)) * (0.35 + 0.65 * cl_dim);   // схід/захід — золоті краї
        cl_night = 1 - cl_day;
        double flash = 0;
        if (cl_wx == 5) {                            // гроза: зрідка подвійний спалах
            long k = (long)std::floor(t / 6.5);
            double tp = t - k * 6.5 - hashf((uint32_t)k * 31u + 7) * 4;
            if (tp > 0 && tp < 0.6) flash = std::exp(-tp * 14) + 0.7 * std::exp(-std::fabs(tp - 0.28) * 22);
            flash = std::min(1.0, flash);
        }
        cl_hy = rows * (0.42 + 0.012 * std::sin(t * 0.13));
        cl_T = t * 0.55;
        cl_sunx = cols * (0.36 + 0.04 * std::sin(t * 0.021));
        cl_suny = cl_hy - std::clamp(elev, -10.0, 70.0) / 38.0 * cl_hy;
        cl_sunr = rows * 0.05;
        cl_glowr = rows * (0.35 + 0.35 * cl_gold);
        cl_suna = smooth((elev + 2.5) / 4) * std::clamp(1 - (cl_cov - 0.62) * 3.2, 0.0, 1.0);
        cl_moona = smooth((cl_night - 0.6) / 0.3) * std::clamp(1 - (cl_cov - 0.6) * 3.0, 0.0, 1.0);

        // Відтінки — лише зсуви акценту: холодний (тіні, зеніт), базовий (світло хмар), теплий (захід, сонце)
        double dw = std::fmod(20.0 - hue + 720.0, 360.0); if (dw > 180) dw -= 360;   // напрям до «теплого» боку кола
        double sh = std::fabs(dw) > 120 ? 40.0 : std::copysign(std::min(40.0, std::fabs(dw) * 0.45 + 6), dw);
        double hc = hue + hue_lo - 14, hw = hue + sh + (sh > 0 ? hue_hi : hue_lo) * 0.3, hb = hue;
        double ss = light ? s * 0.62 : s * 0.7;
        auto col = [&](double h, double sat, double b) {         // «фізична» яскравість b → світлота теми
            b = std::clamp(b * (0.4 + 0.6 * dim), 0.0, 1.0);
            return light ? hsl_rgb(h, sat * (0.55 + 0.45 * b), 0.46 + 0.48 * b) : hsl_rgb(h, sat, 0.02 + 0.6 * std::pow(b, 1.3));
        };
        auto mixc = [](RGB a, RGB b, double k) { return RGB{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k}; };
        auto pack = [](RGB c) {
            return 0xFF000000u | (uint32_t)(std::clamp(c.r, 0.0, 1.0) * 255) << 16 | (uint32_t)(std::clamp(c.g, 0.0, 1.0) * 255) << 8 |
                   (uint32_t)(std::clamp(c.b, 0.0, 1.0) * 255);
        };
        double D = cl_dim, F = 0.55 * flash;
        double bz = (0.07 + 0.36 * cl_day) * D + F, bh = (0.2 + 0.45 * cl_day + 0.2 * cl_gold) * (0.55 + 0.45 * D) + F;
        double hh_hor = hc + (hw - hc) * std::min(1.0, 0.25 + 0.85 * cl_gold);
        RGB horizon_c;
        for (int g = 0; g < 5; g++) {
            double gk = g / 4.0 * (0.25 + 0.75 * std::max(cl_gold, cl_day * 0.6)) * (0.3 + 0.7 * cl_suna + 0.2 * cl_day);
            for (int i = 0; i < 24; i++) {
                double p = i / 23.0;
                double h = hc + (hh_hor - hc) * p * p + (hw - hh_hor) * gk;
                double b = bz + (bh - bz) * std::pow(p, 1.6) + 0.32 * gk;
                cl_sky[g][i] = pack(col(h, ss * (0.75 + 0.3 * gk), b));
            }
        }
        horizon_c = col(hh_hor + (hw - hh_hor) * 0.15, ss * 0.8, bh + 0.05);
        // Хмари: [туман 0..5][світло 0..9] — тінь (холодна) → освітлене (база) → край до сонця (теплий)
        double bs = (0.1 + 0.16 * cl_day) * D + F * 0.8, bl = (0.36 + 0.42 * cl_day) * (0.45 + 0.55 * D) + F;
        for (int f = 0; f < 6; f++) {
            double fk = std::min(1.0, f / 5.0 * (1 + cl_fogk * 0.5));
            for (int k = 0; k < 10; k++) {
                double q = k / 9.0;
                double h = hc + (hb - hc) * q + (q > 0.65 ? (hw - hb) * (q - 0.65) / 0.35 * (0.3 + 0.7 * cl_gold) : 0);
                double b = bs + (bl - bs) * q + (q > 0.8 ? 0.12 * (q - 0.8) / 0.2 * (0.4 + cl_gold) : 0);
                RGB c = col(h, ss * (0.6 + 0.45 * q * (0.4 + cl_gold)), b);
                cl_cld[f][k] = pack(mixc(c, horizon_c, fk));
            }
        }
        cl_rain = pack(col(hc + 8, ss * 0.5, bh + 0.12));
        cl_sun = pack(light ? hsl_rgb(hw, s * 0.75, 0.8 - 0.06 * cl_gold) : hsl_rgb(hw, s * 0.8, 0.8 - 0.12 * cl_gold));
        cl_moon = pack(col(hc + 10, ss * 0.35, 0.8));
        cl_star = pack(col(hc + 20, ss * 0.4, 0.62));

        // «Пухнастики» на висоті камери — летять назустріч і розчиняються, коли камера проходить крізь них
        cl_np = std::min(8, 2 + (int)(cl_cov * 4));
        for (int i = 0; i < cl_np; i++) {
            double sp = 0.8 + 0.5 * hashf(i * 17 + 3);
            double ph = std::fmod(t * 0.045 * sp + hashf(i * 29 + 1), 1.0);
            double pz = 7.0 * (1 - ph) + 0.12;
            double px = (hashf(i * 7 + 11) - 0.5) * 2.2, py = 0.02 + 0.12 * hashf(i * 13 + 5);
            if (std::fabs(px) < 0.25) px = px < 0 ? -0.3 : 0.3;
            ClPuff& P = cl_pf[i];
            P.pz = (float)pz; P.seed = i;
            P.sx = (float)(cols * 0.5 + px / pz * rows);
            P.sy = (float)(cl_hy + py / pz * rows);
            P.rr = (float)(0.32 / pz * rows);
            P.al = (float)(smooth(pz / 0.9) * smooth((7.1 - pz) / 2.0));
        }
        std::sort(cl_pf, cl_pf + cl_np, [](const ClPuff& a, const ClPuff& b) { return a.pz > b.pz; });
        clouds_rows(cols, rows);
    }
    // Шар хмар на відстані z (рядок): >=0 — індекс світла (0..9), -1 — прогалина. k — mip-рівень рядка.
    int clouds_layer(double x, int cols, int rows, double z, int k, double ofs, double thr, double bay, double fq) const {
        double wx = (x - cols * 0.5) / rows * z;
        double u = (wx + ofs + t * 0.04) * fq, v = (z + cl_T + ofs) * fq;
        float d = cl_mtex(k, u, v);
        if (k == 0) d += (cl_tex(cl_nhi, u * 3.7 + 91, v * 3.7 + 17) - 0.5f) * 0.22f;   // поблизу — дрібна деталь
        double a = (d - thr) / (0.1 + 0.05 * k);
        if (a < bay) return -1;
        float d2 = cl_mtex(k, u, v + 10.0 * (1 << k / 2));             // сонце попереду: густина спадає до сонця → підсвічений край
        double q = 0.42 + (d - d2) * 5.5 * (0.5 + cl_day) + std::min(1.0, a) * 0.18 - (d - thr) * 0.4;
        return std::clamp((int)(q * 9 + bay), 0, 9);
    }
    // Покадрові таблиці по рядках/стовпцях: глибина й mip шарів, індекс неба, сяйво сонця (exp розкладено на x·y)
    std::vector<float> cl_rz, cl_rzb, cl_rp, cl_gy, cl_gx, cl_sway; std::vector<int8_t> cl_rk, cl_rkb;
    void clouds_rows(int cols, int rows) {
        cl_rz.assign(rows, 0); cl_rzb.assign(rows, 0); cl_rp.assign(rows, 0); cl_gy.assign(rows, 0);
        cl_rk.assign(rows, 0); cl_rkb.assign(rows, 0); cl_sway.assign(rows, 0); cl_gx.assign(cols, 0);
        double hy = cl_hy, gr2 = cl_glowr * cl_glowr;
        for (int y = 0; y < rows; y++) {
            double fy = y + 0.5;
            double dyy = (fy - cl_suny) * 1.4;
            cl_gy[y] = (float)std::exp(-dyy * dyy / gr2);
            cl_rp[y] = (float)(std::pow(std::clamp(fy / hy, 0.0, 1.0), 0.85) * 23);
            cl_sway[y] = (float)(2 * std::sin(t * 0.6 + fy * 0.08));
            double dy = std::fabs(fy - hy) / rows;
            if (dy < 1e-3) dy = 1e-3;
            auto lod = [&](double h, double fq) {
                double tpp = fq / rows * std::max(h / dy, h / (dy * dy));
                return std::log2(std::max(1.0, tpp));
            };
            if (fy >= hy) {
                cl_rz[y] = (float)(0.16 / dy); cl_rzb[y] = (float)(0.5 / dy);
                double la = lod(0.16, 110), lb = lod(0.5, 110);
                cl_rk[y] = (int8_t)std::clamp((int)(la * 16), 0, 127);   // ×16 — дробова частина mip для дизера
                cl_rkb[y] = (int8_t)std::clamp((int)(lb * 16), 0, 127);
            } else {
                cl_rz[y] = (float)(0.3 / dy);
                cl_rk[y] = (int8_t)std::clamp((int)(lod(0.3, 70) * 16), 0, 127);
            }
        }
        for (int x = 0; x < cols; x++) { double dx = x - cl_sunx; cl_gx[x] = (float)std::exp(-dx * dx / gr2); }
    }
    uint32_t clouds_px(int x, int y, int cols, int rows, double bay) const {
        double fy = y + 0.5, hy = cl_hy;
        uint32_t out;
        auto sky = [&]() {
            double g = cl_gx[x] * cl_gy[y];
            return cl_sky[std::clamp((int)(g * 4 + bay * 0.9), 0, 4)][std::clamp((int)(cl_rp[y] + bay), 0, 23)];
        };
        auto mip = [&](int8_t k16) { return std::min(5, (int)(k16 / 16.0 + bay)); };
        if (fy < hy) {                               // небо: зорі, місяць, сонце, стеля хмар
            out = sky();
            if (cl_night > 0.3 && cl_cov < 0.85) {
                uint32_t h = hash(x * 92821u + y * 68917u);
                if ((h & 1023) < 4 && fy < hy * 0.8 && hashf(h) * 0.7 + 0.3 * std::sin(t * 1.3 + (h >> 10)) < cl_night - 0.15) out = cl_star;
            }
            if (cl_moona > 0) {
                double mx = x - cols * 0.74, my = fy - hy * 0.3, mr = std::max(3.0, rows * 0.035);
                double d1 = mx * mx + my * my, d2 = (mx + mr * 0.45) * (mx + mr * 0.45) + (my - mr * 0.2) * (my - mr * 0.2);
                if (d1 < mr * mr && d2 > mr * mr * 0.8 && bay < cl_moona) out = cl_moon;
            }
            bool on_sun = false;
            if (cl_suna > 0.45) {                    // диск зменшується за щільнішої хмарності (без дизерних «дірок»)
                double dx = x - cl_sunx, dyy = fy - cl_suny, r = cl_sunr * (0.45 + 0.55 * cl_suna);
                if (dx * dx + dyy * dyy < r * r) { out = cl_sun; on_sun = true; }
            }
            if (cl_cov > 0.55) {                     // стеля хмар: темні черева з підсвіченими краями
                double z = cl_rz[y];
                if (z < 40) {
                    double wx = (x - cols * 0.5) / rows * z;
                    const double FQ = 70;
                    double u = (wx + 37 + t * 0.03) * FQ, v = (z + cl_T * 1.2 + 91) * FQ;
                    int k = mip(cl_rk[y]);
                    float d = cl_mtex(k, u, v), thr = (float)(1.05 - (cl_cov - 0.45) * 1.25);
                    if ((d - thr) / 0.1 > bay + (on_sun ? 0.6 : 0)) {
                        float d2 = cl_mtex(k, u, v + 9.0 * (1 << k / 2));
                        double q = 0.22 + (d2 - d) * 4.5 + cl_suna * 0.15;
                        int f = std::clamp((int)(z / 40 * 5 + 0.6 * cl_fogk * 5 + bay), 0, 5);
                        out = cl_cld[f][std::clamp((int)(q * 9 + bay), 0, 9)];
                    }
                }
            }
        } else if ((fy - hy) / rows < 0.004) {
            out = sky();
        } else {                                     // під обрієм: ближній шар → дальній → імла внизу
            double zmax = 26 * (1 - 0.6 * cl_fogk);
            double thr = 1.0 - cl_cov, z = cl_rz[y];
            int q = clouds_layer(x, cols, rows, z, mip(cl_rk[y]), 0, thr, bay, 110);
            if (q >= 0) {
                int f = std::clamp((int)(z / zmax * 5 + bay), 0, 5);
                out = cl_cld[f][q];
            } else {
                double zb = cl_rzb[y];
                int qb = clouds_layer(x * 0.98, cols, rows, zb, mip(cl_rkb[y]), 53.7, thr - 0.12, bay, 110);
                if (qb >= 0) {
                    int f = std::clamp((int)(zb / zmax * 5 + 1.4 + bay), 0, 5);
                    out = cl_cld[f][std::max(0, qb / 2)];
                } else {
                    int f = std::clamp((int)(z / zmax * 5 + 2.5 + bay), 0, 5);
                    out = cl_cld[f][0];
                }
            }
        }
        for (int i = 0; i < cl_np; i++) {            // пухнастики (від дальніх до ближніх)
            const ClPuff& P = cl_pf[i];
            double ly = (fy - P.sy) / (P.rr * 0.7);
            if (ly < -1.4 || ly > 1.2) continue;
            double lx = (x - P.sx) / P.rr;
            if (lx < -1.4 || lx > 1.4) continue;
            double nz = cl_tex(cl_nhi, lx * 70 + P.seed * 41, ly * 50 + P.seed * 23);
            double m = 1 - lx * lx * 1.1 - ly * ly;   // купчаста хмара: основне тіло + 3 «горбки» зверху, пласке дно
            for (int b = 0; b < 3; b++) {
                double bx = lx - (b - 1) * 0.5 - (hashf(P.seed * 5 + b) - 0.5) * 0.3, by = ly + 0.35 + 0.3 * hashf(P.seed * 9 + b);
                double rb = 0.32 + 0.16 * hashf(P.seed * 3 + b);
                m = std::max(m, 1 - (bx * bx + by * by) / (rb * rb));
            }
            m += (nz - 0.5) * 0.7 - std::max(0.0, ly - 0.15) * 3;
            if (m <= 0 || P.al * std::min(1.0, m * 5) < bay) continue;
            double nz2 = cl_tex(cl_nhi, lx * 70 + P.seed * 41, (ly - 0.12) * 50 + P.seed * 23);
            double q = 0.5 - ly * 0.45 + (nz - nz2) * 3.0 + (P.sy > cl_suny ? 0.1 : 0) - (1 - std::min(1.0, m * 3)) * 0.1;
            int f = std::clamp((int)(P.pz / 26 * 5 + cl_fogk * 2 + bay * 0.8), 0, 5);
            out = cl_cld[f][std::clamp((int)(q * 9 + bay), 0, 9)];
        }
        if (cl_wx == 3 || cl_wx == 5) {              // дощ: косі риски
            int xs = x + (int)(fy * 0.28);
            uint32_t h = hash(xs * 7919u + 13);
            if (h % (cl_wx == 5 ? 3 : 4) == 0) {
                double sp = 55 + (h >> 4) % 40, len = 2 + (h >> 10) % 3, span = rows + 20;
                double d = std::fmod(t * sp + hashf(h) * span, span) - fy;
                if (d >= 0 && d < len && bay < 0.75) out = cl_rain;
            }
        } else if (cl_wx == 4) {                     // сніг: повільні пластівці з погойдуванням
            double yy = fy - t * (4.5 + 2 * hashf(x / 5 * 3 + 1));
            double xx = x + cl_sway[y];
            int cxi = (int)std::floor(xx / 5), cyi = (int)std::floor(yy / 5);
            uint32_t h = hash(cxi * 73856093u ^ cyi * 19349663u);
            if (h % 9 == 0 && (int)(xx - cxi * 5) == (int)(h >> 8) % 5 && (int)(yy - cyi * 5) == (int)(h >> 12) % 5) out = cl_moon;
        }
        return out;
    }

    // Рівень пікселя для стилю: 0..1 — яскравість, >0.93 — піна/акцентний «гребінь».
    double style_value(int x, int y, int cols, int rows, double fy, double bay, double cx, double cy) const {
        if (sid == S_EQ) {
            int band = x / 3;
            if (x % 3 == 2) return 0.05 + 0.06 * bay;                     // проміжок між смугами
            if (band < (int)eq_top.size()) {                               // швидкий шлях: висота смуги вже порахована на кадр
                double top = eq_top[band];
                if (fy < top - 0.5 / rows) return 0.07 + 0.05 * bay;
                if (fy < top + 1.0 / rows) return 0.97;
                return 0.55 + 0.35 * (1 - (fy - top));
            }
            double ph = hashf(band * 7 + 1) * 6.28, sp = 1.4 + 2.2 * hashf(band * 13 + 5);
            double lv = 0.5 + 0.28 * std::sin(t * sp + ph) + 0.16 * std::sin(t * sp * 2.3 + ph * 1.7)
                      + 0.1 * std::sin(t * 0.7 + band * 0.35);
            if (eq_mix > 0 && band < (int)eq_lv.size()) lv = eq_mix * (0.04 + 0.96 * eq_lv[band]) + (1 - eq_mix) * lv;
            lv = std::clamp(lv, eq_mix > 0 ? 0.04 : 0.08, 1.0);
            double top = 1.0 - lv;                                         // смуга росте знизу
            if (fy < top - 0.5 / rows) return 0.07 + 0.05 * bay;
            if (fy < top + 1.0 / rows) return 0.97;                        // «шапочка» смуги
            return 0.55 + 0.35 * (1 - (fy - top));
        }
        if (sid == S_RAIN) {
            uint32_t h = hash(x * 2654435761u + 17);
            if (x % 2 == 1 && (h & 3)) return 0.06 + 0.06 * bay;
            double sp = 7 + (h % 9), len = 5 + (h >> 8) % 9, span = rows + len + 6 + (h >> 16) % 12;
            double head = std::fmod(t * sp + hashf(x * 31 + 3) * span, span);
            double d = head - y;
            if (d >= 0 && d < 1) return 0.97;
            if (d > 0 && d < len) return 0.75 * (1 - d / len) + 0.12;
            return 0.06 + 0.06 * bay;
        }
        if (sid == S_VIRUS || sid == S_LIFE) {
            if (cells.size() != (size_t)cols * rows) return 0.1;
            uint8_t c = cells[y * cols + x];
            if (sid == S_VIRUS) {
                if (c == 1) return 0.97;
                if (c == 2) return 0.82;
                if (c > 2) return 0.62 * std::exp(-(c - 2) / 3.5) + 0.06 * (1.0 - double(c - 2) / (VK - 2)) + 0.07;  // короткий шлейф, далі — темний слід
                return 0.1 + 0.035 * std::sin(x * 0.21 + y * 0.33 + t * 0.6);     // здорові — ледь «дихають»
            }
            if (c & 0x80) return (c & 0x7F) < 2 ? 0.97 : 0.8 - std::min(0.25, (c & 0x7F) * 0.02);
            return c ? 0.1 + c * 0.06 : 0.07 + 0.04 * bay;
        }
        // waves
        double dx = x - cx, dy = (y - cy) * 1.6;
        double v = 1.25 * std::sin(x * 0.085 + y * 0.17 - t * 1.3)
                 + 0.75 * std::sin(x * 0.16 - y * 0.08 + t * 0.9)
                 + 0.45 * std::sin(x * 0.045 + y * 0.31 + t * 1.75)
                 + 0.4 * std::sin(std::sqrt(dx * dx + dy * dy) * 0.38 - t * 2.4);
        return (v / 2.85 + 1) * 0.5 - (fy - 0.5) * 0.12;
    }

    ~Waves() { if (surf) cairo_surface_destroy(surf); }

    void set_accent(const std::string& hex) {
        RGB c;
        if (!parse_hex(hex, c)) c = {0.6, 0.16, 0.81};
        rgb_hsl(c, hue0, sat0, light0);
        hue_lo = -15; hue_hi = 15;
    }
    // Межі відтінку = розкид акцентних кольорів pywal (color1..6) навколо акценту.
    // Сірі/надто темні/світлі кольори ігноруються; розкид обмежено ±60°.
    void set_palette_css(const std::string& path) {
        std::ifstream f(path);
        if (!f) return;
        std::stringstream ss; ss << f.rdbuf();
        std::string css = ss.str();
        double lo = 0, hi = 0;
        for (int i = 1; i <= 6; i++) {
            std::string key = "--color" + std::to_string(i) + ":";
            auto p = css.find(key);
            if (p == std::string::npos) continue;
            auto h = css.find('#', p);
            if (h == std::string::npos) continue;
            RGB c;
            if (!parse_hex(css.substr(h, 7), c)) continue;
            double hh, s, l; rgb_hsl(c, hh, s, l);
            if (s < 0.15 || l < 0.12 || l > 0.9) continue;
            double d = std::fmod(hh - hue0 + 540.0, 360.0) - 180.0;
            if (std::fabs(d) > 40) continue;   // колір з іншої групи відтінків — хвилі не мандрують у «чужий» колір
            lo = std::min(lo, d); hi = std::max(hi, d);
        }
        lo = std::max(lo, -30.0); hi = std::min(hi, 30.0);
        if (hi - lo < 16) { double c = (lo + hi) / 2; lo = c - 8; hi = c + 8; }
        hue_lo = lo; hue_hi = hi;
    }
    // Відтінок плавно гойдається між акцентними кольорами палітри, стартуючи рівно з акценту.
    double hue_at(double tt) const {
        double c = (hue_lo + hue_hi) / 2, a = (hue_hi - hue_lo) / 2;
        double ph = std::asin(std::clamp(-c / a, -1.0, 1.0));
        double x = tt * 0.21 + 0.6 * std::sin(tt * 0.13);  // «дихання» швидкості
        return hue0 + c + a * std::sin(x + ph);
    }
    // Горизонтальний зсув градієнта (fx = 0..1), у межах чверті розкиду палітри.
    double grad(double fx) const { return (hue_hi - hue_lo) * 0.5 * (fx - 0.5); }
    double sat() const { return std::clamp(sat0, 0.4, 0.92); }

    // Колір для UI: на старті рівно системний акцент, далі пливе разом із хвилями.
    RGB accent_now() const {
        double k = smooth(t / 1.5);  // перші 1.5 с — м'який перехід від точного акценту
        double l = light0 * (1 - k) + std::clamp(light0, 0.5, 0.62) * k;
        return hsl_rgb(hue_at(t), sat0 * (1 - k) + sat() * k, l);
    }

    // Малює кадр у surf (cols×rows «пікселів», ARGB32) без масштабування.
    // pixel_wallpaper віддає surf на GPU як текстуру — масштабує відеокарта, а не процесор.
    void render(int w, int h) {
        sid = style_id();
        const int L = 6;
        static const double BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        int cols = (w + cell - 1) / cell, rows = (h + cell - 1) / cell;
        if (cols <= 0 || rows <= 0) return;
        if (!surf || sw != cols || sh != rows) {
            if (surf) cairo_surface_destroy(surf);
            surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cols, rows);
            sw = cols; sh = rows;
        }
        cairo_surface_flush(surf);
        unsigned char* data = cairo_image_surface_get_data(surf);
        int stride = cairo_image_surface_get_stride(surf);

        double hue = hue_at(t), s = sat();
        double cx = cols * (0.5 + 0.35 * std::sin(t * 0.23)), cy = rows * (0.45 + 0.3 * std::cos(t * 0.31));
        double rv = ease_out_cubic(reveal) * 1.12;   // з запасом, щоб фронт повністю пройшов
        double dim = 0.35 + 0.65 * smooth(reveal);    // яскравість наростає разом із появою

        const int HB = 12;
        uint32_t pal[HB][L + 1];
        auto pack = [](RGB c) {
            return 0xFF000000u | (uint32_t)(c.r * 255) << 16 | (uint32_t)(c.g * 255) << 8 | (uint32_t)(c.b * 255);
        };
        for (int b = 0; b < HB; b++) {
            double hh = hue + grad((double)b / (HB - 1));  // горизонтальний градієнт у межах палітри
            for (int q = 0; q < L; q++) pal[b][q] = pack(hsl_rgb(hh, s, lt((0.13 + q * 0.085) * dim)));
            pal[b][L] = pack(light ? hsl_rgb(hh, std::max(s, 0.7), 0.4) : hsl_rgb(hh, 0.55, 0.86));  // піна
        }

        if (sid == S_AURORA) aurora_prepare(cols, rows, hue, s, dim);
        if (sid == S_HORIZON) horizon_prepare(cols, rows);
        if (sid == S_CITY) city_prepare(cols, rows, hue, dim);
        else if (!ct_frame.empty()) { ct_frame.clear(); ct_frame.shrink_to_fit(); ct_base.clear(); ct_base.shrink_to_fit(); ct_dep.clear(); ct_dep.shrink_to_fit(); ct_w = 0; }
        if (sid == S_ROOF) roof_prepare(cols, rows, hue, s);
        else if (!rt_frame.empty()) { rt_frame.clear(); rt_frame.shrink_to_fit(); rt_base.clear(); rt_base.shrink_to_fit(); rt_dep.clear(); rt_dep.shrink_to_fit(); rt_w = 0; }
        if (sid == S_BLACKHOLE) { blackhole_prepare(hue, s, dim); blackhole_particles(cols, rows); swirl_t = 0.15 * std::sin(t * 0.2); }
        if (sid == S_VORTEX) vortex_prepare(hue, s, dim);
        if (sid == S_SYNTH) synth_prepare(cols, rows, hue, s, dim);
        else if (!sw_m1.empty()) { sw_m1.clear(); sw_m1.shrink_to_fit(); sw_m2.clear(); sw_m2.shrink_to_fit(); }
        if (sid == S_LAVA) lava_prepare(cols, rows, hue, s, dim);
        if (sid == S_ABYSS) abyss_prepare(cols, rows, hue, s, dim);
        else if (!ab_frame.empty()) { ab_frame.clear(); ab_frame.shrink_to_fit(); ab_w = 0; }
        if (sid == S_CRYSTAL) crystal_prepare(cols, rows);
        if (sid == S_ATTRACTOR) attractor_prepare(cols, rows, hue, s, dim);
        else if (!at_frame.empty()) { at_frame.clear(); at_frame.shrink_to_fit(); at_w = 0; }
        if (sid == S_TOWER) tower_prepare(cols, rows, hue, s);
        else if (!tw_frame.empty()) { tw_frame.clear(); tw_frame.shrink_to_fit(); tw_base.clear(); tw_base.shrink_to_fit(); tw_dep.clear(); tw_dep.shrink_to_fit(); tw_w = 0; }
        if (sid == S_RIFT) rift_prepare(hue, s, dim);
        if (sid == S_VOID) void_prepare(cols, rows, hue, s, dim);
        else if (!vf_frame.empty()) { vf_frame.clear(); vf_frame.shrink_to_fit(); vf_acc.clear(); vf_acc.shrink_to_fit(); vf_w = 0; }
        if (sid == S_SKYLINE) skyline_prepare(cols, rows, hue, s, dim);
        if (sid == S_TORUS || sid == S_KNOT) torus_prepare(cols, rows, hue, s, dim, sid == S_TORUS ? 0 : 1);
        else if (!tr_frame.empty()) { tr_frame.clear(); tr_frame.shrink_to_fit(); tr_z.clear(); tr_z.shrink_to_fit(); tr_v.clear(); tr_v.shrink_to_fit(); tr_m.clear(); tr_g.clear(); tr_s.clear(); tr_tmp.clear(); tr_w = 0; }
        if (sid == S_CLOUDS) clouds_prepare(cols, rows, hue, s, dim);
        else if (!cl_nhi.empty()) { cl_nhi.clear(); cl_nhi.shrink_to_fit(); cl_nlo.clear(); cl_nlo.shrink_to_fit(); }
        // «Еквалайзер»: висоти смуг — раз на кадр (раніше 3 синуси на кожен піксель, ~8 мс/кадр);
        // палітра світлішає вгору екрана: низ — базовий відтінок, верх — найсвітліший (Байєр змішує рівні)
        const int EGB = 8;
        static uint32_t eqpal[EGB][HB][L + 1];
        if (sid == S_EQ) {
            eq_top.clear();
            int nb = (cols + 2) / 3;
            for (int band = 0; band < nb; band++) {
                double ph = hashf(band * 7 + 1) * 6.28, sp = 1.4 + 2.2 * hashf(band * 13 + 5);
                double lv = 0.5 + 0.28 * std::sin(t * sp + ph) + 0.16 * std::sin(t * sp * 2.3 + ph * 1.7)
                          + 0.1 * std::sin(t * 0.7 + band * 0.35);
                if (eq_mix > 0 && band < (int)eq_lv.size()) lv = eq_mix * (0.04 + 0.96 * eq_lv[band]) + (1 - eq_mix) * lv;
                eq_top.push_back((float)(1.0 - std::clamp(lv, eq_mix > 0 ? 0.04 : 0.08, 1.0)));
            }
            for (int g = 0; g < EGB; g++) {
                double k = (double)g / (EGB - 1);                          // 0 — низ, 1 — верх
                for (int b = 0; b < HB; b++) {
                    double hh = hue + grad((double)b / (HB - 1));
                    if (!light) {                                          // темна: вгору яскравіше й трохи тепліше
                        hh += 10 * k;
                        for (int q = 0; q < L; q++) eqpal[g][b][q] = pack(hsl_rgb(hh, s, std::min(0.82, (0.13 + q * 0.085) * dim * (0.72 + 0.62 * k))));
                        eqpal[g][b][L] = pack(hsl_rgb(hh, 0.55, 0.78 + 0.14 * k));
                    } else {                                               // світла: знизу насичений, вгору пастельний
                        hh -= 10 * k;
                        double ss = s * (1 - 0.3 * k);
                        for (int q = 0; q < L; q++) eqpal[g][b][q] = pack(hsl_rgb(hh, ss, lt((0.13 + q * 0.085) * dim * (1.12 - 0.5 * k))));
                        eqpal[g][b][L] = pack(hsl_rgb(hh, std::max(ss, 0.6), 0.36 + 0.2 * k));
                    }
                }
            }
        } else if (!eq_top.empty()) { eq_top.clear(); eq_top.shrink_to_fit(); }
        if (sid == S_VIRUS) cells_advance(cols, rows, 11);
        else if (sid == S_LIFE) cells_advance(cols, rows, 7);
        else if (!cells.empty()) { cells.clear(); cells.shrink_to_fit(); cells2.clear(); cells2.shrink_to_fit(); cells_style.clear(); }

        for (int y = 0; y < rows; y++) {
            uint32_t* row = (uint32_t*)(data + y * stride);
            double fy = (double)y / rows;
            double fade = fade_from < 1 ? (fy - fade_from) / (1 - fade_from) : 0;   // >=1 — без згасання
            for (int x = 0; x < cols; x++) {
                double bay = (BAYER[y & 3][x & 3] + 0.5) / 16.0;
                if (fade > 0 && bay < fade) { row[x] = 0; continue; }
                // поява: піксель видно, коли його поріг (висота + дизер) нижчий за фронт
                double thr = 0.6 * fy + 0.4 * bay;
                if (thr > rv) { row[x] = 0; continue; }
                int b = std::min(HB - 1, x * HB / cols);
                if (rv < 1.1 && thr > rv - 0.07) { row[x] = pal[b][L]; continue; }  // гребінь фронту
                if (sid == S_HORIZON) { row[x] = horizon_px(x, y, cols, rows, bay, hue, s, dim); continue; }
                if (sid == S_AURORA) { row[x] = aurora_px(x, y, cols, rows, hue); continue; }
                if (sid == S_CITY) { row[x] = city_px(x, y, dim); continue; }
                if (sid == S_ROOF) { row[x] = roof_px(x, y, dim); continue; }
                if (sid == S_TOWER) { row[x] = tower_px(x, y, dim); continue; }
                if (sid == S_BLACKHOLE) { row[x] = blackhole_px(x, y, cols, rows, bay, hue, s, dim); continue; }
                if (sid == S_VORTEX) { row[x] = vortex_px(x, y, cols, rows, bay); continue; }
                if (sid == S_SYNTH) { row[x] = synth_px(x, y, cols, rows, bay, hue, s, dim); continue; }
                if (sid == S_LAVA) { row[x] = lava_px(x, y, cols, rows, bay); continue; }
                if (sid == S_ABYSS) { row[x] = abyss_px(x, y); continue; }
                if (sid == S_CRYSTAL) { row[x] = crystal_px(x, y, cols, rows, bay, hue, s, dim); continue; }
                if (sid == S_ATTRACTOR) { row[x] = attractor_px(x, y); continue; }
                if (sid == S_RIFT) { row[x] = rift_px(x, y, cols, rows, bay); continue; }
                if (sid == S_VOID) { row[x] = void_px(x, y, cols, rows, bay); continue; }
                if (sid == S_SKYLINE) { row[x] = sk_px(x, y, cols, rows, bay); continue; }

                if (sid == S_TORUS || sid == S_KNOT) { row[x] = torus_px(x, y); continue; }

                if (sid == S_CLOUDS) { row[x] = clouds_px(x, y, cols, rows, bay); continue; }
                double v = style_value(x, y, cols, rows, fy, bay, cx, cy);
                double vq = v + (bay - 0.5) / L;
                if (sid == S_EQ) {
                    int g = std::clamp((int)((1 - fy) * (EGB - 1) + bay), 0, EGB - 1);   // дизер між рівнями градієнта
                    row[x] = vq > 0.93 ? eqpal[g][b][L] : eqpal[g][b][std::clamp((int)(vq * L), 0, L - 1)];
                    continue;
                }
                if (vq > 0.93) { row[x] = pal[b][L]; continue; }
                row[x] = pal[b][std::clamp((int)(vq * L), 0, L - 1)];
            }
        }
        if (sid == S_BLACKHOLE) blackhole_draw_particles(data, stride, cols, rows);
        else if (!bh_parts.empty()) { bh_parts.clear(); bh_parts.shrink_to_fit(); }
        cairo_surface_mark_dirty(surf);
    }

    void draw(cairo_t* cr, int w, int h) {
        render(w, h);
        if (!surf) return;
        cairo_save(cr);
        cairo_scale(cr, cell, cell);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_restore(cr);
    }
};

}  // namespace pw
