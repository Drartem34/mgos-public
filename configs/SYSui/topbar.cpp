#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <set>
#include <dirent.h>
#include <gio/gio.h>
#include "sysui_common.h"

using json = nlohmann::json;

// ===================== СТАН ТА КЕШ =====================
GtkCssProvider* g_css_provider = nullptr;
GtkWidget* topbar_window = nullptr;

struct WalColors {
    std::string fg = "#ffffff"; 
    std::string accent = "#9929CF"; 
    std::string color3 = "#f9e2af"; // жовтий (гучність)
    std::string color4 = "#89b4fa"; // синій (wifi/активний воркспейс)
    std::string danger = "#f38ba8"; // червоний
};
static WalColors g_wal;

GtkWidget* ws_labels[10];
GtkWidget* ws_cells[10];
GtkWidget* clock_label;
GtkWidget* lang_label;
GtkWidget* net_icon;
GtkWidget* vol_label;
GtkWidget* tray_box;
GtkWidget* mouse_box = nullptr;
GtkWidget* mouse_label = nullptr;

std::map<std::string, std::string> pseudo_tray_apps = {
    {"telegram-desktop", "telegram"},
    {"discord", "discord"},
    {"steam", "steam"},
    {"spotify", "spotify"},
    {"obs", "obs"}
};

// ===================== УТИЛІТИ =====================
static std::string exec(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

// Раніше: system("pgrep -x ...") для КОЖНОЇ з 5 програм кожні 2 с (= 10 fork/exec bash+pgrep).
// Тепер один прохід по /proc/*/comm. comm обрізається ядром до 15 символів,
// тому порівнюємо з перших 15 (pgrep -x "telegram-desktop" через це взагалі ніколи не спрацьовував).
static std::set<std::string> running_process_names() {
    std::set<std::string> names;
    DIR* d = opendir("/proc");
    if (!d) return names;
    struct dirent* e;
    char path[64], buf[64];
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE* f = fopen(path, "re");
        if (!f) continue;
        if (fgets(buf, sizeof(buf), f)) {
            size_t n = strlen(buf);
            if (n && buf[n-1] == '\n') buf[n-1] = 0;
            names.insert(buf);
        }
        fclose(f);
    }
    closedir(d);
    return names;
}

// ФІКС "ГУЧНІСТЬ ЗАВЖДИ 0%": std::stof залежить від системної локалі.
// GTK при ініціалізації виставляє локаль ОС (наприклад uk_UA.UTF-8), де
// десятковий роздільник — кома, а не крапка. wpctl же завжди друкує число
// з крапкою ("Volume: 0.45"), тому std::stof читав лише "0" і зупинявся
// на крапці, повертаючи 0.0. Цей парсер не залежить від локалі взагалі.
static float parse_float_locale_safe(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); i++; }

    long intPart = 0;
    bool any_digit = false;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) { intPart = intPart * 10 + (s[i] - '0'); i++; any_digit = true; }

    double frac = 0.0;
    if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
        i++;
        double scale = 0.1;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) {
            frac += (s[i] - '0') * scale;
            scale *= 0.1;
            i++;
            any_digit = true;
        }
    }

    if (!any_digit) return 0.0f;
    double result = intPart + frac;
    return (float)(neg ? -result : result);
}

// ===================== PYWAL ТЕМА ТА CSS =====================
std::string parse_css_var(const std::string& line, const std::string& var_name) {
    auto pos = line.find(var_name + ":");
    if (pos == std::string::npos) return "";
    auto start = line.find('#', pos);
    if (start == std::string::npos) {
        start = line.find("rgb", pos);
        if (start == std::string::npos) return "";
    }
    auto end = line.find(';', start);
    std::string val = line.substr(start, (end == std::string::npos ? line.size() : end) - start);
    while (!val.empty() && (val.back() == ' ' || val.back() == ';')) val.pop_back();
    return val;
}

void reload_wal_colors() {
    std::ifstream f(std::string(getenv("HOME")) + "/.cache/wal/colors.css");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        std::string v;
        if (!(v = parse_css_var(line, "--color5")).empty()) g_wal.accent = v;
        if (!(v = parse_css_var(line, "--color3")).empty()) g_wal.color3 = v;
        if (!(v = parse_css_var(line, "--color4")).empty()) g_wal.color4 = v;
        if (!(v = parse_css_var(line, "--color1")).empty()) g_wal.danger = v;
        if (!(v = parse_css_var(line, "--foreground")).empty()) g_wal.fg = v;
    }
    g_wal.accent = sysui::accent();   // єдиний акцент для всіх модулів (pywal або власний із налаштувань)
    g_wal.color4 = g_wal.accent;      // активний воркспейс і Wi-Fi — теж акцентом, щоб усе було в одному стилі
}

// ФІКС ЗАТРИМКИ КОЛЬОРІВ: Видалено "transition: all", залишено тільки transform для масштабу
static const char* CSS_BASE = R"(
* { font-family: "JetBrainsMono Nerd Font", "SF Pro Display", sans-serif; font-size: 14px; outline: none; box-shadow: none; }
window, window.background { background: transparent; }

.ignis-bar-inner { padding: 5px 10px; }

.module-box {
    border-radius: 10px;
    padding: 0 10px;
    min-height: 30px;
    /* анімації — спільні класи .live/.live-soft із sysui_common.h */
    margin: 0 3px;
}

/* Воркспейси */
.ws-btn { padding: 0 6px; margin: 3px 1px; border-radius: 8px; }
.ws-btn.occupied { font-weight: bold; }
.ws-btn.active { font-weight: bold; min-width: 25px; }

