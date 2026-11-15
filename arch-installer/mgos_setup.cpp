// mgos_setup.cpp — графічний інсталятор Megumin OS з живої флешки (Arch / Arch + Hyprland / mgOS).
//
//   Вигляд — як у привітання mgOS (configs/SYSui/welcome.cpp): піксельне сяйво на весь екран,
//   «скляна» картка по центру, сторінки з крапками внизу. Піксельні елементи — з mgOS-інсталятора:
//   кнопки з квадратиків, цифри 5×7, прогрес-бар, розбитий на розділи (по одному на крок).
//   Карта часових поясів — теж піксельна (маска суші worldmap.h, міста — /usr/share/zoneinfo/zone1970.tab).
//
//   Сторінки: live → мова → регіон → клавіатура → Wi-Fi → що встановити → диск → обліковий запис →
//             драйвери → перевірка → встановлення → готово.
//   Саме встановлення робить mgos-arch-install.sh (root, через sudo); тут — лише інтерфейс:
//   він пише install.conf (0600) і читає рядки @@STEP/@@PCT/@@MSG/… з виводу.
//
//   mgos-setup                 — звичайний запуск (жива флешка)
//   mgos-setup --demo          — нічого не змінює: бекенд у DRY_RUN, розкладку/Wi-Fi не чіпає
//   mgos-setup --window        — звичайне вікно замість шару на весь екран
//   mgos-setup --shot PAGE OUT — (з --demo) зберегти PAGE у PNG і вийти (перевірка вигляду)
#include "sysui_common.h"
#include "pixel_waves.h"
#include "worldmap.h"
#include <functional>
#include <set>
#include <map>
#include <filesystem>
#include <regex>
#include <deque>
#include <fcntl.h>
namespace fs = std::filesystem;

// ============================================================================
//  Стан
// ============================================================================
struct Opts {
    std::string ui = "uk";                         // мова інтерфейсу: uk | en
    std::string locale = "uk_UA.UTF-8", formats = "uk_UA.UTF-8", tz = "Europe/Kyiv";
    bool tz_user = false, ntp = true;
    std::vector<std::string> layouts = {"us", "ua"};
    std::string toggle = "grp:alt_shift_toggle";
    std::string edition = "mgos";
    std::set<std::string> extras;
    std::string disk, mode = "erase", root_part, fs = "btrfs";
    bool encrypt = false, zram = true;
    std::string fullname, username, hostname;
    bool user_edited = false, host_edited = false, autologin = false;
} O;
static bool g_demo = false, g_window = false;
static std::string g_shot_page, g_shot_out;
static std::string g_self_dir;
static bool g_online = false, g_uefi = true, g_laptop = false;

static const char* T(const char* uk, const char* en) { return O.ui == "uk" ? uk : en; }
static std::string Ts(const char* uk, const char* en) { return T(uk, en); }

static pw::Waves g_wv;
static GtkWidget *g_win, *g_area, *g_ui, *g_card, *g_stack, *g_dots, *g_back, *g_next, *g_nav, *g_mini;
static double g_t = 0; static gint64 g_t0 = 0;
static std::vector<GtkWidget*> g_anim;            // перемальовуються кожен кадр
static const char* PAGES[] = {"live", "lang", "region", "kbd", "wifi", "edition", "disk", "user", "drivers", "summary", "install", "done"};
static const int NPAGES = 12;
static std::string g_page = "live";
static std::string g_return_to;                    // Wi-Fi / Обладнання відкрито з головної — «Назад» повертає туди

static double ease(double x) { x = std::clamp(x, 0.0, 1.0); return 1 - std::pow(1 - x, 3); }
// віджети сторінок перебудовуються — глобальні вказівники на них обнуляються самі (weak pointer),
// а анімовані області прибираються зі списку при знищенні
static void weak(GtkWidget** slot, GtkWidget* w) {
    if (*slot) g_object_remove_weak_pointer(G_OBJECT(*slot), (gpointer*)slot);
    *slot = w;
    if (w) g_object_add_weak_pointer(G_OBJECT(w), (gpointer*)slot);
}
static void anim_add(GtkWidget* w) {
    g_anim.push_back(w);
    g_signal_connect(w, "destroy", G_CALLBACK(+[](GtkWidget* w, gpointer) { g_anim.erase(std::remove(g_anim.begin(), g_anim.end(), w), g_anim.end()); }), nullptr);
}

// ============================================================================
//  Дрібниці
// ============================================================================
static std::string sh(const std::string& cmd) {
    gchar* out = nullptr;
    std::string c = "sh -c " + std::string(g_shell_quote(cmd.c_str()));
    if (!g_spawn_command_line_sync(c.c_str(), &out, nullptr, nullptr, nullptr)) return "";
    std::string r = out ? out : ""; g_free(out); return r;
}
static void sh_async(const std::string& cmd, std::function<void(const std::string&, bool)> cb) {
    GSubprocess* p = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                      nullptr, "sh", "-c", cmd.c_str(), nullptr);
    if (!p) { cb("", false); return; }
    auto* f = new std::function<void(const std::string&, bool)>(std::move(cb));
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, [](GObject* s, GAsyncResult* r, gpointer d) {
        auto* f = (std::function<void(const std::string&, bool)>*)d;
        char* out = nullptr;
        gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(s), r, &out, nullptr, nullptr);
        bool good = ok && g_subprocess_get_if_exited(G_SUBPROCESS(s)) && g_subprocess_get_exit_status(G_SUBPROCESS(s)) == 0;
        (*f)(out ? out : "", good);
        g_free(out); g_object_unref(s); delete f;
    }, f);
}
static void run_bg(const std::string& cmd) {
    char* q = g_shell_quote(cmd.c_str());
    g_spawn_command_line_async(("sh -c " + std::string(q)).c_str(), nullptr); g_free(q);
}
static std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}
static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> r; std::string c; std::istringstream ss(s);
    while (std::getline(ss, c, d)) r.push_back(c);
    if (!s.empty() && s.back() == d) r.push_back("");
    return r;
}
static std::string human(long long b) {
    char buf[32]; double g = b / 1e9;
    if (g >= 1000) snprintf(buf, sizeof buf, "%.1f %s", g / 1000, T("ТБ", "TB"));
    else if (g >= 10) snprintf(buf, sizeof buf, "%.0f %s", g, T("ГБ", "GB"));
    else snprintf(buf, sizeof buf, "%.1f %s", g, T("ГБ", "GB"));
    return buf;
}
static GtkWidget* label(const std::string& t, const char* cls = nullptr, float x = 0) {
    GtkWidget* l = gtk_label_new(t.c_str());
    if (cls) gtk_widget_add_css_class(l, cls);
    gtk_label_set_xalign(GTK_LABEL(l), x);
    return l;
}
static GtkWidget* hbox(int sp = 8) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, sp); }
static GtkWidget* vbox(int sp = 8) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, sp); }
static GtkWidget* spacer() { GtkWidget* s = hbox(0); gtk_widget_set_hexpand(s, TRUE); return s; }
static void clear(GtkWidget* b) { while (GtkWidget* c = gtk_widget_get_first_child(b)) gtk_widget_unparent(c); }
static GtkWidget* icon(const char* glyph, const char* cls = "ico") { return label(glyph, cls, 0.5); }

// ============================================================================
//  Піксельні елементи (з mgOS-інсталятора): кнопки, цифри 5×7, прогрес-бар
// ============================================================================
static const int BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
enum PxKind { PX_NEUTRAL, PX_PRIMARY, PX_DANGER };
static void draw_pxbtn(GtkDrawingArea* da, cairo_t* cr, int w, int h, gpointer data) {
    GtkWidget* btn = gtk_widget_get_ancestor(GTK_WIDGET(da), GTK_TYPE_BUTTON);
    GtkStateFlags st = btn ? gtk_widget_get_state_flags(btn) : (GtkStateFlags)0;
    bool hover = st & GTK_STATE_FLAG_PRELIGHT, press = st & GTK_STATE_FLAG_ACTIVE, dis = st & GTK_STATE_FLAG_INSENSITIVE;
    int kind = GPOINTER_TO_INT(data);
    const int cell = 4, gap = 1;
    int cols = w / cell, rows = h / cell;
    if (cols < 4 || rows < 3) return;
    double ox = (w - cols * cell) / 2.0, oy = (h - rows * cell) / 2.0;
    double sweep = std::fmod(g_t * 28.0, cols + 24.0) - 12;   // смуга світла при наведенні
    for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++) {
        int cx = std::min(x, cols - 1 - x), cy = std::min(y, rows - 1 - y);
        if (cx == 0 && cy == 0) continue;                     // піксельний кут
        bool edge = cx == 0 || cy == 0;
        double bay = BAYER4[y & 3][x & 3] / 16.0;
        double lift = hover ? 0.16 * std::exp(-std::pow((x - y * 0.6 - sweep) / 3.0, 2)) : 0;
        if (press) lift -= 0.08;
        double a = 1; pw::RGB c;
        if (kind == PX_NEUTRAL) {
            c = {1, 1, 1};
            a = (hover ? 0.13 : 0.075) + (edge ? 0.05 : 0) + lift * 0.6 + (bay - 0.5) * 0.02;
        } else {
            double hue = kind == PX_PRIMARY ? g_wv.hue_at(g_t) + g_wv.grad((double)x / cols) : 352;
            double sat = kind == PX_PRIMARY ? g_wv.sat() : 0.78;
            double l = 0.56 + (bay - 0.5) * 0.05 + lift;
            if (y == 0 || (cy == 0 && y < rows / 2)) l += 0.1;
            if (y == rows - 1) l -= 0.12;
            c = pw::hsl_rgb(hue, sat, std::clamp(l, 0.0, 0.92));
        }
        if (dis) a *= 0.4;
        cairo_set_source_rgba(cr, c.r, c.g, c.b, std::clamp(a, 0.0, 1.0));
        cairo_rectangle(cr, ox + x * cell, oy + y * cell, cell - gap, cell - gap);
        cairo_fill(cr);
    }
}
static GtkWidget* pxbutton(const std::string& t, PxKind kind, std::function<void()> cb) {
    GtkWidget* b = gtk_button_new();
    gtk_widget_add_css_class(b, "pxbtn");
    if (kind == PX_PRIMARY) gtk_widget_add_css_class(b, "pri");
    gtk_widget_set_cursor_from_name(b, "pointer");
    GtkWidget* ov = gtk_overlay_new();
    GtkWidget* da = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_pxbtn, GINT_TO_POINTER(kind), nullptr);
    gtk_overlay_set_child(GTK_OVERLAY(ov), da);
    GtkWidget* l = gtk_label_new(t.c_str());
    gtk_widget_add_css_class(l, "pxlbl");
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), l);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(ov), l, TRUE);
    gtk_button_set_child(GTK_BUTTON(b), ov);
    g_object_set_data(G_OBJECT(b), "lbl", l);
    anim_add(da);
    auto* f = new std::function<void()>(std::move(cb));
    g_signal_connect_data(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { (*(std::function<void()>*)d)(); }), f,
                          [](gpointer d, GClosure*) { delete (std::function<void()>*)d; }, (GConnectFlags)0);
    return b;
}
static void pxbutton_set(GtkWidget* b, const std::string& t) {
    if (auto* l = (GtkWidget*)g_object_get_data(G_OBJECT(b), "lbl")) gtk_label_set_text(GTK_LABEL(l), t.c_str());
}

static const char* glyph(char ch) {
    switch (ch) {
        case '0': return "01110100011001110101110011000101110";
        case '1': return "00100011000010000100001000010001110";
        case '2': return "01110100010000100010001000100011111";
        case '3': return "11110000010000101110000010000111110";
        case '4': return "00010001100101010010111110001000010";
        case '5': return "11111100001111000001000011000101110";
        case '6': return "00110010001000011110100011000101110";
        case '7': return "11111000010001000100010000100001000";
        case '8': return "01110100011000101110100011000101110";
        case '9': return "01110100011000101111000010001001100";
        case '%': return "11001110100001000100010000101110011";
        case '-': return "00000000000000011111000000000000000";
        default:  return "00000000000000000000000000000000000";
    }
}
// жирні піксельні цифри по центру області; хвиля яскравості біжить по стовпцях
static void draw_pixel_text(cairo_t* cr, const std::string& txt, int w, int h, int cell) {
    const int GW = 5, GH = 7, DW = GW + 1;
    int gw = (int)txt.size() * (DW + 1) - 1;
    double ox = std::round((w - gw * cell) / 2.0), oy = std::round((h - GH * cell) / 2.0);
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < txt.size(); i++) {
            const char* g = glyph(txt[i]);
            auto on = [&](int r, int c) { return c >= 0 && c < GW && g[r * GW + c] == '1'; };
            for (int r = 0; r < GH; r++) for (int c = 0; c < DW; c++) {
                if (!(on(r, c) || on(r, c - 1))) continue;
                int gx = (int)i * (DW + 1) + c;
                double x = ox + gx * cell, y = oy + r * cell;
                if (pass == 0) { cairo_set_source_rgba(cr, 0, 0, 0, 0.45); cairo_rectangle(cr, x + cell * 0.4, y + cell * 0.4, cell - 1, cell - 1); }
                else {
                    double wave = 0.5 + 0.5 * std::sin(gx * 0.45 - g_t * 4 + r * 0.4);
                    pw::RGB col = pw::hsl_rgb(g_wv.hue_at(g_t) + 1.5 * gx - 6, g_wv.sat(), 0.6 + 0.14 * wave - r * 0.015);
                    cairo_set_source_rgb(cr, col.r, col.g, col.b);
                    cairo_rectangle(cr, x, y, cell - 1, cell - 1);
                }
                cairo_fill(cr);
            }
        }
}

// ============================================================================
//  Кроки встановлення (ті самі id, що @@STEP у mgos-arch-install.sh)
// ============================================================================
enum class St { Pending, Running, Done, Failed };
struct StepDef { const char* id; const char* uk; const char* en; double weight; };
static const StepDef STEP_DEFS[] = {
    {"prepare", "Підготовка", "Preparing", 2},
    {"disk", "Розмітка диска", "Partitioning", 3},
    {"base", "Базова система (pacstrap)", "Base system (pacstrap)", 20},
    {"drivers", "Драйвери", "Drivers", 6},
    {"system", "Мова, користувач, завантажувач", "Locale, user, bootloader", 6},
    {"desktop", "Hyprland і програми", "Hyprland and apps", 15},
    {"mgos", "Пакети та налаштування mgOS", "mgOS packages and settings", 45},
    {"finish", "Завершення", "Finishing", 1},
};
struct Run {
    std::vector<std::string> steps;
    std::map<std::string, St> st;
    std::string cur, msg, fail;
    double sub = 0;                 // 0..1 усередині поточного кроку
    double shown = 0;               // плавне значення для малювання
    bool running = false, ok = false;
    gint64 started = 0;
} R;
static const StepDef* step_def(const std::string& id) { for (auto& d : STEP_DEFS) if (id == d.id) return &d; return nullptr; }
static double run_fraction() {
    double tw = 0, done = 0;
    for (auto& id : R.steps) {
        double w = step_def(id)->weight; tw += w;
        if (R.st[id] == St::Done) done += w;
        else if (id == R.cur) done += w * R.sub;
    }
    return tw > 0 ? done / tw : 0;
}

