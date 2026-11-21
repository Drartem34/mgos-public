// clock_popover.h — клік по годиннику в топбарі: бульбашка «розростається» в блок
// у стилі GUI-інсталятора mgOS (піксельні хвилі, піксельний текст і прогрес-бари).
//
//   • шапка: піксельні хвилі + великий піксельний годинник, дата, аптайм, тиждень
//   • міні-календар (колесо — гортати місяці, клік по назві — назад до сьогодні)
//   • музика: будь-який MPRIS-плеєр, «хвилястий» прогрес Material 3 з квадратиків,
//     кнопка Play морфиться (коло ↔ квадрат) як у Android 15/16, клік по хвилі — перемотка
//   • «Сьогодні»: сегментовані піксельні бари дня / тижня / року
//   • фокус-таймер 25/5 хв: відлік видно прямо в бульбашці годинника, в кінці — сповіщення
//
// Підключається з topbar.cpp ПІСЛЯ music_cache_dir()/update_clock() — це «unity»-хедер.
// Вікно створюється лише при першому кліку; анімація (tick) працює лише поки воно відкрите.
#pragma once
#include "pixel_waves.h"
#include <pango/pangocairo.h>
#include <cmath>
#include <csignal>
#include <glib/gstdio.h>
#include <map>
#include <set>
#include <regex>
#include <algorithm>

namespace cpop {

// ===================== СТАН =====================
static GtkWidget *g_win = nullptr, *g_fixed = nullptr, *g_card = nullptr, *g_bg = nullptr, *g_content = nullptr;
static GtkWidget *g_header = nullptr, *g_cal = nullptr, *g_cal_title = nullptr, *g_wave = nullptr, *g_bars = nullptr;
static GtkWidget *g_art = nullptr, *g_title = nullptr, *g_artist = nullptr, *g_src = nullptr;
static GtkWidget *g_play_btn = nullptr, *g_focus_btn = nullptr, *g_break_btn = nullptr;
static std::vector<GtkWidget*> g_px_areas;   // перемальовуються кожен кадр (hover, «живий» акцент)
static pw::Waves g_wv;
static double g_t = 0;
static gint64 g_t0 = 0;
static guint g_tick = 0, g_poll = 0;
static bool g_open = false;
static gint64 g_anim_start = 0;
static double g_v = 0, g_v_from = 0;            // 0 — бульбашка, 1 — блок
static double g_from_w = 80, g_from_h = 30, g_cx = 400, g_top = 5;
static int g_W = 660, g_H = 440;
static bool g_content_on = false;
static double g_content_a = 0;
static int g_cal_off = 0;                        // зсув місяців у календарі
static double g_play_round = 4;                  // форма кнопки Play: 1 — квадрат, 4 — «коло»
static double g_amp = 0;                         // амплітуда хвилі прогресу
static const double OPEN_MS = 430, CLOSE_MS = 540;
static const int BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static void hide_pop();

// ── Режим «Дроп»: файл, перетягнутий на годинник → Quick Share ──
static GtkWidget *g_main_content = nullptr, *g_drop_content = nullptr;
static GtkWidget *g_drop_zone = nullptr, *g_drop_glyph = nullptr, *g_drop_title = nullptr, *g_drop_sub = nullptr;
static GtkWidget *g_drop_status = nullptr, *g_drop_pin = nullptr;
static GtkWidget *g_qs_send = nullptr, *g_qs_cancel = nullptr, *g_qs_open = nullptr, *g_qs_install = nullptr;
static int g_main_W = 660, g_main_H = 440, g_drop_W = 420, g_drop_H = 300;
static bool g_drop_mode = false;
static std::vector<std::string> g_drop_files;
static GSubprocess* g_qs_proc = nullptr;
// QR-передача (mgos-share): сторінка в локальній Wi-Fi, телефон сканує камерою — без додатків
static GSubprocess* g_sh_proc = nullptr;
static GtkWidget *g_drop_qr = nullptr, *g_sh_allow = nullptr;
static bool g_sh_opened = false;
static GtkWidget *g_last_bubble = nullptr, *g_last_bar = nullptr;
// Mg Share (свої пристрої): pair / push через mgos-share
static GSubprocess* g_dv_proc = nullptr;
static std::string g_dv_mode, g_dv_target;
static void dv_stop();
static void dv_pair(GtkButton* = nullptr, gpointer = nullptr);
static void sh_start();

static std::string fs_basename(const std::string& p) {
    gchar* b = g_path_get_basename(p.c_str()); std::string r = b; g_free(b); return r;
}
static double ease_out_soft(double k) { double x = 1 - k; return 1 - x * x * x; }   // м'яке гальмування, без overshoot
// Стартова «форма» бульбашки годинника (перечитується при кожному show_pop) — картка морфиться з неї
static double g_fbg[4] = {0.118, 0.118, 0.18, 0.8}, g_ffg[3] = {0.9, 0.9, 0.9}, g_fbd[4] = {1, 1, 1, 0.1};
static double g_tx = 10, g_ty = 6, g_th = 18;     // де саме в бульбашці малюється текст годинника (лівий верх, висота)
static std::string g_from_text;
static std::string g_ffamily;            // шрифт справжньої мітки годинника — малюємо ним, щоб підміна не «блимала»
static double g_fpx = 14; static int g_fweight = 700;
static const char* g_ptext_family = nullptr;   // тимчасова підміна сімейства в ptext
static PangoLayout* g_lay = nullptr;           // копія Pango-шару справжньої мітки годинника
static GtkWidget* g_hidden_bubble = nullptr;       // бульбашка, яку ми сховали на час блоку
static double ease_in_out(double k) { return k < 0.5 ? 4 * k * k * k : 1 - std::pow(-2 * k + 2, 3) / 2; }
// Пружинне згортання блоку в бульбашку: швидке схлопування (0..k0) → затухаючі коливання розміру з овершутом
static double spring_close(double k) {
    // справжня згасаюча пружина: e^(-a·t)·(cos(b·t) + a/b·sin(b·t)) — починається з 1 зі швидкістю 0, один плавний рух,
    // трохи «пролітає» нуль (бульбашку) і осідає без сіпань
    const double a = 6.0, b = 7.0;
    return std::exp(-a * k) * (std::cos(b * k) + a / b * std::sin(b * k));
}
static gint64 now_us() { return g_get_monotonic_time(); }

// ===================== ТЕКСТ =====================
// Звичайний текст (Pango): align 0 — ліво, 1 — право, 2 — центр. Повертає ширину.
static int ptext(cairo_t* cr, const std::string& s, double x, double y, double size, PangoWeight wt,
                 double r, double g, double b, double a, int align = 0, double spacing = 0) {
    PangoLayout* l = pango_cairo_create_layout(cr);
    PangoFontDescription* fd = pango_font_description_from_string(g_ptext_family ? g_ptext_family : "JetBrainsMono Nerd Font");
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, wt);
    pango_layout_set_font_description(l, fd);
    if (spacing > 0) {
        PangoAttrList* al = pango_attr_list_new();
        pango_attr_list_insert(al, pango_attr_letter_spacing_new((int)(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, al);
        pango_attr_list_unref(al);
    }
    pango_layout_set_text(l, s.c_str(), -1);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    double xx = align == 1 ? x - tw : align == 2 ? x - tw / 2.0 : x;
    cairo_move_to(cr, xx, y);
    cairo_set_source_rgba(cr, r, g, b, a);
    pango_cairo_show_layout(cr, l);
    pango_font_description_free(fd);
    g_object_unref(l);
    return tw;
}

// Піксельний шрифт 5×7 (як цифри інсталятора) + вузькі «:» і пробіл
struct Glyph { int w; const char* b; };
static Glyph glyph(char ch) {
    switch (ch) {
        case '0': return {5, "01110100011001110101110011000101110"};
        case '1': return {5, "00100011000010000100001000010001110"};
        case '2': return {5, "01110100010000100010001000100011111"};
        case '3': return {5, "11110000010000101110000010000111110"};
        case '4': return {5, "00010001100101010010111110001000010"};
        case '5': return {5, "11111100001111000001000011000101110"};
        case '6': return {5, "00110010001000011110100011000101110"};
        case '7': return {5, "11111000010001000100010000100001000"};
        case '8': return {5, "01110100011000101110100011000101110"};
        case '9': return {5, "01110100011000101111000010001001100"};
        case '%': return {5, "11001110100001000100010000101110011"};
        case '-': return {5, "00000000000000011111000000000000000"};
        case ':': return {2, "00111100111100"};
        case ' ': return {2, "00000000000000"};
        default:  return {5, "00000000000000000000000000000000000"};
    }
}
static int px_text_width(const std::string& s, int cell, bool bold) {
    int cols = 0;
    for (size_t i = 0; i < s.size(); i++) cols += glyph(s[i]).w + (bold ? 1 : 0) + (i + 1 < s.size() ? 1 : 0);
    return cols * cell;
}
// solid == nullptr — колір «живої» хвилі акценту (як у інсталятора), інакше — заданий колір.
static int px_text(cairo_t* cr, const std::string& s, double x, double y, int cell, int align, bool bold,
                   const pw::RGB* solid = nullptr, double alpha = 1, bool shadow = true) {
    int tw = px_text_width(s, cell, bold);
    double ox = align == 1 ? x - tw : align == 2 ? x - tw / 2.0 : x;
    for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
        int gx0 = 0;
        for (char ch : s) {
            Glyph g = glyph(ch);
            int dw = g.w + (bold ? 1 : 0);
            auto on = [&](int r, int c) { return c >= 0 && c < g.w && g.b[r * g.w + c] == '1'; };
            for (int r = 0; r < 7; r++) for (int c = 0; c < dw; c++) {
                if (!(on(r, c) || (bold && on(r, c - 1)))) continue;
                int gx = gx0 + c;
                double px = ox + gx * cell, py = y + r * cell;
                if (pass == 0) {
                    cairo_set_source_rgba(cr, 0, 0, 0, 0.45 * alpha);
                    cairo_rectangle(cr, px + cell * 0.4, py + cell * 0.4, cell - 1, cell - 1);
                } else {
                    pw::RGB col;
                    if (solid) col = *solid;
                    else {
                        double wave = 0.5 + 0.5 * std::sin(gx * 0.45 - g_t * 4 + r * 0.4);
                        col = pw::hsl_rgb(g_wv.hue_at(g_t) + 5.0 * gx, g_wv.sat(), 0.6 + 0.14 * wave - r * 0.015);
                    }
                    cairo_set_source_rgba(cr, col.r, col.g, col.b, alpha);
                    cairo_rectangle(cr, px, py, cell >= 3 ? cell - 1 : cell, cell >= 3 ? cell - 1 : cell);   // дрібні — без зазору
                }
                cairo_fill(cr);
            }
            gx0 += dw + 1;
        }
    }
    return tw;
}
// Колір елементів — рівно системний акцент (sysui::accent), лише з легким градієнтом яскравості.
static double g_acc_h = 280, g_acc_s = 0.7, g_acc_l = 0.55;
static pw::RGB accent_px(double x01, double light) {
    double l = std::clamp(light + (g_acc_l - 0.56) * 0.6 + (x01 - 0.5) * 0.04, 0.0, 0.95);
    return pw::hsl_rgb(g_acc_h + (x01 - 0.5) * 6, std::clamp(g_acc_s, 0.35, 0.95), l);
}

// ===================== ПІКСЕЛЬНІ КНОПКИ (як в інсталяторі) =====================
enum { PX_NEUTRAL = 0, PX_PRIMARY = 1, PX_PLAY = 2 };
static void draw_pxbtn(GtkDrawingArea* da, cairo_t* cr, int w, int h, gpointer data) {
    GtkWidget* btn = gtk_widget_get_ancestor(GTK_WIDGET(da), GTK_TYPE_BUTTON);
    GtkStateFlags st = btn ? gtk_widget_get_state_flags(btn) : (GtkStateFlags)0;
    bool hover = st & GTK_STATE_FLAG_PRELIGHT, press = st & GTK_STATE_FLAG_ACTIVE;
    int kind = GPOINTER_TO_INT(data);
    const int cell = 3, gap = 1;
    int cols = w / cell, rows = h / cell;
    if (cols < 4 || rows < 3) return;
    double ox = (w - cols * cell) / 2.0, oy = (h - rows * cell) / 2.0;
    double sweep = std::fmod(g_t * 28.0, cols + 24.0) - 12;
    double round = kind == PX_PLAY ? g_play_round : 1.0;
    for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++) {
        int cx = std::min(x, cols - 1 - x), cy = std::min(y, rows - 1 - y);
        if (cx + cy < round) continue;                               // піксельні кути (морф форми)
        bool edge = cx == 0 || cy == 0;
        double bay = BAYER[y & 3][x & 3] / 16.0;
        double lift = hover ? 0.16 * std::exp(-std::pow((x - y * 0.6 - sweep) / 3.0, 2)) : 0;
        if (press) lift -= 0.08;
        double a = 1;
        pw::RGB c;
        if (kind == PX_NEUTRAL) {
            c = {1, 1, 1};
            a = (hover ? 0.13 : 0.075) + (edge ? 0.05 : 0) + lift * 0.6 + (bay - 0.5) * 0.02;
        } else {
            double l = 0.56 + (bay - 0.5) * 0.05 + lift;
            if (y == 0 || (cy == 0 && y < rows / 2)) l += 0.1;
            if (y == rows - 1) l -= 0.12;
            c = accent_px((double)x / cols, std::clamp(l, 0.0, 0.92));
        }
        cairo_set_source_rgba(cr, c.r, c.g, c.b, std::clamp(a, 0.0, 1.0));
        cairo_rectangle(cr, ox + x * cell, oy + y * cell, cell - gap, cell - gap);
        cairo_fill(cr);
    }
}
static GtkWidget* pxbutton(const std::string& text, int kind, GCallback cb, gpointer d = nullptr, const char* extra = nullptr) {
    GtkWidget* b = gtk_button_new();
    gtk_widget_add_css_class(b, "cp-pxbtn");
    gtk_widget_set_cursor_from_name(b, "pointer");
    GtkWidget* ov = gtk_overlay_new();
    GtkWidget* da = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_pxbtn, GINT_TO_POINTER(kind), nullptr);
    gtk_overlay_set_child(GTK_OVERLAY(ov), da);
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_widget_add_css_class(l, "cp-pxlbl");
    if (extra) gtk_widget_add_css_class(l, extra);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), l);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(ov), l, TRUE);
    gtk_button_set_child(GTK_BUTTON(b), ov);
    g_object_set_data(G_OBJECT(b), "lbl", l);
    g_px_areas.push_back(da);
    if (cb) g_signal_connect(b, "clicked", cb, d);
    return b;
}
static void set_btn_label(GtkWidget* b, const std::string& t) {
    if (auto* l = (GtkWidget*)g_object_get_data(G_OBJECT(b), "lbl")) gtk_label_set_text(GTK_LABEL(l), t.c_str());
}

// ===================== ДАТА =====================
static const char* MONTHS[] = {"СІЧЕНЬ", "ЛЮТИЙ", "БЕРЕЗЕНЬ", "КВІТЕНЬ", "ТРАВЕНЬ", "ЧЕРВЕНЬ",
                               "ЛИПЕНЬ", "СЕРПЕНЬ", "ВЕРЕСЕНЬ", "ЖОВТЕНЬ", "ЛИСТОПАД", "ГРУДЕНЬ"};
static const char* MONTHS_GEN[] = {"СІЧНЯ", "ЛЮТОГО", "БЕРЕЗНЯ", "КВІТНЯ", "ТРАВНЯ", "ЧЕРВНЯ",
                                   "ЛИПНЯ", "СЕРПНЯ", "ВЕРЕСНЯ", "ЖОВТНЯ", "ЛИСТОПАДА", "ГРУДНЯ"};