/* Іконки */
.power-icon { font-size: 16px; font-weight: bold; }
.clock-box { font-weight: bold; }
.lang-lbl { font-size: 15px; }
.lang-lbl.lang-text { font-size: 12px; font-weight: 800; letter-spacing: 1px; }
.lang-lbl.lang-icon { font-size: 13px; font-weight: 700; }
.wifi-icon { }
.vol-lbl { font-weight: bold; }

/* Псевдо-Трей */
.tray-icon { margin: 0 4px; }


/* Spotify міні-віджет: дві окремі бульбашки — обкладинка і назва */
.music-art-box { padding: 0 4px; }
.music-art { border-radius: 6px; min-width: 22px; min-height: 22px; }
.music-title-box { padding: 0 12px 0 10px; }
.music-icon { color: #1DB954; font-size: 15px; margin-right: 8px; }
.music-title { font-weight: bold; }
)";

static std::string CSS_DARK_TEMPLATE = R"(
/* ===== Стиль «Крапки»: ті самі бульбашки, але воркспейси — крапки, активний — витягнута пігулка ===== */
window.dots .ws-cell { padding: 0 3px; }
window.dots .ws-btn {
    padding: 0; margin: 0; font-size: 1px;
    min-width: 6px; min-height: 6px; border-radius: 999px;
    background-color: alpha($FG, 0.22);
    transition: min-width 260ms cubic-bezier(0.34, 1.4, 0.64, 1), min-height 200ms ease, background-color 200ms ease;
}
window.dots .ws-btn.occupied { min-width: 8px; min-height: 8px; background-color: alpha($FG, 0.75); }
window.dots .ws-btn.active { min-width: 24px; min-height: 8px; background-color: $COLOR4; box-shadow: 0 0 6px alpha($COLOR4, 0.55); }
window.dots .ws-cell:hover .ws-btn:not(.active) { background-color: alpha($COLOR4, 0.7); }
window.light-theme.dots .ws-btn { background-color: rgba(0,0,0,0.18); }
window.light-theme.dots .ws-btn.occupied { background-color: rgba(0,0,0,0.6); }
window.light-theme.dots .ws-btn.active { background-color: $COLOR4; }

.ignis-bar-inner { color: $FG; }
.module-box {
    background-color: $BUBBLE_BG;
    border: $BUBBLE_BORDER;
}

.mouse-lbl { color: $ACCENT; font-weight: bold; }

