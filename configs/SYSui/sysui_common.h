// sysui_common.h — спільні утиліти для всіх модулів SYSui.
//
// ГОЛОВНЕ ПРО ПАМ'ЯТЬ:
//   За замовчуванням GTK4 малює через OpenGL/Vulkan, а на NVIDIA це означає, що
//   КОЖЕН процес тягне libnvidia-glcore + /dev/nvidiactl + gpucomp (~45 МБ RSS)
//   і ще ~50 МБ heap драйвера. Для маленьких панелей це безглуздо — Cairo-рендерер
//   малює їх на CPU за долі мілісекунди. Тест: GL = 193 МБ, Cairo = 62 МБ.
//
//   GTK кешує вибір рендерера у static-змінній при першому створенні, тому
//   ми ставимо GSK_RENDERER=cairo, "прогріваємо" рендерер і одразу повертаємо
//   змінну як була — програми, які запускає док/топбар, НЕ отримають cairo.
//
// Використання:
//   int main() { sysui::mem_init(); ... }          // найпершим рядком
//   static void activate(...) { sysui::mem_ready(); ... }  // першим рядком
//   sysui::trim();                                  // після приховування вікна
#pragma once

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <malloc.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace sysui {

inline void install_shared_style();  // нижче

inline std::string& saved_gsk_renderer() { static std::string v; return v; }
inline bool& had_gsk_renderer() { static bool v = false; return v; }

// Викликати НАЙПЕРШИМ у main(), до gtk_application_new().
inline void mem_init() {
    const char* v = getenv("GSK_RENDERER");
    had_gsk_renderer() = (v != nullptr);
    if (v) saved_gsk_renderer() = v;
    setenv("GSK_RENDERER", "cairo", 1);

    // Не більше 2 malloc-арен (за замовчуванням 8*ядер — кожен потік роздуває heap).
    mallopt(M_ARENA_MAX, 2);
    // Віддавати вільну пам'ять ОС раніше.
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    // Великі блоки (текстури, буфери) — через mmap, повертаються ОС одразу при free().
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
}

inline void trim() { malloc_trim(0); }

// Викликати першим рядком в activate(): фіксує Cairo-рендерер і повертає
// оточення як було, щоб дочірні процеси (запуск програм) його не успадкували.
inline void mem_ready() {
    static bool done = false;
    if (done) return;
    done = true;

    if (GdkDisplay* d = gdk_display_get_default()) {
        GdkSurface* s = gdk_surface_new_toplevel(d);
        GskRenderer* r = gsk_renderer_new_for_surface(s);
        if (r) { gsk_renderer_unrealize(r); g_object_unref(r); }
        gdk_surface_destroy(s);  // destroy звільняє і посилання
    }

    if (had_gsk_renderer()) setenv("GSK_RENDERER", saved_gsk_renderer().c_str(), 1);
    else unsetenv("GSK_RENDERER");

    install_shared_style();

    // Періодично повертаємо ОС фрагментовану пам'ять heap.
    g_timeout_add_seconds(30, [](gpointer) -> gboolean { malloc_trim(0); return G_SOURCE_CONTINUE; }, nullptr);
}

// Cairo-рендерер тримає shm-буфер розміром з вікно навіть коли воно сховане
// (на 4K@1.5 повноекранне вікно ≈ 33 МБ). Після приховування робимо unrealize —
// Wayland-surface і буфер звільняються, при наступному показі GTK створить їх заново.
inline void release_on_hide(GtkWidget* window) {
    g_signal_connect(window, "hide", G_CALLBACK(+[](GtkWidget* w, gpointer) {
        g_object_ref(w);
        g_timeout_add(400, [](gpointer d) -> gboolean {
            GtkWidget* w = GTK_WIDGET(d);
            if (!gtk_widget_get_visible(w) && gtk_widget_get_realized(w)) gtk_widget_unrealize(w);
            malloc_trim(0);
            g_object_unref(w);
            return G_SOURCE_REMOVE;
        }, w);
    }), nullptr);
}

// ---------- Hyprland IPC без fork() ----------
// Замість popen("hyprctl ...") (fork+exec bash+hyprctl на кожен виклик)
// пишемо запит напряму в сокет Hyprland. Формат той самий, що й у hyprctl:
//   "j/clients", "j/activewindow", "j/activeworkspace", "cursorpos",
//   "dispatch focuswindow address:0x..."
inline const std::string& hypr_socket_path() {
    static std::string path = [] {
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (!xdg || !sig) return std::string();
        return std::string(xdg) + "/hypr/" + sig + "/.socket.sock";
    }();
    return path;
}

inline std::string hypr_request(const std::string& req) {
    const std::string& path = hypr_socket_path();
    if (path.empty()) return "";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return "";

    struct timeval tv { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return ""; }

    size_t off = 0;
    while (off < req.size()) {
        ssize_t w = write(fd, req.data() + off, req.size() - off);
        if (w <= 0) { close(fd); return ""; }
        off += (size_t)w;
    }

    std::string out;
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(fd);
    return out;
}

// ---------- Компактне завантаження картинок ----------
// Декодує файл одразу у потрібний розмір (cover-crop), а не тримає повнорозмірну
// текстуру, яку GTK потім масштабує на кожному кадрі. 640x640 обкладинка = 1.6 МБ,
// а 44x44 = 7.7 КБ.
inline GdkTexture* load_texture_cover(const char* path, int w, int h) {
    if (!path || w <= 0 || h <= 0) return nullptr;
    int fw = 0, fh = 0;
    if (!gdk_pixbuf_get_file_info(path, &fw, &fh) || fw <= 0 || fh <= 0) return nullptr;

    // Не збільшуємо понад роздільність джерела: якщо ціль більша за файл —
    // зменшуємо ціль зі збереженням пропорцій (GtkPicture сам розтягне при малюванні).
    double up = std::max((double)w / fw, (double)h / fh);
    if (up > 1.0) { w = std::max(1, (int)(w / up)); h = std::max(1, (int)(h / up)); }

    double scale = std::max((double)w / fw, (double)h / fh);
    int sw = std::max(w, (int)(fw * scale + 0.5));
    int sh = std::max(h, (int)(fh * scale + 0.5));

    GError* err = nullptr;
    GdkPixbuf* scaled = gdk_pixbuf_new_from_file_at_scale(path, sw, sh, FALSE, &err);
    if (!scaled) { if (err) g_error_free(err); return nullptr; }

    int ox = (gdk_pixbuf_get_width(scaled)  - w) / 2;
    int oy = (gdk_pixbuf_get_height(scaled) - h) / 2;
    GdkPixbuf* sub = gdk_pixbuf_new_subpixbuf(scaled, std::max(0, ox), std::max(0, oy),
                                              std::min(w, gdk_pixbuf_get_width(scaled)),
                                              std::min(h, gdk_pixbuf_get_height(scaled)));
    GdkPixbuf* crop = gdk_pixbuf_copy(sub);  // компактна копія без батьківського буфера
    g_object_unref(sub);
    g_object_unref(scaled);
    if (!crop) return nullptr;

    bool alpha = gdk_pixbuf_get_has_alpha(crop);
    GBytes* bytes = gdk_pixbuf_read_pixel_bytes(crop);
    GdkTexture* tex = gdk_memory_texture_new(gdk_pixbuf_get_width(crop), gdk_pixbuf_get_height(crop),
                                             alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                             bytes, gdk_pixbuf_get_rowstride(crop));
    g_bytes_unref(bytes);
    g_object_unref(crop);
    return tex;
}

// ---------- Тема без fork ----------
// Замість popen("gsettings get org.gnome.desktop.interface color-scheme").
inline std::string color_scheme() {
    static GSettings* st = nullptr;
    static bool checked = false;
    if (!checked) {
        checked = true;
        GSettingsSchemaSource* src = g_settings_schema_source_get_default();
        GSettingsSchema* sch = src ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE) : nullptr;
        if (sch) {
            if (g_settings_schema_has_key(sch, "color-scheme")) st = g_settings_new("org.gnome.desktop.interface");
            g_settings_schema_unref(sch);
        }
    }
    if (!st) return "prefer-dark";
    gchar* v = g_settings_get_string(st, "color-scheme");
    std::string r = v ? v : "";
    g_free(v);
    return r;
}

