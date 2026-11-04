#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <cctype>
#include <dirent.h>
#include <signal.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <sys/stat.h>
#include "sysui_common.h"

using json = nlohmann::json;

// ===================== НАЛАШТУВАННЯ =====================

static std::vector<std::string> ALWAYS_VISIBLE_APPS = {
    "nautilus", "kitty", "wezterm", "ignis",
    "code", "modrinth-app", "steam", "spotify"
};
static const int MAX_RECENT_APPS = 15;
static int g_icon_size = 38;   // з налаштувань (dock_icon_size)

// ===================== КЕШ ДЛЯ ОПТИМІЗАЦІЇ (АНТИ-ФРИЗ) =====================
static std::string g_active_class = "";
static bool g_no_window = false;
static bool g_fullscreen = false;   // на активному воркспейсі є fullscreen-вікно → док не показуємо

static std::map<std::string, std::string> icon_cache;
static std::map<std::string, std::string> exec_cache;

// ===================== СТАН =====================

struct AppInfo {
    std::string id;        
    std::string name;      
    std::string icon_name; 
    std::string address;   
    int         pid = 0;
    bool        is_active = false;
};

struct RecentApp {
    std::string id;
    std::string icon_name;
    std::string exec_cmd;
};

static std::vector<RecentApp> recent_apps;

static long last_idle  = 0;
static long last_total = 0;

static int  cached_width  = 0;
static int  cached_height = 0;

static bool last_shown = true;

// ===================== ІСТОРІЯ (ПАМ'ЯТЬ) =====================

static void save_history() {
    std::string dir = std::string(getenv("HOME")) + "/.cache/megumin-os";
    system(("mkdir -p " + dir).c_str());
    std::ofstream f(dir + "/dock_history.txt");
    for (const auto& r : recent_apps) {
        f << r.id << "|" << r.icon_name << "|" << r.exec_cmd << "\n";
    }
}

static void load_history() {
    std::string file = std::string(getenv("HOME")) + "/.cache/megumin-os/dock_history.txt";
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string id, icon, exec_cmd;
        std::getline(ss, id, '|');
        std::getline(ss, icon, '|');
        std::getline(ss, exec_cmd, '|');
        if (!id.empty()) {
            if (id.rfind("steam_app_", 0) == 0) {
                std::string app_id = id.substr(10);
                exec_cmd = "steam -applaunch " + app_id;
            }
            recent_apps.push_back({id, icon, exec_cmd});
        }
    }
}

// ===================== УТИЛІТИ =====================

static std::string exec(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// ===================== БАЗА DESKTOP ФАЙЛІВ (завантажується один раз) =====================

struct DesktopEntry {
    std::string icon;
    std::string exec;
    std::string wm_class;   // StartupWMClass або WMClass
    std::string filename;   // basename без .desktop, lower
};

static std::vector<DesktopEntry> g_desktop_db;
static bool g_desktop_db_loaded = false;

static void load_desktop_db() {
    if (g_desktop_db_loaded) return;
    g_desktop_db_loaded = true;

    const char* home = getenv("HOME");
    std::vector<std::string> dirs = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        std::string(home ? home : "") + "/.local/share/applications"
    };

    for (const auto& dir : dirs) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name(ent->d_name);
            if (name.size() < 8) continue;
            if (name.substr(name.size()-8) != ".desktop") continue;

            std::string path = dir + "/" + name;
            std::ifstream f(path);
            if (!f.is_open()) continue;

            DesktopEntry entry;
            entry.filename = to_lower(name.substr(0, name.size()-8));

            std::string line;
            bool in_entry = false;
            while (std::getline(f, line)) {
                if (line == "[Desktop Entry]") { in_entry = true; continue; }
                if (!line.empty() && line[0] == '[') { if (in_entry) break; continue; }
                if (!in_entry) continue;

                if (line.rfind("Icon=", 0) == 0) {
                    entry.icon = line.substr(5);
                    while (!entry.icon.empty() && (entry.icon.back()=='\r'||entry.icon.back()==' ')) entry.icon.pop_back();
                } else if (line.rfind("Exec=", 0) == 0) {
                    entry.exec = line.substr(5);
                    // Прибираємо %u %f %F тощо
                    auto sp = entry.exec.find(' ');
                    if (sp != std::string::npos) entry.exec = entry.exec.substr(0, sp);
                    while (!entry.exec.empty() && (entry.exec.back()=='\r'||entry.exec.back()==' ')) entry.exec.pop_back();
                } else if (line.rfind("StartupWMClass=", 0) == 0 || line.rfind("WMClass=", 0) == 0) {
                    size_t eq = line.find('=');
                    entry.wm_class = to_lower(line.substr(eq+1));
                    while (!entry.wm_class.empty() && (entry.wm_class.back()=='\r'||entry.wm_class.back()==';'||entry.wm_class.back()==' ')) entry.wm_class.pop_back();
                }
            }
            if (!entry.icon.empty() || !entry.exec.empty())
                g_desktop_db.push_back(std::move(entry));
        }
        closedir(d);
    }
}

static std::string find_icon_for_class(const std::string& wm_class) {
    std::string cls = to_lower(wm_class);

    if (icon_cache.count(cls)) return icon_cache[cls];

    // Steam ігри: steam_app_730 -> шукаємо іконку гри
    if (cls.rfind("steam_app_", 0) == 0) {
        std::string app_id = cls.substr(10);
        const char* home = getenv("HOME");
        std::string home_s = home ? home : "";

        static const std::vector<std::string> sizes = {"256x256","128x128","64x64","48x48","32x32","16x16"};
        for (const auto& sz : sizes) {
            std::string p = home_s + "/.local/share/icons/hicolor/" + sz + "/apps/steam_icon_" + app_id + ".png";
            if (access(p.c_str(), R_OK) == 0) { 
                icon_cache[cls] = p; 
                return p; 
            }
        }
        // 2. Steam librarycache (завантажується при встановленні гри)
        for (const auto& ext : {".jpg", ".png"}) {
            std::string p = home_s + "/.local/share/Steam/appcache/librarycache/" + app_id + "_icon" + ext;
            if (access(p.c_str(), R_OK) == 0) { icon_cache[cls] = p; return p; }
        }
        // 3. Symbolic fallback (GTK знайде якщо є в темі)
        std::string icon_name = "steam_icon_" + app_id;
        icon_cache[cls] = icon_name;
        return icon_name;
    }

    load_desktop_db();

    // Прохід 1: по імені файлу
    for (const auto& e : g_desktop_db) {
        if (e.filename.find(cls) != std::string::npos || cls.find(e.filename) != std::string::npos) {
            if (!e.icon.empty()) { icon_cache[cls] = e.icon; return e.icon; }
        }
    }
    // Прохід 2: по StartupWMClass
    for (const auto& e : g_desktop_db) {
        if (!e.wm_class.empty()) {
            if (e.wm_class == cls || e.wm_class.find(cls) != std::string::npos || cls.find(e.wm_class) != std::string::npos) {
                if (!e.icon.empty()) { icon_cache[cls] = e.icon; return e.icon; }
            }
        }
    }

    icon_cache[cls] = cls;
    return cls;
}

static std::string find_exec_for_class(const std::string& wm_class) {
    std::string cls = to_lower(wm_class);

    if (exec_cache.count(cls)) return exec_cache[cls];
    
    // Steam ігри: запускаємо через steam -applaunch
    if (cls.rfind("steam_app_", 0) == 0) {
        std::string app_id = cls.substr(10);
        std::string exec_cmd = "steam -applaunch " + app_id;
        exec_cache[cls] = exec_cmd;
        return exec_cmd;
    }

    load_desktop_db();

    // Прохід 1: по імені файлу
    for (const auto& e : g_desktop_db) {
        if (e.filename.find(cls) != std::string::npos || cls.find(e.filename) != std::string::npos) {
            if (!e.exec.empty()) { exec_cache[cls] = e.exec; return e.exec; }
        }
    }
    // Прохід 2: по StartupWMClass
    for (const auto& e : g_desktop_db) {
        if (!e.wm_class.empty()) {
            if (e.wm_class == cls || e.wm_class.find(cls) != std::string::npos || cls.find(e.wm_class) != std::string::npos) {
                if (!e.exec.empty()) { exec_cache[cls] = e.exec; return e.exec; }
            }
        }
    }

    exec_cache[cls] = cls;
    return cls;
}


// ===================== СНІМОК ВІДКРИТИХ ПРОГРАМ (на випадок різкого вимкнення ПК) =====================
static std::string g_last_open_snapshot_sig = "\x01";

