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
#include <cstring>

GtkWidget* osd_window = nullptr;
GtkWidget* app_icon_box = nullptr;
GtkWidget* sys_icon_image = nullptr;
GtkWidget* file_icon_pic = nullptr;
GtkWidget* sender_label = nullptr;
GtkWidget* body_label = nullptr;

guint hide_timer_id = 0;
std::string current_icon_url = "";

// ================= УТИЛІТИ =================
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

struct NotifyData {
    std::string sender;
    std::string icon;
    std::string body;
};

static NotifyData read_notify_data() {
    NotifyData d;
    std::ifstream f("/tmp/notify_osd_data");
    if (!f.is_open()) return d;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("SENDER=", 0) == 0) d.sender = line.substr(7);
        else if (line.rfind("ICON=", 0) == 0)   d.icon   = line.substr(5);
        else if (line.rfind("BODY=", 0) == 0)    d.body   = line.substr(5);
    }
    return d;
}

// ================= UI =================
void update_notify_ui() {
    if (!sender_label || !body_label || !osd_window || !sys_icon_image || !file_icon_pic) {
        return;
    }

    NotifyData d = read_notify_data();

    gtk_label_set_text(GTK_LABEL(sender_label), d.sender.empty() ? "Хтось пише..." : d.sender.c_str());
    gtk_label_set_text(GTK_LABEL(body_label),   d.body.empty()   ? ""              : d.body.c_str());

    std::string icon = d.icon;
    if (icon != current_icon_url) {
        current_icon_url = icon;

        bool is_file = false;
        std::string path = "";

        if (icon.find("http://") == 0 || icon.find("https://") == 0) {
            is_file = true;
            std::string hash_str = std::to_string(std::hash<std::string>{}(icon));
            path = "/tmp/notify_icon_" + hash_str + ".png";
            std::ifstream chk(path);
            if (!chk.good()) exec("curl -sL '" + icon + "' -o " + path);
        } else if (icon.find("file://") == 0) {
            is_file = true;
            path = icon.substr(7);
        } else if (!icon.empty() && icon[0] == '/') {
            is_file = true;
            path = icon;
        }

        if (is_file) {
            gtk_widget_set_visible(sys_icon_image, FALSE);
            gtk_widget_set_visible(file_icon_pic, TRUE);
            gtk_picture_set_filename(GTK_PICTURE(file_icon_pic), path.c_str());
        } else if (!icon.empty()) {
            gtk_widget_set_visible(file_icon_pic, FALSE);
            gtk_widget_set_visible(sys_icon_image, TRUE);
            gtk_image_set_from_icon_name(GTK_IMAGE(sys_icon_image), icon.c_str());
            gtk_image_set_pixel_size(GTK_IMAGE(sys_icon_image), 50); 
        } else {
            gtk_widget_set_visible(file_icon_pic, FALSE);
            gtk_widget_set_visible(sys_icon_image, FALSE);
        }
    }
}

// ================= CSS =================
void apply_css() {
    GtkCssProvider* provider = gtk_css_provider_new();
    std::string css = R"(
        window { background: transparent; }
        * { font-family: "JetBrainsMono Nerd Font", "SF Pro Display", sans-serif; outline: none; border: none; }
        .osd-box { background-color: rgba(20, 20, 20, 0.55); border: 1px solid rgba(255, 255, 255, 0.15); border-radius: 17px; padding: 5px 17px 5px 7px; }
        
        .sys-icon { margin-right: 10px; }
        .notify-file-pic { border-radius: 6px; margin-right: 17px; background-color: rgba(0,0,0,0.3); overflow: hidden; }
        
        .sender-label { color: #ffffff; font-size: 11px; font-weight: 900; }
        .body-label { color: rgba(255, 255, 255, 0.72); font-size: 9px; font-weight: 500; }
    )";
    
    gtk_css_provider_load_from_string(provider, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

// ================= ОБРОБКА СОКЕТА =================
gboolean hide_osd(gpointer) {
    if (osd_window) gtk_widget_set_visible(osd_window, FALSE);
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
            
            // Якщо прийшла команда "show" — показуємо сповіщення
            if (command.find("show") != std::string::npos) {
                if (!osd_window || !sender_label) { close(client_fd); return G_SOURCE_CONTINUE; }
                
                update_notify_ui();
                gtk_widget_set_visible(osd_window, TRUE);
                gtk_window_present(GTK_WINDOW(osd_window));

                if (hide_timer_id > 0) g_source_remove(hide_timer_id);
                hide_timer_id = g_timeout_add(3500, hide_osd, nullptr);
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

// ================= ACTIVATE =================
static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    // Вмикаємо сокет
    setup_ipc_socket((sysui::home() + "/.config/SYSui/notify.sock").c_str());
    
    apply_css();

    osd_window = gtk_application_window_new(app);
    sysui::release_on_hide(osd_window);
    sysui::pop_on_show(osd_window, nullptr);
    sysui::live_css(".osd-box { border-color: @ACCENT_A30@; }");
    gtk_layer_init_for_window(GTK_WINDOW(osd_window));
    gtk_layer_set_namespace(GTK_WINDOW(osd_window), "notify_osd");
    gtk_layer_set_layer(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_BOTTOM, 120);

    GtkWidget* osd_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(osd_box, "osd-box");
    gtk_widget_set_size_request(osd_box, 260, 70);
    gtk_widget_set_halign(osd_box, GTK_ALIGN_CENTER);

    app_icon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    
    sys_icon_image = gtk_image_new();
    gtk_widget_add_css_class(sys_icon_image, "sys-icon");
    gtk_widget_set_valign(sys_icon_image, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(sys_icon_image, 50, 50);
    
    file_icon_pic = gtk_picture_new();
    gtk_widget_add_css_class(file_icon_pic, "notify-file-pic");
    gtk_widget_set_valign(file_icon_pic, GTK_ALIGN_CENTER);
    gtk_picture_set_can_shrink(GTK_PICTURE(file_icon_pic), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(file_icon_pic), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(file_icon_pic, 50, 50);

    gtk_box_append(GTK_BOX(app_icon_box), sys_icon_image);
    gtk_box_append(GTK_BOX(app_icon_box), file_icon_pic);
    
    gtk_box_append(GTK_BOX(osd_box), app_icon_box);

    GtkWidget* text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(text_box, TRUE);

    sender_label = gtk_label_new("Хтось пише...");
    gtk_widget_add_css_class(sender_label, "sender-label");
    gtk_widget_set_halign(sender_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(sender_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(sender_label), 22);

    body_label = gtk_label_new("");
    gtk_widget_add_css_class(body_label, "body-label");
    gtk_widget_set_halign(body_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(body_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(body_label), 28);
    gtk_label_set_lines(GTK_LABEL(body_label), 2);
    gtk_label_set_wrap(GTK_LABEL(body_label), TRUE);

    gtk_box_append(GTK_BOX(text_box), sender_label);
    gtk_box_append(GTK_BOX(text_box), body_label);
    gtk_box_append(GTK_BOX(osd_box), text_box);

    gtk_window_set_child(GTK_WINDOW(osd_window), osd_box);
    gtk_widget_set_visible(osd_window, FALSE);
}

// ================= MAIN =================
int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.notify", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    g_application_hold(G_APPLICATION(app));
    return g_application_run(G_APPLICATION(app), argc, argv);
}