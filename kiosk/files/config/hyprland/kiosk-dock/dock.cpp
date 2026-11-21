#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <yyjson.h>

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <thread>
#include <malloc.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

namespace fs = std::filesystem;

// ===================== НАЛАШТУВАННЯ =====================

static std::vector<std::string> ALWAYS_VISIBLE_APPS = {
    "nautilus", "kitty", "wezterm", "ignis",
    "code", "modrinth-app", "steam", "spotify"
};
static const int MAX_RECENT_APPS = 15;
static const int DOCK_ICON_SIZE  = 38;

// ===================== КЕШ ДЛЯ ОПТИМІЗАЦІЇ =====================
static std::string g_active_class = "";
static bool g_no_window = false;

static std::unordered_map<std::string, std::string> icon_cache;
static std::unordered_map<std::string, std::string> exec_cache;

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

static int cached_width  = 0;
static int cached_height = 0;

static bool last_shown = true;
static time_t last_wal_mtime = 0;

// ===================== ПРЯМИЙ СОКЕТ HYPRLAND (0 ФОРКІВ, 0 HYPRCTL) =====================

static std::string hypr_query(const std::string& cmd) {
    static std::string sock_path = "";
    if (sock_path.empty()) {
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (!xdg || !sig) return "";
        sock_path = std::string(xdg) + "/hypr/" + sig + "/.socket.sock";
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "";
    }

    if (write(sock, cmd.c_str(), cmd.size()) < 0) {
        close(sock);
        return "";
    }

    std::string response;
    char buffer[4096];
    ssize_t bytes;
    while ((bytes = read(sock, buffer, sizeof(buffer))) > 0) {
        response.append(buffer, bytes);
    }
    close(sock);
    return response;
}

static void hypr_dispatch_async(const std::string& subcmd) {
    std::string cmd = "dispatch " + subcmd;
    hypr_query(cmd);
}

// ===================== УТИЛІТИ =====================

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return s;
}

// ===================== ІСТОРІЯ ТА ЗБЕРЕЖЕННЯ =====================

static void save_history() {
    const char* home = getenv("HOME");
    if (!home) return;
    std::string dir = std::string(home) + "/.cache/megumin-os";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir + "/dock_history.txt");
    for (const auto& r : recent_apps) {
        f << r.id << "|" << r.icon_name << "|" << r.exec_cmd << "\n";
    }
}

static void load_history() {
    const char* home = getenv("HOME");
    if (!home) return;
    std::string file = std::string(home) + "/.cache/megumin-os/dock_history.txt";
    std::ifstream f(file);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string id, icon, exec_cmd;
        std::getline(ss, id, '|');
        std::getline(ss, icon, '|');
        std::getline(ss, exec_cmd, '|');
        if (!id.empty()) {
            recent_apps.push_back({id, icon, exec_cmd});
        }
    }
}

// ===================== ПОШУК ІКОНОК І EXEC ЧЕРЕЗ C++ FS (БЕЗ FIND) =====================

static void parse_desktop_file(const std::string& path, std::string& icon_out, std::string& exec_out) {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    bool found_icon = false, found_exec = false;
    while (std::getline(f, line)) {
        if (!found_icon && line.rfind("Icon=", 0) == 0) {
            icon_out = line.substr(5);
            while (!icon_out.empty() && (icon_out.back() == '\r' || icon_out.back() == ' ')) icon_out.pop_back();
            found_icon = true;
        } else if (!found_exec && line.rfind("Exec=", 0) == 0) {
            std::string ex = line.substr(5);
            auto pos = ex.find(' ');
            if (pos != std::string::npos) ex = ex.substr(0, pos);
            while (!ex.empty() && (ex.back() == '\r' || ex.back() == ' ')) ex.pop_back();
            exec_out = ex;
            found_exec = true;
        }
        if (found_icon && found_exec) break;
    }
}

