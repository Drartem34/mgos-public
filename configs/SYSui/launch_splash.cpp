// mgos-launch — «підготовлене вікно» для програм, що довго відкриваються.
//
// Одразу відкриває звичайне (тайлове) вікно: прозоре, з блюром Hyprland, іконка програми по центру
// і міні-спінер. Програма запускається паралельно; щойно з'являється її вікно, splash закривається —
// вікно програми відкрилося в його ж клітинці (splash мав фокус) і просто заповнює місце.
//
//   mgos-launch --desktop org.telegram.desktop.desktop      (з .desktop: іконка, назва, StartupWMClass)
//   mgos-launch [--class CLS] [--icon NAME] [--name TITLE] --cmd "shell command"
//
// Вимикач: settings.json → "launch_splash": false — тоді просто запускає програму без вікна.
// Повторний запуск тієї самої програми, поки її splash ще висить, нічого не запускає вдруге.
#include "sysui_common.h"
#include <gio/gdesktopappinfo.h>
#include <sys/file.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <set>

using json = nlohmann::json;

namespace {

struct App {
    std::string icon_name, name, cmd, desktop, key;
    GIcon* gicon = nullptr;
    std::vector<std::string> classes;   // очікувані класи вікна, lower
    bool pid_match = true;              // шукати вікно і за деревом процесів (не для steam -applaunch)
    int timeout_s = 45;
} A;

GPid child = 0;
bool child_done = false, child_failed = false;
int child_status = 0;
gint64 child_done_at = 0;
std::set<std::string> existing;         // вікна з потрібним класом, що були ДО запуску (single-instance)
std::string self_addr;
GtkWidget* win = nullptr;
GtkWidget* spinner = nullptr;
GtkWidget* label = nullptr;
bool finished = false;
gint64 t0 = 0;

std::string lower(std::string s) { for (auto& c : s) c = (char)g_ascii_tolower(c); return s; }
std::string no0x(std::string a) { return a.rfind("0x", 0) == 0 ? a.substr(2) : a; }

void add_class(const std::string& c) {
    std::string l = lower(c);
    if (l.size() > 8 && l.compare(l.size() - 8, 8, ".desktop") == 0) l.resize(l.size() - 8);
    if (l.empty() || l == "mgos-splash") return;
    for (auto& x : A.classes) if (x == l) return;
    A.classes.push_back(l);
}

// перше «справжнє» слово команди: без env VAR=…, шляху і обгорток
void classes_from_cmd(const std::string& cmd) {
    gchar** argv = nullptr; gint argc = 0;
    if (!g_shell_parse_argv(cmd.c_str(), &argc, &argv, nullptr)) return;
    static const std::set<std::string> wrap = {"env", "sh", "bash", "exec", "nohup", "setsid", "flatpak", "run",
                                               "python", "python3", "java", "gamemoderun", "prime-run", "uwsm", "app"};
    for (int i = 0; i < argc; i++) {
        std::string t = argv[i];
        if (t.empty() || t[0] == '-' || t.find('=') != std::string::npos) continue;
        std::string b = lower(t.substr(t.find_last_of('/') + 1));
        if (wrap.count(b)) continue;
        if (b == "steam") break;
        add_class(b);
        break;
    }
    // Steam: вікно гри має клас steam_app_<id>, а не «steam»
    for (int i = 0; i + 1 < argc; i++)
        if (!strcmp(argv[i], "-applaunch")) { A.classes.clear(); add_class(std::string("steam_app_") + argv[i + 1]); A.pid_match = false; A.timeout_s = 120; }
    g_strfreev(argv);
}

bool class_matches(const std::string& cls_in) {
    std::string c = lower(cls_in);
    if (c.empty() || c == "mgos-splash") return false;
    for (auto& w : A.classes) {
        if (c == w) return true;
        if (w.size() >= 4 && c.size() >= 4 && (c.find(w) != std::string::npos || w.find(c) != std::string::npos)) return true;
    }
    return false;
}

bool descends_from(int pid, int root) {
    for (int i = 0; i < 48 && pid > 1; i++) {
        if (pid == root) return true;
        std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
        std::string s; std::getline(f, s);
        auto p = s.rfind(')');
        if (p == std::string::npos) return false;
        int ppid = 0; char st;
        if (sscanf(s.c_str() + p + 1, " %c %d", &st, &ppid) != 2) return false;
        pid = ppid;
    }
    return false;
}

json clients() { try { return json::parse(sysui::hypr_request("j/clients")); } catch (...) { return json::array(); } }

// ---------- завершення ----------
void quit_later(int ms) {
    g_timeout_add(ms, [](gpointer) -> gboolean { if (win) gtk_window_destroy(GTK_WINDOW(win)); win = nullptr; return G_SOURCE_REMOVE; }, nullptr);
}

void finish(const std::string& app_addr) {
    if (finished) return;
    finished = true;
    // splash зникає — вікно програми, що відкрилось у його клітинці, займає все місце
    if (win) { gtk_window_destroy(GTK_WINDOW(win)); win = nullptr; }
    if (!app_addr.empty()) sysui::hypr_request("dispatch focuswindow address:0x" + no0x(app_addr));
}

// Плашка-помилка через Control Center (sysnotify → notify_osd, червона, з іконкою програми) + історія сповіщень
void report_fail() {
    // краш із core-дампом уже покаже сторож падінь у Control Center — не дублюємо плашку
    if (child > 0 && WIFSIGNALED(child_status)) {
        int sg = WTERMSIG(child_status);
        if (sg == SIGSEGV || sg == SIGABRT || sg == SIGBUS || sg == SIGFPE || sg == SIGILL) return;
    }
    std::string who = A.name.empty() ? (A.classes.empty() ? std::string("Програма") : A.classes[0]) : A.name;
    std::string why;
    if (child == 0) why = "Не вдалося запустити команду";
    else if (WIFSIGNALED(child_status)) why = std::string("Впала одразу після старту (") + strsignal(WTERMSIG(child_status)) + ")";
    else if (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 127) why = "Програму не знайдено (код 127)";
    else if (WIFEXITED(child_status)) why = "Завершилась з кодом " + std::to_string(WEXITSTATUS(child_status));
    std::string cmd = A.cmd.empty() ? A.desktop : A.cmd;
    if (cmd.size() > 70) cmd = cmd.substr(0, 67) + "…";
    std::string msg = "sysnotify|error|«" + who + "» не запустилась|" + why + " · " + cmd + "|" +
                      (A.classes.empty() ? std::string() : A.classes[0]);
    for (auto& ch : msg) if (ch == '\n') ch = ' ';
    std::string path = sysui::home() + "/.config/SYSui/cc.sock";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un sa{}; sa.sun_family = AF_UNIX; strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    if (fd >= 0 && connect(fd, (sockaddr*)&sa, sizeof(sa)) == 0) { ssize_t w = write(fd, msg.data(), msg.size()); (void)w; }
    if (fd >= 0) close(fd);
}

void fail() {
    if (finished) return;
    finished = true;
    if (spinner) gtk_widget_set_visible(spinner, false);
    if (label) gtk_label_set_text(GTK_LABEL(label), (A.name.empty() ? std::string("Програма") : A.name).append(" не запустилась").c_str());
    if (win) gtk_widget_add_css_class(win, "failed");
    report_fail();
    quit_later(2600);
}

// ---------- події Hyprland (socket2) ----------
void on_event(const std::string& ev, const std::string& data) {
    if (finished) return;
    if (ev == "openwindow") {
        // ADDR,WS,CLASS,TITLE
        auto p1 = data.find(','), p2 = p1 == std::string::npos ? p1 : data.find(',', p1 + 1);
        if (p2 == std::string::npos) return;
        std::string addr = data.substr(0, p1), cls = data.substr(p2 + 1);
        auto p3 = cls.find(','); if (p3 != std::string::npos) cls.resize(p3);
        if (cls == "mgos-splash") {
            if (self_addr.empty())
                for (auto& c : clients())
                    if (no0x(c.value("address", "")) == addr && c.value("pid", 0) == (int)getpid()) self_addr = addr;
            return;
        }
        bool ok = class_matches(cls);
        if (!ok && A.pid_match && child > 0)
            for (auto& c : clients())
                if (no0x(c.value("address", "")) == addr) { ok = descends_from(c.value("pid", 0), child); break; }
        if (ok) finish(addr);
    } else if (ev == "activewindowv2") {
        // програма вже працювала і лише підняла своє вікно (Telegram, Discord…)
        if (existing.count(data) && (child_done || child == 0)) finish("");
    } else if (ev == "closewindow") {
        if (!self_addr.empty() && data == self_addr) finished = true;   // закрили splash вручну
    }
}

void listen_socket2() {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!xdg || !sig) return;
    std::string path = std::string(xdg) + "/hypr/" + sig + "/.socket2.sock";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un sa{}; sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    if (fd < 0 || connect(fd, (sockaddr*)&sa, sizeof(sa)) < 0) { if (fd >= 0) close(fd); return; }
    GIOChannel* ch = g_io_channel_unix_new(fd);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_channel_set_encoding(ch, nullptr, nullptr);
    g_io_add_watch(ch, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), [](GIOChannel* c, GIOCondition cond, gpointer) -> gboolean {
        if (cond & (G_IO_HUP | G_IO_ERR)) return G_SOURCE_REMOVE;
        gchar* line = nullptr; gsize len = 0;
        while (g_io_channel_read_line(c, &line, &len, nullptr, nullptr) == G_IO_STATUS_NORMAL && line) {
            std::string s(line, len); g_free(line); line = nullptr;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            auto p = s.find(">>");
            if (p != std::string::npos) on_event(s.substr(0, p), s.substr(p + 2));
            GIOCondition q = g_io_channel_get_buffer_condition(c);
            if (!(q & G_IO_IN)) break;
        }
        return G_SOURCE_CONTINUE;
    }, nullptr);
}