static const char* WDAYS[] = {"НЕДІЛЯ", "ПОНЕДІЛОК", "ВІВТОРОК", "СЕРЕДА", "ЧЕТВЕР", "ПʼЯТНИЦЯ", "СУБОТА"};
static const char* WD_SHORT[] = {"ПН", "ВТ", "СР", "ЧТ", "ПТ", "СБ", "НД"};

static struct tm now_tm() { time_t t = time(nullptr); struct tm m; localtime_r(&t, &m); return m; }
static int days_in_month(int y, int m) {   // m 0..11
    static const int D[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return D[m] + (m == 1 && leap ? 1 : 0);
}
static std::string mmss(long long s) {
    if (s < 0) s = 0;
    char b[32];
    if (s >= 3600) snprintf(b, sizeof b, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, sizeof b, "%lld:%02lld", s / 60, s % 60);
    return b;
}

// ===================== ШАПКА =====================
static void draw_header(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    struct tm m = now_tm();
    char hm[8], ss[4];
    snprintf(hm, sizeof hm, "%02d:%02d", m.tm_hour, m.tm_min);
    snprintf(ss, sizeof ss, "%02d", m.tm_sec);
    pw::RGB dim{0.96, 0.96, 0.97};
    int cw = px_text(cr, hm, 26, 22, 7, 0, true, &dim);
    px_text(cr, ss, 26 + cw + 10, 22 + 7 * 7 - 7 * 3, 3, 0, false, &dim, 0.55);

    std::string date = std::string(WDAYS[m.tm_wday]) + " · " + std::to_string(m.tm_mday) + " " + MONTHS_GEN[m.tm_mon] + " " +
                       std::to_string(m.tm_year + 1900);
    ptext(cr, date, 27, 82, 11.5, PANGO_WEIGHT_ULTRABOLD, 0.96, 0.96, 0.97, 0.9, 0, 1.2);

    // праворуч: аптайм і номер тижня
    double up = 0;
    { std::ifstream f("/proc/uptime"); f >> up; }
    long long u = (long long)up;
    std::string upt = u >= 86400 ? std::to_string(u / 86400) + " Д " + std::to_string(u / 3600 % 24) + " ГОД"
                                 : std::to_string(u / 3600) + " ГОД " + std::to_string(u / 60 % 60) + " ХВ";
    char wk[8];
    strftime(wk, sizeof wk, "%V", &m);
    ptext(cr, "АПТАЙМ", w - 26, 24, 9.5, PANGO_WEIGHT_ULTRABOLD, 1, 1, 1, 0.5, 1, 1.2);
    ptext(cr, upt, w - 26, 38, 11.5, PANGO_WEIGHT_ULTRABOLD, 1, 1, 1, 0.92, 1, 1);
    ptext(cr, "ТИЖДЕНЬ", w - 26, 60, 9.5, PANGO_WEIGHT_ULTRABOLD, 1, 1, 1, 0.5, 1, 1.2);
    pw::RGB white{1, 1, 1};
    px_text(cr, std::to_string(atoi(wk)), w - 26, 76, 2, 1, true, &white, 0.92, false);
}

// ===================== КАЛЕНДАР =====================
static void cal_title_update() {
    struct tm m = now_tm();
    int mon = m.tm_mon + g_cal_off, yr = m.tm_year + 1900;
    while (mon < 0) { mon += 12; yr--; }
    while (mon > 11) { mon -= 12; yr++; }
    std::string t = std::string(MONTHS[mon]) + " " + std::to_string(yr);
    if (g_cal_title) gtk_label_set_text(GTK_LABEL(g_cal_title), t.c_str());
}
static void draw_cal(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    struct tm m = now_tm();
    int mon = m.tm_mon + g_cal_off, yr = m.tm_year + 1900;
    while (mon < 0) { mon += 12; yr--; }
    while (mon > 11) { mon -= 12; yr++; }
    struct tm f{};
    f.tm_year = yr - 1900; f.tm_mon = mon; f.tm_mday = 1; f.tm_hour = 12;
    mktime(&f);
    int first = (f.tm_wday + 6) % 7;   // 0 = понеділок
    int dim = days_in_month(yr, mon);
    int pmon = (mon + 11) % 12, pyr = mon == 0 ? yr - 1 : yr;
    int pdim = days_in_month(pyr, pmon);

    double cw = w / 7.0, top = 22, rh = (h - top) / 6.0;
    for (int c = 0; c < 7; c++)
        ptext(cr, WD_SHORT[c], cw * c + cw / 2, 2, 9.5, PANGO_WEIGHT_ULTRABOLD, 1, 1, 1, c >= 5 ? 0.35 : 0.55, 2, 1);

    pw::RGB white{0.96, 0.96, 0.97}, ink{0.08, 0.08, 0.094};
    for (int i = 0; i < 42; i++) {
        int r = i / 7, c = i % 7;
        int day = i - first + 1;
        bool inside = day >= 1 && day <= dim;
        int shown = inside ? day : day < 1 ? pdim + day : day - dim;
        double cx = cw * c + cw / 2, cy = top + rh * r + rh / 2;
        bool today = inside && g_cal_off == 0 && day == m.tm_mday;
        std::string s = std::to_string(shown);
        if (today) {
            // піксельна «плашка» сьогоднішнього дня з хвилею акценту
            const int cell = 3;
            int bw = 10, bh = 7;
            double bx = cx - bw * cell / 2.0, by = cy - bh * cell / 2.0;
            for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
                int ex = std::min(x, bw - 1 - x), ey = std::min(y, bh - 1 - y);
                if (ex + ey < 1) continue;
                double l = 0.6 + 0.08 * std::sin(x * 0.7 - g_t * 4) + (BAYER[y & 3][x & 3] / 16.0 - 0.5) * 0.05;
                pw::RGB col = accent_px((double)x / bw, l);
                cairo_set_source_rgb(cr, col.r, col.g, col.b);
                cairo_rectangle(cr, bx + x * cell, by + y * cell, cell - 1, cell - 1);
                cairo_fill(cr);
            }
            px_text(cr, s, cx, cy - 7, 2, 2, true, &ink, 1, false);
        } else {
            double a = !inside ? 0.18 : c >= 5 ? 0.55 : 0.92;
            px_text(cr, s, cx, cy - 7, 2, 2, false, &white, a, false);
        }
    }
}
static gboolean on_cal_scroll(GtkEventControllerScroll*, double, double dy, gpointer) {
    g_cal_off += dy > 0 ? 1 : -1;
    cal_title_update();
    gtk_widget_queue_draw(g_cal);
    return TRUE;
}
static void cal_shift(GtkButton*, gpointer d) {
    int k = GPOINTER_TO_INT(d);
    g_cal_off = k == 0 ? 0 : g_cal_off + k;
    cal_title_update();
    gtk_widget_queue_draw(g_cal);
}

// ===================== МУЗИКА (MPRIS, будь-який плеєр) =====================
struct Player {
    std::string bus, status, title, artist, art, trackid;
    gint64 len = 0, pos = 0, pos_at = 0;
    double rate = 1;
    bool can_seek = false;
};
static std::map<std::string, Player> g_players;
static std::string g_cur;          // шина вибраного плеєра
static std::string g_art_want, g_art_have;

static GDBusConnection* sess() {
    static GDBusConnection* c = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    return c;
}
static Player* cur() { auto it = g_players.find(g_cur); return it == g_players.end() ? nullptr : &it->second; }
static bool playing(const Player* p) { return p && p->status == "Playing"; }
static gint64 shown_pos(const Player* p) {
    if (!p) return 0;
    gint64 v = p->pos + (playing(p) ? (gint64)((now_us() - p->pos_at) * p->rate) : 0);
    return p->len > 0 ? std::clamp<gint64>(v, 0, p->len) : std::max<gint64>(v, 0);
}
static std::string player_name(const std::string& bus) {
    std::string n = bus.substr(strlen("org.mpris.MediaPlayer2."));
    if (auto d = n.find('.'); d != std::string::npos) n = n.substr(0, d);
    for (auto& ch : n) ch = (char)std::toupper((unsigned char)ch);
    return n;
}

static void art_set_file(const std::string& path) {
    int sf = std::max(1, gtk_widget_get_scale_factor(g_art));
    GdkTexture* t = sysui::load_texture_cover(path.c_str(), 76 * sf, 76 * sf);
    gtk_image_set_from_paintable(GTK_IMAGE(g_art), t ? GDK_PAINTABLE(t) : nullptr);
    if (t) g_object_unref(t);
}
struct ArtJob2 { std::string url, path, tmp; };
static void art_load(const std::string& url) {
    g_art_want = url;
    if (url == g_art_have) return;
    if (url.empty()) { gtk_image_set_from_icon_name(GTK_IMAGE(g_art), "audio-x-generic-symbolic"); g_art_have.clear(); return; }
    if (url.rfind("file://", 0) == 0) {
        if (gchar* fn = g_filename_from_uri(url.c_str(), nullptr, nullptr)) { art_set_file(fn); g_free(fn); g_art_have = url; }
        return;
    }
    gchar* hash = g_compute_checksum_for_string(G_CHECKSUM_MD5, url.c_str(), -1);
    std::string path = music_cache_dir() + "/" + hash + ".img";
    g_free(hash);
    if (access(path.c_str(), R_OK) == 0) { art_set_file(path); g_art_have = url; return; }
    g_mkdir_with_parents(music_cache_dir().c_str(), 0755);
    auto* job = new ArtJob2{url, path, path + ".part2"};
    GSubprocess* sp = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr,
                                       "curl", "-sfL", "--max-time", "10", "-o", job->tmp.c_str(), url.c_str(), nullptr);
    if (!sp) { delete job; return; }
    g_subprocess_wait_check_async(sp, nullptr, [](GObject* src, GAsyncResult* res, gpointer d) {
        auto* j = static_cast<ArtJob2*>(d);
        if (g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, nullptr) && rename(j->tmp.c_str(), j->path.c_str()) == 0) {
            if (g_art && j->url == g_art_want) { art_set_file(j->path); g_art_have = j->url; }
        } else unlink(j->tmp.c_str());
        g_object_unref(src);
        delete j;
    }, job);
}

static void mp_ui() {
    if (!g_title) return;
    // вибір плеєра: той, що грає (поточний у пріоритеті) → Spotify → поточний → перший
    std::string best;
    if (playing(cur())) best = g_cur;
    if (best.empty()) for (auto& [b, p] : g_players) if (playing(&p)) { best = b; break; }
    if (best.empty() && g_players.count(g_cur)) best = g_cur;
    if (best.empty() && g_players.count("org.mpris.MediaPlayer2.spotify")) best = "org.mpris.MediaPlayer2.spotify";
    if (best.empty() && !g_players.empty()) best = g_players.begin()->first;
    g_cur = best;

    Player* p = cur();
    if (!p || p->title.empty()) {
        gtk_label_set_text(GTK_LABEL(g_title), "Тиша");
        gtk_label_set_text(GTK_LABEL(g_artist), p ? "Плеєр відкритий, але нічого не грає" : "Увімкни музику — плеєр з’явиться тут");
        gtk_label_set_text(GTK_LABEL(g_src), p ? player_name(p->bus).c_str() : "MPRIS");
        art_load("");
    } else {
        gtk_label_set_text(GTK_LABEL(g_title), p->title.c_str());
        gtk_label_set_text(GTK_LABEL(g_artist), p->artist.empty() ? "—" : p->artist.c_str());
        gtk_label_set_text(GTK_LABEL(g_src), (std::string("󰝚  ") + player_name(p->bus)).c_str());
        art_load(p->art);
    }
    set_btn_label(g_play_btn, playing(p) ? "\U000F03E4" : "\U000F040A");
}

static void mp_parse(Player& p, GVariant* dict) {
    GVariantIter it;
    const char* key;
    GVariant* val;
    g_variant_iter_init(&it, dict);
    auto as_i64 = [](GVariant* v) -> gint64 {
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) return g_variant_get_int64(v);
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64)) return (gint64)g_variant_get_uint64(v);
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32)) return g_variant_get_int32(v);
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) return g_variant_get_uint32(v);
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) return (gint64)g_variant_get_double(v);
        return 0;
    };
    while (g_variant_iter_next(&it, "{&sv}", &key, &val)) {
        if (!strcmp(key, "PlaybackStatus") && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) p.status = g_variant_get_string(val, nullptr);
        else if (!strcmp(key, "Rate") && g_variant_is_of_type(val, G_VARIANT_TYPE_DOUBLE)) p.rate = std::max(0.0, g_variant_get_double(val));
        else if (!strcmp(key, "CanSeek") && g_variant_is_of_type(val, G_VARIANT_TYPE_BOOLEAN)) p.can_seek = g_variant_get_boolean(val);
        else if (!strcmp(key, "Position")) { gint64 v = as_i64(val); if (v > 0) { p.pos = v; p.pos_at = now_us(); } }
        else if (!strcmp(key, "Metadata") && g_variant_is_of_type(val, G_VARIANT_TYPE_VARDICT)) {
            std::string old_title = p.title;
            p.title.clear(); p.artist.clear(); p.art.clear(); p.trackid.clear(); p.len = 0;
            GVariantDict d;
            g_variant_dict_init(&d, val);
            const char* s = nullptr;
            if (g_variant_dict_lookup(&d, "xesam:title", "&s", &s)) p.title = s;
            if (p.title != old_title) { p.pos = 0; p.pos_at = now_us(); }
            if (g_variant_dict_lookup(&d, "mpris:artUrl", "&s", &s)) p.art = s;
            if (GVariant* t = g_variant_dict_lookup_value(&d, "mpris:trackid", nullptr)) {
                if (g_variant_is_of_type(t, G_VARIANT_TYPE_OBJECT_PATH) || g_variant_is_of_type(t, G_VARIANT_TYPE_STRING))
                    p.trackid = g_variant_get_string(t, nullptr);
                g_variant_unref(t);
            }
            if (GVariant* l = g_variant_dict_lookup_value(&d, "mpris:length", nullptr)) { p.len = as_i64(l); g_variant_unref(l); }
            if (GVariant* a = g_variant_dict_lookup_value(&d, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY)) {
                gsize n = 0;
                const gchar** arr = g_variant_get_strv(a, &n);
                for (gsize i = 0; i < n; i++) { if (i) p.artist += ", "; p.artist += arr[i]; }
                g_free(arr);
                g_variant_unref(a);
            }
            g_variant_dict_clear(&d);
        }
        g_variant_unref(val);
    }
}

static void mp_query(const std::string& bus) {
    g_dbus_connection_call(sess(), bus.c_str(), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", "org.mpris.MediaPlayer2.Player"), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 1500, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer d) {
            std::string* bus = static_cast<std::string*>(d);
            GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
            if (r) {
                GVariant* dict = g_variant_get_child_value(r, 0);
                Player& p = g_players[*bus];
                p.bus = *bus;
                mp_parse(p, dict);
                g_variant_unref(dict);
                g_variant_unref(r);
                mp_ui();
                // Firefox/Chromium у GetAll віддають Position = 0 — питаємо окремо
                g_dbus_connection_call(sess(), bus->c_str(), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
                    g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Position"), G_VARIANT_TYPE("(v)"),
                    G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, [](GObject* src, GAsyncResult* res, gpointer d) {
                        std::string* b = static_cast<std::string*>(d);
                        if (GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr)) {
                            GVariant* v = nullptr;
                            g_variant_get(r, "(v)", &v);
                            auto it = g_players.find(*b);
                            if (v && it != g_players.end() && (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64) || g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64))) {
                                gint64 pos = g_variant_is_of_type(v, G_VARIANT_TYPE_INT64) ? g_variant_get_int64(v) : (gint64)g_variant_get_uint64(v);
                                if (pos > 0 || it->second.pos == 0) { it->second.pos = pos; it->second.pos_at = now_us(); }
                            }
                            if (v) g_variant_unref(v);
                            g_variant_unref(r);
                        }
                        delete b;
                    }, new std::string(*bus));
            }
            delete bus;
        }, new std::string(bus));
}

