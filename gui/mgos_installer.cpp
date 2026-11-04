// mgos_installer.cpp — графічний інсталятор Megumin OS 6.
//
// Режими: Встановлення · Відновлення · Оновлення пакетів mgOS.
// Логіка кроків — у steps.sh (bash, ідемпотентні функції step_<id>), тут — UI,
// виконання, прогрес і ЗБЕРЕЖЕННЯ ПРОГРЕСУ: після кожного кроку стан пишеться в
// ~/.local/state/mgos-installer/state.ini, тож після збою, закриття чи
// перезавантаження інсталятор продовжує з того кроку, де зупинився.
//
// Пароль sudo живе лише в пам'яті процесу: кроки отримують його через SUDO_ASKPASS
// (обгортка sudo → sudo -A у тимчасовій теці 0700 в $XDG_RUNTIME_DIR), на диск не пишеться.
#include <gtk/gtk.h>
#include "pixel_waves.h"
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
//  Кроки
// ============================================================================
struct StepDef { const char* id; const char* title; const char* icon; double weight; bool fatal; };
static const StepDef STEPS[] = {
    {"preflight",   "Перевірка системи",          "󰄬", 1,  true},
    {"snapshot",    "Точка відкату",              "󰁯", 1,  true},
    {"yay",         "Помічник AUR (yay)",         "󰏓", 2,  true},
    {"repos",       "Репозиторії (multilib)",     "󰆼", 1,  true},
    {"sysupdate",   "Оновлення системи",          "󰚰", 8,  true},
    {"core",        "Ключові пакети",             "󰏗", 8,  true},
    {"stable",      "Пакети mgOS",                "󰏖", 14, false},
    {"aur",         "Пакети з AUR",               "󰣇", 10, false},
    {"utils",       "Утиліти",                    "󰦬", 5,  false},
    {"drivers",     "Драйвери під це залізо",     "󰢮", 8,  false},
    {"apps",        "Програми зі знімка",         "󰀻", 14, false},
    {"dev",         "Інструменти розробки",       "󰅩", 8,  false},
    {"files",       "Теки та шрифти",             "󰉋", 1,  false},
    {"configs",     "Конфіги mgOS",               "󰒓", 2,  true},
    {"home",        "Домашні файли (~)",          "󰋜", 1,  false},
    {"build",       "Збірка панелей SYSui",       "󰙨", 3,  false},
    {"theme",       "Кольори, стан, шпалери",     "󰸉", 2,  false},
    {"boot",        "Plymouth та initramfs",      "󰑓", 3,  false},
    {"scripts",     "Системні утиліти",           "󰆍", 1,  false},
    {"services",    "Служби, SDDM, меню сесій",   "󰒋", 1,  false},
    {"look",        "Курсор, GTK-тема, sudoers",  "󰆿", 1,  false},
    {"shell",       "Оболонка fish",              "󰈺", 1,  false},
    {"gpu",         "Налаштування відеокарти",    "󰢮", 1,  false},
    {"hw",          "Екран, масштаб, тачпад",     "󰍹", 1,  false},
    {"upd_repo",    "Оновлення репозиторіїв",     "󰚰", 10, true},
    {"upd_aur",     "AUR-пакети mgOS",            "󰣇", 8,  false},
    {"upd_missing", "Відсутні пакети mgOS",       "󰏗", 5,  false},
    {"cleanup",     "Прибирання автозапуску",     "󰃢", 1,  false},
    {"retry_pkgs",  "Повторне встановлення",      "󰑓", 4,  false},
    {"verify",      "Перевірка результату",       "󰗠", 1,  false},
    {"rollback",    "Повернення як було",         "󰕍", 10, true},
    // kioskOS — окремий режим (steps.sh: step_kiosk_*)
    {"kiosk_pkgs",     "Пакети kioskOS",                    "󰏗", 10, true},
    {"kiosk_extra",    "Файли, Drive, VA-API, VS Code",     "󰏖", 6,  false},
    {"kiosk_files",    "Конфіги kioskOS",                   "󰒓", 2,  true},
    {"kiosk_build",    "Збірка Runner та Dock",             "󰙨", 2,  false},
    {"kiosk_firefox",  "Firefox: RAM-кеш, VA-API, скрол",   "󰈹", 1,  false},
    {"kiosk_theme",    "Тема Graphite, іконки, шрифт",      "󰸉", 2,  false},
    {"kiosk_system",   "Живлення, тачпад, меню сесій",      "󰒋", 1,  false},
    {"kiosk_hw",       "Екран, масштаб, жести",             "󰍹", 1,  false},
    {"kiosk_kernel",   "Параметри ядра (GRUB)",             "󰻠", 2,  false},
    {"kiosk_verify",   "Перевірка kioskOS",                 "󰗠", 1,  false},
};
static const StepDef* step_def(const std::string& id) {
    for (auto& s : STEPS) if (id == s.id) return &s;
    return nullptr;
}

enum class Mode { Install, Restore, Update, Check, Kiosk };
static const char* mode_key(Mode m) {
    return m == Mode::Install ? "install" : m == Mode::Restore ? "restore" : m == Mode::Kiosk ? "kiosk" : "update";
}
static Mode mode_from(const std::string& s) {
    return s == "restore" ? Mode::Restore : s == "update" ? Mode::Update : s == "kiosk" ? Mode::Kiosk : Mode::Install;
}
static const char* mode_title(Mode m) {
    return m == Mode::Install ? "Встановлення" : m == Mode::Restore ? "Відновлення" : m == Mode::Update ? "Оновлення пакетів"
         : m == Mode::Kiosk ? "Встановлення kioskOS" : "Перевірка системи";
}

enum class St { Pending, Running, Done, Warn, Failed, Skipped };

// ============================================================================
//  Стан (зберігається на диск)
// ============================================================================
struct RunState {
    Mode mode = Mode::Install;
    std::vector<std::string> steps;
    std::set<std::string> done, warned, skipped;
    std::map<std::string, std::string> opt;   // OPT_* для steps.sh
    std::vector<std::string> failed_pkgs;
    std::string status;                       // running | paused | failed | done
    std::string log_path;
    gint64 started = 0;
    double elapsed = 0;                       // секунд роботи (без пауз)
    std::string cache;                        // точка відкату: <інсталятор>/cache/<запуск>
    std::set<std::string> pkgwarn;            // кроки, де не встановились пакети
    std::set<std::string> accepted;           // проблеми, які користувач вирішив пропустити
    bool rollback = false;                    // зараз іде відкат
};
static RunState R;

static std::string state_dir() { return std::string(g_get_user_state_dir()) + "/mgos-installer"; }
static std::string state_file() { return state_dir() + "/state.ini"; }

static void save_state() {
    g_mkdir_with_parents(state_dir().c_str(), 0700);
    GKeyFile* k = g_key_file_new();
    auto list = [&](const char* key, const std::vector<std::string>& v) {
        std::vector<const char*> p; for (auto& s : v) p.push_back(s.c_str());
        g_key_file_set_string_list(k, "run", key, p.data(), p.size());
    };
    g_key_file_set_string(k, "run", "mode", mode_key(R.mode));
    g_key_file_set_string(k, "run", "status", R.status.c_str());
    g_key_file_set_string(k, "run", "log", R.log_path.c_str());
    g_key_file_set_int64(k, "run", "started", R.started);
    g_key_file_set_double(k, "run", "elapsed", R.elapsed);
    list("steps", R.steps);
    list("done", {R.done.begin(), R.done.end()});
    list("warned", {R.warned.begin(), R.warned.end()});
    list("skipped", {R.skipped.begin(), R.skipped.end()});
    list("failed_pkgs", R.failed_pkgs);
    list("pkgwarn", {R.pkgwarn.begin(), R.pkgwarn.end()});
    list("accepted", {R.accepted.begin(), R.accepted.end()});
    g_key_file_set_string(k, "run", "cache", R.cache.c_str());
    g_key_file_set_boolean(k, "run", "rollback", R.rollback);
    for (auto& [kk, v] : R.opt) g_key_file_set_string(k, "opt", kk.c_str(), v.c_str());
    g_key_file_save_to_file(k, state_file().c_str(), nullptr);
    g_key_file_free(k);
}

static bool load_state(RunState& s) {
    GKeyFile* k = g_key_file_new();
    if (!g_key_file_load_from_file(k, state_file().c_str(), G_KEY_FILE_NONE, nullptr)) { g_key_file_free(k); return false; }
    auto str = [&](const char* g, const char* key) {
        gchar* v = g_key_file_get_string(k, g, key, nullptr); std::string r = v ? v : ""; g_free(v); return r;
    };
    auto list = [&](const char* key) {
        std::vector<std::string> r; gsize n = 0;
        gchar** v = g_key_file_get_string_list(k, "run", key, &n, nullptr);
        for (gsize i = 0; i < n; i++) r.push_back(v[i]);
        g_strfreev(v); return r;
    };
    s = RunState();
    s.mode = mode_from(str("run", "mode"));
    s.status = str("run", "status");
    s.log_path = str("run", "log");
    s.started = g_key_file_get_int64(k, "run", "started", nullptr);
    s.elapsed = g_key_file_get_double(k, "run", "elapsed", nullptr);
    s.steps = list("steps");
    for (auto& x : list("done")) s.done.insert(x);
    for (auto& x : list("warned")) s.warned.insert(x);
    for (auto& x : list("skipped")) s.skipped.insert(x);
    s.failed_pkgs = list("failed_pkgs");
    for (auto& x : list("pkgwarn")) s.pkgwarn.insert(x);
    for (auto& x : list("accepted")) s.accepted.insert(x);
    s.cache = str("run", "cache");
    s.rollback = g_key_file_get_boolean(k, "run", "rollback", nullptr);
    gsize n = 0;
    if (gchar** keys = g_key_file_get_keys(k, "opt", &n, nullptr)) {
        for (gsize i = 0; i < n; i++) s.opt[keys[i]] = str("opt", keys[i]);
        g_strfreev(keys);
    }
    g_key_file_free(k);
    return !s.steps.empty();
}

static double total_weight(const RunState& s) { double w = 0; for (auto& id : s.steps) if (auto d = step_def(id)) w += d->weight; return w; }
static double done_weight(const RunState& s) {
    double w = 0;
    for (auto& id : s.steps) if (s.done.count(id) || s.skipped.count(id)) if (auto d = step_def(id)) w += d->weight;
    return w;
}

// ============================================================================
//  Глобальні віджети
// ============================================================================
static GtkWidget *g_win, *g_stack, *g_waves, *g_subtitle, *g_chip;
static pw::Waves g_wv;
static GtkCssProvider *g_css, *g_anim_css;
static gint64 g_t0 = 0;
static double g_anim_t = 0;
static std::string g_dir;      // корінь інсталятора (там configs/, steps у gui/)
static std::string g_accent = "#9929CF";

// home
static GtkWidget *g_resume_box, *g_resume_lbl;
// options
static GtkWidget *g_opt_body, *g_opt_title, *g_opt_desc, *g_pw_box, *g_pw_entry, *g_pw_err, *g_start_btn;
static Mode g_opt_mode = Mode::Install;
static bool g_opt_resume = false;
static std::map<std::string, GtkWidget*> g_switches;
static std::vector<std::pair<std::string, GtkWidget*>> g_app_toggles, g_cfg_toggles;
static GtkWidget *g_upd_list = nullptr, *g_upd_status = nullptr;
static int g_upd_count = 0;
// run
static GtkWidget *g_pbar, *g_pct, *g_step_lbl, *g_msg_lbl, *g_time_lbl, *g_steps_box, *g_steps_scroll;
static GtkWidget *g_log_view, *g_log_rev, *g_btn_pause, *g_btn_retry, *g_btn_skip, *g_btn_home, *g_close_banner;
static std::map<std::string, std::pair<GtkWidget*, GtkWidget*>> g_rows;  // id → (icon, row)
static double g_prog_target = 0, g_prog_shown = 0;
// done
static GtkWidget *g_score = nullptr, *g_chk_list, *g_chk_sum, *g_chk_fix, *g_chk_upd, *g_chk_again;
static int g_health = -1;
static std::map<std::string, int> g_chk_bad;   // група → кількість fail/warn
static int g_chk_updates = 0;
static GtkWidget *g_done_icon, *g_done_title, *g_done_sub, *g_done_list, *g_done_reboot;

// виконання
static GSubprocess* g_proc = nullptr;
static GCancellable* g_cancel = nullptr;
static std::string g_cur;          // поточний крок
static double g_cur_sub = -1;      // прогрес усередині кроку з виводу (-1 — невідомо)
static gint64 g_cur_started = 0;
static bool g_pause_req = false, g_running = false, g_cancel_req = false;
static GtkWidget *g_btn_cancel = nullptr, *g_done_cancel = nullptr, *g_done_apply = nullptr, *g_done_items_title = nullptr;
static bool g_opt_rollback = false;
struct Problem { std::string key; bool pkg; GtkWidget* retry; };
static std::vector<Problem> g_problems;
static void start_rollback();
static std::string g_password;
static std::string g_rt;           // тимчасова тека з askpass/обгорткою sudo
static std::ofstream g_logf;
static int g_spin = 0;

static void show_home();
static void run_next();
static void open_options(Mode m, bool resume);

// ============================================================================
//  Утиліти
// ============================================================================
static GtkWidget* label(const std::string& t, const char* cls = nullptr, float x = 0) {
    GtkWidget* l = gtk_label_new(t.c_str());
    if (cls) gtk_widget_add_css_class(l, cls);
    gtk_label_set_xalign(GTK_LABEL(l), x);
    return l;
}
static GtkWidget* hbox(int sp = 8) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, sp); }
static GtkWidget* vbox(int sp = 8) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, sp); }
static GtkWidget* spacer() { GtkWidget* s = hbox(0); gtk_widget_set_hexpand(s, TRUE); return s; }
// ---------- Піксельні елементи: кнопки та цифри з квадратиків ----------
enum PxKind { PX_NEUTRAL, PX_PRIMARY, PX_SUCCESS, PX_DANGER };
static std::vector<GtkWidget*> g_px_areas;   // перемальовуються кожен кадр (hover, «живий» акцент)
static const int BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static void draw_pxbtn(GtkDrawingArea* da, cairo_t* cr, int w, int h, gpointer data) {
    GtkWidget* btn = gtk_widget_get_ancestor(GTK_WIDGET(da), GTK_TYPE_BUTTON);
    GtkStateFlags st = btn ? gtk_widget_get_state_flags(btn) : (GtkStateFlags)0;
    bool hover = st & GTK_STATE_FLAG_PRELIGHT, press = st & GTK_STATE_FLAG_ACTIVE, dis = st & GTK_STATE_FLAG_INSENSITIVE;
    int kind = GPOINTER_TO_INT(data);
    const int cell = 4, gap = 1;
    int cols = w / cell, rows = h / cell;
    if (cols < 4 || rows < 3) return;
    double ox = (w - cols * cell) / 2.0, oy = (h - rows * cell) / 2.0;
    double t = g_anim_t;
    double sweep = std::fmod(t * 28.0, cols + 24.0) - 12;   // смуга світла при наведенні
    for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++) {
        int cx = std::min(x, cols - 1 - x), cy = std::min(y, rows - 1 - y);
        if (cx == 0 && cy == 0) continue;                     // піксельний кут: зрізано лише один квадратик
        bool edge = cx == 0 || cy == 0;
        double bay = BAYER4[y & 3][x & 3] / 16.0;
        double lift = hover ? 0.16 * std::exp(-std::pow((x - y * 0.6 - sweep) / 3.0, 2)) : 0;
        if (press) lift -= 0.08;
        double a = 1;
        pw::RGB c;
        if (kind == PX_NEUTRAL) {
            c = {1, 1, 1};
            a = (hover ? 0.13 : 0.075) + (edge ? 0.05 : 0) + lift * 0.6 + (bay - 0.5) * 0.02;
        } else {
            double hue = kind == PX_PRIMARY ? g_wv.hue_at(t) + g_wv.grad((double)x / cols) : kind == PX_SUCCESS ? 152 : 352;
            double sat = kind == PX_PRIMARY ? g_wv.sat() : kind == PX_SUCCESS ? 0.62 : 0.78;
            double l = (kind == PX_SUCCESS ? 0.5 : 0.56) + (bay - 0.5) * 0.05 + lift;
            if (y == 0 || (cy == 0 && y < rows / 2)) l += 0.1;      // світлий верх — об'єм
            if (y == rows - 1) l -= 0.12;                           // темний низ
            c = pw::hsl_rgb(hue, sat, std::clamp(l, 0.0, 0.92));
        }
        if (dis) a *= 0.4;
        cairo_set_source_rgba(cr, c.r, c.g, c.b, std::clamp(a, 0.0, 1.0));
        cairo_rectangle(cr, ox + x * cell, oy + y * cell, cell - gap, cell - gap);
        cairo_fill(cr);
    }
}

