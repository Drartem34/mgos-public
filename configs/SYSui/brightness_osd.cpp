// brightness_osd.cpp — плашка яскравості екрана та підсвітки клавіатури (стиль volume_osd).
//
//   Екран:     echo up|down|show | socat - UNIX-CLIENT:~/.config/SYSui/brightness.sock
//              (бінди Fn+F2 / Fn+F3 = XF86MonBrightnessDown/Up у hyprland.conf).
//              Яскравість пишеться прямо в /sys/class/backlight/*/brightness (група video),
//              з плавним переходом ~120 мс; якщо запис заборонено — через brightnessctl.
//   Клавіатура: Fn+F9 обробляє BIOS — плашка лише показує новий рівень. Без полінгу:
//              сигнал UPower KbdBacklight.BrightnessChangedWithSource + POLLPRI на
//              brightness_hw_changed (sysfs_notify) як запасний шлях.
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include "sysui_common.h"
#include <dirent.h>
#include <fcntl.h>
#include <cmath>
#include <string>
#include <vector>

static GtkWidget *g_win = nullptr, *g_icon = nullptr, *g_bar = nullptr, *g_segs = nullptr, *g_pct = nullptr;
static std::vector<GtkWidget*> g_seg_items;
static GtkCssProvider* g_css = nullptr;
static guint g_hide = 0;

// ================= SYSFS =================
static std::string read_line(const std::string& p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}
static std::string find_dir(const char* base, const char* needle) {
    std::string found;
    if (DIR* d = opendir(base)) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            if (!needle || strstr(e->d_name, needle)) { found = std::string(base) + "/" + e->d_name; break; }
        }
        closedir(d);
    }
    return found;
}
static const std::string& bl_dir() { static std::string d = find_dir("/sys/class/backlight", nullptr); return d; }
static const std::string& kbd_dir() { static std::string d = find_dir("/sys/class/leds", "kbd_backlight"); return d; }
static int read_int(const std::string& p, int def = 0) { std::string s = read_line(p); return s.empty() ? def : atoi(s.c_str()); }

// ================= CSS =================
static void apply_css() {
    std::string accent = sysui::accent();
    bool light = sysui::is_light_theme();
    std::string bg = light ? "rgba(245,245,245,0.72)" : sysui::rgba(sysui::wal_color("--background", "#141414"), 0.55);
    std::string fg = light ? "#1a1a1a" : sysui::wal_color("--foreground", "#ffffff");
    std::string trough = light ? "rgba(0,0,0,0.15)" : sysui::rgba(fg, 0.2);
    std::string css = R"(
window { background: transparent; }
* { font-family: "JetBrainsMono Nerd Font", "SF Pro Display", sans-serif; outline: none; border: none; }
.bri-box { background-color: )" + bg + R"(; border: 1px solid rgba(128,128,128,0.2); border-radius: 15px; padding: 0 15px; }
.bri-icon { color: )" + fg + R"(; font-size: 20px; margin-right: 12px; }
.pct { color: )" + fg + R"(; font-size: 12px; font-weight: 800; margin-left: 12px; min-width: 58px; opacity: 0.75; }
.bri-box progressbar trough { background-color: )" + trough + R"(; min-height: 4px; min-width: 4px; border-radius: 2px; box-shadow: none; }
.bri-box progressbar progress { background-color: )" + accent + R"(; min-height: 4px; min-width: 4px; border-radius: 2px; box-shadow: none;
                       transition: all 0.3s cubic-bezier(0.25, 0.1, 0.25, 1); }
.seg { min-height: 6px; min-width: 40px; border-radius: 3px; background-color: )" + trough + R"(; transition: background-color 200ms ease; }
.seg.on { background-color: )" + accent + R"(; box-shadow: 0 0 6px )" + sysui::rgba(accent, 0.55) + R"(; }
)";
    gtk_css_provider_load_from_string(g_css, css.c_str());
}

// ================= ПОКАЗ =================
static void show_osd() {
    apply_css();
    gtk_widget_set_visible(g_win, TRUE);
    if (g_hide) g_source_remove(g_hide);
    g_hide = g_timeout_add(1800, [](gpointer) -> gboolean { gtk_widget_set_visible(g_win, FALSE); g_hide = 0; return G_SOURCE_REMOVE; }, nullptr);
}

static void show_screen(double f) {
    f = std::clamp(f, 0.0, 1.0);
    const char* ic = f < 0.2 ? "\U000F00DE" : f < 0.5 ? "\U000F00DF" : f < 0.8 ? "\U000F00E0" : "\U000F00E0";
    gtk_label_set_text(GTK_LABEL(g_icon), f < 0.2 ? "\U000F00DE" : f < 0.6 ? "\U000F00DF" : ic);
    gtk_widget_set_visible(g_bar, TRUE);
    gtk_widget_set_visible(g_segs, FALSE);
    gtk_widget_set_visible(g_pct, FALSE);
    gtk_widget_set_opacity(g_icon, 1.0);
    sysui::animate_fraction(g_bar, f);
    show_osd();
}