.ws-btn { color: #6c7086; }
.ws-btn.occupied { color: $FG; }
.ws-btn.active { background-color: $COLOR4; color: #121212; }

.power-icon { color: $DANGER; }
.clock-box { color: $FG; }
.wifi-icon { color: $COLOR4; }
.vol-lbl { color: $COLOR3; }
.overvol { color: #f38ba8 !important; }
.music-title { color: $FG; }
)";

static std::string CSS_LIGHT_TEMPLATE = R"(
window.light-theme .ignis-bar-inner { color: #121212; }
window.light-theme .module-box {
    background-color: $BUBBLE_BG_LIGHT;
    border: $BUBBLE_BORDER_LIGHT;
}
window.light-theme .module-box:hover { background-color: rgba(0, 0, 0, 0.1); }

window.light-theme .mouse-lbl { color: $ACCENT; font-weight: bold; }

window.light-theme .ws-btn { color: #888888; }
window.light-theme .ws-btn.occupied { color: #121212; }
window.light-theme .ws-btn.active { background-color: $COLOR4; color: #ffffff; }

window.light-theme .power-icon { color: $DANGER; }
window.light-theme .clock-box { color: #121212; }
window.light-theme .wifi-icon { color: $COLOR4; }
window.light-theme .vol-lbl { color: $COLOR3; }
window.light-theme .overvol { color: #f38ba8 !important; }
window.light-theme .music-title { color: #121212; }

/* ===== Стиль macOS: одна суцільна прозора розмита плашка замість бульбашок ===== */
window.macos .ignis-bar-inner {
    background-color: $MAC_BG;
    border-bottom: $MAC_BORDER;
    padding: 0 10px;
}
window.macos .module-box, window.light-theme.macos .module-box {
    background-color: transparent;
    border: none;
    margin: 0;
    padding: 0 9px;
    min-height: 30px;
    border-radius: 8px;
}
window.macos .module-box:hover { background-color: rgba(255,255,255,0.12); }
window.macos .ws-btn { margin: 4px 1px; }
window.light-theme.macos .ignis-bar-inner { background-color: $MAC_BG_LIGHT; border-bottom: $MAC_BORDER_LIGHT; }
window.light-theme.macos .module-box:hover { background-color: rgba(0,0,0,0.08); }
)";

static std::string build_themed_css() {
    reload_wal_colors();

    auto fill = [&](std::string tmpl) -> std::string {
        auto replace_all = [](std::string s, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return s;
        };
        const auto& st = sysui::settings();
        double op = st.topbar_opacity / 100.0;
        tmpl = replace_all(tmpl, "$BUBBLE_BG_LIGHT", sysui::rgba("#f5f5f5", std::min(1.0, op + 0.1)));
        tmpl = replace_all(tmpl, "$BUBBLE_BORDER_LIGHT", st.topbar_border ? "1px solid rgba(0,0,0,0.1)" : "none");
        tmpl = replace_all(tmpl, "$BUBBLE_BG", sysui::rgba("#1e1e2e", op));
        tmpl = replace_all(tmpl, "$BUBBLE_BORDER", st.topbar_border ? "1px solid rgba(255,255,255,0.1)" : "none");
        tmpl = replace_all(tmpl, "$MAC_BG_LIGHT", sysui::rgba("#f6f6f8", op));
        tmpl = replace_all(tmpl, "$MAC_BORDER_LIGHT", st.topbar_border ? "1px solid rgba(0,0,0,0.08)" : "none");
        tmpl = replace_all(tmpl, "$MAC_BG", sysui::rgba("#16161c", op));
        tmpl = replace_all(tmpl, "$MAC_BORDER", st.topbar_border ? "1px solid rgba(255,255,255,0.08)" : "none");
        tmpl = replace_all(tmpl, "$FG", g_wal.fg);
        tmpl = replace_all(tmpl, "$DANGER", g_wal.danger);
        tmpl = replace_all(tmpl, "$COLOR3", g_wal.color3);
        tmpl = replace_all(tmpl, "$COLOR4", g_wal.color4);
        tmpl = replace_all(tmpl, "$ACCENT", g_wal.accent);
        return tmpl;
    };

    return std::string(CSS_BASE) + fill(CSS_DARK_TEMPLATE) + fill(CSS_LIGHT_TEMPLATE);
}

void apply_css() {
    if (!g_css_provider) return;
    std::string css = build_themed_css();
    gtk_css_provider_load_from_string(g_css_provider, css.c_str());
}

// ===================== ЗМІНА ТЕМИ =====================
struct ThemeWatchData { GtkWidget* window; };

static gboolean check_theme(gpointer user_data) {
    auto* d = static_cast<ThemeWatchData*>(user_data);

    // Раніше popen("gsettings ...") кожні 2 с — тепер напряму через GSettings (без fork).
    std::string scheme = sysui::color_scheme();

    bool is_light = (scheme != "prefer-dark");

    static bool last_light = false;
    static bool first_run  = true;

    if (first_run || is_light != last_light) {
        if (is_light)
            gtk_widget_add_css_class(d->window, "light-theme");
        else
            gtk_widget_remove_css_class(d->window, "light-theme");
        last_light = is_light;
        first_run  = false;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean update_workspaces(gpointer);
static gboolean update_language(gpointer);

// Застосувати стиль топбару (bubbles / dots / macos) і режим мови — наживо, при зміні settings.json
static void apply_style_mode() {
    if (!topbar_window) return;
    const auto& style = sysui::settings().topbar_style;
    bool dots = style == "dots";
    if (style == "macos") gtk_widget_add_css_class(topbar_window, "macos"); else gtk_widget_remove_css_class(topbar_window, "macos");
    if (dots) gtk_widget_add_css_class(topbar_window, "dots"); else gtk_widget_remove_css_class(topbar_window, "dots");
    for (int i = 0; i < 10; i++) {
        if (!ws_labels[i]) continue;
        // крапки — без цифр і по центру по вертикалі; цифри — на всю висоту (пігулка активного)
        gtk_label_set_text(GTK_LABEL(ws_labels[i]), dots ? "" : std::to_string(i + 1).c_str());
        gtk_widget_set_valign(ws_labels[i], dots ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
    }
    if (ws_labels[0]) update_workspaces(nullptr);
    if (lang_label) update_language(nullptr);
}

// ФІКС ЗАТРИМКИ АКЦЕНТНИХ КОЛЬОРІВ: раніше тема pywal перечитувалась лише
// раз на 30 секунд (g_timeout_add(30000, ...)), тому нові кольори підтягувались
// з величезною затримкою. Тепер ми раз на секунду дешево перевіряємо mtime
// файлу colors.css і перебудовуємо CSS (apply_css) лише тоді, коли файл
// реально змінився — це і швидко, і не вантажить CPU зайвими перебудовами.
static gboolean reload_wal_timer(gpointer) {
    // Тема pywal АБО settings.json змінились → перебудовуємо CSS і стиль (наживо).
    static long long last = -1;
    long long st = sysui::theme_stamp();
    if (st != last) {
        last = st;
        apply_css();
        apply_style_mode();
    }
    return G_SOURCE_CONTINUE;
}

// ===================== ОБРОБНИКИ КЛІКІВ =====================
static void on_power_click(GtkGestureClick*, int, double, double, gpointer) {
    std::string cmd = "echo 'show' | socat - UNIX-CLIENT:" + std::string(getenv("HOME")) + "/.config/SYSui/powermenu.sock &";
    system(cmd.c_str());
}

static void on_lang_click(GtkGestureClick*, int, double, double, gpointer) {
    sysui::hypr_request("switchxkblayout all next");
}

// Клік по мережі/гучності відкриває відповідну сторінку Control Center
static void cc_send(const char* msg) {
    std::string path = sysui::home() + "/.config/SYSui/cc.sock";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) { ssize_t w = write(fd, msg, strlen(msg)); (void)w; }
    close(fd);
}

static void on_net_click(GtkGestureClick*, int, double, double, gpointer) { cc_send("page|network"); }

static void on_vol_click(GtkGestureClick*, int, double, double, gpointer) { cc_send("page|sound"); }

static gboolean on_vol_scroll(GtkEventControllerScroll*, double dx, double dy, gpointer) {
    if (dy > 0) system("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-");
    else if (dy < 0) system("wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+");
    return TRUE;
}

static void on_ws_click(GtkGestureClick*, int, double, double, gpointer user_data) {
    int ws = GPOINTER_TO_INT(user_data);
    sysui::hypr_request("dispatch workspace " + std::to_string(ws));
}

static void on_tray_click(GtkGestureClick* gesture, int, double, double, gpointer user_data) {
    const char* proc_name = (const char*)user_data;
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    
    if (btn == GDK_BUTTON_MIDDLE) {
        std::string cmd = "killall -9 " + std::string(proc_name) + " >/dev/null 2>&1 &";
        system(cmd.c_str());
    } else if (btn == GDK_BUTTON_PRIMARY) {
        std::string cmd = std::string(proc_name) + " >/dev/null 2>&1 &";
        system(cmd.c_str());
    }
}

// ===================== ОНОВЛЕННЯ ДАНИХ (ПОЛІНГ) =====================
static gboolean update_clock(gpointer) {
    time_t now = time(0);
    struct tm tstruct;
    localtime_r(&now, &tstruct);
    char buf[80];
    strftime(buf, sizeof(buf), " %H:%M", &tstruct);
    gtk_label_set_text(GTK_LABEL(clock_label), buf);
    return G_SOURCE_CONTINUE;
}

static gboolean update_workspaces(gpointer) {
    std::string out_act = sysui::hypr_request("j/activeworkspace");
    int active_id = 1;
    try { active_id = json::parse(out_act).value("id", 1); } catch(...) {}

    std::string out_ws = sysui::hypr_request("j/workspaces");
    std::vector<int> occupied;
    try {
        auto ws_list = json::parse(out_ws);
        for (const auto& w : ws_list) occupied.push_back(w.value("id", 0));
    } catch(...) {}

    // У стилі «Крапки» ховаємо зайві порожні воркспейси: показуємо 1..max(5, найбільший зайнятий/активний)
    bool dots = sysui::settings().topbar_style == "dots";
    int max_used = std::max(5, active_id);
    for (int id : occupied) if (id <= 10) max_used = std::max(max_used, id);
    for (int i = 1; i <= 10; i++) {
        gtk_widget_set_visible(ws_cells[i-1], !dots || i <= max_used);
        gtk_widget_remove_css_class(ws_labels[i-1], "active");
        gtk_widget_remove_css_class(ws_labels[i-1], "occupied");
        
        if (i == active_id) gtk_widget_add_css_class(ws_labels[i-1], "active");
        else if (std::find(occupied.begin(), occupied.end(), i) != occupied.end()) {
            gtk_widget_add_css_class(ws_labels[i-1], "occupied");
        }
    }
    return G_SOURCE_REMOVE;
}

// Двобуквений код розкладки з назви keymap ("Ukrainian" → UA, "English (US)" → EN ...)
static std::string layout_code(const std::string& km) {
    static const std::pair<const char*, const char*> MAP[] = {
        {"Ukrainian","UA"},{"English","EN"},{"Russian","RU"},{"Polish","PL"},{"German","DE"},
        {"French","FR"},{"Spanish","ES"},{"Italian","IT"},{"Czech","CZ"},{"Japanese","JA"}};
    for (auto& [n, c] : MAP) if (km.find(n) != std::string::npos) return c;
    std::string c;
    for (char ch : km) { if (std::isalpha((unsigned char)ch)) c += (char)std::toupper((unsigned char)ch); if (c.size() == 2) break; }
    return c.empty() ? "??" : c;
}

static std::string layout_flag(const std::string& code) {
    if (code == "UA") return "🇺🇦";
    if (code == "EN") return "🇺🇸";
    if (code.size() != 2) return code;
    // регіональні індикатори Unicode: U+1F1E6 + (буква - 'A')
    std::string out;
    for (char ch : code) {
        unsigned cp = 0x1F1E6 + (ch - 'A');
        out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

static gboolean update_language(gpointer) {
    std::string out = sysui::hypr_request("j/devices");
    try {
        auto data = json::parse(out);
        for (const auto& kb : data["keyboards"]) {
            if (kb.value("main", false)) {
                std::string code = layout_code(kb.value("active_keymap", ""));
                const std::string& mode = sysui::settings().topbar_lang;
                gtk_widget_remove_css_class(lang_label, "lang-text");
                gtk_widget_remove_css_class(lang_label, "lang-icon");
                std::string txt;
                if (mode == "text")      { txt = code; gtk_widget_add_css_class(lang_label, "lang-text"); }
                else if (mode == "icon") { txt = "󰌌 " + code; gtk_widget_add_css_class(lang_label, "lang-icon"); }
                else                     { txt = layout_flag(code); }
                gtk_label_set_text(GTK_LABEL(lang_label), txt.c_str());
                break;
            }
        }
    } catch(...) {}
    return G_SOURCE_REMOVE;
}

static gboolean update_network(gpointer) {
    std::string eth = exec("nmcli -t -f TYPE,STATE dev | grep 'ethernet:connected'");
    if (!eth.empty()) {
        gtk_image_set_from_icon_name(GTK_IMAGE(net_icon), "network-wired-symbolic");
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(net_icon), "network-wireless-signal-excellent-symbolic");
    }
    return G_SOURCE_REMOVE;
}

static gboolean update_volume(gpointer) {
    std::string out = exec("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    if (out.empty()) return G_SOURCE_CONTINUE;
    
    bool muted = (out.find("MUTED") != std::string::npos);
    size_t pos = out.find(':');
    if (pos != std::string::npos) {
        try {
            float vol = parse_float_locale_safe(out.substr(pos + 1)) * 100.0f;
            std::string icon = muted ? "󰝟" : (vol > 66 ? "󰕾" : (vol > 33 ? "" : ""));
            gtk_label_set_text(GTK_LABEL(vol_label), (icon + " " + std::to_string((int)vol) + "%").c_str());
            
            if (vol > 100.0f) {
                gtk_widget_add_css_class(vol_label, "overvol");
            } else {
                gtk_widget_remove_css_class(vol_label, "overvol");
            }
        } catch(...) {}
    }
    return G_SOURCE_CONTINUE;
}

static gboolean update_mouse_battery(gpointer) {
    // Смикаємо atk-tool. Якщо миша спить, він поверне пусту строку
    std::string out = exec("atk-tool status --json 2>/dev/null");
    if (out.empty() || out.find("{") == std::string::npos) {
        gtk_widget_set_visible(mouse_box, FALSE);
        return G_SOURCE_CONTINUE;
    }
    
    try {
        auto data = json::parse(out);
        int bat = 0;
        
        // ФІКС: ліземо всередину об'єкта "battery"
        if (data.contains("battery") && data["battery"].is_object()) {
            auto bat_obj = data["battery"];
            if (bat_obj.contains("percentage")) {
                bat = bat_obj["percentage"];
            } else if (bat_obj.contains("level")) {
                bat = bat_obj["level"];
            }
        } 
        // Про всяк випадок залишаємо і старий плоский варіант
        else if (data.contains("percentage")) {
            bat = data["percentage"];
        }
        
        if (bat > 0) {
            std::string text = "󰍽 " + std::to_string(bat) + "%";
            gtk_label_set_text(GTK_LABEL(mouse_label), text.c_str());
            gtk_widget_set_visible(mouse_box, TRUE); // Показуємо плашку
        } else {
            gtk_widget_set_visible(mouse_box, FALSE);
        }
    } catch(...) {
        gtk_widget_set_visible(mouse_box, FALSE);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean update_pseudo_tray(gpointer) {
    // Раніше трей видаляв і заново створював усі віджети кожні 2 с (фрагментація heap).
    // Тепер перебудовуємо лише коли змінився набір запущених програм.
    static std::string last_sig = "\x01";
    std::set<std::string> procs = running_process_names();
    std::vector<std::pair<std::string, std::string>> active;
    std::string sig;
    for (const auto& [proc, icon_name] : pseudo_tray_apps) {
        if (procs.count(proc.substr(0, 15))) { active.emplace_back(proc, icon_name); sig += proc + ";"; }
    }
    if (sig == last_sig) return G_SOURCE_CONTINUE;
    last_sig = sig;

    GtkWidget* child = gtk_widget_get_first_child(tray_box);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(tray_box), child);
        child = next;
    }

    for (const auto& [proc_ref, icon_name] : active) {
        const std::string& proc = pseudo_tray_apps.find(proc_ref)->first;  // стабільний c_str() для колбека
        {
            GtkWidget* img = gtk_image_new_from_icon_name(icon_name.c_str());
            gtk_widget_set_size_request(img, 18, 18);
            
            GtkWidget* btn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(btn, "tray-icon");
            gtk_widget_add_css_class(btn, "live");
            gtk_box_append(GTK_BOX(btn), img);

            GtkGesture* click = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
            g_signal_connect(click, "released", G_CALLBACK(on_tray_click), (gpointer)proc.c_str());
            gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click));

            gtk_box_append(GTK_BOX(tray_box), btn);
        }
    }
    return G_SOURCE_CONTINUE;
}

// ===================== SPOTIFY МІНІ-ВІДЖЕТ =====================
// Дві бульбашки (обкладинка + назва), видно лише коли Spotify реально грає.
// Все через D-Bus (MPRIS) напряму: жодних playerctl/fork, оновлення по сигналах.
static GtkWidget* music_revealer = nullptr;
static GtkWidget* music_art      = nullptr;   // GtkPicture
static GtkWidget* music_title    = nullptr;   // GtkLabel
static GDBusProxy* g_spotify     = nullptr;
static std::string g_art_wanted;              // URL обкладинки, яку зараз треба показати
static std::string g_art_shown;               // URL обкладинки, яка вже показана
static guint g_music_hide_timer  = 0;
static const int MUSIC_ART_PX    = 22;

static std::string music_cache_dir() {
    return std::string(getenv("HOME")) + "/.cache/SYSui/spotify_art";
}

static void music_set_art_file(const std::string& path) {
    int sf = std::max(1, gtk_widget_get_scale_factor(music_art));
    GdkTexture* t = sysui::load_texture_cover(path.c_str(), MUSIC_ART_PX * sf, MUSIC_ART_PX * sf);
    gtk_image_set_from_paintable(GTK_IMAGE(music_art), t ? GDK_PAINTABLE(t) : nullptr);
    if (t) g_object_unref(t);
}

// Тримаємо кеш обкладинок маленьким: якщо файлів > 60 — чистимо все, крім поточної.
static void music_prune_cache(const std::string& keep) {
    std::string dir = music_cache_dir();
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) if (e->d_name[0] != '.') files.push_back(dir + "/" + e->d_name);
    closedir(d);
    if (files.size() <= 60) return;
    for (const auto& f : files) if (f != keep) unlink(f.c_str());
}

struct ArtJob { std::string url, path, tmp; };

static void on_art_downloaded(GObject* src, GAsyncResult* res, gpointer data) {
    auto* job = static_cast<ArtJob*>(data);
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, nullptr);
    if (ok && rename(job->tmp.c_str(), job->path.c_str()) == 0) {
        if (job->url == g_art_wanted) { music_set_art_file(job->path); g_art_shown = job->url; }
        music_prune_cache(job->path);
    } else {
        unlink(job->tmp.c_str());
    }
    g_object_unref(src);
    delete job;
}

static void music_load_art(const std::string& url) {
    g_art_wanted = url;
    if (url == g_art_shown) return;
    if (url.empty()) { gtk_image_clear(GTK_IMAGE(music_art)); g_art_shown.clear(); return; }

    if (url.rfind("file://", 0) == 0) {
        gchar* fn = g_filename_from_uri(url.c_str(), nullptr, nullptr);
        if (fn) { music_set_art_file(fn); g_free(fn); g_art_shown = url; }
        return;
    }

    gchar* hash = g_compute_checksum_for_string(G_CHECKSUM_MD5, url.c_str(), -1);
    std::string path = music_cache_dir() + "/" + hash + ".img";
    g_free(hash);

    if (access(path.c_str(), R_OK) == 0) { music_set_art_file(path); g_art_shown = url; return; }

    g_mkdir_with_parents(music_cache_dir().c_str(), 0755);
    auto* job = new ArtJob{url, path, path + ".part"};
    GSubprocess* sp = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr,
                                       "curl", "-sfL", "--max-time", "10", "-o", job->tmp.c_str(), url.c_str(), nullptr);
    if (!sp) { delete job; return; }
    g_subprocess_wait_check_async(sp, nullptr, on_art_downloaded, job);
}

static void music_show(bool show) {
    if (show) {
        if (g_music_hide_timer) { g_source_remove(g_music_hide_timer); g_music_hide_timer = 0; }
        gtk_widget_set_visible(music_revealer, TRUE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(music_revealer), TRUE);
    } else if (!g_music_hide_timer && gtk_revealer_get_reveal_child(GTK_REVEALER(music_revealer))) {
        // Невелика затримка: при перемиканні треку Spotify на мить шле "Paused" — не блимаємо.
        g_music_hide_timer = g_timeout_add(1500, [](gpointer) -> gboolean {
            g_music_hide_timer = 0;
            gtk_revealer_set_reveal_child(GTK_REVEALER(music_revealer), FALSE);
            return G_SOURCE_REMOVE;
        }, nullptr);
    }
}

static void music_update() {
    if (!music_revealer) return;
    if (!g_spotify) { music_show(false); return; }

    std::string status, title, artist, album, art;
    if (GVariant* st = g_dbus_proxy_get_cached_property(g_spotify, "PlaybackStatus")) {
        if (g_variant_is_of_type(st, G_VARIANT_TYPE_STRING)) status = g_variant_get_string(st, nullptr);
        g_variant_unref(st);
    }
    if (GVariant* md = g_dbus_proxy_get_cached_property(g_spotify, "Metadata")) {
        if (g_variant_is_of_type(md, G_VARIANT_TYPE_VARDICT)) {
            GVariantDict d;
            g_variant_dict_init(&d, md);
            const char* v = nullptr;
            if (g_variant_dict_lookup(&d, "xesam:title", "&s", &v))  title = v;
            if (g_variant_dict_lookup(&d, "xesam:album", "&s", &v))  album = v;
            if (g_variant_dict_lookup(&d, "mpris:artUrl", "&s", &v)) art = v;
            if (GVariant* a = g_variant_dict_lookup_value(&d, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY)) {
                gsize n = 0;
                const gchar** arr = g_variant_get_strv(a, &n);
                for (gsize i = 0; i < n; i++) { if (i) artist += ", "; artist += arr[i]; }
                g_free(arr);
                g_variant_unref(a);
            }
            g_variant_dict_clear(&d);
        }
        g_variant_unref(md);
    }

    bool playing = (status == "Playing") && !title.empty();
    if (playing) {
        std::string text = artist.empty() ? title : title + "  ·  " + artist;
        gtk_label_set_text(GTK_LABEL(music_title), text.c_str());
        std::string tip = title + (artist.empty() ? "" : "\n" + artist) + (album.empty() ? "" : "\n" + album) +
                          "\n\nЛКМ — відкрити Spotify · СКМ — пауза · ПКМ — наступний";
        gtk_widget_set_tooltip_text(music_revealer, tip.c_str());
        music_load_art(art);
    }
    music_show(playing);
}

static void on_spotify_props_changed(GDBusProxy*, GVariant*, GStrv, gpointer) { music_update(); }

// Страховка: деякі версії Spotify не завжди шлють PropertiesChanged.
// Раз на 5 с дешево (D-Bus, без fork) перечитуємо властивості, лише поки Spotify запущений.
static gboolean music_poll(gpointer) {
    if (!g_spotify) return G_SOURCE_CONTINUE;
    g_dbus_connection_call(g_dbus_proxy_get_connection(g_spotify), g_dbus_proxy_get_name(g_spotify),
        "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", "org.mpris.MediaPlayer2.Player"), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer) {
            GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
            if (!r) return;
            if (g_spotify) {
                GVariant* dict = g_variant_get_child_value(r, 0);
                GVariantIter it;
                const char* key;
                GVariant* val;
                g_variant_iter_init(&it, dict);
                while (g_variant_iter_next(&it, "{&sv}", &key, &val)) {
                    if (!strcmp(key, "PlaybackStatus") || !strcmp(key, "Metadata"))
                        g_dbus_proxy_set_cached_property(g_spotify, key, val);
                    g_variant_unref(val);
                }
                g_variant_unref(dict);
                music_update();
            }
            g_variant_unref(r);
        }, nullptr);
    return G_SOURCE_CONTINUE;
}

static void on_spotify_appeared(GDBusConnection* conn, const gchar* name, const gchar*, gpointer) {
    g_dbus_proxy_new(conn, G_DBUS_PROXY_FLAGS_NONE, nullptr, name, "/org/mpris/MediaPlayer2",
                     "org.mpris.MediaPlayer2.Player", nullptr,
                     [](GObject*, GAsyncResult* res, gpointer) {
                         GDBusProxy* p = g_dbus_proxy_new_finish(res, nullptr);
                         if (!p) return;
                         if (g_spotify) g_object_unref(g_spotify);
                         g_spotify = p;
                         g_signal_connect(p, "g-properties-changed", G_CALLBACK(on_spotify_props_changed), nullptr);
                         music_update();
                     }, nullptr);
}

static void on_spotify_vanished(GDBusConnection*, const gchar*, gpointer) {
    if (g_spotify) { g_object_unref(g_spotify); g_spotify = nullptr; }
    music_update();
}

static void spotify_call(const char* method) {
    if (g_spotify) g_dbus_proxy_call(g_spotify, method, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

static void on_music_click(GtkGestureClick* gesture, int, double, double, gpointer) {
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (btn == GDK_BUTTON_PRIMARY)       sysui::hypr_request("dispatch focuswindow class:(?i)^spotify$");
    else if (btn == GDK_BUTTON_MIDDLE)   spotify_call("PlayPause");
    else if (btn == GDK_BUTTON_SECONDARY) spotify_call("Next");
}

static GtkWidget* music_build() {
    // Бульбашка 1: квадратна обкладинка
    music_art = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(music_art), MUSIC_ART_PX);
    gtk_widget_set_valign(music_art, GTK_ALIGN_CENTER);
    gtk_widget_set_overflow(music_art, GTK_OVERFLOW_HIDDEN);   // обрізаємо по border-radius
    gtk_widget_add_css_class(music_art, "music-art");

    GtkWidget* art_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(art_box, "module-box");
    gtk_widget_add_css_class(art_box, "music-art-box");
    gtk_box_append(GTK_BOX(art_box), music_art);

    // Бульбашка 2: іконка Spotify + назва треку
    GtkWidget* icon = gtk_label_new("");   // nf-fa-spotify
    gtk_widget_add_css_class(icon, "music-icon");
    music_title = gtk_label_new("");
    gtk_widget_add_css_class(music_title, "music-title");
    gtk_label_set_ellipsize(GTK_LABEL(music_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(music_title), 42);
    gtk_label_set_single_line_mode(GTK_LABEL(music_title), TRUE);

    GtkWidget* title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(title_box, "module-box");
    gtk_widget_add_css_class(title_box, "music-title-box");
    gtk_box_append(GTK_BOX(title_box), icon);
    gtk_box_append(GTK_BOX(title_box), music_title);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(row), art_box);
    gtk_box_append(GTK_BOX(row), title_box);
    gtk_widget_set_cursor_from_name(row, "pointer");
    gtk_widget_add_css_class(art_box, "live");
    gtk_widget_add_css_class(title_box, "live-soft");

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "released", G_CALLBACK(on_music_click), nullptr);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

    music_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(music_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(music_revealer), 250);
    gtk_revealer_set_reveal_child(GTK_REVEALER(music_revealer), FALSE);
    gtk_revealer_set_child(GTK_REVEALER(music_revealer), row);
    gtk_widget_set_visible(music_revealer, FALSE);
    // Після анімації згортання повністю ховаємо, щоб не лишався відступ у left_box.
    g_signal_connect(music_revealer, "notify::child-revealed", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        GtkRevealer* r = GTK_REVEALER(o);
        if (!gtk_revealer_get_child_revealed(r) && !gtk_revealer_get_reveal_child(r)) {
            gtk_widget_set_visible(GTK_WIDGET(r), FALSE);
            gtk_image_clear(GTK_IMAGE(music_art));  // звільняємо текстуру
            g_art_shown.clear();
        }
    }), nullptr);
    return music_revealer;
}

static void music_init() {
    g_bus_watch_name(G_BUS_TYPE_SESSION, "org.mpris.MediaPlayer2.spotify", G_BUS_NAME_WATCHER_FLAGS_NONE,
                     on_spotify_appeared, on_spotify_vanished, nullptr, nullptr);
    g_timeout_add_seconds(5, music_poll, nullptr);
}

// ===================== UI КОНСТРУКТОРИ =====================
GtkWidget* create_clickable_box(GtkWidget* child, GCallback on_click, gpointer data = nullptr) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "module-box");
    if (on_click) gtk_widget_add_css_class(box, "live-soft");
    gtk_box_append(GTK_BOX(box), child);

    if (on_click) {
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "released", on_click, data);
        gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
    }
    return box;
}