static void save_open_snapshot(const std::vector<std::string>& order, const std::map<std::string, AppInfo>& apps) {
    std::string sig;
    for (const auto& cls : order) sig += cls + ";";
    if (sig == g_last_open_snapshot_sig) return; 
    g_last_open_snapshot_sig = sig;

    std::string dir = std::string(getenv("HOME")) + "/.cache/megumin-os";
    system(("mkdir -p " + dir).c_str());
    std::ofstream f(dir + "/dock_open_snapshot.txt");
    for (const auto& cls : order) {
        const AppInfo& info = apps.at(cls);
        f << cls << "|" << info.icon_name << "|" << find_exec_for_class(cls) << "\n";
    }
}

static std::vector<RecentApp> load_open_snapshot() {
    std::vector<RecentApp> result;
    std::string file = std::string(getenv("HOME")) + "/.cache/megumin-os/dock_open_snapshot.txt";
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string id, icon, exec_cmd;
        std::getline(ss, id, '|');
        std::getline(ss, icon, '|');
        std::getline(ss, exec_cmd, '|');
        if (!id.empty()) {
            if (id.rfind("steam_app_", 0) == 0) {
                std::string app_id = id.substr(10);
                exec_cmd = "steam -applaunch " + app_id;
            }
            result.push_back({id, icon, exec_cmd});
        }
    }
    return result;
}

// ===================== HYPRLAND =====================

static json get_clients() {
    std::string out = sysui::hypr_request("j/clients");
    if (out.empty()) return json::array();
    try { return json::parse(out); } catch(...) { return json::array(); }
}

static json get_active_window() {
    std::string out = sysui::hypr_request("j/activewindow");
    if (out.empty()) return json::object();
    try { return json::parse(out); } catch(...) { return json::object(); }
}

static int get_active_workspace_id() {
    std::string out = sysui::hypr_request("j/activeworkspace");
    if (out.empty()) return 1;
    try { auto d = json::parse(out); return d.value("id", 1); } catch(...) { return 1; }
}

static void get_monitor_size(int& width, int& height) {
    if (cached_width != 0) { width = cached_width; height = cached_height; return; }
    std::string out = sysui::hypr_request("j/monitors");
    try {
        auto d = json::parse(out);
        if (!d.empty()) {
            double scale = d[0].value("scale", 1.0);
            cached_width  = (int)(d[0].value("width",  1920) / scale);
            cached_height = (int)(d[0].value("height", 1080) / scale);
        }
    } catch(...) { cached_width = 1920; cached_height = 1080; }
    width  = cached_width;
    height = cached_height;
}

static void launch_app(const std::string& exec_cmd) {
    std::string cmd = exec_cmd + " >/dev/null 2>&1 &";
    system(cmd.c_str());
}

static void close_window_graceful(const std::string& address) {
    if (address.empty()) return;
    std::string cmd = "hyprctl dispatch closewindow address:" + address + " >/dev/null 2>&1 &";
    system(cmd.c_str());
}

static void close_window_force(int pid, const std::string& address, const std::string& app_id) {
    if (pid > 0) {
        std::string cmd = "kill -9 " + std::to_string(pid) + " >/dev/null 2>&1 &";
        system(cmd.c_str());
    }
    if (!app_id.empty()) {
        std::string cmd = "killall -9 " + app_id + " >/dev/null 2>&1 &";
        system(cmd.c_str());
    }
    if (!address.empty()) {
        std::string cmd = "hyprctl dispatch closewindow address:" + address + " >/dev/null 2>&1 &";
        system(cmd.c_str());
    }
}

// ===================== FORWARD DECLARATIONS =====================
struct DockState;
static gboolean update_dock(gpointer user_data);
static void files_schedule();
static DockState* g_dock_state = nullptr;

// ===================== ІКОНКА =====================

static GtkWidget* make_icon(const std::string& icon_name, int size) {
    GtkWidget* img;
    // Якщо іконка — це шлях до файлу (починається з /) — використовуємо GtkPicture для надійного масштабування
    if (!icon_name.empty() && icon_name[0] == '/') {
        img = gtk_picture_new_for_filename(icon_name.c_str());
        gtk_widget_set_size_request(img, size, size);
    } else {
        img = gtk_image_new_from_icon_name(icon_name.c_str());
        gtk_image_set_pixel_size(GTK_IMAGE(img), size);
    }
    gtk_widget_add_css_class(img, "dock-icon");
    return img;
}


// ===================== MACOS-ЗБІЛЬШЕННЯ ІКОНОК =====================
// Кожна іконка має поточний і цільовий масштаб. Власний layout-менеджер розставляє
// іконки з трансформацією scale() з якорем знизу по центру — іконка «виростає» вгору,
// а сусіди розсуваються (ширина слоту = ширина * масштаб), як у доку macOS.
// Ціль рахується за гаусовою кривою від відстані до курсора в «незбільшених»
// координатах, тож немає дрижання від зворотного зв'язку.
struct Mag { double cur = 1.0, target = 1.0; };
static GtkWidget* g_mag_boxes[3] = {nullptr, nullptr, nullptr};
static GtkWidget* g_mag_window = nullptr;
static bool   g_pointer_in = false;
static double g_px = 0, g_py = 0;          // у координатах вікна
static guint  g_mag_tick = 0;

static Mag* mag_of(GtkWidget* w) { return (Mag*)g_object_get_data(G_OBJECT(w), "mag"); }
static void mag_attach(GtkWidget* w) {
    g_object_set_data_full(G_OBJECT(w), "mag", new Mag(), [](gpointer p) { delete static_cast<Mag*>(p); });
}
static double mag_max() {
    const auto& st = sysui::settings();
    return st.dock_magnify ? st.dock_magnify_pct / 100.0 : 1.0;
}
static int mag_headroom() { return mag_max() > 1.0 ? (int)std::ceil(g_icon_size * (mag_max() - 1.0)) + 6 : 0; }

static void mag_measure(GtkWidget* w, GtkOrientation o, int, int* mn, int* nt, int* mb, int* nb) {
    *mn = *nt = 0; *mb = *nb = -1;
    double acc = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        if (!gtk_widget_should_layout(c)) continue;
        int cm, cn;
        gtk_widget_measure(c, o, -1, &cm, &cn, nullptr, nullptr);
        // Ширина рахується БЕЗ масштабу — док не розтягується, коли іконки збільшуються
        if (o == GTK_ORIENTATION_HORIZONTAL) acc += cn;
        else { *mn = std::max(*mn, cm); *nt = std::max(*nt, cn); }
    }
    if (o == GTK_ORIENTATION_HORIZONTAL) *mn = *nt = (int)std::ceil(acc);
}

static void mag_allocate(GtkWidget* w, int width, int height, int) {
    // Збільшені слоти стискаються пропорційно, щоб уся низка лишалась у незмінній ширині:
    // іконка під курсором росте, а сусіди трохи насуваються — полиця доку не розтягується.
    double base = 0, scaled = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        if (!gtk_widget_should_layout(c)) continue;
        int cm, cw;
        gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &cm, &cw, nullptr, nullptr);
        Mag* m = mag_of(c);
        base += cw; scaled += cw * (m ? m->cur : 1.0);
    }
    double k = scaled > base && scaled > 0 ? std::max((double)width, base) / scaled : 1.0;
    double x = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        if (!gtk_widget_should_layout(c)) continue;
        int cm, cw;
        gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &cm, &cw, nullptr, nullptr);
        Mag* m = mag_of(c);
        double s = m ? m->cur : 1.0, slot = cw * s * k;
        graphene_point_t a = GRAPHENE_POINT_INIT((float)(x + slot / 2.0), (float)height);
        graphene_point_t b = GRAPHENE_POINT_INIT(-(float)cw / 2.0f, -(float)height);
        GskTransform* t = gsk_transform_translate(nullptr, &a);
        t = gsk_transform_scale(t, (float)s, (float)s);
        t = gsk_transform_translate(t, &b);
        gtk_widget_allocate(c, cw, height, -1, t);
        x += slot;
    }
}

static void mag_update_targets() {
    double maxs = mag_max();
    for (GtkWidget* box : g_mag_boxes) {
        if (!box || !gtk_widget_get_mapped(box)) continue;
        std::vector<GtkWidget*> kids;
        std::vector<int> nat;
        for (GtkWidget* c = gtk_widget_get_first_child(box); c; c = gtk_widget_get_next_sibling(c)) {
            if (!gtk_widget_should_layout(c)) continue;
            int cm, cn;
            gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &cm, &cn, nullptr, nullptr);
            kids.push_back(c); nat.push_back(cn);
        }
        graphene_point_t pin = GRAPHENE_POINT_INIT((float)g_px, (float)g_py), pout;
        bool have = g_pointer_in && maxs > 1.0 && gtk_widget_compute_point(g_mag_window, box, &pin, &pout);
        // Курсор у збільшених координатах → у базові (без масштабу)
        double base_px = 0;
        if (have) {
            double tb = 0, ts = 0;
            for (size_t i = 0; i < kids.size(); i++) { Mag* m = mag_of(kids[i]); tb += nat[i]; ts += nat[i] * (m ? m->cur : 1.0); }
            double k = ts > tb && ts > 0 ? std::max((double)gtk_widget_get_width(box), tb) / ts : 1.0;
            double acc = 0, base = 0; bool found = false;
            if (pout.x < 0) { base_px = pout.x; found = true; }   // курсор лівіше блоку — справжня відстань
            for (size_t i = 0; i < kids.size(); i++) {
                Mag* m = mag_of(kids[i]);
                double slot = nat[i] * (m ? m->cur : 1.0) * k;
                if (!found && pout.x < acc + slot) { base_px = base + (pout.x - acc) / slot * nat[i]; found = true; }
                acc += slot; base += nat[i];
            }
            if (!found) base_px = base + (pout.x - acc);       // курсор правіше блоку
        }
        double sigma = g_icon_size * 1.6, base = 0;
        for (size_t i = 0; i < kids.size(); i++) {
            Mag* m = mag_of(kids[i]);
            if (m) {
                double d = (base + nat[i] / 2.0 - base_px) / sigma;
                m->target = have ? 1.0 + (maxs - 1.0) * std::exp(-d * d) : 1.0;
            }
            base += nat[i];
        }
    }
}

