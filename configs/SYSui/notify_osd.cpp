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
GtkWidget* badge_image = nullptr;
GtkWidget* ntf_box = nullptr;      // плашка: класи lvl-error / lvl-warn для системних помилок і попереджень
static int  ntf_timeout_ms = 3500;   // маленька іконка програми в куті (як у macOS)

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
    std::string icon;       // аватарка/картинка (шлях, file://, http) або назва іконки
    std::string body;
    std::string app, desktop, site, site_icon;
    std::string level;      // error | warn | "" — звичайне сповіщення
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
        else if (line.rfind("APP=", 0) == 0)     d.app    = line.substr(4);
        else if (line.rfind("DESKTOP=", 0) == 0) d.desktop = line.substr(8);
        else if (line.rfind("SITE=", 0) == 0)    d.site   = line.substr(5);
        else if (line.rfind("SITEICON=", 0) == 0) d.site_icon = line.substr(9);
        else if (line.rfind("LEVEL=", 0) == 0)   d.level  = line.substr(6);
    }
    return d;
}

// ================= ІКОНКИ =================
// Іконка програми: .desktop із підказки desktop-entry → пошук за назвою програми → назва як іконка теми.
static GIcon* app_gicon(const NotifyData& d) {
    auto from_info = [](GDesktopAppInfo* info) -> GIcon* {
        if (!info) return nullptr;
        GIcon* ic = g_app_info_get_icon(G_APP_INFO(info));
        if (ic) g_object_ref(ic);
        g_object_unref(info);
        return ic;
    };
    if (!d.desktop.empty()) {
        std::string id = d.desktop;
        if (id.size() < 8 || id.compare(id.size() - 8, 8, ".desktop") != 0) id += ".desktop";
        if (GIcon* ic = from_info(g_desktop_app_info_new(id.c_str()))) return ic;
        std::string low = id; std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (GIcon* ic = from_info(g_desktop_app_info_new(low.c_str()))) return ic;
    }
    if (!d.app.empty() && d.app != "notify-send") {
        gchar*** res = g_desktop_app_info_search(d.app.c_str());
        GIcon* found = nullptr;
        if (res) {
            if (res[0] && res[0][0]) found = from_info(g_desktop_app_info_new(res[0][0]));
            for (gchar*** g = res; *g; ++g) g_strfreev(*g);
            g_free(res);
        }
        if (found) return found;
        std::string low = d.app; std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        GtkIconTheme* th = gtk_icon_theme_get_for_display(gdk_display_get_default());
        if (gtk_icon_theme_has_icon(th, low.c_str())) return g_themed_icon_new(low.c_str());
    }
    return nullptr;
}

