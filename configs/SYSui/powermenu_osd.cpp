#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include "sysui_common.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstring>

GtkWidget* osd_window = nullptr;
GtkCssProvider* g_css_provider = nullptr;

GtkWidget* overlay_box_ptr = nullptr;
GtkWidget* card_ptr = nullptr;

struct ButtonData {
    GtkWidget* box;
    std::string cmd;
    std::string css_class;
};

struct State {
    std::vector<ButtonData> buttons;
    int current_index = 0;
} g_state;

// ================= PYWAL ТА ТЕМА =================
struct WalColors {
    std::string accent = "#9929CF"; 
};
static WalColors g_wal;

static std::string exec(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[128];
    std::string res;
    while (fgets(buf, sizeof(buf), pipe)) res += buf;
    pclose(pipe);
    return res;
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

void reload_wal_colors() {
    std::ifstream f(std::string(getenv("HOME")) + "/.cache/wal/colors.css");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        std::string v;
        if (!(v = parse_css_var(line, "--color5")).empty()) g_wal.accent = v;
    }
    g_wal.accent = sysui::accent();  // єдиний акцент для всіх модулів
}

// ================= CSS ТА АНІМАЦІЇ =================
void apply_css() {
    if (!g_css_provider) return;
    reload_wal_colors(); 

    std::string scheme = exec("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null");
    bool is_light = (scheme.find("prefer-light") != std::string::npos);

    std::string card_bg = is_light ? "rgba(245, 245, 245, 0.4)" : "rgba(20, 20, 20, 0.4)";
    std::string text_fg = is_light ? "#1a1a1a" : "#ffffff";
    std::string btn_bg = is_light ? "rgba(0, 0, 0, 0.05)" : "rgba(255, 255, 255, 0.05)";
    std::string btn_border = is_light ? "rgba(0, 0, 0, 0.1)" : "rgba(255, 255, 255, 0.1)";

    std::string css = R"(
        window, window.background, window.transparent-window { 
            background-color: rgba(0, 0, 0, 0); 
            background: none; 
            box-shadow: none;
        }
        
        .overlay-bg { 
            background-color: rgba(0, 0, 0, 0); 
            transition: background-color 1000ms ease-in-out; 
        }
        
        window .overlay-bg.fade-to-black {
            background-color: rgb(0, 0, 0);
        }
        
        * { font-family: "JetBrainsMono Nerd Font", "SF Pro Display", sans-serif; outline: none; box-shadow: none; }
        
        .powermenu-card {
            background-color: )" + card_bg + R"(;
            border: 2px solid )" + btn_border + R"(;
            border-radius: 20px;
            padding: 30px;
            opacity: 1.0;
            transform: scale(1.0);
            transition: opacity 500ms ease-out, transform 500ms ease-out; 
        }

        window .powermenu-card.fade-out {
            opacity: 0.0;
            transform: scale(0.9); 
        }

        .card-title {
            color: )" + text_fg + R"(;
            font-size: 24px;
            font-weight: bold;
            margin-bottom: 25px;
        }

        .power-btn {
            min-width: 120px;
            min-height: 120px;
            border-radius: 16px;
            background-color: )" + btn_bg + R"(;
            border: 1px solid )" + btn_border + R"(;
            transition: transform 100ms ease-out; 
            margin: 0 10px;
        }

        .power-btn .icon { color: )" + text_fg + R"(; font-size: 48px; }
        .power-btn .label { color: )" + text_fg + R"(; font-size: 14px; font-weight: bold; margin-top: 10px; }

        .power-btn.selected { transform: scale(1.08); }
        .power-btn.selected .icon, .power-btn.selected .label { color: #121212; }

        .power-btn.btn-danger.selected  { background-color: #f38ba8; border-color: #f38ba8; } 
        .power-btn.btn-warning.selected { background-color: #f9e2af; border-color: #f9e2af; } 
        .power-btn.btn-accent.selected  { background-color: #89b4fa; border-color: #89b4fa; } 
    )";
    
    gtk_css_provider_load_from_string(g_css_provider, css.c_str());
}

// ================= ЛОГІКА ТА АНІМАЦІЯ =================
static bool is_fading = false;
static std::string pending_cmd = "";

void update_selection() {
    for (int i = 0; i < g_state.buttons.size(); i++) {
        if (i == g_state.current_index) {
            gtk_widget_add_css_class(g_state.buttons[i].box, "selected");
        } else {
            gtk_widget_remove_css_class(g_state.buttons[i].box, "selected");
        }
    }
}

gboolean execute_after_fade(gpointer) {
    system(pending_cmd.c_str());
    
    gtk_widget_remove_css_class(overlay_box_ptr, "fade-to-black");
    gtk_widget_remove_css_class(card_ptr, "fade-out");
    gtk_widget_set_visible(osd_window, FALSE);
    is_fading = false;
    
    return G_SOURCE_REMOVE;
}

static void exec_command_and_close() {
    if(g_state.buttons.empty() || is_fading) return;
    is_fading = true; 
    
    pending_cmd = g_state.buttons[g_state.current_index].cmd + " >/dev/null 2>&1 &";
    
    gtk_widget_add_css_class(overlay_box_ptr, "fade-to-black");
    gtk_widget_add_css_class(card_ptr, "fade-out");
    
    g_timeout_add(1000, execute_after_fade, nullptr);
}

// ================= ОБРОБНИКИ ПОДІЙ =================
static gboolean on_key_pressed(GtkEventControllerKey* controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    if (is_fading) return TRUE; 

    int size = g_state.buttons.size();
    if (size == 0) return FALSE;

    if (keyval == GDK_KEY_Left || keyval == GDK_KEY_Up) {
        g_state.current_index = (g_state.current_index - 1 + size) % size;
        update_selection();
        return TRUE;
    } else if (keyval == GDK_KEY_Right || keyval == GDK_KEY_Down || keyval == GDK_KEY_Tab) {
        g_state.current_index = (g_state.current_index + 1) % size;
        update_selection();
        return TRUE;
    } else if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_space) {
        exec_command_and_close();
        return TRUE;
    } else if (keyval == GDK_KEY_Escape) {
        gtk_widget_set_visible(osd_window, FALSE);
        return TRUE;
    }
    return FALSE;
}

static void on_motion(GtkEventControllerMotion* controller, double x, double y, gpointer user_data) {
    if (is_fading) return;
    g_state.current_index = GPOINTER_TO_INT(user_data);
    update_selection();
}

static void on_click(GtkGestureClick* gesture, int n_press, double x, double y, gpointer user_data) {
    if (is_fading) return;
    g_state.current_index = GPOINTER_TO_INT(user_data);
    exec_command_and_close();
}

static void on_overlay_click(GtkGestureClick* gesture, int n_press, double x, double y, gpointer user_data) {
    if (is_fading) return;
    gtk_widget_set_visible(osd_window, FALSE);
}

// ================= UI КОМПОНЕНТИ =================
GtkWidget* create_button(const char* icon_name, const char* label_text, const char* cmd, const char* css_class, int index) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "power-btn");
    gtk_widget_add_css_class(box, "live");
    gtk_widget_add_css_class(box, css_class); 
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(box, "pointer");

    GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    gtk_widget_add_css_class(icon, "icon");
    gtk_widget_set_vexpand(icon, TRUE);
    gtk_widget_set_valign(icon, GTK_ALIGN_END);

    GtkWidget* lbl = gtk_label_new(label_text);
    gtk_widget_add_css_class(lbl, "label");
    gtk_widget_set_vexpand(lbl, TRUE);
    gtk_widget_set_valign(lbl, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), lbl);

    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "released", G_CALLBACK(on_click), GINT_TO_POINTER(index));
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_motion), GINT_TO_POINTER(index));
    gtk_widget_add_controller(box, motion);

    g_state.buttons.push_back({box, cmd, css_class});
    return box;
}