static gboolean mag_tick(GtkWidget*, GdkFrameClock*, gpointer) {
    mag_update_targets();
    bool moving = false;
    for (GtkWidget* box : g_mag_boxes) {
        if (!box) continue;
        bool changed = false;
        for (GtkWidget* c = gtk_widget_get_first_child(box); c; c = gtk_widget_get_next_sibling(c)) {
            Mag* m = mag_of(c);
            if (!m) continue;
            double d = m->target - m->cur;
            if (std::fabs(d) > 0.002) { m->cur += d * 0.3; changed = true; }
            else if (m->cur != m->target) { m->cur = m->target; changed = true; }
        }
        if (changed) { gtk_widget_queue_allocate(box); moving = true; }
    }
    if (!moving) { g_mag_tick = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

// Скинути збільшення (док сховався з курсором над іконкою — подія «leave» не приходить,
// і раніше іконка лишалась збільшеною при наступній появі доку).
static void mag_reset() {
    g_pointer_in = false;
    for (GtkWidget* box : g_mag_boxes) {
        if (!box) continue;
        for (GtkWidget* c = gtk_widget_get_first_child(box); c; c = gtk_widget_get_next_sibling(c))
            if (Mag* m = mag_of(c)) { m->cur = 1.0; m->target = 1.0; }
        gtk_widget_queue_allocate(box);
    }
}

static void mag_kick() {
    if (!g_mag_tick && g_mag_window) g_mag_tick = gtk_widget_add_tick_callback(g_mag_window, mag_tick, nullptr, nullptr);
}

// ===================== КЛІКИ =====================

static void on_left_click(GtkButton* btn, gpointer user_data) {
    // SUPER+LMB = Мега-кіл: вбиваємо всі процеси застосунку
    GdkModifierType state = {};
    GdkDisplay* display = gdk_display_get_default();
    if (display) {
        GdkSeat* seat = gdk_display_get_default_seat(display);
        if (seat) {
            GdkDevice* pointer = gdk_seat_get_pointer(seat);
            if (pointer) {
                state = gdk_device_get_modifier_state(pointer);
            }
        }
    }

    if (state & GDK_SUPER_MASK) {
        // SUPER+ЛКМ — вбиваємо ВСЕ пов'язане з цим застосунком
        int pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "window-pid"));
        const char* address = (const char*)g_object_get_data(G_OBJECT(btn), "window-address");
        const char* app_id = (const char*)g_object_get_data(G_OBJECT(btn), "app-id");
        std::string addr_str = address ? address : "";
        std::string app_str  = app_id  ? app_id  : "";
        close_window_force(pid, addr_str, app_str);
        return;
    }

    const char* address = (const char*)g_object_get_data(G_OBJECT(btn), "window-address");
    if (address && strlen(address) > 0) {
        std::string cmd = "hyprctl dispatch focuswindow address:" + std::string(address) + " >/dev/null 2>&1 &";
        system(cmd.c_str());
    }
}

static void on_middle_click(GtkGestureClick* gesture, int, double, double, gpointer user_data) {
    GtkWidget* btn = GTK_WIDGET(user_data);
    int pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "window-pid"));
    const char* address = (const char*)g_object_get_data(G_OBJECT(btn), "window-address");
    const char* app_id = (const char*)g_object_get_data(G_OBJECT(btn), "app-id");

    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    std::string addr_str = address ? address : "";
    std::string app_str  = app_id  ? app_id  : "";

    if (state & GDK_SHIFT_MASK) {
        // SHIFT+СКМ = жорсткий кіл
        close_window_force(pid, addr_str, app_str);
    } else {
        // СКМ = м'яке закриття
        close_window_graceful(addr_str);
    }
}

static void on_recent_click(GtkButton* btn, gpointer user_data) {
    const char* cmd = (const char*)g_object_get_data(G_OBJECT(btn), "exec-cmd");
    if (cmd) launch_app(cmd);
    // Іконка підстрибує, поки програма запускається (як у macOS)
    GtkWidget* vbox = gtk_button_get_child(btn);
    GtkWidget* icon = vbox ? gtk_widget_get_first_child(vbox) : nullptr;
    if (icon) {
        gtk_widget_add_css_class(icon, "bounce");
        g_timeout_add(2300, [](gpointer w) -> gboolean {
            gtk_widget_remove_css_class(GTK_WIDGET(w), "bounce");
            g_object_unref(w);
            return G_SOURCE_REMOVE;
        }, g_object_ref(icon));
    }
}

// ОБРОБНИК: Видалення з recent на СКМ + негайне оновлення UI
static void on_recent_middle_click(GtkGestureClick* gesture, int, double, double, gpointer user_data) {
    GtkWidget* btn = GTK_WIDGET(user_data);
    const char* app_id = (const char*)g_object_get_data(G_OBJECT(btn), "app-id");
    if (!app_id) return;

    std::string target_id = app_id;
    
    // Вирізаємо прогу зі списку
    recent_apps.erase(
        std::remove_if(recent_apps.begin(), recent_apps.end(),
            [&](const RecentApp& r) { return r.id == target_id; }),
        recent_apps.end()
    );
    
    // Зберігаємо змінений стан
    save_history();

    // Негайно оновлюємо UI дока
    if (g_dock_state) g_idle_add(update_dock, g_dock_state);
}

// ===================== СТАН ДОКА =====================

struct DockState {
    GtkWidget* dock_box;         
    GtkWidget* recent_box;       
    GtkWidget* separator;        
    GtkWidget* ws_label;         
    GtkWidget* cpu_label;
    GtkWidget* ram_label;
    GtkWidget* revealer;         
    GtkWidget* window;

    std::map<std::string, GtkWidget*> current_items;
    std::vector<GtkWidget*> recent_items;
    std::string last_recent_sig = "\x01"; 
};

// ===================== ОНОВЛЕННЯ ДОКА =====================