// =====================================================================
//  ЄДИНІ НАЛАШТУВАННЯ ТА ТЕМА ДЛЯ ВСІХ МОДУЛІВ
// =====================================================================
// Джерело — ~/.config/megumin-os/settings.json (його редагує ignis/settings.py).
// Кожен модуль кожні ~1-2 с порівнює theme_stamp() і, якщо щось змінилось,
// перебудовує свій CSS. Тож зміни з налаштувань застосовуються наживо.

inline std::string home() { const char* h = getenv("HOME"); return h ? h : ""; }
inline std::string settings_path() { return home() + "/.config/megumin-os/settings.json"; }
inline std::string wal_css_path()  { return home() + "/.cache/wal/colors.css"; }

inline time_t file_mtime(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? st.st_mtime : 0;
}

struct Settings {
    // Загальне
    std::string accent_mode = "wal";      // "wal" — колір із шпалер, "custom" — accent_color
    std::string accent_color = "#9929CF";
    bool animations = true;               // живі анімації у всіх модулях
    // Топбар
    std::string topbar_style = "bubbles"; // "bubbles" | "dots" | "macos"
    std::string topbar_lang = "flag";     // "flag" | "text" | "icon"
    int  topbar_opacity = 80;             // 0..100
    bool topbar_border = true;
    // Док
    int  dock_opacity = 80;
    bool dock_border = true;
    std::string dock_border_style = "subtle"; // "subtle" — як у топбару/CC, "accent" — товста акцентна
    int  dock_border_opacity = 100;
    int  dock_icon_size = 38;
    bool dock_magnify = true;             // збільшення іконок як у macOS
    int  dock_magnify_pct = 160;          // максимальний масштаб, %
    bool dock_files = true;               // недавні файли (завантаження/скопійовані) у доку
    std::string dock_files_source = "both"; // "downloads" | "clipboard" | "both"
    int  dock_files_count = 4;            // скільки файлів показувати
    int  dock_files_days = 3;             // завантаження не старші за N днів
    // Control Center
    int  cc_opacity = 78;
    bool watchdog = true;                 // стежити за модулями та перезапускати
    bool sys_popups = true;               // показувати системні сповіщення спливаючими
};