static GtkWidget* button(const std::string& t, const char* cls, GCallback cb, gpointer d = nullptr) {
    int kind = !cls ? PX_NEUTRAL : !strcmp(cls, "primary") ? PX_PRIMARY : !strcmp(cls, "success") ? PX_SUCCESS :
               !strcmp(cls, "danger") ? PX_DANGER : PX_NEUTRAL;
    GtkWidget* b = gtk_button_new();
    gtk_widget_add_css_class(b, "pxbtn");
    if (cls) gtk_widget_add_css_class(b, cls);
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
    g_px_areas.push_back(da);
    if (cb) g_signal_connect(b, "clicked", cb, d);
    return b;
}
static void set_btn_label(GtkWidget* b, const std::string& t) {
    if (auto* l = (GtkWidget*)g_object_get_data(G_OBJECT(b), "lbl")) gtk_label_set_text(GTK_LABEL(l), t.c_str());
}

// Цифри 5×7 з квадратиків
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
// bold — жирний варіант: кожен піксель гліфа подвоюється вправо (штрихи 2 пікселі завтовшки)
static void draw_pixel_text(cairo_t* cr, const std::string& txt, int w, int h, int cell, double hue_shift, bool bold = false) {
    const int GW = 5, GH = 7, DW = GW + (bold ? 1 : 0);   // ширина гліфа на екрані
    int gw = (int)txt.size() * (DW + 1) - 1;
    double ox = w - gw * cell, oy = (h - GH * cell) / 2.0;   // праворуч
    double t = g_anim_t;
    for (int pass = 0; pass < 2; pass++)                     // 0 — тінь, 1 — пікселі
        for (size_t i = 0; i < txt.size(); i++) {
            const char* g = glyph(txt[i]);
            auto on = [&](int r, int c) { return c >= 0 && c < GW && g[r * GW + c] == '1'; };
            for (int r = 0; r < GH; r++) for (int c = 0; c < DW; c++) {
                if (!(on(r, c) || (bold && on(r, c - 1)))) continue;
                int gx = (int)i * (DW + 1) + c;
                double x = ox + gx * cell, y = oy + r * cell;
                if (pass == 0) { cairo_set_source_rgba(cr, 0, 0, 0, 0.45); cairo_rectangle(cr, x + cell * 0.4, y + cell * 0.4, cell - 1, cell - 1); }
                else {
                    double wave = 0.5 + 0.5 * std::sin(gx * 0.45 - t * 4 + r * 0.4);
                    pw::RGB col = pw::hsl_rgb(g_wv.hue_at(t) + hue_shift + 5.0 * gx, g_wv.sat(), 0.6 + 0.14 * wave - r * 0.015);
                    cairo_set_source_rgb(cr, col.r, col.g, col.b);
                    cairo_rectangle(cr, x, y, cell - 1, cell - 1);
                }
                cairo_fill(cr);
            }
        }
}
static void clear_box(GtkWidget* b) { while (GtkWidget* c = gtk_widget_get_first_child(b)) gtk_box_remove(GTK_BOX(b), c); }
static std::string read_text(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}
static std::string fmt_time(double s) {
    int t = (int)s; char b[32];
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}
static std::string wal_accent(const std::string& path) {
    std::string css = read_text(path);
    auto p = css.find("--color5:");
    if (p == std::string::npos) return "";
    auto h = css.find('#', p);
    if (h == std::string::npos) return "";
    std::string v = css.substr(h, 7);
    pw::RGB c; return pw::parse_hex(v, c) ? v : "";
}