// ================= ОБРОБКА СОКЕТА =================
gboolean on_socket_message(GIOChannel *source, GIOCondition condition, gpointer data) {
    int server_fd = g_io_channel_unix_get_fd(source);
    int client_fd = accept(server_fd, NULL, NULL);
    
    if (client_fd >= 0) {
        char buffer[256] = {0};
        int bytes_read = read(client_fd, buffer, sizeof(buffer)-1);
        if (bytes_read > 0) {
            std::string command(buffer);
            if (command.find("show") != std::string::npos) {
                is_fading = false;
                gtk_widget_remove_css_class(overlay_box_ptr, "fade-to-black");
                gtk_widget_remove_css_class(card_ptr, "fade-out");

                apply_css();
                g_state.current_index = 0;
                update_selection();
                gtk_widget_set_visible(osd_window, TRUE);
                gtk_window_present(GTK_WINDOW(osd_window)); 
            }
        }
        close(client_fd);
    }
    return G_SOURCE_CONTINUE;
}

void setup_ipc_socket(const char* socket_path) {
    unlink(socket_path); 
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return;
    listen(server_fd, 5);
    GIOChannel* channel = g_io_channel_unix_new(server_fd);
    g_io_add_watch(channel, G_IO_IN, on_socket_message, NULL);
}