inline const Settings& settings() {
    static Settings s;
    static time_t last = -1;
    time_t m = file_mtime(settings_path());
    if (m == last) return s;
    last = m;
    Settings n;
    try {
        std::ifstream f(settings_path());
        if (f.good()) {
            nlohmann::json j; f >> j;
            auto gs = [&](const char* k, std::string& v) { if (j.contains(k) && j[k].is_string()) v = j[k].get<std::string>(); };
            auto gb = [&](const char* k, bool& v) { if (j.contains(k) && j[k].is_boolean()) v = j[k].get<bool>(); };
            auto gi = [&](const char* k, int& v, int lo, int hi) {
                if (j.contains(k) && j[k].is_number()) v = std::clamp((int)j[k].get<double>(), lo, hi);
            };
            gs("accent_mode", n.accent_mode); gs("accent_color", n.accent_color);
            gb("animations", n.animations);
            gs("topbar_style", n.topbar_style); gi("topbar_opacity", n.topbar_opacity, 0, 100); gb("topbar_border", n.topbar_border);
            gs("topbar_lang", n.topbar_lang); gs("dock_border_style", n.dock_border_style);
            gi("dock_opacity", n.dock_opacity, 0, 100); gb("dock_border", n.dock_border);
            gi("dock_border_opacity", n.dock_border_opacity, 0, 100); gi("dock_icon_size", n.dock_icon_size, 24, 64);
            gb("dock_magnify", n.dock_magnify); gi("dock_magnify_pct", n.dock_magnify_pct, 110, 220);
            gi("cc_opacity", n.cc_opacity, 20, 100);
            gb("dock_files", n.dock_files); gs("dock_files_source", n.dock_files_source);
            gi("dock_files_count", n.dock_files_count, 1, 8); gi("dock_files_days", n.dock_files_days, 1, 30);
            gb("watchdog", n.watchdog); gb("sys_popups", n.sys_popups);
        }
    } catch (...) {}
    s = n;
    return s;
}