static void find_desktop_info(const std::string& wm_class, std::string& icon_out, std::string& exec_out) {
    std::string cls = to_lower(wm_class);
    if (icon_cache.count(cls) && exec_cache.count(cls)) {
        icon_out = icon_cache[cls];
        exec_out = exec_cache[cls];
        return;
    }

    const char* home = getenv("HOME");
    std::vector<std::string> dirs = {
        "/usr/share/applications",
        "/usr/local/share/applications"
    };
    if (home) {
        dirs.push_back(std::string(home) + "/.local/share/applications");
    }

    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::exists(d, ec)) continue;
        for (const auto& entry : fs::directory_iterator(d, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string filename = to_lower(entry.path().filename().string());
            if (filename.size() < 8 || filename.substr(filename.size() - 8) != ".desktop") continue;

            std::string base = filename.substr(0, filename.size() - 8);
            if (base.find(cls) != std::string::npos || cls.find(base) != std::string::npos) {
                parse_desktop_file(entry.path().string(), icon_out, exec_out);
                if (!icon_out.empty() || !exec_out.empty()) {
                    if (icon_out.empty()) icon_out = cls;
                    if (exec_out.empty()) exec_out = cls;
                    icon_cache[cls] = icon_out;
                    exec_cache[cls] = exec_out;
                    return;
                }
            }
        }
    }

    if (icon_out.empty()) icon_out = cls;
    if (exec_out.empty()) exec_out = cls;
    icon_cache[cls] = icon_out;
    exec_cache[cls] = exec_out;
}

static std::string find_icon_for_class(const std::string& wm_class) {
    std::string cls = to_lower(wm_class);
    if (icon_cache.count(cls)) return icon_cache[cls];
    std::string icon, exec_cmd;
    find_desktop_info(wm_class, icon, exec_cmd);
    return icon;
}

static std::string find_exec_for_class(const std::string& wm_class) {
    std::string cls = to_lower(wm_class);
    if (exec_cache.count(cls)) return exec_cache[cls];
    std::string icon, exec_cmd;
    find_desktop_info(wm_class, icon, exec_cmd);
    return exec_cmd;
}

// ===================== СНІМОК ВІДКРИТИХ ПРОГРАМ =====================
static std::string g_last_open_snapshot_sig = "\x01";

static void save_open_snapshot(const std::vector<std::string>& order, const std::unordered_map<std::string, AppInfo>& apps) {
    std::string sig;
    for (const auto& cls : order) sig += cls + ";";
    if (sig == g_last_open_snapshot_sig) return;
    g_last_open_snapshot_sig = sig;

    const char* home = getenv("HOME");
    if (!home) return;
    std::string dir = std::string(home) + "/.cache/megumin-os";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir + "/dock_open_snapshot.txt");
    for (const auto& cls : order) {
        auto it = apps.find(cls);
        if (it != apps.end()) {
            f << cls << "|" << it->second.icon_name << "|" << find_exec_for_class(cls) << "\n";
        }
    }
}

static std::vector<RecentApp> load_open_snapshot() {
    std::vector<RecentApp> result;
    const char* home = getenv("HOME");
    if (!home) return result;
    std::string file = std::string(home) + "/.cache/megumin-os/dock_open_snapshot.txt";
    std::ifstream f(file);
    if (!f.is_open()) return result;
    std::string line;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string id, icon, exec_cmd;
        std::getline(ss, id, '|');
        std::getline(ss, icon, '|');
        std::getline(ss, exec_cmd, '|');
        if (!id.empty()) result.push_back({id, icon, exec_cmd});
    }
    return result;
}

// ===================== ЗАПУСК / ЗАКРИТТЯ ПРОГРАМ =====================

static void launch_app(const std::string& exec_cmd) {
    if (exec_cmd.empty()) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", (exec_cmd + " >/dev/null 2>&1").c_str(), (char*)NULL);
        _exit(1);
    }
}

static void close_window_graceful(const std::string& address) {
    if (address.empty()) return;
    hypr_dispatch_async("closewindow address:" + address);
}

static void close_window_force(int pid, const std::string& address, const std::string&) {
    if (pid > 0) {
        kill(pid, SIGKILL);
    }
    if (!address.empty()) {
        hypr_dispatch_async("closewindow address:" + address);
    }
}

// ===================== ІКОНКА =====================

