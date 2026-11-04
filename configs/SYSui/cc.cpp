// ============================================================================
//  SYSui Control Center — повністю переписаний
// ============================================================================
//  Принципи:
//   * ЖОДНОГО блокуючого виклику в головному потоці. Усі зовнішні команди йдуть
//     через GSubprocess (sh_async), медіа — через D-Bus (MPRIS), системні дані —
//     напряму з /proc і /sys.
//   * Опитування працює лише поки CC відкритий, і лише для видимої сторінки.
//   * Віджети оновлюються «на місці», а не перестворюються щоразу.
//   * Фікс ігор: при відкритті беремо клавіатуру ексклюзивно — гра втрачає фокус
//     і відпускає захоплений курсор (pointer lock), тож кнопки знову клікабельні.
//   * Сторож модулів: якщо модуль SYSui впав — системне сповіщення + автоперезапуск.
// ============================================================================
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include "sysui_common.h"
#include <gio/gio.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <functional>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <ctime>
#include <cmath>
#include <signal.h>

using json = nlohmann::json;

// ============================================================================
//  УТИЛІТИ
// ============================================================================
static gint64 now_ms() { return g_get_monotonic_time() / 1000; }

static std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

static std::string q(const std::string& s) {
    gchar* x = g_shell_quote(s.c_str());
    std::string r = x;
    g_free(x);
    return r;
}

static double parse_num(const std::string& s) {
    // Не залежить від локалі (uk_UA має кому як десятковий роздільник)
    size_t i = 0;
    while (i < s.size() && !isdigit((unsigned char)s[i]) && s[i] != '-') i++;
    bool neg = false;
    if (i < s.size() && s[i] == '-') { neg = true; i++; }
    double v = 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0');
    if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
        i++;
        double sc = 0.1;
        while (i < s.size() && isdigit((unsigned char)s[i])) { v += (s[i++] - '0') * sc; sc *= 0.1; }
    }
    return neg ? -v : v;
}

static std::string fmt_dot(double v) {
    int m = (int)(std::clamp(v, 0.0, 9.99) * 1000 + 0.5);
    char b[32]; snprintf(b, sizeof(b), "%d.%03d", m / 1000, m % 1000);
    return b;
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Асинхронна shell-команда: колбек отримує stdout і чи успішний код виходу.
using CmdCb = std::function<void(const std::string&, bool)>;
struct CmdJob { CmdCb cb; };

static void sh_async(const std::string& cmd, CmdCb cb = nullptr) {
    GError* err = nullptr;
    GSubprocess* p = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE),
                                      &err, "sh", "-c", cmd.c_str(), nullptr);
    if (!p) { if (err) g_error_free(err); if (cb) cb("", false); return; }
    auto* job = new CmdJob{std::move(cb)};
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, [](GObject* src, GAsyncResult* res, gpointer d) {
        auto* job = static_cast<CmdJob*>(d);
        char* out = nullptr;
        GError* e = nullptr;
        gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, nullptr, &e);
        bool success = ok && g_subprocess_get_if_exited(G_SUBPROCESS(src)) && g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 0;
        if (job->cb) job->cb(out ? out : "", success);
        g_free(out);
        if (e) g_error_free(e);
        g_object_unref(src);
        delete job;
    }, job);
}

// Обмежувач частоти: слайдер шле десятки змін на секунду, а процес запускаємо
// не частіше ніж раз на ms і завжди з ОСТАННІМ значенням.
struct Throttle {
    int ms;
    guint timer = 0;
    std::string pending, last;
};
static void throttle_run(Throttle* t, const std::string& cmd) {
    t->pending = cmd;
    if (t->timer) return;
    t->last = cmd;
    sh_async(cmd);
    t->timer = g_timeout_add(t->ms, [](gpointer d) -> gboolean {
        auto* t = static_cast<Throttle*>(d);
        t->timer = 0;
        if (t->pending != t->last) throttle_run(t, t->pending);
        return G_SOURCE_REMOVE;
    }, t);
}

static GtkWidget* mklabel(const std::string& text, const char* cls = nullptr, float xalign = 0.0f) {
    GtkWidget* l = gtk_label_new(text.c_str());
    if (cls) gtk_widget_add_css_class(l, cls);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    return l;
}

static GtkWidget* mkicon(const char* name, int px = 16) {
    GtkWidget* i = gtk_image_new_from_icon_name(name);
    gtk_image_set_pixel_size(GTK_IMAGE(i), px);
    return i;
}

static void clear_box(GtkWidget* box) {
    while (GtkWidget* c = gtk_widget_get_first_child(box)) gtk_widget_unparent(c);
}

static std::string time_ago(gint64 t) {
    gint64 d = (gint64)time(nullptr) - t;
    if (d < 60) return "щойно";
    if (d < 3600) return std::to_string(d / 60) + " хв тому";
    if (d < 86400) return std::to_string(d / 3600) + " год тому";
    return std::to_string(d / 86400) + " дн тому";
}

// ============================================================================
//  ГЛОБАЛЬНИЙ СТАН
// ============================================================================
struct CC {
    GtkWidget* win = nullptr;
    GtkWidget* root = nullptr;
    GtkWidget* stack = nullptr;
    GtkCssProvider* css = nullptr;
    bool open = false;
    guint poll_timer = 0;
    guint hide_timer = 0;
    long long theme_stamp = -1;

    // головна сторінка
    GtkWidget *notif_count = nullptr, *notif_preview = nullptr, *notif_badge = nullptr;
    GtkWidget *wifi_circle = nullptr, *wifi_status = nullptr;
    GtkWidget *bt_circle = nullptr, *bt_status = nullptr;
    GtkWidget *eth_circle = nullptr, *eth_status = nullptr;
    GtkWidget *theme_tile = nullptr, *theme_icon = nullptr, *theme_status = nullptr;
    GtkWidget *dnd_tile = nullptr, *dnd_icon = nullptr, *dnd_status = nullptr;
    GtkWidget* pp_btn[3] = {nullptr, nullptr, nullptr};
    GtkWidget *bright_tile = nullptr, *bright_scale = nullptr, *bright_pct = nullptr;
    GtkWidget *vol_scale = nullptr, *vol_btn = nullptr, *vol_pct = nullptr;
    GtkWidget *mic_scale = nullptr, *mic_btn = nullptr, *mic_pct = nullptr;
    GtkWidget* media_box = nullptr;
    GtkWidget* eco_btn = nullptr;
    std::vector<GtkWidget*> anim_tiles;

    // стан
    bool wifi_on = false, bt_on = false, dnd = false, vol_muted = false, mic_muted = false, eco = false;
    int eco_saved_rounding = 10;
    std::string pp;
    gint64 touch_vol = 0, touch_mic = 0, touch_bright = 0;
    bool ddc_ok = false;

    Throttle th_vol{50}, th_mic{50}, th_bright{350};
};
static CC cc;

static void cc_close();
static void go_page(const char* name);
static void poll_status();

