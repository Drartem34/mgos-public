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
    bool        minimized = false;   // усі вікна застосунку сховані на док (special:mgos-dock)
    std::string min_address;         // яке з них повертати кліком
};
static const char* DOCK_STASH = "special:mgos-dock";   // прихований стіл для «сховано на док»

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

static std::string make_displayable(const std::string& icon);
static std::string find_icon_for_class(const std::string& wm_class);
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
            if (!make_displayable(icon).empty()) icon = make_displayable(icon);
            else icon = find_icon_for_class(id);   // збережена назва не малюється — шукаємо заново
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
    std::string name;       // Name=, lower
    std::string exec_base;  // basename першого слова Exec=, lower (для збігу з /proc/<pid>/exe)
    std::string wm_class;   // StartupWMClass або WMClass
    std::string filename;   // basename без .desktop, lower
};

static std::vector<DesktopEntry> g_desktop_db;
static bool g_desktop_db_loaded = false;

static void load_desktop_db() {
    if (g_desktop_db_loaded) return;
    g_desktop_db_loaded = true;

    const char* home = getenv("HOME");
    std::string h = home ? home : "";
    // усі місця, де живуть .desktop: користувач, Flatpak, XDG_DATA_DIRS (Nix, snap, /opt-пакети тощо)
    std::vector<std::string> dirs = {h + "/.local/share/applications",
                                     h + "/.local/share/flatpak/exports/share/applications",
                                     "/var/lib/flatpak/exports/share/applications", "/var/lib/snapd/desktop/applications"};
    const char* xdd = getenv("XDG_DATA_DIRS");
    std::stringstream xs(xdd && *xdd ? xdd : "/usr/local/share:/usr/share");
    for (std::string d; std::getline(xs, d, ':');) if (!d.empty()) dirs.push_back(d + "/applications");
    std::sort(dirs.begin(), dirs.end()); dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());

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
                } else if (line.rfind("Name=", 0) == 0) {
                    entry.name = to_lower(line.substr(5));
                } else if (line.rfind("Exec=", 0) == 0) {
                    entry.exec = line.substr(5);
                    {   // basename виконуваного файлу: «"/opt/x/app" %U» → app; «env A=1 app» → app
                        std::stringstream es(entry.exec); std::string tok;
                        while (es >> tok) {
                            if (tok == "env" || tok.find('=') != std::string::npos) continue;
                            tok.erase(std::remove(tok.begin(), tok.end(), '"'), tok.end());
                            entry.exec_base = to_lower(tok.substr(tok.find_last_of('/') + 1));
                            break;
                        }
                    }
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

// ---------- іконки з нестандартних місць ----------
static std::map<std::string, int> g_cls_pid;   // клас → pid вікна (заповнює update_dock)

static bool icon_displayable(const std::string& icon) {
    if (icon.empty()) return false;
    if (icon[0] == '/') return access(icon.c_str(), R_OK) == 0;
    GdkDisplay* dpy = gdk_display_get_default();
    return dpy && gtk_icon_theme_has_icon(gtk_icon_theme_get_for_display(dpy), icon.c_str());
}

static bool is_img(const std::string& n) {
    std::string l = to_lower(n);
    for (const char* e : {".png", ".svg", ".xpm", ".ico", ".jpg"})
        if (l.size() > strlen(e) && l.compare(l.size() - strlen(e), strlen(e), e) == 0) return true;
    return false;
}

// Шукає файл іконки в теці (без рекурсії): точна назва, потім icon/logo/app/назва-програми
static std::string icon_in_dir(const std::string& dir, const std::vector<std::string>& names) {
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::string best; int best_rank = 99;
    while (dirent* e = readdir(d)) {
        std::string f = e->d_name;
        if (!is_img(f)) continue;
        std::string stem = to_lower(f.substr(0, f.find_last_of('.')));
        int rank = 99;
        for (size_t i = 0; i < names.size(); i++) if (!names[i].empty() && stem == names[i]) { rank = (int)i; break; }
        if (rank == 99 && (stem == "icon" || stem == "app" || stem == "logo")) rank = 50;
        if (rank == 99 && (stem.find("icon") != std::string::npos || stem.find("logo") != std::string::npos)) rank = 60;
        // svg/png краще за ico/xpm/jpg
        if (rank < 99 && !(f.size() > 4 && (to_lower(f.substr(f.size() - 4)) == ".png" || to_lower(f.substr(f.size() - 4)) == ".svg"))) rank += 5;
        if (rank < best_rank) { best_rank = rank; best = dir + "/" + f; }
    }
    closedir(d);
    return best;
}

// Ім'я іконки, якого немає в темі (Icon=foo, а файл десь у hicolor/pixmaps/теці програми) → шлях
static std::string icon_file_for_name(const std::string& name) {
    const char* home = getenv("HOME");
    std::string h = home ? home : "";
    std::vector<std::string> bases = {h + "/.local/share/icons/hicolor", "/usr/share/icons/hicolor", h + "/.icons",
                                      "/usr/local/share/icons/hicolor", h + "/.local/share/pixmaps", "/usr/share/pixmaps"};
    for (const char* sz : {"scalable", "512x512", "256x256", "128x128", "96x96", "64x64", "48x48", "32x32"})
        for (const auto& b : bases) {
            std::string r = icon_in_dir(b + "/" + sz + "/apps", {to_lower(name)});
            if (!r.empty() && to_lower(r).find("/" + to_lower(name) + ".") != std::string::npos) return r;
        }
    for (const auto& b : bases) {
        std::string r = icon_in_dir(b, {to_lower(name)});
        if (!r.empty() && to_lower(r).find("/" + to_lower(name) + ".") != std::string::npos) return r;
    }
    return "";
}

// Іконка поруч із програмою: /opt/App/app, AppImage, Electron (resources/app.png), …/share/icons
static std::string icon_near_exe(const std::string& exe, const std::vector<std::string>& names) {
    if (exe.empty() || exe[0] != '/') return "";
    std::string dir = exe.substr(0, exe.find_last_of('/'));
    if (dir == "/usr/bin" || dir == "/bin" || dir == "/usr/local/bin" || dir == "/usr/sbin") return "";   // там іконок немає
    for (int up = 0; up < 3 && dir.size() > 1; up++) {
        for (const std::string sub : {"", "/resources", "/icons", "/share/pixmaps", "/share/icons/hicolor/512x512/apps",
                                      "/share/icons/hicolor/256x256/apps", "/share/icons/hicolor/128x128/apps",
                                      "/share/icons/hicolor/scalable/apps", "/usr/share/pixmaps", "/usr/share/icons/hicolor/256x256/apps"}) {
            std::string r = icon_in_dir(dir + sub, names);
            if (!r.empty()) return r;
        }
        dir = dir.substr(0, dir.find_last_of('/'));
    }
    return "";
}

static std::string exe_of_pid(int pid) {
    if (pid <= 0) return "";
    char buf[4096]; ssize_t n = readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = 0;
    std::string e = buf;
    auto del = e.find(" (deleted)"); if (del != std::string::npos) e.erase(del);
    // AppImage: справжній файл — $APPIMAGE, а exe вказує в тимчасовий /tmp/.mount_*
    return e;
}

// Будь-яка «назва» іконки → те, що GTK точно намалює (ім'я з теми або існуючий файл), або ""
static std::string make_displayable(const std::string& icon) {
    if (icon_displayable(icon)) return icon;
    if (icon.empty() || icon[0] == '/') return "";
    std::string f = icon_file_for_name(icon);
    return f;
}

static std::string find_icon_for_class(const std::string& wm_class);
static std::string find_icon_for_class_raw(const std::string& wm_class);

static std::string find_icon_for_class(const std::string& wm_class) {
    std::string cls = to_lower(wm_class);
    if (icon_cache.count(cls)) return icon_cache[cls];
    std::string r = make_displayable(find_icon_for_class_raw(cls));
    int pid = g_cls_pid.count(cls) ? g_cls_pid[cls] : 0;
    std::string exe = exe_of_pid(pid);
    std::string exe_base = to_lower(exe.substr(exe.find_last_of('/') + 1));
    if (r.empty() && !exe_base.empty()) {
        // .desktop, у якого Exec — цей самий файл (вікно з власним класом: kitty --class X → kitty)
        load_desktop_db();
        for (const auto& e : g_desktop_db)
            if (!e.exec_base.empty() && e.exec_base == exe_base && !e.icon.empty()) {
                r = make_displayable(e.icon); if (!r.empty()) break;
            }
    }
    if (r.empty() && !exe.empty()) r = icon_near_exe(exe, {cls, exe_base, cls.substr(cls.find_last_of('.') + 1)});
    if (r.empty() && !exe_base.empty()) r = make_displayable(exe_base);
    if (r.empty()) r = "application-x-executable";   // не «перекреслена» іконка, а звичайна «програма»
    // без pid ще можемо дізнатись більше пізніше — не кешуємо «запасну» назавжди
    if (pid > 0 || r != "application-x-executable") icon_cache[cls] = r;
    return r;
}

static std::string find_icon_for_class_raw(const std::string& wm_class) {
    std::string cls = to_lower(wm_class);

    // Steam ігри: steam_app_730 -> шукаємо іконку гри
    if (cls.rfind("steam_app_", 0) == 0) {
        std::string app_id = cls.substr(10);
        const char* home = getenv("HOME");
        std::string home_s = home ? home : "";

        static const std::vector<std::string> sizes = {"256x256","128x128","64x64","48x48","32x32","16x16"};
        for (const auto& sz : sizes) {
            std::string p = home_s + "/.local/share/icons/hicolor/" + sz + "/apps/steam_icon_" + app_id + ".png";
            if (access(p.c_str(), R_OK) == 0) { 
                return p; 
            }
        }
        // 2. Steam librarycache (завантажується при встановленні гри)
        for (const auto& ext : {".jpg", ".png"}) {
            std::string p = home_s + "/.local/share/Steam/appcache/librarycache/" + app_id + "_icon" + ext;
            if (access(p.c_str(), R_OK) == 0) return p;
        }
        // 3. Symbolic fallback (GTK знайде якщо є в темі)
        std::string icon_name = "steam_icon_" + app_id;
        return icon_name;
    }

    load_desktop_db();

    // Прохід 1: по імені файлу
    for (const auto& e : g_desktop_db) {
        if (e.filename.find(cls) != std::string::npos || cls.find(e.filename) != std::string::npos) {
            if (!e.icon.empty()) return e.icon;
        }
    }
    // Прохід 2: по StartupWMClass
    for (const auto& e : g_desktop_db) {
        if (!e.wm_class.empty()) {
            if (e.wm_class == cls || e.wm_class.find(cls) != std::string::npos || cls.find(e.wm_class) != std::string::npos) {
                if (!e.icon.empty()) return e.icon;
            }
        }
    }

    // Прохід 3: Name= (вікно «Antigravity» ↔ Name=Antigravity) і basename Exec
    for (const auto& e : g_desktop_db)
        if (!e.icon.empty() && ((!e.name.empty() && e.name == cls) || (!e.exec_base.empty() && e.exec_base == cls))) return e.icon;
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
static bool stash_is_open();   // Сховище відкрите → док завжди видно (визначено нижче)

// ===================== ІКОНКА =====================

static GtkWidget* make_icon(const std::string& icon_name, int size) {
    GtkWidget* img;
    // Якщо іконка — це шлях до файлу (починається з /) — використовуємо GtkPicture для надійного масштабування
    if (!icon_name.empty() && icon_name[0] == '/') {
        img = gtk_picture_new_for_filename(icon_name.c_str());
        gtk_widget_set_size_request(img, size, size);
    } else {
        img = gtk_image_new_from_icon_name(icon_displayable(icon_name) ? icon_name.c_str() : "application-x-executable");
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

    // сховано на док → повертаємо вікно на поточний стіл
    const char* min_addr = (const char*)g_object_get_data(G_OBJECT(btn), "min-address");
    if (min_addr && *min_addr) {
        int ws = 1;
        try { ws = json::parse(sysui::hypr_request("j/activeworkspace")).value("id", 1); } catch (...) {}
        sysui::hypr_request("dispatch movetoworkspacesilent " + std::to_string(ws) + ",address:" + min_addr);
        sysui::hypr_request(std::string("dispatch focuswindow address:") + min_addr);
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
        if (!g_cls_pid.count(cls)) g_cls_pid[cls] = pid;
        bool stashed = w.value("workspace", json::object()).value("name", "") == DOCK_STASH;

        if (new_apps.find(cls) == new_apps.end()) {
            AppInfo info;
            info.id        = cls;
            info.name      = cls;
            info.icon_name = find_icon_for_class(cls);
            info.address   = addr;
            info.pid       = pid;
            info.is_active = active;
            info.minimized = stashed;
            if (stashed) info.min_address = addr;
            new_apps[cls]  = info;
            order.push_back(cls);
        } else {
            AppInfo& ex = new_apps[cls];
            if (!stashed) {
                if (ex.minimized) { ex.address = addr; ex.pid = pid; }   // є видиме вікно — клік фокусує його
                ex.minimized = false;
            } else if (ex.min_address.empty()) ex.min_address = addr;
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
            g_object_set_data_full(G_OBJECT(btn), "min-address", g_strdup(info.minimized ? info.min_address.c_str() : ""), g_free);
            if (info.minimized) gtk_widget_add_css_class(btn, "minimized");

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
            g_object_set_data_full(G_OBJECT(btn), "min-address", g_strdup(info.minimized ? info.min_address.c_str() : ""), g_free);
            if (info.minimized) gtk_widget_add_css_class(btn, "minimized");
            else gtk_widget_remove_css_class(btn, "minimized");
        }
    }

    // «Недавні програми»: вимкнено — секції немає; інакше лише перші N (історія все одно ведеться)
    const auto& dst = sysui::settings();
    size_t recent_n = dst.dock_recent ? std::min(recent_apps.size(), (size_t)dst.dock_recent_count) : 0;
    std::string new_recent_sig = dst.dock_recent ? "on" + std::to_string(recent_n) + ":" : "off:";
    for (size_t i = 0; i < recent_n; ++i) new_recent_sig += recent_apps[i].id + ";";

    if (new_recent_sig != state->last_recent_sig) {
        state->last_recent_sig = new_recent_sig;

        GtkWidget* child = gtk_widget_get_first_child(state->recent_box);
        while (child) {
            GtkWidget* next = gtk_widget_get_next_sibling(child);
            gtk_box_remove(GTK_BOX(state->recent_box), child);
            child = next;
        }
        state->recent_items.clear();

        if (recent_n > 0) {
            gtk_widget_set_visible(state->recent_box, TRUE);
            GtkWidget* sep = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_widget_add_css_class(sep, "dock-separator");
            gtk_widget_set_size_request(sep, 2, -1);
            gtk_widget_set_valign(sep, GTK_ALIGN_FILL);
            gtk_box_append(GTK_BOX(state->recent_box), sep);

            for (size_t ri = 0; ri < recent_n; ++ri) {
                const auto& ra = recent_apps[ri];
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
    // «Док відсуває вікна» (dock_reserve): док завжди на місці, вікна закінчуються над ним.
    bool reserve = sysui::settings().dock_reserve;
    bool should_show = reserve || mouse_bottom || ignored || g_no_window || stash_is_open();
    // fullscreen: док не ховаємо (виїзд + в'їзд при швидкому гортанні столів давали дві копії доку),
    // а переносимо під вікна — fullscreen-вікно його накриває; видимість лишається, як була
    static int under = -1;
    if ((int)g_fullscreen != under) {
        under = g_fullscreen;
        gtk_layer_set_layer(GTK_WINDOW(d->window), g_fullscreen ? GTK_LAYER_SHELL_LAYER_BOTTOM : GTK_LAYER_SHELL_LAYER_TOP);
    }
    if (g_fullscreen) should_show = last_shown;

    // зарезервована смуга = висота самої панелі (без прозорого запасу зверху під збільшення іконок)
    static int last_zone = -1;
    int zone = 0;
    if (reserve && should_show) zone = std::max(0, gtk_widget_get_height(d->window) - mag_headroom());
    if (zone != last_zone) { gtk_layer_set_exclusive_zone(GTK_WINDOW(d->window), zone); last_zone = zone; }

    if (should_show != last_shown) {
        gtk_widget_set_visible(d->window, should_show);
        last_shown = should_show;
    }

    // «Сховати на док»: вікно тягнуть мишею (рухається разом із курсором) і відпускають над доком
    // → застосунок лишається відкритим на прихованому столі special:mgos-dock. Wayland не дає доку
    // drop-подію для вікон, тому розпізнаємо сам жест: збіг зсувів вікна й курсора, потім зупинка над доком.
    static std::string drag_addr; static int last_wx = INT_MIN, last_wy = 0, last_cx = 0, last_cy = 0, still = 0;
    static bool following = false;
    int dw = gtk_widget_get_width(d->window), dh = std::max(0, gtk_widget_get_height(d->window) - mag_headroom());
    bool over_dock = should_show && dh > 0 && cy >= height - dh && cx >= (width - dw) / 2 && cx <= (width + dw) / 2;
    bool near_bottom = cy >= height - 250;   // стежимо заздалегідь: док з'являється лише біля самого низу
    if (sysui::settings().dock_minimize_drag && (near_bottom || following)) {
        json aw;
        try { aw = json::parse(sysui::hypr_request("j/activewindow")); } catch (...) {}
        std::string addr = aw.value("address", "");
        if (!addr.empty() && aw.contains("at") && aw["at"].is_array() && aw.value("fullscreen", 0) == 0) {
            int wx = aw["at"][0].get<int>(), wy = aw["at"][1].get<int>();
            if (addr == drag_addr && last_wx != INT_MIN) {
                int dwx = wx - last_wx, dwy = wy - last_wy, dcx = cx - last_cx, dcy = cy - last_cy;
                bool moved = std::abs(dwx) + std::abs(dwy) > 2;
                if (moved && std::abs(dwx - dcx) < 40 && std::abs(dwy - dcy) < 40) { following = true; still = 0; }
                else if (moved) { following = false; still = 0; }          // рух не від миші (клавіші, анімація)
                else if (following && over_dock && ++still >= 2) {          // ~300 мс нерухомо над доком — відпустили
                    sysui::hypr_request(std::string("dispatch movetoworkspacesilent ") + DOCK_STASH + ",address:" + addr);
                    following = false; still = 0;
                }
            }
            drag_addr = addr; last_wx = wx; last_wy = wy;
        }
        if (!near_bottom && !following) last_wx = INT_MIN;
    } else { following = false; last_wx = INT_MIN; }
    last_cx = cx; last_cy = cy;

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

.minimized image { opacity: 0.5; }
.stash-btn { padding: 0 6px; }
.stash-glyph { font-family: "JetBrainsMono Nerd Font"; font-size: 22px; opacity: 0.85; }
#dock-stash { background: transparent; }
.stash-area { background: transparent; }
.stash-add, .stash-add label, .stash-tile, .stash-tile label, .stash-label, .stash-pop, .stash-pop * { font-family: "SF Pro Display", "Inter", sans-serif; }
.stash-add { border-radius: 14px; padding: 5px 16px; background: rgba(255,255,255,0.16); color: #ffffff; border: 1px solid rgba(255,255,255,0.18);
             font-weight: 600; box-shadow: 0 6px 20px rgba(0,0,0,0.25); }
.stash-add:hover { background: rgba(255,255,255,0.26); }
.stash-tile { border-radius: 18px; padding: 8px 4px; transition: background 150ms ease; }
.stash-tile:hover { background: rgba(255,255,255,0.12); }
.stash-tile.dragging { background: rgba(255,255,255,0.18); }
.stash-thumb { border-radius: 8px; }
.stash-label { font-size: 12px; font-weight: 600; color: #ffffff; text-shadow: 0 1px 4px rgba(0,0,0,0.8); }
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
    if (g_dock_state) update_dock(g_dock_state);   // «недавні програми» вмикаються/вимикаються наживо
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

// ===================== СХОВИЩЕ ДОКУ =====================
// Прихований стіл special:mgos-dock (повноекранне розмиття з вікнами, перетягнутими на док) +
// ЯРЛИКИ програм прямо на ньому. Ярлики — прозорий шар над столом, видимий лише коли стіл відкрито
// (подія Hyprland activespecial). Кліки ловлять тільки плитки й «+», решта екрана — до вікон.
// Плитки тягаються: «Сітка» (прилипають) або «Вільно» — Налаштування → Док. Відкрити: кнопка в доку / Super+A.
#include <gio/gdesktopappinfo.h>
#include <csignal>
#include <glib-unix.h>
#define GCB(...) ((GCallback)(__VA_ARGS__))   // як G_CALLBACK, але коми в лямбдах не ламають макрос
// Ярлики й файли «Робочого столу» на просторі вимкнено (лишились тільки сховані вікна програм).
// Код нижче збережено: STASH_SHORTCUTS 1 повертає плитки й «+ Ярлик».
#define STASH_SHORTCUTS 0

static GtkWidget *g_stash_win = nullptr, *g_stash_fixed = nullptr, *g_stash_add = nullptr;
static const int CELL = 112, TILE = 96, TOP_PAD = 70, SIDE_PAD = 40;
static bool g_stash_open = false;

static std::string stash_file() { return std::string(getenv("HOME") ? getenv("HOME") : "") + "/.config/megumin-os/dock_stash.json"; }
static json stash_load() {
    try { std::ifstream f(stash_file()); json j; f >> j; if (j.is_object()) { if (!j.contains("shortcuts")) j["shortcuts"] = json::array();
                                                                         if (!j.contains("pos")) j["pos"] = json::object(); return j; } } catch (...) {}
    return json{{"shortcuts", json::array()}, {"pos", json::object()}};
}
static void stash_save(const json& j) {
    std::string p = stash_file(), tmp = p + ".tmp";
    { std::ofstream f(tmp); f << j.dump(2); }
    rename(tmp.c_str(), p.c_str());
}
static bool stash_grid() { return sysui::settings().dock_stash_layout != "free"; }
static void stash_area(int& w, int& h) { w = 1920; h = 1080; get_monitor_size(w, h); }

static void stash_snap(double& x, double& y) {
    int W, H; stash_area(W, H);
    x = std::clamp(x, 0.0, (double)W - TILE); y = std::clamp(y, (double)TOP_PAD - 30, (double)H - TILE - 120);
    if (!stash_grid()) return;
    x = SIDE_PAD + std::round((x - SIDE_PAD) / CELL) * CELL;
    y = TOP_PAD + std::round((y - TOP_PAD) / CELL) * CELL;
    x = std::clamp(x, 0.0, (double)W - TILE); y = std::clamp(y, (double)TOP_PAD - 30, (double)H - TILE - 120);
}

// Кліки — лише по плитках і кнопці «+»; решта прозорого шару пропускає мишу до вікон столу
static void stash_input_region() {
    if (!g_stash_win || !gtk_widget_get_realized(g_stash_win)) return;
    GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(g_stash_win));
    if (!surf) return;
    cairo_region_t* reg = cairo_region_create();
    for (GtkWidget* c = gtk_widget_get_first_child(g_stash_fixed); c; c = gtk_widget_get_next_sibling(c)) {
        graphene_rect_t b;
        if (gtk_widget_compute_bounds(c, g_stash_win, &b)) {
            cairo_rectangle_int_t r = {(int)b.origin.x, (int)b.origin.y, (int)b.size.width, (int)b.size.height};
            cairo_region_union_rectangle(reg, &r);
        }
    }
    gdk_surface_set_input_region(surf, reg);
    cairo_region_destroy(reg);
}

static void stash_close_space() { sysui::hypr_request("dispatch togglespecialworkspace mgos-dock"); }

struct StashTile { std::string key, id, kind; double x = 0, y = 0, sx = 0, sy = 0; };   // kind: app | file
static void stash_rebuild();
static bool stash_is_open() { return g_stash_open; }

static void stash_open_tile(StashTile* t) {
    stash_close_space();
    if (t->kind == "app") {
        GDesktopAppInfo* a = g_desktop_app_info_new(t->id.c_str());
        if (!a) a = g_desktop_app_info_new_from_filename(t->id.c_str());     // .desktop з «Робочого столу»
        if (a) { g_app_info_launch(G_APP_INFO(a), nullptr, nullptr, nullptr); g_object_unref(a); }
    } else {
        char* uri = g_filename_to_uri(t->id.c_str(), nullptr, nullptr);
        if (uri) { g_app_info_launch_default_for_uri(uri, nullptr, nullptr); g_free(uri); }
    }
}

static GtkWidget* stash_tile(const std::string& key, const std::string& kind, const std::string& id, const std::string& icon,
                             GIcon* gicon, const std::string& title, double x, double y) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(box, "stash-tile");
    gtk_widget_set_size_request(box, TILE, TILE + 10);
    GtkWidget* img;
    if (gicon) { img = gtk_image_new_from_gicon(gicon); gtk_image_set_pixel_size(GTK_IMAGE(img), 56); gtk_widget_add_css_class(img, "dock-icon"); }
    else img = make_icon(icon, 56);
    if (!icon.empty() && icon[0] == '/' && kind == "file") gtk_widget_add_css_class(img, "stash-thumb");
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), img);
    GtkWidget* lbl = gtk_label_new(title.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 12);
    gtk_widget_add_css_class(lbl, "stash-label");
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_widget_set_tooltip_text(box, kind == "file" ? (title + " — з «Робочого столу», клік відкриває").c_str()
                                                    : (title + " — клік запускає, ПКМ прибирає ярлик").c_str());
    gtk_widget_set_cursor_from_name(box, "pointer");

    auto* t = new StashTile{key, id, kind, x, y};
    g_object_set_data_full(G_OBJECT(box), "tile", t, [](gpointer p) { delete (StashTile*)p; });

    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", GCB(+[](GtkGestureDrag*, double, double, gpointer w) {
        auto* t = (StashTile*)g_object_get_data(G_OBJECT(w), "tile"); t->sx = t->x; t->sy = t->y;
        gtk_widget_add_css_class(GTK_WIDGET(w), "dragging");
        // під час перетягування плитка ловить мишу всюди
        if (GdkSurface* s = gtk_native_get_surface(GTK_NATIVE(g_stash_win))) gdk_surface_set_input_region(s, nullptr);
    }), box);
    g_signal_connect(drag, "drag-update", GCB(+[](GtkGestureDrag*, double dx, double dy, gpointer w) {
        auto* t = (StashTile*)g_object_get_data(G_OBJECT(w), "tile");
        int W, H; stash_area(W, H);
        t->x = std::clamp(t->sx + dx, 0.0, (double)W - TILE); t->y = std::clamp(t->sy + dy, 0.0, (double)H - TILE);
        gtk_fixed_move(GTK_FIXED(g_stash_fixed), GTK_WIDGET(w), t->x, t->y);
    }), box);
    g_signal_connect(drag, "drag-end", GCB(+[](GtkGestureDrag*, double dx, double dy, gpointer w) {
        auto* t = (StashTile*)g_object_get_data(G_OBJECT(w), "tile");
        gtk_widget_remove_css_class(GTK_WIDGET(w), "dragging");
        if (std::abs(dx) + std::abs(dy) < 5) {                     // це клік — запустити / відкрити
            t->x = t->sx; t->y = t->sy;
            stash_open_tile(t);
            stash_input_region();
            return;
        }
        stash_snap(t->x, t->y);
        gtk_fixed_move(GTK_FIXED(g_stash_fixed), GTK_WIDGET(w), t->x, t->y);
        json j = stash_load(); j["pos"][t->key] = {{"x", t->x}, {"y", t->y}}; stash_save(j);
        g_idle_add([](gpointer) -> gboolean { stash_input_region(); return G_SOURCE_REMOVE; }, nullptr);
    }), box);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));

    if (kind != "app" || key.rfind("app:", 0) != 0) return box;   // файли не прибираються ПКМ (не видаляємо чужі файли)
    GtkGesture* rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), 3);
    g_signal_connect(rc, "released", GCB(+[](GtkGestureClick*, int, double, double, gpointer w) {
        auto* t = (StashTile*)g_object_get_data(G_OBJECT(w), "tile");
        json j = stash_load(); json keep = json::array();
        for (auto& sc : j["shortcuts"]) if (sc.value("id", "") != t->id) keep.push_back(sc);
        j["shortcuts"] = keep; j["pos"].erase(t->key); stash_save(j);
        g_idle_add([](gpointer) -> gboolean { stash_rebuild(); return G_SOURCE_REMOVE; }, nullptr);
    }), box);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(rc));
    return box;
}