// прогрес-бар, розбитий на розділи: ширина — за вагою кроку; готові — хвиля акценту, поточний — «голова»
static void draw_pbar(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    const int cell = 6, gap = 1, cols = w / cell, rows = std::max(1, h / cell);
    const int n = (int)R.steps.size();
    if (n == 0) return;
    double tw = 0; for (auto& id : R.steps) tw += step_def(id)->weight;
    const int avail = cols - (n - 1);
    std::vector<int> wd(n); int used = 0, widest = 0;
    for (int i = 0; i < n; i++) {
        wd[i] = std::max(2, (int)std::lround(step_def(R.steps[i])->weight / tw * avail));
        used += wd[i]; if (wd[i] > wd[widest]) widest = i;
    }
    wd[widest] = std::max(2, wd[widest] + avail - used);
    const double pos = R.shown * tw;
    int x = 0; double cw = 0;
    for (int i = 0; i < n; i++) {
        const std::string& id = R.steps[i];
        double wt = step_def(id)->weight;
        St st = R.st[id];
        bool cur = id == R.cur && R.running;
        double sf = std::clamp((pos - cw) / wt, 0.0, 1.0);
        if (st == St::Done) sf = 1;
        int filled = (int)std::lround(sf * wd[i]);
        for (int lx = 0; lx < wd[i]; lx++) {
            int X = x + lx;
            for (int y = 0; y < rows; y++) {
                double d = BAYER4[y & 3][X & 3] / 16.0 - 0.5, a = 1;
                pw::RGB c{1, 1, 1};
                if (lx < filled) {
                    double wave = 0.5 + 0.5 * std::sin(X * 0.25 - g_t * 5 + y * 0.6);
                    double l = 0.42 + 0.16 * wave + d * 0.06, hue = g_wv.hue_at(g_t) + g_wv.grad((double)X / cols), sat = g_wv.sat();
                    if (st == St::Failed) { hue = 352; sat = 0.85; }
                    if (cur && lx >= filled - 2) l = 0.72 + 0.15 * std::sin(g_t * 9);
                    c = pw::hsl_rgb(hue, sat, l);
                } else if (st == St::Failed) { c = {1, 0.35, 0.42}; a = 0.14 + 0.06 * std::sin(g_t * 4); }
                else if (cur) a = 0.12 + 0.04 * std::sin(g_t * 3 - lx * 0.4) + (d < -0.25 ? 0.03 : 0);
                else a = 0.06 + (d < -0.25 ? 0.03 : 0);
                cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
                cairo_rectangle(cr, X * cell, y * cell, cell - gap, cell - gap);
                cairo_fill(cr);
            }
        }
        x += wd[i] + 1; cw += wt;
    }
}

// ============================================================================
//  Дані: мови, часові пояси, розкладки, диски, обладнання
// ============================================================================
struct Lang { const char* name; const char* loc; const char* kb; };
static const Lang LANGS[] = {
    {"Українська", "uk_UA.UTF-8", "ua"}, {"English (US)", "en_US.UTF-8", "us"}, {"English (UK)", "en_GB.UTF-8", "gb"},
    {"Polski", "pl_PL.UTF-8", "pl"}, {"Deutsch", "de_DE.UTF-8", "de"}, {"Français", "fr_FR.UTF-8", "fr"},
    {"Español", "es_ES.UTF-8", "es"}, {"Italiano", "it_IT.UTF-8", "it"}, {"Português (Brasil)", "pt_BR.UTF-8", "br"},
    {"Čeština", "cs_CZ.UTF-8", "cz"}, {"Türkçe", "tr_TR.UTF-8", "tr"}, {"日本語", "ja_JP.UTF-8", "jp"},
};
static std::vector<std::string> all_locales() {
    static std::vector<std::string> v;
    if (!v.empty()) return v;
    std::ifstream f("/usr/share/i18n/SUPPORTED"); std::string l;
    while (std::getline(f, l)) { auto sp = l.find(' '); if (sp != std::string::npos && l.substr(sp + 1) == "UTF-8") v.push_back(l.substr(0, sp)); }
    if (v.empty()) for (auto& L : LANGS) v.push_back(L.loc);
    return v;
}
static std::string lang_name(const std::string& loc) {
    for (auto& L : LANGS) if (loc == L.loc) return L.name;
    return loc;
}

struct Zone { std::string name; double lat, lon; };
static std::vector<Zone>& zones() {
    static std::vector<Zone> z;
    if (!z.empty()) return z;
    std::ifstream f("/usr/share/zoneinfo/zone1970.tab"); std::string l;
    std::regex re(R"(([+-])(\d{2})(\d{2})(\d{2})?([+-])(\d{3})(\d{2})(\d{2})?)");
    while (std::getline(f, l)) {
        if (l.empty() || l[0] == '#') continue;
        auto c = split(l, '\t'); if (c.size() < 3) continue;
        std::smatch m; if (!std::regex_match(c[1], m, re)) continue;
        double lat = std::stoi(m[2]) + std::stoi(m[3]) / 60.0, lon = std::stoi(m[6]) + std::stoi(m[7]) / 60.0;
        if (m[1] == "-") lat = -lat;
        if (m[5] == "-") lon = -lon;
        z.push_back({trim(c[2]), lat, lon});
    }
    std::sort(z.begin(), z.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return z;
}
static std::string tz_now(const std::string& tz) {
    GTimeZone* z = g_time_zone_new_identifier(tz.c_str());
    if (!z) return "";
    GDateTime* d = g_date_time_new_now(z);
    char* s = g_date_time_format(d, "%H:%M");
    std::string r = s; g_free(s); g_date_time_unref(d); g_time_zone_unref(z);
    return r;
}
static double tz_offset_h(const std::string& tz) {
    GTimeZone* z = g_time_zone_new_identifier(tz.c_str());
    if (!z) return 0;
    GDateTime* d = g_date_time_new_now(z);
    double h = g_date_time_get_utc_offset(d) / 3.6e9;
    g_date_time_unref(d); g_time_zone_unref(z);
    return h;
}

struct Layout { std::string code, desc; };
static std::vector<Layout>& xkb_layouts() {
    static std::vector<Layout> v;
    if (!v.empty()) return v;
    std::ifstream f("/usr/share/X11/xkb/rules/base.lst"); std::string l; bool in = false;
    while (std::getline(f, l)) {
        if (l.rfind("! ", 0) == 0) { in = l == "! layout"; continue; }
        if (!in) continue;
        std::istringstream ss(l); std::string code; ss >> code; std::string d; std::getline(ss, d);
        if (!code.empty()) v.push_back({code, trim(d)});
    }
    return v;
}
static std::string layout_desc(const std::string& code) {
    if (code == "ua") return T("Українська", "Ukrainian");
    if (code == "us") return "English (US)";
    for (auto& l : xkb_layouts()) if (l.code == code) return l.desc;
    return code;
}

struct Part { std::string path, fstype, label, parttype; long long size = 0; bool esp = false; };
struct Disk { std::string path, model, tran; long long size = 0; bool rm = false; std::vector<Part> parts; long long free = 0; bool windows = false, other_os = false; Part* esp = nullptr; };
static std::vector<Disk> g_disks;
static void load_disks() {
    g_disks.clear();
    std::string out = sh("lsblk -J -b -o PATH,SIZE,TYPE,MODEL,TRAN,RM,RO,FSTYPE,PARTTYPE,LABEL,MOUNTPOINTS 2>/dev/null");
    nlohmann::json j = nlohmann::json::parse(out, nullptr, false);
    if (j.is_discarded() || !j.contains("blockdevices")) return;
    for (auto& d : j["blockdevices"]) {
        if (d.value("type", "") != "disk" || d.value("ro", false)) continue;
        std::string path = d.value("path", "");
        if (path.find("/dev/zram") == 0 || path.find("/dev/loop") == 0 || path.find("/dev/sr") == 0) continue;
        Disk D; D.path = path; D.size = d["size"].is_number() ? d["size"].get<long long>() : 0;
        D.model = trim(d.value("model", d["model"].is_null() ? "" : "")); if (d["model"].is_string()) D.model = trim(d["model"].get<std::string>());
        if (d["tran"].is_string()) D.tran = d["tran"].get<std::string>();
        D.rm = d.value("rm", false);
        bool live = false; long long used = 0;
        std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& n) {
            if (n.contains("mountpoints")) for (auto& m : n["mountpoints"]) if (m.is_string() && m.get<std::string>().find("/run/archiso") == 0) live = true;
            if (n.contains("children")) for (auto& c : n["children"]) walk(c);
        };
        walk(d);
        if (live) continue;                                   // сама флешка, з якої завантажились
        if (d.contains("children")) for (auto& c : d["children"]) {
            if (c.value("type", "") != "part") continue;
            Part P; P.path = c.value("path", ""); P.size = c["size"].is_number() ? c["size"].get<long long>() : 0;
            if (c["fstype"].is_string()) P.fstype = c["fstype"].get<std::string>();
            if (c["label"].is_string()) P.label = c["label"].get<std::string>();
            if (c["parttype"].is_string()) P.parttype = c["parttype"].get<std::string>();
            P.esp = g_ascii_strcasecmp(P.parttype.c_str(), "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") == 0;
            if (P.fstype == "ntfs" || P.fstype == "BitLocker") D.windows = true;
            if (!P.esp && !P.fstype.empty() && P.fstype != "swap") D.other_os = true;
            used += P.size;
            D.parts.push_back(P);
        }
        D.free = std::max(0LL, D.size - used - 2 * 1048576LL);
        g_disks.push_back(D);
    }
    for (auto& D : g_disks) for (auto& P : D.parts) if (P.esp) { D.esp = &P; break; }
    if (O.disk.empty() && !g_disks.empty()) {
        // типово — найбільший незнімний диск
        const Disk* best = &g_disks[0];
        for (auto& D : g_disks) if ((!D.rm && best->rm) || (D.rm == best->rm && D.size > best->size)) best = &D;
        O.disk = best->path;
    }
}
static Disk* cur_disk() { for (auto& D : g_disks) if (D.path == O.disk) return &D; return nullptr; }
static const long long MIN_FREE = 25LL * 1000 * 1000 * 1000;
static bool free_mode_ok(const Disk* D) { return D && D->esp && D->free >= MIN_FREE && g_uefi; }

struct Hw { std::string ico, kind, dev, pkgs; };
static std::vector<Hw> g_hw;
static std::string backend_path() {
    std::string p = g_self_dir + "/mgos-arch-install.sh";
    if (access(p.c_str(), X_OK) == 0) return p;
    return "/usr/local/bin/mgos-arch-install";
}
static void load_hw() {
    g_hw.clear();
    std::istringstream ss(sh(backend_path() + " --detect 2>/dev/null")); std::string l;
    while (std::getline(ss, l)) {
        if (l.rfind("@@HW ", 0) != 0) continue;
        auto f = split(l.substr(5), '|'); while (f.size() < 4) f.push_back("");
        g_hw.push_back({f[0], f[1], trim(f[2]), trim(f[3])});
        if (f[1] == "laptop" && !trim(f[2]).empty()) g_laptop = true;
        if (f[1] == "fw") g_uefi = trim(f[2]) == "UEFI";
    }
}

// ============================================================================
//  Фон: сяйво на весь екран + «скло» під карткою (як у welcome)
// ============================================================================
G_DECLARE_FINAL_TYPE(SetupView, setup_view, SETUP, VIEW, GtkWidget)
struct _SetupView { GtkWidget parent; };
G_DEFINE_TYPE(SetupView, setup_view, GTK_TYPE_WIDGET)
static void setup_view_snapshot(GtkWidget* w, GtkSnapshot* snap) {
    int W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
    if (W <= 0 || H <= 0) return;
    graphene_rect_t full = GRAPHENE_RECT_INIT(0, 0, (float)W, (float)H);
    GdkRGBA black = {0, 0, 0, 1};
    gtk_snapshot_append_color(snap, &black, &full);
    g_wv.t = g_t; g_wv.reveal = std::clamp(g_t / 1.6, 0.0, 1.0);
    g_wv.cell = std::max(4, W / 320);
    g_wv.render(W, H);
    if (!g_wv.surf) return;
    int cw = g_wv.sw, ch = g_wv.sh, st = cairo_image_surface_get_stride(g_wv.surf);
    GBytes* by = g_bytes_new(cairo_image_surface_get_data(g_wv.surf), (gsize)st * ch);
    GdkTexture* tex = gdk_memory_texture_new(cw, ch, GDK_MEMORY_DEFAULT, by, st);
    g_bytes_unref(by);
    graphene_rect_t tr = GRAPHENE_RECT_INIT(0, 0, (float)(cw * g_wv.cell), (float)(ch * g_wv.cell));
    gtk_snapshot_append_scaled_texture(snap, tex, GSK_SCALING_FILTER_NEAREST, &tr);
    graphene_rect_t cb{};
    if (g_card && gtk_widget_get_visible(g_card) && gtk_widget_get_visible(g_ui) && gtk_widget_compute_bounds(g_card, w, &cb)) {
        GskRoundedRect rr; gsk_rounded_rect_init_from_rect(&rr, &cb, 30);
        gtk_snapshot_push_rounded_clip(snap, &rr);
        gtk_snapshot_push_blur(snap, 36);
        gtk_snapshot_append_scaled_texture(snap, tex, GSK_SCALING_FILTER_LINEAR, &tr);
        gtk_snapshot_pop(snap);
        gtk_snapshot_pop(snap);
    }
    g_object_unref(tex);
}
static void setup_view_class_init(SetupViewClass* k) { GTK_WIDGET_CLASS(k)->snapshot = setup_view_snapshot; }
static void setup_view_init(SetupView*) {}

// ============================================================================
//  Навігація
// ============================================================================
static std::map<std::string, std::function<GtkWidget*()>> g_builders;
static std::map<std::string, std::function<void()>> g_on_show;
static int page_index(const std::string& p) { for (int i = 0; i < NPAGES; i++) if (p == PAGES[i]) return i; return 0; }
static std::function<bool()> g_validate;          // поточна сторінка: чи можна далі
static void update_nav();
static void show_page(const std::string& p, int dir);
static void start_install();

static void update_nav() {
    int i = page_index(g_page), k = 1;
    gtk_widget_set_visible(g_nav, g_page != "live" && g_page != "done");
    for (GtkWidget* d = gtk_widget_get_first_child(g_dots); d; d = gtk_widget_get_next_sibling(d), k++)
        k == i ? gtk_widget_add_css_class(d, "on") : gtk_widget_remove_css_class(d, "on");
    bool back = g_page != "install" && g_page != "lang" || !g_return_to.empty();
    if (g_page == "lang") back = true;              // з мови — назад на головну
    gtk_widget_set_opacity(g_back, back ? 1 : 0);
    gtk_widget_set_sensitive(g_back, back);
    std::string nx = T("Далі", "Next");
    if (!g_return_to.empty()) nx = T("Готово", "Done");
    else if (g_page == "summary") nx = T("Встановити", "Install");
    else if (g_page == "install") nx = R.running ? T("Згорнути", "Minimize") : R.ok ? T("Далі", "Next") : T("Повторити", "Retry");
    pxbutton_set(g_next, nx);
    bool ok = !g_validate || g_validate();
    gtk_widget_set_sensitive(g_next, ok);
}
static void refresh_nav() { if (g_nav) update_nav(); }

static void show_page(const std::string& p, int dir) {
    GtkWidget* old = gtk_stack_get_child_by_name(GTK_STACK(g_stack), p.c_str());
    g_validate = nullptr;
    GtkWidget* pg = g_builders[p]();
    if (old) gtk_stack_remove(GTK_STACK(g_stack), old);
    gtk_stack_add_named(GTK_STACK(g_stack), pg, p.c_str());
    gtk_stack_set_transition_type(GTK_STACK(g_stack), dir > 0 ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT
                                                    : dir < 0 ? GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT : GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), p.c_str());
    // попередня сторінка більше не потрібна — прибираємо після анімації
    std::string prev = g_page;
    g_page = p;
    if (prev != p) g_timeout_add(500, [](gpointer d) -> gboolean {
        std::string* name = (std::string*)d;
        if (*name != g_page) if (GtkWidget* w = gtk_stack_get_child_by_name(GTK_STACK(g_stack), name->c_str())) gtk_stack_remove(GTK_STACK(g_stack), w);
        delete name; return G_SOURCE_REMOVE;
    }, new std::string(prev));
    if (g_on_show.count(p)) g_on_show[p]();
    update_nav();
}
static void go(int dir) {
    if (!g_return_to.empty()) { std::string r = g_return_to; g_return_to.clear(); show_page(r, -1); return; }
    if (g_page == "install") {
        if (R.running) {   // «Згорнути»: картка ховається, лишається піксельний прогрес унизу
            gtk_widget_set_visible(g_card, FALSE); gtk_widget_set_visible(g_mini, TRUE); return;
        }
        if (dir > 0 && R.ok) { show_page("done", 1); return; }
        if (dir > 0) { start_install(); return; }
    }
    if (g_page == "lang" && dir < 0) { show_page("live", -1); return; }
    if (g_page == "summary" && dir > 0) { show_page("install", 1); start_install(); return; }
    int i = page_index(g_page) + dir;
    if (i < 0 || i >= NPAGES) return;
    show_page(PAGES[i], dir);
}

