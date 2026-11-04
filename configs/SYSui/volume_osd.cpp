#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include "sysui_common.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <fstream>
#include <algorithm>
#include <cstring> // <--- БЕЗ ЦЬОГО MEMSET НЕ ПРАЦЮЄ

GtkWidget* osd_window = nullptr;
GtkWidget* progress_bar = nullptr;
GtkWidget* icon_label = nullptr;
GtkWidget* percent_label = nullptr;
guint hide_timer_id = 0;
GtkCssProvider* g_css_provider = nullptr;

// ================= PYWAL ТА КОЛЬОРИ =================
struct WalColors {
    std::string accent = "#ffffff"; 
    std::string bg = "#141414"; 
    std::string fg = "#ffffff"; 
};
static WalColors g_wal;

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

// ================= ЛОГІКА =================
static std::string exec_cmd(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[128];
    std::string res;
    while (fgets(buf, sizeof(buf), pipe)) res += buf;
    pclose(pipe);
    if (!res.empty() && res.back() == '\n') res.pop_back();
    return res;
}

void update_volume_ui() {
    std::string out = exec_cmd("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    
    bool muted = (out.find("[MUTED]") != std::string::npos);
    float vol = 0.0f;
    
    size_t pos = out.find("Volume: ");
    if (pos != std::string::npos) {
        std::string v_str = out.substr(pos + 8);
        std::replace(v_str.begin(), v_str.end(), ',', '.'); 
        vol = g_ascii_strtod(v_str.c_str(), nullptr);
    }
    
    float bar_vol = (vol > 1.0f) ? 1.0f : vol; 
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), bar_vol);
    
    if (vol > 1.0f) {
        gtk_widget_add_css_class(progress_bar, "overvol");
        gtk_label_set_text(GTK_LABEL(percent_label), (std::to_string((int)(vol * 100)) + "%").c_str());
        gtk_widget_set_visible(percent_label, TRUE);
    } else {
        gtk_widget_remove_css_class(progress_bar, "overvol");
        gtk_widget_set_visible(percent_label, FALSE);
    }
    
    if (muted || vol == 0.0f) gtk_label_set_text(GTK_LABEL(icon_label), "󰝟"); 
    else if (vol < 0.3f) gtk_label_set_text(GTK_LABEL(icon_label), "󰕿"); 
    else if (vol < 0.7f) gtk_label_set_text(GTK_LABEL(icon_label), "󰖀"); 
    else gtk_label_set_text(GTK_LABEL(icon_label), "󰕾"); 
}

// ================= CSS =================
void apply_css() {
    if (!g_css_provider) return;
    reload_wal_colors(); 

    std::string theme_str = exec_cmd("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null");
    bool is_light = (theme_str.find("prefer-light") != std::string::npos);

    std::string bg_rgba, fg_color, fg_trough;

    if (is_light) {
        bg_rgba = "rgba(245, 245, 245, 0.7)";
        fg_color = "#1a1a1a";
        fg_trough = "rgba(0, 0, 0, 0.15)";
    } else {
        bg_rgba = hex_to_rgba(g_wal.bg, "0.5");
        fg_color = g_wal.fg;
        fg_trough = hex_to_rgba(g_wal.fg, "0.2");
    }

    std::string css = R"(
        window { background: transparent; }
        * { font-family: "JetBrainsMono Nerd Font", "SF Pro Display", sans-serif; outline: none; border: none; }
        
        .osd-box {
            background-color: )" + bg_rgba + R"(;
            border: 1px solid rgba(128, 128, 128, 0.2);
            border-radius: 15px;
            padding: 0 15px;
        }

        .icon { color: )" + fg_color + R"(; font-size: 20px; margin-right: 12px; }
        
        progressbar trough { 
            background-color: )" + fg_trough + R"(; 
            min-height: 4px; 
            min-width: 4px; /* ЖОРСТКИЙ ЛІМІТ РОЗМІРУ */
            border-radius: 2px; 
            border: none;
            box-shadow: none;
        }
        
        progressbar progress { 
            background-color: )" + g_wal.accent + R"(; 
            min-height: 4px;
            min-width: 4px; /* ЖОРСТКИЙ ЛІМІТ РОЗМІРУ */
            border-radius: 2px; 
            transition: all 0.3s cubic-bezier(0.25, 0.1, 0.25, 1); 
            border: none;
            box-shadow: none;
        }
        
        progressbar.overvol progress {
            background-color: #f38ba8;
        }

        .percent-lbl {
            color: #f38ba8;
            font-size: 14px;
            font-weight: bold;
            margin-left: 12px;
        }
    )";
    
    gtk_css_provider_load_from_string(g_css_provider, css.c_str());
}

