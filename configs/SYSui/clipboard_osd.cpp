#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include "sysui_common.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <sys/stat.h>
#include <cctype>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;

static const std::string CACHE_DIR = "/tmp/ignis_clipboard_cache";
static const int MAX_ITEMS = 30;
static const int WINDOW_WIDTH = 320;
static const int WINDOW_HEIGHT = 450;

GtkWidget* clip_window = nullptr;
GtkWidget* clip_box = nullptr;

struct ClipItem {
    std::string id;
    std::string text;
    bool is_image;
    bool is_file;
};

// ================= УТИЛІТИ =================
static std::string exec(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[256];
    std::string res;
    while (fgets(buf, sizeof(buf), pipe)) res += buf;
    pclose(pipe);
    return res;
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n\v\f");
    if (std::string::npos == first) return "";
    size_t last = str.find_last_not_of(" \t\r\n\v\f");
    return str.substr(first, (last - first + 1));
}

// ================= КЕШ ФОТО =================
static std::string get_cached_image(const std::string& item_id) {
    std::string file_path = CACHE_DIR + "/" + item_id + ".png";
    struct stat buffer;
    if (stat(file_path.c_str(), &buffer) == 0) return file_path;
    
    std::string cmd = "cliphist decode " + item_id + " > " + file_path;
    system(cmd.c_str());
    if (stat(file_path.c_str(), &buffer) == 0) return file_path;
    return "";
}

// ================= ЛОГІКА =================
static void populate_list();

static void restore_item(const std::string& item_id, bool is_file) {
    std::string cmd;
    if (is_file) {
        cmd = "cliphist decode " + item_id + " | wl-copy -t text/uri-list";
    } else {
        cmd = "cliphist decode " + item_id + " | wl-copy";
    }
    system(cmd.c_str());
    gtk_widget_set_visible(clip_window, FALSE);
}

static void delete_item(const std::string& item_id) {
    std::string cmd = "cliphist delete " + item_id;
    system(cmd.c_str());
    std::string p = CACHE_DIR + "/" + item_id + ".png";
    remove(p.c_str());
    populate_list();
}

static void clear_history(GtkWidget* btn, gpointer data) {
    system("cliphist wipe");
    std::string rm_cmd = "rm -rf " + CACHE_DIR;
    system(rm_cmd.c_str());
    mkdir(CACHE_DIR.c_str(), 0755);
    populate_list();
}

static void on_item_click(GtkGestureClick* gesture, int n_press, double x, double y, gpointer user_data) {
    ClipItem* item = static_cast<ClipItem*>(user_data);
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    
    if (button == GDK_BUTTON_PRIMARY) {
        restore_item(item->id, item->is_file);
    } else if (button == GDK_BUTTON_SECONDARY || button == GDK_BUTTON_MIDDLE) {
        delete_item(item->id);
    }
}

static void on_item_hover(GtkEventControllerMotion* controller, double x, double y, gpointer user_data) {
    GtkWidget* widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    gtk_widget_set_cursor_from_name(widget, "pointer");
    gtk_widget_add_css_class(widget, "clip-item-hover");
}

static void on_item_unhover(GtkEventControllerMotion* controller, gpointer user_data) {
    GtkWidget* widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    gtk_widget_set_cursor(widget, nullptr);
    gtk_widget_remove_css_class(widget, "clip-item-hover");
}

static void free_clip_item(gpointer data, GClosure* closure) {
    delete static_cast<ClipItem*>(data);
}

