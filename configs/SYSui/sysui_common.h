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

#include <cmath>
#include <algorithm>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <malloc.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <functional>
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
// mtime з наносекундами: два записи settings.json за одну секунду (accent_mode + accent_color)
// інакше давали однаковий штамп — і новий акцент не підхоплювався до наступної зміни.
inline long long file_mtime_ns(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return 0;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + (long long)st.st_size * 7919LL;
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
    bool dock_reserve = false;            // док завжди видно і він відсуває вікна (exclusive zone), а не лежить поверх
    bool dock_minimize_drag = true;       // перетягнути вікно на док → сховати застосунок (special:mgos-dock)
    std::string dock_stash_layout = "grid"; // сховище доку: "grid" — плитки прилипають до сітки, "free" — вільно
    std::string tp_swipe_down = "minimize"; // 3 пальці вниз: minimize (вікно в док, dock_genie.h) | runner | off
    bool genie_anim = true;               // джин (зжаття вікна); false — плавне зменшення в іконку
    bool genie_grow = true;              // запуск з доку: вікно «виростає» з іконки (зворотна здача, dock_genie.h)
    bool genie_squash = true;           // «здача» вікна в іконку: знімок стискається, іконка пружно вдавлюється (dock_genie.h)
    int  genie_speed = 100;               // швидкість анімації вікно→іконка, % (50…200)
    int tp_min_dist = 300;                // довжина жесту згортання, px тачпада
    std::string tp_swipe_up = "fullscreen"; // 3 пальці вгору: fullscreen | maximize (вікно на весь екран, dock_genie.h) | runner | off
    bool dock_stash_cards = true;         // простір: сховані вікна рядом карток (як «Недавні» в Android) + кнопка «На робочий стіл»
    bool dock_stash_zram = true;          // сховані в сховище програми стискаються в zram (mgos-pageout)
    bool dock_recent = true;              // показувати недавно закриті програми (секція після роздільника)
    int  dock_recent_count = 15;          // скільки закритих програм показувати
    // Control Center
    int  cc_opacity = 78;
    bool watchdog = true;                 // стежити за модулями та перезапускати
    bool sys_popups = true;               // показувати системні сповіщення спливаючими
    std::string osd_position = "bottom";    // плашки OSD (гучність, яскравість, музика): bottom|top|center|top-right|bottom-right
    std::string notify_position = "bottom"; // плашка сповіщень — окремо
    bool topbar_power = true;             // кнопка ⏻ у топбарі
    // Шрифт усіх панелей. Значки (Nerd glyphs) беруться з легкого «Symbols Nerd Font».
    std::string panel_font = "SF Pro Display";
    // Піксельна анімація шапок (about, блок годинника): waves|virus|eq|rain|life
    std::string pixel_style = "waves";
    // Економія: «запечений» блюр на весь екран — блюр лишається, відео-шпалера на паузі (кеш блюру не скидається)
    bool eco_fs_blur = true;
    bool hyprland_blur = true;            // загальний перемикач розмиття з «Налаштувань»
    bool video_optimization = false;      // «Пауза відео-шпалер», поки вікно на весь екран
    bool wp_pause_covered = true;         // «Пауза під вікнами»: шпалери (відео й піксельні) стоять, поки вікна закривають ≥85% екрана
    bool baked_blur = true;               // «запечений» блюр: віджети самі малюють заздалегідь розмиті шпалери (замість layerrule blur)
    bool eco_effects = false;             // «Економ ефекти» (CC / Super+Shift+F1): без блюру, тіней і заокруглень
};

