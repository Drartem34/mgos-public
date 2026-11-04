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

    cairo_surface_t* surf = nullptr;
    int sw = 0, sh = 0;

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
            lo = std::min(lo, d); hi = std::max(hi, d);
        }
        lo = std::max(lo, -60.0); hi = std::min(hi, 60.0);
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

    void draw(cairo_t* cr, int w, int h) {
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
            for (int q = 0; q < L; q++) pal[b][q] = pack(hsl_rgb(hh, s, (0.13 + q * 0.085) * dim));
            pal[b][L] = pack(hsl_rgb(hh, 0.55, 0.86));  // піна
        }

        for (int y = 0; y < rows; y++) {
            uint32_t* row = (uint32_t*)(data + y * stride);
            double fy = (double)y / rows;
            double fade = (fy - fade_from) / (1 - fade_from);
            for (int x = 0; x < cols; x++) {
                double bay = (BAYER[y & 3][x & 3] + 0.5) / 16.0;
                if (fade > 0 && bay < fade) { row[x] = 0; continue; }
                // поява: піксель видно, коли його поріг (висота + дизер) нижчий за фронт
                double thr = 0.6 * fy + 0.4 * bay;
                if (thr > rv) { row[x] = 0; continue; }
                int b = std::min(HB - 1, x * HB / cols);
                if (rv < 1.1 && thr > rv - 0.07) { row[x] = pal[b][L]; continue; }  // гребінь фронту

                double dx = x - cx, dy = (y - cy) * 1.6;
                double v = 1.25 * std::sin(x * 0.085 + y * 0.17 - t * 1.3)
                         + 0.75 * std::sin(x * 0.16 - y * 0.08 + t * 0.9)
                         + 0.45 * std::sin(x * 0.045 + y * 0.31 + t * 1.75)
                         + 0.4 * std::sin(std::sqrt(dx * dx + dy * dy) * 0.38 - t * 2.4);
                v = (v / 2.85 + 1) * 0.5 - (fy - 0.5) * 0.12;
                double vq = v + (bay - 0.5) / L;
                if (vq > 0.93) { row[x] = pal[b][L]; continue; }
                row[x] = pal[b][std::clamp((int)(vq * L), 0, L - 1)];
            }
        }
        cairo_surface_mark_dirty(surf);
        cairo_save(cr);
        cairo_scale(cr, cell, cell);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_restore(cr);
    }
};

}  // namespace pw