static GtkWidget* make_icon(const std::string& icon_name, int size) {
    GtkWidget* img = nullptr;
    if (!icon_name.empty() && icon_name[0] == '/') {
        img = gtk_image_new_from_file(icon_name.c_str());
    } else {
        img = gtk_image_new_from_icon_name(icon_name.c_str());
    }
    gtk_image_set_pixel_size(GTK_IMAGE(img), size);
    gtk_widget_add_css_class(img, "dock-icon");
    return img;
}

// ===================== КЛІКИ =====================

static void on_left_click(GtkButton* btn, gpointer) {
    const char* address = (const char*)g_object_get_data(G_OBJECT(btn), "window-address");
    if (address && strlen(address) > 0) {
        hypr_dispatch_async("focuswindow address:" + std::string(address));
    }
}

static void on_middle_click(GtkGestureClick* gesture, int, double, double, gpointer user_data) {
    GtkWidget* btn = GTK_WIDGET(user_data);
    int pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "window-pid"));
    const char* address = (const char*)g_object_get_data(G_OBJECT(btn), "window-address");
    const char* app_id  = (const char*)g_object_get_data(G_OBJECT(btn), "app-id");

    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    std::string addr_str = address ? address : "";
    std::string app_str  = app_id  ? app_id  : "";

    if (state & GDK_SHIFT_MASK) {
        close_window_force(pid, addr_str, app_str);
    } else {
        close_window_graceful(addr_str);
    }
}

static void on_recent_click(GtkButton* btn, gpointer) {
    const char* cmd = (const char*)g_object_get_data(G_OBJECT(btn), "exec-cmd");
    if (cmd) launch_app(cmd);
}

static void on_recent_middle_click(GtkGestureClick*, int, double, double, gpointer user_data) {
    GtkWidget* btn = GTK_WIDGET(user_data);
    const char* app_id = (const char*)g_object_get_data(G_OBJECT(btn), "app-id");
    if (!app_id) return;

    std::string target_id = app_id;
    recent_apps.erase(
        std::remove_if(recent_apps.begin(), recent_apps.end(),
            [&](const RecentApp& r) { return r.id == target_id; }),
        recent_apps.end()
    );
    save_history();
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

    std::unordered_map<std::string, GtkWidget*> current_items;
    std::vector<GtkWidget*> recent_items;
    std::string last_recent_sig = "\x01";
};

static DockState* g_dock_state = nullptr;
static GtkWidget* g_ws_label   = nullptr;
static GtkWidget* g_window     = nullptr;
static GtkCssProvider* g_css_provider = nullptr;

// ===================== ОНОВЛЕННЯ ДОКА =====================

static void get_monitor_size(int& width, int& height) {
    if (cached_width != 0) { width = cached_width; height = cached_height; return; }
    std::string res = hypr_query("j/monitors");
    if (!res.empty()) {
        yyjson_doc* doc = yyjson_read(res.c_str(), res.size(), 0);
        if (doc) {
            yyjson_val* root = yyjson_doc_get_root(doc);
            yyjson_val* mon = yyjson_arr_get_first(root);
            if (mon) {
                double scale = 1.0;
                yyjson_val* s = yyjson_obj_get(mon, "scale");
                if (s) scale = yyjson_get_num(s);
                if (scale <= 0.0) scale = 1.0;

                int w = 1920, h = 1080;
                yyjson_val* vw = yyjson_obj_get(mon, "width");
                yyjson_val* vh = yyjson_obj_get(mon, "height");
                if (vw) w = yyjson_get_int(vw);
                if (vh) h = yyjson_get_int(vh);

                cached_width  = (int)(w / scale);
                cached_height = (int)(h / scale);
            }
            yyjson_doc_free(doc);
        }
    }
    if (cached_width == 0) { cached_width = 1920; cached_height = 1080; }
    width  = cached_width;
    height = cached_height;
}