static gboolean update_dock(gpointer user_data) {
    auto* state = static_cast<DockState*>(user_data);
    
    std::string old_history = "";
    for(auto& r : recent_apps) old_history += r.id;

    json clients     = get_clients();
    json active_info = get_active_window();
    std::string active_addr = active_info.value("address", "");
    
    g_active_class = to_lower(active_info.value("class", ""));
    g_no_window = !active_info.contains("address") || active_addr.empty();

    // FULLSCREEN: Hyprland 0.4x+ віддає "fullscreen": 0 (нема) / 1 (maximize) / 2 (fullscreen),
    // старіші версії — bool. Ховаємо док лише на справжньому fullscreen (2 або true),
    // maximize залишає док як був.
    g_fullscreen = false;
    if (!g_no_window && active_info.contains("fullscreen")) {
        const auto& fs = active_info["fullscreen"];
        if (fs.is_number_integer()) g_fullscreen = fs.get<int>() >= 2;
        else if (fs.is_boolean())   g_fullscreen = fs.get<bool>() && active_info.value("fullscreenMode", 0) != 1;
    }

    std::vector<json> sorted_clients(clients.begin(), clients.end());
    std::sort(sorted_clients.begin(), sorted_clients.end(), [](const json& a, const json& b) {
        return a.value("workspace", json{{"id", 0}}).value("id", 0) <
               b.value("workspace", json{{"id", 0}}).value("id", 0);
    });

    std::map<std::string, AppInfo> new_apps;
    std::vector<std::string> order;

    for (auto& w : sorted_clients) {
        std::string cls     = to_lower(w.value("class", ""));
        std::string initial = to_lower(w.value("initialClass", ""));
        std::string addr    = w.value("address", "");
        int         pid     = w.value("pid", 0);
        bool        active  = (addr == active_addr);

        if (cls.empty()) continue;

        if (new_apps.find(cls) == new_apps.end()) {
            AppInfo info;
            info.id        = cls;
            info.name      = cls;
            info.icon_name = find_icon_for_class(cls);
            info.address   = addr;
            info.pid       = pid;
            info.is_active = active;
            new_apps[cls]  = info;
            order.push_back(cls);
        } else {
            if (active) {
                new_apps[cls].is_active = true;
                new_apps[cls].address   = addr;
                new_apps[cls].pid       = pid;
            }
        }
    }

    save_open_snapshot(order, new_apps);

    for (auto& [cls, widget] : state->current_items) {
        if (new_apps.find(cls) == new_apps.end()) {
            bool already = false;
            for (auto& r : recent_apps) if (r.id == cls) { already = true; break; }
            if (!already) {
                RecentApp ra;
                ra.id        = cls;
                ra.icon_name = find_icon_for_class(cls);
                ra.exec_cmd  = find_exec_for_class(cls);
                recent_apps.insert(recent_apps.begin(), ra);
                if ((int)recent_apps.size() > MAX_RECENT_APPS) recent_apps.resize(MAX_RECENT_APPS);
            }
        }
    }
    
    recent_apps.erase(std::remove_if(recent_apps.begin(), recent_apps.end(),
        [&](const RecentApp& r) { return new_apps.count(r.id) > 0; }),
        recent_apps.end());

    std::string new_history = "";
    for(auto& r : recent_apps) new_history += r.id;
    if (old_history != new_history) save_history();

    for (auto it = state->current_items.begin(); it != state->current_items.end(); ) {
        if (new_apps.find(it->first) == new_apps.end()) {
            gtk_box_remove(GTK_BOX(state->dock_box), it->second);
            it = state->current_items.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& cls : order) {
        const AppInfo& info = new_apps[cls];
        if (state->current_items.find(cls) == state->current_items.end()) {
            GtkWidget* btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "dock-item");
            mag_attach(btn);
            gtk_widget_add_css_class(btn, "running");
            if (info.is_active) gtk_widget_add_css_class(btn, "active");

            GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
            
            GtkWidget* icon = make_icon(info.icon_name, g_icon_size);
            gtk_box_append(GTK_BOX(vbox), icon);
            
            GtkWidget* dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(dot, "mac-dot");
            gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(vbox), dot);
            
            gtk_button_set_child(GTK_BUTTON(btn), vbox);

            gtk_widget_set_tooltip_text(btn, info.name.c_str());
            gtk_widget_set_cursor_from_name(btn, "pointer");

            g_object_set_data_full(G_OBJECT(btn), "window-address", g_strdup(info.address.c_str()), g_free);
            g_object_set_data(G_OBJECT(btn), "window-pid", GINT_TO_POINTER(info.pid));
            g_object_set_data_full(G_OBJECT(btn), "app-id", g_strdup(info.id.c_str()), g_free);

            g_signal_connect(btn, "clicked", G_CALLBACK(on_left_click), NULL);

            GtkGesture* gmid = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gmid), 2);
            g_signal_connect(gmid, "released", G_CALLBACK(on_middle_click), btn);
            gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(gmid));

            gtk_box_append(GTK_BOX(state->dock_box), btn);
            state->current_items[cls] = btn;
        } else {
            GtkWidget* btn = state->current_items[cls];
            g_object_set_data_full(G_OBJECT(btn), "window-address", g_strdup(info.address.c_str()), g_free);
            g_object_set_data(G_OBJECT(btn), "window-pid", GINT_TO_POINTER(info.pid));
            g_object_set_data_full(G_OBJECT(btn), "app-id", g_strdup(info.id.c_str()), g_free);

            if (info.is_active) gtk_widget_add_css_class(btn, "active");
            else gtk_widget_remove_css_class(btn, "active");
        }
    }

    std::string new_recent_sig;
    for (const auto& r : recent_apps) new_recent_sig += r.id + ";";

    if (new_recent_sig != state->last_recent_sig) {
        state->last_recent_sig = new_recent_sig;

        GtkWidget* child = gtk_widget_get_first_child(state->recent_box);
        while (child) {
            GtkWidget* next = gtk_widget_get_next_sibling(child);
            gtk_box_remove(GTK_BOX(state->recent_box), child);
            child = next;
        }
        state->recent_items.clear();

        if (!recent_apps.empty()) {
            gtk_widget_set_visible(state->recent_box, TRUE);
            GtkWidget* sep = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_widget_add_css_class(sep, "dock-separator");
            gtk_widget_set_size_request(sep, 2, -1);
            gtk_widget_set_valign(sep, GTK_ALIGN_FILL);
            gtk_box_append(GTK_BOX(state->recent_box), sep);

            for (const auto& ra : recent_apps) {
                GtkWidget* btn = gtk_button_new();
                gtk_widget_add_css_class(btn, "dock-item");
                mag_attach(btn);
                gtk_widget_add_css_class(btn, "recent");

                GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
                
                GtkWidget* icon = make_icon(ra.icon_name, g_icon_size);
                gtk_box_append(GTK_BOX(vbox), icon);
                
                GtkWidget* dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
                gtk_widget_add_css_class(dot, "mac-dot");
                gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(vbox), dot);
                
                gtk_button_set_child(GTK_BUTTON(btn), vbox);

                gtk_widget_set_tooltip_text(btn, ra.id.c_str());
                gtk_widget_set_cursor_from_name(btn, "pointer");

                g_object_set_data_full(G_OBJECT(btn), "exec-cmd", g_strdup(ra.exec_cmd.c_str()), g_free);
                g_object_set_data_full(G_OBJECT(btn), "app-id", g_strdup(ra.id.c_str()), g_free);

                g_signal_connect(btn, "clicked", G_CALLBACK(on_recent_click), NULL);

                // ПРИКРІПЛЯЄМО ЖЕСТ СЕРЕДНЬОГО КЛІКУ ДЛЯ ВИДАЛЕННЯ
                GtkGesture* gmid = gtk_gesture_click_new();
                gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gmid), 2);
                g_signal_connect(gmid, "released", G_CALLBACK(on_recent_middle_click), btn);
                gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(gmid));

                gtk_box_append(GTK_BOX(state->recent_box), btn);
                state->recent_items.push_back(btn);
            }
        } else {
            gtk_widget_set_visible(state->recent_box, FALSE);
        }
    }

    return G_SOURCE_REMOVE;
}

// ===================== ВОРКСПЕЙС =====================
static gboolean update_workspace(gpointer user_data) {
    GtkLabel* label = GTK_LABEL(user_data);
    int ws = get_active_workspace_id();
    std::string txt = "ㅤ ㅤ " + std::to_string(ws);
    gtk_label_set_text(label, txt.c_str());
    return G_SOURCE_REMOVE;
}

// ===================== SYS MONITOR =====================
struct SysWidgets { GtkWidget* cpu; GtkWidget* ram; };

static gboolean update_sysmon(gpointer user_data) {
    auto* sw = static_cast<SysWidgets*>(user_data);
    {
        std::ifstream f("/proc/stat");
        std::string line;
        std::getline(f, line);
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        long vals[7] = {};
        for (int i = 0; i < 7; i++) ss >> vals[i];
        long idle  = vals[3] + vals[4];
        long total = 0;
        for (int i = 0; i < 7; i++) total += vals[i];
        long idle_delta  = idle  - last_idle;
        long total_delta = total - last_total;
        last_idle  = idle;
        last_total = total;
        if (total_delta > 0) {
            int pct = (int)(100.0 * (1.0 - (double)idle_delta / total_delta));
            gtk_label_set_text(GTK_LABEL(sw->cpu), (" ㅤ" + std::to_string(pct) + "%").c_str());
        }
    }
    {
        std::ifstream f("/proc/meminfo");
        long mem_total = 0, mem_avail = 0;
        std::string line;
        int found = 0;
        while (std::getline(f, line) && found < 2) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::istringstream ss(line); std::string k; ss >> k >> mem_total; found++;
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                std::istringstream ss(line); std::string k; ss >> k >> mem_avail; found++;
            }
        }
        if (mem_total > 0) {
            int pct = (int)(100.0 * (mem_total - mem_avail) / mem_total);
            gtk_label_set_text(GTK_LABEL(sw->ram), (" ㅤ" + std::to_string(pct) + "%").c_str());
        }
    }
    return G_SOURCE_CONTINUE;
}

// ===================== АВТО-ПРИХОВУВАННЯ =====================
static std::atomic<int> cursor_x{0};
static std::atomic<int> cursor_y{0};