// ============================================================================
//  CSS
// ============================================================================
static std::string build_css() {
    const auto& st = sysui::settings();
    std::string A = sysui::accent();
    double op = st.cc_opacity / 100.0;

    std::string css = R"CSS(
window { background: transparent; }
* { font-family: "SF Pro Display", "Inter", "JetBrainsMono Nerd Font", sans-serif; outline: none; }

/* ---------- Панель ---------- */
.cc-root {
    background-color: @PANEL@;
    border: 1px solid rgba(255,255,255,0.10);
    border-radius: 24px;
    padding: 12px;
    color: #f5f5f7;
}
.cc-root label { color: #f5f5f7; }
.sub { font-size: 11.5px; opacity: 0.62; }
.title { font-weight: 600; font-size: 13.5px; }
.h2 { font-weight: 700; font-size: 15px; }
.caption { font-size: 10.5px; opacity: 0.7; }

/* ---------- Плитки ---------- */
.tile {
    background-color: rgba(255,255,255,0.075);
    border: 1px solid rgba(255,255,255,0.05);
    border-radius: 18px;
    padding: 12px;
}
button.tile { padding: 12px; box-shadow: none; }
button.tile:hover { background-color: rgba(255,255,255,0.11); }
.tile.on { background-color: @ACCENT_SOFT@; border-color: @ACCENT_A40@; }

button { background: none; border: none; box-shadow: none; padding: 0; min-height: 0; min-width: 0; color: inherit; }

/* Круглі кнопки-перемикачі */
.circle {
    min-width: 32px; min-height: 32px; border-radius: 999px;
    background-color: rgba(255,255,255,0.13);
}
.circle:hover { background-color: rgba(255,255,255,0.2); }
.circle.on, .circle-icon.on { background-color: @ACCENT@; }
.circle.on image, .circle-icon.on image { color: #ffffff; }
.circle-icon { min-width: 32px; min-height: 32px; border-radius: 999px; background-color: rgba(255,255,255,0.13); }

.row-btn { border-radius: 10px; padding: 2px 6px; }
.row-btn:hover { background-color: rgba(255,255,255,0.07); }

/* Сегменти (режим живлення, фільтри) */
.segbox { background-color: rgba(0,0,0,0.22); border-radius: 11px; padding: 3px; }
.seg { border-radius: 9px; padding: 5px 10px; font-size: 12px; font-weight: 600; opacity: 0.75; }
.seg:hover { background-color: rgba(255,255,255,0.08); opacity: 1; }
.seg.on { background-color: @ACCENT@; color: #ffffff; opacity: 1; }
.seg.on label { color: #ffffff; }

/* Квадратні кнопки знизу */
.square { min-width: 52px; min-height: 52px; border-radius: 16px;
          background-color: rgba(255,255,255,0.075); border: 1px solid rgba(255,255,255,0.05); }
.square:hover { background-color: rgba(255,255,255,0.13); }
.square.on { background-color: @ACCENT@; }
.square.on image { color: #fff; }

/* ---------- Слайдери ---------- */
scale { padding: 6px 0; }
scale trough { min-height: 6px; border-radius: 999px; background-color: rgba(255,255,255,0.14); }
scale highlight { border-radius: 999px; background-color: @ACCENT@; }
scale slider {
    min-width: 16px; min-height: 16px; margin: -6px; border-radius: 999px;
    background-color: #ffffff; border: none; box-shadow: 0 1px 3px rgba(0,0,0,0.4);
    transition: transform 220ms cubic-bezier(0.34, 1.56, 0.64, 1);
}
scale:hover slider { transform: scale(1.18); }
scale.media trough { min-height: 4px; }
scale.media slider { min-width: 10px; min-height: 10px; margin: -4px; opacity: 0; }
scale.media:hover slider { opacity: 1; }
.icon-btn { min-width: 28px; min-height: 28px; border-radius: 999px; }
.icon-btn:hover { background-color: rgba(255,255,255,0.1); }
.icon-btn.muted image { color: #ff6b6b; }
.pct { font-size: 11px; font-weight: 600; opacity: 0.7; min-width: 34px; }

/* ---------- Медіа ---------- */
.art { border-radius: 12px; background-color: rgba(255,255,255,0.08); }
.media-ctl { min-width: 32px; min-height: 32px; border-radius: 999px; }
.media-ctl:hover { background-color: rgba(255,255,255,0.12); }
.media-ctl.big image { -gtk-icon-size: 20px; }

/* ---------- Сторінки ---------- */
.back { min-width: 30px; min-height: 30px; border-radius: 999px; background-color: rgba(255,255,255,0.1); }
.back:hover { background-color: rgba(255,255,255,0.18); }
.list-row { border-radius: 12px; padding: 9px 10px; }
.list-row:hover { background-color: rgba(255,255,255,0.08); }
.list-row.active { background-color: @ACCENT_SOFT@; }
.check { color: @ACCENT@; }
entry.pw {
    background-color: rgba(0,0,0,0.25); border: 1px solid rgba(255,255,255,0.12);
    border-radius: 10px; padding: 4px 10px; color: #f5f5f7; min-height: 30px;
}
entry.pw:focus-within { border-color: @ACCENT@; }
.pill { background-color: @ACCENT@; color: #fff; border-radius: 10px; padding: 6px 14px; font-weight: 600; }
.pill label { color: #fff; }
.pill:hover { opacity: 0.88; }
.ghost { background-color: rgba(255,255,255,0.1); border-radius: 10px; padding: 6px 12px; font-weight: 600; }
.ghost:hover { background-color: rgba(255,255,255,0.16); }
.toast { background-color: rgba(0,0,0,0.3); border-radius: 10px; padding: 8px 12px; font-size: 12px; }
.toast.err { background-color: rgba(255,90,90,0.22); }
.toast.ok  { background-color: rgba(80,200,120,0.22); }
switch { border-radius: 999px; background-color: rgba(255,255,255,0.18); border: none; min-height: 22px; }
switch:checked { background-color: @ACCENT@; }
switch slider { background-color: #fff; border-radius: 999px; min-width: 20px; min-height: 20px; margin: 1px; border: none; box-shadow: 0 1px 2px rgba(0,0,0,0.3); }

progressbar trough { min-height: 6px; border-radius: 999px; background-color: rgba(255,255,255,0.12); }
progressbar progress { min-height: 6px; border-radius: 999px; background-color: @ACCENT@; }
progressbar.hot progress { background-color: #ff7a59; }
.dot { min-width: 8px; min-height: 8px; border-radius: 999px; background-color: #4cd964; }
.dot.down { background-color: #ff5f57; }

/* ---------- Сповіщення ---------- */
.notif { background-color: rgba(255,255,255,0.07); border-radius: 16px; padding: 10px 12px; }
.notif.warn  { background-color: rgba(255,190,60,0.14); }
.notif.error { background-color: rgba(255,90,90,0.16); }
.notif .dismiss { opacity: 0; min-width: 22px; min-height: 22px; border-radius: 999px; background-color: rgba(255,255,255,0.12); transition: opacity 160ms ease; }
.notif:hover .dismiss { opacity: 1; }
.badge { background-color: #ff5f57; color: #fff; border-radius: 999px; padding: 0 6px; font-size: 10.5px; font-weight: 700; min-height: 18px; }
.badge label { color: #fff; }

scrollbar { background: transparent; }
scrollbar slider { background-color: rgba(255,255,255,0.25); border-radius: 999px; min-width: 4px; }

/* ---------- Живі анімації CC ---------- */
@keyframes tile-in {
    from { opacity: 0; transform: translateY(-10px) scale(0.97); }
    to   { opacity: 1; transform: none; }
}
.tile-in { animation: tile-in 420ms cubic-bezier(0.22, 1, 0.36, 1) backwards; }
.d1 { animation-delay: 20ms; }  .d2 { animation-delay: 55ms; }  .d3 { animation-delay: 90ms; }
.d4 { animation-delay: 125ms; } .d5 { animation-delay: 160ms; } .d6 { animation-delay: 195ms; }
.d7 { animation-delay: 230ms; } .d8 { animation-delay: 265ms; }
@keyframes cc-out { to { opacity: 0; transform: translateY(-10px) scale(0.96); } }
.cc-out { animation: cc-out 170ms cubic-bezier(0.4, 0, 1, 1) forwards; }
@keyframes spin { to { transform: rotate(360deg); } }
.spinning image { animation: spin 900ms linear infinite; }
@keyframes pulse { 0% { transform: scale(1); } 40% { transform: scale(1.18); } 100% { transform: scale(1); } }
.pulse { animation: pulse 380ms cubic-bezier(0.34, 1.56, 0.64, 1); }

/* ---------- Світла тема ---------- */
window.light .cc-root { background-color: @PANEL_LIGHT@; border-color: rgba(0,0,0,0.08); color: #1d1d1f; }
window.light .cc-root label { color: #1d1d1f; }
window.light .cc-root image { color: #1d1d1f; }
window.light .tile, window.light .square, window.light .notif { background-color: rgba(0,0,0,0.05); border-color: rgba(0,0,0,0.04); }
window.light button.tile:hover, window.light .square:hover { background-color: rgba(0,0,0,0.09); }
window.light .circle, window.light .circle-icon, window.light .back, window.light .ghost { background-color: rgba(0,0,0,0.08); }
window.light .row-btn:hover, window.light .list-row:hover, window.light .icon-btn:hover, window.light .media-ctl:hover { background-color: rgba(0,0,0,0.06); }
window.light scale trough, window.light progressbar trough { background-color: rgba(0,0,0,0.1); }
window.light .segbox { background-color: rgba(0,0,0,0.07); }
window.light entry.pw { background-color: rgba(255,255,255,0.7); border-color: rgba(0,0,0,0.12); color: #1d1d1f; }
window.light .circle.on image, window.light .circle-icon.on image, window.light .seg.on label,
window.light .square.on image, window.light .pill label, window.light .badge label { color: #ffffff; }
)CSS";

    auto rep = [&](const std::string& k, const std::string& v) {
        size_t p = 0;
        while ((p = css.find(k, p)) != std::string::npos) { css.replace(p, k.size(), v); p += v.size(); }
    };
    rep("@PANEL_LIGHT@", "rgba(246,246,248," + fmt_dot(std::min(1.0, op + 0.08)) + ")");
    rep("@PANEL@", "rgba(28,28,32," + fmt_dot(op) + ")");
    rep("@ACCENT_SOFT@", sysui::rgba(A, 0.28));
    rep("@ACCENT_A40@", sysui::rgba(A, 0.40));
    rep("@ACCENT@", A);
    return css;
}

static void apply_theme(bool force = false) {
    long long st = sysui::theme_stamp();
    if (!force && st == cc.theme_stamp) goto scheme;
    cc.theme_stamp = st;
    if (!cc.css) {
        cc.css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(cc.css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string(cc.css, build_css().c_str());
scheme:
    if (cc.win) {
        if (sysui::is_light_theme()) gtk_widget_add_css_class(cc.win, "light");
        else gtk_widget_remove_css_class(cc.win, "light");
    }
}

// ============================================================================
//  СПОВІЩЕННЯ (сховище)
// ============================================================================
struct Notif {
    int id;
    gint64 t;
    std::string title, body, level;  // level: "" (звичайне) | info | warn | error
    bool sys;
};
static std::deque<Notif> g_notifs;      // найновіші спереду
static int g_notif_next_id = 1;
static int g_unseen_sys_err = 0;
static std::string notif_path() { return sysui::home() + "/.cache/SYSui/notifications.jsonl"; }

static void notif_save_all() {
    g_mkdir_with_parents((sysui::home() + "/.cache/SYSui").c_str(), 0755);
    std::string tmp = notif_path() + ".tmp";
    {
        std::ofstream o(tmp);
        for (auto it = g_notifs.rbegin(); it != g_notifs.rend(); ++it)
            o << json{{"t", it->t}, {"title", it->title}, {"body", it->body}, {"level", it->level}, {"sys", it->sys}}.dump() << "\n";
    }
    rename(tmp.c_str(), notif_path().c_str());
}

static void notif_load() {
    std::ifstream in(notif_path());
    std::string line;
    while (std::getline(in, line)) {
        try {
            json j = json::parse(line);
            g_notifs.push_front({g_notif_next_id++, j.value("t", (gint64)0), j.value("title", ""), j.value("body", ""),
                                 j.value("level", ""), j.value("sys", false)});
        } catch (...) {}
    }
    // Міграція старого формату "title|||body"
    std::string old = sysui::home() + "/.cache/SYSui/notifications.log";
    std::ifstream o(old);
    bool migrated = false;
    while (std::getline(o, line)) {
        auto p = line.find("|||");
        if (p == std::string::npos) continue;
        g_notifs.push_front({g_notif_next_id++, (gint64)time(nullptr), line.substr(0, p), line.substr(p + 3), "", false});
        migrated = true;
    }
    while (g_notifs.size() > 200) g_notifs.pop_back();
    if (migrated) { notif_save_all(); unlink(old.c_str()); }
}

// Спливаюче вікно через notify_osd (той самий формат, що пише dunst-хук)
static void popup_via_osd(const std::string& title, const std::string& body, const std::string& icon) {
    {
        std::ofstream f("/tmp/notify_osd_data");
        std::string b = body;
        std::replace(b.begin(), b.end(), '\n', ' ');
        f << "SENDER=" << title << "\nICON=" << icon << "\nBODY=" << b << "\n";
    }
    std::string path = sysui::home() + "/.config/SYSui/notify.sock";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) { ssize_t w = write(fd, "show", 4); (void)w; }
    close(fd);
}

static void notif_ui_added(const Notif& n);   // нижче
static void update_notif_tile();

static void notif_add(const std::string& title, const std::string& body, const std::string& level, bool sys) {
    Notif n{g_notif_next_id++, (gint64)time(nullptr), title, body, level, sys};
    g_notifs.push_front(n);
    while (g_notifs.size() > 200) g_notifs.pop_back();
    g_mkdir_with_parents((sysui::home() + "/.cache/SYSui").c_str(), 0755);
    {
        std::ofstream o(notif_path(), std::ios::app);
        o << json{{"t", n.t}, {"title", n.title}, {"body", n.body}, {"level", n.level}, {"sys", n.sys}}.dump() << "\n";
    }
    if (sys) {
        if (level == "error" || level == "warn") g_unseen_sys_err++;
        // Спливаючими — лише попередження й помилки; інформаційні події тільки в історії
        bool show = sysui::settings().sys_popups && level != "info";
        if (show) popup_via_osd("SYSui · " + title, body,
                                level == "error" ? "dialog-error" : level == "warn" ? "dialog-warning" : "dialog-information");
    }
    update_notif_tile();
    notif_ui_added(n);
}

static void sys_event(const std::string& level, const std::string& title, const std::string& body) {
    notif_add(title, body, level, true);
}

// ============================================================================
//  СТОРОЖ МОДУЛІВ (watchdog)
// ============================================================================
struct Module {
    const char* bin;
    const char* name;
    bool restartable;    // wallpapers перезапускає власний скрипт
    int pid = 0;
    bool seen = false;
    int missing = 0;
    bool gave_up = false;
    std::deque<gint64> restarts;
    gint64 we_started_at = 0;
    GtkWidget* dot = nullptr;
    GtkWidget* status = nullptr;
};
static std::vector<Module> g_modules = {
    {"dock", "Док", true}, {"topbar", "Топбар", true}, {"runner_osd", "Лаунчер", true},
    {"wallpapers", "Шпалери", false}, {"clipboard_osd", "Буфер обміну", true},
    {"notify_osd", "Спливаючі сповіщення", true}, {"megumin_osd", "Музичний OSD", true},
    {"powermenu_osd", "Меню живлення", true}, {"volume_osd", "OSD гучності", true},
    {"controller_addon", "Геймпад", true}, {"about_osd", "Про систему", true},
};
static std::string sysui_dir() { return sysui::home() + "/.config/SYSui/"; }

static std::map<std::string, int> scan_sysui_pids() {
    std::map<std::string, int> r;
    std::string dir = sysui_dir();
    DIR* d = opendir("/proc");
    if (!d) return r;
    struct dirent* e;
    char path[64], buf[512];
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/exe", e->d_name);
        ssize_t n = readlink(path, buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string exe = buf;
        if (exe.compare(0, dir.size(), dir) != 0) continue;
        std::string bin = exe.substr(dir.size());
        auto sp = bin.find(' ');                 // "dock (deleted)" після перезбірки
        if (sp != std::string::npos) bin = bin.substr(0, sp);
        r[bin] = atoi(e->d_name);
    }
    closedir(d);
    return r;
}

static void module_restart(Module& m, bool manual) {
    m.we_started_at = now_ms();
    m.missing = 0;
    if (manual) { m.gave_up = false; m.restarts.clear(); }
    m.restarts.push_back(now_ms());
    if (std::string(m.bin) == "wallpapers") {
        // керується wallpapers_watch.sh — достатньо вбити, скрипт підніме
        sh_async("pkill -x wallpapers");
    } else {
        sysui::hypr_request("dispatch exec " + sysui_dir() + m.bin);
    }
}

static void refresh_module_rows() {
    for (auto& m : g_modules) {
        if (!m.dot) continue;
        if (m.pid) gtk_widget_remove_css_class(m.dot, "down"); else gtk_widget_add_css_class(m.dot, "down");
        gtk_label_set_text(GTK_LABEL(m.status), m.pid ? ("працює · PID " + std::to_string(m.pid)).c_str()
                                                       : (m.seen ? "не працює" : "не запускався"));
    }
}

static gboolean watchdog_tick(gpointer) {
    auto pids = scan_sysui_pids();
    bool enabled = sysui::settings().watchdog;
    for (auto& m : g_modules) {
        auto it = pids.find(m.bin);
        int pid = it == pids.end() ? 0 : it->second;
        if (pid) {
            if (m.seen && m.pid && pid != m.pid && now_ms() - m.we_started_at > 8000)
                sys_event("info", "Модуль «" + std::string(m.name) + "» перезапустився",
                          "Процес змінився (PID " + std::to_string(m.pid) + " → " + std::to_string(pid) + "). Ймовірно, після збою.");
            if (m.missing >= 2 && m.seen) sys_event("info", "Модуль «" + std::string(m.name) + "» знову працює", "");
            m.pid = pid; m.seen = true; m.missing = 0;
            continue;
        }
        if (!m.seen) continue;          // модуль ніколи не запускався — не наша справа
        if (now_ms() - m.we_started_at < 15000) { m.pid = 0; continue; }  // плановий перезапуск
        m.pid = 0;
        m.missing++;
        if (m.missing != 2) continue;   // 2 перевірки поспіль (~6 с) — щоб не реагувати на ручний рестарт
        if (!enabled || (!m.restartable && std::string(m.bin) != "wallpapers")) {
            sys_event("error", "Модуль «" + std::string(m.name) + "» відвалився",
                      enabled ? "Автоперезапуск для цього модуля не підтримується." : "Сторож вимкнено в налаштуваннях — перезапустіть вручну (Система → Модулі).");
            continue;
        }
        while (!m.restarts.empty() && now_ms() - m.restarts.front() > 120000) m.restarts.pop_front();
        if (m.gave_up || m.restarts.size() >= 3) {
            if (!m.gave_up)
                sys_event("error", "Модуль «" + std::string(m.name) + "» постійно падає",
                          "3 збої за 2 хвилини — автоперезапуск зупинено. Перевірте модуль і перезапустіть вручну.");
            m.gave_up = true;
            continue;
        }
        sys_event("warn", "Модуль «" + std::string(m.name) + "» відвалився", "Перезапускаю автоматично…");
        if (std::string(m.bin) == "wallpapers") { m.missing = 0; continue; }  // скрипт сам підніме
        module_restart(m, false);
    }
    refresh_module_rows();
    return G_SOURCE_CONTINUE;
}

// ============================================================================
//  МЕДІА (MPRIS через D-Bus)
// ============================================================================
struct Player {
    std::string bus;
    GDBusProxy* proxy = nullptr;
    GtkWidget *card = nullptr, *art = nullptr, *title = nullptr, *artist = nullptr, *play = nullptr, *progress = nullptr;
    std::string art_url, trackid, status;
    double length = 0;
    gint64 touch_seek = 0;
    bool visible = false;
};
static std::map<std::string, Player*> g_players;
static GDBusConnection* g_bus = nullptr;

static std::string art_dir() { return sysui::home() + "/.cache/SYSui/art"; }

struct ArtReq { std::string bus, url, path; };
static void player_set_art(Player* p, const std::string& path) {
    int sf = std::max(1, gtk_widget_get_scale_factor(p->art));
    GdkTexture* t = sysui::load_texture_cover(path.c_str(), 52 * sf, 52 * sf);
    gtk_image_set_from_paintable(GTK_IMAGE(p->art), t ? GDK_PAINTABLE(t) : nullptr);
    if (t) g_object_unref(t);
}

static void player_load_art(Player* p, const std::string& url) {
    if (url == p->art_url) return;
    p->art_url = url;
    if (url.empty()) { gtk_image_clear(GTK_IMAGE(p->art)); return; }
    if (url.rfind("file://", 0) == 0) {
        gchar* fn = g_filename_from_uri(url.c_str(), nullptr, nullptr);
        if (fn) { player_set_art(p, fn); g_free(fn); }
        return;
    }
    gchar* h = g_compute_checksum_for_string(G_CHECKSUM_MD5, url.c_str(), -1);
    std::string path = art_dir() + "/" + h + ".img";
    g_free(h);
    if (access(path.c_str(), R_OK) == 0) { player_set_art(p, path); return; }
    g_mkdir_with_parents(art_dir().c_str(), 0755);
    std::string bus = p->bus;
    sh_async("curl -sfL --max-time 10 -o " + q(path + ".part") + " " + q(url) + " && mv " + q(path + ".part") + " " + q(path),
             [bus, url, path](const std::string&, bool ok) {
                 auto it = g_players.find(bus);
                 if (ok && it != g_players.end() && it->second->art_url == url) player_set_art(it->second, path);
             });
}

static void reorder_media() {
    if (!cc.media_box) return;
    GtkWidget* prev = nullptr;
    for (int pass = 0; pass < 2; pass++)
        for (auto& [k, p] : g_players) {
            if (!p->card) continue;
            bool playing = p->status == "Playing";
            if ((pass == 0) != playing) continue;
            gtk_box_reorder_child_after(GTK_BOX(cc.media_box), p->card, prev);
            prev = p->card;
        }
    bool any = false;
    for (auto& [k, p] : g_players) any |= p->visible;
    gtk_widget_set_visible(cc.media_box, any);
}

static void player_update(Player* p) {
    std::string status, title, artist, art, url;
    double len = 0;
    if (GVariant* v = g_dbus_proxy_get_cached_property(p->proxy, "PlaybackStatus")) {
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) status = g_variant_get_string(v, nullptr);
        g_variant_unref(v);
    }
    if (GVariant* md = g_dbus_proxy_get_cached_property(p->proxy, "Metadata")) {
        if (g_variant_is_of_type(md, G_VARIANT_TYPE_VARDICT)) {
            GVariantDict d;
            g_variant_dict_init(&d, md);
            const char* s = nullptr;
            if (g_variant_dict_lookup(&d, "xesam:title", "&s", &s)) title = s;
            if (g_variant_dict_lookup(&d, "mpris:artUrl", "&s", &s)) art = s;
            if (g_variant_dict_lookup(&d, "xesam:url", "&s", &s)) url = s;
            if (GVariant* tid = g_variant_dict_lookup_value(&d, "mpris:trackid", nullptr)) {
                if (g_variant_is_of_type(tid, G_VARIANT_TYPE_OBJECT_PATH) || g_variant_is_of_type(tid, G_VARIANT_TYPE_STRING))
                    p->trackid = g_variant_get_string(tid, nullptr);
                g_variant_unref(tid);
            }
            if (GVariant* l = g_variant_dict_lookup_value(&d, "mpris:length", nullptr)) {
                if (g_variant_is_of_type(l, G_VARIANT_TYPE_INT64)) len = g_variant_get_int64(l) / 1e6;
                else if (g_variant_is_of_type(l, G_VARIANT_TYPE_UINT64)) len = g_variant_get_uint64(l) / 1e6;
                g_variant_unref(l);
            }
            if (GVariant* a = g_variant_dict_lookup_value(&d, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY)) {
                gsize n = 0;
                const gchar** arr = g_variant_get_strv(a, &n);
                for (gsize i = 0; i < n; i++) { if (i) artist += ", "; artist += arr[i]; }
                g_free(arr);
                g_variant_unref(a);
            }
            g_variant_dict_clear(&d);
        }
        g_variant_unref(md);
    }
    // Немає власної обкладинки (звук із сайту) — беремо фавікон сайту
    if (art.empty() && url.rfind("http", 0) == 0) {
        std::string dom = url.substr(url.find("://") + 3);
        dom = dom.substr(0, dom.find('/'));
        if (!dom.empty()) art = "https://www.google.com/s2/favicons?sz=128&domain=" + dom;
    }

    bool was_playing = p->status == "Playing";
    p->status = status;
    p->length = len;
    p->visible = (status == "Playing" || status == "Paused") && !title.empty();
    gtk_widget_set_visible(p->card, p->visible);
    if (p->visible) {
        gtk_label_set_text(GTK_LABEL(p->title), title.c_str());
        std::string src = p->bus.substr(strlen("org.mpris.MediaPlayer2."));
        src = src.substr(0, src.find('.'));
        gtk_label_set_text(GTK_LABEL(p->artist), artist.empty() ? src.c_str() : artist.c_str());
        gtk_button_set_icon_name(GTK_BUTTON(p->play), status == "Playing" ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
        if (was_playing != (status == "Playing")) {
            gtk_widget_remove_css_class(p->play, "pulse");
            g_idle_add([](gpointer w) -> gboolean { gtk_widget_add_css_class(GTK_WIDGET(w), "pulse"); return G_SOURCE_REMOVE; }, p->play);
        }
        if (len > 0) gtk_range_set_range(GTK_RANGE(p->progress), 0, len);
        gtk_widget_set_visible(p->progress, len > 0);
        player_load_art(p, art);
    }
    reorder_media();
}

static void player_call(Player* p, const char* m) {
    g_dbus_proxy_call(p->proxy, m, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

static GtkWidget* player_build_card(Player* p) {
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(card, "tile");
    gtk_widget_add_css_class(card, "live-soft");

    p->art = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(p->art), 52);
    gtk_widget_set_valign(p->art, GTK_ALIGN_CENTER);
    gtk_widget_set_overflow(p->art, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(p->art, "art");

    GtkWidget* mid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(mid, TRUE);
    gtk_widget_set_valign(mid, GTK_ALIGN_CENTER);
    p->title = mklabel("", "title");
    gtk_label_set_ellipsize(GTK_LABEL(p->title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(p->title), 20);
    p->artist = mklabel("", "sub");
    gtk_label_set_ellipsize(GTK_LABEL(p->artist), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(p->artist), 22);
    p->progress = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
    gtk_widget_add_css_class(p->progress, "media");
    gtk_scale_set_draw_value(GTK_SCALE(p->progress), FALSE);
    gtk_box_append(GTK_BOX(mid), p->title);
    gtk_box_append(GTK_BOX(mid), p->artist);
    gtk_box_append(GTK_BOX(mid), p->progress);

    // перемотування — лише від користувача (change-value), без петель оновлення
    g_signal_connect(p->progress, "change-value", G_CALLBACK(+[](GtkRange*, GtkScrollType, double v, gpointer d) -> gboolean {
        auto* p = static_cast<Player*>(d);
        p->touch_seek = now_ms();
        if (!p->trackid.empty())
            g_dbus_proxy_call(p->proxy, "SetPosition", g_variant_new("(ox)", p->trackid.c_str(), (gint64)(v * 1e6)),
                              G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
        return FALSE;
    }), p);

    GtkWidget* ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(ctl, GTK_ALIGN_CENTER);
    GtkWidget* prev = gtk_button_new_from_icon_name("media-skip-backward-symbolic");
    p->play = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    GtkWidget* next = gtk_button_new_from_icon_name("media-skip-forward-symbolic");
    for (GtkWidget* b : {prev, p->play, next}) { gtk_widget_add_css_class(b, "media-ctl"); gtk_widget_add_css_class(b, "live"); }
    gtk_widget_add_css_class(p->play, "big");
    g_signal_connect(prev, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { player_call((Player*)d, "Previous"); }), p);
    g_signal_connect(p->play, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { player_call((Player*)d, "PlayPause"); }), p);
    g_signal_connect(next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { player_call((Player*)d, "Next"); }), p);
    gtk_box_append(GTK_BOX(ctl), prev);
    gtk_box_append(GTK_BOX(ctl), p->play);
    gtk_box_append(GTK_BOX(ctl), next);

    gtk_box_append(GTK_BOX(card), p->art);
    gtk_box_append(GTK_BOX(card), mid);
    gtk_box_append(GTK_BOX(card), ctl);
    gtk_widget_set_visible(card, FALSE);
    return card;
}

static void player_add(const std::string& bus) {
    if (g_players.count(bus) || bus.find("playerctld") != std::string::npos) return;
    auto* p = new Player();
    p->bus = bus;
    g_players[bus] = p;
    p->card = player_build_card(p);
    if (cc.media_box) gtk_box_append(GTK_BOX(cc.media_box), p->card);
    g_dbus_proxy_new(g_bus, G_DBUS_PROXY_FLAGS_NONE, nullptr, bus.c_str(), "/org/mpris/MediaPlayer2",
                     "org.mpris.MediaPlayer2.Player", nullptr,
                     [](GObject*, GAsyncResult* res, gpointer d) {
                         std::string* bus = static_cast<std::string*>(d);
                         GDBusProxy* px = g_dbus_proxy_new_finish(res, nullptr);
                         auto it = g_players.find(*bus);
                         delete bus;
                         if (!px) return;
                         if (it == g_players.end()) { g_object_unref(px); return; }
                         it->second->proxy = px;
                         g_signal_connect(px, "g-properties-changed", G_CALLBACK(+[](GDBusProxy*, GVariant*, GStrv, gpointer d) {
                             player_update(static_cast<Player*>(d));
                         }), it->second);
                         player_update(it->second);
                     }, new std::string(bus));
}

static void player_remove(const std::string& bus) {
    auto it = g_players.find(bus);
    if (it == g_players.end()) return;
    Player* p = it->second;
    if (p->card) gtk_widget_unparent(p->card);
    if (p->proxy) g_object_unref(p->proxy);
    delete p;
    g_players.erase(it);
    reorder_media();
}

static void poll_positions() {
    for (auto& [bus, p] : g_players) {
        if (!p->visible || !p->proxy || p->length <= 0) continue;
        if (now_ms() - p->touch_seek < 1500) continue;
        std::string b = bus;
        g_dbus_connection_call(g_bus, bus.c_str(), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Position"), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 1000, nullptr,
            [](GObject* src, GAsyncResult* res, gpointer d) {
                std::string* bus = static_cast<std::string*>(d);
                GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
                auto it = g_players.find(*bus);
                delete bus;
                if (!r) return;
                GVariant* v = nullptr;
                g_variant_get(r, "(v)", &v);
                if (v && it != g_players.end()) {
                    double pos = g_variant_is_of_type(v, G_VARIANT_TYPE_INT64) ? g_variant_get_int64(v) / 1e6
                               : g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64) ? g_variant_get_uint64(v) / 1e6 : -1;
                    if (pos >= 0) gtk_range_set_value(GTK_RANGE(it->second->progress), pos);
                }
                if (v) g_variant_unref(v);
                g_variant_unref(r);
            }, new std::string(b));
    }
}

static void mpris_init() {
    g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!g_bus) return;
    g_dbus_connection_call(g_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames",
        nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer) {
            GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
            if (!r) return;
            GVariantIter* it;
            const char* name;
            g_variant_get(r, "(as)", &it);
            while (g_variant_iter_next(it, "&s", &name))
                if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) player_add(name);
            g_variant_iter_free(it);
            g_variant_unref(r);
        }, nullptr);
    g_dbus_connection_signal_subscribe(g_bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", "org.mpris.MediaPlayer2", G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer) {
            const char *name, *old_owner, *new_owner;
            g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
            if (!*new_owner) player_remove(name);
            else if (!*old_owner) player_add(name);
        }, nullptr, nullptr);
}

// ============================================================================
//  ГОЛОВНА СТОРІНКА: стан
// ============================================================================
static void set_on(GtkWidget* w, bool on) {
    if (!w) return;
    if (on) gtk_widget_add_css_class(w, "on"); else gtk_widget_remove_css_class(w, "on");
}

static void set_pp_ui(const std::string& pp) {
    static const char* ids[3] = {"power-saver", "balanced", "performance"};
    for (int i = 0; i < 3; i++) set_on(cc.pp_btn[i], pp == ids[i]);
}

static void set_theme_ui() {
    bool light = sysui::is_light_theme();
    set_on(cc.theme_icon, !light);
    gtk_image_set_from_icon_name(GTK_IMAGE(gtk_widget_get_first_child(cc.theme_icon)),
                                 light ? "weather-clear-symbolic" : "weather-clear-night-symbolic");
    gtk_label_set_text(GTK_LABEL(cc.theme_status), light ? "Світла" : "Темна");
}

static void set_vol_ui(GtkWidget* scale, GtkWidget* btn, GtkWidget* pct, double v, bool muted, bool mic) {
    gtk_range_set_value(GTK_RANGE(scale), v);
    gtk_label_set_text(GTK_LABEL(pct), (std::to_string((int)(v + 0.5)) + "%").c_str());
    if (muted) gtk_widget_add_css_class(btn, "muted"); else gtk_widget_remove_css_class(btn, "muted");
    const char* icon = mic ? (muted ? "microphone-sensitivity-muted-symbolic" : "audio-input-microphone-symbolic")
                           : (muted ? "audio-volume-muted-symbolic" : v > 66 ? "audio-volume-high-symbolic"
                              : v > 33 ? "audio-volume-medium-symbolic" : "audio-volume-low-symbolic");
    gtk_button_set_icon_name(GTK_BUTTON(btn), icon);
}

// Один процес на тік замість ~10: усе потрібне збирає один sh-скрипт.
static void poll_status() {
    static bool busy = false;
    if (busy) return;
    busy = true;
    sh_async(
        "echo \"VOL $(wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null)\";"
        "echo \"MIC $(wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null)\";"
        "echo \"DND $(dunstctl is-paused 2>/dev/null)\";"
        "echo \"PWR $(powerprofilesctl get 2>/dev/null)\";"
        "echo \"WIFI $(timeout 2 nmcli radio wifi 2>/dev/null)\";"
        "timeout 2 nmcli -t -f TYPE,STATE,CONNECTION dev 2>/dev/null | sed 's/^/DEV /';"
        "echo \"BT $(timeout 2 bluetoothctl show 2>/dev/null | grep -c 'Powered: yes')\";"
        "echo \"BTC $(timeout 2 bluetoothctl devices Connected 2>/dev/null | head -n1 | cut -d' ' -f3-)\"",
        [](const std::string& out, bool) {
            busy = false;
            std::istringstream ss(out);
            std::string line, eth, wifi_conn, wifi_state, btc;
            while (std::getline(ss, line)) {
                if (line.rfind("VOL ", 0) == 0 && line.find("Volume:") != std::string::npos) {
                    double v = parse_num(line.substr(line.find(':') + 1)) * 100;
                    cc.vol_muted = line.find("MUTED") != std::string::npos;
                    if (now_ms() - cc.touch_vol > 1200) set_vol_ui(cc.vol_scale, cc.vol_btn, cc.vol_pct, v, cc.vol_muted, false);
                } else if (line.rfind("MIC ", 0) == 0 && line.find("Volume:") != std::string::npos) {
                    double v = parse_num(line.substr(line.find(':') + 1)) * 100;
                    cc.mic_muted = line.find("MUTED") != std::string::npos;
                    if (now_ms() - cc.touch_mic > 1200) set_vol_ui(cc.mic_scale, cc.mic_btn, cc.mic_pct, v, cc.mic_muted, true);
                } else if (line.rfind("DND ", 0) == 0) {
                    cc.dnd = trim(line.substr(4)) == "true";
                } else if (line.rfind("PWR ", 0) == 0) {
                    cc.pp = trim(line.substr(4));
                } else if (line.rfind("WIFI ", 0) == 0) {
                    cc.wifi_on = trim(line.substr(5)) == "enabled";
                } else if (line.rfind("DEV ", 0) == 0) {
                    std::string d = line.substr(4);
                    auto a = d.find(':'), b = d.find(':', a + 1);
                    if (a == std::string::npos || b == std::string::npos) continue;
                    std::string type = d.substr(0, a), state = d.substr(a + 1, b - a - 1), con = d.substr(b + 1);
                    if (type == "ethernet" && state == "connected" && eth.empty()) eth = con.empty() ? "Підключено" : con;
                    if (type == "wifi") { wifi_state = state; if (state == "connected") wifi_conn = con; }
                } else if (line.rfind("BT ", 0) == 0) {
                    cc.bt_on = parse_num(line.substr(3)) > 0;
                } else if (line.rfind("BTC ", 0) == 0) {
                    btc = trim(line.substr(4));
                }
            }
            set_on(cc.wifi_circle, cc.wifi_on);
            gtk_label_set_text(GTK_LABEL(cc.wifi_status), !cc.wifi_on ? "Вимкнено" : !wifi_conn.empty() ? wifi_conn.c_str()
                               : wifi_state == "unavailable" ? "Недоступно" : "Не підключено");
            set_on(cc.bt_circle, cc.bt_on);
            gtk_label_set_text(GTK_LABEL(cc.bt_status), !cc.bt_on ? "Вимкнено" : !btc.empty() ? btc.c_str() : "Увімкнено");
            set_on(cc.eth_circle, !eth.empty());
            gtk_label_set_text(GTK_LABEL(cc.eth_status), eth.empty() ? "Не підключено" : eth.c_str());
            set_on(cc.dnd_icon, cc.dnd);
            gtk_label_set_text(GTK_LABEL(cc.dnd_status), cc.dnd ? "Увімкнено" : "Вимкнено");
            set_pp_ui(cc.pp);
            set_theme_ui();
        });
}

static void read_eco_state() {
    std::string r = sysui::hypr_request("j/getoption decoration:blur:enabled");
    try { cc.eco = json::parse(r).value("int", 1) == 0; } catch (...) {}
    set_on(cc.eco_btn, cc.eco);
}

// ============================================================================
//  ГОЛОВНА СТОРІНКА: побудова
// ============================================================================
static GtkWidget* tile_box(GtkOrientation o, int spacing) {
    GtkWidget* b = gtk_box_new(o, spacing);
    gtk_widget_add_css_class(b, "tile");
    return b;
}

static void anim_tile(GtkWidget* w) { cc.anim_tiles.push_back(w); }

static GtkWidget* conn_row(const char* icon, const char* title, GtkWidget** circle, GtkWidget** status,
                           GCallback on_circle, const char* page) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    *circle = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(*circle, "circle");
    gtk_widget_add_css_class(*circle, "live");
    gtk_widget_set_valign(*circle, GTK_ALIGN_CENTER);
    if (on_circle) g_signal_connect(*circle, "clicked", on_circle, nullptr);
    else g_signal_connect(*circle, "clicked", G_CALLBACK(+[](GtkButton*, gpointer pg) { go_page((const char*)pg); }), (gpointer)page);

    GtkWidget* txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(txt), mklabel(title, "title"));
    *status = mklabel("…", "sub");
    gtk_label_set_ellipsize(GTK_LABEL(*status), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(*status), 14);
    gtk_box_append(GTK_BOX(txt), *status);

    GtkWidget* rb = gtk_button_new();
    gtk_widget_add_css_class(rb, "row-btn");
    gtk_widget_set_hexpand(rb, TRUE);
    gtk_button_set_child(GTK_BUTTON(rb), txt);
    g_signal_connect(rb, "clicked", G_CALLBACK(+[](GtkButton*, gpointer pg) { go_page((const char*)pg); }), (gpointer)page);

    gtk_box_append(GTK_BOX(row), *circle);
    gtk_box_append(GTK_BOX(row), rb);
    return row;
}

static GtkWidget* toggle_tile(const char* icon, const char* title, GtkWidget** icon_box, GtkWidget** status, GCallback cb) {
    GtkWidget* btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "tile");
    gtk_widget_add_css_class(btn, "live-soft");
    GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    *icon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(*icon_box, "circle-icon");
    gtk_widget_set_halign(*icon_box, GTK_ALIGN_START);
    GtkWidget* img = mkicon(icon, 16);
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(img, TRUE);
    gtk_widget_set_hexpand(*icon_box, FALSE);
    gtk_box_append(GTK_BOX(*icon_box), img);
    gtk_box_append(GTK_BOX(v), *icon_box);
    GtkWidget* t = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(t), mklabel(title, "title"));
    *status = mklabel("…", "sub");
    gtk_box_append(GTK_BOX(t), *status);
    gtk_box_append(GTK_BOX(v), t);
    gtk_button_set_child(GTK_BUTTON(btn), v);
    g_signal_connect(btn, "clicked", cb, nullptr);
    return btn;
}

static GtkWidget* slider_row(GtkWidget** btn, GtkWidget** scale, GtkWidget** pct, const char* icon) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    *btn = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(*btn, "icon-btn");
    gtk_widget_add_css_class(*btn, "live");
    *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(*scale), FALSE);
    gtk_widget_set_hexpand(*scale, TRUE);
    *pct = mklabel("0%", "pct", 1.0f);
    gtk_box_append(GTK_BOX(row), *btn);
    gtk_box_append(GTK_BOX(row), *scale);
    gtk_box_append(GTK_BOX(row), *pct);
    return row;
}

static GtkWidget* square_btn(const char* icon, const char* caption, GCallback cb, GtkWidget** out = nullptr) {
    GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* b = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(b, "square");
    gtk_widget_add_css_class(b, "live");
    gtk_widget_set_halign(b, GTK_ALIGN_CENTER);
    g_signal_connect(b, "clicked", cb, nullptr);
    gtk_box_append(GTK_BOX(v), b);
    gtk_box_append(GTK_BOX(v), mklabel(caption, "caption", 0.5f));
    gtk_widget_set_hexpand(v, TRUE);
    if (out) *out = b;
    return v;
}

static void update_notif_tile() {
    if (!cc.notif_count) return;
    size_t n = g_notifs.size();
    gtk_label_set_text(GTK_LABEL(cc.notif_count), n == 0 ? "Немає сповіщень" : (std::to_string(n) + " сповіщень").c_str());
    if (n) {
        const Notif& f = g_notifs.front();
        std::string pv = f.title + (f.body.empty() ? "" : " — " + f.body);
        gtk_label_set_text(GTK_LABEL(cc.notif_preview), pv.c_str());
    } else gtk_label_set_text(GTK_LABEL(cc.notif_preview), "Усе прочитано");
    gtk_widget_set_visible(cc.notif_badge, g_unseen_sys_err > 0);
    if (g_unseen_sys_err > 0) gtk_label_set_text(GTK_LABEL(gtk_widget_get_first_child(cc.notif_badge)), std::to_string(g_unseen_sys_err).c_str());
}

static GtkWidget* build_main_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    // --- Сповіщення ---
    GtkWidget* nbtn = gtk_button_new();
    gtk_widget_add_css_class(nbtn, "tile");
    gtk_widget_add_css_class(nbtn, "live-soft");
    GtkWidget* nrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* bell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(bell, "circle-icon");
    gtk_widget_add_css_class(bell, "on");
    gtk_widget_set_valign(bell, GTK_ALIGN_CENTER);
    GtkWidget* bimg = mkicon("preferences-system-notifications-symbolic", 16);
    gtk_widget_set_hexpand(bimg, TRUE);
    gtk_widget_set_hexpand(bell, FALSE);
    gtk_box_append(GTK_BOX(bell), bimg);
    GtkWidget* ntxt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(ntxt, TRUE);
    cc.notif_count = mklabel("Сповіщення", "title");
    cc.notif_preview = mklabel("", "sub");
    gtk_label_set_ellipsize(GTK_LABEL(cc.notif_preview), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(cc.notif_preview), 34);
    gtk_box_append(GTK_BOX(ntxt), cc.notif_count);
    gtk_box_append(GTK_BOX(ntxt), cc.notif_preview);
    cc.notif_badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(cc.notif_badge, "badge");
    gtk_widget_set_valign(cc.notif_badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(cc.notif_badge), mklabel("0", nullptr, 0.5f));
    gtk_box_append(GTK_BOX(nrow), bell);
    gtk_box_append(GTK_BOX(nrow), ntxt);
    gtk_box_append(GTK_BOX(nrow), cc.notif_badge);
    gtk_box_append(GTK_BOX(nrow), mkicon("go-next-symbolic", 14));
    gtk_button_set_child(GTK_BUTTON(nbtn), nrow);
    g_signal_connect(nbtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { go_page("notifs"); }), nullptr);
    gtk_box_append(GTK_BOX(page), nbtn);
    anim_tile(nbtn);

    // --- Сітка: мережі | тема + не турбувати ---
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    GtkWidget* conn = tile_box(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(conn), conn_row("network-wireless-symbolic", "Wi-Fi", &cc.wifi_circle, &cc.wifi_status,
        G_CALLBACK(+[](GtkButton*, gpointer) {
            cc.wifi_on = !cc.wifi_on;
            set_on(cc.wifi_circle, cc.wifi_on);
            sh_async(cc.wifi_on ? "nmcli radio wifi on" : "nmcli radio wifi off", [](const std::string& o, bool ok) {
                if (!ok) sys_event("error", "Не вдалося перемкнути Wi-Fi", trim(o));
                poll_status();
            });
        }), "network"));
    gtk_box_append(GTK_BOX(conn), conn_row("bluetooth-active-symbolic", "Bluetooth", &cc.bt_circle, &cc.bt_status,
        G_CALLBACK(+[](GtkButton*, gpointer) {
            cc.bt_on = !cc.bt_on;
            set_on(cc.bt_circle, cc.bt_on);
            sh_async(cc.bt_on ? "rfkill unblock bluetooth; sleep 0.3; timeout 5 bluetoothctl power on" : "timeout 5 bluetoothctl power off",
                     [](const std::string& o, bool ok) {
                         if (!ok) sys_event("error", "Не вдалося перемкнути Bluetooth", trim(o));
                         poll_status();
                     });
        }), "bluetooth"));
    gtk_box_append(GTK_BOX(conn), conn_row("network-wired-symbolic", "Мережа", &cc.eth_circle, &cc.eth_status, nullptr, "network"));
    gtk_grid_attach(GTK_GRID(grid), conn, 0, 0, 1, 2);
    anim_tile(conn);

    cc.theme_tile = toggle_tile("weather-clear-night-symbolic", "Тема", &cc.theme_icon, &cc.theme_status,
        G_CALLBACK(+[](GtkButton*, gpointer) {
            bool to_dark = sysui::is_light_theme();
            sh_async(std::string("gsettings set org.gnome.desktop.interface color-scheme ") + (to_dark ? "prefer-dark" : "default"),
                     [](const std::string&, bool) { set_theme_ui(); apply_theme(true); });
            sysui::settings_set("dark_theme", to_dark);
        }));
    gtk_grid_attach(GTK_GRID(grid), cc.theme_tile, 1, 0, 1, 1);
    anim_tile(cc.theme_tile);

    cc.dnd_tile = toggle_tile("notifications-disabled-symbolic", "Не турбувати", &cc.dnd_icon, &cc.dnd_status,
        G_CALLBACK(+[](GtkButton*, gpointer) {
            cc.dnd = !cc.dnd;
            set_on(cc.dnd_icon, cc.dnd);
            gtk_label_set_text(GTK_LABEL(cc.dnd_status), cc.dnd ? "Увімкнено" : "Вимкнено");
            sh_async(std::string("dunstctl set-paused ") + (cc.dnd ? "true" : "false"));
        }));
    gtk_grid_attach(GTK_GRID(grid), cc.dnd_tile, 1, 1, 1, 1);
    anim_tile(cc.dnd_tile);
    gtk_box_append(GTK_BOX(page), grid);

    // --- Режим живлення ---
    GtkWidget* pp = tile_box(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(pp), mkicon("power-profile-balanced-symbolic", 16));
    GtkWidget* ppl = mklabel("Живлення", "title");
    gtk_widget_set_hexpand(ppl, TRUE);
    gtk_box_append(GTK_BOX(pp), ppl);
    GtkWidget* seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(seg, "segbox");
    static const char* pp_ids[3] = {"power-saver", "balanced", "performance"};
    static const char* pp_names[3] = {"Економ", "Баланс", "Макс"};
    for (int i = 0; i < 3; i++) {
        cc.pp_btn[i] = gtk_button_new_with_label(pp_names[i]);
        gtk_widget_add_css_class(cc.pp_btn[i], "seg");
        g_signal_connect(cc.pp_btn[i], "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
            const char* id = (const char*)d;
            cc.pp = id;
            set_pp_ui(id);
            sh_async(std::string("powerprofilesctl set ") + id, [](const std::string& o, bool ok) {
                if (!ok) sys_event("error", "Не вдалося змінити режим живлення", trim(o));
            });
        }), (gpointer)pp_ids[i]);
        gtk_box_append(GTK_BOX(seg), cc.pp_btn[i]);
    }
    gtk_box_append(GTK_BOX(pp), seg);
    gtk_box_append(GTK_BOX(page), pp);
    anim_tile(pp);

    // --- Дисплей (лише якщо монітор підтримує DDC/CI) ---
    cc.bright_tile = tile_box(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(cc.bright_tile), mklabel("Дисплей", "title"));
    GtkWidget* bbtn;
    gtk_box_append(GTK_BOX(cc.bright_tile), slider_row(&bbtn, &cc.bright_scale, &cc.bright_pct, "display-brightness-symbolic"));
    g_signal_connect(cc.bright_scale, "change-value", G_CALLBACK(+[](GtkRange*, GtkScrollType, double v, gpointer) -> gboolean {
        v = std::clamp(v, 0.0, 100.0);
        cc.touch_bright = now_ms();
        gtk_label_set_text(GTK_LABEL(cc.bright_pct), (std::to_string((int)v) + "%").c_str());
        throttle_run(&cc.th_bright, "ddcutil setvcp 10 " + std::to_string((int)v) + " --noverify 2>/dev/null");
        return FALSE;
    }), nullptr);
    gtk_widget_set_visible(cc.bright_tile, FALSE);
    gtk_box_append(GTK_BOX(page), cc.bright_tile);
    anim_tile(cc.bright_tile);

    // --- Звук ---
    GtkWidget* snd = tile_box(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* sh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* shl = mklabel("Звук", "title");
    gtk_widget_set_hexpand(shl, TRUE);
    GtkWidget* more = gtk_button_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(more, "icon-btn");
    gtk_widget_add_css_class(more, "live");
    gtk_widget_set_tooltip_text(more, "Пристрої та мікшер");
    g_signal_connect(more, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { go_page("sound"); }), nullptr);
    gtk_box_append(GTK_BOX(sh), shl);
    gtk_box_append(GTK_BOX(sh), more);
    gtk_box_append(GTK_BOX(snd), sh);
    gtk_box_append(GTK_BOX(snd), slider_row(&cc.vol_btn, &cc.vol_scale, &cc.vol_pct, "audio-volume-high-symbolic"));
    gtk_box_append(GTK_BOX(snd), slider_row(&cc.mic_btn, &cc.mic_scale, &cc.mic_pct, "audio-input-microphone-symbolic"));
    g_signal_connect(cc.vol_scale, "change-value", G_CALLBACK(+[](GtkRange*, GtkScrollType, double v, gpointer) -> gboolean {
        v = std::clamp(v, 0.0, 100.0);
        cc.touch_vol = now_ms();
        set_vol_ui(cc.vol_scale, cc.vol_btn, cc.vol_pct, v, cc.vol_muted, false);
        throttle_run(&cc.th_vol, "wpctl set-volume @DEFAULT_AUDIO_SINK@ " + fmt_dot(v / 100.0));
        return TRUE;
    }), nullptr);
    g_signal_connect(cc.mic_scale, "change-value", G_CALLBACK(+[](GtkRange*, GtkScrollType, double v, gpointer) -> gboolean {
        v = std::clamp(v, 0.0, 100.0);
        cc.touch_mic = now_ms();
        set_vol_ui(cc.mic_scale, cc.mic_btn, cc.mic_pct, v, cc.mic_muted, true);
        throttle_run(&cc.th_mic, "wpctl set-volume @DEFAULT_AUDIO_SOURCE@ " + fmt_dot(v / 100.0));
        return TRUE;
    }), nullptr);
    g_signal_connect(cc.vol_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        cc.vol_muted = !cc.vol_muted;
        set_vol_ui(cc.vol_scale, cc.vol_btn, cc.vol_pct, gtk_range_get_value(GTK_RANGE(cc.vol_scale)), cc.vol_muted, false);
        sh_async("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
    }), nullptr);
    g_signal_connect(cc.mic_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        cc.mic_muted = !cc.mic_muted;
        set_vol_ui(cc.mic_scale, cc.mic_btn, cc.mic_pct, gtk_range_get_value(GTK_RANGE(cc.mic_scale)), cc.mic_muted, true);
        sh_async("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle");
    }), nullptr);
    gtk_box_append(GTK_BOX(page), snd);
    anim_tile(snd);

    // --- Медіа ---
    cc.media_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_visible(cc.media_box, FALSE);
    gtk_box_append(GTK_BOX(page), cc.media_box);
    anim_tile(cc.media_box);

    // --- Швидкі кнопки ---
    GtkWidget* sq = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(sq), TRUE);
    gtk_box_append(GTK_BOX(sq), square_btn("applications-graphics-symbolic", "Економ ефекти", G_CALLBACK(+[](GtkButton*, gpointer) {
        cc.eco = !cc.eco;
        set_on(cc.eco_btn, cc.eco);
        if (cc.eco) {
            try { cc.eco_saved_rounding = json::parse(sysui::hypr_request("j/getoption decoration:rounding")).value("int", 10); } catch (...) {}
            sysui::hypr_request("keyword decoration:blur:enabled 0");
            sysui::hypr_request("keyword decoration:rounding 0");
            sysui::hypr_request("keyword decoration:shadow:enabled 0");
        } else {
            sysui::hypr_request("keyword decoration:blur:enabled 1");
            sysui::hypr_request("keyword decoration:rounding " + std::to_string(cc.eco_saved_rounding));
            sysui::hypr_request("keyword decoration:shadow:enabled 1");
        }
    }), &cc.eco_btn));
    gtk_box_append(GTK_BOX(sq), square_btn("camera-photo-symbolic", "Скріншот", G_CALLBACK(+[](GtkButton*, gpointer) {
        cc_close();
        g_timeout_add(350, [](gpointer) -> gboolean {
            sh_async("mkdir -p ~/Pictures/Screenshots; f=~/Pictures/Screenshots/$(date +%Y-%m-%d_%H-%M-%S).png; "
                     "g=$(slurp) || exit 2; grim -g \"$g\" \"$f\" && wl-copy < \"$f\" && echo \"$f\"",
                     [](const std::string& out, bool ok) {
                         if (ok) notif_add("Скріншот збережено", trim(out) + " (скопійовано в буфер)", "info", true);
                     });
            return G_SOURCE_REMOVE;
        }, nullptr);
    })));
    gtk_box_append(GTK_BOX(sq), square_btn("utilities-system-monitor-symbolic", "Система", G_CALLBACK(+[](GtkButton*, gpointer) { go_page("system"); })));
    gtk_box_append(GTK_BOX(sq), square_btn("system-shutdown-symbolic", "Живлення", G_CALLBACK(+[](GtkButton*, gpointer) {
        cc_close();
        sh_async("echo show | socat - UNIX-CLIENT:$HOME/.config/SYSui/powermenu.sock");
    })));
    gtk_box_append(GTK_BOX(page), sq);
    anim_tile(sq);
    return page;
}

// ============================================================================
//  ЗАГАЛЬНЕ ДЛЯ ПІДСТОРІНОК
// ============================================================================
static GtkWidget* page_header(const char* title, GtkWidget* end = nullptr) {
    GtkWidget* h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* back = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_add_css_class(back, "back");
    gtk_widget_add_css_class(back, "live");
    g_signal_connect(back, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { go_page("main"); }), nullptr);
    GtkWidget* t = mklabel(title, "h2");
    gtk_widget_set_hexpand(t, TRUE);
    gtk_box_append(GTK_BOX(h), back);
    gtk_box_append(GTK_BOX(h), t);
    if (end) gtk_box_append(GTK_BOX(h), end);
    return h;
}