static gboolean update_dock(gpointer user_data) {
    auto* state = static_cast<DockState*>(user_data);
    if (!state) return G_SOURCE_REMOVE;

    std::string old_history = "";
    for (auto& r : recent_apps) old_history += r.id;

    std::string clients_raw = hypr_query("j/clients");
    std::string active_raw  = hypr_query("j/activewindow");

    std::string active_addr = "";
    g_active_class = "";
    g_no_window = true;

    if (!active_raw.empty()) {
        yyjson_doc* doc = yyjson_read(active_raw.c_str(), active_raw.size(), 0);
        if (doc) {
            yyjson_val* root = yyjson_doc_get_root(doc);
            yyjson_val* a = yyjson_obj_get(root, "address");
            yyjson_val* c = yyjson_obj_get(root, "class");
            if (a) active_addr = yyjson_get_str(a) ? yyjson_get_str(a) : "";
            if (c) g_active_class = to_lower(yyjson_get_str(c) ? yyjson_get_str(c) : "");
            g_no_window = active_addr.empty();
            yyjson_doc_free(doc);
        }
    }

    struct ClientItem {
        std::string cls;
        std::string addr;
        int pid = 0;
        int ws_id = 0;
    };
    std::vector<ClientItem> parsed_clients;

    if (!clients_raw.empty()) {
        yyjson_doc* doc = yyjson_read(clients_raw.c_str(), clients_raw.size(), 0);
        if (doc) {
            yyjson_val* root = yyjson_doc_get_root(doc);
            size_t idx, max;
            yyjson_val* item;
            yyjson_arr_foreach(root, idx, max, item) {
                yyjson_val* vc = yyjson_obj_get(item, "class");
                yyjson_val* va = yyjson_obj_get(item, "address");
                yyjson_val* vp = yyjson_obj_get(item, "pid");
                yyjson_val* vw = yyjson_obj_get(item, "workspace");

                std::string cls = vc && yyjson_get_str(vc) ? to_lower(yyjson_get_str(vc)) : "";
                if (cls.empty()) continue;

                ClientItem ci;
                ci.cls = cls;
                ci.addr = va && yyjson_get_str(va) ? yyjson_get_str(va) : "";
                ci.pid = vp ? yyjson_get_int(vp) : 0;
                if (vw) {
                    yyjson_val* wid = yyjson_obj_get(vw, "id");
                    if (wid) ci.ws_id = yyjson_get_int(wid);
                }
                parsed_clients.push_back(std::move(ci));
            }
            yyjson_doc_free(doc);
        }
    }

    std::sort(parsed_clients.begin(), parsed_clients.end(), [](const ClientItem& a, const ClientItem& b) {
        return a.ws_id < b.ws_id;
    });

    std::unordered_map<std::string, AppInfo> new_apps;
    std::vector<std::string> order;

    for (const auto& w : parsed_clients) {
        bool active = (w.addr == active_addr);
        if (new_apps.find(w.cls) == new_apps.end()) {
            AppInfo info;
            info.id        = w.cls;
            info.name      = w.cls;
            info.icon_name = find_icon_for_class(w.cls);
            info.address   = w.addr;
            info.pid       = w.pid;
            info.is_active = active;
            new_apps[w.cls] = info;
            order.push_back(w.cls);
        } else {
            if (active) {
                new_apps[w.cls].is_active = true;
                new_apps[w.cls].address   = w.addr;
                new_apps[w.cls].pid       = w.pid;
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
    for (auto& r : recent_apps) new_history += r.id;
    if (old_history != new_history) save_history();

    // Видаляємо закриті програми
    for (auto it = state->current_items.begin(); it != state->current_items.end(); ) {
        if (new_apps.find(it->first) == new_apps.end()) {
            gtk_box_remove(GTK_BOX(state->dock_box), it->second);
            it = state->current_items.erase(it);
        } else {
            ++it;
        }
    }

    // Додаємо / оновлюємо активні
    for (const auto& cls : order) {
        const AppInfo& info = new_apps[cls];
        if (state->current_items.find(cls) == state->current_items.end()) {
            GtkWidget* btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "dock-item");
            gtk_widget_add_css_class(btn, "running");
            if (info.is_active) gtk_widget_add_css_class(btn, "active");

            GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

            GtkWidget* icon = make_icon(info.icon_name, DOCK_ICON_SIZE);
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
                gtk_widget_add_css_class(btn, "recent");

                GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

                GtkWidget* icon = make_icon(ra.icon_name, DOCK_ICON_SIZE);
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
    if (!label) return G_SOURCE_REMOVE;

    int ws = 1;
    std::string res = hypr_query("j/activeworkspace");
    if (!res.empty()) {
        yyjson_doc* doc = yyjson_read(res.c_str(), res.size(), 0);
        if (doc) {
            yyjson_val* root = yyjson_doc_get_root(doc);
            yyjson_val* v = yyjson_obj_get(root, "id");
            if (v) ws = yyjson_get_int(v);
            yyjson_doc_free(doc);
        }
    }
    std::string txt = "ㅤ ㅤ " + std::to_string(ws);
    gtk_label_set_text(label, txt.c_str());
    return G_SOURCE_REMOVE;
}

// ===================== SYS MONITOR =====================
struct SysWidgets { GtkWidget* cpu; GtkWidget* ram; };

static gboolean update_sysmon(gpointer user_data) {
    auto* sw = static_cast<SysWidgets*>(user_data);
    if (!sw) return G_SOURCE_CONTINUE;

    {
        std::ifstream f("/proc/stat");
        std::string line;
        if (std::getline(f, line)) {
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
struct AutohideData { GtkWidget* revealer; GtkWidget* window; };

static gboolean check_autohide(gpointer user_data) {
    auto* d = static_cast<AutohideData*>(user_data);
    if (!d) return G_SOURCE_CONTINUE;

    // Прямий запит позиції курсора через UNIX-сокет (0.05мс, 0 процесів)
    std::string pos_raw = hypr_query("cursorpos");
    int cx = 0, cy = 0;
    if (sscanf(pos_raw.c_str(), "%d, %d", &cx, &cy) != 2) return G_SOURCE_CONTINUE;

    int width = 0, height = 0;
    get_monitor_size(width, height);

    int trigger_height    = 40;
    int dock_visible_area = 100;
    double center_start   = width  * 0.1;
    double center_end     = width  * 0.9;

    bool revealed = gtk_revealer_get_reveal_child(GTK_REVEALER(d->revealer));
    bool mouse_bottom = false;
    if (revealed) {
        mouse_bottom = (cy > height - dock_visible_area);
    } else {
        mouse_bottom = (cy >= height - trigger_height) && (cx > center_start) && (cx < center_end);
    }

    bool ignored = false;
    for (const auto& app : ALWAYS_VISIBLE_APPS) {
        if (!g_active_class.empty() && g_active_class.find(app) != std::string::npos) {
            ignored = true;
            break;
        }
    }

    bool should_show = mouse_bottom || ignored || g_no_window;

    if (should_show != last_shown) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(d->revealer), should_show);
        last_shown = should_show;
    }

    return G_SOURCE_CONTINUE;
}

// ===================== CSS =====================

static const char* CSS_BASE = R"(
* { font-family: "JetBrainsMono Nerd Font", sans-serif; }

window#dock,
window#dock.background,
window#dock > box,
#dock {
    background-color: transparent;
    background-image: none;
    background: none;
    box-shadow: none;
    border: none;
}

#dock scrolledwindow,
#dock scrolledwindow viewport,
#dock viewport {
    background-color: transparent;
    background-image: none;
    background: none;
    box-shadow: none;
    border: none;
}

#dock .dock-wrapper {
    border-radius: 20px;
    padding: 4px 6px; 
    margin-bottom: 8px; 
    border-width: 2px;
    border-style: solid;
    transition: background-color 0.4s ease, border-color 0.4s ease, box-shadow 0.4s ease;
}

#dock button,
#dock button.dock-item,
#dock .dock-item {
    background-color: transparent;
    background-image: none;
    background: none;
    border: none;
    box-shadow: none;
    outline: none;
    border-radius: 12px;
    padding: 2px 4px;        
    margin: 0 2px;
    transition: background-color 0.2s ease, transform 0.2s ease;
}

#dock button.dock-item:hover,
#dock .dock-item:hover {
    background-color: rgba(255, 255, 255, 0.12);
}