static void mp_refresh() {
    if (!sess()) return;
    g_dbus_connection_call(sess(), "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames",
        nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer) {
            GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
            if (!r) return;
            std::set<std::string> live;
            GVariantIter* it;
            const char* name;
            g_variant_get(r, "(as)", &it);
            while (g_variant_iter_next(it, "&s", &name))
                if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.") && strcmp(name, "org.mpris.MediaPlayer2.playerctld"))
                    live.insert(name);   // playerctld — лише проксі, дублює справжній плеєр
            g_variant_iter_free(it);
            g_variant_unref(r);
            for (auto i = g_players.begin(); i != g_players.end();) i = live.count(i->first) ? std::next(i) : g_players.erase(i);
            for (auto& b : live) mp_query(b);
            if (live.empty()) mp_ui();
        }, nullptr);
}

static void mp_call(const char* method, GVariant* args = nullptr) {
    Player* p = cur();
    if (!p || !sess()) { if (args) g_variant_unref(g_variant_ref_sink(args)); return; }
    g_dbus_connection_call(sess(), p->bus.c_str(), "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", method,
                           args, nullptr, G_DBUS_CALL_FLAGS_NONE, 1500, nullptr, nullptr, nullptr);
    g_timeout_add(180, [](gpointer) -> gboolean { mp_refresh(); return G_SOURCE_REMOVE; }, nullptr);
}
static void on_prev(GtkButton*, gpointer) { mp_call("Previous"); }
static void on_next(GtkButton*, gpointer) { mp_call("Next"); }
static void on_play(GtkButton*, gpointer) {
    if (Player* p = cur()) {   // миттєвий відгук, не чекаючи плеєра
        p->pos = shown_pos(p); p->pos_at = now_us();
        p->status = playing(p) ? "Paused" : "Playing";
        set_btn_label(g_play_btn, playing(p) ? "\U000F03E4" : "\U000F040A");
    }
    mp_call("PlayPause");
}
static void on_wave_click(GtkGestureClick*, int, double x, double, gpointer) {
    Player* p = cur();
    if (!p || p->len <= 0 || !p->can_seek) return;
    double f = std::clamp(x / std::max(1, gtk_widget_get_width(g_wave)), 0.0, 1.0);
    gint64 target = (gint64)(f * p->len);
    if (!p->trackid.empty()) mp_call("SetPosition", g_variant_new("(ox)", p->trackid.c_str(), target));
    else mp_call("Seek", g_variant_new("(x)", target - shown_pos(p)));
    p->pos = target; p->pos_at = now_us();
}

// «Хвилястий» прогрес Material 3 Expressive, але з квадратиків:
// зіграна частина — синусоїда (амплітуда спадає до нуля на паузі), далі «повзунок»-пігулка,
// проміжок і пунктирна доріжка з крапкою-стопом у кінці. Під ним — піксельний час.
static void draw_wave(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    Player* p = cur();
    const int cell = 2;
    int cols = w / cell, mid = 6;
    double f = p && p->len > 0 ? (double)shown_pos(p) / p->len : 0;
    g_amp += ((playing(p) ? 2.2 : 0.0) - g_amp) * 0.1;
    int head = (int)std::lround(f * (cols - 1));
    // Плеєр не дає тривалості (Firefox без Media Session position state, стріми):
    // «невизначений» прогрес M3 — хвиля на всю ширину з яскравою смугою, що біжить.
    if (p && p->len <= 0) {
        double run = std::fmod(g_t * 0.45, 1.4) - 0.2;
        for (int x = 0; x < cols; x++) {
            double fx = (double)x / cols;
            double y = mid + g_amp * std::sin(x * 0.42 - g_t * 5.5);
            double glow = std::exp(-std::pow((fx - run) / 0.12, 2));
            pw::RGB c = accent_px(fx, 0.45 + 0.4 * glow);
            cairo_set_source_rgba(cr, c.r, c.g, c.b, playing(p) ? 0.55 + 0.45 * glow : 0.35);
            cairo_rectangle(cr, x * cell, std::lround(y) * cell, cell, cell * 2);
            cairo_fill(cr);
        }
        pw::RGB dim{0.96, 0.96, 0.97};
        ptext(cr, playing(p) ? "НАЖИВО · БЕЗ ТАЙМІНГУ" : "ПАУЗА", 0, h - 15, 9, PANGO_WEIGHT_ULTRABOLD, dim.r, dim.g, dim.b, 0.5, 0, 1);
        return;
    }
    for (int x = 0; x < cols; x++) {
        if (x < head - 2) {
            double yy = mid + g_amp * std::sin(x * 0.42 - g_t * 5.5);
            int y = (int)std::lround(yy);
            pw::RGB c = accent_px((double)x / cols, 0.62 + 0.06 * std::sin(x * 0.2 - g_t * 3));
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
            cairo_rectangle(cr, x * cell, y * cell, cell, cell * 2);
            cairo_fill(cr);
        } else if (x >= head && x <= head + 1) {                   // повзунок
            pw::RGB c = accent_px(f, 0.86);
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
            cairo_rectangle(cr, x * cell, (mid - 3) * cell, cell, cell * 8);
            cairo_fill(cr);
        } else if (x > head + 4) {
            bool stop = x >= cols - 2;
            if (!stop && (x & 1)) continue;
            cairo_set_source_rgba(cr, 1, 1, 1, stop ? 0.55 : 0.2);
            cairo_rectangle(cr, x * cell, mid * cell, cell, cell * 2);
            cairo_fill(cr);
        }
    }
    pw::RGB dim{0.96, 0.96, 0.97};
    if (p && p->len > 0) {
        px_text(cr, mmss(shown_pos(p) / 1000000), 0, h - 14, 2, 0, false, &dim, 0.65, false);
        px_text(cr, mmss(p->len / 1000000), w, h - 14, 2, 1, false, &dim, 0.65, false);
    } else {
        px_text(cr, "-:--", 0, h - 14, 2, 0, false, &dim, 0.35, false);
    }
}

// ===================== СЬОГОДНІ: бари дня / тижня / року =====================
static void draw_bars(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    time_t tt = time(nullptr);
    struct tm m;
    localtime_r(&tt, &m);
    double sod = m.tm_hour * 3600 + m.tm_min * 60 + m.tm_sec;
    int ydays = days_in_month(m.tm_year + 1900, 1) == 29 ? 366 : 365;
    struct { const char* name; double f; int segs; } rows[] = {
        {"ДЕНЬ", sod / 86400.0, 24},
        {"ТИЖДЕНЬ", (((m.tm_wday + 6) % 7) * 86400.0 + sod) / (7 * 86400.0), 7},
        {"РІК", (m.tm_yday * 86400.0 + sod) / (ydays * 86400.0), 12},
    };
    const int cell = 3, brow = 3;
    const double lab_w = 70, pct_w = 40;
    double rh = h / 3.0;
    pw::RGB white{0.96, 0.96, 0.97};
    for (int i = 0; i < 3; i++) {
        double y0 = rh * i, cy = y0 + rh / 2;
        ptext(cr, rows[i].name, 0, cy - 7, 9.5, PANGO_WEIGHT_ULTRABOLD, 1, 1, 1, 0.6, 0, 1.2);
        int pct = (int)std::floor(rows[i].f * 100);
        px_text(cr, std::to_string(pct) + "%", w, cy - 7, 2, 1, false, &white, 0.9, false);
        int cols = (int)((w - lab_w - pct_w) / cell);
        int n = rows[i].segs, avail = cols - (n - 1);
        if (avail < n) continue;
        double bx = lab_w, by = cy - brow * cell / 2.0;
        double fill = rows[i].f * n;   // у «сегментах»
        int x = 0;
        for (int s = 0; s < n; s++) {
            int sw = (int)std::lround((double)avail * (s + 1) / n) - (int)std::lround((double)avail * s / n);
            for (int cx = 0; cx < sw; cx++) {
                double pos = s + (cx + 0.5) / sw;
                for (int ry = 0; ry < brow; ry++) {
                    double bay = BAYER[ry & 3][(x + cx) & 3] / 16.0;
                    if (pos <= fill) {
                        bool headcol = pos > fill - 1.0 / sw;
                        double l = 0.56 + (bay - 0.5) * 0.06 + (ry == 0 ? 0.08 : 0) +
                                   (headcol ? 0.18 + 0.1 * std::sin(g_t * 6) : 0);
                        pw::RGB c = accent_px((x + cx) / (double)cols, std::clamp(l, 0.0, 0.92));
                        cairo_set_source_rgb(cr, c.r, c.g, c.b);
                    } else {
                        cairo_set_source_rgba(cr, 1, 1, 1, 0.07 + (bay - 0.5) * 0.02);
                    }
                    cairo_rectangle(cr, bx + (x + cx) * cell, by + ry * cell, cell - 1, cell - 1);
                    cairo_fill(cr);
                }
            }
            x += sw + 1;
        }
    }
}

// ===================== ФОКУС-ТАЙМЕР =====================
static gint64 g_focus_end = 0;
static int g_focus_min = 0;
static guint g_focus_src = 0;
static void focus_labels() {
    if (!g_focus_btn) return;
    if (g_focus_end) {
        long long left = (g_focus_end - now_us()) / 1000000 + 1;
        set_btn_label(g_focus_btn, "\U000F04DB  СТОП · " + mmss(left));
        gtk_widget_set_visible(g_break_btn, FALSE);
    } else {
        set_btn_label(g_focus_btn, "\U000F13AB  ФОКУС 25");
        gtk_widget_set_visible(g_break_btn, TRUE);
    }
}
static void focus_stop() {
    g_focus_end = 0;
    if (g_focus_src) { g_source_remove(g_focus_src); g_focus_src = 0; }
    focus_labels();
    update_clock(nullptr);
}
static gboolean focus_tick(gpointer) {
    if (!g_focus_end) { g_focus_src = 0; return G_SOURCE_REMOVE; }
    if (now_us() >= g_focus_end) {
        bool brk = g_focus_min <= 5;
        const char* t = brk ? "Перерва завершена" : "Фокус завершено";
        const char* b = brk ? "Повертаймося до справ 💪" : "25 хвилин позаду — час на перерву ☕";
        GSubprocess* sp = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, nullptr, "notify-send", "-a", "mgOS", "-i", "alarm-symbolic", t, b, nullptr);
        if (sp) g_object_unref(sp);
        g_focus_src = 0;
        g_focus_end = 0;
        focus_labels();
        update_clock(nullptr);
        return G_SOURCE_REMOVE;
    }
    focus_labels();
    update_clock(nullptr);
    return G_SOURCE_CONTINUE;
}
static void focus_start(int minutes) {
    g_focus_min = minutes;
    g_focus_end = now_us() + (gint64)minutes * 60 * 1000000;
    if (!g_focus_src) g_focus_src = g_timeout_add_seconds(1, focus_tick, nullptr);
    focus_labels();
    update_clock(nullptr);
}
static void on_focus(GtkButton*, gpointer) { if (g_focus_end) focus_stop(); else focus_start(25); }
static void on_break(GtkButton*, gpointer) { focus_start(5); }

// ===================== РОЗУМНИЙ ГОДИННИК: ЖИВІ АКТИВНОСТІ =====================
// Джерела: фокус-таймер, завантаження Steam (appmanifest_*.acf), таймери у вікнах браузера (hyprctl clients),
// файли ~/.cache/mgos/activities/*.json {id, icon, text, progress 0..1|null, until_epoch|null, start_epoch?}.
// Жодних демонів: один таймер у процесі топбару — 1 с, коли є відлік, інакше 3 с (IO — не частіше за 3 с).
struct Act {
    std::string id, icon, text;
    double progress = -1;        // 0..1 або -1
    double until = 0;            // unix-секунди кінця (відлік) або 0
    double start = 0;            // unix-секунди старту (для прогресу відліку) або 0
    double frozen = -1;          // браузерний таймер на паузі: залишок, с
    int prio = 50;
};
static std::vector<Act> g_acts;          // зовнішні (без фокус-таймера)
static GtkWidget* g_acts_da = nullptr;
static guint g_act_src = 0;
static double g_act_io = 0;
static std::map<std::string, std::pair<long long, double>> g_btimer;   // адреса вікна → (останній залишок, коли змінився)

static double real_now() { return g_get_real_time() / 1e6; }
static std::string act_dir() { return std::string(g_get_user_cache_dir()) + "/mgos/activities"; }
static std::string cut(const std::string& s, int n) {
    if (g_utf8_strlen(s.c_str(), -1) <= n) return s;
    gchar* e = g_utf8_offset_to_pointer(s.c_str(), n - 1);
    return s.substr(0, e - s.c_str()) + "…";
}
static double act_left(const Act& a) { return a.frozen >= 0 ? a.frozen : std::max(0.0, a.until - real_now()); }
static double act_progress(const Act& a) {
    if (a.progress >= 0) return std::clamp(a.progress, 0.0, 1.0);
    if (a.until > 0 && a.start > 0 && a.until > a.start) return std::clamp(1 - act_left(a) / (a.until - a.start), 0.0, 1.0);
    return -1;
}
static std::string act_icon(const Act& a) { return !a.icon.empty() ? a.icon : (a.until > 0 || a.frozen >= 0) ? "\U000F051B" : "\U000F0E4B"; }
// short: для бульбашки («󰓓 Hades 42%», «󰔛 чай 04:59»); full — для картки «Активності»
static std::string act_value(const Act& a) {
    if (a.until > 0 || a.frozen >= 0) return mmss((long long)std::ceil(act_left(a)));
    if (a.progress >= 0) return std::to_string((int)std::lround(std::clamp(a.progress, 0.0, 1.0) * 100)) + "%";
    return "";
}
static std::string act_short(const Act& a) {
    std::string s = act_icon(a);
    if (!a.text.empty()) s += " " + cut(a.text, 14);
    std::string v = act_value(a);
    if (!v.empty()) s += " " + v;
    return s;
}
static std::vector<Act> acts_all() {
    std::vector<Act> v = g_acts;
    if (g_focus_end) {
        Act f;
        f.id = "focus"; f.prio = 90;
        f.icon = g_focus_min <= 5 ? "\U000F0176" : "\U000F051B";
        f.text = g_focus_min <= 5 ? "Перерва" : "Фокус";
        f.until = real_now() + std::max<double>(0, (g_focus_end - now_us()) / 1e6);
        f.start = f.until - g_focus_min * 60.0;
        v.push_back(f);
    }
    std::stable_sort(v.begin(), v.end(), [](const Act& a, const Act& b) { return a.prio > b.prio; });
    return v;
}