// заготовка сторінки: заголовок, підпис, тіло
static GtkWidget* page(const std::string& title, const std::string& sub, GtkWidget** body) {
    GtkWidget* b = vbox(0);
    gtk_widget_add_css_class(b, "page");
    gtk_box_append(GTK_BOX(b), label(title, "s-title", 0.5));
    GtkWidget* s = label(sub, "s-sub", 0.5);
    gtk_label_set_wrap(GTK_LABEL(s), TRUE); gtk_label_set_justify(GTK_LABEL(s), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(b), s);
    *body = vbox(10);
    gtk_widget_add_css_class(*body, "s-body");
    gtk_widget_set_halign(*body, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(*body, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(*body, TRUE);
    gtk_box_append(GTK_BOX(b), *body);
    return b;
}
// рядок-кнопка зі списку (мова, мережа, режим диска)
static GtkWidget* row_button(GtkWidget* child, bool sel, std::function<void()> cb) {
    GtkWidget* b = gtk_button_new();
    gtk_widget_add_css_class(b, "srow");
    if (sel) gtk_widget_add_css_class(b, "sel");
    gtk_button_set_child(GTK_BUTTON(b), child);
    gtk_widget_set_cursor_from_name(b, "pointer");
    auto* f = new std::function<void()>(std::move(cb));
    g_signal_connect_data(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { (*(std::function<void()>*)d)(); }), f,
                          [](gpointer d, GClosure*) { delete (std::function<void()>*)d; }, (GConnectFlags)0);
    return b;
}
static GtkWidget* sw_row(const std::string& text, bool on, std::function<void(bool)> cb) {
    GtkWidget* r = hbox(12); gtk_widget_add_css_class(r, "opt");
    GtkWidget* l = label(text, "opt-l"); gtk_widget_set_hexpand(l, TRUE); gtk_box_append(GTK_BOX(r), l);
    GtkWidget* s = gtk_switch_new(); gtk_switch_set_active(GTK_SWITCH(s), on); gtk_widget_set_valign(s, GTK_ALIGN_CENTER);
    auto* f = new std::function<void(bool)>(std::move(cb));
    g_signal_connect_data(s, "notify::active", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer d) {
        (*(std::function<void(bool)>*)d)(gtk_switch_get_active(GTK_SWITCH(o))); }), f,
        [](gpointer d, GClosure*) { delete (std::function<void(bool)>*)d; }, (GConnectFlags)0);
    gtk_box_append(GTK_BOX(r), s);
    return r;
}
static GtkWidget* seg(const std::vector<std::pair<std::string, std::string>>& opts, const std::string& cur, std::function<void(const std::string&)> cb) {
    GtkWidget* s = hbox(4); gtk_widget_add_css_class(s, "seg");
    GtkWidget* first = nullptr;
    for (auto& [id, txt] : opts) {
        GtkWidget* b = gtk_toggle_button_new_with_label(txt.c_str());
        if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(first)); else first = b;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), id == cur);
        g_object_set_data_full(G_OBJECT(b), "id", g_strdup(id.c_str()), g_free);
        auto* f = new std::function<void(const std::string&)>(cb);
        g_signal_connect_data(b, "toggled", G_CALLBACK(+[](GtkToggleButton* t, gpointer d) {
            if (gtk_toggle_button_get_active(t)) (*(std::function<void(const std::string&)>*)d)((const char*)g_object_get_data(G_OBJECT(t), "id"));
        }), f, [](gpointer d, GClosure*) { delete (std::function<void(const std::string&)>*)d; }, (GConnectFlags)0);
        gtk_box_append(GTK_BOX(s), b);
    }
    return s;
}
static GtkWidget* field(const std::string& cap, GtkWidget* entry) {
    GtkWidget* b = vbox(5);
    gtk_box_append(GTK_BOX(b), label(cap, "cap"));
    gtk_box_append(GTK_BOX(b), entry);
    gtk_widget_set_hexpand(b, TRUE);
    return b;
}
static void on_changed(GtkWidget* e, std::function<void(const std::string&)> cb) {
    auto* f = new std::function<void(const std::string&)>(std::move(cb));
    g_signal_connect_data(e, "changed", G_CALLBACK(+[](GtkEditable* ed, gpointer d) {
        (*(std::function<void(const std::string&)>*)d)(gtk_editable_get_text(ed)); }), f,
        [](gpointer d, GClosure*) { delete (std::function<void(const std::string&)>*)d; }, (GConnectFlags)0);
}

// ============================================================================
//  0. Головна (live)
// ============================================================================
static void apply_live_keyboard() {
    if (g_demo) return;    // у демо не чіпаємо розкладку справжньої сесії
    std::string l; for (auto& x : O.layouts) l += (l.empty() ? "" : ",") + x;
    run_bg("hyprctl keyword input:kb_layout " + l + " && hyprctl keyword input:kb_options " + O.toggle);
}
static GtkWidget* build_live() {
    GtkWidget* b = vbox(0);
    gtk_widget_add_css_class(b, "page");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(b), label("Megumin OS", "s-big", 0.5));
    GtkWidget* s = label(T("Ласкаво просимо! Встановіть систему на диск або спершу спробуйте її з флешки.",
                           "Welcome! Install the system to disk or try it from the USB stick first."), "s-sub", 0.5);
    gtk_box_append(GTK_BOX(b), s);
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12); gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(grid, 30);
    struct Tile { const char* ico; const char* uk; const char* en; std::function<void()> fn; bool main; };
    std::vector<Tile> tiles = {
        {"󰋊", "Встановити", "Install", [] { show_page("lang", 1); }, true},
        {"󰖩", "Wi-Fi", "Wi-Fi", [] { g_return_to = "live"; show_page("wifi", 1); }, false},
        {"󰈹", "Firefox", "Firefox", [] { run_bg("firefox"); }, false},
        {"󰋊", "Розмітка дисків", "Disks", [] { run_bg("gnome-disks || gparted"); }, false},
        {"", "Термінал", "Terminal", [] { run_bg("kitty || foot || alacritty"); }, false},
        {"󰍛", "Обладнання", "Hardware", [] { g_return_to = "live"; show_page("drivers", 1); }, false},
        {"󰘦", "Документація", "Docs", [] { run_bg("firefox https://wiki.archlinux.org/title/Installation_guide"); }, false},
        {"󰐥", "Вимкнути", "Power off", [] { if (!g_demo) run_bg("systemctl poweroff"); }, false},
    };
    for (size_t i = 0; i < tiles.size(); i++) {
        GtkWidget* t = vbox(10);
        gtk_widget_set_valign(t, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(t), icon(tiles[i].ico, "tile-ico"));
        gtk_box_append(GTK_BOX(t), label(T(tiles[i].uk, tiles[i].en), "tile-l", 0.5));
        GtkWidget* btn = row_button(t, false, tiles[i].fn);
        gtk_widget_remove_css_class(btn, "srow");
        gtk_widget_add_css_class(btn, "tile");
        if (tiles[i].main) gtk_widget_add_css_class(btn, "main");
        gtk_grid_attach(GTK_GRID(grid), btn, (int)i % 4, (int)i / 4, 1, 1);
    }
    gtk_box_append(GTK_BOX(b), grid);
    // рядок стану: інтернет · прошивка · диск · відеокарта
    std::string st = g_online ? T("Інтернет підключено", "Online") : T("Немає інтернету", "Offline");
    st += g_uefi ? " · UEFI" : " · BIOS";
    if (Disk* D = cur_disk()) st += " · " + human(D->size) + (D->tran == "nvme" ? " NVMe" : "");
    for (auto& h : g_hw) if (h.kind == "gpu" && !h.dev.empty()) { st += " · " + h.dev; break; }
    GtkWidget* sl = label(st, "s-foot", 0.5);
    gtk_widget_set_margin_top(sl, 30);
    gtk_box_append(GTK_BOX(b), sl);
    return b;
}

// ============================================================================
//  1. Мова
// ============================================================================
static GtkWidget* build_lang() {
    GtkWidget* body; GtkWidget* p = page(T("Мова", "Language"), T("Мова системи та установщика. Інтерфейс перекладається одразу.",
                                                                  "System and installer language. The interface switches right away."), &body);
    GtkWidget* se = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(se), T("Пошук мови…", "Search language…"));
    gtk_widget_set_size_request(se, 460, -1);
    gtk_box_append(GTK_BOX(body), se);
    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sc, 460, 330);
    GtkWidget* list = vbox(6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), list);
    gtk_box_append(GTK_BOX(body), sc);
    auto fill = std::make_shared<std::function<void(const std::string&)>>();
    *fill = [list, fill](const std::string& q) {
        clear(list);
        std::vector<std::pair<std::string, std::string>> items;
        std::string ql = g_utf8_strdown(q.c_str(), -1);
        for (auto& L : LANGS) {
            std::string n = g_utf8_strdown(L.name, -1);
            if (q.empty() || n.find(ql) != std::string::npos || std::string(L.loc).find(q) != std::string::npos) items.push_back({L.name, L.loc});
        }
        if (!q.empty()) for (auto& loc : all_locales()) {
            if (items.size() > 40) break;
            bool dup = false; for (auto& it : items) if (it.second == loc) dup = true;
            if (!dup && g_ascii_strncasecmp(loc.c_str(), q.c_str(), q.size()) == 0) items.push_back({loc, loc});
        }
        for (auto& [name, loc] : items) {
            GtkWidget* h = hbox(10);
            gtk_box_append(GTK_BOX(h), label(O.locale == loc ? "󰄬" : "", "chk", 0.5));
            GtkWidget* nl = label(name, "row-l"); gtk_widget_set_hexpand(nl, TRUE); gtk_box_append(GTK_BOX(h), nl);
            gtk_box_append(GTK_BOX(h), label(loc, "row-sub", 1));
            std::string l = loc;
            gtk_box_append(GTK_BOX(list), row_button(h, O.locale == loc, [l, fill] {
                O.locale = O.formats = l;
                std::string ui = l.rfind("uk", 0) == 0 ? "uk" : "en";
                for (auto& L : LANGS) if (l == L.loc && std::find(O.layouts.begin(), O.layouts.end(), L.kb) == O.layouts.end()) {
                    // розкладка мови — другою, після англійської
                    O.layouts = {"us"}; if (std::string(L.kb) != "us") O.layouts.push_back(L.kb);
                }
                apply_live_keyboard();
                if (ui != O.ui) { O.ui = ui; show_page("lang", 0); } else (*fill)("");
            }));
        }
    };
    (*fill)("");
    on_changed(se, [fill](const std::string& q) { (*fill)(q); });
    return p;
}