// Записати одне поле в settings.json, зберігши всі інші ключі.
inline void settings_set(const std::string& key, const nlohmann::json& value) {
    nlohmann::json j = nlohmann::json::object();
    try { std::ifstream f(settings_path()); if (f.good()) f >> j; } catch (...) {}
    if (!j.is_object()) j = nlohmann::json::object();
    j[key] = value;
    std::string tmp = settings_path() + ".tmp";
    { std::ofstream o(tmp); o << j.dump(4); }
    rename(tmp.c_str(), settings_path().c_str());
}

inline bool valid_hex(const std::string& h) {
    if (h.size() != 7 || h[0] != '#') return false;
    for (size_t i = 1; i < 7; i++) if (!isxdigit((unsigned char)h[i])) return false;
    return true;
}

// Колір pywal за ім'ям змінної ("--color5", "--background", ...).
inline std::string wal_color(const std::string& var, const std::string& fallback) {
    static time_t last = -1;
    static std::string cache;
    time_t m = file_mtime(wal_css_path());
    if (m != last) {
        last = m;
        std::ifstream f(wal_css_path());
        std::stringstream ss; ss << f.rdbuf();
        cache = ss.str();
    }
    auto pos = cache.find(var + ":");
    if (pos == std::string::npos) return fallback;
    auto start = cache.find('#', pos);
    auto eol = cache.find('\n', pos);
    if (start == std::string::npos || (eol != std::string::npos && start > eol)) return fallback;
    std::string v = cache.substr(start, 7);
    return valid_hex(v) ? v : fallback;
}

// ЄДИНИЙ акцентний колір для всіх модулів.
inline std::string accent() {
    const Settings& s = settings();
    if (s.accent_mode == "custom" && valid_hex(s.accent_color)) return s.accent_color;
    return wal_color("--color5", "#9929CF");
}

inline std::string rgba(const std::string& hex, double a) {
    int r = 153, g = 41, b = 207;
    if (valid_hex(hex)) { r = std::stoi(hex.substr(1,2), nullptr, 16); g = std::stoi(hex.substr(3,2), nullptr, 16); b = std::stoi(hex.substr(5,2), nullptr, 16); }
    // Без %f: у локалі uk_UA printf пише кому ("0,800"), і CSS ламається.
    int m = (int)(std::clamp(a, 0.0, 1.0) * 1000.0 + 0.5);
    char buf[64]; snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%d.%03d)", r, g, b, m / 1000, m % 1000);
    return buf;
}

// Змінюється, коли змінилась тема pywal або settings.json.
inline long long theme_stamp() {
    return (long long)file_mtime(wal_css_path()) * 1000003LL + (long long)file_mtime(settings_path());
}

inline bool is_light_theme() { return color_scheme() != "prefer-dark"; }

// ---------- Спільні «живі» анімації ----------
// Класи, які модулі вішають на свої віджети:
//   .live       — пружинне збільшення при наведенні, стискання при натисканні
//   .live-soft  — те саме, але делікатніше (для великих плашок)
//   .sysui-pop  — поява вікна/блоку (fade + scale + підйом)
//   .bounce     — підстрибування (запуск програми в доку)
inline const char* shared_css() {
    return R"CSS(
@keyframes sysui-pop {
    from { opacity: 0; transform: translateY(14px) scale(0.94); }
    to   { opacity: 1; transform: none; }
}
@keyframes sysui-pop-down {
    from { opacity: 0; transform: translateY(-14px) scale(0.94); }
    to   { opacity: 1; transform: none; }
}
@keyframes sysui-bounce {
    0%   { transform: none; }
    18%  { transform: translateY(-16px); }
    36%  { transform: none; }
    52%  { transform: translateY(-8px); }
    68%  { transform: none; }
    100% { transform: none; }
}
.sysui-pop      { animation: sysui-pop 320ms cubic-bezier(0.22, 1, 0.36, 1); }
.sysui-pop-down { animation: sysui-pop-down 320ms cubic-bezier(0.22, 1, 0.36, 1); }
.bounce         { animation: sysui-bounce 1100ms cubic-bezier(0.3, 0.7, 0.4, 1) 2; }

.live, .live-soft {
    transition: transform 320ms cubic-bezier(0.34, 1.56, 0.64, 1),
                background-color 200ms ease, color 200ms ease,
                border-color 200ms ease, opacity 200ms ease;
}
.live:hover      { transform: scale(1.07); }
.live-soft:hover { transform: scale(1.025); }
.live:active, .live-soft:active {
    transform: scale(0.93);
    transition: transform 90ms ease-out, background-color 120ms ease;
}
)CSS";
}