static void stash_rebuild() {
    if (!g_stash_fixed) return;
    for (GtkWidget* c = gtk_widget_get_first_child(g_stash_fixed); c;) {
        GtkWidget* n = gtk_widget_get_next_sibling(c);
        if (c != g_stash_add) gtk_fixed_remove(GTK_FIXED(g_stash_fixed), c);
        c = n;
    }
    int W, H; stash_area(W, H);
    gtk_fixed_move(GTK_FIXED(g_stash_fixed), g_stash_add, W / 2.0 - 60, 18);
    json j = stash_load();
    struct Item { std::string key, kind, id, icon, title; GIcon* gicon = nullptr; };
    std::vector<Item> items;
    for (auto& sc : j["shortcuts"])
        items.push_back({"app:" + sc.value("id", ""), "app", sc.value("id", ""), sc.value("icon", "application-x-executable"), sc.value("name", "")});
    // файли з теки «Робочий стіл» (XDG_DESKTOP_DIR)
    if (const char* dd = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP)) {
        GFile* dir = g_file_new_for_path(dd);
        if (GFileEnumerator* en = g_file_enumerate_children(dir, "standard::name,standard::display-name,standard::icon,standard::content-type,standard::is-hidden,standard::is-backup",
                                                            G_FILE_QUERY_INFO_NONE, nullptr, nullptr)) {
            std::vector<Item> files;
            while (GFileInfo* fi = g_file_enumerator_next_file(en, nullptr, nullptr)) {
                if (!g_file_info_get_is_hidden(fi) && !g_file_info_get_is_backup(fi)) {
                    std::string name = g_file_info_get_name(fi), path = std::string(dd) + "/" + name;
                    std::string ct = g_file_info_get_content_type(fi) ? g_file_info_get_content_type(fi) : "";
                    Item it{"file:" + name, "file", path, "", g_file_info_get_display_name(fi)};
                    if (name.size() > 8 && name.substr(name.size() - 8) == ".desktop") {           // ярлик на робочому столі
                        if (GDesktopAppInfo* a = g_desktop_app_info_new_from_filename(path.c_str())) {
                            it.kind = "app"; it.title = g_app_info_get_display_name(G_APP_INFO(a));
                            if (GIcon* gi = g_app_info_get_icon(G_APP_INFO(a))) it.gicon = (GIcon*)g_object_ref(gi);
                            g_object_unref(a);
                        }
                    } else if (ct.rfind("image/", 0) == 0) it.icon = path;                         // зображення — мініатюра
                    else if (GIcon* gi = g_file_info_get_icon(fi)) it.gicon = (GIcon*)g_object_ref(gi);
                    files.push_back(it);
                }
                g_object_unref(fi);
            }
            g_object_unref(en);
            std::sort(files.begin(), files.end(), [](const Item& a, const Item& b) { return g_utf8_collate(a.title.c_str(), b.title.c_str()) < 0; });
            items.insert(items.end(), files.begin(), files.end());
        }
        g_object_unref(dir);
    }
    std::vector<std::pair<double, double>> used;
    std::vector<std::tuple<Item, double, double, bool>> placed;
    for (auto& it : items) {
        const std::string& k = it.key;
        if (j["pos"].contains(k)) { double x = j["pos"][k].value("x", 0.0), y = j["pos"][k].value("y", 0.0); stash_snap(x, y);
                                    used.push_back({x, y}); placed.push_back({it, x, y, true}); }
        else placed.push_back({it, 0, 0, false});
    }
    for (auto& [it, x, y, has] : placed) {
        if (!has) {   // перша вільна клітинка зліва згори
            bool found = false;
            for (int r = 0; !found && TOP_PAD + r * CELL < H - 200; r++)
                for (int c = 0; !found && SIDE_PAD + c * CELL < W - TILE; c++) {
                    double cx = SIDE_PAD + c * CELL, cy = TOP_PAD + r * CELL; bool busy = false;
                    for (auto& u : used) if (std::abs(u.first - cx) < TILE * 0.7 && std::abs(u.second - cy) < TILE * 0.7) { busy = true; break; }
                    if (!busy) { x = cx; y = cy; found = true; }
                }
            used.push_back({x, y});
        }
        gtk_fixed_put(GTK_FIXED(g_stash_fixed), stash_tile(it.key, it.kind, it.id, it.icon, it.gicon, it.title, x, y), x, y);
        if (it.gicon) g_object_unref(it.gicon);
    }
    g_idle_add([](gpointer) -> gboolean { stash_input_region(); return G_SOURCE_REMOVE; }, nullptr);
}

