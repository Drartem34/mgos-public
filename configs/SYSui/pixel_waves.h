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
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
    double reveal = 1;       // 0..1 — поява (дизер-розчинення згори вниз з гребенем піни)
    int cell = 5;            // розмір «пікселя», логічні px
    double fade_from = 0.5;  // з якої висоти шапка розсипається у фон (0..1, >1 — без згасання)

    // Стиль анімації (налаштування «Стиль піксельних шапок»):
    //   "waves" — хвилі (за замовчуванням), "virus" — вірус (збудливе середовище, кільця зараження),
    //   "eq" — еквалайзер (вертикальні смуги), "rain" — піксельний дощ, "life" — гра «Життя»,
    //   "aurora" — піксельні хвилі-сяйво (як playhorizon.dev), "horizon" — піксельний захід сонця з горами,
    //   "blackhole" — гравітаційна сингулярність: тінь, акреційний диск, лінзування (курсор і вікна «засмоктує»).
    std::string style = "waves";
    enum { S_WAVES, S_VIRUS, S_EQ, S_RAIN, S_LIFE, S_HORIZON, S_AURORA, S_BLACKHOLE };
    int sid = S_WAVES;   // style як число — рахується раз на кадр (порівнювати рядки на кожен піксель дорого)
    int style_id() const {
        static const char* N[] = {"waves", "virus", "eq", "rain", "life", "horizon", "aurora", "blackhole"};
        for (int i = 0; i < 8; i++) if (style == N[i]) return i;
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
    static constexpr int VK = 9;
    void virus_seed(int cols, int rows, int n) {
        for (int i = 0; i < n; i++) cells[(rnd() % rows) * cols + rnd() % cols] = 1;
    }
    void virus_step(int cols, int rows) {
        int infected = 0;
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) {
                uint8_t c = cells[y * cols + x], o = 0;
                if (c > 0) o = c + 1 > VK ? 0 : c + 1;
                else {
                    int near = 0;
                    for (int k = 0; k < 4; k++) {
                        int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3);
                        if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
                        uint8_t q = cells[ny * cols + nx];
                        if (q >= 1 && q <= 2) near++;
                    }
                    if (near && rndf() < 0.42 + 0.14 * near) o = 1;   // неідеальне зараження → «рвані» фронти
                }
                if (o >= 1 && o <= 2) infected++;
                cells2[y * cols + x] = o;
            }
        cells.swap(cells2);
        if (infected < cols * rows / 400 + 1) virus_seed(cols, rows, 2);   // епідемія згасла — новий спалах
        else if (rndf() < 0.04) virus_seed(cols, rows, 1);
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
            if (style == "virus") virus_seed(cols, rows, 4); else life_seed(cols, rows, 0.3);
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
    // «Засмоктування»: від кожного вікна до діри по спіралі летять його копії — меншають, витягуються
    // вздовж радіуса (спагетифікація), прискорюються і гаснуть за горизонтом подій. Рахуються раз на кадр.
    struct BHCopy { float cx, cy, ux, uy, hr, ht, bound2, a; };
    std::vector<BHCopy> bh_copies;
    void blackhole_copies(int cols, int rows) {
        bh_copies.clear();
        const double hx = cols * 0.5, hy = rows * 0.5, Rs = rows * 0.13;
        int wi = 0;
        for (const auto& w : bh_wins) {
            double wcx = (w.x0 + w.x1) * 0.5, wcy = (w.y0 + w.y1) * 0.5;
            double hw = (w.x1 - w.x0) * 0.5, hh = (w.y1 - w.y0) * 0.5;
            double vx = wcx - hx, vy = wcy - hy, d0 = std::sqrt(vx * vx + vy * vy);
            if (d0 < 1 || hw < 1 || hh < 1) { wi++; continue; }
            double a0 = std::atan2(vy, vx);
            for (int k = 0; k < 3; k++) {
                double ph = std::fmod(t * 0.11 + k / 3.0 + wi * 0.37, 1.0);
                double rr = d0 * std::pow(1 - ph, 1.35);
                if (rr < Rs * 0.9) continue;                              // уже за горизонтом
                double a = a0 + 3.2 * ph * ph;                             // спіраль, що прискорюється
                double sc = 0.55 * std::pow(1 - ph, 1.1);
                double ux = std::cos(a), uy = std::sin(a);
                double hr = std::max(hw, hh) * sc * (1 + 1.8 * ph), ht = std::min(hw, hh) * sc * (1 - 0.55 * ph);
                double fade = std::sin(3.14159265 * ph) * std::min(1.0, (rr - Rs * 0.9) / (Rs * 0.6));
                bh_copies.push_back({(float)(hx + ux * rr), (float)(hy + uy * rr), (float)ux, (float)uy, (float)hr, (float)ht,
                                     (float)((hr * hr + ht * ht) * 1.05), (float)(fade * (w.active ? 0.75 : 0.5))});
            }
            wi++;
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
            uint32_t hs = hash((uint32_t)(ix * 73856093) ^ (uint32_t)(iy * 19349663) ^ 0xB1ACu);
            if ((hs & 511) < 5) {
                double tw = 0.55 + 0.45 * fsin(t * (0.4 + (hs >> 10) % 17 * 0.12) + (hs >> 16) % 628 * 0.01);
                RGB sc = ((hs >> 20) & 1) ? RGB{0.85, 0.82, 1.0} : bh_star;
                bg = mix(bg, sc, std::min(1.0, tw * (0.55 + 0.25 * mag)));
            }

            // копії вікон, що падають у діру (у координатах джерела — їх теж викривляє лінза)
            for (const auto& c : bh_copies) {
                double ex = qx - c.cx, ey = qy - c.cy;
                if (ex * ex + ey * ey > c.bound2) continue;
                double lr = ex * c.ux + ey * c.uy, lt = -ex * c.uy + ey * c.ux;
                double er = c.hr - std::fabs(lr), et = c.ht - std::fabs(lt);
                if (er < 0 || et < 0) continue;
                bool edge = std::min(er, et) < 0.9;
                bg = mix(bg, edge ? bh_rim : bh_acc, c.a * (edge ? 1.0 : 0.45));
            }
            // привиди вікон і курсора — лише там, де лінза помітно зсунула картинку
            double gw = std::clamp((shift - 1.5) / 4.0, 0.0, 1.0);
            if (gw > 0) {
                const RGB& acc = bh_acc;
                for (const auto& wr : bh_wins) {
                    if (qx < wr.x0 || qx > wr.x1 || qy < wr.y0 || qy > wr.y1) continue;
                    double e = std::min({qx - wr.x0, wr.x1 - qx, qy - wr.y0, wr.y1 - qy});
                    double a = e < 1.0 ? 0.9 : (qy - wr.y0 < 2.2 ? 0.5 : 0.22);   // рамка, «заголовок», тіло
                    if (!wr.active) a *= 0.6;
                    bg = mix(bg, e < 1.0 ? bh_rim : acc, a * gw * std::min(1.6, mag) / 1.6);
                }
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

    // Рівень пікселя для стилю: 0..1 — яскравість, >0.93 — піна/акцентний «гребінь».
    double style_value(int x, int y, int cols, int rows, double fy, double bay, double cx, double cy) const {
        if (sid == S_EQ) {
            int band = x / 3;
            if (x % 3 == 2) return 0.05 + 0.06 * bay;                     // проміжок між смугами
            double ph = hashf(band * 7 + 1) * 6.28, sp = 1.4 + 2.2 * hashf(band * 13 + 5);
            double lv = 0.5 + 0.28 * std::sin(t * sp + ph) + 0.16 * std::sin(t * sp * 2.3 + ph * 1.7)
                      + 0.1 * std::sin(t * 0.7 + band * 0.35);
            lv = std::clamp(lv, 0.08, 1.0);
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
                if (c > 2) return 0.7 * (1.0 - double(c - 2) / (VK - 1)) + 0.12;
                return 0.08 + 0.05 * std::sin(x * 0.3 + y * 0.5 + t);         // здорові — ледь «дихають»
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
        if (sid == S_BLACKHOLE) { blackhole_prepare(hue, s, dim); blackhole_copies(cols, rows); swirl_t = 0.15 * std::sin(t * 0.2); }
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
                if (sid == S_BLACKHOLE) { row[x] = blackhole_px(x, y, cols, rows, bay, hue, s, dim); continue; }

                double v = style_value(x, y, cols, rows, fy, bay, cx, cy);
                double vq = v + (bay - 0.5) / L;
                if (vq > 0.93) { row[x] = pal[b][L]; continue; }
                row[x] = pal[b][std::clamp((int)(vq * L), 0, L - 1)];
            }
        }
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