// Раніше тут кожні 150 мс робився popen("hyprctl cursorpos") = fork+exec двох процесів
// (~7 разів на секунду, постійно). Тепер — прямий запит у сокет Hyprland, без fork.
// Поки активне fullscreen-вікно, позиція курсора не потрібна — опитуємо рідше.
static void poll_cursor_thread() {
    while (true) {
        if (g_fullscreen) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        std::string pos_raw = sysui::hypr_request("cursorpos");
        int cx = 0, cy = 0;
        if (sscanf(pos_raw.c_str(), "%d, %d", &cx, &cy) == 2) {
            cursor_x = cx;
            cursor_y = cy;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

struct AutohideData { GtkWidget* revealer; GtkWidget* window; };

static gboolean check_autohide(gpointer user_data) {
    auto* d = static_cast<AutohideData*>(user_data);
    
    int cx = cursor_x;
    int cy = cursor_y;

    int width = 1920, height = 1080;
    get_monitor_size(width, height);

    int trigger_height    = 40;
    int dock_visible_area = 100 + mag_headroom();
    double center_start   = width  * 0.1;
    double center_end     = width  * 0.9;

    bool revealed = gtk_widget_get_visible(d->window);
    bool mouse_bottom = false;
    if (revealed) {
        mouse_bottom = (cy > height - dock_visible_area);
    } else {
        mouse_bottom = (cy >= height - trigger_height) && (cx > center_start) && (cx < center_end);
    }

    bool ignored = false;
    for (const auto& app : ALWAYS_VISIBLE_APPS) {
        if (g_active_class.find(app) != std::string::npos) { ignored = true; break; }
    }

    // Fullscreen перемагає все: ні whitelist, ні курсор знизу не витягують док поверх гри/відео.
    bool should_show = !g_fullscreen && (mouse_bottom || ignored || g_no_window);

    if (should_show != last_shown) {
        gtk_widget_set_visible(d->window, should_show);
        last_shown = should_show;
    }

    return G_SOURCE_CONTINUE;
}

// ===================== CSS =====================

static const char* CSS_BASE = R"(
* { font-family: "JetBrainsMono Nerd Font", sans-serif; }

window, window.background {
    background-color: transparent;
    background: none;
    box-shadow: none;
}

.dock-wrapper {
    border-radius: 20px;
    padding: 4px 6px; 
    margin-bottom: 8px; 
    border-width: 2px;
    border-style: solid;
    transition: background-color 0.6s cubic-bezier(0.25, 0.1, 0.25, 1),
                border-color 0.6s cubic-bezier(0.25, 0.1, 0.25, 1),
                box-shadow 0.6s cubic-bezier(0.25, 0.1, 0.25, 1);
}

.dock-item {
    background: transparent;
    border: none;
    border-radius: 12px;
    padding: 2px 4px;        
    margin: 0 2px;
    transition: background-color 0.25s cubic-bezier(0.25, 0.1, 0.25, 1),
                transform 0.25s cubic-bezier(0.34, 1.56, 0.64, 1);
}

.dock-item:hover {
    background-color: rgba(255, 255, 255, 0.05);
}

.dock-item:active {
    transform: scale(0.92);
}

.mac-dot {
    min-height: 5px;
    min-width: 5px;
    min-height: 5px;
    min-width: 5px;
    background-color: transparent;
    border-radius: 100%;
    margin-top: 2px;
    margin-bottom: 2px;
    transition: background-color 0.3s cubic-bezier(0.25, 0.1, 0.25, 1),
                box-shadow 0.3s cubic-bezier(0.25, 0.1, 0.25, 1);
}

.running .mac-dot {
    background-color: rgba(255, 255, 255, 0.5);
}

.active .mac-dot {
    background-color: $ACCENT;
    box-shadow: 0 0 5px $ACCENT_A30;
}

.dock-separator {
    border-radius: 2px;
    min-width: 2px;
    margin: 8px 6px;
}

.sys-monitor {
    border-radius: 12px;
    padding: 2px 8px;       
    margin: 0 3px;
}

.sys-text {
    font-size: 14px;        
    font-weight: 800;
    margin-right: 6px;
    transition: color 0.6s cubic-bezier(0.25, 0.1, 0.25, 1);
}

.ws-label {
    font-size: 14px;
    font-weight: 700;
    padding: 1px 6px;
    transition: color 0.6s cubic-bezier(0.25, 0.1, 0.25, 1);
}

window.magnify .dock-item:hover { background-color: transparent; }
.file-thumb { border-radius: 8px; }

.recent { opacity: 0.5; transition: opacity 0.25s cubic-bezier(0.25, 0.1, 0.25, 1); }
.recent:hover { opacity: 1.0; }

scrollbar, scrollbar slider {
    min-width: 0px;
    min-height: 0px;
    background: transparent;
}
)";

static std::string CSS_DARK_TEMPLATE = R"(
.dock-wrapper { background-color: $DOCK_BG; border-color: $DOCK_BORDER; border-width: $DOCK_BW; box-shadow: 0 4px 20px rgba(0,0,0,0.5); }
.dock-separator { background: rgba(255,255,255,0.20); }
.sys-monitor { background: transparent; }
.sys-text { color: $ACCENT; }
.ws-label { color: $ACCENT; }
)";

static std::string CSS_LIGHT_TEMPLATE = R"(
window.light-theme .dock-wrapper { background-color: $DOCK_BG_LIGHT; border-color: $DOCK_BORDER_LIGHT; border-width: $DOCK_BW; box-shadow: 0 4px 20px rgba(0,0,0,0.15); }
window.light-theme .dock-separator { background: rgba(0,0,0,0.15); }
window.light-theme .sys-monitor { background: transparent; }
window.light-theme .sys-text { color: $ACCENT_LIGHT; }
window.light-theme .ws-label { color: $ACCENT_LIGHT; }
window.light-theme .active .mac-dot { background-color: $ACCENT_LIGHT; box-shadow: 0 0 5px $ACCENT_LIGHT_A30; }
)";

// ===================== PYWAL / ТЕМА =====================
struct WalColors {
    std::string color1  = "#9929CF"; 
    std::string color2  = "#C86FA3";
    std::string color5  = "#9929CF"; 
    std::string foreground = "#e0e0e0";
    std::string background = "#0a0a21";
};

static WalColors g_wal;

static std::string parse_css_var(const std::string& line, const std::string& var_name) {
    auto pos = line.find(var_name + ":");
    if (pos == std::string::npos) return "";
    auto start = line.find('#', pos);
    if (start == std::string::npos) {
        start = line.find("rgb", pos);
        if (start == std::string::npos) return "";
        auto end = line.find(';', start);
        if (end == std::string::npos) end = line.size();
        std::string val = line.substr(start, end - start);
        while (!val.empty() && (val.back() == ' ' || val.back() == ';')) val.pop_back();
        return val;
    }
    auto end = line.find(';', start);
    if (end == std::string::npos) end = line.size();
    std::string hex = line.substr(start, end - start);
    while (!hex.empty() && (hex.back() == ' ' || hex.back() == ';' || hex.back() == '\r')) hex.pop_back();
    return hex;
}

static void reload_wal_colors() {
    const char* home = getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/.cache/wal/colors.css";
    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        auto try_var = [&](const std::string& name) { return parse_css_var(line, name); };
        std::string v;
        if (!(v = try_var("--color1")).empty())      g_wal.color1 = v;
        if (!(v = try_var("--color2")).empty())      g_wal.color2 = v;
        if (!(v = try_var("--color5")).empty())      g_wal.color5 = v;
        if (!(v = try_var("--foreground")).empty())  g_wal.foreground = v;
        if (!(v = try_var("--background")).empty())  g_wal.background = v;
    }
}

static std::string hex_to_rgba(const std::string& hex, const std::string& alpha) {
    if (hex.size() < 7 || hex[0] != '#') return "rgba(0,0,0,0)";
    int r = 0, g = 0, b = 0;
    try {
        r = std::stoi(hex.substr(1,2), nullptr, 16);
        g = std::stoi(hex.substr(3,2), nullptr, 16);
        b = std::stoi(hex.substr(5,2), nullptr, 16);
    } catch (...) {}
    return "rgba(" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + "," + alpha + ")";
}

static std::string darken_for_contrast(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return hex;
    int r = std::stoi(hex.substr(1,2), nullptr, 16);
    int g = std::stoi(hex.substr(3,2), nullptr, 16);
    int b = std::stoi(hex.substr(5,2), nullptr, 16);

    double luma = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0;
    double factor = luma > 0.6 ? 0.55 : (luma > 0.4 ? 0.7 : 0.85);

    r = std::clamp((int)std::lround(r * factor), 0, 255);
    g = std::clamp((int)std::lround(g * factor), 0, 255);
    b = std::clamp((int)std::lround(b * factor), 0, 255);

    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return std::string(buf);
}

