// about_osd.cpp — «Про систему» Megumin OS 6 (заміна ignis/about.py).
// Super+I → echo toggle | socat - UNIX-CLIENT:~/.config/SYSui/about.sock
//
// Шапка: піксельні хвилі (вид згори) з градієнтом і дизерингом; відтінок
// повільно пливе по колу, стартуючи з системного акценту.
// Дані: /proc, statvfs, nvidia-smi, pacman local db, checkupdates + yay -Qua.
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include "sysui_common.h"
#include "pixel_waves.h"
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <dirent.h>
#include <cmath>
#include <thread>
#include <vector>
#include <map>

// ============================================================================
//  Утиліти
// ============================================================================
static std::string read_file(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}
// Потокобезпечний запуск команди (g_spawn, без popen).
static std::string run(const std::string& cmd, int* status = nullptr) {
    std::string full = "LC_ALL=C " + cmd;
    const char* argv[] = {"sh", "-c", full.c_str(), nullptr};
    gchar* out = nullptr; gint st = 0;
    if (!g_spawn_sync(nullptr, (gchar**)argv, nullptr, (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL),
                      nullptr, nullptr, &out, nullptr, &st, nullptr)) { if (status) *status = -1; return ""; }
    if (status) *status = st;
    std::string r = out ? out : ""; g_free(out);
    return trim(r);
}
static std::string fmt1(double v) {  // без локалі (uk_UA пише кому)
    int m = (int)std::lround(v * 10);
    return std::to_string(m / 10) + "." + std::to_string(std::abs(m % 10));
}
static std::string gib(double bytes) { return fmt1(bytes / 1073741824.0); }
static std::string uptime_str() {
    long s = (long)atof(read_file("/proc/uptime").c_str());
    long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if (d) return std::to_string(d) + " дн " + std::to_string(h) + " год";
    if (h) return std::to_string(h) + " год " + std::to_string(m) + " хв";
    return std::to_string(m) + " хв";
}
template <class F> static void on_main(F f) {  // виконати лямбду в головному потоці
    g_idle_add([](gpointer d) -> gboolean { auto* fp = (F*)d; (*fp)(); delete fp; return G_SOURCE_REMOVE; }, new F(std::move(f)));
}

// ---------- колір (спільне з pixel_waves.h) ----------
using pw::RGB; using pw::rgb_hsl; using pw::hsl_rgb;
static RGB hex_rgb(const std::string& h) { RGB c{0.6, 0.16, 0.81}; pw::parse_hex(h, c); return c; }

// ============================================================================
//  Стан
// ============================================================================
static GtkWidget* g_win = nullptr;
static GtkWidget* g_card = nullptr;
static GtkWidget* g_waves = nullptr;
static GtkCssProvider* g_css = nullptr;
static guint g_tick = 0, g_stats_timer = 0, g_spin_timer = 0;
static pw::Waves g_wv;
static GtkCssProvider* g_anim_css = nullptr;
static guint g_anim_timer = 0;
static gint64 g_t0 = 0;
static double g_anim_t = 0;

struct Tile { GtkWidget *value, *sub, *bar; };
static Tile t_cpu, t_gpu, t_ram, t_disk;
static GtkWidget *l_subtitle, *l_uptime, *l_host;
static GtkWidget *l_kernel, *l_hypr, *l_shell, *l_pkgs;
static GtkWidget *l_upd_badge, *l_upd_status, *g_upd_list, *g_upd_scroll;
static GtkWidget *b_update, *b_check, *l_update_btn;

static bool g_checking = false, g_updating = false;
static gint64 g_last_check = 0;
static int g_spin_i = 0;
static unsigned long long g_cpu_prev_idle = 0, g_cpu_prev_total = 0;
static std::string g_cpu_name, g_temp_path;
static int g_threads = 0;

static void check_updates(bool force);
static GtkWidget* label(const char* txt, const char* cls, float xalign = 0);
static void hide_about() { if (g_win) gtk_widget_set_visible(g_win, FALSE); }

// ============================================================================
//  Піксельні хвилі
// ============================================================================
static void draw_waves(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    g_wv.t = g_anim_t;
    g_wv.draw(cr, w, h);
}

static gboolean on_tick(GtkWidget* w, GdkFrameClock* fc, gpointer) {
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    if (!g_t0) g_t0 = now;
    g_anim_t = (now - g_t0) / 1e6;
    g_wv.reveal = std::min(1.0, g_anim_t / 1.3);   // плавна поява за 1.3 с
    gtk_widget_queue_draw(w);                       // кожен кадр монітора — без ривків
    return G_SOURCE_CONTINUE;
}