// ============================================================================
//  CSS
// ============================================================================
static std::string fz_css();
static void apply_css() {
    std::string A = g_accent;
    std::string css = R"CSS(
window.mgos { background-color: #141418; color: #f5f5f7; }
* { font-family: "SF Pro Display", "Inter", "JetBrainsMono Nerd Font", "Noto Sans", sans-serif; outline: none; }
.mgos label { color: #f5f5f7; }
.mono, .mono label { font-family: "JetBrainsMono Nerd Font", "Noto Sans Mono", monospace; }
.title { font-size: 34px; font-weight: 900; letter-spacing: 1px; text-shadow: 0 2px 10px rgba(0,0,0,0.6); }
.badge { font-family: "JetBrainsMono Nerd Font", monospace; font-size: 22px; font-weight: 900;
         background-color: @A@; color: #fff; border-radius: 10px; padding: 0 12px; margin-left: 10px; }
.subtitle { font-size: 13px; opacity: 0.85; text-shadow: 0 1px 6px rgba(0,0,0,0.7); }
.chip { background-color: rgba(10,10,14,0.55); border: 1px solid rgba(255,255,255,0.12);
        border-radius: 999px; padding: 4px 12px; font-size: 12px; font-weight: 600; }
button.x { background: rgba(10,10,14,0.55); border: 1px solid rgba(255,255,255,0.12); border-radius: 999px;
           min-width: 28px; min-height: 28px; padding: 0; box-shadow: none; color: #fff; }
button.x:hover { background: rgba(255,90,110,0.85); }
.page { padding: 6px 22px 22px 22px; }
.h1 { font-size: 20px; font-weight: 800; }
.h2 { font-size: 14px; font-weight: 800; }
.dimtxt { font-size: 12.5px; opacity: 0.62; }
.small { font-size: 11px; opacity: 0.5; }
.mode-card { background-color: rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.07);
             border-radius: 18px; padding: 16px; box-shadow: none; background-image: none;
             transition: transform 260ms cubic-bezier(0.34,1.56,0.64,1), background-color 200ms, border-color 200ms; }
.mode-card:hover { background-color: rgba(255,255,255,0.09); border-color: @A50@; transform: scale(1.03); }
.mode-card:active { transform: scale(0.97); }
.mode-icon { font-family: "JetBrainsMono Nerd Font"; font-size: 26px; color: @A@; background-color: @SOFT@;
             border-radius: 14px; min-width: 52px; min-height: 52px; }
.mode-title { font-size: 15px; font-weight: 800; }
.mode-desc { font-size: 12px; opacity: 0.6; }
.resume { background-color: @SOFT@; border: 1px solid @A50@; border-radius: 16px; padding: 12px 14px; }
.opt-row { background-color: rgba(255,255,255,0.045); border-radius: 14px; padding: 10px 14px; }
.opt-title { font-size: 13.5px; font-weight: 700; }
.opt-desc { font-size: 11.5px; opacity: 0.55; }
switch { background-color: rgba(255,255,255,0.15); border: none; }
switch:checked { background-color: @A@; }
switch slider { background-color: #fff; border: none; box-shadow: none; }
.chips flowboxchild { padding: 0; }
button.chipt { border-radius: 999px; padding: 4px 12px; font-size: 12px; font-weight: 600;
           background: rgba(255,255,255,0.06); border: 1px solid rgba(255,255,255,0.08); box-shadow: none; color: #ddd; }
button.chipt:checked { background-color: @SOFT@; border-color: @A50@; color: #fff; }
passwordentry, entry { background-color: rgba(0,0,0,0.3); border: 1px solid rgba(255,255,255,0.1); border-radius: 12px;
                       padding: 6px 10px; color: #fff; min-height: 22px; }
passwordentry:focus-within { border-color: @A@; }
.err { color: #ff7b8a; font-size: 12px; }
button.pxbtn { background: none; border: none; box-shadow: none; padding: 0; min-height: 0; min-width: 0;
               transition: transform 260ms cubic-bezier(0.34,1.56,0.64,1); }
button.pxbtn:hover { transform: scale(1.04); }
button.pxbtn:active { transform: scale(0.95); }
.pxlbl { padding: 10px 20px; font-family: "JetBrainsMono Nerd Font", monospace; font-weight: 800; font-size: 13px;
         color: #f5f5f7; text-shadow: 0 1px 0 rgba(0,0,0,0.35); }
button.pxbtn.success .pxlbl { color: #0d1f16; text-shadow: none; }
button.pxbtn:disabled .pxlbl { opacity: 0.5; }
.pct { font-family: "JetBrainsMono Nerd Font", monospace; font-size: 40px; font-weight: 900; }
.steptitle { font-size: 15px; font-weight: 800; }
.msg { font-family: "JetBrainsMono Nerd Font", monospace; font-size: 11.5px; opacity: 0.6; }
.steps-box { background-color: rgba(0,0,0,0.22); border-radius: 16px; padding: 8px; }
.srow { padding: 5px 10px; border-radius: 10px; }
.srow.running { background-color: @SOFT@; }
.sicon { font-family: "JetBrainsMono Nerd Font"; font-size: 14px; min-width: 22px; }
.sicon.pending { opacity: 0.3; }
.sicon.done { color: #5be0a2; }
.sicon.warn { color: #ffd166; }
.sicon.failed { color: #ff7b8a; }
.sicon.skipped { opacity: 0.4; }
.sicon.running { color: @A@; }
.stitle { font-size: 13px; }
.srow.pending .stitle { opacity: 0.55; }
textview.log, textview.log text { background-color: rgba(0,0,0,0.35); color: #cfcfd6; font-family: "JetBrainsMono Nerd Font", monospace; font-size: 11px; }
textview.log { border-radius: 12px; padding: 8px; }
.banner { background-color: rgba(255,90,110,0.18); border: 1px solid rgba(255,90,110,0.5); border-radius: 14px; padding: 10px 14px; }
.big-icon { font-family: "JetBrainsMono Nerd Font"; font-size: 54px; color: @A@; }
.big-icon.warn { color: #ffd166; }
.pkg { font-family: "JetBrainsMono Nerd Font", monospace; font-size: 12px; padding: 3px 8px; }
.tag { font-size: 9.5px; font-weight: 800; color: @A@; border: 1px solid @A50@; border-radius: 6px; padding: 0 5px; }
.badge, .mode-icon, .sicon.running, .srow.running, switch:checked, .tag, .resume
    { transition: background-color 160ms linear, color 160ms linear, border-color 160ms linear; }
scrollbar slider { background-color: rgba(255,255,255,0.2); border-radius: 99px; min-width: 4px; }
)CSS";
    pw::RGB c; pw::parse_hex(A, c);
    auto rep = [&](const std::string& k, const std::string& v) {
        size_t p = 0; while ((p = css.find(k, p)) != std::string::npos) { css.replace(p, k.size(), v); p += v.size(); }
    };
    rep("@SOFT@", pw::to_rgba(c, 0.18)); rep("@A50@", pw::to_rgba(c, 0.5)); rep("@A@", A);
    css += fz_css();
    gtk_css_provider_load_from_string(g_css, css.c_str());
}

// «Живий» акцент — пливе разом із хвилями (як в about_osd)
static gboolean update_anim_css(gpointer) {
    pw::RGB c = g_wv.accent_now();
    std::string H = pw::to_hex(c), S = pw::to_rgba(c, 0.18), G = pw::to_rgba(c, 0.5);
    std::string css =
        ".badge{background-color:" + H + ";box-shadow:0 0 18px " + G + ";}"
        ".mode-icon{color:" + H + ";background-color:" + S + ";}"
        ".mode-card:hover{border-color:" + G + ";}"
        ".sicon.running{color:" + H + ";} .srow.running{background-color:" + S + ";}"
        "switch:checked{background-color:" + H + ";}"
        ".tag{color:" + H + ";border-color:" + G + ";} .resume{background-color:" + S + ";border-color:" + G + ";}"
        ".big-icon:not(.warn){color:" + H + ";} button.chipt:checked{background-color:" + S + ";border-color:" + G + ";}";
    gtk_css_provider_load_from_string(g_anim_css, css.c_str());
    return G_SOURCE_CONTINUE;
}

// ============================================================================
//  Хвилі та піксельний прогрес-бар
// ============================================================================
static void draw_waves(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) { g_wv.t = g_anim_t; g_wv.draw(cr, w, h); }

static gboolean on_tick(GtkWidget*, GdkFrameClock* fc, gpointer) {
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    if (!g_t0) g_t0 = now;
    g_anim_t = (now - g_t0) / 1e6;
    g_wv.reveal = std::min(1.0, g_anim_t / 1.4);
    gtk_widget_queue_draw(g_waves);
    // прогрес плавно наздоганяє ціль
    g_prog_shown += (g_prog_target - g_prog_shown) * 0.08;
    if (std::abs(g_prog_target - g_prog_shown) < 0.0005) g_prog_shown = g_prog_target;
    if (g_pbar && gtk_widget_get_mapped(g_pbar)) { gtk_widget_queue_draw(g_pbar); gtk_widget_queue_draw(g_pct); }
    if (g_score && gtk_widget_get_mapped(g_score)) gtk_widget_queue_draw(g_score);
    for (GtkWidget* a : g_px_areas) if (gtk_widget_get_mapped(a)) gtk_widget_queue_draw(a);
    return G_SOURCE_CONTINUE;
}

// Піксельний прогрес-бар, розбитий на розділи — по одному на крок.
// Ширина розділу пропорційна вазі кроку (як і відсоток), між розділами — порожня колонка.
// Колір готового розділу: хвиля акценту (OK), бурштиновий (з попередженнями), червоний (помилка),
// тьмяний (пропущено). Поточний розділ має підсвічену «доріжку» і яскраву голову, що пульсує.
static void draw_pbar(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    const int cell = 6, gap = 1;
    const int cols = w / cell, rows = std::max(1, h / cell);
    const double t = g_anim_t, f = std::clamp(g_prog_shown, 0.0, 1.0);
    static const double BAY[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

    struct Seg { int x0, x1; double w0, wt; St st; bool cur; };
    std::vector<Seg> segs;
    double tw = 0;
    for (auto& id : R.steps) if (auto d = step_def(id)) tw += d->weight;
    const int n = (int)R.steps.size();
    const int avail = cols - std::max(0, n - 1);
    if (n == 0 || tw <= 0 || avail < n * 2) {
        segs.push_back({0, cols, 0, 1, St::Pending, false});   // замало місця — суцільний бар
        tw = 1;
    } else {
        // ширини: пропорційно вазі, але не менше 2 колонок; залишок округлення — найширшому
        std::vector<int> wd(n);
        int used = 0, widest = 0;
        for (int i = 0; i < n; i++) {
            auto d = step_def(R.steps[i]);
            wd[i] = std::max(2, (int)std::lround((d ? d->weight : 1) / tw * avail));
            used += wd[i];
            if (wd[i] > wd[widest]) widest = i;
        }
        wd[widest] = std::max(2, wd[widest] + avail - used);
        int x = 0; double cw = 0;
        for (int i = 0; i < n; i++) {
            const std::string& id = R.steps[i];
            auto d = step_def(id);
            double wt = d ? d->weight : 1;
            St st = R.skipped.count(id) ? St::Skipped
                  : (R.status == "failed" && id == g_cur) ? St::Failed
                  : R.warned.count(id) ? St::Warn
                  : R.done.count(id) ? St::Done
                  : (id == g_cur && g_running) ? St::Running : St::Pending;
            segs.push_back({x, std::min(cols, x + wd[i]), cw, wt, st, id == g_cur && g_running});
            x += wd[i] + 1; cw += wt;
        }
    }

    const double pos = f * tw;   // заповнення у «вагових» одиницях — та сама величина, що й відсоток
    for (const Seg& sg : segs) {
        const int sw = sg.x1 - sg.x0;
        if (sw <= 0) continue;
        double sf = std::clamp((pos - sg.w0) / sg.wt, 0.0, 1.0);
        if (sg.st == St::Done || sg.st == St::Warn || sg.st == St::Skipped) sf = 1;
        const int filled = (int)std::lround(sf * sw);
        for (int x = sg.x0; x < sg.x1; x++) {
            const int lx = x - sg.x0;
            for (int y = 0; y < rows; y++) {
                const double dith = BAY[y & 3][x & 3] / 16.0 - 0.5;
                double r, g, b, a = 1;
                if (lx < filled) {
                    double hue, sat = g_wv.sat();
                    const double wave = 0.5 + 0.5 * std::sin(x * 0.25 - t * 5 + y * 0.6);
                    double l = 0.42 + 0.16 * wave + dith * 0.06;
                    switch (sg.st) {
                        case St::Warn:    hue = 40;  sat = 0.9; break;
                        case St::Failed:  hue = 352; sat = 0.85; break;
                        case St::Skipped: hue = 0;   sat = 0; l = 0.55; a = 0.22 + (dith < 0 ? 0.06 : 0); break;
                        default:          hue = g_wv.hue_at(t) + g_wv.grad((double)x / cols);
                    }
                    if (sg.cur && lx >= filled - 2) l = 0.72 + 0.15 * std::sin(t * 9);   // «голова»
                    pw::RGB c = pw::hsl_rgb(hue, sat, l);
                    r = c.r; g = c.g; b = c.b;
                } else if (sg.st == St::Failed) {
                    r = 1; g = 0.35; b = 0.42; a = 0.14 + 0.06 * std::sin(t * 4);
                } else if (sg.cur) {
                    // доріжка поточного розділу світліша і ледь «дихає»
                    r = g = b = 1; a = 0.12 + 0.04 * std::sin(t * 3 - lx * 0.4) + (dith < -0.25 ? 0.03 : 0);
                } else {
                    r = g = b = 1; a = 0.06 + (dith < -0.25 ? 0.03 : 0);
                }
                cairo_set_source_rgba(cr, r, g, b, a);
                cairo_rectangle(cr, x * cell, y * cell, cell - gap, cell - gap);
                cairo_fill(cr);
            }
        }
    }
}

// ============================================================================
//  «Барабан» для списків: рядки біля верхнього/нижнього краю зменшуються й тьмяніють,
//  ніби заходять за рамку. Рівні — CSS-класи .fz0…fzN (масштаб + прозорість), тому
//  на кожну прокрутку лише перемикаються класи, без власних віджетів і перемальовок.
// ============================================================================
static const int FZ_LEVELS = 20;
static std::string fz_css() {
    std::string c;
    for (int i = 0; i < FZ_LEVELS; i++) {
        double e = (double)i / FZ_LEVELS, k = e * e * (3 - 2 * e);   // smoothstep
        char b[200];
        snprintf(b, sizeof b, ".fz%d { transform: scale(%.3f); opacity: %.3f; }\n", i, 0.72 + 0.28 * k, 0.05 + 0.95 * k);
        c += b;
    }
    for (std::string::size_type q; (q = c.find(',')) != std::string::npos;) c[q] = '.';   // локаль uk_UA: кома замість крапки
    return c;
}
static void fz_update(GtkScrolledWindow* sw) {
    GtkWidget* vp = gtk_scrolled_window_get_child(sw);
    if (!vp) return;
    GtkWidget* box = GTK_IS_VIEWPORT(vp) ? gtk_viewport_get_child(GTK_VIEWPORT(vp)) : vp;
    if (!box) return;
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(sw);
    const double val = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj),
                 upper = gtk_adjustment_get_upper(adj);
    if (page <= 0) return;
    const double zone = std::clamp(page * 0.22, 24.0, 64.0);
    // ефект лише з того боку, де за краєм справді є вміст, і вмикається плавно
    const double top_k = std::clamp(val / zone, 0.0, 1.0), bot_k = std::clamp((upper - page - val) / zone, 0.0, 1.0);
    for (GtkWidget* c = gtk_widget_get_first_child(box); c; c = gtk_widget_get_next_sibling(c)) {
        graphene_point_t p0 = GRAPHENE_POINT_INIT(0, 0), p;
        if (!gtk_widget_get_visible(c) || !gtk_widget_compute_point(c, vp, &p0, &p)) continue;
        const double hh = gtk_widget_get_height(c);
        // для високих блоків беремо край, ближчий до межі, а не центр
        const double top_y = p.y + std::min(hh, zone) / 2, bot_y = p.y + hh - std::min(hh, zone) / 2;
        double e = 1;
        if (top_y < zone)        e = std::min(e, 1 - top_k * (1 - std::max(0.0, top_y) / zone));
        if (bot_y > page - zone) e = std::min(e, 1 - bot_k * (1 - std::max(0.0, page - bot_y) / zone));
        const int lvl = (int)std::floor(std::clamp(e, 0.0, 1.0) * FZ_LEVELS);   // FZ_LEVELS — без класу
        const int old = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "fz")) - 1;
        if (old == lvl) continue;
        if (old >= 0 && old < FZ_LEVELS) gtk_widget_remove_css_class(c, ("fz" + std::to_string(old)).c_str());
        if (lvl < FZ_LEVELS) gtk_widget_add_css_class(c, ("fz" + std::to_string(lvl)).c_str());
        g_object_set_data(G_OBJECT(c), "fz", GINT_TO_POINTER(lvl + 1));
    }
}
// позиції рядків стають відомі лише після розміщення — тому оновлюємо на idle, один раз на кадр
static void fz_queue(GtkAdjustment*, gpointer sw) {
    if (g_object_get_data(G_OBJECT(sw), "fz-q")) return;
    g_object_set_data(G_OBJECT(sw), "fz-q", GINT_TO_POINTER(1));
    g_idle_add_full(G_PRIORITY_HIGH_IDLE + 20, [](gpointer d) -> gboolean {
        g_object_set_data(G_OBJECT(d), "fz-q", nullptr);
        fz_update(GTK_SCROLLED_WINDOW(d));
        g_object_unref(d);
        return G_SOURCE_REMOVE;
    }, g_object_ref(sw), nullptr);
}
static void fz_attach(GtkWidget* sw) {
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    g_signal_connect(adj, "value-changed", G_CALLBACK(fz_queue), sw);
    g_signal_connect(adj, "changed", G_CALLBACK(fz_queue), sw);
    g_signal_connect(sw, "map", G_CALLBACK(+[](GtkWidget* w, gpointer) { fz_queue(nullptr, w); }), nullptr);
}

// ============================================================================
//  Виконання кроків
// ============================================================================
static void log_line(const std::string& s) {
    if (g_logf.is_open()) { g_logf << s << '\n'; g_logf.flush(); }
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_log_view));
    GtkTextIter end; gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, (s + "\n").c_str(), -1);
    if (gtk_text_buffer_get_line_count(b) > 1500) {
        GtkTextIter a, z; gtk_text_buffer_get_start_iter(b, &a); gtk_text_buffer_get_iter_at_line(b, &z, 300);
        gtk_text_buffer_delete(b, &a, &z);
    }
    if (gtk_revealer_get_reveal_child(GTK_REVEALER(g_log_rev))) {
        gtk_text_buffer_get_end_iter(b, &end);
        GtkTextMark* m = gtk_text_buffer_create_mark(b, nullptr, &end, FALSE);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(g_log_view), m);
        gtk_text_buffer_delete_mark(b, m);
    }
}

static void set_row(const std::string& id, St st) {
    auto it = g_rows.find(id);
    if (it == g_rows.end()) return;
    GtkWidget *ic = it->second.first, *row = it->second.second;
    static const char* cls[] = {"pending", "running", "done", "warn", "failed", "skipped"};
    for (auto c : cls) { gtk_widget_remove_css_class(ic, c); gtk_widget_remove_css_class(row, c); }
    gtk_widget_add_css_class(ic, cls[(int)st]);
    gtk_widget_add_css_class(row, cls[(int)st]);
    const char* glyph = st == St::Pending ? "○" : st == St::Running ? "⠋" : st == St::Done ? "󰄬" :
                        st == St::Warn ? "󰀦" : st == St::Failed ? "󰅙" : "󰒭";
    gtk_label_set_text(GTK_LABEL(ic), glyph);
    if (st == St::Running) {
        // прокрутити список до поточного кроку
        graphene_point_t p, zero = {0, 0};
        if (gtk_widget_compute_point(row, g_steps_box, &zero, &p)) {
            GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(g_steps_scroll));
            gtk_adjustment_set_value(adj, std::max(0.0, (double)p.y - 60));
        }
    }
}

static double step_fraction() {
    if (g_cur.empty()) return 0;
    if (g_cur_sub >= 0) return std::min(0.98, g_cur_sub);
    // невідомо скільки — асимптотично повзе до 90%, щоб бар не завмирав
    auto d = step_def(g_cur);
    double el = (g_get_monotonic_time() - g_cur_started) / 1e6;
    return 0.9 * (1 - std::exp(-el / (6.0 * (d ? d->weight : 1) + 4)));
}

static void update_progress() {
    double tw = total_weight(R);
    double w = done_weight(R);
    if (auto d = step_def(g_cur); d && g_running) w += d->weight * step_fraction();
    g_prog_target = tw > 0 ? w / tw : 0;
    double el = R.elapsed + (g_running ? (g_get_monotonic_time() - g_cur_started) / 1e6 : 0);
    int n = 0, i = 0;
    for (auto& id : R.steps) { n++; if (R.done.count(id) || R.skipped.count(id)) i++; }
    gtk_label_set_text(GTK_LABEL(g_time_lbl), ("крок " + std::to_string(std::min(n, i + 1)) + " з " + std::to_string(n) + "  ·  " + fmt_time(el)).c_str());
}

static gboolean on_ui_timer(gpointer) {
    if (!g_running) return G_SOURCE_CONTINUE;
    update_progress();
    g_spin = (g_spin + 1) % 10;
    static const char* SP[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    if (auto it = g_rows.find(g_cur); it != g_rows.end()) gtk_label_set_text(GTK_LABEL(it->second.first), SP[g_spin]);
    return G_SOURCE_CONTINUE;
}

static void handle_line(const std::string& raw) {
    // прибираємо ANSI-кольори та \r
    std::string s; s.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\x1b') { while (i < raw.size() && !isalpha((unsigned char)raw[i])) i++; continue; }
        if (raw[i] == '\r') { s.clear(); continue; }
        s += raw[i];
    }
    if (s.rfind("@@MSG ", 0) == 0) { gtk_label_set_text(GTK_LABEL(g_msg_lbl), s.substr(6).c_str()); log_line("  › " + s.substr(6)); return; }
    if (s.rfind("@@SUB ", 0) == 0) {
        int a = 0, b = 0;
        if (sscanf(s.c_str() + 6, "%d/%d", &a, &b) == 2 && b > 0) g_cur_sub = (double)a / b;
        return;
    }
    if (s.rfind("@@FAILPKG ", 0) == 0) {
        std::string p = s.substr(10);
        if (std::find(R.failed_pkgs.begin(), R.failed_pkgs.end(), p) == R.failed_pkgs.end()) R.failed_pkgs.push_back(p);
        R.warned.insert(g_cur);
        if (!R.rollback) R.pkgwarn.insert(g_cur);
        log_line(std::string(R.rollback ? "  ⚠ не повернуто: " : "  ⚠ пакет не встановився: ") + p);
        return;
    }
    if (s.rfind("@@CHECK ", 0) == 0) {
        bool ok = s.compare(8, 2, "ok") == 0, warn = s.compare(8, 4, "warn") == 0;
        std::string name = s.substr(ok ? 11 : 13);
        log_line(std::string(ok ? "  [ OK ]  " : warn ? "  [WARN]  " : "  [FAIL]  ") + name);
        if (!ok && !warn) R.warned.insert(g_cur);
        return;
    }
    // pacman: "(12/80) installing foo"
    auto p = s.find('(');
    if (p != std::string::npos && (s.find("installing") != std::string::npos || s.find("upgrading") != std::string::npos ||
                                   s.find("reinstalling") != std::string::npos)) {
        int a = 0, b = 0;
        if (sscanf(s.c_str() + p, "(%d/%d)", &a, &b) == 2 && b > 0) {
            g_cur_sub = (double)a / b;
            gtk_label_set_text(GTK_LABEL(g_msg_lbl), trim(s.substr(s.find(')', p) + 1)).c_str());
        }
    }
    if (!trim(s).empty()) {
        log_line(s);
        if (g_cur_sub < 0 || s.find("installing") != std::string::npos) {
            std::string t = trim(s);
            if (t.size() > 140) t = t.substr(0, 140) + "…";
            gtk_label_set_text(GTK_LABEL(g_msg_lbl), t.c_str());
        }
    }
}

static void read_next(GDataInputStream* in);
static void on_line(GObject* src, GAsyncResult* res, gpointer) {
    gsize len = 0;
    GError* err = nullptr;
    char* line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, &len, &err);
    if (err) { g_error_free(err); g_object_unref(src); return; }
    if (!line) { g_object_unref(src); return; }  // EOF
    handle_line(line);
    g_free(line);
    read_next(G_DATA_INPUT_STREAM(src));
}
static void read_next(GDataInputStream* in) { g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, g_cancel, on_line, nullptr); }

static void set_controls() {
    bool failed = R.status == "failed";
    gtk_widget_set_visible(g_btn_pause, !failed && R.status != "done");
    gtk_widget_set_visible(g_btn_retry, failed);
    gtk_widget_set_visible(g_btn_skip, failed);
    gtk_widget_set_visible(g_btn_home, !g_running);
    if (R.status == "paused" && !g_running) set_btn_label(g_btn_pause, "󰐊  Продовжити");
    else if (g_pause_req) set_btn_label(g_btn_pause, "󰏤  Пауза після кроку…");
    else set_btn_label(g_btn_pause, "󰏤  Пауза");
    gtk_widget_set_sensitive(g_btn_pause, (!g_pause_req || !g_running) && !g_cancel_req);
    gtk_widget_set_visible(g_btn_pause, gtk_widget_get_visible(g_btn_pause) && !R.rollback);
    gtk_widget_set_visible(g_btn_skip, failed && !R.rollback);
    // «Скасувати» — повернути все як було (під час кроку — після його завершення: pacman не можна рвати)
    gtk_widget_set_visible(g_btn_cancel, !R.rollback && R.status != "done");
    set_btn_label(g_btn_cancel, g_cancel_req ? "󰔟  Скасування після кроку…" : "󰕍  Скасувати");
    gtk_widget_set_sensitive(g_btn_cancel, !g_cancel_req);
}

static void finish_all();

static void on_exit(GObject* src, GAsyncResult* res, gpointer) {
    GError* err = nullptr;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err);
    bool ok = !err && g_subprocess_get_if_exited(G_SUBPROCESS(src)) && g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 0;
    if (err) g_error_free(err);
    g_object_unref(g_proc); g_proc = nullptr;
    g_running = false;
    R.elapsed += (g_get_monotonic_time() - g_cur_started) / 1e6;
    auto d = step_def(g_cur);

    if (ok || (d && !d->fatal)) {
        if (!ok) R.warned.insert(g_cur);
        R.done.insert(g_cur);
        set_row(g_cur, R.warned.count(g_cur) ? St::Warn : St::Done);
        log_line(std::string(ok ? "✓ " : "⚠ ") + (d ? d->title : g_cur) + (ok ? " — готово" : " — з помилками, продовжую"));
        g_cur.clear(); g_cur_sub = -1;
        if (g_cancel_req) { g_cancel_req = false; save_state(); start_rollback(); return; }
        if (g_pause_req) {
            g_pause_req = false;
            R.status = "paused";
            save_state();
            gtk_label_set_text(GTK_LABEL(g_step_lbl), "Пауза — прогрес збережено");
            gtk_label_set_text(GTK_LABEL(g_msg_lbl), "Можна закрити інсталятор і продовжити пізніше");
            update_progress(); set_controls();
            return;
        }
        save_state();
        run_next();
    } else {
        R.status = "failed";
        save_state();
        set_row(g_cur, St::Failed);
        if (g_cancel_req && !R.rollback) { g_cancel_req = false; start_rollback(); return; }
        log_line(std::string("✗ ") + (d ? d->title : g_cur) + " — помилка");
        gtk_label_set_text(GTK_LABEL(g_step_lbl), (std::string("Помилка: ") + (d ? d->title : g_cur)).c_str());
        gtk_label_set_text(GTK_LABEL(g_msg_lbl), "Подивись лог, виправ причину і натисни «Повторити». Прогрес збережено.");
        gtk_revealer_set_reveal_child(GTK_REVEALER(g_log_rev), TRUE);
        update_progress(); set_controls();
    }
}

static bool setup_sudo_helpers() {
    if (!g_rt.empty()) return true;
    const char* xdg = g_get_user_runtime_dir();
    char tmpl[512]; snprintf(tmpl, sizeof tmpl, "%s/mgos-installer-XXXXXX", xdg);
    if (!g_mkdtemp_full(tmpl, 0700)) return false;
    g_rt = tmpl;
    // askpass читає пароль зі змінної оточення процесу (не з диска)
    std::ofstream(g_rt + "/askpass") << "#!/bin/sh\nprintf '%s\\n' \"$MGOS_SUDO_PW\"\n";
    std::ofstream(g_rt + "/sudo") << "#!/bin/sh\nexec /usr/bin/sudo -A \"$@\"\n";
    chmod((g_rt + "/askpass").c_str(), 0700);
    chmod((g_rt + "/sudo").c_str(), 0700);
    return true;
}

static GSubprocessLauncher* make_launcher(bool with_opts = true) {
    setup_sudo_helpers();
    GSubprocessLauncher* l = g_subprocess_launcher_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE));
    const char* path = g_getenv("PATH");
    g_subprocess_launcher_setenv(l, "PATH", (g_rt + ":" + (path ? path : "/usr/bin")).c_str(), TRUE);
    g_subprocess_launcher_setenv(l, "SUDO_ASKPASS", (g_rt + "/askpass").c_str(), TRUE);
    g_subprocess_launcher_setenv(l, "MGOS_SUDO_PW", g_password.c_str(), TRUE);
    g_subprocess_launcher_setenv(l, "MGOS_DIR", g_dir.c_str(), TRUE);
    g_subprocess_launcher_setenv(l, "LC_ALL", "C", TRUE);
    g_subprocess_launcher_setenv(l, "TERM", "dumb", TRUE);
    if (with_opts) {
        for (auto& [k, v] : R.opt) g_subprocess_launcher_setenv(l, k.c_str(), v.c_str(), TRUE);
        if (!R.cache.empty()) g_subprocess_launcher_setenv(l, "MGOS_CACHE", R.cache.c_str(), TRUE);
    }
    g_subprocess_launcher_set_cwd(l, g_dir.c_str());
    return l;
}

static GSubprocess* spawn_step(const std::string& id, GSubprocessLauncher* l) {
    std::string cmd = "source \"$MGOS_DIR/gui/steps.sh\" && step_" + id;
    GError* err = nullptr;
    GSubprocess* p = g_subprocess_launcher_spawn(l, &err, "bash", "-c", cmd.c_str(), nullptr);
    if (err) { log_line(std::string("не вдалося запустити: ") + err->message); g_error_free(err); }
    return p;
}

static void run_next() {
    std::string next;
    for (auto& id : R.steps) if (!R.done.count(id) && !R.skipped.count(id)) { next = id; break; }
    if (next.empty()) { finish_all(); return; }
    g_cur = next; g_cur_sub = -1; g_cur_started = g_get_monotonic_time();
    R.status = "running";
    save_state();  // якщо процес обірветься — продовжимо саме з цього кроку
    auto d = step_def(next);
    set_row(next, St::Running);
    gtk_label_set_text(GTK_LABEL(g_step_lbl), d ? d->title : next.c_str());
    gtk_label_set_text(GTK_LABEL(g_msg_lbl), "…");
    log_line(std::string("━━ ") + (d ? d->title : next) + " ━━");

    GSubprocessLauncher* l = make_launcher();
    g_proc = spawn_step(next, l);
    g_object_unref(l);
    if (!g_proc) { R.status = "failed"; save_state(); set_row(next, St::Failed); set_controls(); return; }
    g_running = true;
    GDataInputStream* in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(g_proc));
    read_next(in);
    g_subprocess_wait_async(g_proc, g_cancel, on_exit, nullptr);
    set_controls();
    update_progress();
}

static void on_apply_choices(GtkButton*, gpointer);
static bool sudo_nopasswd();

static GtkWidget* problem_row(const std::string& icon, const std::string& text, bool retry_default, const std::string& key, bool pkg) {
    GtkWidget* row = hbox(10);
    gtk_widget_add_css_class(row, "srow");
    GtkWidget* l = label(icon + "  " + text, "pkg"); gtk_widget_set_hexpand(l, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(row), l);
    GtkWidget* a = gtk_toggle_button_new_with_label(pkg ? "Встановити ще раз" : "Повторити");
    GtkWidget* b = gtk_toggle_button_new_with_label("Пропустити");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(a));
    gtk_widget_add_css_class(a, "chipt"); gtk_widget_add_css_class(b, "chipt");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(retry_default ? a : b), TRUE);
    gtk_box_append(GTK_BOX(row), a); gtk_box_append(GTK_BOX(row), b);
    gtk_box_append(GTK_BOX(g_done_list), row);
    g_problems.push_back({key, pkg, a});
    return row;
}