// ---------- запуск ----------
void child_setup(gpointer) { setsid(); }

void on_child_exit(GPid pid, gint status, gpointer) {
    g_spawn_close_pid(pid);
    child_done = true;
    child_done_at = g_get_monotonic_time();
    child_status = status;
    child_failed = !g_spawn_check_wait_status(status, nullptr);
}

void spawn_app() {
    if (!A.desktop.empty()) {
        GDesktopAppInfo* di = g_desktop_app_info_new(A.desktop.c_str());
        if (!di) di = g_desktop_app_info_new_from_filename(A.desktop.c_str());
        if (!di) return;
        g_desktop_app_info_launch_uris_as_manager(di, nullptr, nullptr,
            (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD), child_setup, nullptr,
            [](GDesktopAppInfo*, GPid pid, gpointer) { child = pid; }, nullptr, nullptr);
        g_object_unref(di);
    } else if (!A.cmd.empty()) {
        const char* argv[] = {"/bin/sh", "-c", A.cmd.c_str(), nullptr};
        g_spawn_async(nullptr, (gchar**)argv, nullptr, G_SPAWN_DO_NOT_REAP_CHILD, child_setup, nullptr, &child, nullptr);
    }
    if (child > 0) g_child_watch_add(child, on_child_exit, nullptr);
}

