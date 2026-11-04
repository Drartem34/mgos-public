#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <cerrno>
#include <cstring>
#include "sysui_common.h"

namespace fs = std::filesystem;

// ================= НАЛАШТУВАННЯ =================
const std::string WALLPAPER_DIR = std::string(getenv("HOME")) + "/Pictures/wallpapers";
const std::string CACHE_DIR = std::string(getenv("HOME")) + "/.cache/ignis/wallpaper_thumbs";
const std::string LAST_WP_FILE = std::string(getenv("HOME")) + "/.cache/ignis/last_wallpaper.txt";
const int THUMB_SIZE = 350;

GtkWidget* wp_window = nullptr;
GtkWidget* flow_box = nullptr;
GtkWidget* spinner = nullptr;
GtkWidget* wp_scroll = nullptr;
static void schedule_thumb_update();
GtkCssProvider* g_css_provider = nullptr;
std::atomic<bool> is_loading{false};
std::atomic<uint64_t> generation_id{0}; // збільшується при кожному reload — старі callbacks ігноруються

enum FilterMode { ALL, IMAGES, VIDEOS };
FilterMode current_filter = ALL;

// ================= УТИЛІТИ ТА КОЛЬОРИ =================
struct WalColors {
    std::string accent = "#00e5ff"; 
    std::string bg = "#141414"; 
    std::string fg = "#ffffff"; 
};
static WalColors g_wal;

void exec_cmd(const std::string& cmd) {
    system((cmd + " >/dev/null 2>&1").c_str());
}

// Читаємо wp_anime4k з settings.json
bool read_anime4k_setting() {
    std::string settings_path = std::string(getenv("HOME")) + "/.config/megumin-os/settings.json";
    std::ifstream f(settings_path);
    if (!f.is_open()) return false;
    std::stringstream buf;
    buf << f.rdbuf();
    std::string content = buf.str();
    // Шукаємо "wp_anime4k": true
    auto pos = content.find("\"wp_anime4k\"");
    if (pos == std::string::npos) return false;
    auto colon = content.find(':', pos);
    if (colon == std::string::npos) return false;
    auto val_start = content.find_first_not_of(" \t\n\r", colon + 1);
    if (val_start == std::string::npos) return false;
    return content.substr(val_start, 4) == "true";
}

std::string get_hash(const std::string& str) {
    return std::to_string(std::hash<std::string>{}(str));
}

bool has_ext(const std::string& path, const std::vector<std::string>& exts) {
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

std::string parse_css_var(const std::string& line, const std::string& var_name) {
    auto pos = line.find(var_name + ":");
    if (pos == std::string::npos) return "";
    auto start = line.find('#', pos);
    if (start == std::string::npos) return "";
    auto end = line.find(';', start);
    std::string hex = line.substr(start, (end == std::string::npos ? line.size() : end) - start);
    while (!hex.empty() && (hex.back() == ' ' || hex.back() == ';')) hex.pop_back();
    return hex;
}

std::string hex_to_rgba(const std::string& hex, const std::string& alpha_str) {
    if (hex.size() < 7 || hex[0] != '#') return "rgba(20, 20, 20, " + alpha_str + ")";
    int r = std::stoi(hex.substr(1,2), nullptr, 16);
    int g = std::stoi(hex.substr(3,2), nullptr, 16);
    int b = std::stoi(hex.substr(5,2), nullptr, 16);
    return "rgba(" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + "," + alpha_str + ")";
}

void reload_wal_colors() {
    std::ifstream f(std::string(getenv("HOME")) + "/.cache/wal/colors.css");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        std::string v;
        if (!(v = parse_css_var(line, "--color5")).empty()) g_wal.accent = v;
        if (!(v = parse_css_var(line, "--background")).empty()) g_wal.bg = v;
        if (!(v = parse_css_var(line, "--foreground")).empty()) g_wal.fg = v;
    }
    g_wal.accent = sysui::accent();  // єдиний акцент для всіх модулів
}