static void stash_add_popover(GtkWidget* anchor) {
    // поки відкритий пошук — шар бере клавіатуру
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_stash_win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    if (GdkSurface* s = gtk_native_get_surface(GTK_NATIVE(g_stash_win))) gdk_surface_set_input_region(s, nullptr);
    GtkWidget* pop = gtk_popover_new();
    gtk_widget_set_parent(pop, anchor);
    gtk_widget_add_css_class(pop, "stash-pop");
    GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Знайти програму…");
    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sc, 300, 360);
    GtkWidget* list = gtk_list_box_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), list);
    gtk_box_append(GTK_BOX(v), search); gtk_box_append(GTK_BOX(v), sc);
    gtk_popover_set_child(GTK_POPOVER(pop), v);

    GList* apps = g_app_info_get_all();
    std::vector<GAppInfo*> sorted;
    for (GList* l = apps; l; l = l->next) if (g_app_info_should_show(G_APP_INFO(l->data))) sorted.push_back(G_APP_INFO(l->data));
    std::sort(sorted.begin(), sorted.end(), [](GAppInfo* a, GAppInfo* b) {
        return g_utf8_collate(g_app_info_get_display_name(a), g_app_info_get_display_name(b)) < 0; });
    for (GAppInfo* a : sorted) {
        const char* id = g_app_info_get_id(a); if (!id) continue;
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GIcon* gi = g_app_info_get_icon(a);
        GtkWidget* img = gi ? gtk_image_new_from_gicon(gi) : gtk_image_new_from_icon_name("application-x-executable");
        gtk_image_set_pixel_size(GTK_IMAGE(img), 24);
        gtk_box_append(GTK_BOX(row), img);
        gtk_box_append(GTK_BOX(row), gtk_label_new(g_app_info_get_display_name(a)));
        GtkWidget* lr = gtk_list_box_row_new(); gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(lr), row);
        std::string icon_s = "application-x-executable";
        if (gi) { char* s = g_icon_to_string(gi); if (s) { icon_s = s; g_free(s); } }
        g_object_set_data_full(G_OBJECT(lr), "id", g_strdup(id), g_free);
        g_object_set_data_full(G_OBJECT(lr), "name", g_strdup(g_app_info_get_display_name(a)), g_free);
        g_object_set_data_full(G_OBJECT(lr), "icon", g_strdup(icon_s.c_str()), g_free);
        g_object_set_data_full(G_OBJECT(lr), "search", g_utf8_strdown(g_app_info_get_display_name(a), -1), g_free);
        gtk_list_box_append(GTK_LIST_BOX(list), lr);
    }
    g_list_free_full(apps, g_object_unref);

    gtk_list_box_set_filter_func(GTK_LIST_BOX(list), [](GtkListBoxRow* r, gpointer e) -> gboolean {
        const char* q = gtk_editable_get_text(GTK_EDITABLE(e));
        if (!q || !*q) return TRUE;
        char* ql = g_utf8_strdown(q, -1);
        bool ok = strstr((const char*)g_object_get_data(G_OBJECT(r), "search"), ql) != nullptr;
        g_free(ql); return ok;
    }, search, nullptr);
    g_signal_connect_swapped(search, "search-changed", G_CALLBACK(gtk_list_box_invalidate_filter), list);
    g_signal_connect(list, "row-activated", GCB(+[](GtkListBox*, GtkListBoxRow* r, gpointer pop) {
        json j = stash_load();
        std::string id = (const char*)g_object_get_data(G_OBJECT(r), "id");
        bool have = false;
        for (auto& sc : j["shortcuts"]) if (sc.value("id", "") == id) have = true;
        if (!have) {
            j["shortcuts"].push_back({{"id", id}, {"name", (const char*)g_object_get_data(G_OBJECT(r), "name")},
                                      {"icon", (const char*)g_object_get_data(G_OBJECT(r), "icon")}});
            stash_save(j);
        }
        gtk_popover_popdown(GTK_POPOVER(pop));
    }), pop);
    g_signal_connect(pop, "closed", GCB(+[](GtkPopover* p, gpointer) {
        gtk_layer_set_keyboard_mode(GTK_WINDOW(g_stash_win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        stash_rebuild();
        // stash_rebuild() може вже знищити батька — тримаємо власне посилання й перевіряємо
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer p) -> gboolean {
            if (gtk_widget_get_parent(GTK_WIDGET(p))) gtk_widget_unparent(GTK_WIDGET(p));
            return G_SOURCE_REMOVE;
        }, g_object_ref(p), g_object_unref);
    }), nullptr);
    gtk_popover_popup(GTK_POPOVER(pop));
    gtk_widget_grab_focus(search);
}