void load_desktop() {
    GDesktopAppInfo* di = g_desktop_app_info_new(A.desktop.c_str());
    if (!di) di = g_desktop_app_info_new_from_filename(A.desktop.c_str());
    if (!di) return;
    if (A.name.empty()) if (const char* n = g_app_info_get_display_name(G_APP_INFO(di))) A.name = n;
    if (A.icon_name.empty() && !A.gicon) if (GIcon* g = g_app_info_get_icon(G_APP_INFO(di))) A.gicon = G_ICON(g_object_ref(g));
    if (const char* w = g_desktop_app_info_get_startup_wm_class(di)) add_class(w);
    if (const char* id = g_app_info_get_id(G_APP_INFO(di))) add_class(id);
    if (const char* c = g_app_info_get_commandline(G_APP_INFO(di))) classes_from_cmd(c);
    g_object_unref(di);
}

// ---------- вікно ----------
void spinner_draw(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    double t = (g_get_monotonic_time() - t0) / 1e6;
    double r = std::min(w, h) / 2.0 - 2.5;
    GdkRGBA acc; if (!gdk_rgba_parse(&acc, sysui::accent().c_str())) acc = {0.6, 0.16, 0.81, 1};
    cairo_set_line_width(cr, 3); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    bool light = sysui::is_light_theme();
    cairo_set_source_rgba(cr, light ? 0 : 1, light ? 0 : 1, light ? 0 : 1, 0.14);
    cairo_arc(cr, w / 2.0, h / 2.0, r, 0, 2 * M_PI); cairo_stroke(cr);
    double a = t * 2 * M_PI * 0.9, len = M_PI * (0.55 + 0.35 * std::sin(t * 2.6));
    cairo_set_source_rgba(cr, acc.red, acc.green, acc.blue, 1);
    cairo_arc(cr, w / 2.0, h / 2.0, r, a, a + len); cairo_stroke(cr);
}