static std::string acf_get(const std::string& s, const std::string& key) {
    size_t p = s.find("\"" + key + "\"");
    if (p == std::string::npos) return "";
    p = s.find('"', p + key.size() + 2);
    if (p == std::string::npos) return "";
    size_t e = s.find('"', p + 1);
    return e == std::string::npos ? "" : s.substr(p + 1, e - p - 1);
}
static void acts_scan_steam(std::vector<Act>& out) {
    std::vector<std::string> roots = {sysui::home() + "/.local/share/Steam", sysui::home() + "/.steam/steam",
                                      sysui::home() + "/.var/app/com.valvesoftware.Steam/.local/share/Steam"};
    std::vector<std::string> libs;
    for (auto& r : roots) {
        std::string vdf;
        gchar* t = nullptr;
        if (g_file_get_contents((r + "/steamapps/libraryfolders.vdf").c_str(), &t, nullptr, nullptr)) {
            vdf = t; g_free(t);
            for (size_t p = 0; (p = vdf.find("\"path\"", p)) != std::string::npos; p += 6) {
                size_t a = vdf.find('"', p + 6), b = a == std::string::npos ? a : vdf.find('"', a + 1);
                if (b != std::string::npos) libs.push_back(vdf.substr(a + 1, b - a - 1) + "/steamapps");
            }
        }
        libs.push_back(r + "/steamapps");
    }
    std::sort(libs.begin(), libs.end());
    libs.erase(std::unique(libs.begin(), libs.end()), libs.end());
    for (auto& d : libs) {
        GDir* dir = g_dir_open(d.c_str(), 0, nullptr);
        if (!dir) continue;
        while (const gchar* n = g_dir_read_name(dir)) {
            std::string fn = n;
            if (fn.rfind("appmanifest_", 0) != 0 || fn.size() < 4 || fn.substr(fn.size() - 4) != ".acf") continue;
            std::string path = d + "/" + fn;
            GStatBuf st;
            if (g_stat(path.c_str(), &st) != 0 || real_now() - st.st_mtime > 180) continue;   // Steam не пише — не качає
            gchar* t = nullptr;
            if (!g_file_get_contents(path.c_str(), &t, nullptr, nullptr)) continue;
            std::string s = t; g_free(t);
            long flags = atol(acf_get(s, "StateFlags").c_str());
            double tot = atof(acf_get(s, "BytesToDownload").c_str()), got = atof(acf_get(s, "BytesDownloaded").c_str());
            if (!(flags & (256 | 1024 | 1048576)) || (flags & 512) || tot <= 0) continue;
            Act a;
            a.id = "steam:" + fn; a.icon = "\U000F04D3"; a.prio = 40;
            a.text = acf_get(s, "name");
            a.progress = std::clamp(got / tot, 0.0, 1.0);
            out.push_back(a);
        }
        g_dir_close(dir);
    }
}
static void acts_scan_files(std::vector<Act>& out) {
    std::string d = act_dir();
    GDir* dir = g_dir_open(d.c_str(), 0, nullptr);
    if (!dir) return;
    while (const gchar* n = g_dir_read_name(dir)) {
        std::string fn = n;
        if (fn.size() < 6 || fn.substr(fn.size() - 5) != ".json") continue;
        std::string path = d + "/" + fn;
        try {
            std::ifstream f(path);
            json j = json::parse(f);
            Act a;
            a.id = j.value("id", fn);
            a.icon = j.value("icon", std::string());
            a.text = j.value("text", std::string());
            if (j.contains("progress") && j["progress"].is_number()) a.progress = j["progress"].get<double>();
            if (j.contains("until_epoch") && j["until_epoch"].is_number()) a.until = j["until_epoch"].get<double>();
            if (j.contains("start_epoch") && j["start_epoch"].is_number()) a.start = j["start_epoch"].get<double>();
            if (j.contains("prio") && j["prio"].is_number()) a.prio = j["prio"].get<int>();
            if (a.until > 0 && a.until < real_now() - 2) { g_unlink(path.c_str()); continue; }   // відлік закінчився
            out.push_back(a);
        } catch (...) {}
    }
    g_dir_close(dir);
}
// Таймери в браузері: «таймер/timer» у заголовку вікна + mm:ss або hh:mm:ss → зворотний відлік
static void acts_scan_browser(std::vector<Act>& out) {
    static const std::regex re(R"((?:^|[^0-9:])(?:(\d{1,2}):)?(\d{1,2}):(\d{2})(?![0-9:]))");
    json cl;
    try { cl = json::parse(sysui::hypr_request("j/clients")); } catch (...) { return; }
    std::set<std::string> seen;
    double now = real_now();
    for (auto& c : cl) {
        std::string cls = c.value("class", std::string()), title = c.value("title", std::string());
        gchar* lc = g_utf8_strdown(cls.c_str(), -1);
        std::string lcls = lc; g_free(lc);
        bool br = false;
        for (const char* b : {"firefox", "chrom", "brave", "zen", "vivaldi", "librewolf", "opera", "edge", "floorp", "waterfox", "thorium", "helium"})
            if (lcls.find(b) != std::string::npos) br = true;
        if (!br) continue;
        gchar* lt = g_utf8_strdown(title.c_str(), -1);
        std::string ltitle = lt; g_free(lt);
        if (ltitle.find("таймер") == std::string::npos && ltitle.find("timer") == std::string::npos) continue;
        if (ltitle.find("stopwatch") != std::string::npos || ltitle.find("секундомір") != std::string::npos) continue;
        std::smatch m;
        if (!std::regex_search(title, m, re)) continue;
        long long secs = (m[1].matched ? atoll(m[1].str().c_str()) * 3600 : 0) + atoll(m[2].str().c_str()) * 60 + atoll(m[3].str().c_str());
        if (secs <= 0) continue;
        std::string addr = c.value("address", std::string("?"));
        seen.insert(addr);
        auto it = g_btimer.find(addr);
        if (it == g_btimer.end() || it->second.first != secs) g_btimer[addr] = {secs, now};
        double since = now - g_btimer[addr].second;
        Act a;
        a.id = "browser:" + addr; a.prio = 70; a.icon = "\U000F051B"; a.text = "Таймер";
        if (since > 4) a.frozen = (double)secs;          // заголовок не змінюється — таймер на паузі
        else { a.until = now + secs - since; a.start = 0; }
        out.push_back(a);
    }
    for (auto it = g_btimer.begin(); it != g_btimer.end();) it = seen.count(it->first) ? std::next(it) : g_btimer.erase(it);
}

static void acts_refresh_ui() {
    if (g_acts_da && g_open) gtk_widget_queue_draw(g_acts_da);
}
static gboolean acts_tick(gpointer);
static void acts_rearm(bool fast) {
    if (g_act_src) g_source_remove(g_act_src);
    g_act_src = g_timeout_add_seconds(fast ? 1 : 3, acts_tick, nullptr);
}
static bool acts_countdown() {
    if (g_focus_end) return true;
    for (auto& a : g_acts) if (a.until > 0) return true;
    return false;
}
static gboolean acts_tick(gpointer) {
    g_act_src = 0;
    double n = real_now();
    if (n - g_act_io >= 2.9 && !g_tick) {   // не скануємо файли/hyprctl під час морфа — без мікро-фризів
        g_act_io = n;
        std::vector<Act> v;
        acts_scan_files(v);
        acts_scan_steam(v);
        acts_scan_browser(v);
        g_acts = v;
    }
    update_clock(nullptr);
    acts_refresh_ui();
    acts_rearm(acts_countdown());
    return G_SOURCE_REMOVE;
}
// Запускається один раз із топбару; сам таймер живе лише в процесі топбару
static void acts_start() { if (!g_act_src) acts_rearm(false); }

// Що показати поруч із часом: активності по черзі (кожні 5 с), найважливіша першою
static std::string clock_suffix() {
    std::vector<Act> v = acts_all();
    if (v.empty()) return "";
    size_t i = v.size() > 1 ? ((long long)real_now() / 5) % v.size() : 0;
    return "  " + act_short(v[i]);
}

static void draw_acts(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    std::vector<Act> v = acts_all();
    if (v.empty()) {
        ptext(cr, "Немає активних завдань", w / 2.0, h / 2.0 - 16, 11.5, PANGO_WEIGHT_BOLD, 1, 1, 1, 0.5, 2);
        ptext(cr, "mgos-timer 5m \"чай\"", w / 2.0, h / 2.0 + 3, 10, PANGO_WEIGHT_NORMAL, 1, 1, 1, 0.32, 2);
        return;
    }
    const double RH = 38;
    int maxrows = std::max(1, (int)(h / RH));
    for (int i = 0; i < (int)v.size() && i < maxrows; i++) {
        const Act& a = v[i];
        double y = i * RH;
        ptext(cr, act_icon(a) + "  " + cut(a.text.empty() ? "Активність" : a.text, 24), 2, y + 2, 12, PANGO_WEIGHT_BOLD, 1, 1, 1, 0.92);
        std::string val = act_value(a);
        ptext(cr, val, w - 2, y + 2, 12, PANGO_WEIGHT_BOLD, 1, 1, 1, 0.7, 1);
        double pr = act_progress(a);
        double bx = 2, bw = w - 4, by = y + 24, bh = 6;
        auto rr = [&](double x, double ww) {
            double r = bh / 2;
            cairo_new_path(cr);
            cairo_arc(cr, x + ww - r, by + r, r, -M_PI / 2, M_PI / 2);
            cairo_arc(cr, x + r, by + r, r, M_PI / 2, 1.5 * M_PI);
            cairo_close_path(cr);
        };
        rr(bx, bw);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
        cairo_fill(cr);
        if (pr >= 0 && pr > 0.005) {
            pw::RGB c = accent_px(0.5, 0);
            rr(bx, std::max(bh, bw * pr));
            cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.95);
            cairo_fill(cr);
        }
    }
    if ((int)v.size() > maxrows)
        ptext(cr, "+" + std::to_string(v.size() - maxrows) + " ще", w - 2, h - 14, 10, PANGO_WEIGHT_NORMAL, 1, 1, 1, 0.5, 1);
}

// ===================== ФОН + МОРФ =====================
static int g_card_x = 0, g_card_y = 0;   // реальна (ціла) позиція картки — до неї «прибиваємо» текст бульбашки
static double morph_y(double h) { return h < g_from_h ? g_top + (g_from_h - h) * 0.5 : g_top; }

static void draw_bg_core(cairo_t* cr, int w, int h) {
    double v = std::clamp(g_v, 0.0, 1.0);
    auto lerp = [&](double a, double b) { return a + (b - a) * v; };
    double r = std::min({lerp(10, 24), w / 2.0, h / 2.0});
    auto path = [&](double in) {
        double rr = std::max(0.0, r - in);
        cairo_new_path(cr);
        cairo_arc(cr, w - in - rr, in + rr, rr, -M_PI / 2, 0);
        cairo_arc(cr, w - in - rr, h - in - rr, rr, 0, M_PI / 2);
        cairo_arc(cr, in + rr, h - in - rr, rr, M_PI / 2, M_PI);
        cairo_arc(cr, in + rr, in + rr, rr, M_PI, 1.5 * M_PI);
        cairo_close_path(cr);
    };
    cairo_save(cr);
    // запечений блюр під плашкою: росте разом із карткою (позиція — як у layout_morph; шар на весь монітор)
    sysui::bb::paint(cr, std::max(0.0, g_cx - w / 2.0), morph_y(h), w, h, r);
    path(0);
    cairo_set_source_rgba(cr, lerp(g_fbg[0], 0.078), lerp(g_fbg[1], 0.078), lerp(g_fbg[2], 0.094), lerp(g_fbg[3], 0.965));
    cairo_fill_preserve(cr);
    cairo_clip(cr);
    // хвилі шапки «проявляються» разом із розростанням
    g_wv.reveal = std::clamp((g_v - 0.2) / 0.8, 0.0, 1.0);
    g_wv.t = g_t;
    g_wv.style = sysui::settings().pixel_style;   // «Стиль піксельних шапок» у Налаштуваннях
    g_wv.draw(cr, w, std::min(h, 128));
    // Час годинника «їде» всередині картки: з місця в бульбашці до заголовка блоку (і назад при закритті)
    if (!g_from_text.empty()) {
        double p = g_drop_mode ? 0 : v * v * (3 - 2 * v);
        double a = std::clamp(1 - (v - 0.45) / 0.4, 0.0, 1.0);
        if (a > 0.001) {
            const double sc = 1.0;   // розмір тексту не міняємо під час «пружини» — дробовий кегль давав блимання/розмиття
            double size = g_fpx * sc + (g_drop_mode ? 0 : 14) * p;
            double x = (g_tx * sc + (w - g_from_w * sc) / 2) * (1 - p) + 26 * p;
            double cy = (g_ty + (h - g_from_h) / 2 + g_th / 2) * (1 - p) + 46 * p;
            double th = g_th * size / g_fpx;
            if (v <= 0.02 && !g_drop_mode) {   // біля бульбашки текст стоїть на екрані нерухомо, на цілих пікселях (картка тремтить ±1px)
                x = std::round(g_cx - g_from_w / 2 + g_tx) - g_card_x;
                cy = std::round(g_top + g_ty) - g_card_y + th / 2;
            }
            double cr_ = g_ffg[0] * (1 - p) + 0.96 * p, cg_ = g_ffg[1] * (1 - p) + 0.96 * p, cb_ = g_ffg[2] * (1 - p) + 0.97 * p;
            if (g_lay) {   // той самий шар, що й у топбарі, лише масштабуємо до заголовка
                cairo_save(cr);
                cairo_translate(cr, x, cy - th / 2);
                cairo_scale(cr, size / g_fpx, size / g_fpx);
                cairo_set_source_rgba(cr, cr_, cg_, cb_, a);
                pango_cairo_update_layout(cr, g_lay);
                pango_cairo_show_layout(cr, g_lay);
                cairo_restore(cr);
            } else {
                ptext(cr, g_from_text, x, cy - th / 2, size, PANGO_WEIGHT_BOLD, cr_, cg_, cb_, a);
            }
        }
    }
    cairo_restore(cr);
    path(0.5);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, lerp(g_fbd[0], 1), lerp(g_fbd[1], 1), lerp(g_fbd[2], 1), lerp(g_fbd[3], 0.09));
    cairo_stroke(cr);
}

static void draw_bg(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    draw_bg_core(cr, w, h);
}

static void layout_morph() {
    double dw = (g_W - g_from_w) * g_v, dh = (g_H - g_from_h) * g_v;
    if (g_v < 0) {
        double s = std::max(-0.3, g_v * (g_from_w > 0 ? (g_W - g_from_w) / g_from_w : 5.0));   // той самий нахил, що й при g_v>0 — рух без злому
        dw = g_from_w * s;
        dh = g_from_h * s;
    }
    double w = std::max(8.0, g_from_w + dw), h = std::max(8.0, g_from_h + dh);
    int iw = (int)std::lround(w), ih = (int)std::lround(h);
    g_card_x = (int)std::lround(std::max(0.0, g_cx - iw / 2.0)); g_card_y = (int)std::lround(morph_y(ih));
    gtk_widget_set_size_request(g_bg, iw, ih);
    gtk_fixed_move(GTK_FIXED(g_fixed), g_card, g_card_x, g_card_y);
}