static void stash_ensure() {
    if (g_stash_win) return;
    if (const char* dd = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP)) {   // файли на столі змінились → оновити
        GFile* dir = g_file_new_for_path(dd);
        if (GFileMonitor* mon = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr))
            g_signal_connect(mon, "changed", GCB(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer) {
                static guint pend = 0;
                if (!g_stash_open || pend) return;
                pend = g_timeout_add(250, [](gpointer) -> gboolean { pend = 0; if (g_stash_open) stash_rebuild(); return G_SOURCE_REMOVE; }, nullptr);
            }), nullptr);
        g_object_unref(dir);
    }
    g_stash_win = gtk_window_new();
    gtk_widget_set_name(g_stash_win, "dock-stash");
    gtk_layer_init_for_window(GTK_WINDOW(g_stash_win));
    gtk_layer_set_namespace(GTK_WINDOW(g_stash_win), "dock_stash");
    gtk_layer_set_layer(GTK_WINDOW(g_stash_win), GTK_LAYER_SHELL_LAYER_TOP);   // над розмиттям столу
    for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
        gtk_layer_set_anchor(GTK_WINDOW(g_stash_win), e, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(g_stash_win), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_stash_win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    g_stash_fixed = gtk_fixed_new();
    gtk_widget_add_css_class(g_stash_fixed, "stash-area");
    g_stash_add = gtk_button_new_with_label("+  Ярлик");
    gtk_widget_add_css_class(g_stash_add, "stash-add");
    gtk_widget_set_tooltip_text(g_stash_add, "Додати ярлик програми в Сховище");
    g_signal_connect(g_stash_add, "clicked", GCB(+[](GtkButton* b, gpointer) { stash_add_popover(GTK_WIDGET(b)); }), nullptr);
    gtk_fixed_put(GTK_FIXED(g_stash_fixed), g_stash_add, 900, 18);
    gtk_window_set_child(GTK_WINDOW(g_stash_win), g_stash_fixed);
    g_signal_connect(g_stash_win, "map", GCB(+[](GtkWidget*, gpointer) {
        g_timeout_add(60, [](gpointer) -> gboolean { stash_input_region(); return G_SOURCE_REMOVE; }, nullptr);
    }), nullptr);
}