// «Живий» акцент: бейдж, іконки, смужки, кнопки пливуть разом із хвилями.
// Окремий маленький CSS-провайдер, оновлюється ~12 разів/с, а CSS-transition
// між оновленнями робить зміну кольору безперервною.
static void update_anim_css() {
    if (!g_anim_css) return;
    RGB c = g_wv.accent_now();
    std::string H = pw::to_hex(c), S = pw::to_rgba(c, 0.18), G = pw::to_rgba(c, 0.5);
    RGB hv = hsl_rgb(0, 0, 0); double h, s, l; rgb_hsl(c, h, s, l); hv = hsl_rgb(h, s, std::min(0.85, l + 0.08));
    std::string css =
        ".badge{background-color:" + H + ";box-shadow:0 0 18px " + G + ";}"
        ".tile-icon{color:" + H + ";background-color:" + S + ";}"
        "progressbar:not(.hot) progress{background-color:" + H + ";}"
        ".count.has{background-color:" + H + ";}"
        "button.btn.primary{background-color:" + H + ";}"
        "button.btn.primary:hover{background-color:" + pw::to_hex(hv) + ";}"
        ".aur{color:" + H + ";border-color:" + G + ";}";
    gtk_css_provider_load_from_string(g_anim_css, css.c_str());
}