static gboolean on_tick(GtkWidget*, GdkFrameClock* fc, gpointer) {
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    if (!g_t0) g_t0 = now;
    g_t = (now - g_t0) / 1e6;
    double k = std::clamp((now - g_anim_start) / 1000.0 / (g_open ? OPEN_MS : CLOSE_MS), 0.0, 1.0);
    g_v = g_open ? g_v_from + (1 - g_v_from) * ease_out_soft(k) : g_v_from * spring_close(k);
    layout_morph();

    if (g_open && k >= 0.92 && !g_content_on) {   // вміст проявляється вже наприкінці розростання
        g_content_on = true;
        g_content_a = 0;
        gtk_widget_set_opacity(g_content, 0);
        gtk_widget_set_visible(g_content, TRUE);
    }
    if (g_content_on && g_content_a < 1) {
        g_content_a = std::min(1.0, g_content_a + 0.1);
        gtk_widget_set_opacity(g_content, pw::smooth(g_content_a));
    }
    if (!g_open && gtk_widget_get_visible(g_content)) {   // при закритті вміст плавно зникає, поки картка стискається
        g_content_a = std::min(g_content_a - 0.25, std::max(0.0, 1.0 - k / 0.12));
        gtk_widget_set_opacity(g_content, pw::smooth(g_content_a));
        if (g_content_a <= 0) gtk_widget_set_visible(g_content, FALSE);
    }
    // форма кнопки Play: грає — квадрат (1), пауза — «коло» (4)
    double want = playing(cur()) ? 1.0 : 4.0;
    g_play_round += (want - g_play_round) * 0.15;

    gtk_widget_queue_draw(g_bg);
    if (g_content_on) {
        gtk_widget_queue_draw(g_header);
        gtk_widget_queue_draw(g_cal);
        gtk_widget_queue_draw(g_wave);
        gtk_widget_queue_draw(g_bars);
        for (GtkWidget* a : g_px_areas) if (gtk_widget_get_mapped(a)) gtk_widget_queue_draw(a);
    }
    if (!g_open && k >= 1) {
        gtk_widget_set_visible(g_content, FALSE);
        gtk_widget_set_visible(g_win, FALSE);
        sysui::bb::release();
        if (g_hidden_bubble) {
            gtk_widget_set_opacity(g_hidden_bubble, 1);   // без CSS-переходу (cp-still), інакше бульбашка «доплавує» 200 мс
            GtkWidget* b = g_hidden_bubble; g_hidden_bubble = nullptr;
            g_object_ref(b);
            g_timeout_add(250, [](gpointer d) -> gboolean {   // знімаємо cp-still, коли курсор уже не над бульбашкою
                GtkWidget* w = GTK_WIDGET(d);
                if (g_hidden_bubble == w) { g_object_unref(w); return G_SOURCE_REMOVE; }
                if (gtk_widget_get_state_flags(w) & GTK_STATE_FLAG_PRELIGHT) return G_SOURCE_CONTINUE;
                gtk_widget_remove_css_class(w, "cp-still"); g_object_unref(w); return G_SOURCE_REMOVE;
            }, b);
        }
        g_tick = 0;
        if (g_poll) { g_source_remove(g_poll); g_poll = 0; }
        sysui::trim();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}
static void ensure_tick() { if (!g_tick) g_tick = gtk_widget_add_tick_callback(g_win, on_tick, nullptr, nullptr); }

// Зчитати «стартову форму» бульбашки: позицію/розмір, кольори (тема), текст і де він малюється
static bool capture_bubble(GtkWidget* bubble, GtkWidget* bar) {
    graphene_rect_t r;
    if (!gtk_widget_compute_bounds(bubble, bar, &r)) return false;
    g_from_w = r.size.width; g_from_h = r.size.height;
    g_cx = r.origin.x + r.size.width / 2; g_top = r.origin.y;
    bool light = gtk_widget_has_css_class(bar, "light-theme"), mac = gtk_widget_has_css_class(bar, "macos");
    double op = sysui::settings().topbar_opacity / 100.0;
    if (light) { g_fbg[0] = g_fbg[1] = g_fbg[2] = 0.961; g_fbg[3] = std::min(1.0, op + 0.1); }
    else { g_fbg[0] = g_fbg[1] = 0.118; g_fbg[2] = 0.18; g_fbg[3] = op; }
    if (mac) g_fbg[3] = 0;
    bool brd = sysui::settings().topbar_border && !mac;
    g_fbd[0] = g_fbd[1] = g_fbd[2] = light ? 0 : 1;
    g_fbd[3] = brd ? (light ? 0.1 : 0.1) : 0;
    if (clock_label) {
        GdkRGBA c;
        gtk_widget_get_color(clock_label, &c);
        g_ffg[0] = c.red; g_ffg[1] = c.green; g_ffg[2] = c.blue;
        g_from_text = gtk_label_get_text(GTK_LABEL(clock_label));
        PangoLayout* l = gtk_widget_create_pango_layout(clock_label, g_from_text.c_str());
        int tw, th;
        pango_layout_get_pixel_size(l, &tw, &th);
        if (g_lay) { g_object_unref(g_lay); g_lay = nullptr; }
        if (PangoLayout* rl = gtk_label_get_layout(GTK_LABEL(clock_label))) {   // справжній шар мітки: той самий шрифт, вага, інтервали
            g_lay = pango_layout_copy(rl);
            pango_layout_get_pixel_size(rl, &tw, &th);
        }
        g_object_unref(l);
        graphene_rect_t lr;
        if (gtk_widget_compute_bounds(clock_label, bubble, &lr)) {
            // мітка в позиції bounds сидить без відступів CSS бульбашки — а на екрані текст по центру плашки
            g_tx = (g_from_w - tw) / 2.0;
            g_ty = (g_from_h - th) / 2.0;
            g_th = th;
        }
    }
    return true;
}

static void hide_pop() {
    if (!g_win || !g_open) return;
    g_open = false;
    g_v_from = std::clamp(g_v, 0.0, 1.05);
    g_anim_start = now_us();
    g_content_on = false;
    // бульбашка могла змінити розмір (текст активності) — перечитуємо її місце, зберігаючи поточну ширину картки
    double cur_w = g_from_w + (g_W - g_from_w) * g_v;
    if (g_last_bubble && capture_bubble(g_last_bubble, g_last_bar) && g_W > g_from_w) {
        g_v_from = std::clamp((cur_w - g_from_w) / (g_W - g_from_w), 0.0, 1.05);
        g_v = g_v_from;
    }
    ensure_tick();
}

// ===================== ПОБУДОВА =====================
static GtkWidget* card_box(const char* title_txt, GtkWidget** title_out = nullptr) {
    GtkWidget* c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(c, "cp-card");
    if (title_txt) {
        GtkWidget* l = gtk_label_new(title_txt);
        gtk_widget_add_css_class(l, "cp-title");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_append(GTK_BOX(c), l);
        if (title_out) *title_out = l;
    }
    return c;
}

static std::string qs_tool() { return sysui::home() + "/.local/bin/mgos-quickshare"; }

static std::string qs_status() {
    std::string out;
    gchar* o = nullptr;
    gchar* argv[] = {(gchar*)qs_tool().c_str(), (gchar*)"status", nullptr};
    std::string tool = qs_tool();
    argv[0] = (gchar*)tool.c_str();
    if (g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_STDERR_TO_DEV_NULL, nullptr, nullptr, &o, nullptr, nullptr, nullptr) && o)
        out = o;
    g_free(o);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out.empty() ? "missing" : out;
}

static std::string human_size(goffset b) {
    gchar* s = g_format_size(b);
    std::string r = s; g_free(s);
    return r;
}

static void drop_ui(const std::string& glyph, const std::string& title, const std::string& sub,
                    const std::string& status, bool sending) {
    gtk_label_set_text(GTK_LABEL(g_drop_glyph), glyph.c_str());
    gtk_label_set_text(GTK_LABEL(g_drop_title), title.c_str());
    gtk_label_set_text(GTK_LABEL(g_drop_sub), sub.c_str());
    gtk_label_set_text(GTK_LABEL(g_drop_status), status.c_str());
    gtk_widget_set_visible(g_drop_status, !status.empty());
    gtk_widget_set_visible(g_drop_pin, FALSE);
    gtk_widget_set_visible(g_drop_qr, FALSE);
    gtk_widget_set_visible(g_drop_glyph, TRUE);
    gtk_widget_remove_css_class(g_drop_zone, "qr");
    std::string st = qs_status();
    gtk_widget_set_visible(g_qs_install, FALSE);
    gtk_widget_set_visible(g_qs_open, FALSE);
    gtk_widget_set_visible(g_sh_allow, FALSE);
    gtk_widget_set_visible(g_qs_send, st == "installed" && !sending && !g_drop_files.empty());
    gtk_widget_set_visible(g_qs_cancel, sending);
}

static void drop_reset() {
    g_drop_files.clear();
    std::string st = qs_status();
    (void)st;
    drop_ui("\U000F0552", "КИНЬТЕ ФАЙЛ СЮДИ", "QR-код для будь-кого або іконка свого пристрою нижче", "", false);
    gtk_widget_remove_css_class(g_drop_zone, "hot");
}

static void qs_read_line(GObject* src, GAsyncResult* res, gpointer);

static void qs_finish(const std::string& result) {
    if (g_qs_proc) { g_object_unref(g_qs_proc); g_qs_proc = nullptr; }
    gtk_widget_remove_css_class(g_drop_zone, "busy");
    if (result == "Finished") {
        drop_ui("\U000F012C", "НАДІСЛАНО", g_drop_files.size() == 1 ? fs_basename(g_drop_files[0]) : std::to_string(g_drop_files.size()) + " файлів",
                "", false);
        gtk_widget_add_css_class(g_drop_zone, "done");
        g_drop_files.clear();
        g_timeout_add(2600, [](gpointer) -> gboolean { if (g_open && g_drop_mode) hide_pop(); return G_SOURCE_REMOVE; }, nullptr);
        return;
    }
    std::string why =
        result == "NoReceiver"   ? "Телефон не знайдено. Відкрийте на ньому Quick Share, видимість «Усі», Bluetooth і Wi-Fi увімкнені" :
        result == "Rejected"     ? "Телефон відхилив передачу" :
        result == "Cancelled"    ? "Скасовано" :
        result == "Disconnected" ? "Зʼєднання обірвалось — спробуйте ще раз ближче до телефона" :
        result == "Timeout"      ? "Телефон не відповів вчасно" :
        result == "gui-only"     ? "Відкрито вікно Quick Share — виберіть файл там" :
        result == "notinstalled" ? "Quick Share ще не встановлено" :
                                   "Не вдалося надіслати (" + result + ")";
    drop_ui("\U000F0026", "НЕ ВИЙШЛО", "", why, false);
    if (!g_drop_files.empty()) set_btn_label(g_qs_send, "\U000F0450  ЩЕ РАЗ");
}

static void qs_handle(const std::string& l) {
    auto field = [&](int i) {
        size_t a = 0;
        for (int k = 0; k < i; k++) { a = l.find('|', a); if (a == std::string::npos) return std::string(); a++; }
        size_t b = l.find('|', a);
        return l.substr(a, b == std::string::npos ? std::string::npos : b - a);
    };
    if (l.rfind("STATUS|", 0) == 0) {
        gtk_label_set_text(GTK_LABEL(g_drop_status), field(1).c_str());
        gtk_widget_set_visible(g_drop_status, TRUE);
    } else if (l.rfind("PIN|", 0) == 0) {
        gtk_label_set_text(GTK_LABEL(g_drop_pin), ("PIN  " + field(1)).c_str());
        gtk_widget_set_visible(g_drop_pin, TRUE);
        gtk_label_set_text(GTK_LABEL(g_drop_status), "Підтвердіть на телефоні — PIN має збігатися");
        gtk_label_set_text(GTK_LABEL(g_drop_title), "НАДСИЛАЮ…");
    } else if (l.rfind("RESULT|", 0) == 0) {
        std::string r = field(2);
        if (r != "Finished") { g_object_set_data_full(G_OBJECT(g_drop_zone), "qs-fail", g_strdup(r.c_str()), g_free); }
    } else if (l.rfind("ERROR|", 0) == 0) {
        g_object_set_data_full(G_OBJECT(g_drop_zone), "qs-fail", g_strdup(field(1).c_str()), g_free);
    }
}

static void qs_read_line(GObject* src, GAsyncResult* res, gpointer) {
    gsize len = 0;
    gchar* line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, &len, nullptr);
    if (line) {
        if (g_drop_content) qs_handle(line);
        g_free(line);
        g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(src), G_PRIORITY_DEFAULT, nullptr, qs_read_line, nullptr);
        return;
    }
    g_object_unref(src);
    // кінець виводу — чекаємо завершення процесу
    if (!g_qs_proc) return;
    g_subprocess_wait_async(g_qs_proc, nullptr, [](GObject* p, GAsyncResult* r, gpointer) {
        g_subprocess_wait_finish(G_SUBPROCESS(p), r, nullptr);
        if ((GObject*)g_qs_proc != p) return;   // уже скасовано
        const char* f = (const char*)g_object_get_data(G_OBJECT(g_drop_zone), "qs-fail");
        std::string res = f ? f : (g_subprocess_get_successful(G_SUBPROCESS(p)) ? "Finished" : "Failed");
        g_object_set_data(G_OBJECT(g_drop_zone), "qs-fail", nullptr);
        qs_finish(res);
    }, nullptr);
}

static void sh_stop();
static void qs_send(GtkButton* = nullptr, gpointer = nullptr) {
    if (g_drop_files.empty() || g_qs_proc) return;
    sh_stop();
    std::vector<const char*> argv = {nullptr, "send"};
    std::string tool = qs_tool();
    argv[0] = tool.c_str();
    for (auto& f : g_drop_files) argv.push_back(f.c_str());
    argv.push_back(nullptr);
    GError* err = nullptr;
    g_qs_proc = g_subprocess_newv(argv.data(), (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), &err);
    if (!g_qs_proc) { qs_finish(err ? err->message : "spawn"); if (err) g_error_free(err); return; }
    g_object_set_data(G_OBJECT(g_drop_zone), "qs-fail", nullptr);
    std::string what = g_drop_files.size() == 1 ? fs_basename(g_drop_files[0]) : std::to_string(g_drop_files.size()) + " файлів";
    drop_ui("\U000F0EB3", "QUICK SHARE", what, "Готую…", true);
    set_btn_label(g_qs_send, "\U000F048A  НАДІСЛАТИ");
    gtk_widget_add_css_class(g_drop_zone, "busy");
    GDataInputStream* in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(g_qs_proc));
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, nullptr, qs_read_line, nullptr);
}

static void qs_cancel(GtkButton*, gpointer) {
    if (g_dv_proc) {
        bool pairing = g_dv_mode == "pair";
        dv_stop();
        if (pairing && !g_drop_files.empty()) sh_start();
        else if (pairing) drop_reset();
        else { g_drop_files.clear(); hide_pop(); }
        return;
    }
    if (g_sh_proc) { sh_stop(); g_drop_files.clear(); hide_pop(); return; }
    if (!g_qs_proc) return;
    g_subprocess_force_exit(g_qs_proc);
    g_object_unref(g_qs_proc); g_qs_proc = nullptr;
    qs_finish("Cancelled");
}

static void sh_start();
static void show_pop(GtkWidget* bubble, GtkWidget* bar, bool drop);

// ===================== QR-ПЕРЕДАЧА =====================
static std::string sh_tool() { return sysui::home() + "/.local/bin/mgos-share"; }
static std::string sh_sub() {
    const char* s = g_drop_zone ? (const char*)g_object_get_data(G_OBJECT(g_drop_zone), "sh-sub") : nullptr;
    return s ? s : "";
}

static void sh_stop() {
    if (!g_sh_proc) return;
    g_subprocess_send_signal(g_sh_proc, SIGTERM);
    g_object_unref(g_sh_proc); g_sh_proc = nullptr;
}

static void sh_show_qr(const std::string& png) {
    gtk_picture_set_filename(GTK_PICTURE(g_drop_qr), png.c_str());
    gtk_widget_set_visible(g_drop_glyph, FALSE);
    gtk_widget_set_visible(g_drop_qr, TRUE);
    gtk_widget_add_css_class(g_drop_zone, "qr");
}

static void sh_buttons(bool serving) {
    set_btn_label(g_qs_cancel, "\U000F0156  ЗАКРИТИ");
    gtk_widget_set_visible(g_qs_cancel, serving);
    set_btn_label(g_qs_send, "\U000F0EB3  QUICK SHARE");
    gtk_widget_set_visible(g_qs_send, serving && qs_status() == "installed");
}

static void sh_handle(const std::string& l) {
    std::string val = l.find('|') == std::string::npos ? "" : l.substr(l.find('|') + 1);
    if (l.rfind("QR|", 0) == 0) {
        drop_ui("", "СКАНУЙТЕ КАМЕРОЮ", sh_sub(), "Телефон має бути в тій самій Wi-Fi", false);
        sh_show_qr(val);
        sh_buttons(true);
    } else if (l.rfind("OPEN", 0) == 0) {
        g_sh_opened = true;
        gtk_label_set_text(GTK_LABEL(g_drop_status), "Телефон відкрив сторінку — натисніть «Завантажити»");
    } else if (l.rfind("GOT|", 0) == 0) {
        gtk_label_set_text(GTK_LABEL(g_drop_status), ("Отримано: " + val).c_str());
    } else if (l.rfind("DONE", 0) == 0) {
        std::string sub = sh_sub();
        g_drop_files.clear();
        drop_ui("\U000F012C", "НА ТЕЛЕФОНІ", sub, "", false);
        gtk_widget_add_css_class(g_drop_zone, "done");
        g_timeout_add(2600, [](gpointer) -> gboolean { if (g_open && g_drop_mode) hide_pop(); return G_SOURCE_REMOVE; }, nullptr);
    } else if (l.rfind("ERROR|", 0) == 0) {
        drop_ui("\U000F0026", "НЕ ВИЙШЛО", sh_sub(),
                val == "nonet" ? "Немає Wi-Fi — підключіться до тієї ж мережі, що й телефон" : "Не вдалося підняти передачу (" + val + ")", false);
        sh_buttons(false);
        gtk_widget_set_visible(g_qs_send, !g_drop_files.empty() && qs_status() == "installed");
    }
}