static GtkWidget* page_scroll(GtkWidget* content, int max_h = 560) {
    GtkWidget* sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), max_h);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), content);
    return sw;
}

static GtkWidget* section_label(const char* t) {
    GtkWidget* l = mklabel(t, "sub");
    gtk_widget_set_margin_top(l, 6);
    gtk_widget_set_margin_start(l, 4);
    return l;
}

static GtkWidget* make_toast() {
    GtkWidget* r = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(r), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(r), 220);
    GtkWidget* l = mklabel("", "toast");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_revealer_set_child(GTK_REVEALER(r), l);
    return r;
}

static void show_toast(GtkWidget* rev, const std::string& text, const char* kind) {
    GtkWidget* l = gtk_revealer_get_child(GTK_REVEALER(rev));
    gtk_label_set_text(GTK_LABEL(l), text.c_str());
    gtk_widget_remove_css_class(l, "ok");
    gtk_widget_remove_css_class(l, "err");
    if (kind) gtk_widget_add_css_class(l, kind);
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
    guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(rev), "t"));
    if (id) g_source_remove(id);
    id = g_timeout_add(4500, [](gpointer r) -> gboolean {
        gtk_revealer_set_reveal_child(GTK_REVEALER(r), FALSE);
        g_object_set_data(G_OBJECT(r), "t", nullptr);
        return G_SOURCE_REMOVE;
    }, rev);
    g_object_set_data(G_OBJECT(rev), "t", GUINT_TO_POINTER(id));
}