// ===================== ACTIVATE =====================
static void activate(GtkApplication* app, gpointer user_data) {
    sysui::mem_ready();
    g_css_provider = gtk_css_provider_new();
    apply_css();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    topbar_window = gtk_application_window_new(app);
    gtk_layer_init_for_window(GTK_WINDOW(topbar_window));
    gtk_layer_set_namespace(GTK_WINDOW(topbar_window), "topbar");
    gtk_layer_set_layer(GTK_WINDOW(topbar_window), GTK_LAYER_SHELL_LAYER_TOP);
    
    gtk_layer_set_anchor(GTK_WINDOW(topbar_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(topbar_window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(topbar_window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_auto_exclusive_zone_enable(GTK_WINDOW(topbar_window));

    GtkWidget* main_box = gtk_center_box_new();
    gtk_widget_add_css_class(main_box, "ignis-bar-inner");

    // --- ЛІВА ЧАСТИНА ---
    GtkWidget* left_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    
    GtkWidget* pwr_lbl = gtk_label_new("⏻");
    gtk_widget_add_css_class(pwr_lbl, "power-icon");
    gtk_box_append(GTK_BOX(left_box), create_clickable_box(pwr_lbl, G_CALLBACK(on_power_click)));

    GtkWidget* ws_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(ws_box, "module-box");
    for (int i = 1; i <= 10; i++) {
        ws_labels[i-1] = gtk_label_new(std::to_string(i).c_str());
        gtk_widget_add_css_class(ws_labels[i-1], "ws-btn");
        gtk_widget_add_css_class(ws_labels[i-1], "live");
        
        GtkWidget* ws_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(ws_btn_box, "ws-cell");
        ws_cells[i-1] = ws_btn_box;
        gtk_box_append(GTK_BOX(ws_btn_box), ws_labels[i-1]);
        
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "released", G_CALLBACK(on_ws_click), GINT_TO_POINTER(i));
        gtk_widget_add_controller(ws_btn_box, GTK_EVENT_CONTROLLER(click));
        
        gtk_box_append(GTK_BOX(ws_box), ws_btn_box);
    }
    gtk_box_append(GTK_BOX(left_box), ws_box);

    // Spotify: обкладинка + назва (з'являється лише коли щось грає)
    gtk_box_append(GTK_BOX(left_box), music_build());

    // --- ЦЕНТР ---
    GtkWidget* center_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    clock_label = gtk_label_new(" 00:00");
    gtk_widget_add_css_class(clock_label, "clock-box");
    gtk_box_append(GTK_BOX(center_box), create_clickable_box(clock_label, nullptr));

    // --- ПРАВА ЧАСТИНА ---
    GtkWidget* right_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    lang_label = gtk_label_new("🇺🇸");
    gtk_widget_add_css_class(lang_label, "lang-lbl");
    gtk_box_append(GTK_BOX(right_box), create_clickable_box(lang_label, G_CALLBACK(on_lang_click)));

    net_icon = gtk_image_new_from_icon_name("network-wireless-signal-excellent-symbolic");
    gtk_widget_add_css_class(net_icon, "wifi-icon");
    gtk_box_append(GTK_BOX(right_box), create_clickable_box(net_icon, G_CALLBACK(on_net_click)));

    vol_label = gtk_label_new("󰕾 100%");
    gtk_widget_add_css_class(vol_label, "vol-lbl");
    GtkWidget* vol_btn = create_clickable_box(vol_label, G_CALLBACK(on_vol_click));
    
    GtkEventController* scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_vol_scroll), nullptr);
    gtk_widget_add_controller(vol_btn, scroll);
    gtk_box_append(GTK_BOX(right_box), vol_btn);


    mouse_label = gtk_label_new("󰍽 --%");
    gtk_widget_add_css_class(mouse_label, "mouse-lbl");
    mouse_box = create_clickable_box(mouse_label, nullptr);
    gtk_widget_set_visible(mouse_box, FALSE); // Ховаємо одразу, поки не прилетять дані
    gtk_box_append(GTK_BOX(right_box), mouse_box);

    // Псевдо-Трей
    tray_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget* tray_module = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(tray_module, "module-box");
    gtk_box_append(GTK_BOX(tray_module), tray_box);
    gtk_box_append(GTK_BOX(right_box), tray_module);

    gtk_center_box_set_start_widget(GTK_CENTER_BOX(main_box), left_box);
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(main_box), center_box);
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(main_box), right_box);

    gtk_window_set_child(GTK_WINDOW(topbar_window), main_box);

    // Запуск таймерів
    update_workspaces(nullptr);
    update_clock(nullptr);      g_timeout_add(1000, update_clock, nullptr);
    update_language(nullptr);
    update_network(nullptr);
    update_volume(nullptr); 
    update_pseudo_tray(nullptr); g_timeout_add(3000, update_pseudo_tray, nullptr);
    update_mouse_battery(nullptr); g_timeout_add(10000, update_mouse_battery, nullptr);
    music_init();

    // Таймер для зміни теми
    auto* twd = new ThemeWatchData{topbar_window};
    check_theme(twd);  
    g_timeout_add(2000, check_theme, twd);
    
    
    // Перевірка Pywal (раз на секунду — перебудова CSS лише при реальній зміні файлу)
    reload_wal_timer(nullptr);
    g_timeout_add(1000, reload_wal_timer, nullptr);

    gtk_widget_set_visible(topbar_window, TRUE);
}