static void finish_all() {
    if (g_logf.is_open()) g_logf.close();
    g_prog_target = 1;
    clear_box(g_done_list);
    g_problems.clear();

    if (R.rollback) {
        // відкат завершено — точки відкату більше немає
        unlink(state_file().c_str());
        bool lost = !R.failed_pkgs.empty();
        gtk_label_set_text(GTK_LABEL(g_done_icon), lost ? "󰀦" : "󰕍");
        if (lost) gtk_widget_add_css_class(g_done_icon, "warn"); else gtk_widget_remove_css_class(g_done_icon, "warn");
        gtk_label_set_text(GTK_LABEL(g_done_title), lost ? "Повернуто, але не все" : "Зміни скасовано — система як до початку");
        gtk_label_set_text(GTK_LABEL(g_done_sub), ("Пакети, файли, служби й налаштування повернуто до стану на момент старту.  Лог: " + R.log_path).c_str());
        for (auto& p : R.failed_pkgs) gtk_box_append(GTK_BOX(g_done_list), label("󰀦  " + p, "pkg"));
        gtk_widget_set_visible(g_done_list, lost);
        gtk_widget_set_visible(g_done_items_title, FALSE);
        gtk_widget_set_visible(g_done_apply, FALSE);
        gtk_widget_set_visible(g_done_cancel, FALSE);
        gtk_widget_set_visible(g_done_reboot, TRUE);
        gtk_label_set_text(GTK_LABEL(g_subtitle), "Скасовано");
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "done");
        return;
    }

    R.status = "done";
    save_state();
    // після перезавантаження в новій системі покажемо вікно «Все готово до роботи» (gui/welcome.sh)
    if (R.mode == Mode::Install || R.mode == Mode::Kiosk) {
        g_mkdir_with_parents(state_dir().c_str(), 0755);
        std::ofstream(state_dir() + "/welcome-pending") << (R.mode == Mode::Kiosk ? "kiosk" : "mgos") << "\n";
    }
    // проблеми, які ще не вирішені: пакети окремо, кроки з попередженнями (не через пакети) — окремо
    for (auto& p : R.failed_pkgs) if (!R.accepted.count("pkg:" + p))
        problem_row("󰅙", p + " — не встановився", true, p, true);
    for (auto& id : R.steps) if (R.warned.count(id) && !R.pkgwarn.count(id) && !R.accepted.count("step:" + id))
        if (auto d = step_def(id)) problem_row("󰀦", std::string(d->title) + " — з попередженнями", id != "verify" && id != "kiosk_verify", id, false);
    bool warn = !g_problems.empty();
    gtk_label_set_text(GTK_LABEL(g_done_icon), warn ? "󰀦" : "󰄬");
    if (warn) gtk_widget_add_css_class(g_done_icon, "warn"); else gtk_widget_remove_css_class(g_done_icon, "warn");
    gtk_label_set_text(GTK_LABEL(g_done_title), warn ? "Майже готово — виріши, що робити з проблемами" : (R.mode == Mode::Update ? "Систему оновлено" : R.mode == Mode::Kiosk ? "kioskOS готова!" : "Megumin OS 6 готова!"));
    std::string sub = std::string(mode_title(R.mode)) + " за " + fmt_time(R.elapsed) +
        (R.mode == Mode::Kiosk ? ".  Вхід: у меню сесій SDDM обери «kiosk» (або «mgOS»), з TTY — команда kiosk" : "") +
        ".  Не подобається результат — «Скасувати все» поверне систему як було.  Лог: " + R.log_path;
    gtk_label_set_text(GTK_LABEL(g_done_sub), sub.c_str());
    gtk_widget_set_visible(g_done_list, warn);
    gtk_widget_set_visible(g_done_items_title, warn);
    gtk_widget_set_visible(g_done_apply, warn);
    gtk_widget_set_visible(g_done_cancel, !R.cache.empty());
    gtk_widget_set_visible(g_done_reboot, R.mode != Mode::Update && !warn);
    gtk_label_set_text(GTK_LABEL(g_subtitle), "Готово");
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "done");
}

// ============================================================================
//  Сторінка виконання
// ============================================================================
static void build_run_rows() {
    clear_box(g_steps_box);
    g_rows.clear();
    for (auto& id : R.steps) {
        auto d = step_def(id);
        if (!d) continue;
        GtkWidget* row = hbox(10);
        gtk_widget_add_css_class(row, "srow");
        GtkWidget* ic = label("○", "sicon", 0.5);
        gtk_box_append(GTK_BOX(row), ic);
        gtk_box_append(GTK_BOX(row), label(std::string(d->icon) + "  " + d->title, "stitle"));
        gtk_box_append(GTK_BOX(g_steps_box), row);
        g_rows[id] = {ic, row};
        St st = R.skipped.count(id) ? St::Skipped : R.done.count(id) ? (R.warned.count(id) ? St::Warn : St::Done) : St::Pending;
        set_row(id, st);
    }
}

static void begin_run() {
    if (R.log_path.empty()) {
        char b[64]; time_t t = time(nullptr); strftime(b, sizeof b, "%Y%m%d_%H%M%S", localtime(&t));
        R.log_path = std::string(g_get_home_dir()) + "/mgOS_install_" + b + ".log";
    }
    if (!R.started) R.started = g_get_real_time();
    if (R.cache.empty()) {
        // точка відкату — у теці інсталятора (cache/), а якщо вона лише для читання — у ~/.local/state
        char b[64]; time_t t = time(nullptr); strftime(b, sizeof b, "%Y%m%d_%H%M%S", localtime(&t));
        std::string base = access(g_dir.c_str(), W_OK) == 0 ? g_dir + "/cache" : state_dir() + "/cache";
        R.cache = base + "/" + b;
    }
    g_logf.open(R.log_path, std::ios::app);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_log_view)), "", -1);
    log_line(std::string("== Megumin OS 6 — ") + mode_title(R.mode) + " ==");
    build_run_rows();
    g_pause_req = false; g_cancel_req = false;
    gtk_widget_set_visible(g_close_banner, FALSE);
    gtk_label_set_text(GTK_LABEL(g_subtitle), R.rollback ? "Повернення як було" : mode_title(R.mode));
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "run");
    g_prog_shown = done_weight(R) / std::max(1.0, total_weight(R));
    run_next();
}

static void on_pause(GtkButton*, gpointer) {
    if (g_running) { g_pause_req = true; set_controls(); return; }
    if (R.status == "paused") begin_run();
}
static void on_retry(GtkButton*, gpointer) { R.status = "running"; begin_run(); }
static void on_skip(GtkButton*, gpointer) {
    for (auto& id : R.steps) if (!R.done.count(id) && !R.skipped.count(id)) { R.skipped.insert(id); break; }
    R.status = "running";
    begin_run();
}

// Повернути все як було на момент старту
static void start_rollback() {
    if (!R.done.count("snapshot")) {   // точка відкату ще не створена — нічого не змінювалось
        unlink(state_file().c_str());
        R = RunState();
        show_home();
        return;
    }
    R.rollback = true;
    R.steps = {"rollback"};
    R.done.clear(); R.warned.clear(); R.skipped.clear(); R.failed_pkgs.clear(); R.pkgwarn.clear();
    R.status = "running";
    save_state();
    begin_run();
}
static void on_cancel(GtkButton*, gpointer) {
    if (g_running) { g_cancel_req = true; set_controls(); return; }
    start_rollback();
}

static void on_apply_choices(GtkButton*, gpointer) {
    std::string retry;
    bool rerun = false;
    for (auto& p : g_problems) {
        bool again = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p.retry));
        if (p.pkg) {
            if (again) retry += p.key + " "; else R.accepted.insert("pkg:" + p.key);
        } else {
            if (again) { R.done.erase(p.key); R.warned.erase(p.key); rerun = true; } else R.accepted.insert("step:" + p.key);
        }
    }
    if (!retry.empty()) {
        R.opt["OPT_RETRY"] = trim(retry);
        std::vector<std::string> keep;
        for (auto& f : R.failed_pkgs) if (retry.find(f + " ") == std::string::npos) keep.push_back(f);
        R.failed_pkgs = keep;
        R.pkgwarn.clear();
        auto it = std::find(R.steps.begin(), R.steps.end(), "retry_pkgs");
        if (it == R.steps.end()) {
            auto v = std::find(R.steps.begin(), R.steps.end(), "verify");
            R.steps.insert(v, "retry_pkgs");
        }
        R.done.erase("retry_pkgs"); R.warned.erase("retry_pkgs");
        rerun = true;
    }
    if (rerun) {
        R.done.erase("verify"); R.warned.erase("verify"); R.status = "running"; save_state();
        // пароля в пам'яті немає (напр., раніше його «пропустив» кеш sudo) — спершу запитати, інакше все знову впаде
        if (g_password.empty() && !sudo_nopasswd()) { open_options(R.mode, true); return; }
        begin_run();
    }
    else { save_state(); finish_all(); }
}

// ============================================================================
//  Сторінка параметрів
// ============================================================================
static GtkWidget* opt_switch(const std::string& key, const char* title, const char* desc, bool on) {
    GtkWidget* row = hbox(12);
    gtk_widget_add_css_class(row, "opt-row");
    GtkWidget* col = vbox(2);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_box_append(GTK_BOX(col), label(title, "opt-title"));
    GtkWidget* d = label(desc, "opt-desc"); gtk_label_set_wrap(GTK_LABEL(d), TRUE);
    gtk_box_append(GTK_BOX(col), d);
    gtk_box_append(GTK_BOX(row), col);
    GtkWidget* sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), on);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), sw);
    g_switches[key] = sw;
    return row;
}