inline void apply_animation_setting() {
    if (GtkSettings* gs = gtk_settings_get_default())
        g_object_set(gs, "gtk-enable-animations", (gboolean)settings().animations, NULL);
}

inline void install_shared_style() {
    static GtkCssProvider* p = nullptr;
    if (p) return;
    GdkDisplay* d = gdk_display_get_default();
    if (!d) return;
    p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, shared_css());
    gtk_style_context_add_provider_for_display(d, GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION - 1);
    apply_animation_setting();
    // Перемикач анімацій застосовується наживо
    g_timeout_add_seconds(2, [](gpointer) -> gboolean {
        static long long last = -1;
        long long st = theme_stamp();
        if (st != last) { last = st; apply_animation_setting(); }
        return G_SOURCE_CONTINUE;
    }, nullptr);
}

// Анімація появи: при кожному показі вікна child отримує клас (.sysui-pop за замовчуванням).
inline void pop_on_show(GtkWidget* window, GtkWidget* child, const char* cls = "sysui-pop") {
    g_object_set_data(G_OBJECT(window), "sysui-pop-child", child);
    g_object_set_data(G_OBJECT(window), "sysui-pop-class", (gpointer)cls);
    g_signal_connect(window, "map", G_CALLBACK(+[](GtkWidget* w, gpointer) {
        GtkWidget* c = (GtkWidget*)g_object_get_data(G_OBJECT(w), "sysui-pop-child");
        if (!c) c = gtk_window_get_child(GTK_WINDOW(w));
        const char* k = (const char*)g_object_get_data(G_OBJECT(w), "sysui-pop-class");
        if (c) gtk_widget_add_css_class(c, k);
    }), nullptr);
    g_signal_connect(window, "unmap", G_CALLBACK(+[](GtkWidget* w, gpointer) {
        GtkWidget* c = (GtkWidget*)g_object_get_data(G_OBJECT(w), "sysui-pop-child");
        if (!c) c = gtk_window_get_child(GTK_WINDOW(w));
        const char* k = (const char*)g_object_get_data(G_OBJECT(w), "sysui-pop-class");
        if (c) gtk_widget_remove_css_class(c, k);
    }), nullptr);
}

// Оверлей CSS з акцентними плейсхолдерами (@ACCENT@, @ACCENT_SOFT@, @ACCENT_A30@, @ACCENT_A50@),
// що автоматично оновлюється при зміні теми/налаштувань. Має пріоритет над CSS модуля.
inline void live_css(const std::string& tmpl) {
    static std::string t;
    static GtkCssProvider* p = nullptr;
    t = tmpl;
    auto apply = []() {
        std::string css = t, A = accent();
        auto rep = [&](const std::string& k, const std::string& v) {
            size_t pos = 0;
            while ((pos = css.find(k, pos)) != std::string::npos) { css.replace(pos, k.size(), v); pos += v.size(); }
        };
        rep("@ACCENT_SOFT@", rgba(A, 0.22)); rep("@ACCENT_A30@", rgba(A, 0.30));
        rep("@ACCENT_A50@", rgba(A, 0.50)); rep("@ACCENT@", A);
        gtk_css_provider_load_from_string(p, css.c_str());
    };
    if (!p) {
        p = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        g_timeout_add_seconds(2, [](gpointer d) -> gboolean {
            static long long last = theme_stamp();
            long long st = theme_stamp();
            if (st != last) { last = st; (*(decltype(apply)*)d)(); }
            return G_SOURCE_CONTINUE;
        }, new decltype(apply)(apply));
    }
    apply();
}

// Системне сповіщення: пишеться в Control Center (історія, вкладка «Системні»)
// і, якщо дозволено, показується спливаючим через notify_osd.
//   level: "info" | "warn" | "error"
inline void sys_notify(const std::string& level, const std::string& title, const std::string& body) {
    std::string path = home() + "/.config/SYSui/cc.sock";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        std::string msg = "sysnotify|" + level + "|" + title + "|" + body;
        ssize_t w = write(fd, msg.data(), msg.size()); (void)w;
    }
    close(fd);
}

}  // namespace sysui