static void populate_list() {
    GtkWidget* child = gtk_widget_get_first_child(clip_box);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(clip_box), child);
        child = next;
    }

    std::string out = exec("cliphist list | head -n " + std::to_string(MAX_ITEMS));
    if (out.empty()) {
        GtkWidget* lbl = gtk_label_new("Пусто... Перетягни файл або скопіюй текст");
        gtk_widget_add_css_class(lbl, "empty-lbl");
        gtk_box_append(GTK_BOX(clip_box), lbl);
        return;
    }

    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        size_t tab_pos = line.find('\t');
        if (tab_pos == std::string::npos) continue;
        
        std::string item_id = line.substr(0, tab_pos);
        std::string raw_text = line.substr(tab_pos + 1);
        
        std::string lower_text = to_lower(raw_text);
        bool is_image = (lower_text.find("binary data") != std::string::npos && raw_text.length() < 150);
        bool is_file = false;
        std::string display_text = raw_text;

        if (!is_image) {
            display_text = trim(raw_text);
            if (display_text.find("file://") == 0) {
                is_file = true;
                size_t slash = display_text.find_last_of('/');
                if (slash != std::string::npos) display_text = display_text.substr(slash + 1);
            } else if (display_text.find("/") == 0) {
                struct stat buf;
                if (stat(display_text.c_str(), &buf) == 0) {
                    is_file = true;
                    size_t slash = display_text.find_last_of('/');
                    if (slash != std::string::npos) display_text = display_text.substr(slash + 1);
                }
            } else {
                if (display_text.length() > 50) {
                    // Обрізаємо по межі UTF-8 символу (раніше різало кирилицю навпіл → «�»)
                    size_t cut = 50;
                    while (cut > 0 && (static_cast<unsigned char>(display_text[cut]) & 0xC0) == 0x80) cut--;
                    display_text = display_text.substr(0, cut) + "...";
                }
            }
        }

        ClipItem* item = new ClipItem{item_id, display_text, is_image, is_file};
        
        GtkWidget* item_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(item_box, "clip-item");
        
        if (is_image) {
            std::string path = get_cached_image(item_id);
            if (!path.empty()) {
                GtkWidget* pic = gtk_picture_new_for_filename(path.c_str());
                gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
                gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
                gtk_widget_set_size_request(pic, -1, 100);
                gtk_widget_add_css_class(pic, "clip-img");
                
                GtkWidget* img_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
                gtk_box_append(GTK_BOX(img_wrap), pic);
                gtk_widget_set_hexpand(img_wrap, TRUE);
                gtk_box_append(GTK_BOX(item_box), img_wrap);
            }
        } else if (is_file) {
            GtkWidget* icon = gtk_image_new_from_icon_name("folder-documents-symbolic");
            gtk_widget_add_css_class(icon, "clip-file-icon");
            gtk_box_append(GTK_BOX(item_box), icon);
            
            GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
            
            GtkWidget* type_lbl = gtk_label_new("Файл");
            gtk_widget_add_css_class(type_lbl, "clip-type-lbl");
            gtk_widget_set_halign(type_lbl, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(vbox), type_lbl);
            
            GtkWidget* text_lbl = gtk_label_new(display_text.c_str());
            gtk_widget_add_css_class(text_lbl, "clip-file-text");
            gtk_widget_set_halign(text_lbl, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(text_lbl), PANGO_ELLIPSIZE_START);
            gtk_label_set_max_width_chars(GTK_LABEL(text_lbl), 20);
            gtk_box_append(GTK_BOX(vbox), text_lbl);
            
            gtk_box_append(GTK_BOX(item_box), vbox);
        } else {
            GtkWidget* icon = gtk_image_new_from_icon_name("text-x-generic-symbolic");
            gtk_widget_add_css_class(icon, "clip-text-icon");
            gtk_box_append(GTK_BOX(item_box), icon);
            
            GtkWidget* text_lbl = gtk_label_new(display_text.c_str());
            gtk_widget_add_css_class(text_lbl, "clip-text");
            gtk_widget_set_halign(text_lbl, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(text_lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(text_lbl), 25);
            gtk_box_append(GTK_BOX(item_box), text_lbl);
        }
        
        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
        g_signal_connect_data(click, "released", G_CALLBACK(on_item_click), item, free_clip_item, G_CONNECT_DEFAULT);
        gtk_widget_add_controller(item_box, GTK_EVENT_CONTROLLER(click));
        
        GtkEventController* motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "enter", G_CALLBACK(on_item_hover), nullptr);
        g_signal_connect(motion, "leave", G_CALLBACK(on_item_unhover), nullptr);
        gtk_widget_add_controller(item_box, motion);
        
        gtk_box_append(GTK_BOX(clip_box), item_box);
    }
}

// ================= IPC =================
static gboolean handle_ipc(gpointer data) {
    gtk_widget_set_visible(clip_window, !gtk_widget_get_visible(clip_window));
    return G_SOURCE_REMOVE;
}

static void ipc_server_thread() {
    int server_fd;
    struct sockaddr_un address;
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == 0) return;

    std::string sock_path = std::string(getenv("HOME")) + "/.config/SYSui/clipboard.sock";
    unlink(sock_path.c_str());

    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, sock_path.c_str());

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 5);

    while (true) {
        int client = accept(server_fd, nullptr, nullptr);
        if (client < 0) continue;

        char buffer[1024] = {0};
        read(client, buffer, 1024);
        std::string cmd(buffer);
        
        if (cmd.find("toggle") != std::string::npos) {
            g_idle_add(handle_ipc, nullptr);
        }
        
        close(client);
    }
}

