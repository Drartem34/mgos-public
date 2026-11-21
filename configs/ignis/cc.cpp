#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

GtkWidget* cc_window = nullptr;
GtkCssProvider* g_css_provider = nullptr;
bool is_visible = false;

// ===================== ТЕМА =====================
struct WalColors {
    std::string fg = "#ffffff"; 
    std::string accent = "#ff9800"; // Дефолтний оранжевий як на скріні
    std::string accent_bg = "rgba(255, 152, 0, 0.8)";
};
static WalColors g_wal;

static std::string exec(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[128];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

std::string parse_css_var(const std::string& line, const std::string& var_name) {
    auto pos = line.find(var_name + ":");
    if (pos == std::string::npos) return "";
    auto start = line.find('#', pos);
    if (start == std::string::npos) return "";
    auto end = line.find(';', start);
    if (end == std::string::npos) end = line.size();
    std::string val = line.substr(start, end - start);
    while (!val.empty() && (val.back() == ' ' || val.back() == ';')) val.pop_back();
    return val;
}

void reload_wal_colors() {
    std::ifstream f(std::string(getenv("HOME")) + "/.cache/wal/colors.css");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        std::string v;
        if (!(v = parse_css_var(line, "--color3")).empty()) {
            g_wal.accent = v;
            // Робимо напівпрозорий варіант акценту
            g_wal.accent_bg = "alpha(" + v + ", 0.9)";
        }
        if (!(v = parse_css_var(line, "--foreground")).empty()) g_wal.fg = v;
    }
}

void apply_css() {
    if (!g_css_provider) return;
    reload_wal_colors(); 

    std::string css = R"(
        window {
            background: transparent;
        }
        * {
            font-family: "JetBrainsMono Nerd Font", "SF Pro Display", sans-serif;
            outline: none;
            box-shadow: none;
        }

        /* Головний контейнер (усі плашки) */
        .cc-container {
            padding: 20px;
        }

        /* Базова напівпрозора плашка */
        .cc-block {
            background-color: rgba(255, 255, 255, 0.25);
            border-radius: 20px;
            padding: 12px 16px;
            box-shadow: 0 4px 15px rgba(0,0,0,0.1);
        }

        /* Активна плашка (як Wi-Fi на твоєму скріні) */
        .cc-block-active {
            background-color: )" + g_wal.accent_bg + R"(;
            color: #ffffff;
        }

        .cc-title { font-weight: bold; font-size: 15px; }
        .cc-sub { font-size: 11px; opacity: 0.8; }
        .cc-icon { font-size: 20px; margin-right: 10px; }

        /* Кнопки керування медіа */
        .media-btn {
            background: transparent;
            border-radius: 100px;
            min-width: 36px;
            min-height: 36px;
            padding: 0;
            color: white;
        }
        .media-btn:hover { background: rgba(255,255,255,0.2); }

        /* Маленькі швидкі перемикачі (ряд із 7 кнопок) */
        .quick-btn {
            background-color: rgba(255, 255, 255, 0.85);
            color: #121212;
            border-radius: 12px;
            min-width: 40px;
            min-height: 40px;
            padding: 0;
        }
        .quick-btn:hover { background-color: #ffffff; }

        /* ІДЕАЛЬНІ ТОВСТІ СЛАЙДЕРИ ЯК В IOS */
        scale.thick-slider trough {
            min-height: 14px;
            background-color: rgba(255, 255, 255, 0.4);
            border-radius: 10px;
        }
        scale.thick-slider highlight {
            background-color: )" + g_wal.accent + R"(;
            border-radius: 10px;
        }
        /* Ховаємо сам кружечок (повзунок) */
        scale.thick-slider slider {
            min-width: 0px;
            min-height: 0px;
            background: transparent;
            border: none;
            box-shadow: none;
            margin: 0;
            padding: 0;
        }
    )";
    
    gtk_css_provider_load_from_string(g_css_provider, css.c_str());
}

// ===================== БУДІВНИЦТВО БЛОКІВ =====================

GtkWidget* make_notification_block() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "cc-block");

    GtkWidget* icon = gtk_image_new_from_icon_name("mail-unread-symbolic");
    gtk_widget_set_pixel_size(GTK_IMAGE(icon), 24);
    gtk_widget_add_css_class(icon, "cc-icon");

    GtkWidget* text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);
    
    GtkWidget* title = gtk_label_new("Сповіщення");
    gtk_widget_add_css_class(title, "cc-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    
    GtkWidget* sub = gtk_label_new("1 сповіщення");
    gtk_widget_add_css_class(sub, "cc-sub");
    gtk_widget_set_halign(sub, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(text_box), title);
    gtk_box_append(GTK_BOX(text_box), sub);

    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);

    GtkWidget* arrow = gtk_image_new_from_icon_name("external-link-symbolic");
    
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), text_box);
    gtk_box_append(GTK_BOX(box), spacer);
    gtk_box_append(GTK_BOX(box), arrow);

    return box;
}