// ================= ACTIVATE =================
static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    setup_ipc_socket((sysui::home() + "/.config/SYSui/powermenu.sock").c_str());

    g_css_provider = gtk_css_provider_new();
    apply_css();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    osd_window = gtk_application_window_new(app);
    sysui::release_on_hide(osd_window);
    sysui::live_css(".power-btn.btn-accent.selected { background-color: @ACCENT@; border-color: @ACCENT@; }");
    gtk_widget_add_css_class(osd_window, "transparent-window");

    gtk_layer_init_for_window(GTK_WINDOW(osd_window));
    gtk_layer_set_namespace(GTK_WINDOW(osd_window), "powermenu");
    gtk_layer_set_layer(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    
    gtk_layer_set_anchor(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    
    // ФІКС ПЕРЕКРИТТЯ: -1 означає, що менюшка ігнорує зони панелей і лягає поверх них!
    gtk_layer_set_exclusive_zone(GTK_WINDOW(osd_window), -1);
    
    gtk_layer_set_keyboard_mode(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    GtkWidget* overlay_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    overlay_box_ptr = overlay_box; 
    gtk_widget_add_css_class(overlay_box, "overlay-bg");
    gtk_widget_set_valign(overlay_box, GTK_ALIGN_FILL);
    gtk_widget_set_halign(overlay_box, GTK_ALIGN_FILL);

    GtkGesture* overlay_click = gtk_gesture_click_new();
    g_signal_connect(overlay_click, "released", G_CALLBACK(on_overlay_click), nullptr);
    gtk_widget_add_controller(overlay_box, GTK_EVENT_CONTROLLER(overlay_click));

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    card_ptr = card; 
    gtk_widget_add_css_class(card, "powermenu-card");
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(card, TRUE);
    gtk_widget_set_vexpand(card, TRUE);

    GtkGesture* card_click = gtk_gesture_click_new();
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(card_click));

    GtkWidget* title = gtk_label_new("Goodbye!");
    gtk_widget_add_css_class(title, "card-title");
    gtk_box_append(GTK_BOX(card), title);

    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(btn_box), create_button("system-shutdown-symbolic", "Вимкнути", "systemctl poweroff", "btn-danger", 0));
    gtk_box_append(GTK_BOX(btn_box), create_button("system-reboot-symbolic", "Перезапуск", "systemctl reboot", "btn-warning", 1));
    gtk_box_append(GTK_BOX(btn_box), create_button("weather-clear-night-symbolic", "Сон", "systemctl suspend", "btn-accent", 2));

    gtk_box_append(GTK_BOX(card), btn_box);
    gtk_box_append(GTK_BOX(overlay_box), card);
    gtk_window_set_child(GTK_WINDOW(osd_window), overlay_box);

    GtkEventController* key_ctl = gtk_event_controller_key_new();
    g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(on_key_pressed), nullptr);
    gtk_widget_add_controller(osd_window, key_ctl);

    gtk_widget_set_visible(osd_window, FALSE); 
}

int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.powermenu", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}