// ============================================================================
//  2. Регіон і час: піксельна карта поясів
// ============================================================================
static GtkWidget* g_map = nullptr; static GtkWidget* g_tz_dd = nullptr;
static int g_map_hover = -1;
static void map_xy(double lat, double lon, int& x, int& y) {
    x = (int)std::floor((lon + 180) / 360 * MAP_W);
    y = (int)std::floor((MAP_LAT0 - lat) / (MAP_LAT0 - MAP_LAT1) * MAP_H);
}
static const Zone* zone_by_name(const std::string& n) { for (auto& z : zones()) if (z.name == n) return &z; return nullptr; }
static void draw_map(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    const int cell = std::max(3, std::min(w / MAP_W, h / MAP_H));
    const double ox = (w - MAP_W * cell) / 2.0, oy = (h - MAP_H * cell) / 2.0;
    const Zone* sel = zone_by_name(O.tz);
    int sx = -100, sy = -100; if (sel) map_xy(sel->lat, sel->lon, sx, sy);
    // смуга поясу: ±7.5° від середини поясу (за поточним зсувом UTC)
    double off = tz_offset_h(O.tz);
    int bx0 = (int)std::floor((off * 15 - 7.5 + 180) / 360 * MAP_W), bx1 = (int)std::floor((off * 15 + 7.5 + 180) / 360 * MAP_W);
    double hue = g_wv.hue_at(g_t), sat = g_wv.sat();
    for (int y = 0; y < MAP_H; y++) for (int x = 0; x < MAP_W; x++) {
        bool land = MAP_ROWS[y][x] == '1', band = x >= bx0 && x < bx1;
        double d = BAYER4[y & 3][x & 3] / 16.0 - 0.5;
        double dist = std::hypot(x - sx, (y - sy) * 1.1), glow = std::max(0.0, 1 - dist / 14);
        if (land) {
            pw::RGB c = pw::hsl_rgb(hue + x * 0.25, sat * 0.35 + glow * 0.5, 0.2 + d * 0.04 + glow * 0.25 + (band ? 0.06 : 0));
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
        } else if (band) {
            pw::RGB c = pw::hsl_rgb(hue, sat, 0.6);
            cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.05 + (d < -0.2 ? 0.03 : 0));
        } else if ((x + y) % 2 == 0 && d < -0.3) cairo_set_source_rgba(cr, 1, 1, 1, 0.035);
        else continue;
        cairo_rectangle(cr, ox + x * cell, oy + y * cell, cell - 1, cell - 1);
        cairo_fill(cr);
    }
    int i = 0;
    for (auto& z : zones()) {
        int x, y; map_xy(z.lat, z.lon, x, y);
        if (y < 0 || y >= MAP_H) { i++; continue; }
        double a = i == g_map_hover ? 0.95 : 0.45;
        cairo_set_source_rgba(cr, 0.8, 0.92, 1, a);
        int pad = i == g_map_hover ? 0 : 1;
        cairo_rectangle(cr, ox + x * cell + pad, oy + y * cell + pad, cell - 1 - 2 * pad, cell - 1 - 2 * pad);
        cairo_fill(cr); i++;
    }
    if (!sel) return;
    double pulse = 0.5 + 0.5 * std::sin(g_t * 3);
    for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
        int m = std::abs(dx) + std::abs(dy); if (m > 2) continue;
        pw::RGB c = m == 0 ? pw::RGB{1, 1, 1} : pw::hsl_rgb(hue, sat, 0.6 + 0.15 * pulse);
        cairo_set_source_rgba(cr, c.r, c.g, c.b, m == 2 ? 0.55 : 1);
        cairo_rectangle(cr, ox + (sx + dx) * cell, oy + (sy + dy) * cell, cell - 1, cell - 1); cairo_fill(cr);
    }
    // підпис: «Europe/Kyiv · 12:14»
    std::string lbl = O.tz + " · " + tz_now(O.tz);
    for (auto& ch : lbl) if (ch == '_') ch = ' ';
    PangoLayout* pl = pango_cairo_create_layout(cr);
    PangoFontDescription* fd = pango_font_description_from_string("SF Pro Text Semibold 10");
    pango_layout_set_font_description(pl, fd); pango_layout_set_text(pl, lbl.c_str(), -1);
    int tw, th; pango_layout_get_pixel_size(pl, &tw, &th);
    double lx = ox + sx * cell + 16, ly = oy + sy * cell - 30;
    if (lx + tw + 22 > w) lx = ox + sx * cell - tw - 36;
    if (ly < 0) ly = oy + sy * cell + 14;
    cairo_set_source_rgba(cr, 0.03, 0.12, 0.17, 0.92); cairo_rectangle(cr, lx, ly, tw + 20, th + 8); cairo_fill(cr);
    pw::RGB ac = pw::hsl_rgb(hue, sat, 0.6);
    cairo_set_source_rgb(cr, ac.r, ac.g, ac.b); cairo_rectangle(cr, lx, ly, 3, th + 8); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.91, 0.96, 0.99); cairo_move_to(cr, lx + 11, ly + 4); pango_cairo_show_layout(cr, pl);
    pango_font_description_free(fd); g_object_unref(pl);
}
static int map_pick(double px, double py) {
    int w = gtk_widget_get_width(g_map), h = gtk_widget_get_height(g_map);
    const int cell = std::max(3, std::min(w / MAP_W, h / MAP_H));
    const double ox = (w - MAP_W * cell) / 2.0, oy = (h - MAP_H * cell) / 2.0;
    double best = 1e9; int bi = -1, i = 0;
    for (auto& z : zones()) {
        int x, y; map_xy(z.lat, z.lon, x, y);
        double d = std::hypot(ox + (x + 0.5) * cell - px, oy + (y + 0.5) * cell - py);
        if (d < best) { best = d; bi = i; }
        i++;
    }
    return best < cell * 4 ? bi : -1;
}
static void set_tz(const std::string& tz, bool from_dd = false) {
    O.tz = tz;
    if (!from_dd && g_tz_dd) {
        auto& z = zones();
        for (size_t i = 0; i < z.size(); i++) if (z[i].name == tz) { gtk_drop_down_set_selected(GTK_DROP_DOWN(g_tz_dd), (guint)i); break; }
    }
    if (g_map) gtk_widget_queue_draw(g_map);
}
static GtkWidget* build_region() {
    GtkWidget* body; GtkWidget* p = page(T("Регіон і час", "Region and time"),
        T("Часовий пояс визначено автоматично за IP. Формати дат і чисел — окремо.",
          "Time zone was detected from your IP. Date and number formats are separate."), &body);
    weak(&g_map, gtk_drawing_area_new());
    gtk_widget_set_size_request(g_map, 640, 260);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_map), draw_map, nullptr, nullptr);
    gtk_widget_set_cursor_from_name(g_map, "crosshair");
    anim_add(g_map);
    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick*, int, double x, double y, gpointer) {
        int i = map_pick(x, y); if (i >= 0) { O.tz_user = true; set_tz(zones()[i].name); }
    }), nullptr);
    gtk_widget_add_controller(g_map, GTK_EVENT_CONTROLLER(click));
    GtkEventController* mot = gtk_event_controller_motion_new();
    g_signal_connect(mot, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double x, double y, gpointer) { g_map_hover = map_pick(x, y); }), nullptr);
    gtk_widget_add_controller(g_map, mot);
    gtk_box_append(GTK_BOX(body), g_map);

    GtkWidget* row = hbox(16);
    gtk_widget_set_size_request(row, 640, -1);
    GtkStringList* zl = gtk_string_list_new(nullptr);
    for (auto& z : zones()) gtk_string_list_append(zl, z.name.c_str());
    weak(&g_tz_dd, gtk_drop_down_new(G_LIST_MODEL(zl), gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string")));
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(g_tz_dd), TRUE);
    gtk_drop_down_set_search_match_mode(GTK_DROP_DOWN(g_tz_dd), GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
    set_tz(O.tz);
    g_signal_connect(g_tz_dd, "notify::selected", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
        if (i < zones().size() && zones()[i].name != O.tz) { O.tz_user = true; set_tz(zones()[i].name, true); }
    }), nullptr);
    gtk_box_append(GTK_BOX(row), field(T("Часовий пояс", "Time zone"), g_tz_dd));
    // формати: дата й число так, як вони виглядатимуть
    GtkStringList* fl = gtk_string_list_new(nullptr);
    std::vector<std::string> locs; for (auto& L : LANGS) locs.push_back(L.loc);
    if (std::find(locs.begin(), locs.end(), O.formats) == locs.end()) locs.push_back(O.formats);
    guint fsel = 0;
    for (size_t i = 0; i < locs.size(); i++) {
        std::string ex = locs[i].rfind("en_US", 0) == 0 ? "11/14/2026, 1,234.5" : locs[i].rfind("en", 0) == 0 ? "14/11/2026, 1,234.5"
                       : locs[i].rfind("ja", 0) == 0 ? "2026/11/14, 1,234.5" : "14.11.2026, 1 234,5";
        gtk_string_list_append(fl, (lang_name(locs[i]) + " (" + ex + ")").c_str());
        if (locs[i] == O.formats) fsel = (guint)i;
    }
    GtkWidget* fdd = gtk_drop_down_new(G_LIST_MODEL(fl), nullptr);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(fdd), fsel);
    g_object_set_data_full(G_OBJECT(fdd), "locs", new std::vector<std::string>(locs), [](gpointer d) { delete (std::vector<std::string>*)d; });
    g_signal_connect(fdd, "notify::selected", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        auto* v = (std::vector<std::string>*)g_object_get_data(o, "locs");
        guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(o)); if (i < v->size()) O.formats = (*v)[i];
    }), nullptr);
    gtk_box_append(GTK_BOX(row), field(T("Формати", "Formats"), fdd));
    gtk_box_append(GTK_BOX(body), row);
    GtkWidget* ntp = sw_row(T("Синхронізувати час через інтернет (NTP)", "Sync time over the internet (NTP)"), O.ntp, [](bool v) { O.ntp = v; });
    gtk_widget_set_size_request(ntp, 640, -1);
    gtk_box_append(GTK_BOX(body), ntp);
    return p;
}
static void detect_tz() {
    sh_async("curl -fs --max-time 5 https://ipapi.co/timezone", [](const std::string& out, bool ok) {
        std::string tz = trim(out);
        if (ok && !O.tz_user && zone_by_name(tz)) set_tz(tz);
    });
}

// ============================================================================
//  3. Клавіатура
// ============================================================================
static GtkWidget* build_kbd() {
    GtkWidget* body; GtkWidget* p = page(T("Клавіатура", "Keyboard"), T("Розкладки та перемикання між ними. Можна перевірити нижче.",
                                                                       "Layouts and how to switch between them. Try it below."), &body);
    gtk_widget_set_size_request(body, 560, -1);
    GtkWidget* list = vbox(6);
    gtk_box_append(GTK_BOX(body), list);
    auto fill = std::make_shared<std::function<void()>>();
    *fill = [list, fill] {
        clear(list);
        for (size_t i = 0; i < O.layouts.size(); i++) {
            GtkWidget* r = hbox(12); gtk_widget_add_css_class(r, "opt"); gtk_widget_add_css_class(r, "sel");
            gtk_box_append(GTK_BOX(r), label(std::to_string(i + 1), "num", 0.5));
            GtkWidget* l = label(layout_desc(O.layouts[i]), "row-l"); gtk_widget_set_hexpand(l, TRUE); gtk_box_append(GTK_BOX(r), l);
            gtk_box_append(GTK_BOX(r), label(O.layouts[i], "row-sub", 1));
            if (O.layouts.size() > 1) {
                GtkWidget* x = gtk_button_new_with_label("󰅖"); gtk_widget_add_css_class(x, "flat-x");
                size_t idx = i;
                auto* f = new std::function<void()>([idx, fill] { O.layouts.erase(O.layouts.begin() + idx); apply_live_keyboard(); (*fill)(); });
                g_signal_connect_data(x, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { (*(std::function<void()>*)d)(); }), f,
                                      [](gpointer d, GClosure*) { delete (std::function<void()>*)d; }, (GConnectFlags)0);
                gtk_box_append(GTK_BOX(r), x);
            }
            gtk_box_append(GTK_BOX(list), r);
        }
        // «Додати розкладку…» — спливаюче вікно з пошуком
        GtkWidget* addh = hbox(10);
        gtk_box_append(GTK_BOX(addh), label("+", "num", 0.5));
        gtk_box_append(GTK_BOX(addh), label(T("Додати розкладку…", "Add layout…"), "row-l"));
        GtkWidget* add = gtk_menu_button_new();
        gtk_widget_add_css_class(add, "srow");
        gtk_menu_button_set_child(GTK_MENU_BUTTON(add), addh);
        GtkWidget* pop = gtk_popover_new();
        GtkWidget* pb = vbox(6);
        GtkWidget* se = gtk_search_entry_new();
        GtkWidget* sc = gtk_scrolled_window_new();
        gtk_widget_set_size_request(sc, 420, 300);
        GtkWidget* pl = vbox(2);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), pl);
        gtk_box_append(GTK_BOX(pb), se); gtk_box_append(GTK_BOX(pb), sc);
        gtk_popover_set_child(GTK_POPOVER(pop), pb);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(add), pop);
        auto pfill = std::make_shared<std::function<void(const std::string&)>>();
        *pfill = [pl, pop, fill](const std::string& q) {
            clear(pl);
            std::string ql = g_utf8_strdown(q.c_str(), -1); int n = 0;
            for (auto& L : xkb_layouts()) {
                if (n > 60) break;
                std::string d = g_utf8_strdown(L.desc.c_str(), -1);
                if (!q.empty() && d.find(ql) == std::string::npos && L.code.find(ql) != 0) continue;
                if (std::find(O.layouts.begin(), O.layouts.end(), L.code) != O.layouts.end()) continue;
                GtkWidget* h = hbox(8);
                GtkWidget* dl = label(L.desc, "row-l"); gtk_widget_set_hexpand(dl, TRUE); gtk_box_append(GTK_BOX(h), dl);
                gtk_box_append(GTK_BOX(h), label(L.code, "row-sub", 1));
                std::string code = L.code;
                gtk_box_append(GTK_BOX(pl), row_button(h, false, [code, pop, fill] {
                    if (O.layouts.size() < 4) O.layouts.push_back(code);
                    gtk_popover_popdown(GTK_POPOVER(pop)); apply_live_keyboard();
                    g_idle_add([](gpointer d) -> gboolean { (*(*(std::shared_ptr<std::function<void()>>*)d))(); delete (std::shared_ptr<std::function<void()>>*)d; return G_SOURCE_REMOVE; },
                               new std::shared_ptr<std::function<void()>>(fill));
                }));
                n++;
            }
        };
        (*pfill)("");
        on_changed(se, [pfill](const std::string& q) { (*pfill)(q); });
        if (O.layouts.size() < 4) gtk_box_append(GTK_BOX(list), add);
    };
    (*fill)();
    GtkWidget* cap = label(T("Перемикання", "Switch with"), "cap"); gtk_widget_set_margin_top(cap, 8);
    gtk_box_append(GTK_BOX(body), cap);
    gtk_box_append(GTK_BOX(body), seg({{"grp:alt_shift_toggle", "Alt + Shift"}, {"grp:win_space_toggle", "Super + Space"},
                                       {"grp:caps_toggle", "Caps Lock"}, {"grp:ctrl_shift_toggle", "Ctrl + Shift"}},
                                      O.toggle, [](const std::string& v) { O.toggle = v; apply_live_keyboard(); }));
    GtkWidget* test = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(test), T("Привіт, Megumin OS! Hello 123", "Hello, Megumin OS! 123"));
    gtk_box_append(GTK_BOX(body), field(T("Перевірте тут", "Try it here"), test));
    return p;
}

// ============================================================================
//  4. Wi-Fi
// ============================================================================
static GtkWidget *g_wifi_list = nullptr, *g_wifi_status = nullptr;
static void wifi_scan();
static void check_online(std::function<void()> then = nullptr) {
    sh_async("curl -fsI --max-time 5 https://archlinux.org >/dev/null", [then](const std::string&, bool ok) {
        bool changed = g_online != ok;
        g_online = ok; refresh_nav(); if (then) then();
        if (changed && (g_page == "live" || g_page == "summary")) show_page(g_page, 0);   // рядок стану / попередження
    });
}
static void wifi_connect(const std::string& ssid, const std::string& pass) {
    if (g_wifi_status) gtk_label_set_text(GTK_LABEL(g_wifi_status), (Ts("Підключення до «", "Connecting to “") + ssid + T("»…", "”…")).c_str());
    if (g_demo) return;
    char* qs = g_shell_quote(ssid.c_str()); char* qp = g_shell_quote(pass.c_str());
    std::string cmd = "timeout 30 nmcli dev wifi connect " + std::string(qs) + (pass.empty() ? "" : " password " + std::string(qp));
    g_free(qs); g_free(qp);
    sh_async(cmd, [ssid](const std::string&, bool ok) {
        if (g_wifi_status) gtk_label_set_text(GTK_LABEL(g_wifi_status), ok ? (Ts("Підключено до «", "Connected to “") + ssid + T("»", "”")).c_str()
                                                                         : T("Не вдалося — перевірте пароль", "Failed — check the password"));
        if (ok) { wifi_scan(); check_online(); }
    });
}
static void wifi_scan() {
    sh_async("nmcli radio wifi on >/dev/null 2>&1; timeout 12 nmcli -t -f IN-USE,SIGNAL,SECURITY,SSID dev wifi list --rescan auto",
             [](const std::string& out, bool) {
        if (!g_wifi_list || g_page != "wifi") return;
        clear(g_wifi_list);
        std::istringstream ss(out); std::string line; std::set<std::string> seen; int n = 0;
        while (std::getline(ss, line) && n < 6) {
            std::vector<std::string> f; std::string cur;
            for (size_t i = 0; i < line.size(); i++) {
                if (line[i] == '\\' && i + 1 < line.size()) { cur += line[++i]; continue; }
                if (line[i] == ':' && f.size() < 3) { f.push_back(cur); cur.clear(); continue; }
                cur += line[i];
            }
            f.push_back(cur);
            if (f.size() < 4 || f[3].empty() || seen.count(f[3])) continue;
            seen.insert(f[3]); n++;
            bool used = f[0] == "*", sec = !f[2].empty() && f[2] != "--";
            int sig = atoi(f[1].c_str());
            GtkWidget* row = vbox(6);
            GtkWidget* h = hbox(12);
            gtk_box_append(GTK_BOX(h), icon(sig > 66 ? "󰤨" : sig > 40 ? "󰤥" : "󰤟"));
            GtkWidget* nm = label(f[3], "row-l"); gtk_widget_set_hexpand(nm, TRUE); gtk_box_append(GTK_BOX(h), nm);
            if (used) gtk_box_append(GTK_BOX(h), label(T("Підключено", "Connected"), "row-sub", 1));
            else if (sec) gtk_box_append(GTK_BOX(h), icon("󰌾"));
            GtkWidget* rev = gtk_revealer_new();
            GtkWidget* pr = hbox(8);
            GtkWidget* en = gtk_password_entry_new();
            gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(en), TRUE);
            gtk_widget_set_hexpand(en, TRUE);
            std::string ssid = f[3];
            GtkWidget* go_b = pxbutton(T("Підключити", "Connect"), PX_PRIMARY, [ssid, en] { wifi_connect(ssid, gtk_editable_get_text(GTK_EDITABLE(en))); });
            gtk_box_append(GTK_BOX(pr), en); gtk_box_append(GTK_BOX(pr), go_b);
            gtk_revealer_set_child(GTK_REVEALER(rev), pr);
            bool need_pass = sec && !used;
            gtk_box_append(GTK_BOX(row), row_button(h, used, [ssid, rev, need_pass] {
                if (need_pass) gtk_revealer_set_reveal_child(GTK_REVEALER(rev), !gtk_revealer_get_reveal_child(GTK_REVEALER(rev)));
                else wifi_connect(ssid, "");
            }));
            gtk_box_append(GTK_BOX(row), rev);
            g_signal_connect(en, "activate", G_CALLBACK(+[](GtkWidget* e, gpointer d) {
                wifi_connect((const char*)d, gtk_editable_get_text(GTK_EDITABLE(e))); }), g_strdup(ssid.c_str()));
            gtk_box_append(GTK_BOX(g_wifi_list), row);
        }
        if (n == 0) gtk_box_append(GTK_BOX(g_wifi_list), label(T("Мереж Wi-Fi не знайдено", "No Wi-Fi networks found"), "row-sub", 0.5));
    });
}
static GtkWidget* build_wifi() {
    GtkWidget* body; GtkWidget* p = page(T("Підключення до мережі", "Network"),
        T("Потрібне для встановлення: пакети завантажуються з дзеркал Arch. Дротова мережа підключається сама.",
          "Required: packages are downloaded from Arch mirrors. Wired network connects by itself."), &body);
    gtk_widget_set_size_request(body, 460, -1);
    weak(&g_wifi_list, vbox(6));
    gtk_box_append(GTK_BOX(body), g_wifi_list);
    gtk_box_append(GTK_BOX(g_wifi_list), label(T("Пошук мереж…", "Scanning…"), "row-sub", 0.5));
    weak(&g_wifi_status, label(g_online ? T("󰄬 Інтернет є — можна продовжувати", "󰄬 Online — you can continue")
                                   : T("Без інтернету встановлення неможливе", "Installation needs internet"), "s-note", 0.5));
    gtk_box_append(GTK_BOX(body), g_wifi_status);
    g_validate = [] { return g_online || !g_return_to.empty(); };
    return p;
}

