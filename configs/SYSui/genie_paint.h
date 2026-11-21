// Малювання «джина» (як на macOS): знімок вікна (x,y,w,h) стікає «лійкою» в іконку доку (центр tx,ty, розмір s), p ∈ [0,1].
//   1) низ вікна тягнеться до іконки, боки вигинаються: чим нижче, тим ближче до ширини іконки (лійка, p 0…0.5);
//   2) верх ковзає вниз крізь лійку — вміст стискається по висоті до іконки;
//   3) наприкінці знімок зливається з іконкою (коротке згасання).
// Лійку рисуємо горизонтальними смугами: кожна смуга — той самий текстурований прямокутник, обрізаний до своєї смуги
// зі своїм горизонтальним масштабом. Усе це GPU (Vulkan) — кілька сотень квадів за кадр, CPU майже не працює.
// Межі смуг по x дробові — GSK згладжує край кліпу, тому боки лійки рівні, без «драбинки».
// Окремо від genie_osd.cpp, щоб перевіряти рендер без екрана.
#pragma once
#include <gtk/gtk.h>
#include <algorithm>
#include <cmath>

// Звичайний TextureNode (лінійне масштабування), не append_scaled_texture: знімок з пам'яті на Intel/Vulkan не має
// mipmap-ів (TRILINEAR = LINEAR), а GskTextureScaleNode GSK не вміє малювати під opacity — на згасанні й у простому
// режимі КОЖНА смуга йшла окремим offscreen-проходом («Unsupported node GskTextureScaleNode»).
#define genie_tex(sn, tex, b) gtk_snapshot_append_texture(sn, tex, b)
static inline double genie_clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static inline double genie_ease(double t) { t = genie_clamp01(t); return t * t * (3 - 2 * t); }
static inline double genie_ease_in_out(double t) { t = genie_clamp01(t); return t < .5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }

struct GenieGeom {
    double L0, R0, T0, B0;    // вікно
    double tL, tR, iT, iB;    // іконка
    double bend;              // наскільки сформована лійка 0..1
    double top, bot;          // куди зараз відображається верх/низ знімка
    double span;              // висота лійки (від верху вікна до низу іконки)
};

static inline GenieGeom genie_geom(double p, double wx, double wy, double ww, double wh, double tx, double ty, double s) {
    GenieGeom g;
    g.L0 = wx; g.R0 = wx + ww; g.T0 = wy; g.B0 = wy + wh;
    g.tL = tx - s / 2; g.tR = tx + s / 2; g.iT = ty - s / 2; g.iB = ty + s / 2;
    // як у macOS: спершу низ вікна витягується до іконки й боки згинаються в лійку (вікно розтягнуте по висоті),
    // потім уже весь вміст стікає крізь лійку — верх доганяє низ
    g.bend = genie_ease(p / 0.40);
    double kb = genie_ease_in_out(p / 0.50);            // низ знімка — до іконки за першу половину
    double kt = genie_ease_in_out((p - 0.18) / 0.82);   // верх рушає трохи пізніше й доганяє в кінці
    g.top = g.T0 + (g.iT - g.T0) * kt;
    g.bot = g.B0 + (g.iB - g.B0) * kb;
    g.span = std::max(1.0, g.iB - std::min(g.T0, g.iT));
    return g;
}

// ширина лійки на висоті y екрана
static inline void genie_edges(const GenieGeom& g, double y, double& l, double& r) {
    double u = genie_clamp01((y - std::min(g.T0, g.iT)) / g.span);
    double w = genie_ease(u) * g.bend;
    l = g.L0 + (g.tL - g.L0) * w;
    r = g.R0 + (g.tR - g.R0) * w;
}

// Простий режим (без джина): знімок рівномірно зменшується й зсувається в іконку + згасає
static bool genie_plain = false;
static void genie_paint_plain(GtkSnapshot* sn, GdkTexture* tex, double p, double wx, double wy, double ww, double wh,
                              double tx, double ty, double s) {
    double alpha = 1 - genie_ease((p - 0.55) / 0.45);
    if (alpha <= 0.01) return;
    double e = genie_ease(p);
    double k = 1 + (std::min(1.0, s / std::max(ww, wh)) - 1) * e;
    double cx = wx + ww / 2 + (tx - wx - ww / 2) * e, cy = wy + wh / 2 + (ty - wy - wh / 2) * e;
    double w = ww * k, h = wh * k;
    graphene_rect_t b = GRAPHENE_RECT_INIT((float)(cx - w / 2), (float)(cy - h / 2), (float)w, (float)h);
    if (alpha < 0.999) gtk_snapshot_push_opacity(sn, alpha);
    genie_tex(sn, tex, &b);
    if (alpha < 0.999) gtk_snapshot_pop(sn);
}

static void genie_paint(GtkSnapshot* sn, GdkTexture* tex, double p, double wx, double wy, double ww, double wh,
                        double tx, double ty, double s) {
    if (genie_plain) { genie_paint_plain(sn, tex, p, wx, wy, ww, wh, tx, ty, s); return; }
    double alpha = 1 - genie_ease((p - 0.86) / 0.14);
    if (alpha <= 0.01) return;
    if (p <= 0.0005) {   // спокій: рівно вікно, один квад
        graphene_rect_t b = GRAPHENE_RECT_INIT((float)wx, (float)wy, (float)ww, (float)wh);
        gtk_snapshot_append_texture(sn, tex, &b);
        return;
    }
    GenieGeom g = genie_geom(p, wx, wy, ww, wh, tx, ty, s);
    if (g.bot - g.top < 0.5) return;
    if (alpha < 0.999) gtk_snapshot_push_opacity(sn, alpha);
    // смуги: висота підбирається так, щоб край зсувався не більше ніж на ~1 px між сусідніми смугами
    double y = std::floor(g.top);
    double yend = std::ceil(g.bot);
    while (y < yend) {
        double l0, r0, l1, r1;
        genie_edges(g, y, l0, r0);
        genie_edges(g, y + 4, l1, r1);
        double slope = std::max(std::abs(l1 - l0), std::abs(r1 - r0)) / 4;
        double h = std::clamp(std::floor(1.0 / std::max(slope, 1e-3)), 1.0, 24.0);
        double y1 = std::min(y + h, yend);
        double l, r; genie_edges(g, (y + y1) / 2, l, r);
        if (r - l > 0.25) {
            graphene_rect_t clip = GRAPHENE_RECT_INIT((float)l, (float)y, (float)(r - l), (float)(y1 - y));
            graphene_rect_t b = GRAPHENE_RECT_INIT((float)l, (float)g.top, (float)(r - l), (float)(g.bot - g.top));
            gtk_snapshot_push_clip(sn, &clip);
            genie_tex(sn, tex, &b);
            gtk_snapshot_pop(sn);
        }
        y = y1;
    }
    if (alpha < 0.999) gtk_snapshot_pop(sn);
}