// ============================================================================
//  CSS
// ============================================================================
static void apply_css() {
    std::string A = sysui::accent();
    double h, s, l; rgb_hsl(hex_rgb(A), h, s, l);
    g_wv.set_accent(A);
    if (sysui::settings().accent_mode != "custom") g_wv.set_palette_css(sysui::wal_css_path());
    int op = (int)std::lround(sysui::settings().cc_opacity * 10.0 + 180);  // ‰, трохи щільніше за CC
    op = std::clamp(op, 300, 1000);
    std::string panel = "rgba(20,20,24," + std::to_string(op / 1000) + "." + std::to_string(1000 + op % 1000).substr(1) + ")";

    std::string css = R"CSS(
window { background: transparent; }
.dim { background-color: rgba(0,0,0,0.38); }
* { font-family: "SF Pro Display", "Inter", "JetBrainsMono Nerd Font", sans-serif; outline: none; }
.about-card {
    background-color: @PANEL@;
    border: 1px solid rgba(255,255,255,0.10);
    border-radius: 24px;
    color: #f5f5f7;
    box-shadow: 0 24px 60px rgba(0,0,0,0.55);
}
.about-card label { color: #f5f5f7; }
.mono, .mono label { font-family: "JetBrainsMono Nerd Font", monospace; }
.title { font-size: 34px; font-weight: 900; letter-spacing: 1px; text-shadow: 0 2px 10px rgba(0,0,0,0.6); }
.badge { font-family: "JetBrainsMono Nerd Font", monospace; font-size: 22px; font-weight: 900;
         background-color: @ACCENT@; color: #ffffff; border-radius: 10px; padding: 0 12px; margin-left: 10px;
         box-shadow: 0 0 18px @ACCENT_A50@; }
.subtitle { font-size: 13px; opacity: 0.8; text-shadow: 0 1px 6px rgba(0,0,0,0.7); }
.chip { background-color: rgba(10,10,14,0.55); border: 1px solid rgba(255,255,255,0.12);
        border-radius: 999px; padding: 4px 12px; font-size: 12px; font-weight: 600; }
.body { padding: 4px 18px 18px 18px; }
.tile { background-color: rgba(255,255,255,0.055); border: 1px solid rgba(255,255,255,0.06);
        border-radius: 16px; padding: 12px 14px; }
.tile-icon { font-family: "JetBrainsMono Nerd Font"; font-size: 18px; color: @ACCENT@;
             background-color: @ACCENT_SOFT@; border-radius: 10px; min-width: 34px; min-height: 34px; }
.tile-title { font-size: 11px; font-weight: 700; opacity: 0.55; letter-spacing: 1px; }
.tile-value { font-size: 14px; font-weight: 700; }
.tile-sub { font-size: 11.5px; opacity: 0.6; }
progressbar trough { min-height: 6px; border-radius: 999px; background-color: rgba(255,255,255,0.10); border: none; }
progressbar progress { min-height: 6px; border-radius: 999px; background-color: @ACCENT@; border: none; }
progressbar.hot progress { background-color: #ff6b81; }
.kv { background-color: rgba(255,255,255,0.04); border-radius: 12px; padding: 8px 12px; }
.kv-k { font-size: 10.5px; font-weight: 700; opacity: 0.5; letter-spacing: 1px; }
.kv-v { font-size: 12.5px; font-weight: 600; }
.section { font-size: 13px; font-weight: 800; }
.count { font-family: "JetBrainsMono Nerd Font"; font-size: 11px; font-weight: 800; border-radius: 999px;
         padding: 1px 9px; background-color: rgba(255,255,255,0.12); }
.count.has { background-color: @ACCENT@; color: #ffffff; }
.count.ok { background-color: rgba(80,220,140,0.25); color: #7ff0ae; }
.upd-box { background-color: rgba(0,0,0,0.22); border-radius: 14px; padding: 6px; }
.upd-row { padding: 4px 8px; border-radius: 8px; }
.upd-row:hover { background-color: rgba(255,255,255,0.05); }
.upd-name { font-size: 12.5px; font-weight: 600; }
.upd-ver { font-family: "JetBrainsMono Nerd Font"; font-size: 11px; opacity: 0.55; }
.aur { font-size: 9.5px; font-weight: 800; color: @ACCENT@; border: 1px solid @ACCENT_A50@; border-radius: 6px; padding: 0 5px; }
.status { font-size: 12px; opacity: 0.65; padding: 8px; }
button.btn { border-radius: 12px; padding: 7px 16px; font-weight: 700; font-size: 13px; border: none;
             background-image: none; box-shadow: none;
             background-color: rgba(255,255,255,0.08); color: #f5f5f7; }
button.btn:hover { background-color: rgba(255,255,255,0.14); }
button.btn label { color: inherit; }
button.btn.primary { background-color: @ACCENT@; color: #ffffff; }
button.btn.primary:hover { background-color: @ACCENT_HOVER@; }
button.btn.success { background-color: #3fcf8e; color: #0d1f16; }
button.btn.success:hover { background-color: #5be0a2; }
button.btn:disabled { opacity: 0.55; }
.hint { font-size: 10.5px; opacity: 0.4; }
.badge, .tile-icon, progressbar progress, .count, .aur { transition: background-color 160ms linear, color 160ms linear, border-color 160ms linear, box-shadow 160ms linear; }
)CSS";
    auto rep = [&](const std::string& k, const std::string& v) {
        size_t p = 0; while ((p = css.find(k, p)) != std::string::npos) { css.replace(p, k.size(), v); p += v.size(); }
    };
    RGB hv = hsl_rgb(h, s, std::min(0.85, l + 0.08));
    char hov[8]; snprintf(hov, sizeof hov, "#%02x%02x%02x", (int)(hv.r * 255), (int)(hv.g * 255), (int)(hv.b * 255));
    rep("@PANEL@", panel); rep("@ACCENT_SOFT@", sysui::rgba(A, 0.18)); rep("@ACCENT_A50@", sysui::rgba(A, 0.5));
    rep("@ACCENT_HOVER@", hov); rep("@ACCENT@", A);
    gtk_css_provider_load_from_string(g_css, css.c_str());
}

// ============================================================================
//  Дані
// ============================================================================
static void set_bar(GtkWidget* bar, double f) {
    f = std::clamp(f, 0.0, 1.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), f);
    if (f > 0.85) gtk_widget_add_css_class(bar, "hot"); else gtk_widget_remove_css_class(bar, "hot");
}

static void init_static() {
    std::string ci = read_file("/proc/cpuinfo");
    std::istringstream is(ci); std::string line;
    while (std::getline(is, line)) {
        if (line.rfind("model name", 0) == 0 && g_cpu_name.empty()) g_cpu_name = trim(line.substr(line.find(':') + 1));
        if (line.rfind("processor", 0) == 0) g_threads++;
    }
    // датчик температури CPU
    for (int i = 0; i < 16; i++) {
        std::string base = "/sys/class/hwmon/hwmon" + std::to_string(i) + "/";
        std::string n = trim(read_file(base + "name"));
        if (n == "k10temp" || n == "coretemp" || n == "zenpower") { g_temp_path = base + "temp1_input"; break; }
    }
    struct utsname u; uname(&u);
    gtk_label_set_text(GTK_LABEL(l_kernel), u.release);
    gtk_label_set_text(GTK_LABEL(t_cpu.value), g_cpu_name.c_str());
    gtk_widget_set_tooltip_text(t_cpu.value, g_cpu_name.c_str());

    std::string user = g_get_user_name(), host = g_get_host_name();
    gtk_label_set_text(GTK_LABEL(l_host), (" " + user + "@" + host).c_str());
    const char* sh = getenv("SHELL");
    std::string shell = sh ? sh : "?"; shell = shell.substr(shell.rfind('/') + 1);
    std::string bv = run(shell + " --version 2>/dev/null | head -1");
    // "GNU bash, version 5.3.3(1)-release" → "bash 5.3.3"
    size_t vp = bv.find("version ");
    if (vp != std::string::npos) { std::string v = bv.substr(vp + 8); v = v.substr(0, v.find_first_of("( ")); shell += " " + v; }
    gtk_label_set_text(GTK_LABEL(l_shell), shell.c_str());

    std::string hv = "—";
    try {
        auto j = nlohmann::json::parse(sysui::hypr_request("j/version"));
        if (j.contains("version")) hv = j["version"].get<std::string>();
        else if (j.contains("tag")) hv = j["tag"].get<std::string>();
    } catch (...) {}
    gtk_label_set_text(GTK_LABEL(l_hypr), hv.c_str());
    gtk_label_set_text(GTK_LABEL(l_subtitle), ("на базі Arch Linux  ·  Hyprland " + hv + "  ·  ядро " + std::string(u.release)).c_str());
}

static void refresh_pkg_count() {
    int n = 0;
    if (DIR* d = opendir("/var/lib/pacman/local")) {
        while (dirent* e = readdir(d)) if (e->d_type == DT_DIR && e->d_name[0] != '.') n++;
        closedir(d);
    }
    gtk_label_set_text(GTK_LABEL(l_pkgs), std::to_string(n).c_str());
}

static void refresh_gpu() {
    std::thread([] {
        int st = 0;
        std::string o = run("nvidia-smi --query-gpu=name,driver_version,memory.used,memory.total,utilization.gpu,temperature.gpu "
                            "--format=csv,noheader,nounits", &st);
        on_main([o, st] {
            if (!g_win) return;
            std::vector<std::string> p; std::stringstream ss(o); std::string x;
            while (std::getline(ss, x, ',')) p.push_back(trim(x));
            if (st != 0 || p.size() < 6) {
                gtk_label_set_text(GTK_LABEL(t_gpu.value), "Інтегрована / невідомо");
                gtk_label_set_text(GTK_LABEL(t_gpu.sub), "nvidia-smi недоступний");
                set_bar(t_gpu.bar, 0);
                return;
            }
            double used = atof(p[2].c_str()), total = atof(p[3].c_str());
            gtk_label_set_text(GTK_LABEL(t_gpu.value), p[0].c_str());
            gtk_widget_set_tooltip_text(t_gpu.value, p[0].c_str());
            std::string sub = "драйвер " + p[1] + "  ·  " + fmt1(used / 1024) + "/" + fmt1(total / 1024) + " ГБ  ·  " + p[4] + "%  ·  " + p[5] + "°C";
            gtk_label_set_text(GTK_LABEL(t_gpu.sub), sub.c_str());
            set_bar(t_gpu.bar, total > 0 ? used / total : 0);
        });
    }).detach();
}

static gboolean refresh_stats(gpointer = nullptr) {
    // CPU: завантаження з /proc/stat (дельта), частота, температура
    {
        std::istringstream is(read_file("/proc/stat"));
        std::string cpu; unsigned long long v[10] = {0};
        is >> cpu; for (auto& x : v) is >> x;
        unsigned long long idle = v[3] + v[4], total = 0; for (auto x : v) total += x;
        double load = 0;
        if (g_cpu_prev_total && total > g_cpu_prev_total)
            load = 1.0 - (double)(idle - g_cpu_prev_idle) / (double)(total - g_cpu_prev_total);
        g_cpu_prev_idle = idle; g_cpu_prev_total = total;

        double mhz = 0; int n = 0;
        std::istringstream ci(read_file("/proc/cpuinfo")); std::string line;
        while (std::getline(ci, line)) if (line.rfind("cpu MHz", 0) == 0) { mhz += atof(line.substr(line.find(':') + 1).c_str()); n++; }
        std::string sub = std::to_string(g_threads) + " потоків";
        if (n) sub += "  ·  " + fmt1(mhz / n / 1000.0) + " ГГц";
        sub += "  ·  " + std::to_string((int)std::lround(load * 100)) + "%";
        if (!g_temp_path.empty()) sub += "  ·  " + std::to_string(atoi(read_file(g_temp_path).c_str()) / 1000) + "°C";
        gtk_label_set_text(GTK_LABEL(t_cpu.sub), sub.c_str());
        set_bar(t_cpu.bar, load);
    }
    // RAM
    {
        std::istringstream is(read_file("/proc/meminfo")); std::string k; long long val; std::string unit;
        long long tot = 0, avail = 0, swt = 0, swf = 0;
        while (is >> k >> val >> unit) {
            if (k == "MemTotal:") tot = val; else if (k == "MemAvailable:") avail = val;
            else if (k == "SwapTotal:") swt = val; else if (k == "SwapFree:") swf = val;
        }
        double used = (tot - avail) * 1024.0, total = tot * 1024.0;
        gtk_label_set_text(GTK_LABEL(t_ram.value), (gib(used) + " / " + gib(total) + " ГБ").c_str());
        std::string sub = "доступно " + gib(avail * 1024.0) + " ГБ";
        if (swt) sub += "  ·  swap " + gib((swt - swf) * 1024.0) + "/" + gib(swt * 1024.0);
        gtk_label_set_text(GTK_LABEL(t_ram.sub), sub.c_str());
        set_bar(t_ram.bar, total > 0 ? used / total : 0);
    }
    // Диск /
    {
        struct statvfs s;
        if (statvfs("/", &s) == 0) {
            double total = (double)s.f_blocks * s.f_frsize, freeb = (double)s.f_bavail * s.f_frsize;
            double used = total - (double)s.f_bfree * s.f_frsize;
            gtk_label_set_text(GTK_LABEL(t_disk.value), (gib(used) + " / " + gib(total) + " ГБ").c_str());
            gtk_label_set_text(GTK_LABEL(t_disk.sub), ("вільно " + gib(freeb) + " ГБ  ·  /").c_str());
            set_bar(t_disk.bar, total > 0 ? used / total : 0);
        }
    }
    gtk_label_set_text(GTK_LABEL(l_uptime), ("󰔚  " + uptime_str()).c_str());
    static int k = 0;
    if (k++ % 2 == 0) refresh_gpu();
    return G_SOURCE_CONTINUE;
}

// ---------- оновлення пакетів ----------
static const char* SPIN[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

static void update_buttons() {
    bool busy = g_checking || g_updating;
    gtk_widget_set_sensitive(b_check, !busy);
    gtk_widget_set_sensitive(b_update, !g_updating);
    if (busy && !g_spin_timer) {
        g_spin_timer = g_timeout_add(90, [](gpointer) -> gboolean {
            g_spin_i = (g_spin_i + 1) % 10;
            std::string s = SPIN[g_spin_i];
            if (g_updating) gtk_label_set_text(GTK_LABEL(l_update_btn), (s + "  Оновлюю…").c_str());
            if (g_checking) gtk_label_set_text(GTK_LABEL(l_upd_status), (s + "  Перевіряю репозиторії та AUR…").c_str());
            return G_SOURCE_CONTINUE;
        }, nullptr);
    } else if (!busy && g_spin_timer) { g_source_remove(g_spin_timer); g_spin_timer = 0; }
    if (!g_updating) gtk_label_set_text(GTK_LABEL(l_update_btn), "󰚰  Оновити");
}

static GtkWidget* upd_row(const std::string& line, bool aur) {
    // "name old -> new"
    std::istringstream is(line); std::string name, oldv, arrow, newv;
    is >> name >> oldv >> arrow >> newv;
    GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(r, "upd-row");
    GtkWidget* n = gtk_label_new(name.c_str());
    gtk_widget_add_css_class(n, "upd-name");
    gtk_label_set_xalign(GTK_LABEL(n), 0);
    gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(n, TRUE);
    gtk_box_append(GTK_BOX(r), n);
    if (aur) { GtkWidget* a = gtk_label_new("AUR"); gtk_widget_add_css_class(a, "aur"); gtk_widget_set_valign(a, GTK_ALIGN_CENTER); gtk_box_append(GTK_BOX(r), a); }
    GtkWidget* v = gtk_label_new((oldv + "  →  " + newv).c_str());
    gtk_widget_add_css_class(v, "upd-ver");
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 34);
    gtk_box_append(GTK_BOX(r), v);
    return r;
}

static void check_updates(bool force) {
    if (g_checking || g_updating) return;
    if (!force && g_last_check && g_get_monotonic_time() - g_last_check < 10LL * 60 * 1000000) return;
    g_checking = true;
    gtk_widget_set_visible(g_upd_scroll, FALSE);
    gtk_widget_set_visible(l_upd_status, TRUE);
    update_buttons();
    std::thread([] {
        int st1 = 0, st2 = 0;
        std::string repo = run("checkupdates 2>/dev/null", &st1);
        std::string aur = run("yay -Qua 2>/dev/null", &st2);
        on_main([repo, aur, st1] {
            g_checking = false;
            g_last_check = g_get_monotonic_time();
            while (GtkWidget* c = gtk_widget_get_first_child(g_upd_list)) gtk_box_remove(GTK_BOX(g_upd_list), c);
            int n = 0;
            auto add = [&](const std::string& txt, bool is_aur) {
                std::istringstream is(txt); std::string line;
                while (std::getline(is, line)) {
                    if (trim(line).empty() || line.find("->") == std::string::npos) continue;
                    if (n < 120) gtk_box_append(GTK_BOX(g_upd_list), upd_row(line, is_aur));
                    n++;
                }
            };
            add(repo, false); add(aur, true);
            if (n > 120) {
                GtkWidget* more = label(("… і ще " + std::to_string(n - 120)).c_str(), "status");
                gtk_box_append(GTK_BOX(g_upd_list), more);
            }
            gtk_widget_remove_css_class(l_upd_badge, "has"); gtk_widget_remove_css_class(l_upd_badge, "ok");
            // checkupdates: 0 — є оновлення, 2 — немає, інше — помилка (мережа/база)
            bool err = n == 0 && !(WIFEXITED(st1) && (WEXITSTATUS(st1) == 0 || WEXITSTATUS(st1) == 2));
            if (n > 0) {
                gtk_label_set_text(GTK_LABEL(l_upd_badge), std::to_string(n).c_str());
                gtk_widget_add_css_class(l_upd_badge, "has");
                gtk_widget_set_visible(l_upd_status, FALSE);
                gtk_widget_set_visible(g_upd_scroll, TRUE);
            } else if (err) {
                gtk_label_set_text(GTK_LABEL(l_upd_badge), "?");
                gtk_label_set_text(GTK_LABEL(l_upd_status), "󰀦  Не вдалося перевірити (мережа?)");
            } else {
                gtk_label_set_text(GTK_LABEL(l_upd_badge), "0");
                gtk_widget_add_css_class(l_upd_badge, "ok");
                gtk_label_set_text(GTK_LABEL(l_upd_status), "󰄬  Система актуальна");
            }
            refresh_pkg_count();
            update_buttons();
        });
    }).detach();
}

static void start_update() {
    if (g_updating) return;
    const char* argv[] = {"kitty", "--title", "Megumin OS — оновлення", "-e", "sh", "-c",
        "yay -Syu; echo; echo '✅ Оновлення завершено! Натисніть Enter для виходу…'; read _", nullptr};
    GPid pid; GError* err = nullptr;
    if (!g_spawn_async(nullptr, (gchar**)argv, nullptr, (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
                       nullptr, nullptr, &pid, &err)) {
        sysui::sys_notify("error", "Оновлення", err ? err->message : "не вдалося запустити kitty");
        if (err) g_error_free(err);
        return;
    }
    g_updating = true;
    update_buttons();
    hide_about();  // термінал не має ховатися під оверлеєм
    g_child_watch_add(pid, [](GPid p, gint, gpointer) {
        g_spawn_close_pid(p);
        g_updating = false;
        update_buttons();
        sysui::sys_notify("info", "Megumin OS", "Оновлення завершено");
        check_updates(true);
    }, nullptr);
}

// ============================================================================
//  UI
// ============================================================================
static GtkWidget* label(const char* txt, const char* cls, float xalign) {
    GtkWidget* l = gtk_label_new(txt);
    if (cls) gtk_widget_add_css_class(l, cls);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    return l;
}

static GtkWidget* make_tile(const char* icon, const char* title, Tile& t) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box, "tile");
    gtk_widget_add_css_class(box, "live-soft");
    gtk_widget_set_hexpand(box, TRUE);
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* ic = label(icon, "tile-icon", 0.5);
    gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(top), ic);
    GtkWidget* col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_box_append(GTK_BOX(col), label(title, "tile-title"));
    t.value = label("…", "tile-value");
    gtk_label_set_ellipsize(GTK_LABEL(t.value), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(t.value), 22);
    gtk_box_append(GTK_BOX(col), t.value);
    gtk_box_append(GTK_BOX(top), col);
    gtk_box_append(GTK_BOX(box), top);
    t.sub = label("", "tile-sub");
    gtk_label_set_ellipsize(GTK_LABEL(t.sub), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), t.sub);
    t.bar = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(box), t.bar);
    return box;
}

static GtkWidget* make_kv(const char* k, GtkWidget*& v) {
    GtkWidget* b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(b, "kv");
    gtk_widget_set_hexpand(b, TRUE);
    gtk_box_append(GTK_BOX(b), label(k, "kv-k"));
    v = label("…", "kv-v");
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(b), v);
    return b;
}

static GtkWidget* make_btn(const char* txt, const char* cls, GCallback cb, GtkWidget** lbl_out = nullptr) {
    GtkWidget* b = gtk_button_new();
    GtkWidget* l = gtk_label_new(txt);
    gtk_button_set_child(GTK_BUTTON(b), l);
    gtk_widget_add_css_class(b, "btn");
    gtk_widget_add_css_class(b, "live");
    if (cls) gtk_widget_add_css_class(b, cls);
    gtk_widget_set_cursor_from_name(b, "pointer");
    g_signal_connect(b, "clicked", cb, nullptr);
    if (lbl_out) *lbl_out = l;
    return b;
}

static void show_about() {
    apply_css();
    refresh_pkg_count();
    refresh_stats();
    gtk_widget_set_visible(g_win, TRUE);
    gtk_window_present(GTK_WINDOW(g_win));
    check_updates(false);
}

static void on_map(GtkWidget*, gpointer) {
    g_t0 = 0; g_anim_t = 0;
    if (sysui::settings().animations) {
        g_wv.reveal = 0;
        if (!g_tick) g_tick = gtk_widget_add_tick_callback(g_waves, on_tick, nullptr, nullptr);
        update_anim_css();
        if (!g_anim_timer) g_anim_timer = g_timeout_add(80, [](gpointer) -> gboolean { update_anim_css(); return G_SOURCE_CONTINUE; }, nullptr);
    } else {
        g_wv.reveal = 1; g_anim_t = 0;
        gtk_css_provider_load_from_string(g_anim_css, "");
        gtk_widget_queue_draw(g_waves);
    }
    if (!g_stats_timer) g_stats_timer = g_timeout_add(1500, refresh_stats, nullptr);
}
static void on_unmap(GtkWidget*, gpointer) {
    if (g_tick) { gtk_widget_remove_tick_callback(g_waves, g_tick); g_tick = 0; }
    if (g_anim_timer) { g_source_remove(g_anim_timer); g_anim_timer = 0; }
    if (g_stats_timer) { g_source_remove(g_stats_timer); g_stats_timer = 0; }
}

static gboolean on_key(GtkEventControllerKey*, guint kv, guint, GdkModifierType, gpointer) {
    switch (kv) {
        case GDK_KEY_Escape: case GDK_KEY_q: hide_about(); return TRUE;
        case GDK_KEY_u: start_update(); return TRUE;
        case GDK_KEY_r: check_updates(true); return TRUE;
        case GDK_KEY_b: g_spawn_command_line_async("kitty -e btop", nullptr); hide_about(); return TRUE;
    }
    return FALSE;
}

static void on_bg_click(GtkGestureClick* g, int, double x, double y, gpointer) {
    GtkWidget* root = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    graphene_rect_t r;
    graphene_point_t pt = {(float)x, (float)y};
    if (gtk_widget_compute_bounds(g_card, root, &r) && graphene_rect_contains_point(&r, &pt)) return;
    hide_about();
}

static gboolean on_socket(GIOChannel* src, GIOCondition, gpointer) {
    int fd = accept(g_io_channel_unix_get_fd(src), nullptr, nullptr);
    if (fd < 0) return G_SOURCE_CONTINUE;
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1); close(fd);
    if (n <= 0) return G_SOURCE_CONTINUE;
    std::string c(buf);
    if (c.find("hide") != std::string::npos) hide_about();
    else if (c.find("show") != std::string::npos) show_about();
    else if (c.find("toggle") != std::string::npos) { if (gtk_widget_get_visible(g_win)) hide_about(); else show_about(); }
    return G_SOURCE_CONTINUE;
}

