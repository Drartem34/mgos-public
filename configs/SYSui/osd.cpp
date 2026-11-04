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
#include <cstring> // Для очищення пам'яті (memset)

GtkWidget* osd_window = nullptr;
GtkWidget* title_label = nullptr;
GtkWidget* artist_label = nullptr;
GtkWidget* art_box = nullptr;

guint hide_timer_id = 0;
std::string current_art_url = "";
GtkCssProvider* art_css_provider = nullptr;

// ================= ЛОГІКА ПЛЕЄРА =================
static std::string exec(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[256];
    std::string res;
    while (fgets(buf, sizeof(buf), pipe)) res += buf;
    pclose(pipe);
    if (!res.empty() && res.back() == '\n') res.pop_back();
    return res;
}

void update_music_ui() {
    if (!art_css_provider) {
        art_css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(art_css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    std::string player = "spotify";
    std::string status = exec("playerctl -p spotify status 2>/dev/null");
    if (status.find("Playing") == std::string::npos) {
        std::string active = exec("playerctl -a status | grep Playing | head -n 1 | cut -d' ' -f1");
        if (!active.empty()) player = active;
    }

    std::string p_arg = "-p " + player;
    std::string title = exec("playerctl " + p_arg + " metadata --format '{{title}}' 2>/dev/null");
    std::string artist = exec("playerctl " + p_arg + " metadata --format '{{artist}}' 2>/dev/null");
    std::string art_url = exec("playerctl " + p_arg + " metadata --format '{{mpris:artUrl}}' 2>/dev/null");
    
    gtk_label_set_text(GTK_LABEL(title_label), title.empty() ? "..." : title.c_str());
    gtk_label_set_text(GTK_LABEL(artist_label), artist.empty() ? "..." : artist.c_str());

    if (!art_url.empty() && art_url != current_art_url) {
        current_art_url = art_url;
        
        if (art_url.find("googleusercontent.com") != std::string::npos || art_url.find("spotify") != std::string::npos) {
             art_url = "https://i.scdn.co/image/" + art_url.substr(art_url.find_last_of('/') + 1);
        }

        std::string hash_str = std::to_string(std::hash<std::string>{}(art_url));
        std::string path = "/tmp/m_art_" + hash_str + ".png";

        if (art_url.find("file://") == 0) {
            path = art_url.substr(7);
        } else {
            std::ifstream f(path);
            if (!f.good()) {
                exec("curl -sL '" + art_url + "' -o " + path);
            }
        }
        
        std::string css = ".art-box { background-image: url('file://" + path + "'); background-size: cover; background-position: center; }";
        gtk_css_provider_load_from_string(art_css_provider, css.c_str());
    }
}

// ================= CSS =================
void apply_css() {
    GtkCssProvider* provider = gtk_css_provider_new();
    std::string css = R"(
        window { background: transparent; }
        * { font-family: "JetBrainsMono Nerd Font", "SF Pro Display", sans-serif; outline: none; }
        
        .osd-box {
            background-color: rgba(20, 20, 20, 0.5); 
            border: 1px solid rgba(255, 255, 255, 0.2);
            border-radius: 17px;
            padding: 5px 10px 5px 5px;
        }

        .art-box {
            border-radius: 7px;
            margin-right: 10px;
            min-width: 50px;
            min-height: 50px;
            max-width: 50px;
            max-height: 50px;
            background-color: rgba(0,0,0,0.3);
        }

        .title-label { color: #ffffff; font-size: 12px; font-weight: 900; }
        .artist-label { color: rgba(255,255,255,0.75); font-size: 9px; font-weight: 600; }
    )";
    gtk_css_provider_load_from_string(provider, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

// ================= ОБРОБКА СОКЕТА =================
gboolean hide_osd(gpointer data) {
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
            
            if (command.find("show") != std::string::npos) {
                update_music_ui();
                gtk_widget_set_visible(osd_window, TRUE);
                gtk_window_present(GTK_WINDOW(osd_window)); 
                
                if (hide_timer_id > 0) g_source_remove(hide_timer_id);
                hide_timer_id = g_timeout_add(3000, hide_osd, nullptr);
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
    memset(&addr, 0, sizeof(addr)); // Очищення пам'яті
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return;
    
    listen(server_fd, 5);

    GIOChannel* channel = g_io_channel_unix_new(server_fd);
    g_io_add_watch(channel, G_IO_IN, on_socket_message, NULL);
}

// ================= ACTIVATE & MAIN =================
static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    // Вмикаємо сокет
    setup_ipc_socket((sysui::home() + "/.config/SYSui/music.sock").c_str());
    
    apply_css();

    osd_window = gtk_application_window_new(app);
    sysui::release_on_hide(osd_window);
    sysui::pop_on_show(osd_window, nullptr);
    sysui::live_css(".osd-box { border-color: @ACCENT_A30@; }");
    gtk_layer_init_for_window(GTK_WINDOW(osd_window));
    gtk_layer_set_namespace(GTK_WINDOW(osd_window), "megumin_osd");
    gtk_layer_set_layer(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_LAYER_OVERLAY);

    gtk_layer_set_anchor(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_BOTTOM, 120);

    GtkWidget* osd_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(osd_box, "osd-box");
    gtk_widget_set_size_request(osd_box, 200, 60);
    gtk_widget_set_halign(osd_box, GTK_ALIGN_CENTER);

    art_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(art_box, "art-box");
    gtk_box_append(GTK_BOX(osd_box), art_box);

    GtkWidget* center_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(center_box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(center_box, TRUE); 

    title_label = gtk_label_new("...");
    gtk_widget_add_css_class(title_label, "title-label");
    gtk_widget_set_halign(title_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title_label), 18);

    artist_label = gtk_label_new("");
    gtk_widget_add_css_class(artist_label, "artist-label");
    gtk_widget_set_halign(artist_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(artist_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(artist_label), 22);

    gtk_box_append(GTK_BOX(center_box), title_label);
    gtk_box_append(GTK_BOX(center_box), artist_label);

    gtk_box_append(GTK_BOX(osd_box), center_box);

    gtk_window_set_child(GTK_WINDOW(osd_window), osd_box);
    gtk_widget_set_visible(osd_window, FALSE); 
}

int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.osd", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    g_application_hold(G_APPLICATION(app)); 
    return g_application_run(G_APPLICATION(app), argc, argv);
}