// ============================================================================
//  СТОРІНКА: МЕРЕЖА
// ============================================================================
struct NetPage {
    GtkWidget *sw = nullptr, *eth = nullptr, *list = nullptr, *toast = nullptr, *refresh = nullptr;
    bool scanning = false, block_sw = false;
};
static NetPage np;

static GtkWidget* signal_icon(int s) {
    return mkicon(s > 75 ? "network-wireless-signal-excellent-symbolic" : s > 50 ? "network-wireless-signal-good-symbolic"
                : s > 25 ? "network-wireless-signal-ok-symbolic" : "network-wireless-signal-weak-symbolic", 16);
}

static void net_refresh(bool rescan);

static void wifi_connect(const std::string& ssid, const std::string& pw) {
    show_toast(np.toast, "Підключення до «" + ssid + "»…", nullptr);
    std::string cmd = "timeout 30 nmcli dev wifi connect " + q(ssid) + (pw.empty() ? "" : " password " + q(pw));
    sh_async(cmd, [ssid](const std::string& out, bool ok) {
        if (ok) { show_toast(np.toast, "Підключено до «" + ssid + "»", "ok"); notif_add("Wi-Fi", "Підключено до «" + ssid + "»", "info", true); }
        else {
            std::string why = trim(out);
            if (why.find("Secrets were required") != std::string::npos || why.find("802-11-wireless-security") != std::string::npos)
                why = "Невірний пароль";
            show_toast(np.toast, "Помилка: " + why, "err");
            sys_event("error", "Не вдалося підключитися до «" + ssid + "»", why);
        }
        net_refresh(false);
        poll_status();
    });
}