static GtkWidget* chips(const std::vector<std::string>& items, std::vector<std::pair<std::string, GtkWidget*>>& out, bool on) {
    GtkWidget* fb = gtk_flow_box_new();
    gtk_widget_add_css_class(fb, "chips");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fb), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(fb), 20);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(fb), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fb), 6);
    out.clear();
    for (auto& s : items) {
        GtkWidget* t = gtk_toggle_button_new_with_label(s.c_str());
        gtk_widget_add_css_class(t, "chipt");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), on);
        gtk_flow_box_append(GTK_FLOW_BOX(fb), t);
        out.push_back({s, t});
    }
    return fb;
}

// виклик функції зі steps.sh без root (списки пакетів зі знімка)
static std::vector<std::string> steps_list(const char* fn) {
    std::string cmd = std::string("source \"$MGOS_DIR/gui/steps.sh\" && ") + fn;
    const char* argv[] = {"bash", "-c", cmd.c_str(), nullptr};
    std::string env_dir = "MGOS_DIR=" + g_dir;
    gchar** envp = g_get_environ();
    envp = g_environ_setenv(envp, "MGOS_DIR", g_dir.c_str(), TRUE);
    gchar* out = nullptr;
    g_spawn_sync(g_dir.c_str(), (gchar**)argv, envp, (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL), nullptr, nullptr, &out, nullptr, nullptr, nullptr);
    g_strfreev(envp);
    std::vector<std::string> r; std::istringstream is(out ? out : ""); std::string l;
    while (std::getline(is, l)) if (!trim(l).empty()) r.push_back(trim(l));
    g_free(out);
    return r;
}

// Пароль не потрібен ЛИШЕ при справжньому NOPASSWD у sudoers. «-k» змушує sudo ігнорувати кеш:
// раніше «sudo -n true» проходив через нещодавно введений у терміналі пароль, поле пароля ховалось,
// а через ~5 хв кеш закінчувався — і всі встановлення падали на «3 incorrect password attempts».
static bool sudo_nopasswd() {
    gint st = 1;
    const char* argv[] = {"sudo", "-n", "-k", "true", nullptr};
    g_spawn_sync(nullptr, (gchar**)argv, nullptr, (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                 nullptr, nullptr, nullptr, nullptr, &st, nullptr);
    return g_spawn_check_wait_status(st, nullptr);
}

// аналіз застарілих пакетів mgOS (режим «Оновлення»)
static void run_analyze() {
    clear_box(g_upd_list);
    g_upd_count = 0;
    gtk_label_set_text(GTK_LABEL(g_upd_status), "⠋  Аналізую пакети mgOS…");
    gtk_widget_set_sensitive(g_start_btn, FALSE);
    GSubprocessLauncher* l = make_launcher(false);
    GSubprocess* p = spawn_step("analyze", l);
    g_object_unref(l);
    if (!p) return;
    struct Ctx { int out = 0, miss = 0; };
    auto* ctx = new Ctx;
    GDataInputStream* in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(p));
    struct Reader {
        static void next(GDataInputStream* in, Ctx* c) {
            g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, nullptr, [](GObject* s, GAsyncResult* r, gpointer d) {
                auto* c = (Ctx*)d;
                char* line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(s), r, nullptr, nullptr);
                if (!line) {
                    g_object_unref(s);
                    std::string t;
                    if (c->out + c->miss == 0) t = "󰄬  Усі пакети mgOS актуальні та встановлені";
                    else t = "Застарілих: " + std::to_string(c->out) + "   ·   відсутніх: " + std::to_string(c->miss);
                    gtk_label_set_text(GTK_LABEL(g_upd_status), t.c_str());
                    g_upd_count = c->out + c->miss;
                    set_btn_label(g_start_btn, g_upd_count ? "󰚰  Оновити (" + std::to_string(g_upd_count) + ")" : "󰚰  Оновити все одно");
                    gtk_widget_set_sensitive(g_start_btn, TRUE);
                    delete c;
                    return;
                }
                std::string L = line; g_free(line);
                std::istringstream is(L); std::string tag, n, o, v, src;
                is >> tag >> n >> o >> v >> src;
                GtkWidget* row = nullptr;
                if (tag == "@@OUT") {
                    c->out++;
                    row = hbox(8);
                    GtkWidget* nl = label(n, "pkg"); gtk_widget_set_hexpand(nl, TRUE);
                    gtk_box_append(GTK_BOX(row), nl);
                    if (src == "aur") { GtkWidget* t = label("AUR", "tag"); gtk_widget_set_valign(t, GTK_ALIGN_CENTER); gtk_box_append(GTK_BOX(row), t); }
                    GtkWidget* vl = label(o + "  →  " + v, "pkg"); gtk_widget_set_opacity(vl, 0.6);
                    gtk_box_append(GTK_BOX(row), vl);
                } else if (tag == "@@MISS") {
                    c->miss++;
                    row = hbox(8);
                    GtkWidget* nl = label(n, "pkg"); gtk_widget_set_hexpand(nl, TRUE);
                    gtk_box_append(GTK_BOX(row), nl);
                    GtkWidget* t = label("НЕМАЄ", "tag"); gtk_widget_set_valign(t, GTK_ALIGN_CENTER); gtk_box_append(GTK_BOX(row), t);
                } else if (tag == "@@MSG") {
                    gtk_label_set_text(GTK_LABEL(g_upd_status), ("⠋  " + L.substr(6) + "…").c_str());
                }
                if (row) gtk_box_append(GTK_BOX(g_upd_list), row);
                next(G_DATA_INPUT_STREAM(s), c);
            }, c);
        }
    };
    Reader::next(in, ctx);
    g_object_unref(p);
}

static void open_options(Mode m, bool resume) {
    g_opt_mode = m; g_opt_resume = resume; g_opt_rollback = false;
    clear_box(g_opt_body);
    g_switches.clear(); g_app_toggles.clear(); g_cfg_toggles.clear();
    g_upd_list = g_upd_status = nullptr;
    gtk_label_set_text(GTK_LABEL(g_opt_title), resume ? (std::string("Продовжити: ") + mode_title(m)).c_str() : mode_title(m));
    set_btn_label(g_start_btn, resume ? "󰐊  Продовжити" : "󰐊  Далі");
    gtk_widget_set_sensitive(g_start_btn, TRUE);

    if (resume) {
        int n = R.steps.size(), d = R.done.size() + R.skipped.size();
        std::string next;
        for (auto& id : R.steps) if (!R.done.count(id) && !R.skipped.count(id)) { next = id; break; }
        auto sd = step_def(next);
        gtk_label_set_text(GTK_LABEL(g_opt_desc), ("Виконано " + std::to_string(d) + " з " + std::to_string(n) +
            " кроків. Продовжу з «" + (sd ? sd->title : next) + "» — попередні кроки повторювати не треба.").c_str());
    } else if (m == Mode::Install) {
        gtk_label_set_text(GTK_LABEL(g_opt_desc), "Повне встановлення Megumin OS поверх Arch Linux: пакети, панелі SYSui, тема, служби. Старі конфіги зберігаються в бекап.");
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("OPT_UPDATE", "Спершу оновити систему", "pacman -Syu перед встановленням (рекомендовано)", true));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("OPT_WALL", "Скопіювати шпалери", "Колекція шпалер mgOS у ~/Pictures/wallpapers (кілька ГБ)", true));
        auto drv = steps_list("drivers_here");
        std::string drv_s; for (auto& x : drv) drv_s += (drv_s.empty() ? "" : ", ") + x;
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("OPT_DRIVERS", "Драйвери під це залізо",
            drv.empty() ? "Для цього ПК додаткових драйверів не треба" : ("Визначено автоматично: " + drv_s).c_str(), !drv.empty()));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("OPT_DEV", "Інструменти розробки", "git, cmake, rust, nodejs, python-бібліотеки, docker, ollama…", false));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("OPT_FISH", "Оболонка fish", "Зробити fish оболонкою за замовчуванням (у знімку — bash)", false));
        auto apps = steps_list("snapshot_apps");
        gtk_box_append(GTK_BOX(g_opt_body), label(("Програми зі знімка (" + std::to_string(apps.size()) + ")").c_str(), "h2"));
        gtk_box_append(GTK_BOX(g_opt_body), chips(apps, g_app_toggles, true));
        std::vector<std::string> cfgs;
        if (GDir* d = g_dir_open((g_dir + "/configs").c_str(), 0, nullptr)) {
            while (const char* n = g_dir_read_name(d)) if (g_file_test((g_dir + "/configs/" + n).c_str(), G_FILE_TEST_IS_DIR)) cfgs.push_back(n);
            g_dir_close(d);
        }
        std::sort(cfgs.begin(), cfgs.end());
        gtk_box_append(GTK_BOX(g_opt_body), label("Конфіги (~/.config)", "h2"));
        gtk_box_append(GTK_BOX(g_opt_body), chips(cfgs, g_cfg_toggles, true));
    } else if (m == Mode::Kiosk) {
        std::string ks = trim(read_text(g_dir + "/kiosk/files/snapshot_date.txt"));
        bool have = g_file_test((g_dir + "/kiosk/files/config/hyprland/kiosk.conf").c_str(), G_FILE_TEST_EXISTS);
        gtk_label_set_text(GTK_LABEL(g_opt_desc), have
            ? ("Енергоощадна kiosk-сесія Hyprland 1 в 1 як на kiosk-машині (знімок від " + ks + "): без анімацій, блюру й шпалер, "
               "waybar з індикатором живлення, Spotlight Runner, оптимізований Firefox. Ставиться поруч із mgOS: економія, тема й налаштування Firefox "
               "діють лише в kiosk-сесії, у mgOS ПК працює на повну.").c_str()
            : "У інсталяторі немає знімка kioskOS. Запусти kiosk/sync_kiosk.sh на kiosk-машині, щоб його створити.");
        gtk_widget_set_sensitive(g_start_btn, have);
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("OPT_UPDATE", "Спершу оновити систему", "pacman -Syu перед встановленням (рекомендовано)", true));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("OPT_KIOSK_THEME", "Тема Graphite, іконки MacTahoe, шрифт SF Compact",
            "Темний матовий GTK3/4 і Nautilus без фризів — вмикається лише в kiosk-сесії, тема mgOS лишається своя", true));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("OPT_KIOSK_EXT", "Розширення Firefox",
            "uBlock Origin, The Tab Suspender, SponsorBlock, YouTube Shorts Block, укр. словник (policies.json, діють в обох сесіях)", true));
        std::string kp = trim(read_text(g_dir + "/kiosk/files/kernel_params.txt"));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("OPT_KIOSK_KERNEL", "Параметри ядра",
            (kp.empty() ? std::string("у знімку немає") : kp + " — менше переривань CPU, тихе завантаження (GRUB)").c_str(), !kp.empty()));
        gtk_box_append(GTK_BOX(g_opt_body), label("Поруч із mgOS нічого не ламається", "h2"));
        GtkWidget* coex = label("󰌪  power-saver — лише поки працює kiosk; вийшов чи kiosk впав — профіль mgOS повертається сам\n"
                                "󰈹  Firefox: RAM-кеш, VA-API, авто-сон вкладок — лише в kiosk (той самий профіль, вкладки спільні)\n"
                                "󰸉  тема, іконки, курсор, gsettings — на час kiosk-сесії, потім знову тема mgOS\n"
                                "󰅟  Google Drive: одне монтування на сесію (у mgOS — своє, у kiosk — VFS-кеш)",
                                "opt-desc");
        gtk_label_set_wrap(GTK_LABEL(coex), TRUE);
        gtk_box_append(GTK_BOX(g_opt_body), coex);
        GtkWidget* inc = label("󰄬  kiosk.conf + touchpad.conf (жести, плавний скрол, VFR/scanout як на kiosk-машині)\n"
                               "󰄬  waybar kiosk + анімований модуль гучності · wofi · safe-close (Super+Q не вбиває браузер)\n"
                               "󰄬  Spotlight Runner і Kiosk Dock (C++, збираються під цю систему) · carpeted\n"
                               "󰄬  Firefox: RAM-кеш 512 МБ, VA-API, авто-сон вкладок, кінетичний скрол (autoconfig, лише в kiosk)\n"
                               "󰄬  udev: тачпад без авто-сну · power-profiles-daemon (Super+F4) · Google Drive (rclone)\n"
                               "󰄬  intel-media-driver на Intel (апаратне декодування відео) · пункти «mgOS» і «kiosk» у меню сесій",
                               "opt-desc");
        gtk_label_set_wrap(GTK_LABEL(inc), TRUE);
        gtk_box_append(GTK_BOX(g_opt_body), label("Завжди встановлюється", "h2"));
        gtk_box_append(GTK_BOX(g_opt_body), inc);
    } else if (m == Mode::Restore) {
        gtk_label_set_text(GTK_LABEL(g_opt_desc), "Повертає систему до стану знімка mgOS: якщо щось зламалось у конфігах, панелях чи темі. Поточні конфіги переносяться в бекап, нічого не видаляється.");
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("R_PKGS", "Пакети", "Доставити відсутні пакети mgOS, утиліти та драйвери під це залізо", true));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("R_CFG", "Конфіги та панелі SYSui", "~/.config і домашні файли зі знімка, перезбірка панелей, прибирання мертвих автозапусків, масштаб під екран і жести тачпада", true));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("R_THEME", "Тема та кольори", "pywal, курсор, GTK-тема, шрифти", true));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("R_SYS", "Системне", "Утиліти /usr/local/bin, служби, SDDM, sudoers", true));
        gtk_box_append(GTK_BOX(g_opt_body), opt_switch("R_BOOT", "Plymouth та initramfs", "Тема завантаження і перегенерація initramfs (довго)", false));
    } else {
        gtk_label_set_text(GTK_LABEL(g_opt_desc), "Оновлює застарілі пакети mgOS і доставляє відсутні. Репозиторії оновлюються лише повністю (pacman -Syu) — часткові оновлення в Arch ламають систему.");
        g_upd_status = label("", "dimtxt");
        gtk_box_append(GTK_BOX(g_opt_body), g_upd_status);
        GtkWidget* sc = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sc), 230);
        gtk_widget_add_css_class(sc, "steps-box");
        g_upd_list = vbox(0);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), g_upd_list);
        fz_attach(sc);
        gtk_box_append(GTK_BOX(g_opt_body), sc);
        run_analyze();
    }

    bool need_pw = !sudo_nopasswd();
    gtk_widget_set_visible(g_pw_box, need_pw);
    gtk_editable_set_text(GTK_EDITABLE(g_pw_entry), "");
    gtk_label_set_text(GTK_LABEL(g_pw_err), "");
    gtk_label_set_text(GTK_LABEL(g_subtitle), mode_title(m));
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "options");
    if (need_pw) gtk_widget_grab_focus(g_pw_entry);
}