inline const Settings& settings() {
    static Settings s;
    static long long last = -1;
    long long m = file_mtime_ns(settings_path());
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
            gi("dock_files_count", n.dock_files_count, 1, 8); gi("dock_files_days", n.dock_files_days, 1, 30); gb("dock_reserve", n.dock_reserve); gb("dock_minimize_drag", n.dock_minimize_drag); gs("dock_stash_layout", n.dock_stash_layout); gb("dock_stash_zram", n.dock_stash_zram); gs("tp_swipe_down", n.tp_swipe_down); gi("tp_min_dist", n.tp_min_dist, 120, 800); gs("tp_swipe_up", n.tp_swipe_up); gb("dock_stash_cards", n.dock_stash_cards); gb("genie_squash", n.genie_squash); gb("genie_grow", n.genie_grow); gb("genie_anim", n.genie_anim); gi("genie_speed", n.genie_speed, 50, 200);
            gb("dock_recent", n.dock_recent); gi("dock_recent_count", n.dock_recent_count, 1, 15);
            gb("watchdog", n.watchdog); gb("sys_popups", n.sys_popups); gs("osd_position", n.osd_position); gs("notify_position", n.notify_position);
            gb("topbar_power", n.topbar_power); gs("panel_font", n.panel_font); gs("pixel_style", n.pixel_style);
            gb("eco_fs_blur", n.eco_fs_blur); gb("hyprland_blur", n.hyprland_blur); gb("eco_effects", n.eco_effects); gb("video_optimization", n.video_optimization);
            gb("wp_pause_covered", n.wp_pause_covered); gb("baked_blur", n.baked_blur);
        }
    } catch (...) {}
    // «Економ ефекти»: блюру немає — напівпрозорі панелі без нього виглядають брудно, тож тимчасово
    // робимо їх суцільними (модулі перебудовують CSS за theme_stamp; збережені значення не чіпаємо).
    if (n.eco_effects) { n.topbar_opacity = 100; n.dock_opacity = 100; n.cc_opacity = 100; }
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
// Акцент зі шпалер. Раніше брали pywal --color5, але pywal кладе туди будь-який колір —
// напр. на блакитних шпалерах (color1..6 сині/бірюзові) color5 = сіро-лавандовий #A38FB8,
// і CC/хвилі/about були «чужого» кольору. Тепер: колір із ДОМІНАНТНОЇ групи відтінків палітри,
// найнасиченіший і середньої яскравості; яскравість трохи затискаємо для читабельності.
// Той самий алгоритм — у ~/.local/bin/mgos-accent (Налаштування, SDDM).
namespace detail {
inline void hex_hsl(const std::string& hx, double& h, double& s, double& l) {
    unsigned v = 0; sscanf(hx.c_str() + 1, "%06x", &v);
    double r = ((v >> 16) & 255) / 255.0, g = ((v >> 8) & 255) / 255.0, b = (v & 255) / 255.0;
    double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2; h = s = 0;
    if (mx == mn) return;
    double d = mx - mn;
    s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0); else if (mx == g) h = (b - r) / d + 2; else h = (r - g) / d + 4;
    h *= 60;
}
inline std::string hsl_hex(double h, double s, double l) {
    auto f = [&](double n) {
        double k = std::fmod(n + h / 30.0, 12.0), a = s * std::min(l, 1 - l);
        return l - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
    };
    char b[8]; snprintf(b, sizeof b, "#%02x%02x%02x", (int)std::lround(f(0) * 255), (int)std::lround(f(8) * 255), (int)std::lround(f(4) * 255));
    return b;
}
inline double hue_dist(double a, double b) { double d = std::fabs(a - b); return std::min(d, 360 - d); }
}  // namespace detail