static GtkWidget* wifi_row(const std::string& ssid, int sig, bool secure, bool active, bool known) {
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "list-row");
    gtk_widget_add_css_class(btn, "live-soft");
    if (active) gtk_widget_add_css_class(btn, "active");
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(row), signal_icon(sig));
    GtkWidget* name = mklabel(ssid, "title");
    gtk_widget_set_hexpand(name, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(row), name);
    if (secure) gtk_box_append(GTK_BOX(row), mkicon("channel-secure-symbolic", 14));
    if (active) { GtkWidget* c = mkicon("object-select-symbolic", 16); gtk_widget_add_css_class(c, "check"); gtk_box_append(GTK_BOX(row), c); }
    gtk_button_set_child(GTK_BUTTON(btn), row);
    gtk_box_append(GTK_BOX(outer), btn);

    // Поле пароля (з'являється для захищених невідомих мереж)
    GtkWidget* rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    GtkWidget* pwrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(pwrow, 8);
    gtk_widget_set_margin_end(pwrow, 8);
    GtkWidget* entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
    gtk_widget_add_css_class(entry, "pw");
    gtk_widget_set_hexpand(entry, TRUE);
    GtkWidget* go = gtk_button_new_with_label("Підключити");
    gtk_widget_add_css_class(go, "pill");
    gtk_widget_add_css_class(go, "live");
    gtk_box_append(GTK_BOX(pwrow), entry);
    gtk_box_append(GTK_BOX(pwrow), go);
    gtk_revealer_set_child(GTK_REVEALER(rev), pwrow);
    gtk_box_append(GTK_BOX(outer), rev);

    g_object_set_data_full(G_OBJECT(btn), "ssid", g_strdup(ssid.c_str()), g_free);
    g_object_set_data(G_OBJECT(btn), "rev", rev);
    g_object_set_data(G_OBJECT(btn), "entry", entry);
    g_object_set_data(G_OBJECT(btn), "flags", GINT_TO_POINTER((secure ? 1 : 0) | (active ? 2 : 0) | (known ? 4 : 0)));
    g_object_set_data_full(G_OBJECT(go), "ssid", g_strdup(ssid.c_str()), g_free);
    g_object_set_data(G_OBJECT(go), "entry", entry);

    g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
        std::string ssid = (const char*)g_object_get_data(G_OBJECT(b), "ssid");
        int f = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flags"));
        if (f & 2) {
            show_toast(np.toast, "Відключення від «" + ssid + "»…", nullptr);
            sh_async("timeout 15 nmcli con down id " + q(ssid), [](const std::string&, bool) { net_refresh(false); poll_status(); });
        } else if (f & 4) {
            show_toast(np.toast, "Підключення до «" + ssid + "»…", nullptr);
            sh_async("timeout 30 nmcli con up id " + q(ssid), [ssid](const std::string& o, bool ok) {
                if (ok) show_toast(np.toast, "Підключено до «" + ssid + "»", "ok");
                else { show_toast(np.toast, "Помилка: " + trim(o), "err"); sys_event("error", "Не вдалося підключитися до «" + ssid + "»", trim(o)); }
                net_refresh(false); poll_status();
            });
        } else if (f & 1) {
            GtkWidget* rev = (GtkWidget*)g_object_get_data(G_OBJECT(b), "rev");
            bool show = !gtk_revealer_get_reveal_child(GTK_REVEALER(rev));
            gtk_revealer_set_reveal_child(GTK_REVEALER(rev), show);
            if (show) gtk_widget_grab_focus((GtkWidget*)g_object_get_data(G_OBJECT(b), "entry"));
        } else {
            wifi_connect(ssid, "");
        }
    }), nullptr);
    auto submit = +[](GtkWidget* w, gpointer) {
        std::string ssid = (const char*)g_object_get_data(G_OBJECT(w), "ssid");
        GtkWidget* e = (GtkWidget*)g_object_get_data(G_OBJECT(w), "entry");
        std::string pw = gtk_editable_get_text(GTK_EDITABLE(e));
        if (pw.size() < 8) { show_toast(np.toast, "Пароль Wi-Fi має бути щонайменше 8 символів", "err"); return; }
        wifi_connect(ssid, pw);
    };
    g_signal_connect(go, "clicked", G_CALLBACK(submit), nullptr);
    g_object_set_data_full(G_OBJECT(entry), "ssid", g_strdup(ssid.c_str()), g_free);
    g_object_set_data(G_OBJECT(entry), "entry", entry);
    g_signal_connect(entry, "activate", G_CALLBACK(submit), nullptr);
    return outer;
}

// nmcli -t екранує ':' як '\:'
static std::vector<std::string> split_nmcli(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '\\' && i + 1 < line.size()) { cur += line[++i]; continue; }
        if (line[i] == ':') { f.push_back(cur); cur.clear(); continue; }
        cur += line[i];
    }
    f.push_back(cur);
    return f;
}

static void net_refresh(bool rescan) {
    if (np.scanning) return;
    np.scanning = true;
    gtk_widget_add_css_class(np.refresh, "spinning");
    sh_async(std::string("echo \"RADIO $(nmcli radio wifi)\"; timeout 3 nmcli -t -f TYPE,STATE,CONNECTION,DEVICE dev | sed 's/^/DEV:/';"
                         "timeout 3 nmcli -t -f NAME,TYPE con show | sed 's/^/CON:/';"
                         "timeout 12 nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list --rescan ") + (rescan ? "yes" : "auto") + " | sed 's/^/AP:/'",
        [](const std::string& out, bool) {
            np.scanning = false;
            gtk_widget_remove_css_class(np.refresh, "spinning");
            clear_box(np.list);
            clear_box(np.eth);
            std::istringstream ss(out);
            std::string line;
            bool radio = false;
            std::set<std::string> known, seen;
            struct AP { std::string ssid; int sig; bool sec, act; };
            std::vector<AP> aps;
            while (std::getline(ss, line)) {
                if (line.rfind("RADIO ", 0) == 0) radio = trim(line.substr(6)) == "enabled";
                else if (line.rfind("DEV:", 0) == 0) {
                    auto f = split_nmcli(line.substr(4));
                    if (f.size() >= 4 && f[0] == "ethernet") {
                        GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
                        gtk_widget_add_css_class(r, "list-row");
                        gtk_box_append(GTK_BOX(r), mkicon("network-wired-symbolic", 16));
                        GtkWidget* t = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                        gtk_widget_set_hexpand(t, TRUE);
                        gtk_box_append(GTK_BOX(t), mklabel(f[2].empty() ? f[3] : f[2], "title"));
                        gtk_box_append(GTK_BOX(t), mklabel(f[1] == "connected" ? "Підключено · " + f[3] : "Не підключено · " + f[3], "sub"));
                        gtk_box_append(GTK_BOX(r), t);
                        if (f[1] == "connected") { GtkWidget* c = mkicon("object-select-symbolic", 16); gtk_widget_add_css_class(c, "check"); gtk_box_append(GTK_BOX(r), c); }
                        gtk_box_append(GTK_BOX(np.eth), r);
                    }
                } else if (line.rfind("CON:", 0) == 0) {
                    auto f = split_nmcli(line.substr(4));
                    if (f.size() >= 2 && f[1].find("wireless") != std::string::npos) known.insert(f[0]);
                } else if (line.rfind("AP:", 0) == 0) {
                    auto f = split_nmcli(line.substr(3));
                    if (f.size() < 4 || f[1].empty()) continue;
                    if (seen.count(f[1])) continue;
                    seen.insert(f[1]);
                    aps.push_back({f[1], atoi(f[2].c_str()), !f[3].empty() && f[3] != "--", f[0] == "*"});
                }
            }
            np.block_sw = true;
            gtk_switch_set_active(GTK_SWITCH(np.sw), radio);
            np.block_sw = false;
            gtk_widget_set_visible(np.eth, gtk_widget_get_first_child(np.eth) != nullptr);
            if (!radio) { gtk_box_append(GTK_BOX(np.list), mklabel("Wi-Fi вимкнено", "sub", 0.5f)); return; }
            std::stable_sort(aps.begin(), aps.end(), [](const AP& a, const AP& b) { return a.act != b.act ? a.act : a.sig > b.sig; });
            if (aps.empty()) gtk_box_append(GTK_BOX(np.list), mklabel("Мереж не знайдено", "sub", 0.5f));
            for (auto& a : aps) gtk_box_append(GTK_BOX(np.list), wifi_row(a.ssid, a.sig, a.sec, a.act, known.count(a.ssid) > 0));
        });
}