GtkWidget* make_media_block() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "cc-block");

    // Тут буде обкладинка альбому
    GtkWidget* art = gtk_image_new_from_icon_name("folder-music-symbolic");
    gtk_widget_set_pixel_size(GTK_IMAGE(art), 36);
    gtk_widget_add_css_class(art, "cc-icon");

    GtkWidget* text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);
    
    GtkWidget* title = gtk_label_new("Боюся тебе");
    gtk_widget_add_css_class(title, "cc-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    
    GtkWidget* sub = gtk_label_new("Твій зайчик пише");
    gtk_widget_add_css_class(sub, "cc-sub");
    gtk_widget_set_halign(sub, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(text_box), title);
    gtk_box_append(GTK_BOX(text_box), sub);

    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);

    GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_valign(controls, GTK_ALIGN_CENTER);

    GtkWidget* prev = gtk_button_new_from_icon_name("media-skip-backward-symbolic");
    GtkWidget* play = gtk_button_new_from_icon_name("media-playback-pause-symbolic");
    GtkWidget* next = gtk_button_new_from_icon_name("media-skip-forward-symbolic");
    
    gtk_widget_add_css_class(prev, "media-btn");
    gtk_widget_add_css_class(play, "media-btn");
    gtk_widget_add_css_class(next, "media-btn");

    gtk_box_append(GTK_BOX(controls), prev);
    gtk_box_append(GTK_BOX(controls), play);
    gtk_box_append(GTK_BOX(controls), next);

    gtk_box_append(GTK_BOX(box), art);
    gtk_box_append(GTK_BOX(box), text_box);
    gtk_box_append(GTK_BOX(box), spacer);
    gtk_box_append(GTK_BOX(box), controls);

    return box;
}

GtkWidget* make_conn_block() {
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(hbox), TRUE);

    // Wi-Fi (Активний)
    GtkWidget* wifi_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(wifi_box, "cc-block");
    gtk_widget_add_css_class(wifi_box, "cc-block-active"); // Оранжевий
    
    GtkWidget* wifi_icon = gtk_image_new_from_icon_name("network-wireless-symbolic");
    gtk_widget_add_css_class(wifi_icon, "cc-icon");
    
    GtkWidget* wifi_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* w_title = gtk_label_new("WIFI / мережа");
    gtk_widget_add_css_class(w_title, "cc-title");
    gtk_widget_set_halign(w_title, GTK_ALIGN_START);
    GtkWidget* w_sub = gtk_label_new("name");
    gtk_widget_add_css_class(w_sub, "cc-sub");
    gtk_widget_set_halign(w_sub, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(wifi_text), w_title);
    gtk_box_append(GTK_BOX(wifi_text), w_sub);

    gtk_box_append(GTK_BOX(wifi_box), wifi_icon);
    gtk_box_append(GTK_BOX(wifi_box), wifi_text);

    // Bluetooth (Неактивний)
    GtkWidget* bt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(bt_box, "cc-block");
    
    GtkWidget* bt_icon = gtk_image_new_from_icon_name("bluetooth-active-symbolic");
    gtk_widget_add_css_class(bt_icon, "cc-icon");
    
    GtkWidget* bt_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* b_title = gtk_label_new("Bluetooth");
    gtk_widget_add_css_class(b_title, "cc-title");
    gtk_widget_set_halign(b_title, GTK_ALIGN_START);
    GtkWidget* b_sub = gtk_label_new("name");
    gtk_widget_add_css_class(b_sub, "cc-sub");
    gtk_widget_set_halign(b_sub, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(bt_text), b_title);
    gtk_box_append(GTK_BOX(bt_text), b_sub);

    gtk_box_append(GTK_BOX(bt_box), bt_icon);
    gtk_box_append(GTK_BOX(bt_box), bt_text);

    gtk_box_append(GTK_BOX(hbox), wifi_box);
    gtk_box_append(GTK_BOX(hbox), bt_box);

    return hbox;
}