// ============================================================================
//  5. Що встановити
// ============================================================================
struct Extra { const char* pkg; const char* name; };
static const Extra EXTRAS[] = {
    {"steam", "Steam"}, {"obs-studio", "OBS Studio"}, {"libreoffice-fresh", "LibreOffice"}, {"discord", "Discord"},
    {"telegram-desktop", "Telegram"}, {"vlc", "VLC"}, {"gimp", "GIMP"}, {"krita", "Krita"}, {"blender", "Blender"},
    {"code", "Code - OSS"}, {"chromium", "Chromium"}, {"thunderbird", "Thunderbird"},
    {"artcraft", "ArtCraft"},   // не в репозиторіях Arch — бекенд ставить Flatpak із getartcraft.com
};
static std::string mgos_preview() {
    for (const char* d : {"/usr/share/mgos/edition-mgos.png", "/opt/mgos/recovery/arch-installer/edition-mgos.png"})
        if (access(d, R_OK) == 0) return d;
    std::string p = g_self_dir + "/edition-mgos.png";
    return access(p.c_str(), R_OK) == 0 ? p : "";
}
static GtkWidget* build_edition() {
    GtkWidget* body; GtkWidget* p = page(T("Що встановити", "What to install"),
        T("Виберіть варіант. Драйвери та базові програми поставляться в будь-якому разі.",
          "Pick an edition. Drivers and basics are installed in any case."), &body);
    GtkWidget* row = hbox(16);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    struct Ed { const char* id; const char* name; const char* uk; const char* en; };
    Ed eds[] = {
        {"arch", "Arch Linux", "Чиста система з терміналом. Без графіки — для тих, хто збере все сам.", "Plain system with a terminal. No GUI — build it yourself."},
        {"hyprland", "Arch + Hyprland", "Тайловий композитор Hyprland зі стандартним конфігом, Waybar, Wofi.", "Hyprland tiling compositor with stock config, Waybar, Wofi."},
        {"mgos", "Arch + Hyprland + mgOS", "Повний Megumin OS: SYSui, Control Center, шпалери, усі налаштування.", "Full Megumin OS: SYSui, Control Center, wallpapers, all settings."},
    };
    GtkWidget* first = nullptr;
    for (auto& e : eds) {
        GtkWidget* c = vbox(10);
        GtkWidget* ov = gtk_overlay_new();
        GtkWidget* pv = vbox(0); gtk_widget_add_css_class(pv, "pv"); gtk_widget_add_css_class(pv, (std::string("pv-") + e.id).c_str());
        gtk_widget_set_size_request(pv, 220, 124); gtk_widget_set_vexpand(pv, FALSE); gtk_widget_set_valign(pv, GTK_ALIGN_START);
        if (std::string(e.id) == "arch") {
            GtkWidget* t = label("[root@archiso ~]# _", "pv-term"); gtk_widget_set_valign(t, GTK_ALIGN_START); gtk_box_append(GTK_BOX(pv), t);
        } else if (std::string(e.id) == "hyprland") {
            GtkWidget* g = hbox(6); gtk_widget_add_css_class(g, "pv-tiles");
            GtkWidget* a = vbox(0); gtk_widget_add_css_class(a, "pv-win"); gtk_widget_set_hexpand(a, TRUE);
            GtkWidget* r = vbox(6); gtk_widget_set_hexpand(r, TRUE);
            for (int i = 0; i < 2; i++) { GtkWidget* w = vbox(0); gtk_widget_add_css_class(w, "pv-win"); gtk_widget_set_vexpand(w, TRUE); gtk_box_append(GTK_BOX(r), w); }
            gtk_box_append(GTK_BOX(g), a); gtk_box_append(GTK_BOX(g), r); gtk_widget_set_vexpand(g, TRUE);
            gtk_box_append(GTK_BOX(pv), g);
        } else {
            std::string img = mgos_preview();
            // готова текстура 220×124: природний розмір прев'ю однаковий у всіх карток
            GdkPixbuf* pb = img.empty() ? nullptr : gdk_pixbuf_new_from_file_at_scale(img.c_str(), 220, 124, FALSE, nullptr);
            if (pb) { GdkTexture* tx = gdk_texture_new_for_pixbuf(pb); g_object_unref(pb);
                                GtkWidget* pic = gtk_picture_new_for_paintable(GDK_PAINTABLE(tx)); g_object_unref(tx);
                                gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER); gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
                                gtk_widget_set_vexpand(pic, TRUE);
                                gtk_widget_set_overflow(pv, GTK_OVERFLOW_HIDDEN); gtk_box_append(GTK_BOX(pv), pic); }
        }
        gtk_overlay_set_child(GTK_OVERLAY(ov), pv);
        if (std::string(e.id) == "mgos") {
            GtkWidget* tag = label(T("Рекомендовано", "Recommended"), "tag", 0.5);
            gtk_widget_set_halign(tag, GTK_ALIGN_END); gtk_widget_set_valign(tag, GTK_ALIGN_START);
            gtk_overlay_add_overlay(GTK_OVERLAY(ov), tag);
        }
        gtk_box_append(GTK_BOX(c), ov);
        gtk_box_append(GTK_BOX(c), label(e.name, "ed-name"));
        GtkWidget* d = label(T(e.uk, e.en), "ed-desc"); gtk_label_set_wrap(GTK_LABEL(d), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(d), 26);
        gtk_box_append(GTK_BOX(c), d);
        GtkWidget* b = gtk_toggle_button_new();
        gtk_widget_add_css_class(b, "edcard");
        gtk_button_set_child(GTK_BUTTON(b), c);
        if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(first)); else first = b;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), O.edition == e.id);
        g_signal_connect_data(b, "toggled", G_CALLBACK(+[](GtkToggleButton* t, gpointer d) {
            if (gtk_toggle_button_get_active(t)) { O.edition = (const char*)d; show_page("edition", 0); }
        }), g_strdup(e.id), (GClosureNotify)(void (*)(void))g_free, (GConnectFlags)0);
        gtk_box_append(GTK_BOX(row), b);
    }
    gtk_box_append(GTK_BOX(body), row);
    if (O.edition != "arch") {
        GtkWidget* h = hbox(10);
        gtk_box_append(GTK_BOX(h), icon("󰏗"));
        GtkWidget* l = label(T("Додаткові програми (Steam, OBS, LibreOffice, Discord…)", "Extra apps (Steam, OBS, LibreOffice, Discord…)"), "row-l");
        gtk_widget_set_hexpand(l, TRUE); gtk_box_append(GTK_BOX(h), l);
        gtk_box_append(GTK_BOX(h), label(std::to_string(O.extras.size()) + T(" вибрано  ›", " selected  ›"), "row-sub", 1));
        GtkWidget* mb = gtk_menu_button_new(); gtk_widget_add_css_class(mb, "srow");
        gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), h);
        GtkWidget* pop = gtk_popover_new();
        GtkWidget* grid = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(grid), 18); gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        int i = 0;
        for (auto& x : EXTRAS) {
            GtkWidget* cb = gtk_check_button_new_with_label(x.name);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), O.extras.count(x.pkg));
            g_signal_connect(cb, "toggled", G_CALLBACK(+[](GtkCheckButton* c, gpointer d) {
                if (gtk_check_button_get_active(c)) O.extras.insert((const char*)d); else O.extras.erase((const char*)d);
            }), (gpointer)x.pkg);
            gtk_grid_attach(GTK_GRID(grid), cb, i % 3, i / 3, 1, 1); i++;
        }
        gtk_popover_set_child(GTK_POPOVER(pop), grid);
        g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover*, gpointer) { g_idle_add([](gpointer) -> gboolean { show_page("edition", 0); return G_SOURCE_REMOVE; }, nullptr); }), nullptr);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
        gtk_widget_set_margin_top(mb, 14);
        gtk_box_append(GTK_BOX(body), mb);
    }
    return p;
}

// ============================================================================
//  6. Диск: вибір, режим, піксельна смуга розділів, ФС, шифрування
// ============================================================================
static std::string g_luks1, g_luks2;
static void draw_disk_bar(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    Disk* D = cur_disk(); if (!D || D->size <= 0) return;
    const int cell = 5, cols = w / cell, rows = std::max(1, h / cell);
    struct Seg { double frac; int kind; };   // 0 — чужий розділ, 1 — EFI/boot, 2 — mgOS, 3 — вільне
    std::vector<Seg> segs;
    double tot = (double)D->size;
    if (O.mode == "erase") { segs.push_back({1.07e9 / tot, 1}); segs.push_back({1 - 1.07e9 / tot, 2}); }
    else {
        for (auto& P : D->parts) {
            bool target = O.mode == "part" && P.path == O.root_part;
            segs.push_back({P.size / tot, target ? 2 : P.esp ? 1 : 0});
        }
        if (D->free > 0) {
            if (O.mode == "free") { segs.push_back({1.07e9 / tot, 1}); segs.push_back({std::max(0.0, (D->free - 1.07e9) / tot), 2}); }
            else segs.push_back({D->free / tot, 3});
        }
    }
    int x = 0; double hue = g_wv.hue_at(g_t), sat = g_wv.sat();
    for (size_t i = 0; i < segs.size(); i++) {
        int wd = i + 1 == segs.size() ? cols - x : std::max(2, (int)std::lround(segs[i].frac * cols));
        for (int lx = 0; lx < wd && x + lx < cols; lx++) for (int y = 0; y < rows; y++) {
            int X = x + lx; double d = BAYER4[y & 3][X & 3] / 16.0 - 0.5;
            pw::RGB c{1, 1, 1}; double a = 1;
            switch (segs[i].kind) {
                case 1: c = pw::hsl_rgb(hue + 40, sat * 0.6, 0.55 + d * 0.05); break;
                case 2: c = pw::hsl_rgb(hue + g_wv.grad((double)X / cols), sat, 0.5 + 0.12 * std::sin(X * 0.2 - g_t * 3 + y * 0.5) + d * 0.06); break;
                case 0: a = 0.22 + d * 0.04; break;
                default: a = 0.06 + (d < -0.25 ? 0.03 : 0);
            }
            cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
            cairo_rectangle(cr, X * cell, y * cell, cell - 1, cell - 1); cairo_fill(cr);
        }
        x += wd + 1;
    }
}
static GtkWidget* build_disk() {
    if (g_disks.empty()) load_disks();
    GtkWidget* body; GtkWidget* p = page(T("Диск", "Disk"), T("Куди встановити систему. Перед стиранням уважно перевірте диск.",
                                                             "Where to install. Double-check the disk before erasing."), &body);
    gtk_widget_set_size_request(body, 600, -1);
    if (g_disks.empty()) {
        gtk_box_append(GTK_BOX(body), label(T("Дисків не знайдено", "No disks found"), "s-note", 0.5));
        g_validate = [] { return false; };
        return p;
    }
    GtkStringList* dl = gtk_string_list_new(nullptr); guint dsel = 0;
    for (size_t i = 0; i < g_disks.size(); i++) {
        auto& D = g_disks[i];
        std::string m = D.model.empty() ? T("Диск", "Disk") : D.model;
        gtk_string_list_append(dl, (std::string(D.tran == "usb" ? "󰕓  " : "󰋊  ") + m + " · " + human(D.size) + " · " + D.path).c_str());
        if (D.path == O.disk) dsel = (guint)i;
    }
    GtkWidget* dd = gtk_drop_down_new(G_LIST_MODEL(dl), nullptr);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), dsel);
    g_signal_connect(dd, "notify::selected", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
        if (i < g_disks.size() && g_disks[i].path != O.disk) {
            O.disk = g_disks[i].path; O.root_part.clear();
            if (O.mode == "free" && !free_mode_ok(&g_disks[i])) O.mode = "erase";
            g_idle_add([](gpointer) -> gboolean { show_page("disk", 0); return G_SOURCE_REMOVE; }, nullptr);
        }
    }), nullptr);
    gtk_box_append(GTK_BOX(body), field(T("Диск", "Disk"), dd));

    Disk* D = cur_disk();
    std::string other = D && D->windows ? T("Поруч з Windows", "Alongside Windows") : T("Поруч з іншою ОС", "Alongside other OS");
    struct M { std::string id, name, sub; bool ok; };
    std::vector<M> modes = {
        {"erase", T("Стерти диск і встановити", "Erase disk and install"), T("усі дані буде видалено", "all data will be deleted"), true},
        {"free", other, free_mode_ok(D) ? Ts("у вільне місце · ", "into free space · ") + human(D->free)
                                        : Ts("потрібно ≥ 25 ГБ вільного місця і розділ EFI", "needs ≥ 25 GB free space and an EFI partition"), free_mode_ok(D)},
        {"part", T("Вручну: вибрати розділ", "Manual: pick a partition"), T("для досвідчених", "for advanced users"), D && g_uefi && D->esp && !D->parts.empty()},
    };
    for (auto& m : modes) {
        GtkWidget* h = hbox(12);
        gtk_box_append(GTK_BOX(h), label(O.mode == m.id ? "󰄯" : "󰄰", "chk", 0.5));
        GtkWidget* l = label(m.name, "row-l"); gtk_widget_set_hexpand(l, TRUE); gtk_box_append(GTK_BOX(h), l);
        gtk_box_append(GTK_BOX(h), label(m.sub, "row-sub", 1));
        std::string id = m.id;
        GtkWidget* b = row_button(h, O.mode == m.id, [id] { O.mode = id; show_page("disk", 0); });
        gtk_widget_set_sensitive(b, m.ok);
        gtk_box_append(GTK_BOX(body), b);
    }
    if (O.mode == "part" && D) {
        GtkStringList* pl = gtk_string_list_new(nullptr);
        auto* paths = new std::vector<std::string>();
        guint psel = GTK_INVALID_LIST_POSITION;
        for (auto& P : D->parts) {
            if (P.esp || P.size < MIN_FREE) continue;
            gtk_string_list_append(pl, (P.path + " · " + human(P.size) + (P.fstype.empty() ? "" : " · " + P.fstype) + (P.label.empty() ? "" : " · " + P.label)).c_str());
            if (P.path == O.root_part) psel = (guint)paths->size();
            paths->push_back(P.path);
        }
        GtkWidget* pd = gtk_drop_down_new(G_LIST_MODEL(pl), nullptr);
        if (psel != GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(GTK_DROP_DOWN(pd), psel);
        else if (!paths->empty()) O.root_part = (*paths)[0];
        g_object_set_data_full(G_OBJECT(pd), "paths", paths, [](gpointer d) { delete (std::vector<std::string>*)d; });
        g_signal_connect(pd, "notify::selected", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
            auto* v = (std::vector<std::string>*)g_object_get_data(o, "paths");
            guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
            if (i < v->size()) { O.root_part = (*v)[i]; gtk_widget_queue_draw(GTK_WIDGET(g_object_get_data(o, "bar"))); }
        }), nullptr);
        gtk_box_append(GTK_BOX(body), field(T("Розділ для системи (буде відформатовано)", "Partition for the system (will be formatted)"), pd));
        g_validate = [] { return !O.root_part.empty(); };
    }
    GtkWidget* cap = label(T("Після встановлення", "After install"), "cap"); gtk_widget_set_margin_top(cap, 6);
    gtk_box_append(GTK_BOX(body), cap);
    GtkWidget* bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(bar, 600, 30);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bar), draw_disk_bar, nullptr, nullptr);
    anim_add(bar);
    gtk_box_append(GTK_BOX(body), bar);
    GtkWidget* lg = hbox(16);
    auto legend = [&](const char* cls, const std::string& t) { GtkWidget* b = hbox(6); GtkWidget* s = vbox(0); gtk_widget_add_css_class(s, "lg"); gtk_widget_add_css_class(s, cls);
                                                                gtk_box_append(GTK_BOX(b), s); gtk_box_append(GTK_BOX(b), label(t, "row-sub")); gtk_box_append(GTK_BOX(lg), b); };
    legend("lg-boot", "EFI"); legend("lg-mgos", Ts("Megumin OS · ", "Megumin OS · ") + O.fs);
    if (O.mode != "erase") { legend("lg-other", T("Наявні розділи", "Existing")); legend("lg-free", T("Вільне", "Free")); }
    gtk_box_append(GTK_BOX(body), lg);
    for (GtkWidget* c = gtk_widget_get_first_child(body); c; c = gtk_widget_get_next_sibling(c))
        if (GTK_IS_DROP_DOWN(c)) g_object_set_data(G_OBJECT(c), "bar", bar);

    GtkWidget* opts = hbox(12);
    gtk_box_append(GTK_BOX(opts), seg({{"btrfs", "btrfs"}, {"ext4", "ext4"}}, O.fs, [](const std::string& v) { O.fs = v; show_page("disk", 0); }));
    GtkWidget* enc = sw_row(T("Шифрування (LUKS)", "Encryption (LUKS)"), O.encrypt, [](bool v) { O.encrypt = v; show_page("disk", 0); });
    gtk_widget_set_hexpand(enc, TRUE);
    gtk_box_append(GTK_BOX(opts), enc);
    GtkWidget* zr = sw_row(T("Swap (zram)", "Swap (zram)"), O.zram, [](bool v) { O.zram = v; });
    gtk_widget_set_hexpand(zr, TRUE);
    gtk_box_append(GTK_BOX(opts), zr);
    gtk_box_append(GTK_BOX(body), opts);
    if (O.encrypt) {
        GtkWidget* pr = hbox(12);
        GtkWidget* e1 = gtk_password_entry_new(); gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(e1), TRUE);
        GtkWidget* e2 = gtk_password_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(e1), g_luks1.c_str()); gtk_editable_set_text(GTK_EDITABLE(e2), g_luks2.c_str());
        on_changed(e1, [](const std::string& v) { g_luks1 = v; refresh_nav(); });
        on_changed(e2, [](const std::string& v) { g_luks2 = v; refresh_nav(); });
        gtk_box_append(GTK_BOX(pr), field(T("Пароль диска", "Disk passphrase"), e1));
        gtk_box_append(GTK_BOX(pr), field(T("Ще раз", "Repeat"), e2));
        gtk_box_append(GTK_BOX(body), pr);
        auto prev = g_validate;
        g_validate = [prev] { return (!prev || prev()) && g_luks1.size() >= 4 && g_luks1 == g_luks2; };
    }
    return p;
}