static std::string build_themed_css() {
    reload_wal_colors();
    std::string accent     = sysui::accent();   // єдиний акцент для всіх модулів
    std::string accent_a30 = hex_to_rgba(accent, "0.30");

    std::string accent_light       = darken_for_contrast(accent);
    std::string accent_light_a30   = hex_to_rgba(accent_light, "0.30");

    auto fill = [&](std::string tmpl) -> std::string {
        auto replace_all = [](std::string s, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return s;
        };
        const auto& st = sysui::settings();
        double bo = st.dock_border_opacity / 100.0, op = st.dock_opacity / 100.0;
        tmpl = replace_all(tmpl, "$DOCK_BG_LIGHT", sysui::rgba("#ffffff", std::min(1.0, op * 0.9)));
        // "subtle" — тонкий напівпрозорий контур як у топбару та Control Center; "accent" — товста акцентна рамка
        bool subtle = st.dock_border_style != "accent";
        tmpl = replace_all(tmpl, "$DOCK_BORDER_LIGHT", subtle ? sysui::rgba("#000000", 0.10 * bo) : sysui::rgba(accent_light, bo));
        tmpl = replace_all(tmpl, "$DOCK_BG", sysui::rgba("#1e1e1e", op));
        tmpl = replace_all(tmpl, "$DOCK_BORDER", subtle ? sysui::rgba("#ffffff", 0.10 * bo) : sysui::rgba(accent, bo));
        tmpl = replace_all(tmpl, "$DOCK_BW", !st.dock_border ? "0px" : (subtle ? "1px" : "2px"));
        tmpl = replace_all(tmpl, "$ACCENT_LIGHT_A30", accent_light_a30);
        tmpl = replace_all(tmpl, "$ACCENT_LIGHT",      accent_light);
        tmpl = replace_all(tmpl, "$ACCENT_A30", accent_a30);
        tmpl = replace_all(tmpl, "$ACCENT",     accent);
        return tmpl;
    };

    return fill(std::string(CSS_BASE)) + fill(CSS_DARK_TEMPLATE) + fill(CSS_LIGHT_TEMPLATE);
}

static GtkCssProvider* g_css_provider = nullptr;
static GtkWidget* g_ws_label = nullptr;
static GtkWidget* g_window       = nullptr;  

static void apply_css() {
    std::string css = build_themed_css();
    gtk_css_provider_load_from_string(g_css_provider, css.c_str());
}

static time_t file_mtime(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
}

// ===================== ЗМІНА ТЕМИ =====================
struct ThemeWatchData { GtkWidget* window; };

static gboolean check_theme(gpointer user_data) {
    auto* d = static_cast<ThemeWatchData*>(user_data);
    // Раніше: popen("gsettings get ...") кожні 2 с. Тепер читаємо GSettings напряму (без fork).
    std::string scheme = sysui::color_scheme();

    bool is_light = (scheme != "prefer-dark");
    static bool last_light = false;
    static bool first_run  = true;

    if (first_run || is_light != last_light) {
        if (is_light) gtk_widget_add_css_class(d->window, "light-theme");
        else gtk_widget_remove_css_class(d->window, "light-theme");
        last_light = is_light;
        first_run  = false;
    }

    static time_t settings_mtime = -1;
    std::string settings_path = std::string(getenv("HOME")) + "/.config/megumin-os/settings.json";
    time_t sm = file_mtime(settings_path);
    if (sm == settings_mtime) return G_SOURCE_CONTINUE;  // settings.json не змінювався
    settings_mtime = sm;

    try {
        std::ifstream f(settings_path);
        if (f.good()) {
            json s;
            f >> s;
            if (s.contains("dock_whitelist") && s["dock_whitelist"].is_string()) {
                std::string w_str = s["dock_whitelist"].get<std::string>();
                std::vector<std::string> new_whitelist;
                size_t pos = 0;
                while ((pos = w_str.find(',')) != std::string::npos) {
                    std::string app = w_str.substr(0, pos);
                    app.erase(0, app.find_first_not_of(" \t\r\n"));
                    app.erase(app.find_last_not_of(" \t\r\n") + 1);
                    if (!app.empty()) new_whitelist.push_back(to_lower(app));
                    w_str.erase(0, pos + 1);
                }
                w_str.erase(0, w_str.find_first_not_of(" \t\r\n"));
                w_str.erase(w_str.find_last_not_of(" \t\r\n") + 1);
                if (!w_str.empty()) new_whitelist.push_back(to_lower(w_str));
                
                if (!new_whitelist.empty()) ALWAYS_VISIBLE_APPS = new_whitelist;
            }
        }
    } catch (...) {}

    return G_SOURCE_CONTINUE;
}

// Раніше CSS повністю перебудовувався і перепарсювався КОЖНІ 2 секунди (+ запис у /tmp),
// що постійно роздувало heap GTK. Тепер — лише коли colors.css реально змінився.
static GtkWidget* g_outer = nullptr;
static gboolean update_dock(gpointer user_data);
static void files_schedule();

// Тема pywal або settings.json змінились → застосовуємо наживо.
static gboolean reload_wal_timer(gpointer) {
    static long long last = -1;
    long long st = sysui::theme_stamp();
    if (st == last) return G_SOURCE_CONTINUE;
    last = st;
    apply_css();
    files_schedule();
    if (g_outer) gtk_widget_set_margin_top(g_outer, mag_headroom());
    if (g_window) {
        if (mag_max() > 1.0) gtk_widget_add_css_class(g_window, "magnify");
        else gtk_widget_remove_css_class(g_window, "magnify");
    }
    int want = sysui::settings().dock_icon_size;
    if (want != g_icon_size) {
        // Розмір іконок змінився — перебудовуємо всі елементи доку
        g_icon_size = want;
        if (g_dock_state) {
            for (auto& [cls, w] : g_dock_state->current_items) gtk_box_remove(GTK_BOX(g_dock_state->dock_box), w);
            g_dock_state->current_items.clear();
            g_dock_state->last_recent_sig = "\x01";
            update_dock(g_dock_state);
        }
    }
    return G_SOURCE_CONTINUE;
}

static GtkWidget* make_sep() {
    GtkWidget* s = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(s, "dock-separator");
    gtk_widget_set_size_request(s, 2, -1);
    gtk_widget_set_valign(s, GTK_ALIGN_FILL);
    return s;
}

// ===================== СКРОЛ ДОКБАРУ КОЛЕСОМ МИШІ =====================
static gboolean on_dock_scroll(GtkEventControllerScroll*, double dx, double dy, gpointer user_data) {
    GtkScrolledWindow* scrolled = GTK_SCROLLED_WINDOW(user_data);
    GtkAdjustment* hadj = gtk_scrolled_window_get_hadjustment(scrolled);
    if (!hadj) return FALSE;

    double delta = std::fabs(dy) > std::fabs(dx) ? dy : dx;
    if (delta == 0.0) return FALSE;

    const double SCROLL_STEP = 60.0;
    double lower = gtk_adjustment_get_lower(hadj);
    double upper = gtk_adjustment_get_upper(hadj) - gtk_adjustment_get_page_size(hadj);
    double new_value = gtk_adjustment_get_value(hadj) + delta * SCROLL_STEP;
    new_value = std::max(lower, std::min(new_value, upper));
    gtk_adjustment_set_value(hadj, new_value);

    return TRUE;
}

// ===================== НЕДАВНІ ФАЙЛИ (завантаження / скопійовані) =====================
// Біля індикатора воркспейсу — останні завантажені або скопійовані файли.
//  ЛКМ — відкрити, СКМ — показати в теці, перетягування — кинути файл у будь-яку програму.
// Оновлення без опитування: GFileMonitor стежить за ~/Downloads і базою cliphist.
static GtkWidget* g_files_box = nullptr;
static GtkWidget* g_files_sep = nullptr;
static guint g_files_pending = 0;
static GFileMonitor* g_dl_monitor = nullptr;
static GFileMonitor* g_clip_monitor = nullptr;

struct RecentFile { std::string path; gint64 t; bool copied; };

static std::string downloads_dir() {
    const char* d = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    std::string home = sysui::home();
    // Без налаштованих XDG-тек GLib повертає домашню теку — тоді беремо ~/Downloads
    if (!d || home == d || std::string(d) == home + "/") return home + "/Downloads";
    return d;
}

static bool is_image_ext(const std::string& p) {
    std::string e = to_lower(p.substr(p.find_last_of('.') + 1));
    return e == "png" || e == "jpg" || e == "jpeg" || e == "webp" || e == "gif" || e == "bmp";
}