// ================= ШПАЛЕРИ =================
void apply_wallpaper(const std::string& path) {
    bool is_video = has_ext(path, {"mp4", "webm", "gif"});
    
    exec_cmd("pkill -9 mpvpaper");
    if (is_video) {
        exec_cmd("killall awww-daemon; rm -f /tmp/mpv-socket");
        
        bool anime4k = read_anime4k_setting();
        std::string mpv_opts;
        if (anime4k) {
            // profile=wallpaper: Mode A шейдери (Clamp + Restore + Upscale + AutoDownscale)
            // Читаються з ~/.config/mpv/mpv.conf профіль [wallpaper]
            mpv_opts = "profile=wallpaper "
                       "input-ipc-server=/tmp/mpv-socket "
                       "loop-file=inf "
                       "hwdec=nvdec "
                       "no-audio "
                       "panscan=1.0 "
                       "cache=no "
                       "demuxer-max-bytes=512KiB "
                       "demuxer-max-back-bytes=128KiB";
        } else {
            // Без шейдерів — мінімальне споживання ресурсів
            mpv_opts = "no-config "
                       "glsl-shaders= "
                       "input-ipc-server=/tmp/mpv-socket "
                       "loop-file=inf "
                       "hwdec=nvdec "
                       "no-audio "
                       "panscan=1.0 "
                       "cache=no "
                       "demuxer-max-bytes=512KiB "
                       "demuxer-max-back-bytes=128KiB";
        }
        exec_cmd("nohup nice -n 19 mpvpaper -o '" + mpv_opts + "' '*' '" + path + "' &");
    } else {
        exec_cmd("pgrep awww-daemon || (awww-daemon & sleep 0.5)");
        exec_cmd("awww img '" + path + "' --resize crop --transition-type fade --transition-angle 30 --transition-fps 60 --transition-duration 2");
    }

    std::ofstream out(LAST_WP_FILE);
    out << path;
    out.close();

    std::thread([path, is_video]() {
        std::string target = path;
        if (is_video) {
            target = std::string(getenv("HOME")) + "/.cache/ignis/sddm_frame.png";
            exec_cmd("ffmpeg -i '" + path + "' -ss 00:00:00.000 -vframes 1 '" + target + "' -y");
        }
        exec_cmd("wal -q -t -i '" + target + "'");
        exec_cmd("killall -SIGUSR2 waybar");
        exec_cmd("sudo -n " + sysui::home() + "/.config/megumin-os/sync_sddm.sh");
    }).detach();

    if (wp_window) gtk_widget_set_visible(wp_window, FALSE);
}

std::string generate_thumbnail_blocking(const std::string& path) {
    std::string thumb_path = CACHE_DIR + "/" + get_hash(path) + ".png";
    if (fs::exists(thumb_path)) return thumb_path;

    std::string source = path;
    if (fs::is_directory(path)) {
        source = path + "/day.jpg"; 
        if (!fs::exists(source)) source = path + "/day.png";
        if (!fs::exists(source)) return path; 
    }

    if (has_ext(source, {"mp4", "webm", "gif"})) {
        exec_cmd("ffmpeg -i '" + source + "' -ss 00:00:00.000 -vframes 1 -s " + std::to_string(THUMB_SIZE) + "x" + std::to_string(THUMB_SIZE) + " '" + thumb_path + "' -y");
    } else {
        exec_cmd("magick '" + source + "' -resize " + std::to_string(THUMB_SIZE) + "x" + std::to_string(THUMB_SIZE) + "^ -gravity center -extent " + std::to_string(THUMB_SIZE) + "x" + std::to_string(THUMB_SIZE) + " '" + thumb_path + "'");
    }
    
    if (fs::exists(thumb_path)) return thumb_path;
    return source;
}

struct CardData {
    std::string thumb;
    std::string original;
    uint64_t gen; // generation_id на момент створення
};