void build_css() {
    bool light = sysui::is_light_theme();
    std::string bg = light ? "rgba(246,246,250,0.34)" : "rgba(16,16,22,0.32)";
    std::string fg = light ? "rgba(20,20,28,0.78)" : "rgba(255,255,255,0.80)";
    std::string css =
        "window.mgos-splash { background: " + bg + "; }"
        "window.mgos-splash .splash-icon { -gtk-icon-shadow: 0 6px 18px rgba(0,0,0,0.30); }"
        "window.mgos-splash .splash-name { color: " + fg + "; font-size: 15px; font-weight: 600; }"
        "window.mgos-splash.failed .splash-name { color: #e5484d; }"
        "@keyframes splash-in { from { opacity: 0; transform: scale(0.86); } to { opacity: 1; transform: scale(1); } }"
        "@keyframes splash-breathe { 0% { opacity: 1; } 50% { opacity: 0.72; } 100% { opacity: 1; } }"
        "window.mgos-splash .splash-box { animation: splash-in 260ms cubic-bezier(.2,.9,.3,1.2); }";
    if (sysui::settings().animations)
        css += "window.mgos-splash .splash-icon { animation: splash-breathe 1.8s ease-in-out infinite; }";
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    t0 = g_get_monotonic_time();
    build_css();

    win = gtk_application_window_new(app);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_title(GTK_WINDOW(win), A.name.empty() ? "Запуск…" : A.name.c_str());
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 600);
    gtk_widget_add_css_class(win, "mgos-splash");
    g_signal_connect(win, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { win = nullptr; }), nullptr);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(box, "splash-box");
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget* img = A.gicon ? gtk_image_new_from_gicon(A.gicon)
                             : gtk_image_new_from_icon_name(A.icon_name.empty() ? "application-x-executable" : A.icon_name.c_str());
    gtk_image_set_pixel_size(GTK_IMAGE(img), 112);
    gtk_widget_add_css_class(img, "splash-icon");
    gtk_box_append(GTK_BOX(box), img);

    label = gtk_label_new(A.name.c_str());
    gtk_widget_add_css_class(label, "splash-name");
    gtk_widget_set_visible(label, !A.name.empty());
    gtk_box_append(GTK_BOX(box), label);

    spinner = gtk_drawing_area_new();
    gtk_widget_set_size_request(spinner, 26, 26);
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(spinner), spinner_draw, nullptr, nullptr);
    gtk_widget_add_tick_callback(spinner, [](GtkWidget* w, GdkFrameClock*, gpointer) -> gboolean {
        gtk_widget_queue_draw(w); return G_SOURCE_CONTINUE; }, nullptr, nullptr);
    gtk_box_append(GTK_BOX(box), spinner);

    gtk_window_set_child(GTK_WINDOW(win), box);

    // Esc / клік — прибрати splash (програма все одно запускається)
    GtkEventController* k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint kv, guint, GdkModifierType, gpointer) -> gboolean {
        if (kv == GDK_KEY_Escape) { finish(""); return TRUE; } return FALSE; }), nullptr);
    gtk_widget_add_controller(win, k);

    // Показуємо не одразу: швидкі програми встигають відкритись самі, і splash навіть не блимає
    g_timeout_add(140, [](gpointer) -> gboolean { if (!finished && win) gtk_window_present(GTK_WINDOW(win)); return G_SOURCE_REMOVE; }, nullptr);

    // Сторож: впала одразу / вже працювала (лише підняла вікно) / ліміт часу
    g_timeout_add(250, [](gpointer) -> gboolean {
        if (finished) { if (!win) { g_application_quit(g_application_get_default()); return G_SOURCE_REMOVE; } return G_SOURCE_CONTINUE; }
        gint64 now = g_get_monotonic_time();
        if (child == 0 && A.desktop.empty() && A.cmd.empty()) { fail(); return G_SOURCE_CONTINUE; }
        if (child_done) {
            double since = (now - child_done_at) / 1e6;
            if (child_failed && since > 1.2) { fail(); return G_SOURCE_CONTINUE; }
            if (since > 4 && !existing.empty()) { finish(""); return G_SOURCE_CONTINUE; }   // single-instance передав іншій копії
        }
        if ((now - t0) / 1e6 > A.timeout_s) finish("");
        return G_SOURCE_CONTINUE;
    }, nullptr);
}