inline std::string smart_wal_accent() {
    static time_t last = -2;
    static std::string cache;
    time_t m = file_mtime(wal_css_path());
    if (m == last && !cache.empty()) return cache;
    last = m;
    struct C { std::string hex; double h, s, l; };
    std::vector<C> cs;
    for (int i = 1; i <= 6; i++) {
        std::string hx = wal_color("--color" + std::to_string(i), "");
        if (!valid_hex(hx)) continue;
        C c{hx, 0, 0, 0}; detail::hex_hsl(hx, c.h, c.s, c.l);
        if (c.s < 0.15 || c.l < 0.15 || c.l > 0.9) continue;   // сірі/майже чорні/білі — не акцент
        cs.push_back(c);
    }
    if (cs.empty()) return cache = wal_color("--color5", "#9929CF");
    const C* best = nullptr; double bs = -1e9;
    for (auto& c : cs) {
        double support = 0;
        for (auto& o : cs) if (&o != &c && detail::hue_dist(c.h, o.h) <= 35) support += o.s;
        double score = support + c.s - std::fabs(c.l - 0.55) * 1.2;
        if (score > bs) { bs = score; best = &c; }
    }
    double l = std::clamp(best->l, 0.45, 0.66), sat = std::max(best->s, 0.45);
    return cache = (l == best->l && sat == best->s) ? best->hex : detail::hsl_hex(best->h, sat, l);
}

inline std::string accent() {
    const Settings& s = settings();
    if (s.accent_mode == "custom" && valid_hex(s.accent_color)) return s.accent_color;
    return smart_wal_accent();
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
    // unsigned — переповнення визначене і не страшне (штамп лише порівнюється)
    return (long long)((unsigned long long)file_mtime_ns(wal_css_path()) * 1000003ULL + (unsigned long long)file_mtime_ns(settings_path()));
}

inline bool is_light_theme() { return color_scheme() != "prefer-dark"; }