static GtkWidget* build_net_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    np.sw = gtk_switch_new();
    gtk_widget_set_valign(np.sw, GTK_ALIGN_CENTER);
    g_signal_connect(np.sw, "notify::active", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        if (np.block_sw) return;
        bool on = gtk_switch_get_active(GTK_SWITCH(o));
        sh_async(on ? "nmcli radio wifi on" : "nmcli radio wifi off", [](const std::string&, bool) {
            g_timeout_add(1500, [](gpointer) -> gboolean { net_refresh(true); return G_SOURCE_REMOVE; }, nullptr);
            poll_status();
        });
    }), nullptr);
    np.refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_add_css_class(np.refresh, "icon-btn");
    gtk_widget_add_css_class(np.refresh, "live");
    g_signal_connect(np.refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { net_refresh(true); }), nullptr);
    GtkWidget* end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(end), np.refresh);
    gtk_box_append(GTK_BOX(end), np.sw);
    gtk_box_append(GTK_BOX(page), page_header("Мережа", end));
    np.toast = make_toast();
    gtk_box_append(GTK_BOX(page), np.toast);

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    np.eth = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(np.eth, "tile");
    gtk_box_append(GTK_BOX(content), np.eth);
    gtk_box_append(GTK_BOX(content), section_label("Wi-Fi мережі"));
    np.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(np.list, "tile");
    gtk_box_append(GTK_BOX(content), np.list);
    gtk_box_append(GTK_BOX(page), page_scroll(content));
    return page;
}

// ============================================================================
//  СТОРІНКА: BLUETOOTH
// ============================================================================
struct BtPage { GtkWidget *sw = nullptr, *list = nullptr, *toast = nullptr, *scan = nullptr; bool block_sw = false, busy = false; };
static BtPage bp;

static void bt_refresh();

static void bt_refresh() {
    if (bp.busy) return;
    bp.busy = true;
    sh_async("echo \"POWER $(timeout 3 bluetoothctl show | grep -c 'Powered: yes')\";"
             "timeout 3 bluetoothctl devices Connected | sed 's/^/CON:/';"
             "timeout 3 bluetoothctl devices Paired | sed 's/^/PAIR:/';"
             "timeout 3 bluetoothctl devices | sed 's/^/ALL:/'",
        [](const std::string& out, bool) {
            bp.busy = false;
            clear_box(bp.list);
            std::istringstream ss(out);
            std::string line;
            bool power = false;
            std::set<std::string> con, pair;
            std::vector<std::pair<std::string, std::string>> all;
            while (std::getline(ss, line)) {
                if (line.rfind("POWER ", 0) == 0) { power = parse_num(line.substr(6)) > 0; continue; }
                std::string tag = line.substr(0, line.find(':'));
                std::string rest = line.substr(line.find(':') + 1);
                if (rest.rfind("Device ", 0) != 0) continue;
                std::string mac = rest.substr(7, 17);
                std::string name = rest.size() > 25 ? rest.substr(25) : mac;
                if (tag == "CON") con.insert(mac);
                else if (tag == "PAIR") pair.insert(mac);
                else if (tag == "ALL") all.push_back({mac, name});
            }
            bp.block_sw = true;
            gtk_switch_set_active(GTK_SWITCH(bp.sw), power);
            bp.block_sw = false;
            if (!power) { gtk_box_append(GTK_BOX(bp.list), mklabel("Bluetooth вимкнено", "sub", 0.5f)); return; }
            std::stable_sort(all.begin(), all.end(), [&](auto& a, auto& b) {
                int ra = con.count(a.first) ? 0 : pair.count(a.first) ? 1 : 2;
                int rb = con.count(b.first) ? 0 : pair.count(b.first) ? 1 : 2;
                return ra < rb;
            });
            if (all.empty()) gtk_box_append(GTK_BOX(bp.list), mklabel("Пристроїв немає — натисніть пошук", "sub", 0.5f));
            for (auto& [mac, name] : all) {
                bool c = con.count(mac), p = pair.count(mac);
                GtkWidget* btn = gtk_button_new();
                gtk_widget_add_css_class(btn, "list-row");
                gtk_widget_add_css_class(btn, "live-soft");
                if (c) gtk_widget_add_css_class(btn, "active");
                GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
                gtk_box_append(GTK_BOX(r), mkicon(c ? "bluetooth-active-symbolic" : "bluetooth-symbolic", 16));
                GtkWidget* t = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                gtk_widget_set_hexpand(t, TRUE);
                GtkWidget* nl = mklabel(name, "title");
                gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
                gtk_box_append(GTK_BOX(t), nl);
                gtk_box_append(GTK_BOX(t), mklabel(c ? "Підключено" : p ? "Сполучено" : "Доступний", "sub"));
                gtk_box_append(GTK_BOX(r), t);
                gtk_button_set_child(GTK_BUTTON(btn), r);
                g_object_set_data_full(G_OBJECT(btn), "mac", g_strdup(mac.c_str()), g_free);
                g_object_set_data_full(G_OBJECT(btn), "name", g_strdup(name.c_str()), g_free);
                g_object_set_data(G_OBJECT(btn), "c", GINT_TO_POINTER(c ? 1 : 0));
                g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
                    std::string mac = (const char*)g_object_get_data(G_OBJECT(b), "mac");
                    std::string name = (const char*)g_object_get_data(G_OBJECT(b), "name");
                    bool c = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "c"));
                    show_toast(bp.toast, (c ? "Відключення: " : "Підключення: ") + name + "…", nullptr);
                    sh_async(c ? "timeout 10 bluetoothctl disconnect " + mac
                               : "timeout 20 bluetoothctl pair " + mac + " >/dev/null 2>&1; timeout 5 bluetoothctl trust " + mac +
                                 " >/dev/null; timeout 20 bluetoothctl connect " + mac + " | tail -n1; bluetoothctl info " + mac + " | grep -q 'Connected: yes'",
                        [name, c](const std::string& o, bool ok) {
                            if (ok) show_toast(bp.toast, c ? "Відключено: " + name : "Підключено: " + name, "ok");
                            else {
                                show_toast(bp.toast, "Помилка: " + name, "err");
                                sys_event("error", "Bluetooth: не вдалося " + std::string(c ? "відключити " : "підключити ") + name, trim(o));
                            }
                            bt_refresh();
                            poll_status();
                        });
                }), nullptr);
                gtk_box_append(GTK_BOX(bp.list), btn);
            }
        });
}

static GtkWidget* build_bt_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    bp.sw = gtk_switch_new();
    gtk_widget_set_valign(bp.sw, GTK_ALIGN_CENTER);
    g_signal_connect(bp.sw, "notify::active", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        if (bp.block_sw) return;
        bool on = gtk_switch_get_active(GTK_SWITCH(o));
        sh_async(on ? "rfkill unblock bluetooth; sleep 0.3; timeout 5 bluetoothctl power on" : "timeout 5 bluetoothctl power off",
                 [](const std::string&, bool) { bt_refresh(); poll_status(); });
    }), nullptr);
    bp.scan = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_add_css_class(bp.scan, "icon-btn");
    gtk_widget_add_css_class(bp.scan, "live");
    gtk_widget_set_tooltip_text(bp.scan, "Пошук пристроїв (10 с)");
    g_signal_connect(bp.scan, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
        if (gtk_widget_has_css_class(GTK_WIDGET(b), "spinning")) return;
        gtk_widget_add_css_class(GTK_WIDGET(b), "spinning");
        show_toast(bp.toast, "Пошук пристроїв…", nullptr);
        sh_async("timeout 11 bluetoothctl --timeout 10 scan on >/dev/null 2>&1", [](const std::string&, bool) {
            gtk_widget_remove_css_class(bp.scan, "spinning");
            bt_refresh();
        });
    }), nullptr);
    GtkWidget* end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(end), bp.scan);
    gtk_box_append(GTK_BOX(end), bp.sw);
    gtk_box_append(GTK_BOX(page), page_header("Bluetooth", end));
    bp.toast = make_toast();
    gtk_box_append(GTK_BOX(page), bp.toast);
    bp.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(bp.list, "tile");
    gtk_box_append(GTK_BOX(page), page_scroll(bp.list));
    return page;
}

// ============================================================================
//  СТОРІНКА: ЗВУК
// ============================================================================
struct MixRow { GtkWidget *root, *scale, *pct, *mute; };
struct SoundPage {
    GtkWidget *outs = nullptr, *ins = nullptr, *mixer = nullptr;
    std::map<int, MixRow> rows;
    std::map<int, gint64> touch;
    std::map<int, Throttle*> th;
    bool busy = false;
    std::string last_sig;
};
static SoundPage sp;

struct PaDev { std::string name, desc; bool monitor; };
struct PaStream { int id; std::string app, icon; int vol; bool mute; };

static void sound_refresh() {
    if (sp.busy) return;
    sp.busy = true;
    sh_async("echo \"DEFSINK $(pactl get-default-sink)\"; echo \"DEFSRC $(pactl get-default-source)\";"
             "pactl list sinks | grep -E '^(Sink #|\\s+(Name|Description):)' | sed 's/^/S /';"
             "pactl list sources | grep -E '^(Source #|\\s+(Name|Description):)' | sed 's/^/I /';"
             "pactl list sink-inputs | grep -E '^Sink Input #|^\\s+(Mute:|Volume:)|application\\.(name|icon_name) =' | sed 's/^/M /'",
        [](const std::string& out, bool) {
            sp.busy = false;
            std::istringstream ss(out);
            std::string line, defsink, defsrc;
            std::vector<PaDev> sinks, srcs;
            std::vector<PaStream> streams;
            auto val = [](const std::string& l) { auto p = l.find(':'); return trim(l.substr(p + 1)); };
            auto quoted = [](const std::string& l) { auto a = l.find('"'), b = l.rfind('"'); return (a != std::string::npos && b > a) ? l.substr(a + 1, b - a - 1) : std::string(); };
            while (std::getline(ss, line)) {
                if (line.rfind("DEFSINK ", 0) == 0) { defsink = trim(line.substr(8)); continue; }
                if (line.rfind("DEFSRC ", 0) == 0) { defsrc = trim(line.substr(7)); continue; }
                if (line.size() < 2) continue;
                char k = line[0];
                std::string l = line.substr(2);
                if (k == 'S' || k == 'I') {
                    auto& v = (k == 'S') ? sinks : srcs;
                    if (l.rfind("Sink #", 0) == 0 || l.rfind("Source #", 0) == 0) v.push_back({});
                    else if (!v.empty() && l.find("Name:") != std::string::npos) { v.back().name = val(l); v.back().monitor = v.back().name.find(".monitor") != std::string::npos; }
                    else if (!v.empty() && l.find("Description:") != std::string::npos) v.back().desc = val(l);
                } else if (k == 'M') {
                    if (l.rfind("Sink Input #", 0) == 0) streams.push_back({atoi(l.c_str() + 12), "", "", 0, false});
                    else if (streams.empty()) continue;
                    else if (l.find("Mute:") != std::string::npos) streams.back().mute = l.find("yes") != std::string::npos;
                    else if (l.find("Volume:") != std::string::npos) { auto p = l.find('%'); auto s = l.rfind('/', p); if (p != std::string::npos && s != std::string::npos) streams.back().vol = atoi(l.c_str() + s + 1); }
                    else if (l.find("application.name") != std::string::npos) streams.back().app = quoted(l);
                    else if (l.find("application.icon_name") != std::string::npos) streams.back().icon = quoted(l);
                }
            }

            // Пристрої перебудовуємо лише коли список змінився
            std::string sig = defsink + "|" + defsrc;
            for (auto& d : sinks) sig += d.name + ";";
            for (auto& d : srcs) sig += d.name + ";";
            if (sig != sp.last_sig) {
                sp.last_sig = sig;
                auto fill = [](GtkWidget* box, std::vector<PaDev>& v, const std::string& def, bool sink) {
                    clear_box(box);
                    for (auto& d : v) {
                        if (d.monitor) continue;
                        GtkWidget* b = gtk_button_new();
                        gtk_widget_add_css_class(b, "list-row");
                        gtk_widget_add_css_class(b, "live-soft");
                        bool act = d.name == def;
                        if (act) gtk_widget_add_css_class(b, "active");
                        GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
                        bool hp = d.name.find("usb") != std::string::npos || d.desc.find("Pods") != std::string::npos || d.desc.find("Headphone") != std::string::npos;
                        gtk_box_append(GTK_BOX(r), mkicon(sink ? (hp ? "audio-headphones-symbolic" : d.name.find("hdmi") != std::string::npos ? "video-display-symbolic" : "audio-speakers-symbolic")
                                                               : "audio-input-microphone-symbolic", 16));
                        GtkWidget* l = mklabel(d.desc.empty() ? d.name : d.desc, "title");
                        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
                        gtk_widget_set_hexpand(l, TRUE);
                        gtk_box_append(GTK_BOX(r), l);
                        if (act) { GtkWidget* c = mkicon("object-select-symbolic", 16); gtk_widget_add_css_class(c, "check"); gtk_box_append(GTK_BOX(r), c); }
                        gtk_button_set_child(GTK_BUTTON(b), r);
                        g_object_set_data_full(G_OBJECT(b), "n", g_strdup(d.name.c_str()), g_free);
                        g_signal_connect(b, "clicked", sink ? G_CALLBACK(+[](GtkButton* b, gpointer) {
                            sh_async(std::string("pactl set-default-sink ") + q((const char*)g_object_get_data(G_OBJECT(b), "n")), [](const std::string&, bool) { sp.last_sig.clear(); sound_refresh(); });
                        }) : G_CALLBACK(+[](GtkButton* b, gpointer) {
                            sh_async(std::string("pactl set-default-source ") + q((const char*)g_object_get_data(G_OBJECT(b), "n")), [](const std::string&, bool) { sp.last_sig.clear(); sound_refresh(); });
                        }), nullptr);
                        gtk_box_append(GTK_BOX(box), b);
                    }
                };
                fill(sp.outs, sinks, defsink, true);
                fill(sp.ins, srcs, defsrc, false);
            }

            // Мікшер: оновлюємо рядки на місці
            std::set<int> alive;
            for (auto& s : streams) {
                if (s.app.empty() || s.app == "speech-dispatcher-dummy") continue;
                alive.insert(s.id);
                auto it = sp.rows.find(s.id);
                if (it == sp.rows.end()) {
                    MixRow m;
                    m.root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                    gtk_widget_add_css_class(m.root, "list-row");
                    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                    gtk_box_append(GTK_BOX(top), mkicon(s.icon.empty() ? "application-x-executable" : s.icon.c_str(), 20));
                    GtkWidget* n = mklabel(s.app, "title");
                    gtk_widget_set_hexpand(n, TRUE);
                    gtk_box_append(GTK_BOX(top), n);
                    m.pct = mklabel("", "pct", 1.0f);
                    gtk_box_append(GTK_BOX(top), m.pct);
                    GtkWidget* bot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                    m.mute = gtk_button_new_from_icon_name("audio-volume-high-symbolic");
                    gtk_widget_add_css_class(m.mute, "icon-btn");
                    gtk_widget_add_css_class(m.mute, "live");
                    m.scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 150, 1);
                    gtk_scale_set_draw_value(GTK_SCALE(m.scale), FALSE);
                    gtk_widget_set_hexpand(m.scale, TRUE);
                    gtk_box_append(GTK_BOX(bot), m.mute);
                    gtk_box_append(GTK_BOX(bot), m.scale);
                    gtk_box_append(GTK_BOX(m.root), top);
                    gtk_box_append(GTK_BOX(m.root), bot);
                    int id = s.id;
                    g_signal_connect(m.scale, "change-value", G_CALLBACK(+[](GtkRange*, GtkScrollType, double v, gpointer d) -> gboolean {
                        int id = GPOINTER_TO_INT(d);
                        v = std::clamp(v, 0.0, 150.0);
                        sp.touch[id] = now_ms();
                        if (!sp.th[id]) sp.th[id] = new Throttle{60};
                        gtk_label_set_text(GTK_LABEL(sp.rows[id].pct), (std::to_string((int)v) + "%").c_str());
                        throttle_run(sp.th[id], "pactl set-sink-input-volume " + std::to_string(id) + " " + std::to_string((int)v) + "%");
                        return FALSE;
                    }), GINT_TO_POINTER(id));
                    g_signal_connect(m.mute, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
                        sh_async("pactl set-sink-input-mute " + std::to_string(GPOINTER_TO_INT(d)) + " toggle", [](const std::string&, bool) { sound_refresh(); });
                    }), GINT_TO_POINTER(id));
                    gtk_box_append(GTK_BOX(sp.mixer), m.root);
                    it = sp.rows.emplace(s.id, m).first;
                }
                MixRow& m = it->second;
                if (now_ms() - sp.touch[s.id] > 1200) {
                    gtk_range_set_value(GTK_RANGE(m.scale), s.vol);
                    gtk_label_set_text(GTK_LABEL(m.pct), (std::to_string(s.vol) + "%").c_str());
                }
                gtk_button_set_icon_name(GTK_BUTTON(m.mute), s.mute ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
                if (s.mute) gtk_widget_add_css_class(m.mute, "muted"); else gtk_widget_remove_css_class(m.mute, "muted");
            }
            for (auto it = sp.rows.begin(); it != sp.rows.end();) {
                if (!alive.count(it->first)) { gtk_widget_unparent(it->second.root); it = sp.rows.erase(it); }
                else ++it;
            }
            GtkWidget* empty = (GtkWidget*)g_object_get_data(G_OBJECT(sp.mixer), "empty");
            if (empty) gtk_widget_set_visible(empty, sp.rows.empty());
        });
}