#dock button.dock-item:active,
#dock .dock-item:active {
    transform: scale(0.92);
}

#dock image,
#dock image.dock-icon,
#dock .dock-icon,
#dock button image {
    background-color: transparent;
    background-image: none;
    background: none;
    border: none;
    box-shadow: none;
}

#dock .mac-dot {
    min-height: 5px;
    min-width: 5px;
    background-color: transparent;
    border-radius: 100%;
    margin-top: 2px;
    margin-bottom: 2px;
    transition: background-color 0.25s ease, box-shadow 0.25s ease;
}

#dock .running .mac-dot {
    background-color: rgba(255, 255, 255, 0.5);
}

#dock .active .mac-dot {
    background-color: $ACCENT;
    box-shadow: 0 0 5px $ACCENT_A30;
}

#dock .dock-separator {
    border-radius: 2px;
    min-width: 2px;
    margin: 8px 6px;
}

#dock .sys-monitor {
    background: transparent;
    border-radius: 12px;
    padding: 2px 8px;       
    margin: 0 3px;
}

#dock .sys-text {
    font-size: 13px;        
    font-weight: 800;
    margin-right: 6px;
}

#dock .ws-label {
    font-size: 13px;
    font-weight: 700;
    padding: 1px 6px;
}

#dock .recent { opacity: 0.5; transition: opacity 0.2s ease; }
#dock .recent:hover { opacity: 1.0; }