// Hyprland повідомив, який спеціальний стіл тепер видно → показуємо / ховаємо ярлики
static void stash_on_special(bool open) {
    g_stash_open = open;
#if !STASH_SHORTCUTS
    return;   // без шару ярликів: лише «док завжди видно, поки простір відкритий»
#endif
    stash_ensure();
    if (open) { stash_rebuild(); gtk_widget_set_visible(g_stash_win, TRUE); }
    else gtk_widget_set_visible(g_stash_win, FALSE);
}

static void stash_toggle() { sysui::hypr_request("dispatch togglespecialworkspace mgos-dock"); }

// ---- Стискання схованих програм у zram ----
// Вікно переїхало в сховище (перетягування на док / Super+Q) → одразу після анімації, якщо воно все ще
// там і процес не має видимих вікон деінде, його анонімна пам'ять витісняється в zram (mgos-pageout,
// cap_sys_nice). Повернення вікна нічого не вимагає: сторінки підтягуються з zram самі.
static void stash_compress_later(const std::string& addr) {
    if (!sysui::settings().dock_stash_zram) return;
    if (access("/usr/local/bin/mgos-pageout", X_OK) != 0) return;   // не встановлено (mgos-zram setup)
    // одразу, щойно вікно доїхало в сховище (900 мс — дати анімації переходу закінчитись без ривка)
    g_timeout_add_full(G_PRIORITY_LOW, 900, [](gpointer d) -> gboolean {
        std::string a = *(std::string*)d;
        try {
            json cl = json::parse(sysui::hypr_request("j/clients"));
            int pid = -1;
            for (auto& c : cl) if (c.value("address", "") == a) {
                if (c.value("workspace", json::object()).value("name", "") != DOCK_STASH) return G_SOURCE_REMOVE;   // уже повернули
                pid = c.value("pid", -1);
            }
            if (pid <= 1) return G_SOURCE_REMOVE;
            for (auto& c : cl)   // той самий процес має вікна поза сховищем (kitty, Firefox) — не чіпаємо
                if (c.value("pid", -1) == pid && c.value("workspace", json::object()).value("name", "") != DOCK_STASH) return G_SOURCE_REMOVE;
            std::string cmd = "/usr/local/bin/mgos-pageout --tree " + std::to_string(pid);
            g_spawn_command_line_async(cmd.c_str(), nullptr);
        } catch (...) {}
        return G_SOURCE_REMOVE;
    }, new std::string(addr), [](gpointer d) { delete (std::string*)d; });
}
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
#if 0   // кнопку «Сховище» прибрано з доку — простір відкривається Super+A
    {   // кнопка «Сховище»
        GtkWidget* sb = gtk_button_new();
        gtk_widget_add_css_class(sb, "dock-item"); gtk_widget_add_css_class(sb, "stash-btn");
        GtkWidget* gl = gtk_label_new("\U000F0570");   // nf-md-view-grid
        gtk_widget_add_css_class(gl, "stash-glyph");
        gtk_button_set_child(GTK_BUTTON(sb), gl);
        gtk_widget_set_tooltip_text(sb, "Сховище — сховані вікна і ярлики (Super+A)");
        gtk_widget_set_valign(sb, GTK_ALIGN_CENTER);
        g_signal_connect(sb, "clicked", GCB(+[](GtkButton*, gpointer) { stash_toggle(); }), nullptr);
        gtk_box_append(GTK_BOX(wrapper), sb);
    }