// ---------- «Запечений» блюр (mgos-bakeblur) ----------
// ~/.cache/mgos/baked_blur_<монітор>.png — розмиті шпалери на весь монітор (логічний розмір).
// Віджет малює шматок цього зображення під своєю напівпрозорою плашкою замість живого блюру Hyprland.
// Якщо файла нема або налаштування вимкнене — bb::paint нічого не малює (лишається живий блюр).
namespace bb {
struct Cache { cairo_surface_t* s = nullptr; time_t mtime = 0; std::string mon; double k = 1; gint64 chk = 0, used = 0; };
inline Cache& cache() { static Cache c; return c; }
inline std::string& mon_name() { static std::string m; return m; }
// Монітор, на якому живе вікно віджета (для кількох моніторів).
inline void set_monitor(GdkMonitor* m) {
    const char* c = m ? gdk_monitor_get_connector(m) : nullptr;
    mon_name() = c ? c : "";
}
inline bool enabled() {
    const auto& st = settings();
    return st.baked_blur && st.hyprland_blur && !st.eco_effects;
}
// Лишаємо в пам'яті не більше ~10 с після останнього малювання (1/2 роздільності ≈ 2 МБ).
inline void release() { Cache& c = cache(); if (c.s) { cairo_surface_destroy(c.s); c.s = nullptr; c.mtime = 0; } }
inline std::string path_for(const std::string& mon) { return home() + "/.cache/mgos/baked_blur_" + mon + ".png"; }
inline std::string resolve() {
    std::string mon = mon_name();
    std::string p = mon.empty() ? "" : path_for(mon);
    if (mon.empty()) {   // монітор не задано — беремо перший наявний файл
        if (GDir* d = g_dir_open((home() + "/.cache/mgos").c_str(), 0, nullptr)) {
            while (const char* n = g_dir_read_name(d)) {
                std::string s = n;
                if (s.rfind("baked_blur_", 0) == 0 && s.size() > 15 && s.compare(s.size() - 4, 4, ".png") == 0) { p = home() + "/.cache/mgos/" + s; break; }
            }
            g_dir_close(d);
        }
    }
    return p;
}
// mtime файла блюру для поточного монітора (0 — нема): для перемальовування, коли шпалери змінились.
inline time_t current_mtime() { std::string p = resolve(); return p.empty() ? 0 : file_mtime(p); }
inline cairo_surface_t* surface() {
    Cache& c = cache();
    gint64 now = g_get_monotonic_time();
    if (c.s && now - c.chk < 1500000) { c.used = now; return c.s; }   // mtime перевіряємо не частіше, ніж раз на 1.5 с
    c.chk = now; c.used = now;
    std::string p = resolve();
    time_t m = p.empty() ? 0 : file_mtime(p);
    if (!m) { release(); return nullptr; }
    if (c.s && m == c.mtime && p == c.mon) return c.s;
    release();
    int fw = 0, fh = 0;
    if (!gdk_pixbuf_get_file_info(p.c_str(), &fw, &fh) || fw < 2 || fh < 2) return nullptr;
    // 1/2 роздільності: блюр гладкий, втрат майже нема, пам'яті вдвічі·вдвічі менше
    int hw = std::max(1, fw / 2), hh = std::max(1, fh / 2);
    GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(p.c_str(), hw, hh, FALSE, nullptr);
    if (!pb) return nullptr;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, hw, hh);
    cairo_t* cc = cairo_create(s);
    gdk_cairo_set_source_pixbuf(cc, pb, 0, 0);
    cairo_paint(cc);
    cairo_destroy(cc);
    g_object_unref(pb);
    c.s = s; c.mtime = m; c.mon = p; c.k = (double)hw / fw;
    return s;
}
// Малює шматок блюру. cr — у системі координат віджета (початок = його лівий верхній кут);
// x,y — позиція цього кута в координатах МОНІТОРА (для layer-shell на весь екран = позиція у вікні + маржини);
// w,h — розмір ділянки; radius ≥ 0 — клип із закругленням (радіус < 0 — без клипу).
// Повертає true, якщо щось намальовано.
inline bool paint(cairo_t* cr, double x, double y, double w, double h, double radius = -1) {
    if (!enabled() || w <= 0 || h <= 0) return false;
    cairo_surface_t* s = surface();
    if (!s) return false;
    Cache& c = cache();
    cairo_save(cr);
    if (radius >= 0) {
        double r = std::min({radius, w / 2, h / 2});
        cairo_new_path(cr);
        cairo_arc(cr, w - r, r, r, -M_PI / 2, 0);
        cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
        cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, r, r, r, M_PI, 1.5 * M_PI);
        cairo_close_path(cr);
        cairo_clip(cr);
    } else {
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_clip(cr);
    }
    cairo_translate(cr, -x, -y);
    cairo_scale(cr, 1.0 / c.k, 1.0 / c.k);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
    cairo_restore(cr);
    return true;
}
// Викликати після приховування вікна (разом з trim): звільняє зображення, якщо давно не малювали.
inline void trim() {
    Cache& c = cache();
    if (c.s && g_get_monotonic_time() - c.used > 10000000) release();
}