void listen_hyprland_ipc() {
    std::string xdg = getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "";
    std::string sig = getenv("HYPRLAND_INSTANCE_SIGNATURE") ? getenv("HYPRLAND_INSTANCE_SIGNATURE") : "";
    if (xdg.empty() || sig.empty()) return;
    
    std::string socket_path = xdg + "/hypr/" + sig + "/.socket2.sock";
    
    while (true) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) { sleep(1); continue; }
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            sleep(1);
            continue;
        }
        
        char buffer[1024];
        std::string leftover;
        while (true) {
            int bytes = read(sock, buffer, sizeof(buffer) - 1);
            if (bytes <= 0) break; // Disconnected
            
            buffer[bytes] = '\0';
            std::string data = leftover + buffer;
            leftover = "";
            
            size_t pos = 0;
            while ((pos = data.find('\n')) != std::string::npos) {
                std::string line = data.substr(0, pos);
                data.erase(0, pos + 1);
                
                if (line.find("workspace>>") == 0 || line.find("createworkspace>>") == 0 || line.find("destroyworkspace>>") == 0 || line.find("activewindow>>") == 0) {
                    g_idle_add([](gpointer) -> gboolean { update_workspaces(nullptr); return G_SOURCE_REMOVE; }, nullptr);
                } else if (line.find("activelayout>>") == 0) {
                    g_idle_add([](gpointer) -> gboolean { update_language(nullptr); return G_SOURCE_REMOVE; }, nullptr);
                }
            }
            leftover = data;
        }
        close(sock);
        sleep(1);
    }
}