// ================= ОБРОБКА СОКЕТА =================
gboolean hide_osd(gpointer) {
    gtk_widget_set_visible(osd_window, FALSE);
    hide_timer_id = 0;
    return G_SOURCE_REMOVE;
}

gboolean on_socket_message(GIOChannel *source, GIOCondition condition, gpointer data) {
    int server_fd = g_io_channel_unix_get_fd(source);
    int client_fd = accept(server_fd, NULL, NULL);
    
    if (client_fd >= 0) {
        char buffer[256] = {0};
        int bytes_read = read(client_fd, buffer, sizeof(buffer)-1);
        
        if (bytes_read > 0) {
            std::string command(buffer);
            std::cout << "[DEBUG] Прилетіло в сокет: '" << command << "'" << std::endl;

            if (command.find("show") != std::string::npos) {
                std::cout << "[DEBUG] Команда прийнята! Малюю UI..." << std::endl;
                apply_css();
                update_volume_ui();
                
                gtk_widget_set_visible(osd_window, TRUE);
                gtk_window_present(GTK_WINDOW(osd_window)); 
                
                if (hide_timer_id > 0) g_source_remove(hide_timer_id);
                hide_timer_id = g_timeout_add(2000, hide_osd, nullptr);
            }
        }
        close(client_fd);
    }
    return G_SOURCE_CONTINUE;
}

void setup_ipc_socket(const char* socket_path) {
    unlink(socket_path); 
    
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[ERROR] Не вдалося створити сокет!" << std::endl;
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr)); // Очищення пам'яті
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Помилка bind! Файл сокета не створено." << std::endl;
        return;
    }
    
    listen(server_fd, 5);

    GIOChannel* channel = g_io_channel_unix_new(server_fd);
    g_io_add_watch(channel, G_IO_IN, on_socket_message, NULL);
    
    std::cout << "[DEBUG] Поштова скринька успішно відкрита: " << socket_path << std::endl;
}

// ================= ACTIVATE & MAIN =================
static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    setup_ipc_socket((sysui::home() + "/.config/SYSui/volume.sock").c_str());

    g_css_provider = gtk_css_provider_new();
    apply_css();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    osd_window = gtk_application_window_new(app);
    sysui::release_on_hide(osd_window);
    sysui::pop_on_show(osd_window, nullptr);
    gtk_layer_init_for_window(GTK_WINDOW(osd_window));
    gtk_layer_set_namespace(GTK_WINDOW(osd_window), "volume_osd"); 
    gtk_layer_set_layer(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    
    gtk_layer_set_anchor(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_BOTTOM, 120); 

    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(main_box, "osd-box");
    gtk_widget_set_size_request(main_box, 200, 60);
    gtk_widget_set_halign(main_box, GTK_ALIGN_CENTER);

    icon_label = gtk_label_new("󰕾");
    gtk_widget_add_css_class(icon_label, "icon");

    progress_bar = gtk_progress_bar_new();
    gtk_widget_set_valign(progress_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(progress_bar, TRUE); 

    percent_label = gtk_label_new("100%");
    gtk_widget_add_css_class(percent_label, "percent-lbl");
    gtk_widget_set_visible(percent_label, FALSE);

    gtk_box_append(GTK_BOX(main_box), icon_label);
    gtk_box_append(GTK_BOX(main_box), progress_bar);
    gtk_box_append(GTK_BOX(main_box), percent_label);

    gtk_window_set_child(GTK_WINDOW(osd_window), main_box);
    gtk_widget_set_visible(osd_window, FALSE); 
}

int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.volume", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    g_application_hold(G_APPLICATION(app));
    return g_application_run(G_APPLICATION(app), argc, argv);
}