// ============================================================================
//  7. Обліковий запис
// ============================================================================
static std::string g_pass1, g_pass2;
static GtkWidget *g_user_e, *g_host_e, *g_user_msg, *g_avatar;
static std::string translit(const std::string& s) {
    static const std::map<std::string, std::string> M = {
        {"а","a"},{"б","b"},{"в","v"},{"г","h"},{"ґ","g"},{"д","d"},{"е","e"},{"є","ie"},{"ж","zh"},{"з","z"},{"и","y"},{"і","i"},{"ї","i"},
        {"й","i"},{"к","k"},{"л","l"},{"м","m"},{"н","n"},{"о","o"},{"п","p"},{"р","r"},{"с","s"},{"т","t"},{"у","u"},{"ф","f"},{"х","kh"},
        {"ц","ts"},{"ч","ch"},{"ш","sh"},{"щ","shch"},{"ь",""},{"ю","iu"},{"я","ia"},{"'",""},{"’",""}};
    std::string out; char* low = g_utf8_strdown(s.c_str(), -1);
    for (const char* p = low; *p; p = g_utf8_next_char(p)) {
        std::string ch(p, g_utf8_next_char(p) - p);
        auto it = M.find(ch);
        if (it != M.end()) out += it->second;
        else if (ch.size() == 1 && (isalnum((unsigned char)ch[0]))) out += ch;
        else if (ch == " " || ch == "-" || ch == "_") { if (!out.empty()) break; }   // лише перше слово
    }
    g_free(low);
    while (!out.empty() && isdigit((unsigned char)out[0])) out.erase(0, 1);
    return out.substr(0, 32);
}
static bool user_ok(std::string* why) {
    static const std::regex ure("^[a-z_][a-z0-9_-]{0,31}$"), hre("^[a-zA-Z0-9][a-zA-Z0-9-]{0,62}$");
    static const std::set<std::string> reserved = {"root", "bin", "daemon", "mail", "ftp", "http", "nobody", "dbus", "systemd", "git", "sddm"};
    if (O.username.empty()) { *why = ""; return false; }
    if (!std::regex_match(O.username, ure)) { *why = T("Ім'я користувача: малі латинські літери, цифри, «-» і «_»", "Username: lowercase latin letters, digits, “-”, “_”"); return false; }
    if (reserved.count(O.username)) { *why = T("Це ім'я зайняте системою", "This name is reserved"); return false; }
    if (!std::regex_match(O.hostname, hre)) { *why = T("Назва комп'ютера: латиниця, цифри і «-»", "Computer name: latin letters, digits, “-”"); return false; }
    if (g_pass1.empty()) { *why = ""; return false; }
    if (g_pass1 != g_pass2) { *why = T("Паролі не збігаються", "Passwords don't match"); return false; }
    if (g_pass1.size() < 4) { *why = T("Пароль закороткий (мінімум 4 символи)", "Password too short (4+ characters)"); return false; }
    *why = g_pass1.size() < 8 ? T("󰀦 Простий пароль — краще 8+ символів", "󰀦 Weak password — 8+ characters is better") : "";
    return true;
}
static void user_refresh() {
    std::string why; user_ok(&why);
    if (g_user_msg) gtk_label_set_text(GTK_LABEL(g_user_msg), why.c_str());
    if (g_avatar) {
        std::string src = O.fullname.empty() ? O.username : O.fullname;
        std::string ini = src.empty() ? "?" : std::string(src.c_str(), g_utf8_next_char(src.c_str()) - src.c_str());
        char* up = g_utf8_strup(ini.c_str(), -1); gtk_label_set_text(GTK_LABEL(g_avatar), up); g_free(up);
    }
    refresh_nav();
}
static GtkWidget* build_user() {
    GtkWidget* body; GtkWidget* p = page(T("Обліковий запис", "Account"), T("Ім'я, вхід і пароль. Пароль також використовується для sudo.",
                                                                          "Name, login and password. The password is also used for sudo."), &body);
    gtk_widget_set_size_request(body, 460, -1);
    weak(&g_avatar, label("?", "avatar", 0.5));
    gtk_widget_set_halign(g_avatar, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(body), g_avatar);
    GtkWidget* fn = gtk_entry_new(); gtk_editable_set_text(GTK_EDITABLE(fn), O.fullname.c_str());
    weak(&g_user_e, gtk_entry_new()); gtk_editable_set_text(GTK_EDITABLE(g_user_e), O.username.c_str());
    weak(&g_host_e, gtk_entry_new()); gtk_editable_set_text(GTK_EDITABLE(g_host_e), O.hostname.c_str());
    gtk_box_append(GTK_BOX(body), field(T("Ваше ім'я", "Your name"), fn));
    gtk_box_append(GTK_BOX(body), field(T("Ім'я користувача", "Username"), g_user_e));
    gtk_box_append(GTK_BOX(body), field(T("Назва комп'ютера", "Computer name"), g_host_e));
    on_changed(fn, [](const std::string& v) {
        O.fullname = v;
        if (!O.user_edited) { O.username = translit(v); g_signal_handlers_block_matched(g_user_e, G_SIGNAL_MATCH_ID, g_signal_lookup("changed", GTK_TYPE_EDITABLE), 0, nullptr, nullptr, nullptr);
                              gtk_editable_set_text(GTK_EDITABLE(g_user_e), O.username.c_str());
                              g_signal_handlers_unblock_matched(g_user_e, G_SIGNAL_MATCH_ID, g_signal_lookup("changed", GTK_TYPE_EDITABLE), 0, nullptr, nullptr, nullptr); }
        if (!O.host_edited) { O.hostname = (O.username.empty() ? "mgos" : O.username) + (g_laptop ? "-laptop" : "-pc");
                              g_signal_handlers_block_matched(g_host_e, G_SIGNAL_MATCH_ID, g_signal_lookup("changed", GTK_TYPE_EDITABLE), 0, nullptr, nullptr, nullptr);
                              gtk_editable_set_text(GTK_EDITABLE(g_host_e), O.hostname.c_str());
                              g_signal_handlers_unblock_matched(g_host_e, G_SIGNAL_MATCH_ID, g_signal_lookup("changed", GTK_TYPE_EDITABLE), 0, nullptr, nullptr, nullptr); }
        user_refresh();
    });
    on_changed(g_user_e, [](const std::string& v) { O.username = v; O.user_edited = true; user_refresh(); });
    on_changed(g_host_e, [](const std::string& v) { O.hostname = v; O.host_edited = true; user_refresh(); });
    GtkWidget* pr = hbox(12);
    GtkWidget* p1 = gtk_password_entry_new(); gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(p1), TRUE);
    GtkWidget* p2 = gtk_password_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(p1), g_pass1.c_str()); gtk_editable_set_text(GTK_EDITABLE(p2), g_pass2.c_str());
    on_changed(p1, [](const std::string& v) { g_pass1 = v; user_refresh(); });
    on_changed(p2, [](const std::string& v) { g_pass2 = v; user_refresh(); });
    gtk_box_append(GTK_BOX(pr), field(T("Пароль", "Password"), p1));
    gtk_box_append(GTK_BOX(pr), field(T("Ще раз", "Repeat"), p2));
    gtk_box_append(GTK_BOX(body), pr);
    if (O.edition != "arch")
        gtk_box_append(GTK_BOX(body), sw_row(T("Входити автоматично", "Log in automatically"), O.autologin, [](bool v) { O.autologin = v; }));
    weak(&g_user_msg, label("", "s-note", 0.5));
    gtk_box_append(GTK_BOX(body), g_user_msg);
    g_validate = [] { std::string w; return user_ok(&w); };
    user_refresh();
    return p;
}

// ============================================================================
//  8. Драйвери
// ============================================================================
static GtkWidget* build_drivers() {
    if (g_hw.empty()) load_hw();
    GtkWidget* body; GtkWidget* p = page(T("Драйвери", "Drivers"), T("Обладнання визначено автоматично. Потрібні драйвери встановляться самі.",
                                                                    "Hardware was detected automatically. Needed drivers install by themselves."), &body);
    gtk_widget_set_size_request(body, 640, -1);
    std::map<std::string, std::pair<const char*, const char*>> K = {
        {"gpu", {"Відеокарта", "Graphics"}}, {"dgpu", {"Дискретна", "Discrete GPU"}}, {"cpu", {"Процесор", "CPU"}},
        {"wifi", {"Wi-Fi", "Wi-Fi"}}, {"bt", {"Bluetooth", "Bluetooth"}}, {"laptop", {"Ноутбук", "Laptop"}},
        {"audio", {"Звук", "Audio"}}, {"virt", {"Віртуальна машина", "Virtual machine"}}, {"fw", {"Прошивка", "Firmware"}}};
    bool nvidia = false;
    for (auto& h : g_hw) {
        GtkWidget* r = hbox(12); gtk_widget_add_css_class(r, "opt");
        gtk_box_append(GTK_BOX(r), icon(h.ico.c_str()));
        auto k = K.count(h.kind) ? K[h.kind] : std::make_pair(h.kind.c_str(), h.kind.c_str());
        GtkWidget* kl = label(T(k.first, k.second), "kv-k"); gtk_widget_set_size_request(kl, 140, -1); gtk_box_append(GTK_BOX(r), kl);
        std::string dev = h.dev;
        if (h.kind == "laptop") dev = h.dev.empty() ? T("ні — настільний ПК", "no — desktop PC") : Ts("батарею знайдено · ", "battery found · ") + h.dev;
        if (dev.empty()) dev = T("не знайдено", "not found");
        GtkWidget* dl = label(dev, h.dev.empty() && h.kind != "laptop" ? "row-sub" : "row-l");
        gtk_label_set_ellipsize(GTK_LABEL(dl), PANGO_ELLIPSIZE_END); gtk_widget_set_hexpand(dl, TRUE);
        gtk_box_append(GTK_BOX(r), dl);
        if (!h.pkgs.empty()) {
            std::string first = split(h.pkgs, ' ')[0]; size_t n = split(h.pkgs, ' ').size();
            GtkWidget* chip = label(n > 1 ? first + " +" + std::to_string(n - 1) : first, "chip", 0.5);
            gtk_widget_set_tooltip_text(chip, h.pkgs.c_str());
            gtk_box_append(GTK_BOX(r), chip);
        } else gtk_box_append(GTK_BOX(r), label("—", "row-sub", 1));
        if (h.kind == "dgpu" && !h.dev.empty()) nvidia = true;
        gtk_box_append(GTK_BOX(body), r);
    }
    GtkWidget* note = label(nvidia ? T("Для NVIDIA: відкритий драйвер nvidia-open (Turing і новіші). Гібрид — prime-run для запуску на дискретній.",
                                       "NVIDIA: open driver nvidia-open (Turing and newer). Hybrid — prime-run runs apps on the discrete GPU.")
                                   : T("Відеодрайвери Mesa (Intel/AMD) — у ядрі та mesa; додатково VA-API для апаратного відео.",
                                       "Mesa drivers (Intel/AMD) are in the kernel and mesa; plus VA-API for hardware video."), "s-note", 0.5);
    gtk_label_set_wrap(GTK_LABEL(note), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(note), 80);
    gtk_box_append(GTK_BOX(body), note);
    return p;
}