gboolean add_card_to_ui(gpointer data) {
    CardData* cd = static_cast<CardData*>(data);
    
    // Якщо прийшов новий reload — ігноруємо старі callbacks
    if (cd->gen != generation_id.load()) {
        delete cd;
        return G_SOURCE_REMOVE;
    }
    if (!flow_box) { delete cd; return G_SOURCE_REMOVE; }
    
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "wp-card");
    gtk_widget_set_size_request(card, 220, 140);
    
    // ЛІНИВЕ ЗАВАНТАЖЕННЯ: раніше одразу створювалась повна текстура для КОЖНОЇ
    // з ~215 шпалер (350x350 RGBA ≈ 0.5 МБ × 215 ≈ 100 МБ). Тепер картка порожня,
    // а текстура декодується лише коли картка близько до видимої області (update_thumbs).
    GtkWidget* pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
    gtk_widget_set_vexpand(pic, TRUE);
    gtk_widget_add_css_class(pic, "wp-pic");
    
    gtk_box_append(GTK_BOX(card), pic);

    GtkWidget* child = gtk_flow_box_child_new();
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), card);
    
    char* path_dup = g_strdup(cd->original.c_str());
    g_object_set_data_full(G_OBJECT(child), "wp-path", path_dup, g_free);
    g_object_set_data_full(G_OBJECT(child), "wp-thumb", g_strdup(cd->thumb.c_str()), g_free);
    g_object_set_data(G_OBJECT(child), "wp-pic", pic);

    gtk_flow_box_insert(GTK_FLOW_BOX(flow_box), child, -1);
    schedule_thumb_update();

    delete cd;
    return G_SOURCE_REMOVE;
}

// Завантажує текстури для карток у межах [видима область ± пів сторінки]
// і звільняє текстури карток, що далеко. Пам'ять тримається ~постійною
// незалежно від кількості шпалер.
static bool g_thumb_update_pending = false;

static gboolean update_thumbs(gpointer) {
    g_thumb_update_pending = false;
    if (!flow_box || !wp_scroll || !wp_window || !gtk_widget_get_visible(wp_window)) return G_SOURCE_REMOVE;

    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(wp_scroll));
    double val = gtk_adjustment_get_value(adj);
    double page = gtk_adjustment_get_page_size(adj);
    if (page <= 1) { schedule_thumb_update(); return G_SOURCE_REMOVE; }  // ще не розкладено
    double lo = val - page * 0.5, hi = val + page * 1.5;

    double scale = 1.0;
    if (GtkNative* n = gtk_widget_get_native(flow_box))
        if (GdkSurface* s = gtk_native_get_surface(n)) scale = gdk_surface_get_scale(s);

    int loaded_now = 0;
    bool more = false;
    for (GtkWidget* c = gtk_widget_get_first_child(flow_box); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget* pic = (GtkWidget*)g_object_get_data(G_OBJECT(c), "wp-pic");
        if (!pic) continue;
        bool loaded = gtk_picture_get_paintable(GTK_PICTURE(pic)) != nullptr;

        graphene_rect_t b;
        bool near = gtk_widget_get_child_visible(c) &&
                    gtk_widget_compute_bounds(c, flow_box, &b) &&
                    b.origin.y + b.size.height >= lo && b.origin.y <= hi;

        if (near && !loaded) {
            if (loaded_now >= 10) { more = true; continue; }  // не фрізимо UI — порціями
            const char* thumb = (const char*)g_object_get_data(G_OBJECT(c), "wp-thumb");
            int w = std::max(1, gtk_widget_get_width(pic));
            int h = std::max(1, gtk_widget_get_height(pic));
            if (w < 50 || h < 50) { w = 220; h = 140; }
            GdkTexture* t = sysui::load_texture_cover(thumb, (int)(w * scale), (int)(h * scale));
            if (t) { gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(t)); g_object_unref(t); }
            loaded_now++;
        } else if (!near && loaded) {
            gtk_picture_set_paintable(GTK_PICTURE(pic), nullptr);
        }
    }
    if (more) schedule_thumb_update();
    return G_SOURCE_REMOVE;
}