static void setup_socket() {
    std::string path = sysui::home() + "/.config/SYSui/about.sock";
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_un a{}; a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    if (bind(fd, (sockaddr*)&a, sizeof a) < 0) { close(fd); return; }
    listen(fd, 4);
    g_io_add_watch(g_io_channel_unix_new(fd), G_IO_IN, on_socket, nullptr);
}

static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    if (g_win) return;
    setup_socket();
    g_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_anim_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_anim_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);

    g_win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_win), "Megumin OS — про систему");
    sysui::release_on_hide(g_win);
    GtkWindow* w = GTK_WINDOW(g_win);
    gtk_layer_init_for_window(w);
    gtk_layer_set_namespace(w, "about");
    gtk_layer_set_layer(w, GTK_LAYER_SHELL_LAYER_OVERLAY);
    for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
        gtk_layer_set_anchor(w, e, TRUE);
    gtk_layer_set_exclusive_zone(w, -1);
    gtk_layer_set_keyboard_mode(w, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root, "dim");
    GtkGesture* bg = gtk_gesture_click_new();
    g_signal_connect(bg, "released", G_CALLBACK(on_bg_click), nullptr);
    gtk_widget_add_controller(root, GTK_EVENT_CONTROLLER(bg));

    g_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(g_card, "about-card");
    gtk_widget_set_overflow(g_card, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(g_card, 700, -1);
    gtk_widget_set_halign(g_card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(g_card, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(g_card, TRUE);

    // ---------- шапка ----------
    GtkWidget* hdr = gtk_overlay_new();
    g_waves = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_waves), 196);
    gtk_widget_set_hexpand(g_waves, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_waves), draw_waves, nullptr, nullptr);
    gtk_overlay_set_child(GTK_OVERLAY(hdr), g_waves);

    GtkWidget* topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(topbar, GTK_ALIGN_START);
    gtk_widget_set_margin_top(topbar, 14); gtk_widget_set_margin_start(topbar, 16); gtk_widget_set_margin_end(topbar, 16);
    l_host = label("", "chip"); gtk_widget_add_css_class(l_host, "mono");
    gtk_box_append(GTK_BOX(topbar), l_host);
    GtkWidget* sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(topbar), sp);
    l_uptime = label("", "chip"); gtk_widget_add_css_class(l_uptime, "mono");
    gtk_box_append(GTK_BOX(topbar), l_uptime);
    gtk_overlay_add_overlay(GTK_OVERLAY(hdr), topbar);

    GtkWidget* tbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(tbox, GTK_ALIGN_END);
    gtk_widget_set_halign(tbox, GTK_ALIGN_START);
    gtk_widget_set_margin_start(tbox, 22); gtk_widget_set_margin_bottom(tbox, 10);
    GtkWidget* trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(trow), label("Megumin OS", "title"));
    GtkWidget* badge = label("6", "badge", 0.5);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(trow), badge);
    gtk_box_append(GTK_BOX(tbox), trow);
    l_subtitle = label("", "subtitle");
    gtk_box_append(GTK_BOX(tbox), l_subtitle);
    gtk_overlay_add_overlay(GTK_OVERLAY(hdr), tbox);
    gtk_box_append(GTK_BOX(g_card), hdr);

    // ---------- тіло ----------
    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(body, "body");
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10); gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_attach(GTK_GRID(grid), make_tile("󰻠", "ПРОЦЕСОР", t_cpu), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_tile("󰢮", "ВІДЕОКАРТА", t_gpu), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_tile("󰘚", "ПАМ'ЯТЬ", t_ram), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_tile("󰋊", "ДИСК", t_disk), 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(body), grid);

    GtkWidget* kvs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(kvs), TRUE);
    gtk_box_append(GTK_BOX(kvs), make_kv("ЯДРО", l_kernel));
    gtk_box_append(GTK_BOX(kvs), make_kv("HYPRLAND", l_hypr));
    gtk_box_append(GTK_BOX(kvs), make_kv("ОБОЛОНКА", l_shell));
    gtk_box_append(GTK_BOX(kvs), make_kv("ПАКЕТИ", l_pkgs));
    gtk_box_append(GTK_BOX(body), kvs);

    // оновлення
    GtkWidget* uh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(uh, 2);
    gtk_box_append(GTK_BOX(uh), label("󰏗  Неоновлені пакети", "section"));
    l_upd_badge = label("…", "count", 0.5);
    gtk_widget_set_valign(l_upd_badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(uh), l_upd_badge);
    GtkWidget* sp2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_set_hexpand(sp2, TRUE);
    gtk_box_append(GTK_BOX(uh), sp2);
    gtk_box_append(GTK_BOX(uh), label("B btop · R перевірити · U оновити · Esc", "hint"));
    gtk_box_append(GTK_BOX(body), uh);

    GtkWidget* ubox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(ubox, "upd-box");
    l_upd_status = label("Перевіряю…", "status");
    gtk_box_append(GTK_BOX(ubox), l_upd_status);
    g_upd_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_upd_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(g_upd_scroll), 150);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(g_upd_scroll), TRUE);
    g_upd_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(g_upd_scroll), g_upd_list);
    gtk_widget_set_visible(g_upd_scroll, FALSE);
    gtk_box_append(GTK_BOX(ubox), g_upd_scroll);
    gtk_box_append(GTK_BOX(body), ubox);

    // кнопки
    GtkWidget* btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(btns, 4);
    gtk_box_append(GTK_BOX(btns), make_btn("󰒋  btop", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) {
        g_spawn_command_line_async("kitty -e btop", nullptr); hide_about(); })));
    GtkWidget* sp3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_set_hexpand(sp3, TRUE);
    gtk_box_append(GTK_BOX(btns), sp3);
    b_check = make_btn("󰑓  Перевірити", nullptr, G_CALLBACK(+[](GtkButton*, gpointer) { check_updates(true); }));
    gtk_box_append(GTK_BOX(btns), b_check);
    b_update = make_btn("󰚰  Оновити", "success", G_CALLBACK(+[](GtkButton*, gpointer) { start_update(); }), &l_update_btn);
    gtk_box_append(GTK_BOX(btns), b_update);
    gtk_box_append(GTK_BOX(btns), make_btn("󰅙  Закрити", "primary", G_CALLBACK(+[](GtkButton*, gpointer) { hide_about(); })));
    gtk_box_append(GTK_BOX(body), btns);

    gtk_box_append(GTK_BOX(g_card), body);
    gtk_box_append(GTK_BOX(root), g_card);
    gtk_window_set_child(w, root);
    sysui::pop_on_show(g_win, g_card);

    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), nullptr);
    gtk_widget_add_controller(g_win, key);
    g_signal_connect(g_win, "map", G_CALLBACK(on_map), nullptr);
    g_signal_connect(g_win, "unmap", G_CALLBACK(on_unmap), nullptr);

    apply_css();
    init_static();
    gtk_widget_set_visible(g_win, FALSE);
    if (getenv("ABOUT_SHOW")) show_about();  // для тестів
}

int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.about", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