// pactl/nmcli шлють пачки подій — коалесуємо їх, щоб не робити fork на кожну.
static std::atomic<bool> g_vol_pending{false};
static std::atomic<bool> g_net_pending{false};

void listen_volume_ipc() {
    FILE* pipe = popen("pactl subscribe 2>/dev/null", "r");
    if (!pipe) return;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line = buffer;
        if (line.find("Event 'change' on sink") != std::string::npos || line.find("Event 'new' on sink") != std::string::npos) {
            if (!g_vol_pending.exchange(true))
                g_timeout_add(60, [](gpointer) -> gboolean { g_vol_pending = false; update_volume(nullptr); return G_SOURCE_REMOVE; }, nullptr);
        }
    }
    pclose(pipe);
}

void listen_network_ipc() {
    FILE* pipe = popen("nmcli monitor 2>/dev/null", "r");
    if (!pipe) return;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        if (!g_net_pending.exchange(true))
            g_timeout_add(700, [](gpointer) -> gboolean { g_net_pending = false; update_network(nullptr); return G_SOURCE_REMOVE; }, nullptr);
    }
    pclose(pipe);
}

int main(int argc, char **argv) {
    sysui::mem_init();
    std::thread ipc_thread(listen_hyprland_ipc);
    ipc_thread.detach();
    std::thread vol_thread(listen_volume_ipc);
    vol_thread.detach();
    std::thread net_thread(listen_network_ipc);
    net_thread.detach();

    GtkApplication *app = gtk_application_new("com.SYSui.topbar", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}