// ================= CSS =================
static const char* CSS_BASE = R"(
window, window.background {
    background: transparent;
}
.clip-container {
    background-color: rgba(30, 30, 46, 0.9);
    border: 1px solid #313244;
    border-radius: 16px;
    padding: 12px;
    box-shadow: 0 4px 15px rgba(0,0,0,0.5);
    color: #cdd6f4;
}
.clip-header {
    padding-bottom: 8px;
    border-bottom: 1px solid rgba(255,255,255,0.1);
    margin-bottom: 10px;
}
.clip-title {
    font-size: 14px;
    font-weight: bold;
}
.clear-btn {
    color: #f38ba8;
    font-weight: bold;
    font-size: 11px;
    background: transparent;
    border: none;
    box-shadow: none;
    padding: 0;
}
.clear-btn:hover { color: #f9e2af; cursor: pointer; }
.empty-lbl {
    color: #6c7086;
    margin: 20px;
    font-size: 12px;
}
.clip-item {
    padding: 10px;
    margin-bottom: 5px;
    border-radius: 10px;
    background-color: rgba(49, 50, 68, 0.0);
    border: 1px solid rgba(255,255,255,0.05);
    transition: all 0.2s;
}
.clip-item-hover {
    background-color: rgba(255,255,255,0.1);
    border-color: #89b4fa;
}
.clip-img {
    border-radius: 8px;
    border: 1px solid rgba(137, 180, 250, 0.5);
}
.clip-file-icon { color: #f9e2af; font-size: 24px; }
.clip-type-lbl { font-size: 9px; color: #a6adc8; }
.clip-file-text { font-weight: bold; color: #cdd6f4; }
.clip-text-icon { opacity: 0.7; font-size: 16px; }
.clip-text { color: #cdd6f4; }
scrollbar, scrollbar slider {
    min-width: 4px;
    background: rgba(255,255,255,0.1);
    border-radius: 4px;
}
)";

// ================= ВІКНО =================
static void apply_css() {
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, CSS_BASE);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static gboolean on_key_pressed(GtkEventControllerKey* controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    if (keyval == GDK_KEY_Escape) {
        gtk_widget_set_visible(clip_window, FALSE);
        return TRUE;
    }
    return FALSE;
}

static void on_window_show(GObject* object, GParamSpec* pspec, gpointer user_data) {
    if (gtk_widget_get_visible(clip_window)) {
        populate_list();
        
        std::string cur_pos = exec("hyprctl cursorpos -j");
        std::string mon_info = exec("hyprctl monitors -j");
        
        int cx = 0, cy = 0;
        try {
            auto cur = json::parse(cur_pos);
            cx = cur["x"]; cy = cur["y"];
            auto mons = json::parse(mon_info);
            
            json am = mons[0];
            for (auto& m : mons) {
                if (m.value("focused", false)) { am = m; break; }
            }
            
            int mx = am["x"];
            int my = am["y"];
            int mw = am["width"];
            int mh = am["height"];
            
            int tx = (cx - mx) + 15;
            int ty = (cy - my) + 15;
            
            if (tx + WINDOW_WIDTH > mw) tx = mw - WINDOW_WIDTH - 20;
            if (ty + WINDOW_HEIGHT > mh) ty = mh - WINDOW_HEIGHT - 50;
            
            gtk_layer_set_margin(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_LEFT, tx);
            gtk_layer_set_margin(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_TOP, ty);
        } catch(...) {
            gtk_layer_set_margin(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_LEFT, 100);
            gtk_layer_set_margin(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_TOP, 100);
        }
    }
}

static void activate(GtkApplication* app, gpointer user_data) {
    sysui::mem_ready();
    mkdir(CACHE_DIR.c_str(), 0755);
    apply_css();

    clip_window = gtk_application_window_new(app);
    sysui::release_on_hide(clip_window);
    sysui::pop_on_show(clip_window, nullptr, "sysui-pop-down");
    // Єдиний стиль: акцент замість захардкожених кольорів catppuccin + живий hover
    sysui::live_css(".clip-container { border-color: @ACCENT_A30@; }"
                    ".clip-item { transition: transform 280ms cubic-bezier(0.34,1.56,0.64,1), background-color 180ms ease, border-color 180ms ease; }"
                    ".clip-item-hover { border-color: @ACCENT@; background-color: @ACCENT_SOFT@; transform: scale(1.015); }"
                    ".clip-img { border-color: @ACCENT_A50@; }"
                    ".clear-btn:hover { color: @ACCENT@; }");
    gtk_layer_init_for_window(GTK_WINDOW(clip_window));
    gtk_layer_set_namespace(GTK_WINDOW(clip_window), "clipboard");
    
    gtk_layer_set_anchor(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    
    gtk_layer_set_layer(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    
    gtk_widget_set_size_request(clip_window, WINDOW_WIDTH, WINDOW_HEIGHT);
    gtk_widget_set_visible(clip_window, FALSE);

    GtkWidget* container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(container, "clip-container");
    
    // Header
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(header, "clip-header");
    
    GtkWidget* title = gtk_label_new("Буфер обміну");
    gtk_widget_add_css_class(title, "clip-title");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(header), title);
    
    GtkWidget* clear_btn = gtk_button_new_with_label("Очистити");
    gtk_widget_add_css_class(clear_btn, "clear-btn");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(clear_history), nullptr);
    gtk_box_append(GTK_BOX(header), clear_btn);
    
    gtk_box_append(GTK_BOX(container), header);
    
    // Scrolled window
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    
    clip_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clip_box);
    
    gtk_box_append(GTK_BOX(container), scroll);
    
    gtk_window_set_child(GTK_WINDOW(clip_window), container);

    GtkEventController* key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), nullptr);
    gtk_widget_add_controller(clip_window, key_ctrl);
    
    g_signal_connect(clip_window, "notify::visible", G_CALLBACK(on_window_show), nullptr);

    std::thread(ipc_server_thread).detach();
}

int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.clipboard", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}