static void sh_read_line(GObject* src, GAsyncResult* res, gpointer proc) {
    gsize len = 0;
    gchar* line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, &len, nullptr);
    if (line) {
        if (proc == g_sh_proc && g_drop_content) sh_handle(line);
        g_free(line);
        g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(src), G_PRIORITY_DEFAULT, nullptr, sh_read_line, proc);
        return;
    }
    g_object_unref(src);
    if (proc == g_sh_proc) { g_object_unref(g_sh_proc); g_sh_proc = nullptr; }
    g_object_unref(G_OBJECT(proc));
}

static void sh_start() {
    sh_stop(); dv_stop();
    g_sh_opened = false;
    if (g_drop_files.empty()) return;
    gtk_widget_remove_css_class(g_drop_zone, "done");
    std::string chk;
    {
        gchar* o = nullptr;
        std::string c = sh_tool() + " check";
        if (g_spawn_command_line_sync(c.c_str(), &o, nullptr, nullptr, nullptr) && o) chk = g_strstrip(o);
        g_free(o);
    }
    if (chk == "nonet") { sh_handle("ERROR|nonet"); return; }
    if (chk == "firewall") {
        drop_ui("\U000F033E", "ОДИН КРОК", sh_sub(), "Дозвольте телефону підключатися до цього ПК по Wi-Fi (пароль — лише один раз)", false);
        gtk_widget_set_visible(g_sh_allow, TRUE);
        return;
    }
    GSubprocessLauncher* l = g_subprocess_launcher_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    g_subprocess_launcher_setenv(l, "MGOS_ACCENT", sysui::accent().c_str(), TRUE);
    std::vector<const char*> argv;
    std::string tool = sh_tool();
    argv.push_back(tool.c_str()); argv.push_back("serve");
    for (auto& f : g_drop_files) argv.push_back(f.c_str());
    argv.push_back(nullptr);
    GError* err = nullptr;
    g_sh_proc = g_subprocess_launcher_spawnv(l, argv.data(), &err);
    g_object_unref(l);
    if (!g_sh_proc) { sh_handle(std::string("ERROR|") + (err ? err->message : "spawn")); if (err) g_error_free(err); return; }
    drop_ui("\U000F0432", "ГОТУЮ QR…", sh_sub(), "", false);
    GDataInputStream* in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(g_sh_proc));
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, nullptr, sh_read_line, g_object_ref(g_sh_proc));
}

static void sh_allow(GtkButton*, gpointer) {
    std::string tool = sh_tool();
    const char* argv[] = {"kitty", "--class", "mgos-recovery", "-e", tool.c_str(), "setup", nullptr};
    GPid pid;
    if (!g_spawn_async(nullptr, (gchar**)argv, nullptr, (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD), nullptr, nullptr, &pid, nullptr)) return;
    gtk_label_set_text(GTK_LABEL(g_drop_status), "Введіть пароль у вікні терміналу…");
    gtk_widget_set_visible(g_sh_allow, FALSE);
    g_child_watch_add(pid, [](GPid p, gint, gpointer) {
        g_spawn_close_pid(p);
        if (g_drop_files.empty()) { if (g_open && g_drop_mode) dv_pair(); return; }
        if (!g_open && g_last_bubble && g_last_bar) show_pop(g_last_bubble, g_last_bar, true);
        sh_start();
    }, nullptr);
}


// ===================== Mg Share: свої пристрої =====================
// Іконки з'єднаних пристроїв під зоною дропу. Стан пише служба mgos-share daemon у
// ~/.cache/mgos/share-devices.tsv (id, ім'я, тип, час останнього «привіт»); «онлайн» = привіт < 45 с.
struct MgDev { std::string id, name, kind; gint64 seen = 0; };
static GtkWidget* g_dv_row = nullptr;
static GFileMonitor* g_dv_mon = nullptr;
static guint g_dv_tick = 0;
static std::string g_dv_sig;

static std::string dv_tsv() { return std::string(g_get_user_cache_dir()) + "/mgos/share-devices.tsv"; }
static bool dv_online(const MgDev& d) { return d.seen > 0 && time(nullptr) - d.seen < 45; }
static std::vector<MgDev> dv_load() {
    std::vector<MgDev> v;
    gchar* txt = nullptr;
    if (!g_file_get_contents(dv_tsv().c_str(), &txt, nullptr, nullptr)) return v;
    std::istringstream ss(txt);
    g_free(txt);
    for (std::string ln; std::getline(ss, ln);) {
        std::vector<std::string> f;
        size_t a = 0, b;
        while ((b = ln.find('\t', a)) != std::string::npos) { f.push_back(ln.substr(a, b - a)); a = b + 1; }
        f.push_back(ln.substr(a));
        if (f.size() < 4 || f[0].empty()) continue;
        v.push_back({f[0], f[1], f[2], g_ascii_strtoll(f[3].c_str(), nullptr, 10)});
    }
    return v;
}
static std::string dv_upper(const std::string& s) { gchar* u = g_utf8_strup(s.c_str(), -1); std::string r = u; g_free(u); return r; }

static void dv_stop() {
    if (!g_dv_proc) return;
    g_subprocess_send_signal(g_dv_proc, SIGTERM);
    g_object_unref(g_dv_proc); g_dv_proc = nullptr;
    g_dv_mode.clear();
}

static void dv_rebuild();
static void dv_push(const std::string& id, const std::string& name);

static GtkWidget* dv_chip(const std::string& glyph, const std::string& name, bool online, bool add) {
    GtkWidget* b = gtk_button_new();
    gtk_widget_add_css_class(b, "cp-dv");
    if (online) gtk_widget_add_css_class(b, "online");
    if (add) gtk_widget_add_css_class(b, "add");
    gtk_widget_set_cursor_from_name(b, "pointer");
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* g = gtk_label_new(glyph.c_str());
    gtk_widget_add_css_class(g, "cp-dv-glyph");
    GtkWidget* n = gtk_label_new(name.c_str());
    gtk_widget_add_css_class(n, "cp-dv-name");
    gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(n), 9);
    gtk_box_append(GTK_BOX(box), g);
    gtk_box_append(GTK_BOX(box), n);
    gtk_button_set_child(GTK_BUTTON(b), box);
    return b;
}

static void dv_rebuild() {
    if (!g_dv_row) return;
    while (GtkWidget* c = gtk_widget_get_first_child(g_dv_row)) gtk_box_remove(GTK_BOX(g_dv_row), c);
    auto devs = dv_load();
    g_dv_sig.clear();
    for (auto& d : devs) {
        bool on = dv_online(d);
        g_dv_sig += d.id + (on ? "1" : "0");
        GtkWidget* b = dv_chip(d.kind == "laptop" ? "\U000F0322" : "\U000F011C", d.name, on, false);
        gtk_widget_set_tooltip_text(b, on ? "Відкрито Mg Share — надішле одразу" : "Надішле, щойно відкриєте Mg Share на пристрої");
        g_object_set_data_full(G_OBJECT(b), "id", g_strdup(d.id.c_str()), g_free);
        g_object_set_data_full(G_OBJECT(b), "name", g_strdup(d.name.c_str()), g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
            std::string id = (const char*)g_object_get_data(G_OBJECT(b), "id");
            std::string nm = (const char*)g_object_get_data(G_OBJECT(b), "name");
            if (g_drop_files.empty()) {
                gtk_label_set_text(GTK_LABEL(g_drop_status), ("Киньте файл сюди — і натисніть на " + nm).c_str());
                gtk_widget_set_visible(g_drop_status, TRUE);
                return;
            }
            dv_push(id, nm);
        }), nullptr);
        gtk_box_append(GTK_BOX(g_dv_row), b);
    }
    GtkWidget* add = dv_chip("\U000F0415", devs.empty() ? "Додати телефон" : "Додати", false, true);
    gtk_label_set_max_width_chars(GTK_LABEL(gtk_widget_get_last_child(gtk_button_get_child(GTK_BUTTON(add)))), 14);
    gtk_widget_set_tooltip_text(add, "З'єднати новий пристрій через застосунок Mg Share");
    g_signal_connect(add, "clicked", G_CALLBACK(dv_pair), nullptr);
    gtk_box_append(GTK_BOX(g_dv_row), add);
    // «онлайн» старіє без подій — перевіряємо, поки блок дропу відкритий
    if (!g_dv_tick)
        g_dv_tick = g_timeout_add_seconds(5, [](gpointer) -> gboolean {
            if (!g_open || !g_drop_mode) { g_dv_tick = 0; return G_SOURCE_REMOVE; }
            std::string sig;
            for (auto& d : dv_load()) sig += d.id + (dv_online(d) ? "1" : "0");
            if (sig != g_dv_sig) dv_rebuild();
            return G_SOURCE_CONTINUE;
        }, nullptr);
}

static void dv_finish_hide() {
    g_timeout_add(2600, [](gpointer) -> gboolean { if (g_open && g_drop_mode && !g_dv_proc) hide_pop(); return G_SOURCE_REMOVE; }, nullptr);
}

static void dv_handle(const std::string& l) {
    std::string val = l.find('|') == std::string::npos ? "" : l.substr(l.find('|') + 1);
    auto status = [](const std::string& t) {
        gtk_label_set_text(GTK_LABEL(g_drop_status), t.c_str());
        gtk_widget_set_visible(g_drop_status, !t.empty());
    };
    if (g_dv_mode == "pair") {
        if (l.rfind("QR|", 0) == 0) {
            drop_ui("", "ДОДАТИ ПРИСТРІЙ", "Mg Share на телефоні → «Додати пристрій»", "Наведіть телефон на цей QR — один раз, і все", true);
            set_btn_label(g_qs_cancel, "\U000F0156  СКАСУВАТИ");
            sh_show_qr(val);
        } else if (l.rfind("PAIRED|", 0) == 0) {
            std::string nm = val.substr(val.find('|') + 1);
            g_dv_mode.clear();
            drop_ui("\U000F012C", "З'ЄДНАНО", nm, g_drop_files.empty() ? "Тепер кидайте файли на його іконку" : "Натисніть на його іконку, щоб надіслати", false);
            gtk_widget_add_css_class(g_drop_zone, "done");
            dv_rebuild();
        } else if (l.rfind("EXPIRED", 0) == 0) {
            g_dv_mode.clear();
            drop_ui("\U000F0026", "ЧАС ВИЙШОВ", "", "QR діє 5 хвилин — натисніть «Додати» ще раз", false);
        } else if (l.rfind("ERROR|", 0) == 0) {
            g_dv_mode.clear();
            drop_ui("\U000F0026", "НЕ ВИЙШЛО", "", val == "nonet" ? "Немає Wi-Fi — підключіться до тієї ж мережі, що й телефон" : "Служба Mg Share не відповідає (" + val + ")", false);
        }
        return;
    }
    // push
    std::string who = g_dv_target;
    if (l.rfind("WAIT", 0) == 0) {
        gtk_label_set_text(GTK_LABEL(g_drop_glyph), "\U000F011C");
        status("Відкрийте Mg Share на «" + who + "» (плитка в шторці) — файл прилетить сам");
    } else if (l.rfind("FOUND", 0) == 0) {
        status("«" + who + "» на зв'язку…");
    } else if (l.rfind("SENDING|", 0) == 0) {
        g_object_set_data_full(G_OBJECT(g_drop_zone), "dv-cur", g_strdup(val.c_str()), g_free);
        status("Надсилаю " + val + "…");
    } else if (l.rfind("PROG|", 0) == 0) {
        const char* cur = (const char*)g_object_get_data(G_OBJECT(g_drop_zone), "dv-cur");
        status(std::string("Надсилаю ") + (cur ? cur : "") + " · " + val + "%");
    } else if (l.rfind("DONE", 0) == 0) {
        std::string sub = sh_sub();
        g_drop_files.clear();
        g_dv_mode.clear();
        if (!g_open) {
            std::string c = "notify-send -a 'Mg Share' -i emblem-ok-symbolic " + std::string(g_shell_quote(("Надіслано на " + who).c_str())) +
                            " " + g_shell_quote(sub.c_str());
            g_spawn_command_line_async(c.c_str(), nullptr);
        }
        drop_ui("\U000F012C", "НА «" + dv_upper(who) + "»", sub, "", false);
        gtk_widget_add_css_class(g_drop_zone, "done");
        dv_finish_hide();
    } else if (l.rfind("ERROR|", 0) == 0) {
        g_dv_mode.clear();
        drop_ui("\U000F0026", "НЕ ВИЙШЛО", sh_sub(),
                val == "timeout" ? "«" + who + "» так і не відкрили за 15 хвилин" :
                val == "auth"    ? "«" + who + "» більше не довіряє цьому ПК — з'єднайте заново" :
                val == "unknown" ? "Такого пристрою вже немає" :
                                   "Не вдалося надіслати (" + val + ")", false);
    }
}

static void dv_read_line(GObject* src, GAsyncResult* res, gpointer proc) {
    gsize len = 0;
    gchar* line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, &len, nullptr);
    if (line) {
        if (proc == g_dv_proc && g_drop_content) dv_handle(line);
        g_free(line);
        g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(src), G_PRIORITY_DEFAULT, nullptr, dv_read_line, proc);
        return;
    }
    g_object_unref(src);
    if (proc == g_dv_proc) {
        g_object_unref(g_dv_proc); g_dv_proc = nullptr;
        if (!g_dv_mode.empty()) { std::string m = g_dv_mode; g_dv_mode.clear(); if (m == "push") dv_handle("ERROR|daemon"); }
    }
    g_object_unref(G_OBJECT(proc));
}

static bool dv_spawn(const std::vector<std::string>& args, const std::string& mode) {
    std::vector<const char*> argv;
    std::string tool = sh_tool();
    argv.push_back(tool.c_str());
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* err = nullptr;
    g_dv_proc = g_subprocess_newv(argv.data(), (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), &err);
    if (!g_dv_proc) { if (err) g_error_free(err); return false; }
    g_dv_mode = mode;
    GDataInputStream* in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(g_dv_proc));
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, nullptr, dv_read_line, g_object_ref(g_dv_proc));
    return true;
}

static std::string sh_check() {
    std::string chk;
    gchar* o = nullptr;
    std::string c = sh_tool() + " check";
    if (g_spawn_command_line_sync(c.c_str(), &o, nullptr, nullptr, nullptr) && o) chk = g_strstrip(o);
    g_free(o);
    return chk;
}

static void dv_push(const std::string& id, const std::string& name) {
    sh_stop(); dv_stop();
    g_dv_target = name;
    std::vector<std::string> args = {"push", id};
    for (auto& f : g_drop_files) args.push_back(f);
    gtk_widget_remove_css_class(g_drop_zone, "done");
    if (!dv_spawn(args, "push")) { drop_ui("\U000F0026", "НЕ ВИЙШЛО", sh_sub(), "Не вдалося запустити mgos-share", false); return; }
    drop_ui("\U000F011C", "НА «" + dv_upper(name) + "»", sh_sub(), "Готую…", true);
    set_btn_label(g_qs_cancel, "\U000F0156  СКАСУВАТИ");
}

static void dv_pair(GtkButton*, gpointer) {
    sh_stop(); dv_stop();
    gtk_widget_remove_css_class(g_drop_zone, "done");
    std::string chk = sh_check();
    if (chk == "nonet") { drop_ui("\U000F0026", "НЕМАЄ WI-FI", "", "Підключіться до тієї ж мережі, що й телефон", false); return; }
    if (chk == "firewall") {
        drop_ui("\U000F033E", "ОДИН КРОК", "", "Дозвольте телефону підключатися до цього ПК по Wi-Fi (пароль — лише один раз)", false);
        gtk_widget_set_visible(g_sh_allow, TRUE);
        return;
    }
    if (!dv_spawn({"pair"}, "pair")) return;
    drop_ui("\U000F0432", "ГОТУЮ QR…", "", "", true);
}