// ---- Підложка для вікон-шарів, що не на весь монітор (OSD, runner, dock, буфер...) ----
// bb::underlay(win, child, "namespace", radius [, plates]) повертає GtkOverlay, який треба поставити
// замість child як дочірній віджет вікна. Під child малюється блюр у межах плашок (за замовчуванням — сам child).
// Позицію шару на моніторі підказує Hyprland (hyprctl layers) після показу вікна.
struct Under {
    GtkWidget *area = nullptr, *win = nullptr, *child = nullptr;
    std::string ns, mon;
    double radius = 0, ox = 0, oy = 0, lw = 0, lh = 0;   // lw/lh — розмір шару на момент визначення позиції
    bool cx = false, bot = false;   // шар центрований по горизонталі / притиснутий до низу: при зміні розміру зсув рахуємо самі
    bool known = false, empty = true;   // empty — на моніторі немає вікон: тоді малюємо запечений блюр
    guint pending = 0;
    std::function<void(std::vector<GtkWidget*>&)> plates;
};
inline bool under_locate(Under* u) {
    try {
        auto L = nlohmann::json::parse(hypr_request("j/layers"));
        auto M = nlohmann::json::parse(hypr_request("j/monitors"));
        for (auto it = L.begin(); it != L.end(); ++it) {
            if (!it.value().contains("levels")) continue;
            for (auto lv = it.value()["levels"].begin(); lv != it.value()["levels"].end(); ++lv)
                for (auto& l : lv.value()) {
                    if (l.value("namespace", "") != u->ns) continue;
                    double mx = 0, my = 0;
                    for (auto& m : M) if (m.value("name", "") == it.key()) { mx = m.value("x", 0); my = m.value("y", 0); }
                    u->ox = l.value("x", 0.0) - mx; u->oy = l.value("y", 0.0) - my; u->mon = it.key();
                    u->lw = l.value("w", 0.0); u->lh = l.value("h", 0.0);
                    try {
                        int ws = -1;
                        for (auto& m : M) if (m.value("name", "") == it.key()) ws = m["activeWorkspace"].value("id", -1);
                        u->empty = true;
                        for (auto& c : nlohmann::json::parse(hypr_request("j/clients")))
                            if (c.value("mapped", false) && !c.value("hidden", false) && c["workspace"].value("id", -2) == ws) { u->empty = false; break; }
                    } catch (...) { u->empty = false; }
                    return true;
                }
        }
    } catch (...) {}
    return false;
}
inline void under_schedule(Under* u, int ms) {
    if (u->pending) g_source_remove(u->pending);
    u->pending = g_timeout_add(ms, [](gpointer d) -> gboolean {
        Under* u = (Under*)d; u->pending = 0;
        if (gtk_widget_get_mapped(u->win) && under_locate(u)) { u->known = true; gtk_widget_queue_draw(u->area); }
        return G_SOURCE_REMOVE;
    }, u);
}
inline void under_draw(GtkDrawingArea*, cairo_t* cr, int, int, gpointer d) {
    Under* u = (Under*)d;
    if (!enabled() || !u->known) return;
    if (!u->empty) return;   // поверх програм — живий блюр Hyprland (запечений дає xray)
    mon_name() = u->mon;
    std::vector<GtkWidget*> v;
    if (u->plates) u->plates(v); else v.push_back(u->child);
    double ax = 0, ay = 0;   // зсув шару відносно моменту визначення (вікно змінило розмір, поки Hyprland ще не відповів)
    if (u->cx) ax = (u->lw - gtk_widget_get_width(u->win)) / 2.0;
    if (u->bot) ay = u->lh - gtk_widget_get_height(u->win);
    for (GtkWidget* p : v) {
        graphene_rect_t r;
        if (!gtk_widget_get_mapped(p) || !gtk_widget_compute_bounds(p, u->area, &r)) continue;
        cairo_save(cr);
        cairo_translate(cr, r.origin.x, r.origin.y);
        paint(cr, u->ox + ax + r.origin.x, u->oy + ay + r.origin.y, r.size.width, r.size.height, u->radius);
        cairo_restore(cr);
    }
}
inline GtkWidget* underlay(GtkWidget* win, GtkWidget* child, const char* ns, double radius,
                           std::function<void(std::vector<GtkWidget*>&)> plates = nullptr,
                           bool center_x = false, bool bottom = false) {
    Under* u = new Under;
    u->cx = center_x; u->bot = bottom;
    u->win = win; u->child = child; u->ns = ns; u->radius = radius; u->plates = std::move(plates);
    GtkWidget* ov = gtk_overlay_new();
    u->area = gtk_drawing_area_new();
    gtk_widget_set_can_target(u->area, FALSE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(u->area), under_draw, u, nullptr);
    gtk_overlay_set_child(GTK_OVERLAY(ov), u->area);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), child);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(ov), child, TRUE);
    g_signal_connect(win, "map", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        Under* u = (Under*)d;
        u->known = false;
        if (!enabled()) return;
        under_schedule(u, 35);
        g_timeout_add(250, [](gpointer d) -> gboolean { Under* u = (Under*)d; if (gtk_widget_get_mapped(u->win) && under_locate(u)) { u->known = true; gtk_widget_queue_draw(u->area); } return G_SOURCE_REMOVE; }, u);
    }), u);
    g_signal_connect(win, "unmap", G_CALLBACK(+[](GtkWidget*, gpointer) {
        g_timeout_add_seconds(11, [](gpointer) -> gboolean { trim(); return G_SOURCE_REMOVE; }, nullptr);
    }), u);
    g_signal_connect(u->area, "resize", G_CALLBACK(+[](GtkDrawingArea*, int, int, gpointer d) {
        Under* u = (Under*)d;
        if (u->known && gtk_widget_get_mapped(u->win)) under_schedule(u, 30);
    }), u);
    return ov;
}
}  // namespace bb

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
/* «здача» вікна в іконку: вдавлюється (плющиться), відскакує з перебігом, вгамовується */
@keyframes sysui-genie-squash {
    0%   { -gtk-icon-transform: none; opacity: 1; }
    16%  { -gtk-icon-transform: translateY(4px) scale(1.22, 0.62); opacity: 0.8; }
    38%  { -gtk-icon-transform: translateY(-5px) scale(0.9, 1.16); opacity: 1; }
    58%  { -gtk-icon-transform: translateY(1px) scale(1.06, 0.95); }
    78%  { -gtk-icon-transform: scale(0.98, 1.02); }
    100% { -gtk-icon-transform: none; opacity: 1; }
}
.genie-squash   { animation: sysui-genie-squash 520ms cubic-bezier(0.25, 0.8, 0.4, 1); }

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