static void start_with_options() {
    if (g_opt_rollback) { g_opt_rollback = false; start_rollback(); return; }
    if (g_opt_resume) { begin_run(); return; }
    R = RunState();
    R.mode = g_opt_mode;
    auto on = [](const char* k) { auto it = g_switches.find(k); return it != g_switches.end() && gtk_switch_get_active(GTK_SWITCH(it->second)); };
    auto& S = R.steps;
    if (R.mode == Mode::Install) {
        R.opt["OPT_UPDATE"] = on("OPT_UPDATE") ? "1" : "0";
        R.opt["OPT_WALL"] = on("OPT_WALL") ? "1" : "0";
        R.opt["OPT_DEV"] = on("OPT_DEV") ? "1" : "0";
        R.opt["OPT_DRIVERS"] = on("OPT_DRIVERS") ? "1" : "0";
        std::string apps, cfgs;
        for (auto& [n, t] : g_app_toggles) if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(t))) apps += n + " ";
        bool all = true;
        for (auto& [n, t] : g_cfg_toggles) { if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(t))) cfgs += n + " "; else all = false; }
        R.opt["OPT_APPS"] = trim(apps);
        R.opt["OPT_CONFIGS"] = all ? "" : (cfgs.empty() ? "-" : trim(cfgs));
        S = {"preflight", "snapshot", "yay", "repos"};
        if (on("OPT_UPDATE")) S.push_back("sysupdate");
        for (auto x : {"core", "stable", "aur", "utils"}) S.push_back(x);
        if (on("OPT_DRIVERS")) S.push_back("drivers");
        if (!R.opt["OPT_APPS"].empty()) S.push_back("apps");
        if (on("OPT_DEV")) S.push_back("dev");
        S.push_back("files");
        if (R.opt["OPT_CONFIGS"] != "-") { S.push_back("configs"); S.push_back("home"); S.push_back("build"); }
        for (auto x : {"theme", "boot", "scripts", "services", "look"}) S.push_back(x);
        if (on("OPT_FISH")) S.push_back("shell");
        S.push_back("gpu");
        S.push_back("hw");   // масштаб під екран, жести тачпада з kiosk, розкладки
        if (R.opt["OPT_CONFIGS"] != "-") S.push_back("cleanup");
        S.push_back("verify");
    } else if (R.mode == Mode::Restore) {
        R.opt["OPT_WALL"] = "0";
        S = {"preflight", "snapshot"};
        if (on("R_PKGS")) for (auto x : {"yay", "repos", "core", "stable", "aur", "utils", "drivers"}) S.push_back(x);
        if (on("R_CFG")) { S.push_back("configs"); S.push_back("home"); S.push_back("build"); S.push_back("cleanup"); S.push_back("hw"); }
        if (on("R_THEME")) { S.push_back("theme"); S.push_back("look"); }
        if (on("R_SYS")) { S.push_back("scripts"); S.push_back("services"); if (!on("R_THEME")) S.push_back("look"); }
        if (on("R_BOOT")) S.push_back("boot");
        S.push_back("verify");
    } else if (R.mode == Mode::Kiosk) {
        R.opt["OPT_UPDATE"] = on("OPT_UPDATE") ? "1" : "0";
        R.opt["OPT_KIOSK_THEME"] = on("OPT_KIOSK_THEME") ? "1" : "0";
        R.opt["OPT_KIOSK_EXT"] = on("OPT_KIOSK_EXT") ? "1" : "0";
        R.opt["OPT_KIOSK_KERNEL"] = on("OPT_KIOSK_KERNEL") ? "1" : "0";
        S = {"preflight", "snapshot", "yay"};
        if (on("OPT_UPDATE")) S.push_back("sysupdate");
        for (auto x : {"kiosk_pkgs", "kiosk_extra", "kiosk_files", "kiosk_build", "kiosk_firefox"}) S.push_back(x);
        if (on("OPT_KIOSK_THEME")) S.push_back("kiosk_theme");
        S.push_back("kiosk_system");
        S.push_back("kiosk_hw");
        if (on("OPT_KIOSK_KERNEL")) S.push_back("kiosk_kernel");
        S.push_back("kiosk_verify");
    } else {
        S = {"preflight", "snapshot", "yay", "repos", "upd_repo", "upd_aur", "upd_missing", "build", "verify"};
    }
    begin_run();
}

static void on_start(GtkButton*, gpointer) {
    if (!gtk_widget_get_visible(g_pw_box)) { g_password.clear(); start_with_options(); return; }
    g_password = gtk_editable_get_text(GTK_EDITABLE(g_pw_entry));
    if (g_password.empty()) { gtk_label_set_text(GTK_LABEL(g_pw_err), "Введи пароль користувача (для sudo)"); return; }
    // перевіряємо пароль асинхронно: sudo -A -k true
    gtk_widget_set_sensitive(g_start_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_pw_err), "Перевіряю пароль…");
    GSubprocessLauncher* l = make_launcher(false);
    GError* err = nullptr;
    GSubprocess* p = g_subprocess_launcher_spawn(l, &err, "/usr/bin/sudo", "-A", "-k", "true", nullptr);
    g_object_unref(l);
    if (!p) { gtk_label_set_text(GTK_LABEL(g_pw_err), err ? err->message : "sudo не запустився"); if (err) g_error_free(err); gtk_widget_set_sensitive(g_start_btn, TRUE); return; }
    g_subprocess_wait_async(p, nullptr, [](GObject* s, GAsyncResult* r, gpointer) {
        g_subprocess_wait_finish(G_SUBPROCESS(s), r, nullptr);
        bool ok = g_subprocess_get_if_exited(G_SUBPROCESS(s)) && g_subprocess_get_exit_status(G_SUBPROCESS(s)) == 0;
        g_object_unref(s);
        gtk_widget_set_sensitive(g_start_btn, TRUE);
        if (!ok) { gtk_label_set_text(GTK_LABEL(g_pw_err), "Невірний пароль"); g_password.clear(); return; }
        gtk_label_set_text(GTK_LABEL(g_pw_err), "");
        start_with_options();
    }, nullptr);
}

// ============================================================================
//  Перевірка системи
// ============================================================================
static std::set<std::string> g_chk_fixes;   // які перемикачі відновлення увімкнути

static void chk_row(const std::string& lvl, const std::string& name, const std::string& detail) {
    GtkWidget* row = hbox(10);
    gtk_widget_add_css_class(row, "srow");
    GtkWidget* ic = label(lvl == "ok" ? "󰄬" : lvl == "warn" ? "󰀦" : "󰅙", "sicon", 0.5);
    gtk_widget_add_css_class(ic, lvl == "ok" ? "done" : lvl == "warn" ? "warn" : "failed");
    gtk_widget_set_valign(ic, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), ic);
    GtkWidget* col = vbox(1); gtk_widget_set_hexpand(col, TRUE);
    gtk_box_append(GTK_BOX(col), label(name, "opt-title"));
    GtkWidget* d = label(detail, "opt-desc"); gtk_label_set_wrap(GTK_LABEL(d), TRUE); gtk_label_set_wrap_mode(GTK_LABEL(d), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(col), d);
    gtk_box_append(GTK_BOX(row), col);
    gtk_box_append(GTK_BOX(g_chk_list), row);
}

static void run_check() {
    clear_box(g_chk_list);
    g_chk_fixes.clear(); g_chk_updates = 0; g_health = -1;
    gtk_widget_set_visible(g_chk_fix, FALSE);
    gtk_widget_set_visible(g_chk_upd, FALSE);
    gtk_widget_set_sensitive(g_chk_again, FALSE);
    gtk_label_set_text(GTK_LABEL(g_chk_sum), "Перевіряю…");
    GSubprocessLauncher* l = make_launcher(false);
    GSubprocess* p = spawn_step("check", l);
    g_object_unref(l);
    if (!p) return;
    struct Ctx { int ok = 0, warn = 0, fail = 0; std::string group; };
    struct Rd {
        static void next(GDataInputStream* in, Ctx* c) {
            g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, nullptr, [](GObject* s, GAsyncResult* r, gpointer d) {
                auto* c = (Ctx*)d;
                char* line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(s), r, nullptr, nullptr);
                if (!line) {
                    g_object_unref(s);
                    int total = c->ok + c->warn + c->fail;
                    g_health = total ? (int)std::lround(100.0 * (c->ok + 0.5 * c->warn) / total) : 0;
                    std::string sum = std::to_string(total) + " перевірок  ·  " + std::to_string(c->fail) + " проблем  ·  " +
                                      std::to_string(c->warn) + " попереджень";
                    if (c->fail + c->warn == 0) sum = "󰄬  Усе гаразд — система відповідає mgOS.  " + std::to_string(total) + " перевірок";
                    gtk_label_set_text(GTK_LABEL(g_chk_sum), sum.c_str());
                    gtk_widget_set_visible(g_chk_fix, !g_chk_fixes.empty());
                    gtk_widget_set_visible(g_chk_upd, g_chk_updates > 0);
                    gtk_widget_set_sensitive(g_chk_again, TRUE);
                    delete c;
                    return;
                }
                std::string L = line; g_free(line);
                if (L.rfind("@@MSG ", 0) == 0) gtk_label_set_text(GTK_LABEL(g_chk_sum), ("Перевіряю: " + L.substr(6) + "…").c_str());
                else if (L.rfind("@@CHECK ", 0) == 0) {
                    std::string rest = L.substr(8);
                    std::string lvl = rest.substr(0, rest.find(' '));
                    rest = rest.substr(rest.find(' ') + 1);
                    std::string grp = rest.substr(0, rest.find('|')); rest = rest.substr(rest.find('|') + 1);
                    std::string name = rest.substr(0, rest.find('|'));
                    std::string det = rest.find('|') != std::string::npos ? rest.substr(rest.find('|') + 1) : "";
                    if (grp != c->group) {
                        c->group = grp;
                        GtkWidget* h = label(grp, "h2");
                        gtk_widget_set_margin_top(h, gtk_widget_get_first_child(g_chk_list) ? 10 : 2);
                        gtk_widget_set_margin_start(h, 6);
                        gtk_box_append(GTK_BOX(g_chk_list), h);
                    }
                    chk_row(lvl, name, det);
                    (lvl == "ok" ? c->ok : lvl == "warn" ? c->warn : c->fail)++;
                    if (lvl != "ok") {
                        if (grp == "Пакети" && name == "Оновлення") g_chk_updates++;
                        else if (grp == "Пакети" && name != "Сироти" && name != "Кеш pacman") g_chk_fixes.insert("R_PKGS");
                        else if (grp == "Панелі SYSui" || grp == "Конфіги" || (grp == "Hyprland" && (name == "Автозапуск" || name == "Масштаб екранів" || name == "Жести тачпада" || name.rfind("Екран ", 0) == 0))) g_chk_fixes.insert("R_CFG");
                        else if (grp == "Тема") g_chk_fixes.insert("R_THEME");
                        else if (grp == "Система" && (name == "Служби зі знімка" || name == "SDDM")) g_chk_fixes.insert("R_SYS");
                        else if (grp == "Система" && name == "multilib") g_chk_fixes.insert("R_PKGS");
                    }
                }
                next(G_DATA_INPUT_STREAM(s), c);
            }, c);
        }
    };
    Rd::next(g_data_input_stream_new(g_subprocess_get_stdout_pipe(p)), new Ctx);
    g_object_unref(p);
}

static void open_check() {
    gtk_label_set_text(GTK_LABEL(g_subtitle), "Перевірка системи");
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "check");
    run_check();
}

static GtkWidget* build_check() {
    GtkWidget* p = vbox(12);
    gtk_widget_add_css_class(p, "page");
    GtkWidget* top = hbox(14);
    GtkWidget* tl = vbox(4); gtk_widget_set_hexpand(tl, TRUE);
    gtk_box_append(GTK_BOX(tl), label("Перевірка системи", "h1"));
    GtkWidget* d = label("Порівнює цей ПК зі знімком mgOS: пакети, панелі, конфіги, тему, Hyprland і служби. Нічого не змінює.", "dimtxt");
    gtk_label_set_wrap(GTK_LABEL(d), TRUE);
    gtk_box_append(GTK_BOX(tl), d);
    g_chk_sum = label("", "msg");
    gtk_label_set_wrap(GTK_LABEL(g_chk_sum), TRUE);
    gtk_box_append(GTK_BOX(tl), g_chk_sum);
    gtk_box_append(GTK_BOX(top), tl);
    g_score = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_score), 136);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_score), 44);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_score), [](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
        draw_pixel_text(cr, g_health < 0 ? "--%" : std::to_string(g_health) + "%", w, h, 5, 0);
    }, nullptr, nullptr);
    gtk_widget_set_valign(g_score, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(top), g_score);
    gtk_box_append(GTK_BOX(p), top);

    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_widget_add_css_class(sc, "steps-box");
    g_chk_list = vbox(1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), g_chk_list);
    fz_attach(sc);
    gtk_box_append(GTK_BOX(p), sc);

    GtkWidget* btns = hbox(8);
    gtk_box_append(GTK_BOX(btns), button("󰁍  Назад", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) { show_home(); })));
    g_chk_again = button("󰑓  Ще раз", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) { run_check(); }));
    gtk_box_append(GTK_BOX(btns), g_chk_again);
    gtk_box_append(GTK_BOX(btns), spacer());
    g_chk_upd = button("󰚰  Оновити пакети", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) { open_options(Mode::Update, false); }));
    gtk_box_append(GTK_BOX(btns), g_chk_upd);
    g_chk_fix = button("󰁯  Виправити", "primary", G_CALLBACK(+[](GtkButton*, gpointer) {
        open_options(Mode::Restore, false);
        // вмикаємо лише те, що реально зламано
        for (auto& [k, sw] : g_switches) gtk_switch_set_active(GTK_SWITCH(sw), g_chk_fixes.count(k) > 0);
    }));
    gtk_box_append(GTK_BOX(btns), g_chk_fix);
    gtk_box_append(GTK_BOX(p), btns);
    return p;
}

// ============================================================================
//  Головна
// ============================================================================
static GtkWidget *g_resume_title, *g_resume_go;
static void refresh_resume() {
    RunState s;
    bool loaded = load_state(s);
    bool unfinished = loaded && s.status != "done";
    bool undoable = loaded && s.status == "done" && !s.cache.empty() && g_file_test((s.cache + "/pkgs.before").c_str(), G_FILE_TEST_EXISTS);
    gtk_widget_set_visible(g_resume_box, unfinished || undoable);
    gtk_widget_set_visible(g_resume_go, unfinished);
    if (undoable) {
        gtk_label_set_text(GTK_LABEL(g_resume_title), "󰕍  Останню операцію можна скасувати");
        gtk_label_set_text(GTK_LABEL(g_resume_lbl), (std::string(mode_title(s.mode)) + " завершено. Якщо щось не так — «Повернути як було» відкотить пакети, файли й налаштування до стану перед ним.").c_str());
        return;
    }
    if (!unfinished) return;
    int n = s.steps.size(), d = s.done.size() + s.skipped.size();
    std::string next;
    for (auto& id : s.steps) if (!s.done.count(id) && !s.skipped.count(id)) { next = id; break; }
    auto sd = step_def(next);
    int pct = (int)(100 * done_weight(s) / std::max(1.0, total_weight(s)));
    const char* why = s.rollback ? "(повернення як було) перервано" : s.status == "failed" ? "зупинилось з помилкою" : s.status == "paused" ? "на паузі" : "перервано";
    gtk_label_set_text(GTK_LABEL(g_resume_title), "󰁯  Незавершена операція");
    gtk_label_set_text(GTK_LABEL(g_resume_lbl), (std::string(mode_title(s.mode)) + " " + why + " — " + std::to_string(pct) + "%, крок " +
        std::to_string(d + 1) + "/" + std::to_string(n) + ": «" + (sd ? sd->title : next) + "»").c_str());
}

static void open_rollback() {
    if (!load_state(R)) return;
    open_options(R.mode, true);
    g_opt_rollback = true;
    gtk_label_set_text(GTK_LABEL(g_opt_title), "Повернути як було");
    gtk_label_set_text(GTK_LABEL(g_opt_desc), "Пакети повернуться до версій на момент старту (додані — видаляться), конфіги, домашні файли, тема, служби й налаштування — з точки відкату в теці cache/ інсталятора.");
    set_btn_label(g_start_btn, "󰕍  Повернути");
}

static void show_home() {
    refresh_resume();
    gtk_label_set_text(GTK_LABEL(g_subtitle), "Інсталятор · встановлення, відновлення, оновлення, kioskOS");
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "home");
}