static GtkWidget* file_icon(const std::string& path, int size) {
    GtkWidget* img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), size);
    int px = size * 2;   // запас на HiDPI
    GdkTexture* t = nullptr;
    if (is_image_ext(path)) t = sysui::load_texture_cover(path.c_str(), px, px);
    if (!t) {
        // мініатюра з кешу freedesktop (є для відео/PDF, які вже показував файловий менеджер)
        gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
        if (uri) {
            gchar* md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
            for (const char* sz : {"large", "normal"}) {
                std::string th = sysui::home() + "/.cache/thumbnails/" + sz + "/" + md5 + ".png";
                if (access(th.c_str(), R_OK) == 0) { t = sysui::load_texture_cover(th.c_str(), px, px); if (t) break; }
            }
            g_free(md5);
            g_free(uri);
        }
    }
    if (t) { gtk_image_set_from_paintable(GTK_IMAGE(img), GDK_PAINTABLE(t)); g_object_unref(t); gtk_widget_add_css_class(img, "file-thumb"); gtk_widget_set_overflow(img, GTK_OVERFLOW_HIDDEN); }
    else {
        GFile* f = g_file_new_for_path(path.c_str());
        GFileInfo* info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_ICON, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
        if (info && g_file_info_get_icon(info)) gtk_image_set_from_gicon(GTK_IMAGE(img), g_file_info_get_icon(info));
        else gtk_image_set_from_icon_name(GTK_IMAGE(img), "text-x-generic");
        if (info) g_object_unref(info);
        g_object_unref(f);
    }
    gtk_widget_add_css_class(img, "dock-icon");
    return img;
}

static std::string ago(gint64 t) {
    gint64 d = (gint64)time(nullptr) - t;
    if (d < 60) return "щойно";
    if (d < 3600) return std::to_string(d / 60) + " хв тому";
    if (d < 86400) return std::to_string(d / 3600) + " год тому";
    return std::to_string(d / 86400) + " дн тому";
}

static void files_render(std::vector<RecentFile> files) {
    if (!g_files_box) return;
    while (GtkWidget* c = gtk_widget_get_first_child(g_files_box)) gtk_box_remove(GTK_BOX(g_files_box), c);
    for (auto& f : files) {
        GtkWidget* btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "dock-item");
        gtk_widget_add_css_class(btn, "file-item");
        mag_attach(btn);
        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(vbox), file_icon(f.path, g_icon_size));
        GtkWidget* dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);   // та сама висота, що й у програм
        gtk_widget_add_css_class(dot, "mac-dot");
        gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(vbox), dot);
        gtk_button_set_child(GTK_BUTTON(btn), vbox);
        std::string name = f.path.substr(f.path.find_last_of('/') + 1);
        std::string tip = name + "\n" + (f.copied ? "Скопійовано" : "Завантажено · " + ago(f.t)) + "\nЛКМ — відкрити · СКМ — показати в теці · можна перетягнути";
        gtk_widget_set_tooltip_text(btn, tip.c_str());
        gtk_widget_set_cursor_from_name(btn, "pointer");
        g_object_set_data_full(G_OBJECT(btn), "path", g_strdup(f.path.c_str()), g_free);

        g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
            gchar* uri = g_filename_to_uri((const char*)g_object_get_data(G_OBJECT(b), "path"), nullptr, nullptr);
            if (uri) { g_app_info_launch_default_for_uri_async(uri, nullptr, nullptr, nullptr, nullptr); g_free(uri); }
        }), nullptr);
        GtkGesture* mid = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mid), 2);
        g_signal_connect(mid, "released", G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer b) {
            const char* p = (const char*)g_object_get_data(G_OBJECT(b), "path");
            GSubprocess* sp = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, nullptr, "nautilus", "--select", p, nullptr);
            if (sp) g_object_unref(sp);
        }), btn);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(mid));
        // Перетягування файлу в іншу програму (браузер, Telegram, термінал…)
        GtkDragSource* drag = gtk_drag_source_new();
        g_signal_connect(drag, "prepare", G_CALLBACK(+[](GtkDragSource*, double, double, gpointer b) -> GdkContentProvider* {
            GFile* f = g_file_new_for_path((const char*)g_object_get_data(G_OBJECT(b), "path"));
            GSList* l = g_slist_append(nullptr, f);
            GdkFileList* fl = gdk_file_list_new_from_list(l);
            GdkContentProvider* cp = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, fl);
            g_slist_free(l);
            g_object_unref(f);
            return cp;
        }), btn);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(drag));
        gtk_box_append(GTK_BOX(g_files_box), btn);
    }
    bool any = !files.empty();
    gtk_widget_set_visible(g_files_box, any);
    gtk_widget_set_visible(g_files_sep, any);
    sysui::trim();
}

static std::vector<RecentFile> scan_downloads() {
    std::vector<RecentFile> r;
    const auto& st = sysui::settings();
    std::string dir = downloads_dir();
    DIR* d = opendir(dir.c_str());
    if (!d) return r;
    gint64 now = time(nullptr), limit = (gint64)st.dock_files_days * 86400;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        std::string ln = to_lower(n);
        // недокачані файли браузерів
        if (ln.size() > 5 && (ln.rfind(".part") == ln.size() - 5 || ln.rfind(".crdownload") == ln.size() - 11 ||
                              ln.rfind(".tmp") == ln.size() - 4 || ln.rfind(".download") == ln.size() - 9)) continue;
        std::string p = dir + "/" + n;
        struct stat sb;
        if (stat(p.c_str(), &sb) != 0) continue;
        if (now - sb.st_mtime > limit) continue;
        r.push_back({p, (gint64)sb.st_mtime, false});
    }
    closedir(d);
    return r;
}

static void files_refresh_now() {
    const auto& st = sysui::settings();
    if (!g_files_box) return;
    if (!st.dock_files) { files_render({}); return; }
    std::vector<RecentFile> dl;
    if (st.dock_files_source != "clipboard") dl = scan_downloads();
    if (st.dock_files_source == "downloads") {
        std::sort(dl.begin(), dl.end(), [](auto& a, auto& b) { return a.t > b.t; });
        if ((int)dl.size() > st.dock_files_count) dl.resize(st.dock_files_count);
        files_render(dl);
        return;
    }
    // Скопійовані файли — з історії cliphist (асинхронно, без блокування доку)
    auto* carry = new std::vector<RecentFile>(std::move(dl));
    GSubprocess* sp = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr,
                                       "sh", "-c", "cliphist list 2>/dev/null | head -n 60", nullptr);
    if (!sp) { files_render(*carry); delete carry; return; }
    g_subprocess_communicate_utf8_async(sp, nullptr, nullptr, [](GObject* src, GAsyncResult* res, gpointer d) {
        auto* files = static_cast<std::vector<RecentFile>*>(d);
        char* out = nullptr;
        g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, nullptr, nullptr);
        g_object_unref(src);
        std::istringstream ss(out ? out : "");
        g_free(out);
        std::string line;
        gint64 now = time(nullptr);
        int idx = 0;
        while (std::getline(ss, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string v = line.substr(tab + 1);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            std::string path;
            if (v.rfind("file://", 0) == 0) {
                gchar* fn = g_filename_from_uri(v.substr(0, v.find_first_of(" \n")).c_str(), nullptr, nullptr);
                if (fn) { path = fn; g_free(fn); }
            }   // звичайний текст зі шляхом (скопійований з чату/терміналу) — не файл, пропускаємо
            if (path.empty() || access(path.c_str(), F_OK) != 0) continue;
            // cliphist іде від найновішого — даємо «свіжість» за порядком
            files->push_back({path, now + 1000 - idx++, true});
        }
        const auto& st = sysui::settings();
        std::sort(files->begin(), files->end(), [](auto& a, auto& b) { return a.t > b.t; });
        std::vector<RecentFile> uniq;
        std::set<std::string> seen;
        for (auto& f : *files) {
            if (seen.count(f.path)) continue;
            seen.insert(f.path);
            if (f.copied) f.t = now;
            uniq.push_back(f);
            if ((int)uniq.size() >= st.dock_files_count) break;
        }
        files_render(uniq);
        delete files;
    }, carry);
}

static void files_schedule() {
    if (g_files_pending) return;
    g_files_pending = g_timeout_add(600, [](gpointer) -> gboolean {
        g_files_pending = 0;
        files_refresh_now();
        return G_SOURCE_REMOVE;
    }, nullptr);
}