// Один шрифт на всі панелі. Раніше модулі змішували JetBrainsMono Nerd + SF Pro у кількох
// накресленнях — кожне відкрите накреслення FreeType тримає ~1.5 МБ heap у КОЖНОМУ процесі.
// Тепер текст — panel_font, а значки Nerd Font (PUA) — з «Symbols Nerd Font» (лише гліфи).
// Пріоритет вищий за CSS модулів і live_css, тож перекриває їхні font-family.
inline void apply_font_setting() {
    static GtkCssProvider* fp = nullptr;
    static std::string last;
    std::string f = settings().panel_font;
    std::string clean;
    for (char c : f) if (c != '"' && c != '\\' && c != ';' && c != '{' && c != '}') clean += c;
    if (clean.empty()) clean = "SF Pro Display";
    if (fp && clean == last) return;
    last = clean;
    GdkDisplay* d = gdk_display_get_default();
    if (!d) return;
    if (!fp) {
        fp = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(d, GTK_STYLE_PROVIDER(fp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 5);
    }
    std::string css = "* { font-family: \"" + clean + "\", \"Symbols Nerd Font\", sans-serif; }";
    gtk_css_provider_load_from_string(fp, css.c_str());
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
    apply_font_setting();
    // Перемикач анімацій і шрифт застосовуються наживо
    g_timeout_add_seconds(2, [](gpointer) -> gboolean {
        static long long last = -1;
        long long st = theme_stamp();
        if (st != last) { last = st; apply_animation_setting(); apply_font_setting(); }
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

// Взаємне закриття оверлеїв: щойно відкривається один (шпалери, годинник, CC, буфер, runner, powermenu, about),
// решта згортаються своєю звичайною анімацією. Без демонів: одноразовий неблокуючий «hide» у сокети модулів
// (топбар — «clock-close» у topbar.sock). Хто не запущений — connect просто не вдається.
inline void overlay_sock_send(const std::string& name, const char* msg) {
    std::string path = home() + "/.config/SYSui/" + name + ".sock";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) { ssize_t w = write(fd, msg, strlen(msg)); (void)w; }
    close(fd);
}
// self — свій ідентифікатор ("wallpapers", "clock", "cc", "clipboard", "runner", "powermenu", "about");
// keep — ще один, який не чіпати (напр. стрічка шпалер, відкрита кнопкою з годинника, не закриває годинник).
inline void close_other_overlays(const char* self, const char* keep = "") {
    static const char* socks[] = {"wallpapers", "cc", "clipboard", "runner", "powermenu", "about"};
    for (const char* s : socks)
        if (strcmp(s, self) && strcmp(s, keep)) overlay_sock_send(s, "hide");
    if (strcmp(self, "clock") && strcmp(keep, "clock")) overlay_sock_send("topbar", "clock-close");
}
// Зручно для простих вікон: щоразу, коли вікно показується — закрити інші.
inline void exclusive_overlay(GtkWidget* win, const char* self) {
    g_signal_connect(win, "show", G_CALLBACK(+[](GtkWidget*, gpointer d) { close_other_overlays((const char*)d); }), (gpointer)self);
}

// Плавний прогрес-бар OSD: замість ривка — доїзд до цілі за ~ms мс (ease-out cubic) на tick-callback кадрів.
// Повторний виклик під час анімації просто змінює ціль — затиснута клавіша дає безперервний рух.
struct FracAnim { double from = 0, to = 0, cur = 0; gint64 t0 = 0; int ms = 220; guint id = 0; };
inline void animate_fraction(GtkWidget* bar, double target, int ms = 220) {
    target = std::clamp(target, 0.0, 1.0);
    auto* a = (FracAnim*)g_object_get_data(G_OBJECT(bar), "sysui-frac");
    if (!a) {
        a = new FracAnim();
        a->cur = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(bar));
        g_object_set_data_full(G_OBJECT(bar), "sysui-frac", a, [](gpointer p) { delete (FracAnim*)p; });
    }
    // вікно сховане → анімувати нікому: ставимо одразу (перший показ не «виїжджає» з нуля)
    if (!gtk_widget_get_mapped(bar)) {
        if (a->id) { gtk_widget_remove_tick_callback(bar, a->id); a->id = 0; }
        a->cur = a->to = target;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), target);
        return;
    }
    a->from = a->cur; a->to = target; a->ms = ms;
    a->t0 = g_get_monotonic_time();
    if (a->id) return;
    a->id = gtk_widget_add_tick_callback(bar, [](GtkWidget* w, GdkFrameClock*, gpointer d) -> gboolean {
        auto* a = (FracAnim*)d;
        double k = std::min(1.0, (g_get_monotonic_time() - a->t0) / (a->ms * 1000.0));
        double e = 1 - std::pow(1 - k, 3);
        a->cur = a->from + (a->to - a->from) * e;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w), a->cur);
        if (k >= 1.0) { a->id = 0; return G_SOURCE_REMOVE; }
        return G_SOURCE_CONTINUE;
    }, a, nullptr);
}