#endif
    gtk_box_append(GTK_BOX(wrapper), sys_box);
    g_unix_signal_add(SIGUSR1, [](gpointer) -> gboolean { stash_toggle(); return G_SOURCE_CONTINUE; }, nullptr);

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
                
                if (line.find("activespecial>>") == 0) {   // activespecial>>special:mgos-dock,eDP-1  /  activespecial>>,eDP-1
                    bool open = line.find(std::string(">>") + DOCK_STASH + ",") != std::string::npos;
                    g_idle_add(open ? (GSourceFunc)[](gpointer) -> gboolean { stash_on_special(true); return G_SOURCE_REMOVE; }
                                    : (GSourceFunc)[](gpointer) -> gboolean { stash_on_special(false); return G_SOURCE_REMOVE; }, nullptr);
                }
                if (line.find("movewindowv2>>") == 0) {   // movewindowv2>>ADDR,WSID,WSNAME
                    std::string rest = line.substr(14);
                    size_t c1 = rest.find(','), c2 = c1 == std::string::npos ? c1 : rest.find(',', c1 + 1);
                    if (c2 != std::string::npos && rest.substr(c2 + 1) == DOCK_STASH) {
                        std::string* addr = new std::string("0x" + rest.substr(0, c1));
                        g_idle_add([](gpointer d) -> gboolean { stash_compress_later(*(std::string*)d); delete (std::string*)d; return G_SOURCE_REMOVE; }, addr);
                    }
                }
                if (line.find("workspace>>") == 0 || line.find("activewindow>>") == 0 || line.find("closewindow>>") == 0 || line.find("openwindow>>") == 0 || line.find("fullscreen>>") == 0 || line.find("movewindowv2>>") == 0) {
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