GtkWidget* make_quick_toggles() {
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(hbox), TRUE);

    const char* icons[] = {
        "speedometer-symbolic", 
        "temperature-symbolic", 
        "eco-symbolic", 
        "display-brightness-symbolic", 
        "battery-level-80-symbolic", 
        "emblem-system-symbolic", 
        "system-shutdown-symbolic"
    };

    for (int i = 0; i < 7; ++i) {
        GtkWidget* btn = gtk_button_new_from_icon_name(icons[i]);
        gtk_widget_add_css_class(btn, "quick-btn");
        gtk_box_append(GTK_BOX(hbox), btn);
    }

    return hbox;
}

GtkWidget* make_slider_block(const char* icon_name, int default_val) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_widget_add_css_class(box, "cc-block");

    GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_set_pixel_size(GTK_IMAGE(icon), 24);
    
    GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_range_set_value(GTK_RANGE(scale), default_val);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(scale, "thick-slider");

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), scale);

    return box;
}

// ===================== ОСНОВНЕ ВІКНО =====================

void build_ui(GtkApplication* app) {
    g_css_provider = gtk_css_provider_new();
    apply_css();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    cc_window = gtk_application_window_new(app);
    gtk_layer_init_for_window(GTK_WINDOW(cc_window));
    gtk_layer_set_namespace(GTK_WINDOW(cc_window), "control_center");
    gtk_layer_set_layer(GTK_WINDOW(cc_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    
    // Вішаємо справа знизу (можеш змінити на TOP, якщо треба)
    gtk_layer_set_anchor(GTK_WINDOW(cc_window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(cc_window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(cc_window), GTK_LAYER_SHELL_EDGE_BOTTOM, 20); 
    gtk_layer_set_margin(GTK_WINDOW(cc_window), GTK_LAYER_SHELL_EDGE_RIGHT, 20);
    
    gtk_layer_set_keyboard_mode(GTK_WINDOW(cc_window), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(main_box, "cc-container");
    gtk_widget_set_size_request(main_box, 450, -1); // Ширина панелі

    // Додаємо блоки як на скріні
    gtk_box_append(GTK_BOX(main_box), make_notification_block());
    gtk_box_append(GTK_BOX(main_box), make_media_block());
    gtk_box_append(GTK_BOX(main_box), make_conn_block());
    gtk_box_append(GTK_BOX(main_box), make_quick_toggles());
    gtk_box_append(GTK_BOX(main_box), make_slider_block("audio-volume-high-symbolic", 65)); // Гучність
    gtk_box_append(GTK_BOX(main_box), make_slider_block("audio-input-microphone-symbolic", 85)); // Мікрофон

    gtk_window_set_child(GTK_WINDOW(cc_window), main_box);

    // Закриття на ESC
    GtkEventController* win_keys = gtk_event_controller_key_new();
    g_signal_connect(win_keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer) -> gboolean {
        if (keyval == GDK_KEY_Escape) {
            is_visible = false;
            gtk_widget_set_visible(cc_window, FALSE);
            return TRUE;
        }
        return FALSE;
    }), nullptr);
    gtk_widget_add_controller(cc_window, win_keys);

    gtk_widget_set_visible(cc_window, FALSE);
}

// ===================== SOCKET IPC =====================
gboolean on_socket_message(GIOChannel *source, GIOCondition condition, gpointer data) {
    int server_fd = g_io_channel_unix_get_fd(source);
    int client_fd = accept(server_fd, NULL, NULL);
    
    if (client_fd >= 0) {
        char buffer[256] = {0};
        int bytes_read = read(client_fd, buffer, sizeof(buffer)-1);
        if (bytes_read > 0) {
            std::string cmd(buffer);
            if (cmd.find("toggle") != std::string::npos) {
                is_visible = !is_visible;
                if (is_visible) {
                    apply_css();
                    gtk_widget_set_visible(cc_window, TRUE);
                    gtk_window_present(GTK_WINDOW(cc_window)); 
                } else {
                    gtk_widget_set_visible(cc_window, FALSE);
                }
            }
        }
        close(client_fd);
    }
    return G_SOURCE_CONTINUE;
}

void setup_ipc_socket() {
    std::string socket_path = std::string(getenv("HOME")) + "/.config/SYSui/cc.sock";
    unlink(socket_path.c_str()); 
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path)-1);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return;
    listen(server_fd, 5);
    GIOChannel* channel = g_io_channel_unix_new(server_fd);
    g_io_add_watch(channel, G_IO_IN, on_socket_message, NULL);
}

static void activate(GtkApplication* app, gpointer) {
    static bool init = false;
    if (init) return;
    init = true;
    setup_ipc_socket();
    build_ui(app);
}

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("com.megumin.cc", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}