// ============================================================================
//  9. Перевірка
// ============================================================================
static std::string edition_name() { return O.edition == "arch" ? "Arch Linux" : O.edition == "hyprland" ? "Arch + Hyprland" : "Arch + Hyprland + mgOS"; }
static GtkWidget* build_summary() {
    GtkWidget* body; GtkWidget* p = page(T("Перевірте все", "Review"), T("Після натискання «Встановити» диск буде змінено.",
                                                                        "The disk is changed once you press “Install”."), &body);
    gtk_widget_set_size_request(body, 700, -1);
    Disk* D = cur_disk();
    std::string kb; for (auto& l : O.layouts) kb += (kb.empty() ? "" : ", ") + l;
    std::string tg = O.toggle == "grp:alt_shift_toggle" ? "Alt+Shift" : O.toggle == "grp:win_space_toggle" ? "Super+Space" : O.toggle == "grp:caps_toggle" ? "Caps Lock" : "Ctrl+Shift";
    std::string mode = O.mode == "erase" ? T("стерти", "erase") : O.mode == "free" ? T("у вільне місце", "free space") : O.root_part;
    std::string drv;
    for (auto& h : g_hw) if ((h.kind == "gpu" || h.kind == "dgpu" || h.kind == "cpu") && !h.pkgs.empty()) drv += (drv.empty() ? "" : ", ") + h.pkgs;
    std::string extras; for (auto& e : O.extras) extras += (extras.empty() ? "" : ", ") + e;
    std::vector<std::pair<std::string, std::string>> kv = {
        {T("Варіант", "Edition"), edition_name() + (extras.empty() ? "" : " + " + extras)},
        {T("Мова / регіон", "Language / region"), lang_name(O.locale) + " · " + O.tz},
        {T("Клавіатура", "Keyboard"), kb + " · " + tg},
        {T("Користувач", "User"), O.username + " @ " + O.hostname},
        {T("Диск", "Disk"), (D ? D->path : "?") + " · " + mode + " · " + O.fs + " · " + (O.encrypt ? T("шифрування LUKS", "LUKS encryption") : T("без шифрування", "no encryption"))},
        {T("Драйвери", "Drivers"), drv.empty() ? "—" : drv},
        {T("Завантажувач", "Bootloader"), g_uefi ? "systemd-boot (UEFI)" : "GRUB (BIOS)"},
    };
    for (auto& [k, v] : kv) {
        GtkWidget* r = hbox(12); gtk_widget_add_css_class(r, "opt");
        GtkWidget* kl = label(k, "kv-k"); gtk_widget_set_size_request(kl, 140, -1); gtk_box_append(GTK_BOX(r), kl);
        GtkWidget* vl = label(v, "row-l"); gtk_label_set_ellipsize(GTK_LABEL(vl), PANGO_ELLIPSIZE_END); gtk_widget_set_hexpand(vl, TRUE);
        gtk_box_append(GTK_BOX(r), vl);
        gtk_box_append(GTK_BOX(body), r);
    }
    std::string w;
    if (O.mode == "erase" && D) w = Ts("󰀦  Усі дані на ", "󰀦  All data on ") + (D->model.empty() ? D->path : D->model) + " (" + human(D->size) + T(") буде видалено без можливості відновлення.", ") will be erased permanently.");
    else if (O.mode == "part") w = Ts("󰀦  Розділ ", "󰀦  Partition ") + O.root_part + T(" буде відформатовано.", " will be formatted.");
    else w = T("Наявні розділи не змінюються — система стане у вільне місце.", "Existing partitions stay untouched — the system goes into free space.");
    if (!g_online) w += T("\nНемає інтернету — підключіться до мережі.", "\nNo internet — connect to a network first.");
    GtkWidget* wl = label(w, O.mode == "free" && g_online ? "s-note" : "warn", 0);
    gtk_label_set_wrap(GTK_LABEL(wl), TRUE);
    gtk_widget_set_margin_top(wl, 8);
    gtk_box_append(GTK_BOX(body), wl);
    g_validate = [] { std::string x; return g_online && cur_disk() && user_ok(&x); };
    return p;
}

// ============================================================================
//  10. Встановлення
// ============================================================================
static GtkWidget *g_steps_box, *g_log_view, *g_msg_l, *g_eta_l, *g_pct;
static std::deque<std::string> g_logq;
static GSubprocess* g_proc = nullptr;
static const char* st_glyph(St s) { return s == St::Done ? "󰄬" : s == St::Running ? "󰑮" : s == St::Failed ? "󰅖" : ""; }
static void render_steps() {
    if (!g_steps_box || g_page != "install") return;
    clear(g_steps_box);
    for (auto& id : R.steps) {
        St s = R.st[id];
        GtkWidget* r = hbox(12); gtk_widget_add_css_class(r, "opt"); gtk_widget_add_css_class(r, "step");
        if (s == St::Running) gtk_widget_add_css_class(r, "sel");
        if (s == St::Pending) gtk_widget_add_css_class(r, "dim");
        if (s == St::Failed) gtk_widget_add_css_class(r, "bad");
        gtk_box_append(GTK_BOX(r), label(st_glyph(s), "chk", 0.5));
        gtk_box_append(GTK_BOX(r), label(T(step_def(id)->uk, step_def(id)->en), "row-l"));
        gtk_box_append(GTK_BOX(g_steps_box), r);
    }
}
static void log_render() {
    if (!g_log_view || g_page != "install") return;
    std::string s; size_t n = g_logq.size(), from = n > 7 ? n - 7 : 0;
    for (size_t i = from; i < n; i++) { s += g_logq[i]; if (i + 1 < n) s += "\n"; }
    gtk_label_set_text(GTK_LABEL(g_log_view), s.c_str());
}
static void on_line(const std::string& line) {
    if (line.rfind("@@", 0) != 0) {
        if (!line.empty()) { g_logq.push_back(line); if (g_logq.size() > 400) g_logq.pop_front(); log_render(); }
        return;
    }
    auto sp = line.find(' ');
    std::string k = line.substr(2, sp == std::string::npos ? std::string::npos : sp - 2), v = sp == std::string::npos ? "" : line.substr(sp + 1);
    if (k == "STEP") { if (!R.cur.empty() && R.st[R.cur] == St::Running) R.st[R.cur] = St::Done; R.cur = v; R.sub = 0; R.st[v] = St::Running; render_steps(); }
    else if (k == "DONE") { R.st[v] = St::Done; R.sub = 1; render_steps(); }
    else if (k == "PCT") R.sub = std::clamp(atoi(v.c_str()) / 100.0, 0.0, 1.0);
    else if (k == "MSG") { R.msg = v; if (g_msg_l) gtk_label_set_text(GTK_LABEL(g_msg_l), v.c_str()); }
    else if (k == "WARN") { g_logq.push_back("⚠ " + v); log_render(); }
    else if (k == "FAIL") { R.fail = v; if (!R.cur.empty()) R.st[R.cur] = St::Failed; render_steps(); }
    else if (k == "END") { R.ok = true; for (auto& id : R.steps) R.st[id] = St::Done; render_steps(); }
}
static void read_next(GDataInputStream* in);
static void read_next(GDataInputStream* in) {
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, nullptr, [](GObject* s, GAsyncResult* r, gpointer) {
        gsize n = 0;
        char* l = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(s), r, &n, nullptr);
        if (!l) {   // кінець виводу — процес завершився
            R.running = false;
            if (!R.ok && R.fail.empty()) R.fail = T("Інсталятор несподівано завершився", "Installer exited unexpectedly");
            if (!R.fail.empty() && g_msg_l) gtk_label_set_text(GTK_LABEL(g_msg_l), ("󰅖  " + R.fail).c_str());
            if (R.ok && g_msg_l) gtk_label_set_text(GTK_LABEL(g_msg_l), T("Готово!", "Done!"));
            gtk_widget_set_visible(g_card, TRUE); gtk_widget_set_visible(g_mini, FALSE);
            refresh_nav();
            g_object_unref(s);
            if (R.ok && g_page == "install") g_timeout_add(1200, [](gpointer) -> gboolean { if (g_page == "install") show_page("done", 1); return G_SOURCE_REMOVE; }, nullptr);
            return;
        }
        on_line(l); g_free(l);
        read_next(G_DATA_INPUT_STREAM(s));
    }, nullptr);
}
static std::string q(const std::string& s) { char* x = g_shell_quote(s.c_str()); std::string r = x; g_free(x); return r; }
static void start_install() {
    if (R.running) return;
    R = Run();
    R.steps = {"prepare", "disk", "base", "drivers", "system"};
    if (O.edition != "arch") R.steps.push_back("desktop");
    if (O.edition == "mgos") R.steps.push_back("mgos");
    R.steps.push_back("finish");
    for (auto& s : R.steps) R.st[s] = St::Pending;
    R.running = true; R.started = g_get_monotonic_time();
    g_logq.clear();
    // install.conf — 0600 у runtime-теці користувача; бекенд знищує його одразу після читання
    std::string dir = g_get_user_runtime_dir(), conf = dir + "/mgos-install.conf";
    std::string loc_extra = O.formats != O.locale ? O.formats : "";
    if (O.locale.rfind("en_US", 0) != 0 && O.formats.rfind("en_US", 0) != 0) loc_extra += (loc_extra.empty() ? "" : " ") + std::string("en_US.UTF-8");
    std::string kb; for (auto& l : O.layouts) kb += (kb.empty() ? "" : ",") + l;
    std::string extras; for (auto& e : O.extras) extras += (extras.empty() ? "" : " ") + e;
    std::string mgsrc = "/opt/mgos/recovery";
    if (access((mgsrc + "/gui").c_str(), F_OK) != 0) mgsrc = fs::path(g_self_dir).parent_path().string();
    {
        int fd = open(conf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        std::string c;
        c += "EDITION=" + q(O.edition) + "\nDISK=" + q(O.disk) + "\nMODE=" + q(O.mode) + "\nROOT_PART=" + q(O.root_part) + "\n";
        c += "FS=" + q(O.fs) + "\nENCRYPT=" + std::string(O.encrypt ? "1" : "0") + "\nLUKS_PASS=" + q(g_luks1) + "\nSWAP_ZRAM=" + (O.zram ? "1" : "0") + "\n";
        c += "LOCALE=" + q(O.locale) + "\nEXTRA_LOCALES=" + q(loc_extra) + "\nTIMEZONE=" + q(O.tz) + "\nNTP=" + (O.ntp ? "1" : "0") + "\n";
        c += "KB_LAYOUTS=" + q(kb) + "\nKB_TOGGLE=" + q(O.toggle) + "\n";
        c += "FULLNAME=" + q(O.fullname) + "\nUSERNAME=" + q(O.username) + "\nUSER_PASS=" + q(g_pass1) + "\nHOSTNAME=" + q(O.hostname) + "\n";
        c += "AUTOLOGIN=" + std::string(O.autologin ? "1" : "0") + "\nEXTRA_PKGS=" + q(extras) + "\nMGOS_SRC=" + q(mgsrc) + "\n";
        if (g_demo) c += "DRY_RUN=1\nDRY_DELAY=0.12\n";
        if (fd >= 0) { if (write(fd, c.data(), c.size()) < 0) {} close(fd); }
    }
    std::vector<const char*> argv;
    std::string be = backend_path();
    if (g_demo) { argv = {"bash", be.c_str(), conf.c_str(), nullptr}; }
    else argv = {"sudo", "-n", be.c_str(), conf.c_str(), nullptr};
    GError* err = nullptr;
    g_proc = g_subprocess_newv(argv.data(), (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE), &err);
    if (!g_proc) { R.running = false; R.fail = err ? err->message : "spawn"; if (err) g_error_free(err); refresh_nav(); return; }
    GDataInputStream* in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(g_proc));
    read_next(in);
    if (g_page == "install") { render_steps(); refresh_nav(); }
}
static GtkWidget* build_install() {
    GtkWidget* body; GtkWidget* p = page(T("Встановлення", "Installing"),
        T("Можна користуватися Firefox і Wi-Fi, поки йде встановлення. Займе 10–20 хвилин.",
          "Feel free to use Firefox while it installs. Takes 10–20 minutes."), &body);
    gtk_widget_set_size_request(body, 600, -1);
    weak(&g_pct, gtk_drawing_area_new());
    gtk_widget_set_size_request(g_pct, 240, 60);
    gtk_widget_set_halign(g_pct, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_pct), [](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
        draw_pixel_text(cr, std::to_string((int)std::lround(R.shown * 100)) + "%", w, h, 7);
    }, nullptr, nullptr);
    anim_add(g_pct);
    gtk_box_append(GTK_BOX(body), g_pct);
    GtkWidget* bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(bar, 600, 30);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bar), draw_pbar, nullptr, nullptr);
    anim_add(bar);
    gtk_box_append(GTK_BOX(body), bar);
    GtkWidget* mr = hbox(8);
    weak(&g_msg_l, label(R.msg, "row-sub")); gtk_label_set_ellipsize(GTK_LABEL(g_msg_l), PANGO_ELLIPSIZE_END); gtk_widget_set_hexpand(g_msg_l, TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(g_msg_l), 60);
    weak(&g_eta_l, label("", "row-sub", 1));
    gtk_box_append(GTK_BOX(mr), g_msg_l); gtk_box_append(GTK_BOX(mr), g_eta_l);
    gtk_box_append(GTK_BOX(body), mr);
    weak(&g_steps_box, vbox(6));
    gtk_box_append(GTK_BOX(body), g_steps_box);
    weak(&g_log_view, label("", "log"));
    gtk_label_set_max_width_chars(GTK_LABEL(g_log_view), 70); gtk_label_set_width_chars(GTK_LABEL(g_log_view), 70);
    gtk_label_set_yalign(GTK_LABEL(g_log_view), 0);
    gtk_label_set_ellipsize(GTK_LABEL(g_log_view), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(g_log_view, 600, 140);
    gtk_box_append(GTK_BOX(body), g_log_view);
    render_steps(); log_render();
    if (!R.fail.empty()) gtk_label_set_text(GTK_LABEL(g_msg_l), ("󰅖  " + R.fail).c_str());
    g_validate = [] { return true; };
    return p;
}

// ============================================================================
//  11. Готово
// ============================================================================
static GtkWidget* build_done() {
    GtkWidget* b = vbox(16);
    gtk_widget_add_css_class(b, "page");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    GtkWidget* ok = label("󰄬", "done-ico", 0.5); gtk_widget_set_halign(ok, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(b), ok);
    std::string what = O.edition == "mgos" ? "Megumin OS" : O.edition == "hyprland" ? "Arch + Hyprland" : "Arch Linux";
    gtk_box_append(GTK_BOX(b), label(what + T(" встановлено", " is installed"), "s-big2", 0.5));
    GtkWidget* s = label(O.edition == "mgos" ? T("Вийміть флешку і перезавантажте комп'ютер.\nПісля входу вас зустріне Welcome — вигляд, шпалери, робочий стіл.",
                                                 "Remove the USB stick and restart.\nAfter login, Welcome will help with look, wallpapers and desktop.")
                                             : T("Вийміть флешку і перезавантажте комп'ютер.", "Remove the USB stick and restart."), "s-sub", 0.5);
    gtk_label_set_justify(GTK_LABEL(s), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(b), s);
    GtkWidget* row = hbox(12); gtk_widget_set_halign(row, GTK_ALIGN_CENTER); gtk_widget_set_margin_top(row, 20);
    gtk_box_append(GTK_BOX(row), pxbutton(T("Залишитися в live", "Stay in live"), PX_NEUTRAL, [] { show_page("live", -1); }));
    gtk_box_append(GTK_BOX(row), pxbutton(T("Перезавантажити", "Restart"), PX_PRIMARY, [] { if (!g_demo) run_bg("systemctl reboot"); }));
    gtk_box_append(GTK_BOX(b), row);
    return b;
}