static void files_init() {
    auto on_change = +[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer) { files_schedule(); };
    GFile* dl = g_file_new_for_path(downloads_dir().c_str());
    g_dl_monitor = g_file_monitor_directory(dl, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    if (g_dl_monitor) g_signal_connect(g_dl_monitor, "changed", G_CALLBACK(on_change), nullptr);
    g_object_unref(dl);
    GFile* db = g_file_new_for_path((sysui::home() + "/.cache/cliphist/db").c_str());
    g_clip_monitor = g_file_monitor_file(db, G_FILE_MONITOR_NONE, nullptr, nullptr);
    if (g_clip_monitor) g_signal_connect(g_clip_monitor, "changed", G_CALLBACK(on_change), nullptr);
    g_object_unref(db);
    // раз на хвилину — щоб «N хв тому» і ліміт днів були актуальні
    g_timeout_add_seconds(60, [](gpointer) -> gboolean { files_schedule(); return G_SOURCE_CONTINUE; }, nullptr);
    files_refresh_now();
}

// ===================== ACTIVATE =====================
static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    load_history();

    for (const auto& snap : load_open_snapshot()) {
        bool already = false;
        for (auto& r : recent_apps) if (r.id == snap.id) { already = true; break; }
        if (!already) recent_apps.insert(recent_apps.begin(), snap);
    }
    if ((int)recent_apps.size() > MAX_RECENT_APPS) recent_apps.resize(MAX_RECENT_APPS);
    save_history();

    g_css_provider = gtk_css_provider_new();
    apply_css();
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(g_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    GtkWidget* window = gtk_application_window_new(app);
    g_window = window;
    gtk_widget_set_name(window, "dock");

    // Нерозтягуване вікно завжди дорівнює вмісту — інакше після зменшення доку лишались
    // прозорі краї, які перехоплювали кліки внизу екрана.
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_namespace(GTK_WINDOW(window), "dock");
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_TOP);
    
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, 0); 
    gtk_layer_set_exclusive_zone(GTK_WINDOW(window), 0); 

    GtkWidget* apps_scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(apps_scrolled), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(apps_scrolled), 1300); 
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(apps_scrolled), TRUE);

    GtkWidget* apps_container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    
    GtkWidget* dock_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dock_box, "dock-content");
    gtk_widget_set_valign(dock_box, GTK_ALIGN_END);

    GtkWidget* recent_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(recent_box, "dock-content");
    gtk_widget_set_valign(recent_box, GTK_ALIGN_END);
    gtk_widget_set_visible(recent_box, FALSE);

    // Власний layout для збільшення іконок
    gtk_widget_set_layout_manager(dock_box, gtk_custom_layout_new(nullptr, mag_measure, mag_allocate));
    gtk_widget_set_layout_manager(recent_box, gtk_custom_layout_new(nullptr, mag_measure, mag_allocate));
    g_mag_boxes[0] = dock_box;
    g_mag_boxes[1] = recent_box;
    gtk_box_append(GTK_BOX(apps_container), dock_box);
    gtk_box_append(GTK_BOX(apps_container), recent_box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(apps_scrolled), apps_container);
    // Збільшені іконки малюються вище за межі доку — нічого не повинно їх обрізати
    gtk_widget_set_overflow(apps_scrolled, GTK_OVERFLOW_VISIBLE);
    if (GtkWidget* vp = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(apps_scrolled))) gtk_widget_set_overflow(vp, GTK_OVERFLOW_VISIBLE);

    GtkWidget* ws_label = gtk_label_new(" 1");
    gtk_widget_add_css_class(ws_label, "ws-label");
    GtkWidget* ws_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(ws_box, "sys-monitor");
    gtk_widget_set_valign(ws_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(ws_box), ws_label);

    GtkWidget* cpu_label = gtk_label_new(" 0%");
    GtkWidget* ram_label = gtk_label_new(" 0%");
    gtk_widget_add_css_class(cpu_label, "sys-text");
    gtk_widget_add_css_class(ram_label, "sys-text");
    GtkWidget* sys_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(sys_box, "sys-monitor");
    gtk_widget_set_valign(sys_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(sys_box), cpu_label);
    gtk_box_append(GTK_BOX(sys_box), ram_label);

    GtkWidget* wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(wrapper, "dock-wrapper");
    gtk_widget_set_valign(wrapper, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(wrapper), apps_scrolled);
    gtk_box_append(GTK_BOX(wrapper), make_sep());
    gtk_box_append(GTK_BOX(wrapper), ws_box);
    // Недавні файли — біля індикатора воркспейсу
    g_files_sep = make_sep();
    g_files_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(g_files_box, GTK_ALIGN_END);
    gtk_widget_set_layout_manager(g_files_box, gtk_custom_layout_new(nullptr, mag_measure, mag_allocate));
    gtk_widget_set_overflow(g_files_box, GTK_OVERFLOW_VISIBLE);
    g_mag_boxes[2] = g_files_box;
    gtk_widget_set_visible(g_files_sep, FALSE);
    gtk_widget_set_visible(g_files_box, FALSE);
    gtk_box_append(GTK_BOX(wrapper), g_files_sep);
    gtk_box_append(GTK_BOX(wrapper), g_files_box);
    gtk_box_append(GTK_BOX(wrapper), make_sep());
    gtk_box_append(GTK_BOX(wrapper), sys_box);

    GtkEventController* scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(scroll_controller, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_dock_scroll), apps_scrolled);
    gtk_widget_add_controller(wrapper, scroll_controller);

    GtkWidget* revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_NONE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 0);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
    gtk_revealer_set_child(GTK_REVEALER(revealer), wrapper);

    gtk_widget_set_overflow(revealer, GTK_OVERFLOW_VISIBLE);
    gtk_widget_set_overflow(wrapper, GTK_OVERFLOW_VISIBLE);
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    g_outer = outer;
    gtk_widget_set_valign(outer, GTK_ALIGN_END);
    g_icon_size = sysui::settings().dock_icon_size;
    gtk_widget_set_margin_top(outer, mag_headroom());   // місце над доком для збільшених іконок

    // Курсор над доком → збільшення
    g_mag_window = window;
    GtkEventController* mag_motion = gtk_event_controller_motion_new();
    g_signal_connect(mag_motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double x, double y, gpointer) {
        g_pointer_in = true; g_px = x; g_py = y; mag_kick();
    }), nullptr);
    g_signal_connect(mag_motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer) {
        g_pointer_in = false; mag_kick();
    }), nullptr);
    gtk_widget_add_controller(window, mag_motion);
    g_signal_connect(window, "unmap", G_CALLBACK(+[](GtkWidget*, gpointer) { mag_reset(); }), nullptr);
    g_signal_connect(window, "map", G_CALLBACK(+[](GtkWidget*, gpointer) { mag_reset(); }), nullptr);
    gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(outer), revealer);

    GtkWidget* dummy = gtk_drawing_area_new();
    gtk_widget_set_size_request(dummy, 1, 1);
    gtk_box_append(GTK_BOX(outer), dummy);

    gtk_window_set_child(GTK_WINDOW(window), outer);

    auto* state = new DockState();
    state->dock_box   = dock_box;
    state->recent_box = recent_box;
    state->ws_label   = ws_label;
    state->cpu_label  = cpu_label;
    state->ram_label  = ram_label;
    state->revealer   = revealer;
    state->window     = window;
    g_dock_state = state;
    g_ws_label = ws_label;

    update_dock(state);
    update_workspace(ws_label);
    files_init();

    auto* sw = new SysWidgets{cpu_label, ram_label};
    update_sysmon(sw);
    g_timeout_add(2000, update_sysmon, sw);

    auto* ahd = new AutohideData{revealer, window};
    g_timeout_add(150, check_autohide, ahd);

    auto* twd = new ThemeWatchData{window};
    check_theme(twd);  
    g_timeout_add(2000, check_theme, twd);

    reload_wal_timer(nullptr);
    g_timeout_add(2000, reload_wal_timer, nullptr);

    gtk_window_present(GTK_WINDOW(window));
}

// ===================== MAIN =====================

void listen_hyprland_ipc_dock() {
    std::string xdg = getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "";
    std::string sig = getenv("HYPRLAND_INSTANCE_SIGNATURE") ? getenv("HYPRLAND_INSTANCE_SIGNATURE") : "";
    if (xdg.empty() || sig.empty()) return;
    
    std::string socket_path = xdg + "/hypr/" + sig + "/.socket2.sock";
    
    while (true) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) { sleep(1); continue; }
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            sleep(1);
            continue;
        }
        
        char buffer[1024];
        std::string leftover;
        while (true) {
            int bytes = read(sock, buffer, sizeof(buffer) - 1);
            if (bytes <= 0) break; // Disconnected
            
            buffer[bytes] = '\0';
            std::string data = leftover + buffer;
            leftover = "";
            
            size_t pos = 0;
            while ((pos = data.find('\n')) != std::string::npos) {
                std::string line = data.substr(0, pos);
                data.erase(0, pos + 1);
                
                if (line.find("workspace>>") == 0 || line.find("activewindow>>") == 0 || line.find("closewindow>>") == 0 || line.find("openwindow>>") == 0 || line.find("fullscreen>>") == 0) {
                    g_idle_add([](gpointer) -> gboolean {
                        if (g_dock_state) update_dock(g_dock_state);
                        if (g_ws_label) update_workspace(g_ws_label);
                        return G_SOURCE_REMOVE;
                    }, nullptr);
                }
            }
            leftover = data;
        }
        close(sock);
        sleep(1);
    }
}

int main(int argc, char** argv) {
    // Ігноруємо SIGPIPE щоб не крашитись від дочірніх процесів
    sysui::mem_init();
    signal(SIGPIPE, SIG_IGN);

    // Завантажуємо всі .desktop файли один раз при старті
    load_desktop_db();

    std::thread ipc_thread(listen_hyprland_ipc_dock);
    ipc_thread.detach();
    
    std::thread cursor_thread(poll_cursor_thread);
    cursor_thread.detach();

    GtkApplication* app = gtk_application_new("com.megumin.dock", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}