static GtkWidget* build_sound_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(page), page_header("Звук"));
    GtkWidget* c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(c), section_label("Вихід"));
    sp.outs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(sp.outs, "tile");
    gtk_box_append(GTK_BOX(c), sp.outs);
    gtk_box_append(GTK_BOX(c), section_label("Вхід"));
    sp.ins = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(sp.ins, "tile");
    gtk_box_append(GTK_BOX(c), sp.ins);
    gtk_box_append(GTK_BOX(c), section_label("Програми"));
    sp.mixer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(sp.mixer, "tile");
    GtkWidget* empty = mklabel("Зараз нічого не відтворює звук", "sub", 0.5f);
    g_object_set_data(G_OBJECT(sp.mixer), "empty", empty);
    gtk_box_append(GTK_BOX(sp.mixer), empty);
    gtk_box_append(GTK_BOX(c), sp.mixer);
    gtk_box_append(GTK_BOX(page), page_scroll(c));
    return page;
}

// ============================================================================
//  СТОРІНКА: СИСТЕМА
// ============================================================================
struct Meter { GtkWidget *val, *bar, *sub; };
struct SysPage {
    Meter cpu, gpu, ram;
    GtkWidget *disks = nullptr, *uptime = nullptr, *mods = nullptr;
    long last_idle = 0, last_total = 0;
    bool gpu_busy = false;
    std::string temp_path;
};
static SysPage syp;

static GtkWidget* meter_row(const char* icon, const char* title, Meter& m) {
    GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(v, "list-row");
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(top), mkicon(icon, 18));
    GtkWidget* t = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(t, TRUE);
    gtk_box_append(GTK_BOX(t), mklabel(title, "title"));
    m.sub = mklabel("…", "sub");
    gtk_label_set_ellipsize(GTK_LABEL(m.sub), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(t), m.sub);
    gtk_box_append(GTK_BOX(top), t);
    m.val = mklabel("", "pct", 1.0f);
    gtk_box_append(GTK_BOX(top), m.val);
    m.bar = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(v), top);
    gtk_box_append(GTK_BOX(v), m.bar);
    return v;
}

static void set_meter(Meter& m, double pct, const std::string& val) {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m.bar), std::clamp(pct / 100.0, 0.0, 1.0));
    if (pct > 85) gtk_widget_add_css_class(m.bar, "hot"); else gtk_widget_remove_css_class(m.bar, "hot");
    gtk_label_set_text(GTK_LABEL(m.val), val.c_str());
}

static void sys_refresh() {
    // CPU — напряму з /proc/stat
    {
        std::ifstream f("/proc/stat");
        std::string tag; long t[8] = {0};
        f >> tag; for (long& x : t) f >> x;
        long idle = t[3] + t[4], total = 0;
        for (long x : t) total += x;
        double pct = (total - syp.last_total) > 0 ? 100.0 * (1.0 - (double)(idle - syp.last_idle) / (total - syp.last_total)) : 0;
        syp.last_idle = idle; syp.last_total = total;
        int temp = syp.temp_path.empty() ? 0 : atoi(read_file(syp.temp_path).c_str()) / 1000;
        set_meter(syp.cpu, pct, std::to_string((int)pct) + "%" + (temp ? " · " + std::to_string(temp) + "°" : ""));
    }
    // RAM
    {
        std::ifstream f("/proc/meminfo");
        std::string k; long v; std::string unit; long tot = 0, avail = 0;
        while (f >> k >> v >> unit) { if (k == "MemTotal:") tot = v; else if (k == "MemAvailable:") { avail = v; break; } }
        if (tot) {
            double used = (tot - avail) / 1048576.0, total = tot / 1048576.0;
            char b[64]; snprintf(b, sizeof(b), "%.1f / %.1f ГБ", used, total);
            for (char* c = b; *c; c++) if (*c == ',') *c = '.';
            gtk_label_set_text(GTK_LABEL(syp.ram.sub), b);
            set_meter(syp.ram, 100.0 * (tot - avail) / tot, std::to_string((int)(100.0 * (tot - avail) / tot)) + "%");
        }
    }
    // Аптайм
    {
        long up = atol(read_file("/proc/uptime").c_str());
        std::string s = "Аптайм: ";
        if (up >= 86400) s += std::to_string(up / 86400) + " дн ";
        s += std::to_string((up % 86400) / 3600) + " год " + std::to_string((up % 3600) / 60) + " хв";
        gtk_label_set_text(GTK_LABEL(syp.uptime), s.c_str());
    }
    // GPU — nvidia-smi асинхронно
    if (!syp.gpu_busy) {
        syp.gpu_busy = true;
        sh_async("nvidia-smi --query-gpu=name,utilization.gpu,temperature.gpu,memory.used,memory.total --format=csv,noheader,nounits",
            [](const std::string& out, bool ok) {
                syp.gpu_busy = false;
                if (!ok) { gtk_label_set_text(GTK_LABEL(syp.gpu.sub), "Немає даних"); return; }
                std::vector<std::string> f;
                std::stringstream ss(trim(out));
                std::string x;
                while (std::getline(ss, x, ',')) f.push_back(trim(x));
                if (f.size() < 5) return;
                std::string name = f[0];
                if (name.rfind("NVIDIA GeForce ", 0) == 0) name = name.substr(15);
                gtk_label_set_text(GTK_LABEL(syp.gpu.sub), (name + " · VRAM " + std::to_string(atoi(f[3].c_str()) / 1024) + "." +
                                   std::to_string(atoi(f[3].c_str()) % 1024 * 10 / 1024) + "/" + std::to_string(atoi(f[4].c_str()) / 1024) + " ГБ").c_str());
                set_meter(syp.gpu, atof(f[1].c_str()), f[1] + "% · " + f[2] + "°");
            });
    }
}

static void disks_refresh() {
    clear_box(syp.disks);
    std::ifstream f("/proc/mounts");
    std::string dev, mnt, type, rest;
    std::set<std::string> seen;
    while (f >> dev >> mnt >> type && std::getline(f, rest)) {
        if (dev.rfind("/dev/", 0) != 0 || dev.find("loop") != std::string::npos) continue;
        if (mnt.rfind("/boot", 0) == 0 || mnt.rfind("/snap", 0) == 0 || mnt.find("/docker") != std::string::npos) continue;
        if (seen.count(dev)) continue;
        seen.insert(dev);
        // \040 у /proc/mounts = пробіл
        size_t p;
        while ((p = mnt.find("\\040")) != std::string::npos) mnt.replace(p, 4, " ");
        struct statvfs s;
        if (statvfs(mnt.c_str(), &s) != 0 || s.f_blocks == 0) continue;
        double tot = (double)s.f_blocks * s.f_frsize / 1e9, fr = (double)s.f_bavail * s.f_frsize / 1e9;
        double used = tot - fr;
        Meter m;
        GtkWidget* row = meter_row(mnt == "/" ? "drive-harddisk-system-symbolic" : "drive-harddisk-symbolic", mnt.c_str(), m);
        char b[64]; snprintf(b, sizeof(b), "%.0f / %.0f ГБ · вільно %.0f ГБ", used, tot, fr);
        gtk_label_set_text(GTK_LABEL(m.sub), b);
        set_meter(m, 100.0 * used / tot, std::to_string((int)(100.0 * used / tot)) + "%");
        gtk_box_append(GTK_BOX(syp.disks), row);
    }
}

static GtkWidget* build_sys_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(page), page_header("Система"));
    GtkWidget* c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget* hw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(hw, "tile");
    gtk_box_append(GTK_BOX(hw), meter_row("cpu-symbolic", "Процесор", syp.cpu));
    gtk_box_append(GTK_BOX(hw), meter_row("video-display-symbolic", "Відеокарта", syp.gpu));
    gtk_box_append(GTK_BOX(hw), meter_row("memory-symbolic", "Пам'ять", syp.ram));
    syp.uptime = mklabel("", "sub");
    gtk_widget_set_margin_start(syp.uptime, 10);
    gtk_widget_set_margin_bottom(syp.uptime, 4);
    gtk_box_append(GTK_BOX(hw), syp.uptime);
    gtk_box_append(GTK_BOX(c), hw);

    // Назва CPU і сенсор температури — один раз
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("model name", 0) == 0) {
                std::string n = trim(line.substr(line.find(':') + 1));
                for (const char* cut : {" Processor", " 6-Core", " 8-Core", " 12-Core", " 16-Core"}) {
                    auto p = n.find(cut); if (p != std::string::npos) n.erase(p, strlen(cut));
                }
                gtk_label_set_text(GTK_LABEL(syp.cpu.sub), n.c_str());
                break;
            }
        for (int i = 0; i < 16; i++) {
            std::string base = "/sys/class/hwmon/hwmon" + std::to_string(i);
            std::string name = trim(read_file(base + "/name"));
            if (name == "k10temp" || name == "coretemp" || name == "zenpower") { syp.temp_path = base + "/temp1_input"; break; }
        }
    }

    gtk_box_append(GTK_BOX(c), section_label("Накопичувачі"));
    syp.disks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(syp.disks, "tile");
    gtk_box_append(GTK_BOX(c), syp.disks);

    gtk_box_append(GTK_BOX(c), section_label("Модулі SYSui"));
    syp.mods = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(syp.mods, "tile");
    for (auto& m : g_modules) {
        GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(r, "list-row");
        m.dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(m.dot, "dot");
        gtk_widget_set_valign(m.dot, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(r), m.dot);
        GtkWidget* t = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(t, TRUE);
        gtk_box_append(GTK_BOX(t), mklabel(m.name, "title"));
        m.status = mklabel("", "sub");
        gtk_box_append(GTK_BOX(t), m.status);
        gtk_box_append(GTK_BOX(r), t);
        GtkWidget* rb = gtk_button_new_from_icon_name("view-refresh-symbolic");
        gtk_widget_add_css_class(rb, "icon-btn");
        gtk_widget_add_css_class(rb, "live");
        gtk_widget_set_tooltip_text(rb, "Перезапустити");
        g_signal_connect(rb, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
            Module& m = *static_cast<Module*>(d);
            if (m.pid) kill(m.pid, SIGTERM);
            Module* mp = &m;
            g_timeout_add(500, [](gpointer d) -> gboolean {
                Module& m = *static_cast<Module*>(d);
                if (m.pid && kill(m.pid, 0) == 0) kill(m.pid, SIGKILL);
                m.pid = 0;
                m.seen = true;
                module_restart(m, true);
                notif_add("Модуль «" + std::string(m.name) + "» перезапущено вручну", "", "info", true);
                return G_SOURCE_REMOVE;
            }, mp);
        }), &m);
        gtk_box_append(GTK_BOX(r), rb);
        gtk_box_append(GTK_BOX(syp.mods), r);
    }
    gtk_box_append(GTK_BOX(c), syp.mods);
    gtk_box_append(GTK_BOX(page), page_scroll(c));
    return page;
}

// ============================================================================
//  СТОРІНКА: СПОВІЩЕННЯ
// ============================================================================
struct NotifPage { GtkWidget *list = nullptr, *empty = nullptr, *seg_all = nullptr, *seg_sys = nullptr; bool only_sys = false; };
static NotifPage nfp;

static void notif_dismiss(int id) {
    for (auto it = g_notifs.begin(); it != g_notifs.end(); ++it)
        if (it->id == id) { g_notifs.erase(it); break; }
    notif_save_all();
    update_notif_tile();
}

static GtkWidget* notif_card(const Notif& n) {
    GtkWidget* rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(rev), 260);
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(card, "notif");
    gtk_widget_add_css_class(card, "live-soft");
    if (n.level == "warn") gtk_widget_add_css_class(card, "warn");
    if (n.level == "error") gtk_widget_add_css_class(card, "error");
    const char* icon = n.sys ? (n.level == "error" ? "dialog-error-symbolic" : n.level == "warn" ? "dialog-warning-symbolic" : "emblem-system-symbolic")
                             : "mail-unread-symbolic";
    GtkWidget* ic = mkicon(icon, 20);
    gtk_widget_set_valign(ic, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ic, 2);
    gtk_box_append(GTK_BOX(card), ic);
    GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(v, TRUE);
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* t = mklabel(n.title.empty() ? "Без назви" : n.title, "title");
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(t, TRUE);
    gtk_box_append(GTK_BOX(top), t);
    gtk_box_append(GTK_BOX(top), mklabel((n.sys ? "Система · " : "") + time_ago(n.t), "caption", 1.0f));
    gtk_box_append(GTK_BOX(v), top);
    if (!n.body.empty()) {
        GtkWidget* b = mklabel(n.body, "sub");
        gtk_label_set_wrap(GTK_LABEL(b), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(b), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_lines(GTK_LABEL(b), 4);
        gtk_label_set_ellipsize(GTK_LABEL(b), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(b), 40);
        gtk_box_append(GTK_BOX(v), b);
    }
    gtk_box_append(GTK_BOX(card), v);
    GtkWidget* x = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(x, "dismiss");
    gtk_widget_set_valign(x, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), x);
    gtk_revealer_set_child(GTK_REVEALER(rev), card);
    g_object_set_data(G_OBJECT(rev), "nid", GINT_TO_POINTER(n.id));
    g_signal_connect(x, "clicked", G_CALLBACK(+[](GtkButton*, gpointer r) {
        GtkWidget* rev = GTK_WIDGET(r);
        notif_dismiss(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(rev), "nid")));
        gtk_revealer_set_reveal_child(GTK_REVEALER(rev), FALSE);
        g_timeout_add(300, [](gpointer r) -> gboolean {
            GtkWidget* w = GTK_WIDGET(r);
            if (gtk_widget_get_parent(w)) gtk_widget_unparent(w);
            g_object_unref(w);
            if (nfp.empty) gtk_widget_set_visible(nfp.empty, gtk_widget_get_first_child(nfp.list) == nullptr);
            return G_SOURCE_REMOVE;
        }, g_object_ref(rev));
    }), rev);
    return rev;
}