static void schedule_thumb_update() {
    if (g_thumb_update_pending) return;
    g_thumb_update_pending = true;
    g_timeout_add(30, update_thumbs, nullptr);
}

// При закритті панелі звільняємо всі картки й текстури (раніше вони жили до наступного відкриття).
static void on_wp_hide(GtkWidget*, gpointer) {
    ++generation_id;  // зупиняє фоновий скан
    if (flow_box)
        while (GtkWidget* c = gtk_widget_get_first_child(flow_box))
            gtk_flow_box_remove(GTK_FLOW_BOX(flow_box), c);
    is_loading = false;
    if (spinner) gtk_widget_set_visible(spinner, FALSE);
}

gboolean finish_loading(gpointer data) {
    uint64_t gen = (uint64_t)(uintptr_t)data;
    if (gen != generation_id.load()) return G_SOURCE_REMOVE; // застарілий скан
    if (!flow_box || !spinner) return G_SOURCE_REMOVE;
    
    is_loading = false;
    gtk_widget_set_visible(spinner, FALSE);
    
    GtkWidget* first_child = gtk_widget_get_first_child(flow_box);
    if (first_child) {
        gtk_widget_grab_focus(first_child);
        gtk_flow_box_select_child(GTK_FLOW_BOX(flow_box), GTK_FLOW_BOX_CHILD(first_child));
    }
    return G_SOURCE_REMOVE;
}

void scan_wallpapers_thread(uint64_t gen) {
    is_loading = true;
    g_idle_add([](gpointer)->gboolean{ if(spinner) gtk_widget_set_visible(spinner, TRUE); return G_SOURCE_REMOVE; }, nullptr);

    std::vector<std::string> files;
    if (fs::exists(WALLPAPER_DIR)) {
        std::vector<std::string> valid_exts = {"png", "jpg", "jpeg", "webp", "mp4", "webm", "gif"};
        for (const auto& entry : fs::directory_iterator(WALLPAPER_DIR)) {
            std::string path = entry.path().string();
            if (has_ext(path, valid_exts) || fs::is_directory(path)) {
                files.push_back(path);
            }
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& f : files) {
        // Перевіряємо що наш скан ще актуальний перед важкою операцією
        if (gen != generation_id.load()) { is_loading = false; return; }
        
        std::string thumb = generate_thumbnail_blocking(f);
        
        if (gen != generation_id.load()) { is_loading = false; return; } // ще раз після ffmpeg/magick
        
        CardData* cd = new CardData{thumb, f, gen};
        g_idle_add(add_card_to_ui, cd);
    }
    
    g_idle_add(finish_loading, (gpointer)(uintptr_t)gen);
}

void reload_ui() {
    // Збільшуємо generation — всі старі callbacks стануть "застарілими"
    uint64_t gen = ++generation_id;
    
    // Очищаємо flow_box в GTK потоці (ми вже в ньому — IPC callback через GIOChannel)
    if (flow_box) {
        while (GtkWidget* child = gtk_widget_get_first_child(flow_box)) {
            gtk_flow_box_remove(GTK_FLOW_BOX(flow_box), child);
        }
    }

    is_loading = false; // скидаємо — scan_wallpapers_thread поставить знову
    std::thread(scan_wallpapers_thread, gen).detach();
}

// ================= IPC =================
gboolean on_socket_message(GIOChannel *source, GIOCondition condition, gpointer data) {
    int server_fd = g_io_channel_unix_get_fd(source);
    int client_fd = accept(server_fd, NULL, NULL);
    
    if (client_fd >= 0) {
        char buffer[256] = {0};
        int bytes_read = read(client_fd, buffer, sizeof(buffer)-1);
        if (bytes_read > 0) {
            std::string command(buffer);
            if (command.find("toggle") != std::string::npos || command.find("show") != std::string::npos) {
                bool is_vis = gtk_widget_get_visible(wp_window);
                if (!is_vis || command.find("show") != std::string::npos) {
                    reload_ui();
                    gtk_widget_set_visible(wp_window, TRUE);
                    gtk_window_present(GTK_WINDOW(wp_window));
                    
                    g_timeout_add(100, [](gpointer)->gboolean{
                        GtkWidget* first = gtk_widget_get_first_child(flow_box);
                        if (first) {
                            gtk_widget_grab_focus(first);
                        }
                        return G_SOURCE_REMOVE;
                    }, nullptr);
                } else {
                    gtk_widget_set_visible(wp_window, FALSE);
                }
            } else if (command.find("hide") != std::string::npos) {
                gtk_widget_set_visible(wp_window, FALSE);
            }
        }
        close(client_fd);
    }
    return G_SOURCE_CONTINUE;
}

void setup_ipc_socket(const char* socket_path) {
    // Видаляємо старий сокет якщо є (stale socket від попереднього процесу)
    unlink(socket_path);
    
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "[wallpapers] socket() failed: %s\n", strerror(errno));
        return;
    }

    // SO_REUSEADDR щоб не блокував повторний запуск
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[wallpapers] bind() failed: %s\n", strerror(errno));
        close(server_fd);
        return;
    }
    listen(server_fd, 5);

    GIOChannel* channel = g_io_channel_unix_new(server_fd);
    g_io_add_watch(channel, G_IO_IN, on_socket_message, NULL);
}