static GtkWidget* mode_card(const char* icon, const char* title, const char* desc, const std::string& meta, Mode m, int wchars = 22) {
    GtkWidget* b = gtk_button_new();
    gtk_widget_add_css_class(b, "mode-card");
    gtk_widget_set_cursor_from_name(b, "pointer");
    gtk_widget_set_hexpand(b, TRUE);
    GtkWidget* col = vbox(8);
    GtkWidget* ic = label(icon, "mode-icon", 0.5);
    gtk_widget_set_halign(ic, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(col), ic);
    gtk_box_append(GTK_BOX(col), label(title, "mode-title"));
    GtkWidget* d = label(desc, "mode-desc"); gtk_label_set_wrap(GTK_LABEL(d), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(d), wchars);
    gtk_widget_set_vexpand(d, TRUE); gtk_widget_set_valign(d, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(col), d);
    gtk_box_append(GTK_BOX(col), label(meta, "small"));
    gtk_button_set_child(GTK_BUTTON(b), col);
    g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        Mode m = (Mode)GPOINTER_TO_INT(d);
        if (m == Mode::Check) open_check(); else open_options(m, false);
    }),
                     GINT_TO_POINTER((int)m));
    return b;
}

// ============================================================================
//  Побудова вікна
// ============================================================================
// ---------- «Все готово до роботи» — перший вхід після встановлення (MGOS_WELCOME=mgos|kiosk) ----------
static GtkWidget* build_welcome() {
    std::string kind = g_getenv("MGOS_WELCOME") ? g_getenv("MGOS_WELCOME") : "mgos";
    bool kiosk = kind == "kiosk";
    GtkWidget* p = vbox(12);
    gtk_widget_add_css_class(p, "page");
    gtk_widget_set_valign(p, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(p), label("󰄬", "big-icon", 0.5));
    gtk_box_append(GTK_BOX(p), label("Все готово до роботи", "h1", 0.5));
    GtkWidget* sub = label(kiosk ? "kioskOS встановлено. Тиха, енергоощадна сесія — для браузера, документів і довгої роботи від батареї."
                                 : "Megumin OS 6 встановлено і налаштовано під цей комп'ютер. Ласкаво просимо!", "dimtxt", 0.5);
    gtk_label_set_wrap(GTK_LABEL(sub), TRUE); gtk_label_set_justify(GTK_LABEL(sub), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(p), sub);

    // екран: що підібрав mgos-display
    std::string scr;
    gchar* out = nullptr;
    if (g_spawn_command_line_sync("/usr/local/bin/mgos-display detect", &out, nullptr, nullptr, nullptr) && out) {
        std::istringstream is(out); std::string l;
        while (std::getline(is, l)) { auto a = l.find("→ "); if (a != std::string::npos) scr += (scr.empty() ? "" : "\n") + trim(l.substr(0, l.find(' '))) + "  " + trim(l.substr(a + 4)); }
    }
    g_free(out);

    GtkWidget* tips = vbox(6);
    gtk_widget_set_margin_top(tips, 8);
    auto tip = [&](const char* ic, const std::string& t, const std::string& d) {
        GtkWidget* r = hbox(12); gtk_widget_add_css_class(r, "opt-row");
        gtk_box_append(GTK_BOX(r), label(ic, "opt-title"));
        GtkWidget* c = vbox(2);
        gtk_box_append(GTK_BOX(c), label(t, "opt-title"));
        GtkWidget* dl = label(d, "opt-desc"); gtk_label_set_wrap(GTK_LABEL(dl), TRUE); gtk_box_append(GTK_BOX(c), dl);
        gtk_box_append(GTK_BOX(r), c);
        gtk_box_append(GTK_BOX(tips), r);
    };
    if (kiosk) {
        tip("󰍉", "Super + Space", "Spotlight Runner: програми, файли, калькулятор");
        tip("󰒲", "Super + F4", "Режим живлення: економ → баланс → продуктивний");
    } else {
        tip("󰍉", "Alt + Space", "Runner: програми, файли, команди");
        tip("󰸉", "Шпалери й кольори", "Тема підлаштовується під шпалери (pywal) — змінюй у налаштуваннях");
    }
    tip("󰟸", "Жести тачпада", "3 пальці вбік — робочі столи, вгору/вниз — Runner");
    if (!scr.empty()) tip("󰍹", "Екран налаштовано автоматично", scr + "\nСвій масштаб: ~/.config/mgos/display.conf");
    tip("󰂚", "Оновлення", "Коли на GitHub з'явиться нова версія mgOS — прийде сповіщення. Інсталятор є у списку програм: mgOS Installer");
    tip("󰍃", "Дві сесії", "В екрані входу: «mgOS» — повна система, «kiosk» — енергоощадна");
    // підказки прокручуються — вікно не вище за екран ноутбука
    GtkWidget* tsc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tsc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(tsc), 360);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(tsc), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tsc), tips);
    fz_attach(tsc);
    gtk_box_append(GTK_BOX(p), tsc);

    GtkWidget* btns = hbox(8);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(btns, 8);
    gtk_box_append(GTK_BOX(btns), button("󰓙  Перевірити систему", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) { open_check(); })));
    gtk_box_append(GTK_BOX(btns), button("󰐊  Почати роботу", "primary", G_CALLBACK(+[](GtkButton*, gpointer) { gtk_window_destroy(GTK_WINDOW(g_win)); })));
    gtk_box_append(GTK_BOX(p), btns);
    return p;
}

// ---------- оновлення самого інсталятора з git (gui/selfupdate.sh) ----------
static GtkWidget *g_selfupd_box = nullptr, *g_selfupd_lbl = nullptr, *g_selfupd_btn = nullptr;
static void selfupd_check();
static void selfupd_apply(GtkButton*, gpointer) {
    if (g_running) return;
    gtk_widget_set_sensitive(g_selfupd_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(g_selfupd_lbl), "Завантажую оновлення з GitHub…");
    GError* e = nullptr;
    GSubprocess* p = g_subprocess_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE), &e,
                                      "bash", (g_dir + "/gui/selfupdate.sh").c_str(), "apply", nullptr);
    if (!p) { gtk_label_set_text(GTK_LABEL(g_selfupd_lbl), e ? e->message : "не вдалося запустити"); g_clear_error(&e); return; }
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, [](GObject* o, GAsyncResult* r, gpointer) {
        char* out = nullptr; GError* e = nullptr;
        GSubprocess* p = G_SUBPROCESS(o);
        g_subprocess_communicate_utf8_finish(p, r, &out, nullptr, &e);
        bool ok = !e && g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0;
        std::string txt = out ? out : (e ? e->message : "");
        g_free(out); g_clear_error(&e); g_object_unref(p);
        if (!ok) {
            auto pos = txt.find("@@UPDERR ");
            gtk_label_set_text(GTK_LABEL(g_selfupd_lbl), trim(pos != std::string::npos ? txt.substr(pos + 9) : txt).c_str());
            gtk_widget_set_sensitive(g_selfupd_btn, TRUE);
            return;
        }
        // перезапуск через install-gui.sh: він перезбере інсталятор, якщо змінився код
        std::string sh = g_dir + "/install-gui.sh";
        execl("/usr/bin/env", "env", "bash", sh.c_str(), (char*)nullptr);
        gtk_label_set_text(GTK_LABEL(g_selfupd_lbl), "Оновлено. Перезапусти інсталятор.");
    }, nullptr);
}
static void selfupd_check() {
    if (!g_file_test((g_dir + "/.git").c_str(), G_FILE_TEST_EXISTS)) return;   // не git-клон — мовчимо
    GSubprocess* p = g_subprocess_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr,
                                      "bash", (g_dir + "/gui/selfupdate.sh").c_str(), "check", nullptr);
    if (!p) return;
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, [](GObject* o, GAsyncResult* r, gpointer) {
        char* out = nullptr;
        g_subprocess_communicate_utf8_finish(G_SUBPROCESS(o), r, &out, nullptr, nullptr);
        std::string t = out ? trim(out) : ""; g_free(out); g_object_unref(o);
        if (t.rfind("@@UPD ", 0) != 0) return;   // немає мережі/доступу — не заважаємо
        std::istringstream is(t.substr(6)); int n = 0; is >> n;
        if (n <= 0) return;
        std::string subj; std::getline(is, subj); subj = trim(subj);
        gtk_label_set_text(GTK_LABEL(g_selfupd_lbl), ("Нова версія на GitHub (" + std::to_string(n) + " змін): " + subj).c_str());
        gtk_widget_set_visible(g_selfupd_box, TRUE);
    }, nullptr);
}

static GtkWidget* build_home() {
    GtkWidget* p = vbox(14);
    gtk_widget_add_css_class(p, "page");

    g_resume_box = vbox(8);
    gtk_widget_add_css_class(g_resume_box, "resume");
    g_resume_title = label("", "opt-title");
    gtk_box_append(GTK_BOX(g_resume_box), g_resume_title);
    g_resume_lbl = label("", "opt-desc"); gtk_label_set_wrap(GTK_LABEL(g_resume_lbl), TRUE);
    gtk_box_append(GTK_BOX(g_resume_box), g_resume_lbl);
    GtkWidget* rbtns = hbox(8);
    gtk_box_append(GTK_BOX(rbtns), button("Забути", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) {
        RunState s; if (load_state(s) && !s.cache.empty()) { gchar* q = g_shell_quote(s.cache.c_str());
            g_spawn_command_line_async(("rm -rf " + std::string(q)).c_str(), nullptr); g_free(q); }
        unlink(state_file().c_str()); refresh_resume(); })));
    gtk_box_append(GTK_BOX(rbtns), spacer());
    gtk_box_append(GTK_BOX(rbtns), button("󰕍  Повернути як було", "danger", G_CALLBACK(+[](GtkButton*, gpointer) { open_rollback(); })));
    g_resume_go = button("󰐊  Продовжити", "primary", G_CALLBACK(+[](GtkButton*, gpointer) {
        if (load_state(R)) open_options(R.mode, true);
    }));
    gtk_box_append(GTK_BOX(rbtns), g_resume_go);
    gtk_box_append(GTK_BOX(g_resume_box), rbtns);
    gtk_box_append(GTK_BOX(p), g_resume_box);

    // банер «є нова версія інсталятора» — з'являється після фонової перевірки GitHub
    g_selfupd_box = hbox(10);
    gtk_widget_add_css_class(g_selfupd_box, "resume");
    gtk_box_append(GTK_BOX(g_selfupd_box), label("󰚰", "opt-title"));
    g_selfupd_lbl = label("", "opt-desc"); gtk_label_set_wrap(GTK_LABEL(g_selfupd_lbl), TRUE);
    gtk_widget_set_hexpand(g_selfupd_lbl, TRUE); gtk_widget_set_valign(g_selfupd_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(g_selfupd_box), g_selfupd_lbl);
    g_selfupd_btn = button("󰚰  Оновити інсталятор", "primary", G_CALLBACK(selfupd_apply));
    gtk_box_append(GTK_BOX(g_selfupd_box), g_selfupd_btn);
    gtk_widget_set_visible(g_selfupd_box, FALSE);
    gtk_box_append(GTK_BOX(p), g_selfupd_box);

    gtk_box_append(GTK_BOX(p), label("Що робимо?", "h1"));
    std::string snap = trim(read_text(g_dir + "/system_state/snapshot_date.txt"));
    int npk = 0; { std::istringstream is(read_text(g_dir + "/pkglist.txt")); std::string l; while (std::getline(is, l)) if (!trim(l).empty()) npk++; }
    GtkWidget* cards = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(cards), 12); gtk_grid_set_column_spacing(GTK_GRID(cards), 12);
    gtk_grid_set_column_homogeneous(GTK_GRID(cards), TRUE); gtk_grid_set_row_homogeneous(GTK_GRID(cards), TRUE);
    // 3 колонки: п'ята картка (kioskOS) не робить вікно вищим за екран ноутбука
    gtk_grid_attach(GTK_GRID(cards), mode_card("󰏔", "Встановити", "Повне встановлення Megumin OS 6 на чистий Arch Linux — система 1 в 1 як у знімку", "≈ 30–90 хв", Mode::Install), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cards), mode_card("󰁯", "Відновити", "Повернути конфіги, панелі, тему й служби до знімка mgOS", snap.empty() ? "зі знімка" : "знімок від " + snap, Mode::Restore), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cards), mode_card("󰚰", "Оновити", "Знайти й оновити застарілі пакети mgOS, доставити відсутні", std::to_string(npk) + " пакетів у знімку", Mode::Update), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cards), mode_card("󰓙", "Перевірити", "Діагностика: що зламано, чого бракує, що відрізняється від знімка", "≈ 2 с · нічого не змінює", Mode::Check), 0, 1, 1, 1);
    std::string ksnap = trim(read_text(g_dir + "/kiosk/files/snapshot_date.txt"));
    gtk_grid_attach(GTK_GRID(cards), mode_card("󰂄", "Встановити kioskOS",
        "Енергоощадна kiosk-сесія поруч із mgOS: Hyprland без анімацій і шпалер, оптимізований Firefox, тиша й 2× автономність. Економія — лише в kiosk",
        ksnap.empty() ? "знімка немає — kiosk/sync_kiosk.sh" : "≈ 10–25 хв · знімок від " + ksnap, Mode::Kiosk, 30), 1, 1, 2, 1);
    gtk_widget_set_valign(cards, GTK_ALIGN_START);
    gtk_widget_set_vexpand(cards, FALSE);
    gtk_box_append(GTK_BOX(p), cards);

    // що всередині знімка
    GtkWidget* info = hbox(8);
    gtk_box_set_homogeneous(GTK_BOX(info), TRUE);
    auto count_dir = [](const std::string& d) { int n = 0; if (GDir* g = g_dir_open(d.c_str(), 0, nullptr)) { while (g_dir_read_name(g)) n++; g_dir_close(g); } return n; };
    auto kv = [&](const char* k, const std::string& v) {
        GtkWidget* b = vbox(2); gtk_widget_add_css_class(b, "opt-row");
        gtk_box_append(GTK_BOX(b), label(k, "small")); gtk_box_append(GTK_BOX(b), label(v, "opt-title"));
        gtk_box_append(GTK_BOX(info), b);
    };
    kv("КОНФІГИ", std::to_string(count_dir(g_dir + "/configs")));
    kv("ШПАЛЕРИ", std::to_string(count_dir(g_dir + "/wallpapers")));
    kv("ПАКЕТИ", std::to_string(npk));
    kv("ПАНЕЛІ SYSui", "11 модулів");
    gtk_box_append(GTK_BOX(p), info);
    gtk_box_append(GTK_BOX(p), [] { GtkWidget* s = vbox(0); gtk_widget_set_vexpand(s, TRUE); return s; }());

    GtkWidget* foot = hbox(8);
    gtk_box_append(GTK_BOX(foot), label("󰋼  Прогрес зберігається після кожного кроку — можна зупинитись і продовжити навіть після перезавантаження.", "small"));
    gtk_box_append(GTK_BOX(p), foot);
    return p;
}