static void show_kbd(int level) {
    int mx = std::max(1, read_int(kbd_dir() + "/max_brightness", 3));
    level = std::clamp(level, 0, mx);
    // сегменти: по одному на рівень (у Galaxy Book 0..3)
    if ((int)g_seg_items.size() != mx) {
        for (GtkWidget* s : g_seg_items) gtk_box_remove(GTK_BOX(g_segs), s);
        g_seg_items.clear();
        for (int i = 0; i < mx; i++) {
            GtkWidget* s = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(s, "seg");
            gtk_widget_set_hexpand(s, TRUE);
            gtk_widget_set_valign(s, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(g_segs), s);
            g_seg_items.push_back(s);
        }
    }
    for (int i = 0; i < mx; i++) {
        if (i < level) gtk_widget_add_css_class(g_seg_items[i], "on");
        else gtk_widget_remove_css_class(g_seg_items[i], "on");
    }
    gtk_label_set_text(GTK_LABEL(g_icon), level ? "\U000F030C" : "\U000F030E");   // keyboard / keyboard-off
    // бар поділений на секції — по одній на апаратний рівень (у Galaxy Book: 3 + вимкнено)
    gtk_widget_set_visible(g_bar, FALSE);
    gtk_widget_set_visible(g_segs, TRUE);
    gtk_widget_set_visible(g_pct, FALSE);
    gtk_widget_set_opacity(g_icon, 0.45 + 0.55 * level / mx);   // іконка «світиться» сильніше з рівнем
    show_osd();
}

// ================= ЯСКРАВІСТЬ ЕКРАНА (плавно) =================
static int g_target = -1, g_cur = -1;
static guint g_fade = 0;
static bool write_bl(int v) {
    int fd = open((bl_dir() + "/brightness").c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::string s = std::to_string(v);
    bool ok = write(fd, s.c_str(), s.size()) == (ssize_t)s.size();
    close(fd);
    return ok;
}
static void step_screen(int dir) {
    if (bl_dir().empty()) return;
    int mx = std::max(1, read_int(bl_dir() + "/max_brightness", 100));
    int now = g_target >= 0 && g_fade ? g_target : read_int(bl_dir() + "/brightness", mx / 2);
    // крок 5%, а внизу — дрібніший (1%), щоб можна було зробити зовсім тьмяно, але не 0
    double pct = now * 100.0 / mx;
    double step = (pct <= 10 && dir < 0) || (pct < 10 && dir > 0) ? 1 : 5;
    double np = std::clamp(std::round((pct + dir * step) / step) * step, 1.0, 100.0);
    int target = std::max(1, (int)std::lround(np * mx / 100.0));
    show_screen(target / (double)mx);
    if (g_cur < 0 || !g_fade) g_cur = read_int(bl_dir() + "/brightness", target);
    g_target = target;
    if (!write_bl(g_cur)) {   // немає прав на sysfs — запасний шлях
        std::string arg = std::to_string((int)np) + "%";
        GSubprocess* sp = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE, nullptr, "brightnessctl", "-q", "set", arg.c_str(), nullptr);
        if (sp) g_object_unref(sp);
        return;
    }
    if (!g_fade) g_fade = g_timeout_add(15, [](gpointer) -> gboolean {
        int d = g_target - g_cur;
        if (d == 0) { g_fade = 0; return G_SOURCE_REMOVE; }
        int st = (int)std::ceil(std::abs(d) / 3.0);
        g_cur += d > 0 ? st : -st;
        write_bl(g_cur);
        return G_SOURCE_CONTINUE;
    }, nullptr);
}