#dock scrollbar, #dock scrollbar slider {
    min-width: 0px;
    min-height: 0px;
    background: transparent;
}
)";

static const char* CSS_DARK_TEMPLATE = R"(
#dock .dock-wrapper { background-color: rgba(22, 22, 26, 0.85); border-color: $ACCENT; box-shadow: 0 4px 20px rgba(0,0,0,0.5); }
#dock .dock-separator { background: rgba(255,255,255,0.18); }
#dock .sys-monitor { background: transparent; }
#dock .sys-text { color: $ACCENT; }
#dock .ws-label { color: $ACCENT; }
)";

static const char* CSS_LIGHT_TEMPLATE = R"(
#dock.light-theme .dock-wrapper { background-color: rgba(255, 255, 255, 0.85); border-color: $ACCENT_LIGHT; box-shadow: 0 4px 20px rgba(0,0,0,0.15); }
#dock.light-theme .dock-separator { background: rgba(0,0,0,0.15); }
#dock.light-theme .sys-monitor { background: transparent; }
#dock.light-theme .sys-text { color: $ACCENT_LIGHT; }
#dock.light-theme .ws-label { color: $ACCENT_LIGHT; }
#dock.light-theme .active .mac-dot { background-color: $ACCENT_LIGHT; box-shadow: 0 0 5px $ACCENT_LIGHT_A30; }
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

static bool reload_wal_colors_if_changed() {
    const char* home = getenv("HOME");
    if (!home) return false;
    std::string path = std::string(home) + "/.cache/wal/colors.css";

    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (st.st_mtime == last_wal_mtime) return false; // Без змін
    last_wal_mtime = st.st_mtime;

    std::ifstream f(path);
    if (!f.is_open()) return false;

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
    return true;
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
    int r = 0, g = 0, b = 0;
    try {
        r = std::stoi(hex.substr(1,2), nullptr, 16);
        g = std::stoi(hex.substr(3,2), nullptr, 16);
        b = std::stoi(hex.substr(5,2), nullptr, 16);
    } catch (...) {}

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
    std::string accent     = g_wal.color5;   
    std::string accent_a30 = hex_to_rgba(accent, "0.30");

    std::string accent_light     = darken_for_contrast(g_wal.color2);
    std::string accent_light_a30 = hex_to_rgba(accent_light, "0.30");

    auto fill = [&](std::string tmpl) -> std::string {
        auto replace_all = [](std::string s, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return s;
        };
        tmpl = replace_all(tmpl, "$ACCENT_LIGHT_A30", accent_light_a30);
        tmpl = replace_all(tmpl, "$ACCENT_LIGHT",      accent_light);
        tmpl = replace_all(tmpl, "$ACCENT_A30",        accent_a30);
        tmpl = replace_all(tmpl, "$ACCENT",            accent);
        return tmpl;
    };

    return fill(std::string(CSS_BASE)) + fill(CSS_DARK_TEMPLATE) + fill(CSS_LIGHT_TEMPLATE);
}