// ================= UI ТА ПОДІЇ =================
void on_child_activated(GtkFlowBox*, GtkFlowBoxChild* child, gpointer) {
    const char* path = (const char*)g_object_get_data(G_OBJECT(child), "wp-path");
    if (path) {
        apply_wallpaper(std::string(path));
    }
}

gboolean filter_func(GtkFlowBoxChild *child, gpointer data) {
    const char* path = (const char*)g_object_get_data(G_OBJECT(child), "wp-path");
    if (!path) return FALSE;
    bool is_video = has_ext(path, {"mp4", "webm", "gif"});
    if (current_filter == VIDEOS && !is_video) return FALSE;
    if (current_filter == IMAGES && is_video) return FALSE;
    return TRUE;
}

void on_filter_toggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn)) return; // реагуємо тільки на ту кнопку, що стала активною
    long mode = (long)data;
    current_filter = static_cast<FilterMode>(mode);
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(flow_box));
    schedule_thumb_update();
}

static gboolean on_key_pressed(GtkEventControllerKey* controller, guint keyval, guint keycode, GdkModifierType state, gpointer) {
    if (keyval == GDK_KEY_Escape || keycode == 9) {
        gtk_widget_set_visible(wp_window, FALSE);
        return TRUE;
    }
    
    bool handled = false;
    GtkMovementStep step = GTK_MOVEMENT_LOGICAL_POSITIONS;
    int count = 0;

    // Use keycodes for layout-independent WASD
    // W=25, A=38, S=39, D=40
    // Up=111, Down=116, Left=113, Right=114
    // PgUp=112, PgDn=117, Enter=36, KP_Enter=104
    if (keycode == 38 || keycode == 113) { // A or Left
        step = GTK_MOVEMENT_VISUAL_POSITIONS; count = -1; handled = true;
    } else if (keycode == 40 || keycode == 114) { // D or Right
        step = GTK_MOVEMENT_VISUAL_POSITIONS; count = 1; handled = true;
    } else if (keycode == 25 || keycode == 111) { // W or Up
        step = GTK_MOVEMENT_DISPLAY_LINES; count = -1; handled = true;
    } else if (keycode == 39 || keycode == 116) { // S or Down
        step = GTK_MOVEMENT_DISPLAY_LINES; count = 1; handled = true;
    } else if (keycode == 112) { // PgUp
        step = GTK_MOVEMENT_PAGES; count = -1; handled = true;
    } else if (keycode == 117) { // PgDn
        step = GTK_MOVEMENT_PAGES; count = 1; handled = true;
    } else if (keycode == 36 || keycode == 104 || keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) { // Enter
        GList* selected = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(flow_box));
        if (selected) {
            GtkFlowBoxChild* child = GTK_FLOW_BOX_CHILD(selected->data);
            const char* path = (const char*)g_object_get_data(G_OBJECT(child), "wp-path");
            if (path) apply_wallpaper(std::string(path));
            g_list_free(selected);
        }
        return TRUE;
    }

    if (handled) {
        // Якщо фокус десь на кнопці фільтру, повертаємо його в flowbox
        GtkWidget* focus_widget = gtk_window_get_focus(GTK_WINDOW(wp_window));
        bool is_in_flowbox = false;
        GtkWidget* parent = focus_widget;
        while (parent) {
            if (parent == flow_box) { is_in_flowbox = true; break; }
            parent = gtk_widget_get_parent(parent);
        }
        
        if (!is_in_flowbox) {
            GList* selected = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(flow_box));
            if (selected) {
                gtk_widget_grab_focus(GTK_WIDGET(selected->data));
                g_list_free(selected);
            } else {
                GtkWidget* first = gtk_widget_get_first_child(flow_box);
                if (first) gtk_widget_grab_focus(first);
            }
        }
        
        gboolean ret;
        g_signal_emit_by_name(flow_box, "move-cursor", step, count, &ret);
        return TRUE;
    }

    return FALSE;
}