static void notif_page_fill() {
    clear_box(nfp.list);
    int shown = 0;
    for (auto& n : g_notifs) {
        if (nfp.only_sys && !n.sys) continue;
        if (++shown > 80) break;
        GtkWidget* c = notif_card(n);
        gtk_revealer_set_reveal_child(GTK_REVEALER(c), TRUE);
        gtk_box_append(GTK_BOX(nfp.list), c);
    }
    gtk_widget_set_visible(nfp.empty, shown == 0);
    set_on(nfp.seg_all, !nfp.only_sys);
    set_on(nfp.seg_sys, nfp.only_sys);
}

static void notif_ui_added(const Notif& n) {
    if (!cc.open || !nfp.list) return;
    const char* cur = gtk_stack_get_visible_child_name(GTK_STACK(cc.stack));
    if (!cur || strcmp(cur, "notifs") != 0) return;
    if (nfp.only_sys && !n.sys) return;
    GtkWidget* c = notif_card(n);
    gtk_box_prepend(GTK_BOX(nfp.list), c);
    gtk_widget_set_visible(nfp.empty, FALSE);
    g_idle_add([](gpointer r) -> gboolean { gtk_revealer_set_reveal_child(GTK_REVEALER(r), TRUE); return G_SOURCE_REMOVE; }, c);
}

static GtkWidget* build_notif_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget* clear = gtk_button_new_with_label("Очистити");
    gtk_widget_add_css_class(clear, "ghost");
    gtk_widget_add_css_class(clear, "live");
    g_signal_connect(clear, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        // Каскадне згортання карток, потім очищення
        int i = 0;
        for (GtkWidget* c = gtk_widget_get_first_child(nfp.list); c; c = gtk_widget_get_next_sibling(c), i++)
            g_timeout_add(i * 35, [](gpointer r) -> gboolean { gtk_revealer_set_reveal_child(GTK_REVEALER(r), FALSE); return G_SOURCE_REMOVE; }, c);
        if (nfp.only_sys) { for (auto it = g_notifs.begin(); it != g_notifs.end();) it = it->sys ? g_notifs.erase(it) : it + 1; }
        else g_notifs.clear();
        g_unseen_sys_err = 0;
        notif_save_all();
        update_notif_tile();
        g_timeout_add(i * 35 + 320, [](gpointer) -> gboolean { notif_page_fill(); return G_SOURCE_REMOVE; }, nullptr);
    }), nullptr);
    gtk_box_append(GTK_BOX(page), page_header("Сповіщення", clear));

    GtkWidget* seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(seg, "segbox");
    gtk_box_set_homogeneous(GTK_BOX(seg), TRUE);
    nfp.seg_all = gtk_button_new_with_label("Усі");
    nfp.seg_sys = gtk_button_new_with_label("Системні");
    for (GtkWidget* b : {nfp.seg_all, nfp.seg_sys}) { gtk_widget_add_css_class(b, "seg"); gtk_box_append(GTK_BOX(seg), b); }
    g_signal_connect(nfp.seg_all, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { nfp.only_sys = false; notif_page_fill(); }), nullptr);
    g_signal_connect(nfp.seg_sys, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { nfp.only_sys = true; notif_page_fill(); }), nullptr);
    gtk_box_append(GTK_BOX(page), seg);

    GtkWidget* c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    nfp.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    nfp.empty = mklabel("Сповіщень немає", "sub", 0.5f);
    gtk_widget_set_margin_top(nfp.empty, 30);
    gtk_widget_set_margin_bottom(nfp.empty, 30);
    gtk_box_append(GTK_BOX(c), nfp.empty);
    gtk_box_append(GTK_BOX(c), nfp.list);
    gtk_box_append(GTK_BOX(page), page_scroll(c, 600));
    return page;
}

// ============================================================================
//  НАВІГАЦІЯ, ОПИТУВАННЯ, ВІДКРИТТЯ/ЗАКРИТТЯ
// ============================================================================
static gboolean poll_tick(gpointer) {
    if (!cc.open) { cc.poll_timer = 0; return G_SOURCE_REMOVE; }
    const char* pg = gtk_stack_get_visible_child_name(GTK_STACK(cc.stack));
    std::string p = pg ? pg : "main";
    static int tick = 0;
    tick++;
    if (p == "main") { poll_status(); poll_positions(); }
    else if (p == "sound") sound_refresh();
    else if (p == "system") { sys_refresh(); if (tick % 10 == 0) disks_refresh(); }
    else if (p == "bluetooth" && tick % 3 == 0) bt_refresh();
    else if (p == "network" && tick % 8 == 0) net_refresh(false);
    else if (p == "notifs" && tick % 30 == 0) notif_page_fill();  // оновлює «N хв тому»
    return G_SOURCE_CONTINUE;
}

static void go_page(const char* name) {
    gtk_stack_set_visible_child_name(GTK_STACK(cc.stack), name);
    std::string p = name;
    if (p == "main") poll_status();
    else if (p == "network") net_refresh(false);
    else if (p == "bluetooth") bt_refresh();
    else if (p == "sound") { sp.last_sig.clear(); sound_refresh(); }
    else if (p == "system") { sys_refresh(); disks_refresh(); refresh_module_rows(); }
    else if (p == "notifs") { g_unseen_sys_err = 0; update_notif_tile(); notif_page_fill(); }
}

static void cc_open(const char* page = "main") {
    if (cc.hide_timer) { g_source_remove(cc.hide_timer); cc.hide_timer = 0; }
    gtk_widget_remove_css_class(cc.root, "cc-out");
    bool was_open = cc.open;
    cc.open = true;
    apply_theme();
    if (!was_open) {
        gtk_stack_set_transition_type(GTK_STACK(cc.stack), GTK_STACK_TRANSITION_TYPE_NONE);
        gtk_stack_set_visible_child_name(GTK_STACK(cc.stack), "main");
        gtk_stack_set_transition_type(GTK_STACK(cc.stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
        gtk_widget_set_visible(cc.win, TRUE);
        gtk_window_present(GTK_WINDOW(cc.win));
        // Каскадна поява плиток
        static const char* delays[] = {"d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8"};
        int i = 0;
        for (GtkWidget* t : cc.anim_tiles) {
            for (const char* d : delays) gtk_widget_remove_css_class(t, d);
            gtk_widget_remove_css_class(t, "tile-in");
            gtk_widget_add_css_class(t, delays[std::min(i++, 7)]);
            gtk_widget_add_css_class(t, "tile-in");
        }
        read_eco_state();
        update_notif_tile();
        if (!cc.poll_timer) cc.poll_timer = g_timeout_add(1500, poll_tick, nullptr);
    }
    go_page(page);
}

static void cc_close() {
    if (!cc.open) return;
    cc.open = false;
    gtk_widget_add_css_class(cc.root, "cc-out");
    cc.hide_timer = g_timeout_add(180, [](gpointer) -> gboolean {
        cc.hide_timer = 0;
        if (!cc.open) {
            gtk_widget_set_visible(cc.win, FALSE);
            gtk_widget_remove_css_class(cc.root, "cc-out");
            for (GtkWidget* t : cc.anim_tiles) gtk_widget_remove_css_class(t, "tile-in");
            clear_box(nfp.list);   // звільняємо картки сповіщень
        }
        return G_SOURCE_REMOVE;
    }, nullptr);
}

// ============================================================================
//  IPC
// ============================================================================
static gboolean on_socket(GIOChannel* src, GIOCondition, gpointer) {
    int fd = accept(g_io_channel_unix_get_fd(src), nullptr, nullptr);
    if (fd < 0) return G_SOURCE_CONTINUE;
    char buf[4096] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return G_SOURCE_CONTINUE;
    std::string cmd(buf, (size_t)n);
    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();

    if (cmd.rfind("notify|", 0) == 0) {                        // від dunst-хука
        std::string p = cmd.substr(7);
        auto a = p.find('|');
        notif_add(a == std::string::npos ? p : p.substr(0, a), a == std::string::npos ? "" : p.substr(a + 1), "", false);
    } else if (cmd.rfind("sysnotify|", 0) == 0) {              // sysnotify|level|title|body
        std::string p = cmd.substr(10);
        auto a = p.find('|'), b = p.find('|', a + 1);
        if (a != std::string::npos)
            sys_event(p.substr(0, a), p.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1),
                      b == std::string::npos ? "" : p.substr(b + 1));
    } else if (cmd.rfind("expect|", 0) == 0) {                 // expect|dock — плановий перезапуск (build.sh, налаштування)
        std::string bin = cmd.substr(7);
        for (auto& m : g_modules)
            if (bin == m.bin || bin == "all") { m.we_started_at = now_ms(); m.missing = -3; }
    } else if (cmd.rfind("page|", 0) == 0) {                   // page|network — відкрити одразу на сторінці
        std::string pg = cmd.substr(5);
        static std::string keep;
        keep = pg;
        cc_open(keep.c_str());
    } else if (cmd.find("toggle") != std::string::npos) {
        if (cc.open) cc_close(); else cc_open();
    } else if (cmd.find("show") != std::string::npos) {
        cc_open();
    } else if (cmd.find("hide") != std::string::npos) {
        cc_close();
    }
    return G_SOURCE_CONTINUE;
}

static void setup_socket() {
    std::string path = sysui::home() + "/.config/SYSui/cc.sock";
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return; }
    listen(fd, 8);
    GIOChannel* ch = g_io_channel_unix_new(fd);
    g_io_add_watch(ch, G_IO_IN, on_socket, nullptr);
}

// ============================================================================
//  ПОБУДОВА ВІКНА
// ============================================================================
static void build_ui(GtkApplication* app) {
    cc.win = gtk_application_window_new(app);
    sysui::release_on_hide(cc.win);
    gtk_layer_init_for_window(GTK_WINDOW(cc.win));
    gtk_layer_set_namespace(GTK_WINDOW(cc.win), "control_center");
    gtk_layer_set_layer(GTK_WINDOW(cc.win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    // Прозоре вікно на весь екран (під топбаром), панель — у правому верхньому куті.
    //  * клік повз панель закриває CC (як у macOS);
    //  * ФІКС ІГОР: курсор опиняється на нашій поверхні, а клавіатура — ексклюзивно в нас,
    //    тож гра втрачає фокус і відпускає захоплений курсор (pointer lock).
    // Повноекранний буфер існує лише поки CC відкритий (release_on_hide звільняє його).
    for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
        gtk_layer_set_anchor(GTK_WINDOW(cc.win), e, TRUE);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(cc.win), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    apply_theme(true);

    cc.root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(cc.root, "cc-root");
    gtk_widget_set_size_request(cc.root, 380, -1);
    gtk_widget_set_valign(cc.root, GTK_ALIGN_START);
    gtk_widget_set_halign(cc.root, GTK_ALIGN_END);
    gtk_widget_set_margin_top(cc.root, 8);
    gtk_widget_set_margin_end(cc.root, 10);

    cc.stack = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(cc.stack), TRUE);
    gtk_stack_set_vhomogeneous(GTK_STACK(cc.stack), FALSE);
    gtk_stack_set_interpolate_size(GTK_STACK(cc.stack), TRUE);
    gtk_stack_set_transition_type(GTK_STACK(cc.stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(cc.stack), 280);
    gtk_stack_add_named(GTK_STACK(cc.stack), build_main_page(), "main");
    gtk_stack_add_named(GTK_STACK(cc.stack), build_net_page(), "network");
    gtk_stack_add_named(GTK_STACK(cc.stack), build_bt_page(), "bluetooth");
    gtk_stack_add_named(GTK_STACK(cc.stack), build_sound_page(), "sound");
    gtk_stack_add_named(GTK_STACK(cc.stack), build_sys_page(), "system");
    gtk_stack_add_named(GTK_STACK(cc.stack), build_notif_page(), "notifs");
    gtk_box_append(GTK_BOX(cc.root), cc.stack);
    gtk_window_set_child(GTK_WINDOW(cc.win), cc.root);
    sysui::pop_on_show(cc.win, cc.root, "sysui-pop-down");

    // Escape: на підсторінці — назад, на головній — закрити
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint kv, guint, GdkModifierType, gpointer) -> gboolean {
        if (kv != GDK_KEY_Escape) return FALSE;
        const char* p = gtk_stack_get_visible_child_name(GTK_STACK(cc.stack));
        if (p && strcmp(p, "main") != 0) go_page("main"); else cc_close();
        return TRUE;
    }), nullptr);
    gtk_widget_add_controller(cc.win, keys);

    // Клік у прозору область навколо панелі → закрити (як у macOS).
    GtkGesture* outside = gtk_gesture_click_new();
    g_signal_connect(outside, "pressed", G_CALLBACK(+[](GtkGestureClick*, int, double x, double y, gpointer) {
        if (gtk_widget_pick(cc.win, x, y, GTK_PICK_DEFAULT) == cc.win) cc_close();
    }), nullptr);
    gtk_widget_add_controller(cc.win, GTK_EVENT_CONTROLLER(outside));

    gtk_widget_set_visible(cc.win, FALSE);

    // Перевірка DDC/CI у фоні (на моніторах без DDC займає ~6 с — тому один раз і асинхронно)
    sh_async("command -v ddcutil >/dev/null && timeout 12 ddcutil getvcp 10 --brief 2>/dev/null", [](const std::string& out, bool ok) {
        // формат: "VCP 10 C 50 100"
        std::istringstream ss(out);
        std::string vcp, code, type; int cur = -1, max = -1;
        if (ok && (ss >> vcp >> code >> type >> cur >> max) && max > 0) {
            cc.ddc_ok = true;
            gtk_range_set_value(GTK_RANGE(cc.bright_scale), 100.0 * cur / max);
            gtk_label_set_text(GTK_LABEL(cc.bright_pct), (std::to_string(100 * cur / max) + "%").c_str());
            gtk_widget_set_visible(cc.bright_tile, TRUE);
        }
    });

    // Тема/налаштування застосовуються наживо
    g_timeout_add_seconds(2, [](gpointer) -> gboolean { if (cc.open) apply_theme(); return G_SOURCE_CONTINUE; }, nullptr);
}

static void activate(GtkApplication* app, gpointer) {
    static bool init = false;
    if (init) return;
    init = true;
    sysui::mem_ready();
    notif_load();
    setup_socket();
    build_ui(app);
    mpris_init();
    update_notif_tile();
    poll_status();
    // Сторож модулів: перша перевірка за 4 с (щоб інші модулі встигли стартувати), далі кожні 3 с
    g_timeout_add_seconds(4, [](gpointer) -> gboolean {
        watchdog_tick(nullptr);
        g_timeout_add_seconds(3, watchdog_tick, nullptr);
        return G_SOURCE_REMOVE;
    }, nullptr);
}

int main(int argc, char** argv) {
    sysui::mem_init();
    signal(SIGPIPE, SIG_IGN);
    GtkApplication* app = gtk_application_new("com.megumin.cc", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