// Картинка з ICON: локальний файл, file:// або http(s) (качаємо без shell — адреса приходить ззовні)
static std::string icon_file(const std::string& icon) {
    if (icon.rfind("http://", 0) == 0 || icon.rfind("https://", 0) == 0) {
        std::string path = "/tmp/notify_icon_" + std::to_string(std::hash<std::string>{}(icon)) + ".png";
        if (access(path.c_str(), F_OK) != 0) {
            const char* argv[] = {"curl", "-fsSL", "--max-time", "2", "--max-filesize", "2000000", "-o", path.c_str(), "--", icon.c_str(), nullptr};
            g_spawn_sync(nullptr, (gchar**)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        return access(path.c_str(), F_OK) == 0 ? path : "";
    }
    if (icon.rfind("file://", 0) == 0) return icon.substr(7);
    if (!icon.empty() && icon[0] == '/') return access(icon.c_str(), F_OK) == 0 ? icon : "";
    return "";
}

// ================= UI =================
void update_notify_ui() {
    if (!sender_label || !body_label || !osd_window || !sys_icon_image || !file_icon_pic || !badge_image) return;

    NotifyData d = read_notify_data();
    // Помилка / попередження: червона чи бурштинова плашка зі значком рівня в куті, висить довше
    if (ntf_box) {
        gtk_widget_remove_css_class(ntf_box, "lvl-error");
        gtk_widget_remove_css_class(ntf_box, "lvl-warn");
        if (d.level == "error" || d.level == "warn") gtk_widget_add_css_class(ntf_box, ("lvl-" + d.level).c_str());
    }
    ntf_timeout_ms = d.level == "error" ? 7000 : d.level == "warn" ? 5500 : 3500;
    gtk_label_set_text(GTK_LABEL(sender_label), d.sender.empty() ? (d.site.empty() ? "Сповіщення" : d.site.c_str()) : d.sender.c_str());
    gtk_label_set_text(GTK_LABEL(body_label), d.body.c_str());

    GIcon* app_ic = app_gicon(d);
    std::string pic = !d.site_icon.empty() && access(d.site_icon.c_str(), F_OK) == 0 ? d.site_icon : icon_file(d.icon);

    if (!pic.empty()) {
        // головна — іконка сайту або аватарка, програма — бейджем у куті
        gtk_widget_set_visible(sys_icon_image, FALSE);
        gtk_widget_set_visible(file_icon_pic, TRUE);
        // через gdk-pixbuf: він читає й .ico (favicon), чого не вміє завантажувач текстур GTK
        const bool site = !d.site_icon.empty();
        const int px = site ? 36 : 50;   // GtkPicture бере розмір текстури — вантажимо одразу в розмірі плашки
        GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(pic.c_str(), px, px, TRUE, nullptr);
        if (pb) {
            GdkTexture* tx = gdk_texture_new_for_pixbuf(pb);
            gtk_picture_set_paintable(GTK_PICTURE(file_icon_pic), GDK_PAINTABLE(tx));
            g_object_unref(tx); g_object_unref(pb);
        } else gtk_picture_set_filename(GTK_PICTURE(file_icon_pic), pic.c_str());
        if (!d.site_icon.empty()) gtk_widget_add_css_class(file_icon_pic, "site");
        else gtk_widget_remove_css_class(file_icon_pic, "site");
        if (app_ic) gtk_image_set_from_gicon(GTK_IMAGE(badge_image), app_ic);
        gtk_widget_set_visible(badge_image, app_ic != nullptr);
    } else {
        gtk_widget_set_visible(file_icon_pic, FALSE);
        gtk_widget_set_visible(badge_image, FALSE);
        gtk_widget_set_visible(sys_icon_image, TRUE);
        if (app_ic) gtk_image_set_from_gicon(GTK_IMAGE(sys_icon_image), app_ic);
        else gtk_image_set_from_icon_name(GTK_IMAGE(sys_icon_image), d.icon.empty() ? "dialog-information" : d.icon.c_str());
        gtk_image_set_pixel_size(GTK_IMAGE(sys_icon_image), 46);
    }
    if (!d.level.empty() && pic.empty() && app_ic) {
        // іконка програми велика, а в куті — знак помилки/попередження
        gtk_image_set_from_icon_name(GTK_IMAGE(badge_image), d.level == "error" ? "dialog-error" : "dialog-warning");
        gtk_widget_set_visible(badge_image, TRUE);
    }
    if (app_ic) g_object_unref(app_ic);
    current_icon_url = pic;
}

// ================= CSS =================
void apply_css() {
    GtkCssProvider* provider = gtk_css_provider_new();
    std::string css = R"(
        window { background: transparent; }
        * { font-family: "JetBrainsMono Nerd Font", "SF Pro Display", sans-serif; outline: none; border: none; }
        .ntf-box { background-color: rgba(20, 20, 20, 0.55); border: 1px solid rgba(255, 255, 255, 0.15); border-radius: 17px; padding: 5px 17px 5px 7px; }
        
        .sys-icon { }
        .notify-file-pic { border-radius: 12px; background-color: rgba(0,0,0,0.3); }
        .notify-file-pic.site { background-color: rgba(255,255,255,0.94); padding: 7px; }
        .ntf-icon-wrap { margin-right: 12px; }
        .ntf-badge { margin: 0 -4px -4px 0; }
        
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
                hide_timer_id = g_timeout_add(ntf_timeout_ms, hide_osd, nullptr);
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
    sysui::live_css(".ntf-box { border-color: @ACCENT_A30@; }");
    gtk_layer_init_for_window(GTK_WINDOW(osd_window));
    gtk_layer_set_namespace(GTK_WINDOW(osd_window), "notify_osd");
    gtk_layer_set_layer(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(osd_window), GTK_LAYER_SHELL_EDGE_BOTTOM, 120);
    sysui::follow_osd_position(osd_window, true);   // «Налаштування → OSD → Сповіщення»

    GtkWidget* osd_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(osd_box, "ntf-box");
    ntf_box = osd_box;
    {   // вище за live_css (акцентна рамка), щоб колір рівня перемагав
        GtkCssProvider* lp = gtk_css_provider_new();
        gtk_css_provider_load_from_string(lp,
            ".ntf-box.lvl-error { background-color: rgba(58, 14, 18, 0.78); border: 1px solid rgba(255, 92, 98, 0.55);"
            "  background-clip: padding-box; box-shadow: none; }"
            ".ntf-box.lvl-error .sender-label { color: #ffb3b6; }"
            ".ntf-box.lvl-warn { background-color: rgba(54, 38, 8, 0.78); border: 1px solid rgba(255, 184, 64, 0.55);"
            "  background-clip: padding-box; box-shadow: none; }"
            ".ntf-box.lvl-warn .sender-label { color: #ffd98f; }"
            ".ntf-box.lvl-error .body-label, .ntf-box.lvl-warn .body-label { color: rgba(255,255,255,0.86); }");
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(lp), GTK_STYLE_PROVIDER_PRIORITY_USER + 10);
    }
    gtk_widget_set_size_request(osd_box, 260, 70);
    gtk_widget_set_halign(osd_box, GTK_ALIGN_CENTER);

    app_icon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    
    sys_icon_image = gtk_image_new();
    gtk_widget_add_css_class(sys_icon_image, "sys-icon");
    gtk_image_set_pixel_size(GTK_IMAGE(sys_icon_image), 46);
    gtk_widget_set_valign(sys_icon_image, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(sys_icon_image, 50, 50);
    
    file_icon_pic = gtk_picture_new();
    gtk_widget_add_css_class(file_icon_pic, "notify-file-pic");
    gtk_widget_set_valign(file_icon_pic, GTK_ALIGN_CENTER);
    gtk_picture_set_can_shrink(GTK_PICTURE(file_icon_pic), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(file_icon_pic), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(file_icon_pic, 50, 50);

    GtkWidget* pics = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(pics), sys_icon_image);
    gtk_box_append(GTK_BOX(pics), file_icon_pic);
    GtkWidget* ov = gtk_overlay_new();
    gtk_widget_add_css_class(ov, "ntf-icon-wrap");
    gtk_widget_set_valign(ov, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(ov), pics);
    badge_image = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(badge_image), 22);
    gtk_widget_add_css_class(badge_image, "ntf-badge");
    gtk_widget_set_halign(badge_image, GTK_ALIGN_END);
    gtk_widget_set_valign(badge_image, GTK_ALIGN_END);
    gtk_widget_set_visible(badge_image, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), badge_image);
    gtk_box_append(GTK_BOX(app_icon_box), ov);

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

    gtk_window_set_child(GTK_WINDOW(osd_window), sysui::bb::underlay(osd_window, osd_box, "notify_osd", 17, nullptr, true, true));   // запечений блюр
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