int lock_key(const std::string& key) {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    std::string dir = std::string(xdg ? xdg : "/tmp") + "/mgos-launch";
    g_mkdir_with_parents(dir.c_str(), 0700);
    std::string k = key; for (auto& c : k) if (!g_ascii_isalnum(c) && c != '.' && c != '-' && c != '_') c = '_';
    int fd = open((dir + "/" + k.substr(0, 120) + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return 0;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return -1; }
    return fd;   // тримаємо до виходу
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--desktop") A.desktop = next();
        else if (a == "--cmd") A.cmd = next();
        else if (a == "--class") add_class(next());
        else if (a == "--icon") A.icon_name = next();
        else if (a == "--name") A.name = next();
        else if (a == "--timeout") A.timeout_s = std::clamp(atoi(next().c_str()), 5, 600);
        else if (a == "--") { std::string c; for (i++; i < argc; i++) { if (!c.empty()) c += ' '; gchar* q = g_shell_quote(argv[i]); c += q; g_free(q); } A.cmd = c; }
    }
    if (A.desktop.empty() && A.cmd.empty()) { fprintf(stderr, "usage: mgos-launch --desktop ID | [--class C] [--icon I] [--name N] --cmd CMD\n"); return 2; }
    if (!A.desktop.empty()) load_desktop();
    if (!A.cmd.empty()) classes_from_cmd(A.cmd);
    A.key = !A.desktop.empty() ? A.desktop : (!A.classes.empty() ? A.classes[0] : A.cmd);

    bool on = true;
    try { std::ifstream f(sysui::settings_path()); json j; if (f.good()) { f >> j; if (j.contains("launch_splash") && j["launch_splash"].is_boolean()) on = j["launch_splash"]; } } catch (...) {}
    if (!getenv("HYPRLAND_INSTANCE_SIGNATURE")) on = false;

    if (on && lock_key(A.key) < 0) return 0;   // ця програма вже запускається — не дублюємо

    // програма стартує першою, ще до GTK — splash нічого не гальмує
    for (auto& c : clients()) if (class_matches(c.value("class", ""))) existing.insert(no0x(c.value("address", "")));
    spawn_app();
    if (!on) { return 0; }

    sysui::mem_init();
    g_set_prgname("mgos-splash");   // app_id вікна → клас «mgos-splash» у Hyprland
    GtkApplication* app = gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    listen_socket2();
    int rc = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return rc;
}