static void dv_watch() {
    if (g_dv_mon) return;
    std::string dir = std::string(g_get_user_cache_dir()) + "/mgos";
    g_mkdir_with_parents(dir.c_str(), 0755);
    GFile* f = g_file_new_for_path(dv_tsv().c_str());
    g_dv_mon = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(f);
    if (g_dv_mon) g_signal_connect(g_dv_mon, "changed", G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer) {
        if (g_open && g_drop_mode) dv_rebuild();
    }), nullptr);
}

static void drop_files(GSList* files) {
    g_drop_files.clear();
    goffset total = 0;
    for (GSList* l = files; l; l = l->next) {
        gchar* p = g_file_get_path(G_FILE(l->data));
        if (!p) continue;
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
            g_drop_files.push_back(p);
            GStatBuf st; if (g_stat(p, &st) == 0) total += st.st_size;
        }
        g_free(p);
    }
    gtk_widget_remove_css_class(g_drop_zone, "hot");
    if (g_drop_files.empty()) { drop_ui("\U000F0026", "ЦЕ НЕ ФАЙЛ", "Теки поки не надсилаються — заархівуйте їх", "", false); return; }
    std::string name = g_drop_files.size() == 1 ? fs_basename(g_drop_files[0]) : std::to_string(g_drop_files.size()) + " файлів";
    g_object_set_data_full(G_OBJECT(g_drop_zone), "sh-sub", g_strdup((name + "  ·  " + human_size(total)).c_str()), g_free);
    sh_start();
}

static void drop_log(const std::string& m) {
    FILE* f = fopen("/tmp/sysui-topbar-drop.log", "a");
    if (f) { fprintf(f, "%s\n", m.c_str()); fclose(f); }
}

// Картинка з браузера приходить як http(s)-посилання — спершу завантажуємо її у тимчасову теку
static void drop_download(const std::string& url) {
    std::string dir = std::string(g_get_tmp_dir()) + "/mgos-share";
    g_mkdir_with_parents(dir.c_str(), 0700);
    std::string name = fs_basename(url.substr(0, url.find_first_of("?#")));
    if (name.empty() || name == "/" || name.find('.') == std::string::npos) name += ".jpg";
    std::string out = dir + "/" + name;
    drop_ui("\U000F01DA", "ЗАВАНТАЖУЮ…", name, "", true);
    gtk_widget_set_visible(g_qs_cancel, FALSE);
    GSubprocess* p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, nullptr, "curl", "-sfL", "--max-time", "60", "-o",
                                      out.c_str(), url.c_str(), nullptr);
    if (!p) { qs_finish("download"); return; }
    g_subprocess_wait_async(p, nullptr, [](GObject* o, GAsyncResult* r, gpointer d) {
        std::string* path = (std::string*)d;
        bool ok = g_subprocess_wait_finish(G_SUBPROCESS(o), r, nullptr) && g_subprocess_get_successful(G_SUBPROCESS(o));
        if (ok) {
            GFile* f = g_file_new_for_path(path->c_str());
            GSList* l = g_slist_append(nullptr, f);
            drop_files(l);
            g_slist_free_full(l, g_object_unref);
        } else {
            drop_ui("\U000F0026", "НЕ ВИЙШЛО", "", "Не вдалося завантажити картинку — збережіть її у файл і перетягніть файл", false);
        }
        delete path;
        g_object_unref(o);
    }, new std::string(out));
}

static gboolean on_drop(GtkDropTarget*, const GValue* v, double, double, gpointer) {
    drop_log(std::string("drop: type ") + G_VALUE_TYPE_NAME(v));
    if ((!g_open || !g_drop_mode) && g_last_bubble && g_last_bar) show_pop(g_last_bubble, g_last_bar, true);
    if (G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST)) {
        drop_log("drop: file-list");
        drop_files(gdk_file_list_get_files((GdkFileList*)g_value_get_boxed(v)));
        return TRUE;
    }
    if (G_VALUE_HOLDS(v, G_TYPE_STRING)) {
        std::string t = g_value_get_string(v) ? g_value_get_string(v) : "";
        drop_log("drop: string " + t.substr(0, 200));
        // text/uri-list або просто шлях/посилання; беремо перший рядок
        std::string first = t.substr(0, t.find_first_of("\r\n"));
        while (!first.empty() && first.back() == ' ') first.pop_back();
        if (first.rfind("file://", 0) == 0 || first.rfind("/", 0) == 0) {
            GSList* l = nullptr;
            std::istringstream ss(t);
            std::string line;
            while (std::getline(ss, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                l = g_slist_append(l, line[0] == '/' ? g_file_new_for_path(line.c_str()) : g_file_new_for_uri(line.c_str()));
            }
            drop_files(l);
            g_slist_free_full(l, g_object_unref);
            return TRUE;
        }
        if (first.rfind("http://", 0) == 0 || first.rfind("https://", 0) == 0) { drop_download(first); return TRUE; }
        drop_ui("\U000F0026", "ЦЕ НЕ ФАЙЛ", "", "Перетягніть файл із файлового менеджера або картинку з браузера", false);
        return TRUE;
    }
    return FALSE;
}

static GdkDrop* g_cur_drop = nullptr;
static GdkDragAction pick_action() {
    GdkDragAction a = g_cur_drop ? gdk_drop_get_actions(g_cur_drop) : GDK_ACTION_COPY;
    return (a & GDK_ACTION_COPY) ? GDK_ACTION_COPY : (a & GDK_ACTION_MOVE) ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
}

static GtkDropTarget* file_drop_target() {
    // Nautilus під Hyprland часто пропонує лише MOVE — приймаємо обидва (файл тільки читаємо)
    GtkDropTarget* dt = gtk_drop_target_new(G_TYPE_INVALID, (GdkDragAction)(GDK_ACTION_COPY | GDK_ACTION_MOVE));
    GType types[] = {GDK_TYPE_FILE_LIST, G_TYPE_STRING};
    gtk_drop_target_set_gtypes(dt, types, 2);
    g_signal_connect(dt, "drop", G_CALLBACK(on_drop), nullptr);
    g_signal_connect(dt, "notify::current-drop", G_CALLBACK(+[](GtkDropTarget* t, GParamSpec*, gpointer) {
        if (GdkDrop* d = gtk_drop_target_get_current_drop(t)) g_cur_drop = d; }), nullptr);
    return dt;
}

static void qs_install(GtkButton*, gpointer) {
    std::string tool = qs_tool();
    const char* argv[] = {"kitty", "--class", "mgos-recovery", "-e", tool.c_str(), "install", nullptr};
    g_spawn_async(nullptr, (gchar**)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr);
    hide_pop();
}

static void build_drop() {
    g_drop_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(g_drop_content, GTK_ALIGN_START);
    gtk_widget_set_valign(g_drop_content, GTK_ALIGN_START);
    gtk_widget_set_size_request(g_drop_content, g_drop_W, -1);
    for (auto f : {gtk_widget_set_margin_top, gtk_widget_set_margin_bottom, gtk_widget_set_margin_start, gtk_widget_set_margin_end})
        f(g_drop_content, 14);

    g_drop_zone = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(g_drop_zone, "cp-drop");
    g_drop_glyph = gtk_label_new("");
    gtk_widget_add_css_class(g_drop_glyph, "cp-drop-glyph");
    g_drop_title = gtk_label_new("");
    gtk_widget_add_css_class(g_drop_title, "cp-title");
    g_drop_sub = gtk_label_new("");
    gtk_widget_add_css_class(g_drop_sub, "cp-artist");
    gtk_label_set_ellipsize(GTK_LABEL(g_drop_sub), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(g_drop_sub), 40);
    g_drop_pin = gtk_label_new("");
    gtk_widget_add_css_class(g_drop_pin, "cp-drop-pin");
    g_drop_qr = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(g_drop_qr), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(g_drop_qr), TRUE);
    gtk_widget_set_size_request(g_drop_qr, 196, 196);
    gtk_widget_set_halign(g_drop_qr, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(g_drop_qr, FALSE);
    gtk_widget_add_css_class(g_drop_qr, "cp-qr");
    GtkWidget* vis = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(vis, -1, 220);
    gtk_widget_set_valign(g_drop_glyph, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(g_drop_glyph, TRUE);
    gtk_widget_set_valign(g_drop_qr, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(g_drop_qr, TRUE);
    gtk_box_append(GTK_BOX(vis), g_drop_glyph);
    gtk_box_append(GTK_BOX(vis), g_drop_qr);
    for (GtkWidget* w : {vis, g_drop_title, g_drop_sub, g_drop_pin}) gtk_box_append(GTK_BOX(g_drop_zone), w);
    gtk_box_append(GTK_BOX(g_drop_content), g_drop_zone);

    g_dv_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(g_dv_row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(g_drop_content), g_dv_row);
    dv_rebuild();
    dv_watch();

    g_drop_status = gtk_label_new("");
    gtk_widget_add_css_class(g_drop_status, "cp-artist");
    gtk_label_set_wrap(GTK_LABEL(g_drop_status), TRUE);
    gtk_label_set_justify(GTK_LABEL(g_drop_status), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(g_drop_status), 44);
    gtk_box_append(GTK_BOX(g_drop_content), g_drop_status);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    g_qs_send = pxbutton("\U000F048A  НАДІСЛАТИ", PX_PRIMARY, G_CALLBACK(qs_send));
    g_qs_cancel = pxbutton("\U000F0156  СКАСУВАТИ", PX_NEUTRAL, G_CALLBACK(qs_cancel));
    g_qs_open = pxbutton("\U000F0EB3  ВІКНО QUICK SHARE", PX_NEUTRAL, G_CALLBACK(+[](GtkButton*, gpointer) {
        std::string c = qs_tool() + " open";
        g_spawn_command_line_async(c.c_str(), nullptr);
        hide_pop();
    }));
    g_qs_install = pxbutton("\U000F01DA  ВСТАНОВИТИ", PX_PRIMARY, G_CALLBACK(qs_install));
    g_sh_allow = pxbutton("\U000F0FC6  ДОЗВОЛИТИ", PX_PRIMARY, G_CALLBACK(sh_allow));
    for (GtkWidget* b : {g_sh_allow, g_qs_send, g_qs_cancel, g_qs_open, g_qs_install}) gtk_box_append(GTK_BOX(row), b);
    gtk_box_append(GTK_BOX(g_drop_content), row);

    drop_reset();
    // фіксований розмір блоку дропу (з усіма рядками, щоб не «стрибав»): міряємо найвищий стан —
    // двохрядковий статус і рядок кнопок
    gtk_widget_set_visible(g_drop_status, TRUE);
    gtk_label_set_text(GTK_LABEL(g_drop_status), "Дозвольте телефону підключатися до цього ПК по Wi-Fi (пароль — лише один раз)");
    gtk_widget_set_visible(g_qs_cancel, TRUE);
    int mn, nat;
    gtk_widget_measure(g_drop_content, GTK_ORIENTATION_VERTICAL, g_drop_W + 28, &mn, &nat, nullptr, nullptr);
    g_drop_H = std::max(mn, nat) + 24;
    gtk_widget_set_visible(g_qs_cancel, FALSE);
    drop_reset();
    gtk_widget_measure(g_drop_content, GTK_ORIENTATION_HORIZONTAL, -1, &mn, &nat, nullptr, nullptr);
    g_drop_W = std::max(g_drop_W + 28, nat);
    gtk_widget_set_visible(g_drop_content, FALSE);
}

static void build(GtkWindow* bar) {
    g_win = gtk_application_window_new(gtk_window_get_application(bar));
    gtk_widget_add_css_class(g_win, "clock-popover");
    gtk_layer_init_for_window(GTK_WINDOW(g_win));
    gtk_layer_set_namespace(GTK_WINDOW(g_win), "clock_popover");
    gtk_layer_set_layer(GTK_WINDOW(g_win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
        gtk_layer_set_anchor(GTK_WINDOW(g_win), e, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(g_win), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_win), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    g_fixed = gtk_fixed_new();
    gtk_window_set_child(GTK_WINDOW(g_win), g_fixed);
    // Блок — повноекранний шар (перекриває й сам годинник), тож файл приймаємо будь-де на ньому
    {
        GtkDropTarget* dt = file_drop_target();
        g_signal_connect(dt, "enter", G_CALLBACK(+[](GtkDropTarget*, double, double, gpointer) -> GdkDragAction {
            if (g_drop_zone) gtk_widget_add_css_class(g_drop_zone, "hot");
            drop_log("pop: enter");
            return pick_action(); }), nullptr);
        g_signal_connect(dt, "leave", G_CALLBACK(+[](GtkDropTarget*, gpointer) {
            if (g_drop_zone) gtk_widget_remove_css_class(g_drop_zone, "hot"); drop_log("pop: leave"); }), nullptr);
        gtk_widget_add_controller(g_fixed, GTK_EVENT_CONTROLLER(dt));
    }

    g_card = gtk_overlay_new();
    gtk_widget_set_overflow(g_card, GTK_OVERFLOW_HIDDEN);
    g_bg = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_bg), draw_bg, nullptr, nullptr);
    gtk_overlay_set_child(GTK_OVERLAY(g_card), g_bg);
    gtk_fixed_put(GTK_FIXED(g_fixed), g_card, 0, 0);

    // ---------- вміст ----------
    g_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_main_content = g_content;
    gtk_widget_set_halign(g_content, GTK_ALIGN_START);
    gtk_widget_set_valign(g_content, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(g_card), g_content);

    g_header = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_header, -1, 108);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_header), draw_header, nullptr, nullptr);
    gtk_box_append(GTK_BOX(g_content), g_header);

    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(body, 14); gtk_widget_set_margin_end(body, 14); gtk_widget_set_margin_bottom(body, 14);
    gtk_box_append(GTK_BOX(g_content), body);

    // Календар
    GtkWidget* cal = card_box(nullptr);
    gtk_widget_set_size_request(cal, 300, -1);
    GtkWidget* ch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g_cal_title = gtk_label_new("");
    gtk_widget_add_css_class(g_cal_title, "cp-title");
    gtk_label_set_xalign(GTK_LABEL(g_cal_title), 0);
    GtkWidget* tbtn = gtk_button_new();
    gtk_widget_add_css_class(tbtn, "cp-flat");
    gtk_button_set_child(GTK_BUTTON(tbtn), g_cal_title);
    gtk_widget_set_tooltip_text(tbtn, "Повернутись до сьогодні");
    g_signal_connect(tbtn, "clicked", G_CALLBACK(cal_shift), GINT_TO_POINTER(0));
    gtk_box_append(GTK_BOX(ch), tbtn);
    GtkWidget* sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(ch), sp);
    gtk_box_append(GTK_BOX(ch), pxbutton("\U000F0141", PX_NEUTRAL, G_CALLBACK(cal_shift), GINT_TO_POINTER(-1), "small"));
    gtk_box_append(GTK_BOX(ch), pxbutton("\U000F0142", PX_NEUTRAL, G_CALLBACK(cal_shift), GINT_TO_POINTER(1), "small"));
    gtk_box_append(GTK_BOX(cal), ch);
    g_cal = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_cal, 276, 222);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_cal), draw_cal, nullptr, nullptr);
    GtkEventController* sc = gtk_event_controller_scroll_new(
        (GtkEventControllerScrollFlags)(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE));
    g_signal_connect(sc, "scroll", G_CALLBACK(on_cal_scroll), nullptr);
    gtk_widget_add_controller(g_cal, sc);
    gtk_box_append(GTK_BOX(cal), g_cal);
    GtkWidget* lcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(lcol), cal);
    // Активності: завантаження Steam, таймери, фокус — повний список із прогрес-барами
    GtkWidget* ac = card_box("АКТИВНОСТІ");
    g_acts_da = gtk_drawing_area_new();
    gtk_widget_set_vexpand(g_acts_da, TRUE);
    gtk_widget_set_size_request(g_acts_da, -1, 44);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_acts_da), draw_acts, nullptr, nullptr);
    gtk_box_append(GTK_BOX(ac), g_acts_da);
    gtk_box_append(GTK_BOX(lcol), ac);
    gtk_box_append(GTK_BOX(body), lcol);

    // Права колонка
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(right, 344, -1);
    gtk_box_append(GTK_BOX(body), right);

    // Музика
    GtkWidget* mc = card_box(nullptr);
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    g_art = gtk_image_new_from_icon_name("audio-x-generic-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(g_art), 64);
    gtk_widget_set_size_request(g_art, 64, 64);
    gtk_widget_set_overflow(g_art, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(g_art, "cp-art");
    gtk_box_append(GTK_BOX(top), g_art);
    GtkWidget* meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(meta, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(meta, TRUE);
    g_src = gtk_label_new("MPRIS");
    gtk_widget_add_css_class(g_src, "cp-src");
    g_title = gtk_label_new("");
    gtk_widget_add_css_class(g_title, "cp-track");
    g_artist = gtk_label_new("");
    gtk_widget_add_css_class(g_artist, "cp-artist");
    for (GtkWidget* l : {g_src, g_title, g_artist}) {
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 24);
        gtk_label_set_single_line_mode(GTK_LABEL(l), TRUE);
        gtk_box_append(GTK_BOX(meta), l);
    }
    gtk_box_append(GTK_BOX(top), meta);
    gtk_box_append(GTK_BOX(mc), top);

    g_wave = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_wave, -1, 40);
    gtk_widget_set_cursor_from_name(g_wave, "pointer");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_wave), draw_wave, nullptr, nullptr);
    GtkGesture* wc = gtk_gesture_click_new();
    g_signal_connect(wc, "released", G_CALLBACK(on_wave_click), nullptr);
    gtk_widget_add_controller(g_wave, GTK_EVENT_CONTROLLER(wc));
    gtk_box_append(GTK_BOX(mc), g_wave);

    GtkWidget* ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(ctl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(ctl), pxbutton("\U000F04AE", PX_NEUTRAL, G_CALLBACK(on_prev), nullptr, "mid"));
    g_play_btn = pxbutton("\U000F040A", PX_PLAY, G_CALLBACK(on_play), nullptr, "big");
    gtk_box_append(GTK_BOX(ctl), g_play_btn);
    gtk_box_append(GTK_BOX(ctl), pxbutton("\U000F04AD", PX_NEUTRAL, G_CALLBACK(on_next), nullptr, "mid"));
    gtk_box_append(GTK_BOX(mc), ctl);
    gtk_box_append(GTK_BOX(right), mc);

    // Сьогодні
    GtkWidget* tc = card_box("СЬОГОДНІ");
    g_bars = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_bars, -1, 72);
    gtk_widget_set_vexpand(g_bars, TRUE);
    gtk_widget_set_vexpand(tc, TRUE);   // права колонка добирає висоту лівої — низ блоку рівний
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_bars), draw_bars, nullptr, nullptr);
    gtk_box_append(GTK_BOX(tc), g_bars);
    GtkWidget* fr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_focus_btn = pxbutton("", PX_PRIMARY, G_CALLBACK(on_focus), nullptr);
    gtk_widget_set_hexpand(g_focus_btn, TRUE);
    g_break_btn = pxbutton("\U000F0176  ПЕРЕРВА 5", PX_NEUTRAL, G_CALLBACK(on_break), nullptr);
    gtk_box_append(GTK_BOX(fr), g_focus_btn);
    gtk_box_append(GTK_BOX(fr), g_break_btn);
    gtk_box_append(GTK_BOX(tc), fr);
    gtk_box_append(GTK_BOX(right), tc);
    focus_labels();

    // ---------- закриття: клік повз блок, Esc ----------
    GtkGesture* outside = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(outside), GTK_PHASE_CAPTURE);
    g_signal_connect(outside, "pressed", G_CALLBACK(+[](GtkGestureClick* gc, int, double x, double y, gpointer) {
        graphene_rect_t r;
        if (!gtk_widget_compute_bounds(g_card, g_fixed, &r)) return;
        graphene_point_t pt = GRAPHENE_POINT_INIT((float)x, (float)y);
        if (!graphene_rect_contains_point(&r, &pt)) {
            gtk_gesture_set_state(GTK_GESTURE(gc), GTK_EVENT_SEQUENCE_CLAIMED);
            hide_pop();
        }
    }), nullptr);
    gtk_widget_add_controller(g_fixed, GTK_EVENT_CONTROLLER(outside));
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint kv, guint, GdkModifierType, gpointer) -> gboolean {
        if (kv == GDK_KEY_Escape) { hide_pop(); return TRUE; }
        return FALSE;
    }), nullptr);
    gtk_widget_add_controller(g_win, keys);

    // натуральний розмір вмісту = кінцевий розмір блоку
    int mn, nat;
    gtk_widget_measure(g_content, GTK_ORIENTATION_HORIZONTAL, -1, &mn, &nat, nullptr, nullptr);
    g_W = std::max(mn, nat);
    gtk_widget_measure(g_content, GTK_ORIENTATION_VERTICAL, g_W, &mn, &nat, nullptr, nullptr);
    g_H = std::max(mn, nat);
    g_main_W = g_W; g_main_H = g_H;
    gtk_widget_set_visible(g_content, FALSE);

    build_drop();
    gtk_overlay_add_overlay(GTK_OVERLAY(g_card), g_drop_content);
    // файл можна кинути будь-куди на відкритий блок дропу
    GtkDropTarget* any = file_drop_target();
    gtk_widget_add_controller(g_fixed, GTK_EVENT_CONTROLLER(any));
}