// ============================================================================
//  Стилі
// ============================================================================
static const char* CSS = R"(
window { background: transparent; }
.s-ui { color: #eef3f8; font-family: "SF Pro Text", "Inter", sans-serif; }
.s-card { background: rgba(10,14,24,0.55); border-radius: 30px; padding: 46px 56px 30px 56px;
          border: 1px solid rgba(255,255,255,0.08); box-shadow: 0 30px 80px rgba(0,0,0,0.55); }
.s-title { font-family: "SF Pro Display"; font-size: 30px; font-weight: 700; margin-bottom: 8px; }
.s-big { font-family: "SF Pro Display"; font-size: 52px; font-weight: 800; margin-bottom: 14px; }
.s-big2 { font-family: "SF Pro Display"; font-size: 40px; font-weight: 800; }
.s-sub { font-size: 14px; color: rgba(238,243,248,0.72); }
.s-foot, .s-note { font-size: 12px; color: rgba(238,243,248,0.5); }
.s-body { margin-top: 22px; }
.cap { font-size: 12px; color: rgba(238,243,248,0.55); }
.row-l { font-size: 14px; }
.row-sub { font-size: 12px; color: rgba(238,243,248,0.55); }
.kv-k { font-size: 13px; color: rgba(238,243,248,0.55); }
.ico, .chk { font-family: "JetBrainsMono Nerd Font Propo"; font-size: 15px; min-width: 18px; color: @ACCENT@; }
.srow, .opt { background: rgba(255,255,255,0.045); border: 1px solid rgba(255,255,255,0.07); border-radius: 12px; padding: 10px 16px; color: inherit; min-height: 22px; box-shadow: none; }
.srow:hover { background: rgba(255,255,255,0.08); }
.srow.sel, .opt.sel { background: alpha(@ACCENT@, 0.16); border-color: alpha(@ACCENT@, 0.55); }
.srow:disabled { opacity: 0.45; }
.opt.step { padding: 7px 16px; }
.opt.dim { color: rgba(238,243,248,0.45); }
.opt.bad { border-color: rgba(255,90,110,0.6); background: rgba(255,90,110,0.12); }
.num { font-family: "JetBrainsMono Nerd Font Propo"; font-size: 12px; min-width: 22px; min-height: 22px; border-radius: 6px; background: alpha(@ACCENT@, 0.2); color: @ACCENT@; }
.flat-x { background: transparent; border: none; box-shadow: none; color: rgba(238,243,248,0.5); font-family: "JetBrainsMono Nerd Font Propo"; padding: 0 4px; min-height: 0; }
.tile { min-width: 112px; min-height: 104px; border-radius: 16px; background: rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.07); color: inherit; box-shadow: none; }
.tile:hover { background: rgba(255,255,255,0.10); }
.tile.main { background: @ACCENT@; color: #06202b; font-weight: 700; }
.tile-ico { font-family: "JetBrainsMono Nerd Font Propo"; font-size: 26px; }
.tile-l { font-size: 13px; }
.seg { background: rgba(255,255,255,0.06); border-radius: 12px; padding: 4px; }
.seg > button { background: transparent; border: none; border-radius: 9px; padding: 6px 16px; font-size: 13px; color: rgba(238,243,248,0.7); box-shadow: none; }
.seg > button:checked { background: @ACCENT@; color: #06202b; font-weight: 600; }
entry, passwordentry, dropdown > button, searchentry { background: rgba(255,255,255,0.06); border: 1px solid rgba(255,255,255,0.08); border-radius: 12px; min-height: 40px; color: inherit; box-shadow: none; padding: 0 12px; }
entry:focus-within, passwordentry:focus-within { border-color: alpha(@ACCENT@, 0.8); }
switch:checked { background: @ACCENT@; }
.pxbtn { background: transparent; border: none; box-shadow: none; padding: 0; min-height: 42px; }
.pxlbl { padding: 0 22px; font-weight: 600; font-size: 15px; color: #eef3f8; }
.pxbtn.pri .pxlbl { color: #ffffff; text-shadow: 0 1px 2px rgba(0,0,0,0.55); font-weight: 700; }
.dot { min-width: 8px; min-height: 8px; border-radius: 0; background: rgba(255,255,255,0.25); transition: all 300ms ease; }
.dot.on { min-width: 22px; background: @ACCENT@; }
.edcard { background: rgba(255,255,255,0.045); border: 2px solid transparent; border-radius: 18px; padding: 14px; color: inherit; box-shadow: none; }
.edcard:hover { background: rgba(255,255,255,0.08); }
.edcard:checked { border-color: @ACCENT@; background: alpha(@ACCENT@, 0.12); }
.pv { border-radius: 10px; background: #070a10; border: 1px solid rgba(255,255,255,0.08); padding: 10px; }
.pv-term { font-family: "JetBrainsMono Nerd Font"; font-size: 11px; color: #c8d3df; }
.pv-hyprland { background: linear-gradient(135deg, #1a1d3a, #0b0d1a); }
.pv-win { border-radius: 6px; border: 2px solid alpha(@ACCENT@, 0.8); background: rgba(255,255,255,0.05); }
.pv-mgos { padding: 0; }
.tag { font-size: 11px; font-weight: 700; padding: 3px 8px; margin: 6px; border-radius: 6px; background: @ACCENT@; color: #06202b; }
.ed-name { font-size: 14px; font-weight: 700; }
.ed-desc { font-size: 12px; color: rgba(238,243,248,0.6); }
.chip { font-size: 12px; padding: 2px 9px; border-radius: 6px; background: alpha(@ACCENT@, 0.15); color: @ACCENT@; }
.warn { padding: 12px 16px; border-radius: 12px; background: rgba(255,170,60,0.10); border: 1px solid rgba(255,170,60,0.35); color: #ffcf8a; font-size: 13px; }
.avatar { min-width: 72px; min-height: 72px; border-radius: 99px; background: linear-gradient(135deg, @ACCENT@, alpha(@ACCENT@, 0.55)); color: #06202b; font-family: "SF Pro Display"; font-size: 30px; font-weight: 800; }
.log { font-family: "JetBrainsMono Nerd Font"; font-size: 11px; color: #8fa0b3; background: rgba(0,0,0,0.35); border: 1px solid rgba(255,255,255,0.07); border-radius: 12px; padding: 10px 14px; }
.lg { min-width: 10px; min-height: 10px; }
.lg-boot { background: #e0b46a; } .lg-mgos { background: @ACCENT@; } .lg-other { background: rgba(255,255,255,0.3); } .lg-free { background: rgba(255,255,255,0.08); }
.done-ico { font-family: "JetBrainsMono Nerd Font Propo"; font-size: 48px; min-width: 96px; min-height: 96px; border-radius: 99px; background: @ACCENT@; color: #06202b; }
.mini { background: rgba(10,14,24,0.7); border-radius: 14px; padding: 8px 14px; border: 1px solid rgba(255,255,255,0.08); }
menubutton > button { background: transparent; border: none; box-shadow: none; padding: 0; min-height: 0; color: inherit; }
menubutton.srow { padding: 0; }
menubutton.srow > button { padding: 10px 16px; border-radius: 12px; }
menubutton.srow > button:hover { background: rgba(255,255,255,0.05); }
popover > contents { background: rgba(16,20,30,0.97); border-radius: 14px; color: #eef3f8; }
scrolledwindow, viewport { background: transparent; }
)";

// ============================================================================
//  main
// ============================================================================
static void save_shot() {
    int W = gtk_widget_get_width(g_win), H = gtk_widget_get_height(g_win);
    // прихована робоча область не шле кадрів — розкладку робимо самі
    GtkAllocation al = {0, 0, W, H};
    gtk_widget_size_allocate(gtk_window_get_child(GTK_WINDOW(g_win)), &al, -1);
    GtkSnapshot* s = gtk_snapshot_new();
    GTK_WIDGET_GET_CLASS(g_win)->snapshot(g_win, s);   // рендер усього вікна без компостора
    GskRenderNode* node = gtk_snapshot_free_to_node(s);
    fprintf(stderr, "shot: %dx%d node=%p renderer=%p\n", W, H, (void*)node, (void*)gtk_native_get_renderer(GTK_NATIVE(g_win)));
    if (node) {
        GskRenderer* r = gtk_native_get_renderer(GTK_NATIVE(g_win));
        graphene_rect_t vp = GRAPHENE_RECT_INIT(0, 0, (float)W, (float)H);
        GdkTexture* tex = gsk_renderer_render_texture(r, node, &vp);
        gboolean ok = gdk_texture_save_to_png(tex, g_shot_out.c_str());
        fprintf(stderr, "shot: saved=%d %s\n", ok, g_shot_out.c_str());
        g_object_unref(tex); gsk_render_node_unref(node);
    }
}

static void on_activate(GtkApplication* app, gpointer) {
        sysui::install_shared_style();
        g_wv.style = "aurora";
        g_wv.set_accent(sysui::accent());
        if (sysui::settings().accent_mode != "custom") g_wv.set_palette_css(sysui::wal_css_path());
        g_wv.fade_from = 2;
        sysui::live_css(CSS);

        g_win = gtk_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(g_win), "Megumin OS Setup");
        if (!g_window && gtk_layer_is_supported()) {
            // шар BOTTOM на весь екран: інсталятор — як робочий стіл; Firefox/термінал відкриваються над ним
            gtk_layer_init_for_window(GTK_WINDOW(g_win));
            gtk_layer_set_namespace(GTK_WINDOW(g_win), "mgos_setup");
            gtk_layer_set_layer(GTK_WINDOW(g_win), GTK_LAYER_SHELL_LAYER_BOTTOM);
            for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
                gtk_layer_set_anchor(GTK_WINDOW(g_win), e, TRUE);
            gtk_layer_set_exclusive_zone(GTK_WINDOW(g_win), -1);
            gtk_layer_set_keyboard_mode(GTK_WINDOW(g_win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        } else gtk_window_set_default_size(GTK_WINDOW(g_win), 1920, 1080);

        GtkWidget* ov = gtk_overlay_new();
        g_area = (GtkWidget*)g_object_new(setup_view_get_type(), nullptr);
        gtk_widget_set_hexpand(g_area, TRUE); gtk_widget_set_vexpand(g_area, TRUE);
        gtk_overlay_set_child(GTK_OVERLAY(ov), g_area);

        g_ui = vbox(0);
        gtk_widget_add_css_class(g_ui, "s-ui");
        gtk_widget_set_halign(g_ui, GTK_ALIGN_CENTER); gtk_widget_set_valign(g_ui, GTK_ALIGN_CENTER);
        g_card = vbox(18);
        gtk_widget_add_css_class(g_card, "s-card");
        gtk_widget_set_size_request(g_card, 880, 760);
        g_stack = gtk_stack_new();
        gtk_stack_set_transition_duration(GTK_STACK(g_stack), 380);
        gtk_widget_set_vexpand(g_stack, TRUE);
        gtk_box_append(GTK_BOX(g_card), g_stack);
        g_nav = gtk_center_box_new();
        g_back = pxbutton(T("Назад", "Back"), PX_NEUTRAL, [] { go(-1); });
        g_next = pxbutton(T("Далі", "Next"), PX_PRIMARY, [] { go(+1); });
        g_dots = hbox(6); gtk_widget_set_valign(g_dots, GTK_ALIGN_CENTER);
        for (int i = 1; i < NPAGES; i++) { GtkWidget* d = hbox(0); gtk_widget_add_css_class(d, "dot"); gtk_box_append(GTK_BOX(g_dots), d); }
        gtk_center_box_set_start_widget(GTK_CENTER_BOX(g_nav), g_back);
        gtk_center_box_set_center_widget(GTK_CENTER_BOX(g_nav), g_dots);
        gtk_center_box_set_end_widget(GTK_CENTER_BOX(g_nav), g_next);
        gtk_box_append(GTK_BOX(g_card), g_nav);
        gtk_box_append(GTK_BOX(g_ui), g_card);
        gtk_overlay_add_overlay(GTK_OVERLAY(ov), g_ui);

        // «згорнуто»: маленький піксельний прогрес унизу, клік — розгорнути
        g_mini = gtk_button_new();
        gtk_widget_add_css_class(g_mini, "mini");
        GtkWidget* mh = hbox(10);
        GtkWidget* mp = gtk_drawing_area_new(); gtk_widget_set_size_request(mp, 110, 30);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(mp), [](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
            draw_pixel_text(cr, std::to_string((int)std::lround(R.shown * 100)) + "%", w, h, 3); }, nullptr, nullptr);
        anim_add(mp);
        GtkWidget* mb = gtk_drawing_area_new(); gtk_widget_set_size_request(mb, 300, 12);
        gtk_widget_set_valign(mb, GTK_ALIGN_CENTER);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(mb), draw_pbar, nullptr, nullptr);
        anim_add(mb);
        gtk_box_append(GTK_BOX(mh), mp); gtk_box_append(GTK_BOX(mh), mb);
        gtk_button_set_child(GTK_BUTTON(g_mini), mh);
        gtk_widget_set_halign(g_mini, GTK_ALIGN_CENTER); gtk_widget_set_valign(g_mini, GTK_ALIGN_END);
        gtk_widget_set_margin_bottom(g_mini, 40);
        gtk_widget_set_visible(g_mini, FALSE);
        g_signal_connect(g_mini, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { gtk_widget_set_visible(g_card, TRUE); gtk_widget_set_visible(g_mini, FALSE); }), nullptr);
        gtk_overlay_add_overlay(GTK_OVERLAY(ov), g_mini);
        gtk_window_set_child(GTK_WINDOW(g_win), ov);

        GtkEventController* keys = gtk_event_controller_key_new();
        g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint kv, guint, GdkModifierType, gpointer) -> gboolean {
            if (kv == GDK_KEY_Escape && g_demo) { gtk_window_destroy(GTK_WINDOW(g_win)); return TRUE; }
            return FALSE;
        }), nullptr);
        gtk_widget_add_controller(g_win, keys);

        g_t0 = g_get_monotonic_time();
        bool run_test = g_shot_page == "install-run";   // перевірка: справжній бекенд у DRY_RUN, знімок посеред процесу
        show_page(g_shot_page.empty() ? "live" : run_test ? "summary" : g_shot_page, 0);
        if (run_test) { g_online = true; show_page("install", 1); start_install(); }
        if (g_shot_page == "install") {   // знімок прогресу: зупиняємось на «пакетах mgOS»
            R.steps = {"prepare", "disk", "base", "drivers", "system", "desktop", "mgos", "finish"};
            for (auto& s : R.steps) R.st[s] = St::Done;
            R.st["mgos"] = St::Running; R.st["finish"] = St::Pending; R.cur = "mgos"; R.sub = 0.18; R.running = true;
            R.shown = run_fraction();
            g_logq = {"[dry chroot] runuser -u doki -- … step_core", ":: installing hyprland (0.53.1-1)...", ":: installing gtk4-layer-shell (1.1.1-1)...",
                     ":: building SYSui: topbar dock cc_osd welcome ...", ":: copying configs → /home/doki/.config", ":: wal -q -i arona-cloudscape.png"};
            R.msg = "mgOS: stable · installing gtk4-layer-shell (184 / 296)";
            show_page("install", 0);
        }
        gtk_window_present(GTK_WINDOW(g_win));
        if (!g_shot_page.empty()) {
            g_timeout_add(g_shot_page == "install-run" ? 9000 : 1800, [](gpointer) -> gboolean { save_shot(); g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(g_win)))); return G_SOURCE_REMOVE; }, nullptr);
        }
        check_online();
        detect_tz();

        // ~30 кадрів/с: час, анімації пікселів, плавний прогрес
        g_timeout_add(33, [](gpointer) -> gboolean {
            g_t = (g_get_monotonic_time() - g_t0) / 1e6 + 2.0;
            double tgt = run_fraction();
            if (!g_shot_page.empty() && g_shot_page == "install") tgt = R.shown;
            R.shown += (tgt - R.shown) * 0.08;
            if (std::abs(tgt - R.shown) < 0.0005) R.shown = tgt;
            if (g_eta_l && g_page == "install" && R.running && R.shown > 0.03) {
                double el = (g_get_monotonic_time() - R.started) / 1e6, left = el / R.shown * (1 - R.shown);
                gtk_label_set_text(GTK_LABEL(g_eta_l), ("≈ " + std::to_string((int)std::ceil(left / 60)) + T(" хв", " min")).c_str());
            }
            gtk_widget_queue_draw(g_area);
            for (GtkWidget* a : std::vector<GtkWidget*>(g_anim)) if (gtk_widget_get_mapped(a)) gtk_widget_queue_draw(a);
            return G_SOURCE_CONTINUE;
        }, nullptr);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--demo") g_demo = true;
        else if (a == "--window") g_window = true;
        else if (a == "--shot" && i + 2 < argc) { g_shot_page = argv[++i]; g_shot_out = argv[++i]; g_window = true; }
        else if (a == "--en") O.ui = "en";
    }
    if (!g_shot_page.empty() && !g_demo) { fprintf(stderr, "--shot лише разом з --demo\n"); return 2; }
    g_self_dir = fs::path(fs::canonical("/proc/self/exe")).parent_path().string();
    g_builders = {{"live", build_live}, {"lang", build_lang}, {"region", build_region}, {"kbd", build_kbd}, {"wifi", build_wifi},
                  {"edition", build_edition}, {"disk", build_disk}, {"user", build_user}, {"drivers", build_drivers},
                  {"summary", build_summary}, {"install", build_install}, {"done", build_done}};
    g_on_show["wifi"] = [] { wifi_scan(); check_online([] { if (g_wifi_status && g_online && g_page == "wifi")
        gtk_label_set_text(GTK_LABEL(g_wifi_status), T("󰄬 Інтернет є — можна продовжувати", "󰄬 Online — you can continue")); }); };
    load_hw(); load_disks();
    if (g_demo) {   // для знімків — правдоподібні дані замість порожніх полів
        O.fullname = "Doki"; O.username = "doki"; O.hostname = g_laptop ? "doki-laptop" : "doki-pc"; g_pass1 = g_pass2 = "megumin1";
    }

    GtkApplication* app = gtk_application_new("com.megumin.setup", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int r = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return r;
}