static void apply_css() {
    std::string css = build_themed_css();
    gtk_css_provider_load_from_string(g_css_provider, css.c_str());
}

static gboolean check_wal_and_theme(gpointer) {
    if (reload_wal_colors_if_changed()) {
        apply_css();
    }
    return G_SOURCE_CONTINUE;
}

// Періодичне повернення невикористаної пам'яті ОС
static gboolean trim_memory_timer(gpointer) {
    malloc_trim(0);
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

// ===================== ACTIVATE =====================
static void activate(GtkApplication* app, gpointer) {
    load_history();

    for (const auto& snap : load_open_snapshot()) {
        bool already = false;
        for (auto& r : recent_apps) if (r.id == snap.id) { already = true; break; }
        if (!already) recent_apps.insert(recent_apps.begin(), snap);
    }
    if ((int)recent_apps.size() > MAX_RECENT_APPS) recent_apps.resize(MAX_RECENT_APPS);
    save_history();

    reload_wal_colors_if_changed();
    g_css_provider = gtk_css_provider_new();
    apply_css();
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(g_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER
    );

    GtkWidget* window = gtk_application_window_new(app);
    g_window = window;
    gtk_widget_set_name(window, "dock");

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

    gtk_box_append(GTK_BOX(apps_container), dock_box);
    gtk_box_append(GTK_BOX(apps_container), recent_box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(apps_scrolled), apps_container);

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
    gtk_box_append(GTK_BOX(wrapper), make_sep());
    gtk_box_append(GTK_BOX(wrapper), sys_box);

    GtkEventController* scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(scroll_controller, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_dock_scroll), apps_scrolled);
    gtk_widget_add_controller(wrapper, scroll_controller);

    GtkWidget* revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 300);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
    gtk_revealer_set_child(GTK_REVEALER(revealer), wrapper);

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(outer), revealer);

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

    auto* sw = new SysWidgets{cpu_label, ram_label};
    update_sysmon(sw);
    g_timeout_add(2500, update_sysmon, sw);

    auto* ahd = new AutohideData{revealer, window};
    g_timeout_add(150, check_autohide, ahd);

    g_timeout_add(3000, check_wal_and_theme, nullptr);
    g_timeout_add(10000, trim_memory_timer, nullptr);

    gtk_window_present(GTK_WINDOW(window));

    // Одразу чистимо купу після первинної побудови віджетів
    malloc_trim(0);
}

// ===================== IPC СЛУХАЧ (ПОДІЇ HYPRLAND) =====================

void listen_hyprland_ipc_dock() {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!xdg || !sig) return;

    std::string socket_path = std::string(xdg) + "/hypr/" + sig + "/.socket2.sock";

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
            ssize_t bytes = read(sock, buffer, sizeof(buffer) - 1);
            if (bytes <= 0) break;

            buffer[bytes] = '\0';
            std::string data = leftover + buffer;
            leftover = "";

            size_t pos = 0;
            while ((pos = data.find('\n')) != std::string::npos) {
                std::string line = data.substr(0, pos);
                data.erase(0, pos + 1);

                if (line.rfind("workspace>>", 0) == 0 ||
                    line.rfind("activewindow>>", 0) == 0 ||
                    line.rfind("closewindow>>", 0) == 0 ||
                    line.rfind("openwindow>>", 0) == 0) {
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
    // Якщо випадково було передано cairo — скидаємо, бо Cairo в GTK4 ламає альфа-прозорість на Wayland
    const char* r = getenv("GSK_RENDERER");
    if (r && strcmp(r, "cairo") == 0) {
        unsetenv("GSK_RENDERER");
    }
    setenv("MALLOC_ARENA_MAX", "1", 0);

    std::thread ipc_thread(listen_hyprland_ipc_dock);
    ipc_thread.detach();

    GtkApplication* app = gtk_application_new("com.megumin.kioskdock", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}