// Розташування OSD-плашки (layer-shell). pos: bottom | top | center | top-right | bottom-right.
// Поля рахуються від краю робочої області (топбар/док з exclusive zone уже враховані композитором).
inline void place_osd(GtkWidget* win, const std::string& pos) {
    GtkWindow* w = GTK_WINDOW(win);
    bool top = pos == "top" || pos == "top-right";
    bool bottom = pos == "bottom" || pos == "bottom-right" || (pos != "top" && pos != "top-right" && pos != "center");
    bool right = pos == "top-right" || pos == "bottom-right";
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_TOP, top);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_BOTTOM, bottom);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_RIGHT, right);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_LEFT, FALSE);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_TOP, top ? 14 : 0);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_BOTTOM, bottom ? (right ? 24 : 120) : 0);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_RIGHT, right ? 16 : 0);
}
// Перед кожною появою — свіжа позиція з налаштувань (kind: "osd" або "notify")
inline void follow_osd_position(GtkWidget* win, bool notify) {
    g_signal_connect(win, "show", G_CALLBACK(+[](GtkWidget* w, gpointer d) {
        const auto& st = settings();
        place_osd(w, d ? st.notify_position : st.osd_position);
    }), notify ? GINT_TO_POINTER(1) : nullptr);
}

}  // namespace sysui