static GtkWidget* build_options() {
    GtkWidget* p = vbox(12);
    gtk_widget_add_css_class(p, "page");
    g_opt_title = label("", "h1");
    gtk_box_append(GTK_BOX(p), g_opt_title);
    g_opt_desc = label("", "dimtxt"); gtk_label_set_wrap(GTK_LABEL(g_opt_desc), TRUE);
    gtk_box_append(GTK_BOX(p), g_opt_desc);
    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sc, TRUE);
    g_opt_body = vbox(10);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), g_opt_body);
    fz_attach(sc);
    gtk_box_append(GTK_BOX(p), sc);

    g_pw_box = vbox(6);
    gtk_box_append(GTK_BOX(g_pw_box), label("󰌾  Пароль sudo", "h2"));
    g_pw_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(g_pw_entry), TRUE);
    g_signal_connect(g_pw_entry, "activate", G_CALLBACK(on_start), nullptr);
    gtk_box_append(GTK_BOX(g_pw_box), g_pw_entry);
    g_pw_err = label("", "err");
    gtk_box_append(GTK_BOX(g_pw_box), g_pw_err);
    gtk_box_append(GTK_BOX(g_pw_box), label("Пароль лишається в пам'яті інсталятора і ніде не зберігається.", "small"));
    gtk_box_append(GTK_BOX(p), g_pw_box);

    GtkWidget* btns = hbox(8);
    gtk_box_append(GTK_BOX(btns), button("󰅖  Скасувати", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) { show_home(); })));
    gtk_box_append(GTK_BOX(btns), spacer());
    g_start_btn = button("󰐊  Далі", "primary", G_CALLBACK(on_start));
    gtk_box_append(GTK_BOX(btns), g_start_btn);
    gtk_box_append(GTK_BOX(p), btns);
    return p;
}

static GtkWidget* build_run() {
    GtkWidget* p = vbox(12);
    gtk_widget_add_css_class(p, "page");

    g_close_banner = hbox(10);
    gtk_widget_add_css_class(g_close_banner, "banner");
    GtkWidget* bl = label("Крок ще виконується. Якщо закрити зараз — він перерветься, але все виконане раніше збережено: наступного разу інсталятор продовжить з цього кроку або поверне все як було.", "opt-desc");
    gtk_label_set_wrap(GTK_LABEL(bl), TRUE); gtk_widget_set_hexpand(bl, TRUE);
    gtk_box_append(GTK_BOX(g_close_banner), bl);
    gtk_box_append(GTK_BOX(g_close_banner), button("Закрити", "danger", G_CALLBACK(+[](GtkButton*, gpointer) {
        if (g_proc) g_subprocess_send_signal(g_proc, SIGTERM);
        save_state();
        exit(0);
    })));
    gtk_widget_set_visible(g_close_banner, FALSE);
    gtk_box_append(GTK_BOX(p), g_close_banner);

    GtkWidget* top = hbox(14);
    GtkWidget* tl = vbox(2); gtk_widget_set_hexpand(tl, TRUE);
    g_step_lbl = label("", "steptitle");
    gtk_box_append(GTK_BOX(tl), g_step_lbl);
    g_msg_lbl = label("", "msg");
    gtk_label_set_ellipsize(GTK_LABEL(g_msg_lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(tl), g_msg_lbl);
    g_time_lbl = label("", "small");
    gtk_box_append(GTK_BOX(tl), g_time_lbl);
    gtk_box_append(GTK_BOX(top), tl);
    g_pct = gtk_drawing_area_new();
    // «100%» жирним: 4 знаки × 7 колонок − 1 = 27 пікселів по 6 px
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_pct), 172);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_pct), 50);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_pct), [](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
        draw_pixel_text(cr, std::to_string((int)std::lround(g_prog_shown * 100)) + "%", w, h, 6, 0, true);
    }, nullptr, nullptr);
    gtk_widget_set_valign(g_pct, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(top), g_pct);
    gtk_box_append(GTK_BOX(p), top);

    g_pbar = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_pbar), 30);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_pbar), draw_pbar, nullptr, nullptr);
    gtk_box_append(GTK_BOX(p), g_pbar);

    g_steps_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_steps_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(g_steps_scroll, TRUE);
    gtk_widget_add_css_class(g_steps_scroll, "steps-box");
    g_steps_box = vbox(1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(g_steps_scroll), g_steps_box);
    fz_attach(g_steps_scroll);
    gtk_box_append(GTK_BOX(p), g_steps_scroll);

    g_log_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(g_log_rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    GtkWidget* lsc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(lsc), 180);
    g_log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(g_log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_log_view), GTK_WRAP_CHAR);
    gtk_widget_add_css_class(g_log_view, "log");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lsc), g_log_view);
    gtk_revealer_set_child(GTK_REVEALER(g_log_rev), lsc);
    gtk_box_append(GTK_BOX(p), g_log_rev);

    GtkWidget* btns = hbox(8);
    gtk_box_append(GTK_BOX(btns), button("󰦪  Лог", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) {
        bool show = !gtk_revealer_get_reveal_child(GTK_REVEALER(g_log_rev));
        gtk_revealer_set_reveal_child(GTK_REVEALER(g_log_rev), show);
        if (show) g_timeout_add(50, [](gpointer) -> gboolean {
            GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_log_view));
            GtkTextIter e; gtk_text_buffer_get_end_iter(b, &e);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(g_log_view), &e, 0, FALSE, 0, 1);
            return G_SOURCE_REMOVE;
        }, nullptr);
    })));
    g_btn_home = button("󰋜  На головну", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) { show_home(); }));
    gtk_box_append(GTK_BOX(btns), g_btn_home);
    gtk_box_append(GTK_BOX(btns), spacer());
    g_btn_skip = button("󰒭  Пропустити крок", nullptr, G_CALLBACK(on_skip));
    gtk_box_append(GTK_BOX(btns), g_btn_skip);
    g_btn_retry = button("󰑓  Повторити", "primary", G_CALLBACK(on_retry));
    gtk_box_append(GTK_BOX(btns), g_btn_retry);
    g_btn_cancel = button("󰕍  Скасувати", "danger", G_CALLBACK(on_cancel));
    gtk_box_append(GTK_BOX(btns), g_btn_cancel);
    g_btn_pause = button("󰏤  Пауза", "primary", G_CALLBACK(on_pause));
    gtk_box_append(GTK_BOX(btns), g_btn_pause);
    gtk_box_append(GTK_BOX(p), btns);
    return p;
}

static GtkWidget* build_done() {
    GtkWidget* p = vbox(12);
    gtk_widget_add_css_class(p, "page");
    gtk_widget_set_valign(p, GTK_ALIGN_CENTER);
    g_done_icon = label("󰄬", "big-icon", 0.5);
    gtk_box_append(GTK_BOX(p), g_done_icon);
    g_done_title = label("", "h1", 0.5);
    gtk_box_append(GTK_BOX(p), g_done_title);
    g_done_sub = label("", "dimtxt", 0.5); gtk_label_set_wrap(GTK_LABEL(g_done_sub), TRUE); gtk_label_set_justify(GTK_LABEL(g_done_sub), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(p), g_done_sub);
    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sc), 260);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sc), TRUE);
    g_done_items_title = label("Для кожної проблеми обери: спробувати ще раз чи пропустити", "h2", 0.5);
    gtk_box_append(GTK_BOX(p), g_done_items_title);
    g_done_list = vbox(0);
    gtk_widget_add_css_class(g_done_list, "steps-box");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), g_done_list);
    fz_attach(sc);
    gtk_box_append(GTK_BOX(p), sc);
    GtkWidget* btns = hbox(8);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(btns), button("󰋜  На головну", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) { show_home(); })));
    gtk_box_append(GTK_BOX(btns), button("Закрити", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) { gtk_window_destroy(GTK_WINDOW(g_win)); })));
    g_done_cancel = button("󰕍  Скасувати все", "danger", G_CALLBACK(+[](GtkButton*, gpointer) { start_rollback(); }));
    gtk_box_append(GTK_BOX(btns), g_done_cancel);
    g_done_apply = button("󰄬  Застосувати вибір", "primary", G_CALLBACK(on_apply_choices));
    gtk_box_append(GTK_BOX(btns), g_done_apply);
    g_done_reboot = button("󰜉  Перезавантажити", "primary", G_CALLBACK(+[](GtkButton*, gpointer) {
        g_spawn_command_line_async("systemctl reboot", nullptr);
    }));
    gtk_box_append(GTK_BOX(btns), g_done_reboot);
    gtk_box_append(GTK_BOX(p), btns);
    return p;
}

static gboolean on_close(GtkWindow*, gpointer) {
    if (g_running) { gtk_widget_set_visible(g_close_banner, TRUE); return TRUE; }
    return FALSE;
}

static void activate(GtkApplication* app, gpointer) {
    if (g_win) { gtk_window_present(GTK_WINDOW(g_win)); return; }
    std::string a = wal_accent(std::string(g_get_user_cache_dir()) + "/wal/colors.css");
    if (a.empty()) a = wal_accent(g_dir + "/cache_wal/colors.css");
    if (!a.empty()) g_accent = a;
    g_wv.set_accent(g_accent);
    {
        std::string pc = std::string(g_get_user_cache_dir()) + "/wal/colors.css";
        if (!g_file_test(pc.c_str(), G_FILE_TEST_EXISTS)) pc = g_dir + "/cache_wal/colors.css";
        g_wv.set_palette_css(pc);
    }
    g_wv.fade_from = 0.45;
    g_cancel = g_cancellable_new();

    g_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_anim_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_anim_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    apply_css();

    g_win = gtk_application_window_new(app);
    gtk_widget_add_css_class(g_win, "mgos");
    gtk_window_set_title(GTK_WINDOW(g_win), "Megumin OS 6 — Інсталятор");
    gtk_window_set_default_size(GTK_WINDOW(g_win), 760, 780);
    gtk_window_set_decorated(GTK_WINDOW(g_win), FALSE);
    g_signal_connect(g_win, "close-request", G_CALLBACK(on_close), nullptr);

    GtkWidget* root = vbox(0);
    // шапка
    GtkWidget* hdr = gtk_overlay_new();
    g_waves = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_waves), 190);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_waves), draw_waves, nullptr, nullptr);
    gtk_overlay_set_child(GTK_OVERLAY(hdr), g_waves);
    GtkWidget* tb = hbox(8);
    gtk_widget_set_valign(tb, GTK_ALIGN_START);
    gtk_widget_set_margin_top(tb, 14); gtk_widget_set_margin_start(tb, 16); gtk_widget_set_margin_end(tb, 16);
    g_chip = label("󰣇  mgOS installer", "chip"); gtk_widget_add_css_class(g_chip, "mono");
    gtk_box_append(GTK_BOX(tb), g_chip);
    gtk_box_append(GTK_BOX(tb), spacer());
    GtkWidget* x = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(x, "x");
    gtk_widget_set_cursor_from_name(x, "pointer");
    g_signal_connect_swapped(x, "clicked", G_CALLBACK(gtk_window_close), g_win);
    gtk_box_append(GTK_BOX(tb), x);
    gtk_overlay_add_overlay(GTK_OVERLAY(hdr), tb);
    GtkWidget* tbox = vbox(2);
    gtk_widget_set_valign(tbox, GTK_ALIGN_END);
    gtk_widget_set_halign(tbox, GTK_ALIGN_START);
    gtk_widget_set_margin_start(tbox, 22); gtk_widget_set_margin_bottom(tbox, 10);
    GtkWidget* tr = hbox(0);
    gtk_box_append(GTK_BOX(tr), label("Megumin OS", "title"));
    GtkWidget* badge = label("6", "badge", 0.5); gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(tr), badge);
    gtk_box_append(GTK_BOX(tbox), tr);
    g_subtitle = label("", "subtitle");
    gtk_box_append(GTK_BOX(tbox), g_subtitle);
    gtk_overlay_add_overlay(GTK_OVERLAY(hdr), tbox);
    // перетягування вікна за шапку (без декорацій)
    GtkGesture* drag = gtk_gesture_click_new();
    g_signal_connect(drag, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int, double x, double y, gpointer) {
        GdkSurface* s = gtk_native_get_surface(GTK_NATIVE(g_win));
        GdkDevice* dev = gtk_gesture_get_device(GTK_GESTURE(g));
        if (GDK_IS_TOPLEVEL(s) && dev)
            gdk_toplevel_begin_move(GDK_TOPLEVEL(s), dev, GDK_BUTTON_PRIMARY, x, y, gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(g)));
    }), nullptr);
    gtk_widget_add_controller(g_waves, GTK_EVENT_CONTROLLER(drag));
    gtk_box_append(GTK_BOX(root), hdr);

    g_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(g_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(g_stack), 320);
    gtk_widget_set_vexpand(g_stack, TRUE);
    gtk_stack_add_named(GTK_STACK(g_stack), build_home(), "home");
    gtk_stack_add_named(GTK_STACK(g_stack), build_options(), "options");
    gtk_stack_add_named(GTK_STACK(g_stack), build_run(), "run");
    gtk_stack_add_named(GTK_STACK(g_stack), build_done(), "done");
    gtk_stack_add_named(GTK_STACK(g_stack), build_check(), "check");
    if (g_getenv("MGOS_WELCOME")) gtk_stack_add_named(GTK_STACK(g_stack), build_welcome(), "welcome");
    gtk_box_append(GTK_BOX(root), g_stack);
    gtk_window_set_child(GTK_WINDOW(g_win), root);

    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint kv, guint, GdkModifierType, gpointer) -> gboolean {
        // у bootstrap-сесії Esc не закриває інсталятор (інакше вихід у TTY одним натиском)
        if (kv == GDK_KEY_Escape && !g_getenv("MGOS_FULLSCREEN")) { gtk_window_close(GTK_WINDOW(g_win)); return TRUE; }
        return FALSE;
    }), nullptr);
    gtk_widget_add_controller(g_win, key);

    gtk_widget_add_tick_callback(g_waves, on_tick, nullptr, nullptr);
    g_timeout_add(80, update_anim_css, nullptr);
    g_timeout_add(100, on_ui_timer, nullptr);
    show_home();
    gtk_window_set_default_icon_name("com.megumin.installer");
    // bootstrap (mgos-bootstrap.sh): інсталятор на весь екран у мінімальній сесії Hyprland
    if (g_getenv("MGOS_FULLSCREEN")) gtk_window_fullscreen(GTK_WINDOW(g_win));
    if (g_getenv("MGOS_WELCOME")) {
        gtk_label_set_text(GTK_LABEL(g_subtitle), "Ласкаво просимо");
        gtk_stack_set_visible_child_name(GTK_STACK(g_stack), "welcome");
    }
    gtk_window_present(GTK_WINDOW(g_win));
    if (!g_getenv("MGOS_DEMO_PAGE") && !g_getenv("MGOS_NO_SELFUPDATE") && !g_getenv("MGOS_WELCOME")) selfupd_check();

    const char* demo = g_getenv("MGOS_DEMO_PAGE");  // для скриншотів
    if (demo) {
        std::string d = demo;
        if (d == "install" || d == "restore" || d == "update" || d == "kiosk") open_options(mode_from(d), false);
        if (d == "check") open_check();
        if (const char* c = g_getenv("MGOS_DEMO_CANCEL"))   // тест: «Скасувати» через N секунд
            g_timeout_add_seconds(atoi(c), [](gpointer) -> gboolean { on_cancel(nullptr, nullptr); return G_SOURCE_REMOVE; }, nullptr);
        if (d.rfind("run:", 0) == 0) { open_options(mode_from(d.substr(4)), false); start_with_options(); }  // тест на фейкових кроках
    }
}

static void cleanup() {
    if (!g_rt.empty()) {
        unlink((g_rt + "/askpass").c_str()); unlink((g_rt + "/sudo").c_str()); rmdir(g_rt.c_str());
    }
    std::fill(g_password.begin(), g_password.end(), '\0');
}

int main(int argc, char** argv) {
    // корінь інсталятора: MGOS_DIR або тека над бінарником (gui/..)
    if (const char* e = g_getenv("MGOS_DIR")) g_dir = e;
    else {
        char buf[4096]; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        std::string exe = n > 0 ? std::string(buf, n) : argv[0];
        gchar* d = g_path_get_dirname(exe.c_str()); gchar* up = g_path_get_dirname(d);
        g_dir = up; g_free(d); g_free(up);
    }
    GtkApplication* app = gtk_application_new("com.megumin.installer", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    cleanup();
    g_object_unref(app);
    return r;
}