// ================= ПІДСВІТКА КЛАВІАТУРИ =================
static int g_kbd_last = -1;
static void kbd_changed(int v) {
    if (v == g_kbd_last) return;
    g_kbd_last = v;
    show_kbd(v);
}
static void kbd_watch() {
    if (kbd_dir().empty()) return;
    g_kbd_last = read_int(kbd_dir() + "/brightness", 0);
    // 1) UPower: сигнал із джерелом ("external" — гаряча клавіша BIOS)
    if (GDBusConnection* sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr)) {
        g_dbus_connection_signal_subscribe(sys, "org.freedesktop.UPower", "org.freedesktop.UPower.KbdBacklight", nullptr,
            "/org/freedesktop/UPower/KbdBacklight", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* sig, GVariant* p, gpointer) {
                int v = 0;
                if (!strcmp(sig, "BrightnessChanged") && g_variant_is_of_type(p, G_VARIANT_TYPE("(i)"))) g_variant_get(p, "(i)", &v);
                else if (!strcmp(sig, "BrightnessChangedWithSource") && g_variant_is_of_type(p, G_VARIANT_TYPE("(is)"))) g_variant_get(p, "(i&s)", &v, nullptr);
                else return;
                kbd_changed(v);
            }, nullptr, nullptr);
    }
    // 2) sysfs_notify на brightness_hw_changed (драйвер samsung-galaxybook)
    std::string hw = kbd_dir() + "/brightness_hw_changed";
    int fd = open(hw.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char b[16];
        ssize_t r = read(fd, b, sizeof b); (void)r;   // перше читання «озброює» POLLPRI
        GIOChannel* ch = g_io_channel_unix_new(fd);
        g_io_add_watch(ch, (GIOCondition)(G_IO_PRI | G_IO_ERR), [](GIOChannel* c, GIOCondition, gpointer) -> gboolean {
            int fd = g_io_channel_unix_get_fd(c);
            char b[16] = {0};
            lseek(fd, 0, SEEK_SET);
            ssize_t r = read(fd, b, sizeof b - 1); (void)r;
            kbd_changed(read_int(kbd_dir() + "/brightness", atoi(b)));
            return G_SOURCE_CONTINUE;
        }, nullptr);
    }
}

// ================= СОКЕТ =================
static gboolean on_sock(GIOChannel* src, GIOCondition, gpointer) {
    int cfd = accept(g_io_channel_unix_get_fd(src), nullptr, nullptr);
    if (cfd < 0) return G_SOURCE_CONTINUE;
    char buf[64] = {0};
    ssize_t n = read(cfd, buf, sizeof buf - 1);
    close(cfd);
    if (n <= 0) return G_SOURCE_CONTINUE;
    std::string cmd(buf);
    if (cmd.rfind("up", 0) == 0) step_screen(+1);
    else if (cmd.rfind("down", 0) == 0) step_screen(-1);
    else if (cmd.rfind("kbd", 0) == 0) show_kbd(read_int(kbd_dir() + "/brightness", 0));
    else if (cmd.rfind("show", 0) == 0 && !bl_dir().empty())
        show_screen(read_int(bl_dir() + "/brightness") / (double)std::max(1, read_int(bl_dir() + "/max_brightness", 1)));
    return G_SOURCE_CONTINUE;
}
static void setup_socket() {
    std::string path = sysui::home() + "/.config/SYSui/brightness.sock";
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    if (bind(fd, (sockaddr*)&a, sizeof a) < 0) { close(fd); return; }
    listen(fd, 8);
    g_io_add_watch(g_io_channel_unix_new(fd), G_IO_IN, on_sock, nullptr);
}

// ================= ACTIVATE =================
static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    g_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    apply_css();

    g_win = gtk_application_window_new(app);
    sysui::release_on_hide(g_win);
    sysui::pop_on_show(g_win, nullptr);
    gtk_layer_init_for_window(GTK_WINDOW(g_win));
    gtk_layer_set_namespace(GTK_WINDOW(g_win), "brightness_osd");
    gtk_layer_set_layer(GTK_WINDOW(g_win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_BOTTOM, 120);
    sysui::follow_osd_position(g_win, false);   // «Налаштування → OSD»

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "bri-box");
    gtk_widget_set_size_request(box, 200, 60);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    g_icon = gtk_label_new("\U000F00E0");
    gtk_widget_add_css_class(g_icon, "bri-icon");
    g_bar = gtk_progress_bar_new();
    gtk_widget_set_valign(g_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(g_bar, TRUE);
    g_segs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand(g_segs, TRUE);
    gtk_widget_set_valign(g_segs, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(g_segs, FALSE);
    g_pct = gtk_label_new("");
    gtk_widget_add_css_class(g_pct, "pct");
    gtk_label_set_xalign(GTK_LABEL(g_pct), 1);
    gtk_box_append(GTK_BOX(box), g_icon);
    gtk_box_append(GTK_BOX(box), g_bar);
    gtk_box_append(GTK_BOX(box), g_segs);
    gtk_box_append(GTK_BOX(box), g_pct);
    gtk_widget_set_visible(g_pct, FALSE);
    gtk_window_set_child(GTK_WINDOW(g_win), sysui::bb::underlay(g_win, box, "brightness_osd", 15, nullptr, true, true));   // запечений блюр
    gtk_widget_set_visible(g_win, FALSE);

    setup_socket();
    kbd_watch();
}

int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.brightness", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    g_application_hold(G_APPLICATION(app));
    return g_application_run(G_APPLICATION(app), argc, argv);
}