static void show_pop(GtkWidget* bubble, GtkWidget* bar, bool drop = false) {
    if (!gtk_widget_get_visible(bar)) return;   // топбар схований (вікно на весь екран) — блок не відкриваємо
    if (!g_win) build(GTK_WINDOW(bar));
    g_last_bubble = bubble; g_last_bar = bar;
    if (g_open && drop == g_drop_mode) return;
    if (!g_open) sysui::close_other_overlays("clock");   // відкриваємо годинник — решта оверлеїв згортаються
    if (g_open) {                       // інший режим — миттєво згортаємо і відкриваємо заново
        g_open = false; g_v = 0; g_content_on = false;
        gtk_widget_set_visible(g_content, FALSE);
    }
    g_drop_mode = drop;
    // Під час перетягування не забираємо клавіатуру: Hyprland інакше обриває DnD
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_win), drop ? GTK_LAYER_SHELL_KEYBOARD_MODE_NONE
                                                        : GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    g_content = drop ? g_drop_content : g_main_content;
    g_W = drop ? g_drop_W : g_main_W;
    g_H = drop ? g_drop_H : g_main_H;
    if (drop && !g_qs_proc && !g_dv_proc) drop_reset();
    if (drop) dv_rebuild();
    // той самий монітор, що й топбар
    if (GdkSurface* s = gtk_native_get_surface(GTK_NATIVE(bar)))
        if (GdkMonitor* mon = gdk_display_get_monitor_at_surface(gtk_widget_get_display(bar), s))
            { gtk_layer_set_monitor(GTK_WINDOW(g_win), mon); sysui::bb::set_monitor(mon); }
    capture_bubble(bubble, bar);
    // акцент і палітра — як у топбару / інсталятора
    std::string acc = sysui::accent();
    g_wv.set_accent(acc);                      // хвилі — у відтінку акценту (±8°), без мандрів по палітрі
    g_wv.hue_lo = -8; g_wv.hue_hi = 8;
    { pw::RGB c; if (pw::parse_hex(acc, c)) pw::rgb_hsl(c, g_acc_h, g_acc_s, g_acc_l); }
    g_wv.fade_from = 0.5;

    g_open = true;
    g_v_from = std::clamp(g_v, 0.0, 1.0);
    g_anim_start = now_us();
    if (g_v_from <= 0.001) g_t0 = 0;
    g_cal_off = 0;
    cal_title_update();
    layout_morph();
    gtk_widget_add_css_class(bubble, "cp-still");
    gtk_widget_set_opacity(bubble, 0);          // сама бульбашка ховається — замість неї «живе» картка
    g_hidden_bubble = bubble;
    gtk_widget_set_visible(g_win, TRUE);
    ensure_tick();
    mp_refresh();
    acts_refresh_ui();
    if (!g_poll) g_poll = g_timeout_add_seconds(1, [](gpointer) -> gboolean { mp_refresh(); return G_SOURCE_CONTINUE; }, nullptr);
}

static void toggle(GtkWidget* bubble, GtkWidget* bar) { if (g_open) hide_pop(); else show_pop(bubble, bar); }

// Запит «надіслати на телефон» ззовні (Nautilus Scripts / mgos-share send): шляхи по рядку в
// ~/.cache/mgos/share-request, топбар отримує SIGUSR2 → відкриває блок і одразу показує QR.
static void open_request(GtkWidget* bubble, GtkWidget* bar) {
    std::string req = std::string(g_get_user_cache_dir()) + "/mgos/share-request";
    gchar* txt = nullptr;
    show_pop(bubble, bar, true);
    if (!g_file_get_contents(req.c_str(), &txt, nullptr, nullptr)) return;
    g_unlink(req.c_str());
    GSList* files = nullptr;
    std::istringstream ss(txt);
    for (std::string ln; std::getline(ss, ln);) if (!ln.empty()) files = g_slist_append(files, g_file_new_for_path(ln.c_str()));
    g_free(txt);
    if (files) drop_files(files);
    g_slist_free_full(files, g_object_unref);
}

// Дроп на саму бульбашку годинника: щойно будь-що перетягують над годинником —
// відкриваємо блок із зоною для файлу (DropControllerMotion реагує на будь-який формат,
// тож блок відкривається навіть для картинок із браузера чи Telegram).
static void attach_drop(GtkWidget* bubble, GtkWidget* bar) {
    GtkEventController* mc = gtk_drop_controller_motion_new();
    g_object_set_data(G_OBJECT(mc), "bar", bar);
    g_signal_connect(mc, "enter", G_CALLBACK(+[](GtkDropControllerMotion* m, double, double, gpointer b) {
        if (GdkDrop* d = gtk_drop_controller_motion_get_drop(m)) {
            gchar* f = gdk_content_formats_to_string(gdk_drop_get_formats(d));
            drop_log(std::string("enter: ") + (f ? f : ""));
            g_free(f);
        }
        // нове вікно посеред DnD Hyprland обриває — лише підсвічуємо годинник, блок відкриваємо після дропу
        g_last_bubble = GTK_WIDGET(b);
        g_last_bar = GTK_WIDGET(g_object_get_data(G_OBJECT(m), "bar"));
        gtk_widget_add_css_class(GTK_WIDGET(b), "drop-hot");
    }), bubble);
    g_signal_connect(mc, "leave", G_CALLBACK(+[](GtkDropControllerMotion*, gpointer b) {
        gtk_widget_remove_css_class(GTK_WIDGET(b), "drop-hot"); drop_log("bubble: leave"); }), bubble);
    gtk_widget_add_controller(bubble, mc);
    GtkDropTarget* bdt = file_drop_target();
    g_signal_connect(bdt, "enter", G_CALLBACK(+[](GtkDropTarget*, double, double, gpointer) -> GdkDragAction {
        drop_log("bubble-dt: enter"); return pick_action(); }), nullptr);
    g_signal_connect(bdt, "leave", G_CALLBACK(+[](GtkDropTarget*, gpointer) { drop_log("bubble-dt: leave"); }), nullptr);
    gtk_widget_add_controller(bubble, GTK_EVENT_CONTROLLER(bdt));
}

}  // namespace cpop

// CSS блоку (додається до CSS_BASE топбару)
static const char* clock_popover_css() { return R"(
.cp-card { background-color: rgba(255,255,255,0.045); border: 1px solid rgba(255,255,255,0.06); border-radius: 16px; padding: 12px; }
.cp-title, .cp-title label { font-family: "JetBrainsMono Nerd Font", monospace; font-weight: 800; font-size: 12px; letter-spacing: 1px; color: #f5f5f7; }
.cp-src { font-family: "JetBrainsMono Nerd Font", monospace; font-weight: 800; font-size: 10px; letter-spacing: 1px; color: alpha(#f5f5f7, 0.5); }
.cp-track { font-family: "SF Pro Display", "Inter", sans-serif; font-weight: 700; font-size: 15px; color: #f5f5f7; }
.cp-artist { font-family: "SF Pro Display", "Inter", sans-serif; font-size: 12px; color: alpha(#f5f5f7, 0.6); }
.cp-art { border-radius: 12px; background-color: rgba(255,255,255,0.06); color: alpha(#f5f5f7, 0.5); }
button.cp-pxbtn, button.cp-flat { background: none; border: none; box-shadow: none; padding: 0; margin: 0; min-height: 0; min-width: 0; }
button.cp-flat:hover label { color: #ffffff; }
.cp-pxlbl { font-family: "JetBrainsMono Nerd Font", monospace; font-weight: 800; font-size: 12px; letter-spacing: 1px; color: #f5f5f7; padding: 9px 14px; }
.cp-pxlbl.small { font-size: 16px; padding: 2px 8px; }
.cp-pxlbl.mid { font-size: 18px; padding: 8px 14px; }
.cp-pxlbl.big { font-size: 22px; padding: 10px 22px; }
.cp-drop { border: 2px dashed alpha(#f5f5f7, 0.28); border-radius: 18px; padding: 22px 16px 18px 16px;
           background-color: rgba(255,255,255,0.03); transition: background-color 160ms ease, border-color 160ms ease; }
.cp-drop.hot { border-color: alpha(#f5f5f7, 0.85); background-color: rgba(255,255,255,0.10); }
.cp-drop.busy { border-style: solid; border-color: alpha(#f5f5f7, 0.18); }
.cp-drop.done { border-style: solid; border-color: #30d158; background-color: rgba(48,209,88,0.12); }
.cp-drop-glyph { font-family: "JetBrainsMono Nerd Font"; font-size: 44px; color: #f5f5f7; margin-bottom: 4px; }
.cp-drop-pin { font-family: "JetBrainsMono Nerd Font", monospace; font-weight: 800; font-size: 22px; letter-spacing: 4px;
               color: #f5f5f7; margin-top: 6px; }
.module-box.cp-still, .module-box.cp-still:hover, .module-box.cp-still:active { transition: none; transform: none; }
.module-box.drop-hot { background-color: rgba(255,255,255,0.28); box-shadow: 0 0 0 2px alpha(#f5f5f7, 0.85); }
.cp-qr { background-color: #ffffff; border-radius: 18px; padding: 10px; }
.cp-drop.qr { border-style: solid; border-color: alpha(#f5f5f7, 0.14); }
.cp-drop-glyph { font-size: 64px; }
button.cp-dv { background-color: rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.07); border-radius: 14px;
               padding: 8px 10px 6px 10px; min-width: 64px; box-shadow: none; transition: background-color 140ms ease, border-color 140ms ease; }
button.cp-dv:hover { background-color: rgba(255,255,255,0.11); }
button.cp-dv.online { border-color: alpha(#30d158, 0.75); }
button.cp-dv.add { border-style: dashed; border-color: alpha(#f5f5f7, 0.22); background-color: transparent; }
.cp-dv-glyph { font-family: "JetBrainsMono Nerd Font"; font-size: 22px; color: alpha(#f5f5f7, 0.55); }
button.cp-dv.online .cp-dv-glyph { color: #f5f5f7; }
.cp-dv-name { font-family: "SF Pro Display", "Inter", sans-serif; font-size: 11px; color: alpha(#f5f5f7, 0.7); }
)"; }