void apply_css() {
    if (!g_css_provider) g_css_provider = gtk_css_provider_new();
    reload_wal_colors();

    std::string bg_rgba = hex_to_rgba(g_wal.bg, "0.65");
    
    std::string css = R"(
        window { background: transparent; }
        * { outline: none; border: none; font-family: "JetBrainsMono Nerd Font", sans-serif; }
        
        .main-box {
            background-color: )" + bg_rgba + R"(;
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 20px;
            padding: 25px;
        }

        flowboxchild {
            border-radius: 12px;
            transition: all 0.2s cubic-bezier(0.25, 0.1, 0.25, 1);
            margin: 10px;
            padding: 0;
            background: transparent;
        }

        flowboxchild:hover, flowboxchild:focus, flowboxchild:selected {
            transform: scale(1.05);
            background: transparent;
            box-shadow: 0 0 0 3px )" + g_wal.accent + R"(;
        }
        
        .wp-card {
            border-radius: 10px;
            /* overflow: hidden — не підтримується GTK4, використовуємо border-radius */
            background-color: #1a1a1a;
        }

        .wp-pic {
            transition: opacity 0.2s;
        }

        button.filter-btn {
            background-color: rgba(255, 255, 255, 0.05);
            color: )" + g_wal.fg + R"(;
            border-radius: 8px;
            padding: 8px 16px;
            margin: 0 5px;
            transition: all 0.2s;
        }

        button.filter-btn:checked {
            background-color: )" + g_wal.accent + R"(;
            color: #121212;
            font-weight: bold;
        }
    )";
    
    gtk_css_provider_load_from_string(g_css_provider, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    fs::create_directories(CACHE_DIR);
    setup_ipc_socket((sysui::home() + "/.config/SYSui/wallpapers.sock").c_str());
    apply_css();

    wp_window = gtk_application_window_new(app);
    gtk_layer_init_for_window(GTK_WINDOW(wp_window));
    gtk_layer_set_namespace(GTK_WINDOW(wp_window), "wallpapers");
    gtk_layer_set_layer(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND); 
    
    // Центруємо вікно з фіксованими відступами або розміром
    gtk_layer_set_anchor(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_EDGE_TOP, 100);
    gtk_layer_set_margin(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_EDGE_BOTTOM, 100);
    gtk_layer_set_margin(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_EDGE_LEFT, 150);
    gtk_layer_set_margin(GTK_WINDOW(wp_window), GTK_LAYER_SHELL_EDGE_RIGHT, 150);

    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_add_css_class(main_box, "main-box");
    
    GtkWidget* header = gtk_label_new("Шпалери");
    gtk_widget_add_css_class(header, "header-title");
    gtk_widget_set_halign(header, GTK_ALIGN_CENTER);
    std::string header_css = ".header-title { font-size: 24px; font-weight: bold; color: " + g_wal.fg + "; margin-bottom: 10px; }";
    gtk_css_provider_load_from_string(g_css_provider, (std::string(gtk_css_provider_to_string(g_css_provider)) + header_css).c_str());
    gtk_box_append(GTK_BOX(main_box), header);

    GtkWidget* filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(filter_box, GTK_ALIGN_CENTER);

    GtkWidget* btn_all = gtk_toggle_button_new_with_label("Усі");
    GtkWidget* btn_img = gtk_toggle_button_new_with_label("Картинки");
    GtkWidget* btn_vid = gtk_toggle_button_new_with_label("Відео");

    gtk_widget_add_css_class(btn_all, "filter-btn");
    gtk_widget_add_css_class(btn_img, "filter-btn");
    gtk_widget_add_css_class(btn_vid, "filter-btn");

    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn_img), GTK_TOGGLE_BUTTON(btn_all));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn_vid), GTK_TOGGLE_BUTTON(btn_all));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_all), TRUE);

    g_signal_connect(btn_all, "toggled", G_CALLBACK(on_filter_toggled), (gpointer)(long)ALL);
    g_signal_connect(btn_img, "toggled", G_CALLBACK(on_filter_toggled), (gpointer)(long)IMAGES);
    g_signal_connect(btn_vid, "toggled", G_CALLBACK(on_filter_toggled), (gpointer)(long)VIDEOS);

    gtk_box_append(GTK_BOX(filter_box), btn_all);
    gtk_box_append(GTK_BOX(filter_box), btn_img);
    gtk_box_append(GTK_BOX(filter_box), btn_vid);
    
    gtk_box_append(GTK_BOX(main_box), filter_box);

    GtkWidget* scroll = gtk_scrolled_window_new();
    wp_scroll = scroll;
    {
        GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        auto cb = +[](GObject*, GParamSpec*, gpointer) { schedule_thumb_update(); };
        g_signal_connect(adj, "value-changed", G_CALLBACK(+[](GtkAdjustment*, gpointer) { schedule_thumb_update(); }), nullptr);
        g_signal_connect(adj, "notify::upper", G_CALLBACK(cb), nullptr);
        g_signal_connect(adj, "notify::page-size", G_CALLBACK(cb), nullptr);
    }
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);

    flow_box = gtk_flow_box_new();
    gtk_widget_set_valign(flow_box, GTK_ALIGN_START);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow_box), 5);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow_box), GTK_SELECTION_SINGLE);
    gtk_widget_set_halign(flow_box, GTK_ALIGN_CENTER);
    
    gtk_flow_box_set_filter_func(GTK_FLOW_BOX(flow_box), filter_func, NULL, NULL);
    gtk_widget_set_halign(flow_box, GTK_ALIGN_CENTER);
    
    g_signal_connect(flow_box, "child-activated", G_CALLBACK(on_child_activated), NULL);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), flow_box);
    gtk_box_append(GTK_BOX(main_box), scroll);

    spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_visible(spinner, FALSE);
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(main_box), spinner);

    GtkEventController* key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    gtk_widget_add_controller(wp_window, key_ctrl);

    gtk_window_set_child(GTK_WINDOW(wp_window), main_box);
    // ВАЖЛИВО: realize без показу — реєструє layer-shell surface в Wayland
    // Без цього gtk_widget_set_visible(TRUE) не спрацює після FALSE
    g_signal_connect(wp_window, "hide", G_CALLBACK(on_wp_hide), nullptr);
    sysui::release_on_hide(wp_window);
    gtk_widget_realize(wp_window);
    gtk_widget_set_visible(wp_window, FALSE);
}

int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.wallpapers", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    g_application_hold(G_APPLICATION(app)); 
    return g_application_run(G_APPLICATION(app), argc, argv);
}
