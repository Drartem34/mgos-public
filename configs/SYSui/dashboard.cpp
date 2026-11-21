// dashboard.cpp — дашборд на робочому столі 1 (SYSui).
//
// Шар layer-shell BOTTOM (над шпалерами, під вікнами) з картками-віджетами: годинник, система,
// мережа, погода (wttr.in, без ключа), музика (playerctl), нотатки, швидкі дії, календар.
// Видно лише коли активний робочий стіл 1 (за замовчуванням — і на ньому немає вікон).
// Без постійних опитувань: поки дашборд схований — жодних таймерів, лише події Hyprland (socket2).
// Поки видно — один таймер раз на секунду, кожен віджет оновлюється зі своїм кроком.
//
// Конфіг: ~/.config/megumin-os/dashboard.json  (редагує Налаштування → Дашборд)
//   { "enabled": true, "columns": 3, "position": "center|left|right", "only_empty": true,
//     "widgets": [ { "type": "clock", "seconds": false, "date": true }, ... ] }
// Перечитати конфіг: pkill -USR1 -x dashboard. Вимкнено → процес виходить.
// Прев'ю звичайним вікном (для знімка): dashboard --window
#include "sysui_common.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <atomic>
#include <ctime>
#include <map>

using json = nlohmann::json;

static GtkWidget* g_win = nullptr;
static GtkWidget* g_root = nullptr;
static bool g_preview = false;
static json g_cfg;
static long long g_cfg_mtime = -1;
static int g_gen = 0;                       // покоління віджетів: async-відповіді старих поколінь ігноруються
static guint g_tick = 0;
static guint64 g_sec = 0;
static std::vector<GtkWidget*> g_cards;
static std::vector<std::function<void(guint64)>> g_updaters;
static bool g_light = false;
static std::string g_monitor;

static std::string cfg_path() { return sysui::home() + "/.config/megumin-os/dashboard.json"; }
static std::string notes_path() { return sysui::home() + "/.config/megumin-os/dashboard_notes.txt"; }

static json default_cfg() {
    return json{{"enabled", true}, {"columns", 3}, {"position", "center"}, {"only_empty", false},
                {"widgets", json::array({
                    {{"type", "clock"}, {"seconds", false}, {"date", true}},
                    {{"type", "system"}, {"cpu", true}, {"ram", true}, {"disk", true}, {"battery", true}, {"temp", false}},
                    {{"type", "weather"}, {"city", ""}},
                    {{"type", "music"}},
                    {{"type", "network"}},
                    {{"type", "actions"}, {"items", json::array({"terminal", "files", "browser", "settings", "screenshot", "lock"})}},
                    {{"type", "notes"}},
                    {{"type", "calendar"}},
                    {{"type", "perf"}},
                    {{"type", "games"}, {"count", 4}},
                    {{"type", "alerts"}, {"region", ""}},
                    {{"type", "rates"}},
                    {{"type", "updates"}},
                })}};
}

static void load_cfg() {
    g_cfg_mtime = sysui::file_mtime_ns(cfg_path());
    json j;
    try { std::ifstream f(cfg_path()); if (f.good()) f >> j; } catch (...) { j = json(); }
    if (!j.is_object()) {
        j = default_cfg();
        if (!g_preview && g_cfg_mtime == 0) {   // першого разу — записуємо типовий конфіг, щоб Налаштування його бачили
            std::ofstream o(cfg_path()); o << j.dump(2);
            g_cfg_mtime = sysui::file_mtime_ns(cfg_path());
        }
    }
    if (!j.contains("widgets") || !j["widgets"].is_array()) j["widgets"] = default_cfg()["widgets"];
    g_cfg = j;
}
static bool cfg_enabled() { return g_cfg.value("enabled", true); }

// ---------- дрібниці ----------
static std::string read_file(const std::string& p) {
    std::ifstream f(p); if (!f.good()) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string trim_s(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}
static std::string fmt(const char* f, double v) { char b[64]; snprintf(b, sizeof b, f, v); return b; }
static std::string human_bytes(double b) {
    const char* u[] = {"Б", "КБ", "МБ", "ГБ", "ТБ"}; int i = 0;
    while (b >= 1024 && i < 4) { b /= 1024; i++; }
    return fmt(b < 10 && i ? "%.1f" : "%.0f", b) + " " + u[i];
}
static bool opt(const json& o, const char* k, bool d) { return o.contains(k) && o[k].is_boolean() ? o[k].get<bool>() : d; }
static std::string opts(const json& o, const char* k, const std::string& d) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : d; }

static GtkWidget* label(const std::string& t, const char* cls = nullptr, float xalign = 0) {
    GtkWidget* l = gtk_label_new(t.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    if (cls) gtk_widget_add_css_class(l, cls);
    return l;
}
static void set_text(GtkWidget* l, const std::string& t) {
    if (strcmp(gtk_label_get_text(GTK_LABEL(l)), t.c_str()) != 0) gtk_label_set_text(GTK_LABEL(l), t.c_str());
}
static void spawn(const std::string& cmd) {
    gchar* argv[] = {(gchar*)"bash", (gchar*)"-c", (gchar*)cmd.c_str(), nullptr};
    g_spawn_async(nullptr, argv, nullptr, (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                  nullptr, nullptr, nullptr, nullptr);
}

// Асинхронна команда без потоків: результат приходить у головний цикл. cb не викликається, якщо віджети перебудовано.
struct AsyncCb { int gen; std::function<void(const std::string&)> cb; };
static void run_async(const std::vector<std::string>& args, std::function<void(const std::string&)> cb) {
    std::vector<const gchar*> av; for (auto& a : args) av.push_back(a.c_str()); av.push_back(nullptr);
    GError* e = nullptr;
    GSubprocess* p = g_subprocess_newv(av.data(), (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), &e);
    if (!p) { if (e) g_error_free(e); cb(""); return; }
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, [](GObject* src, GAsyncResult* res, gpointer d) {
        auto* a = (AsyncCb*)d;
        gchar* out = nullptr;
        g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, nullptr, nullptr);
        if (a->gen == g_gen) a->cb(out ? out : "");
        g_free(out); g_object_unref(src); delete a;
    }, new AsyncCb{g_gen, std::move(cb)});
}

// ---------- картка ----------
static GtkWidget* card(const json& o, const std::string& def_title, const char* glyph) {
    GtkWidget* c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(c, "dash-card");
    gtk_widget_set_size_request(c, 300, -1);
    std::string t = opts(o, "title", def_title);
    if (!t.empty() && opt(o, "show_title", true)) {
        GtkWidget* h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(h), label(glyph, "dash-hglyph"));
        gtk_box_append(GTK_BOX(h), label(t, "dash-title"));
        gtk_box_append(GTK_BOX(c), h);
    }
    g_cards.push_back(c);
    return c;
}
static GtkWidget* bar_row(GtkWidget* parent, const std::string& name, GtkWidget** val, GtkWidget** bar) {
    GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* n = label(name, "dash-key"); gtk_widget_set_hexpand(n, TRUE);
    *val = label("…", "dash-val", 1);
    gtk_box_append(GTK_BOX(h), n); gtk_box_append(GTK_BOX(h), *val);
    *bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(*bar, "dash-bar");
    gtk_box_append(GTK_BOX(v), h); gtk_box_append(GTK_BOX(v), *bar);
    gtk_box_append(GTK_BOX(parent), v);
    return v;
}

// ---------- годинник ----------
static const char* UA_DAYS[] = {"неділя", "понеділок", "вівторок", "середа", "четвер", "пʼятниця", "субота"};
static const char* UA_MON[] = {"січня", "лютого", "березня", "квітня", "травня", "червня", "липня", "серпня", "вересня", "жовтня", "листопада", "грудня"};
static void w_clock(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "", "");
    gtk_widget_add_css_class(c, "dash-clock");
    GtkWidget* t = label("", "dash-time");
    GtkWidget* d = label("", "dash-date");
    gtk_box_append(GTK_BOX(c), t);
    if (opt(o, "date", true)) gtk_box_append(GTK_BOX(c), d);
    bool secs = opt(o, "seconds", false);
    g_updaters.push_back([t, d, secs](guint64) {   // set_text не чіпає GTK, якщо текст не змінився
        time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
        char b[32]; strftime(b, sizeof b, secs ? "%H:%M:%S" : "%H:%M", &tm);
        set_text(t, b);
        set_text(d, std::string(UA_DAYS[tm.tm_wday]) + ", " + std::to_string(tm.tm_mday) + " " + UA_MON[tm.tm_mon]);
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- система ----------
static void w_system(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Система", "\U000F035B");
    struct S { GtkWidget *cv = 0, *cb = 0, *rv = 0, *rb = 0, *dv = 0, *db = 0, *bv = 0, *bb = 0, *tv = 0, *tb = 0;
               unsigned long long pi = 0, pt = 0; std::string disk = "/", bat; };
    auto* s = new S;
    s->disk = opts(o, "disk_path", "/");
    if (opt(o, "cpu", true)) bar_row(c, "Процесор", &s->cv, &s->cb);
    if (opt(o, "temp", false)) bar_row(c, "Температура", &s->tv, &s->tb);
    if (opt(o, "ram", true)) bar_row(c, "Памʼять", &s->rv, &s->rb);
    if (opt(o, "disk", true)) bar_row(c, "Диск " + s->disk, &s->dv, &s->db);
    if (opt(o, "battery", true)) {
        if (GDir* d = g_dir_open("/sys/class/power_supply", 0, nullptr)) {
            while (const char* n = g_dir_read_name(d)) if (!strncmp(n, "BAT", 3)) { s->bat = std::string("/sys/class/power_supply/") + n; break; }
            g_dir_close(d);
        }
        if (!s->bat.empty()) bar_row(c, "Батарея", &s->bv, &s->bb);
    }
    g_object_set_data_full(G_OBJECT(c), "st", s, [](gpointer p) { delete (S*)p; });
    g_updaters.push_back([s](guint64 sec) {
        if (sec % 2) return;
        if (s->cv) {
            std::istringstream f(read_file("/proc/stat"));
            std::string cpu; unsigned long long v[8] = {0}; f >> cpu; for (auto& x : v) f >> x;
            unsigned long long idle = v[3] + v[4], tot = 0; for (auto x : v) tot += x;
            if (s->pt) {
                double u = tot > s->pt ? 1.0 - double(idle - s->pi) / double(tot - s->pt) : 0;
                u = std::clamp(u, 0.0, 1.0);
                set_text(s->cv, fmt("%.0f%%", u * 100));
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->cb), u);
            }
            s->pi = idle; s->pt = tot;
        }
        if (s->tv) {
            double best = 0;
            for (int i = 0; i < 12; i++) {
                std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(i);
                std::string ty = trim_s(read_file(base + "/type"));
                if (ty.empty()) break;
                if (ty.find("x86_pkg") != std::string::npos || ty.find("cpu") != std::string::npos || ty.find("k10") != std::string::npos || best == 0) {
                    double t = atof(read_file(base + "/temp").c_str()) / 1000.0;
                    if (ty.find("x86_pkg") != std::string::npos || ty.find("cpu") != std::string::npos) { best = t; break; }
                    if (best == 0) best = t;
                }
            }
            set_text(s->tv, fmt("%.0f°C", best));
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->tb), std::clamp(best / 100.0, 0.0, 1.0));
        }
        if (s->rv && sec % 4 == 0) {
            std::istringstream f(read_file("/proc/meminfo"));
            std::string k; double v, tot = 0, av = 0; std::string unit;
            while (f >> k >> v >> unit) { if (k == "MemTotal:") tot = v; else if (k == "MemAvailable:") av = v; }
            if (tot > 0) {
                set_text(s->rv, human_bytes((tot - av) * 1024) + " / " + human_bytes(tot * 1024));
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->rb), (tot - av) / tot);
            }
        }
        if (s->dv && sec % 30 == 0) {
            struct statvfs st;
            if (statvfs(s->disk.c_str(), &st) == 0 && st.f_blocks) {
                double tot = (double)st.f_blocks * st.f_frsize, fr = (double)st.f_bavail * st.f_frsize;
                set_text(s->dv, "вільно " + human_bytes(fr) + " з " + human_bytes(tot));
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->db), 1.0 - fr / tot);
            }
        }
        if (s->bv && sec % 10 == 0) {
            int cap = atoi(read_file(s->bat + "/capacity").c_str());
            std::string stt = trim_s(read_file(s->bat + "/status"));
            std::string ua = stt == "Charging" ? " · заряджається" : stt == "Full" ? " · повна" : stt == "Not charging" ? " · не заряджається" : "";
            set_text(s->bv, std::to_string(cap) + "%" + ua);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->bb), cap / 100.0);
        }
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- мережа ----------
static void w_network(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Мережа", "\U000F05A9");
    struct N { GtkWidget *name, *ip, *speed; std::string iface; unsigned long long rx = 0, tx = 0; gint64 t = 0; bool busy = false; };
    auto* n = new N;
    n->name = label("…", "dash-big2"); n->ip = label("", "dash-sub"); n->speed = label("", "dash-sub");
    gtk_box_append(GTK_BOX(c), n->name); gtk_box_append(GTK_BOX(c), n->ip); gtk_box_append(GTK_BOX(c), n->speed);
    g_object_set_data_full(G_OBJECT(c), "st", n, [](gpointer p) { delete (N*)p; });
    g_updaters.push_back([n](guint64 sec) {
        if (sec % 3) return;
        // інтерфейс за замовчуванням
        std::string iface;
        { std::istringstream f(read_file("/proc/net/route")); std::string line; std::getline(f, line);
          while (std::getline(f, line)) { std::istringstream l(line); std::string i, dst; l >> i >> dst; if (dst == "00000000") { iface = i; break; } } }
        if (iface.empty()) { set_text(n->name, "Немає підключення"); set_text(n->ip, ""); set_text(n->speed, ""); n->iface.clear(); return; }
        if (iface != n->iface) { n->iface = iface; n->rx = n->tx = 0; set_text(n->name, iface); }
        std::string base = "/sys/class/net/" + iface;
        unsigned long long rx = strtoull(read_file(base + "/statistics/rx_bytes").c_str(), nullptr, 10);
        unsigned long long tx = strtoull(read_file(base + "/statistics/tx_bytes").c_str(), nullptr, 10);
        gint64 now = g_get_monotonic_time();
        if (n->t && n->rx) {
            double dt = (now - n->t) / 1e6;
            if (dt > 0) set_text(n->speed, "↓ " + human_bytes((rx - n->rx) / dt) + "/с   ↑ " + human_bytes((tx - n->tx) / dt) + "/с");
        }
        n->rx = rx; n->tx = tx; n->t = now;
        std::string ip;
        struct ifaddrs* ifa = nullptr;
        if (getifaddrs(&ifa) == 0) {
            for (auto* p = ifa; p; p = p->ifa_next)
                if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && iface == p->ifa_name) {
                    char b[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &((sockaddr_in*)p->ifa_addr)->sin_addr, b, sizeof b); ip = b; break;
                }
            freeifaddrs(ifa);
        }
        bool wifi = g_file_test((base + "/wireless").c_str(), G_FILE_TEST_EXISTS);
        set_text(n->ip, std::string(wifi ? "Wi-Fi · " : "Дріт · ") + iface + (ip.empty() ? "" : " · " + ip));
        if (sec % 15 == 0 && !n->busy) {
            n->busy = true;
            run_async({"nmcli", "-t", "-g", "GENERAL.CONNECTION", "device", "show", iface}, [n, iface](const std::string& out) {
                n->busy = false;
                std::string s = trim_s(out);
                set_text(n->name, s.empty() ? iface : s);
            });
        }
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- погода (wttr.in, без ключа; кеш на диску, щоб перезапуск не смикав мережу) ----------
static const char* weather_glyph(int code) {
    switch (code) {
        case 113: return "\U000F0599";
        case 116: return "\U000F0595";
        case 119: case 122: return "\U000F0590";
        case 143: case 248: case 260: return "\U000F0591";
        case 200: case 386: case 389: case 392: case 395: return "\U000F0593";
        case 179: case 182: case 185: case 227: case 230: case 281: case 284: case 311: case 314: case 317: case 320: case 323: case 326:
        case 329: case 332: case 335: case 338: case 350: case 362: case 365: case 368: case 371: case 374: case 377: return "\U000F0598";
        default: return "\U000F0597";
    }
}
static const char* weather_ua(int c) {   // wttr.in перекладає lang_uk не завжди — свій опис за кодом
    switch (c) {
        case 113: return "Ясно"; case 116: return "Мінлива хмарність"; case 119: return "Хмарно"; case 122: return "Похмуро";
        case 143: case 248: case 260: return "Туман";
        case 176: case 263: case 266: case 293: case 296: case 353: return "Невеликий дощ";
        case 299: case 302: case 305: case 308: case 356: case 359: return "Дощ";
        case 179: case 323: case 326: case 368: return "Невеликий сніг";
        case 227: case 230: return "Хуртовина";
        case 329: case 332: case 335: case 338: case 371: return "Сніг";
        case 182: case 185: case 281: case 284: case 311: case 314: case 317: case 320: case 350: case 362: case 365: case 374: case 377: return "Мокрий сніг";
        case 200: case 386: case 389: case 392: case 395: return "Гроза";
        default: return "";
    }
}
static std::string url_enc(const std::string& s) {
    std::string r; char b[4];
    for (unsigned char ch : s) { if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') r += ch; else if (ch == ' ') r += '+'; else { snprintf(b, sizeof b, "%%%02X", ch); r += b; } }
    return r;
}
static void w_weather(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Погода", "\U000F0590");
    struct Wt { GtkWidget *icon, *temp, *desc, *more, *days; std::string city; int mins; bool busy = false; };
    auto* w = new Wt;
    w->city = trim_s(opts(o, "city", ""));
    if (w->city.empty()) w->city = trim_s(sysui::settings().location);   // загальне місцезнаходження з Налаштувань
    w->mins = std::clamp(o.value("interval_min", 30), 10, 240);
    GtkWidget* h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    w->icon = label("\U000F0590", "dash-wicon"); w->temp = label("…", "dash-big");
    GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2); gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    w->desc = label("Завантаження…", "dash-key"); w->more = label("", "dash-sub");
    gtk_label_set_wrap(GTK_LABEL(w->more), TRUE); gtk_label_set_wrap(GTK_LABEL(w->desc), TRUE);
    gtk_box_append(GTK_BOX(v), w->desc); gtk_box_append(GTK_BOX(v), w->more);
    gtk_box_append(GTK_BOX(h), w->icon); gtk_box_append(GTK_BOX(h), w->temp); gtk_box_append(GTK_BOX(h), v);
    w->days = label("", "dash-sub");
    gtk_box_append(GTK_BOX(c), h); gtk_box_append(GTK_BOX(c), w->days);
    g_object_set_data_full(G_OBJECT(c), "st", w, [](gpointer p) { delete (Wt*)p; });
    std::string cache = sysui::home() + "/.cache/mgos/dashboard_weather.json";
    auto show = [w](const json& d) {
        try {
            auto& cc = d["current_condition"][0];
            int code = atoi(cc.value("weatherCode", "0").c_str());
            set_text(w->icon, weather_glyph(code));
            std::string t = cc.value("temp_C", "?");
            set_text(w->temp, (t.size() && t[0] != '-' && t != "0" ? "+" : "") + t + "°");
            std::string desc = weather_ua(code);
            if (desc.empty() && cc.contains("lang_uk")) desc = cc["lang_uk"][0].value("value", "");
            if (desc.empty()) desc = cc["weatherDesc"][0].value("value", "");
            std::string area;
            try { area = d["nearest_area"][0]["areaName"][0].value("value", ""); } catch (...) {}
            set_text(w->desc, trim_s(desc) + (area.empty() ? "" : " · " + area));
            set_text(w->more, "відчувається " + cc.value("FeelsLikeC", "?") + "° · вологість " + cc.value("humidity", "?") +
                              "% · вітер " + cc.value("windspeedKmph", "?") + " км/год");
            std::string days; const char* nm[] = {"Сьогодні", "Завтра", "Післязавтра"};
            for (int i = 0; i < 3 && i < (int)d["weather"].size(); i++)
                days += std::string(i ? "    " : "") + nm[i] + " " + d["weather"][i].value("mintempC", "?") + "…" + d["weather"][i].value("maxtempC", "?") + "°";
            set_text(w->days, days);
        } catch (...) { set_text(w->desc, "Немає даних"); }
    };
    g_updaters.push_back([w, cache, show](guint64 sec) {
        if (sec % 60 != 0 && sec != 1) return;
        json cj; try { std::ifstream f(cache); if (f.good()) f >> cj; } catch (...) {}
        long long now = time(nullptr);
        bool fresh = cj.is_object() && cj.value("city", "") == w->city && now - cj.value("ts", 0LL) < w->mins * 60;
        if (cj.is_object() && cj.value("city", "") == w->city && cj.contains("data")) show(cj["data"]);
        if (fresh || w->busy) return;
        w->busy = true;
        std::string url = "https://wttr.in/" + url_enc(w->city) + "?format=j1&lang=uk";
        run_async({"curl", "-sf", "-m", "12", "-A", "curl", url}, [w, cache, show](const std::string& out) {
            w->busy = false;
            json d; try { d = json::parse(out); } catch (...) { if (!gtk_label_get_text(GTK_LABEL(w->temp))[1]) set_text(w->desc, "Немає звʼязку з wttr.in"); return; }
            if (!d.contains("current_condition")) return;
            json cj{{"ts", (long long)time(nullptr)}, {"city", w->city}, {"data", d}};
            g_mkdir_with_parents((sysui::home() + "/.cache/mgos").c_str(), 0755);
            { std::ofstream o(cache); o << cj.dump(); }
            show(d);
        });
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- нотатки ----------
static guint g_note_save = 0;
static void w_notes(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Нотатки", "\U000F082E");
    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sc, -1, std::clamp(o.value("height", 130), 60, 500));
    GtkWidget* tv = gtk_text_view_new();
    gtk_widget_add_css_class(tv, "dash-notes");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    std::string txt = read_file(notes_path());
    gtk_text_buffer_set_text(b, txt.c_str(), -1);
    if (!g_preview)
        g_signal_connect(b, "changed", G_CALLBACK(+[](GtkTextBuffer* b, gpointer) {
            if (g_note_save) g_source_remove(g_note_save);
            g_object_ref(b);
            g_note_save = g_timeout_add(700, [](gpointer d) -> gboolean {
                g_note_save = 0;
                GtkTextBuffer* b = GTK_TEXT_BUFFER(d);
                GtkTextIter s, e; gtk_text_buffer_get_bounds(b, &s, &e);
                gchar* t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
                std::string tmp = notes_path() + ".tmp";
                { std::ofstream f(tmp); f << t; }
                rename(tmp.c_str(), notes_path().c_str());
                g_free(t); g_object_unref(b);
                return G_SOURCE_REMOVE;
            }, b);
        }), nullptr);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);
    gtk_box_append(GTK_BOX(c), sc);
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- швидкі дії ----------
struct Act { const char *glyph, *label, *cmd; };
static const std::map<std::string, Act> ACTS = {
    {"terminal",   {"\U000F018D", "Термінал", "kitty"}},
    {"files",      {"\U000F024B", "Файли", "xdg-open \"$HOME\""}},
    {"browser",    {"\U000F059F", "Браузер", "gtk-launch \"$(xdg-settings get default-web-browser)\""}},
    {"settings",   {"\U000F0493", "Налаштування", "gtk-launch megumin-settings"}},
    {"screenshot", {"\U000F0100", "Знімок", "grim -g \"$(slurp)\" - | wl-copy"}},
    {"lock",       {"\U000F033E", "Блокування", "loginctl lock-session"}},
    {"wallpapers", {"\U000F02E9", "Шпалери", "gtk-launch megumin-settings --page wallpapers || python3 ~/.config/ignis/settings.py --page wallpapers"}},
    {"music",      {"\U000F075A", "Музика", "playerctl play-pause"}},
    {"monitor",    {"\U000F0128", "Монітор", "kitty -e btop"}},
};
static void w_actions(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Швидкі дії", "\U000F0241");
    GtkWidget* g = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 8); gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(g), TRUE);
    json items = o.contains("items") && o["items"].is_array() ? o["items"] : default_cfg()["widgets"][5]["items"];
    int i = 0;
    for (auto& it : items) {
        std::string gl, lb, cmd;
        if (it.is_string()) { auto f = ACTS.find(it.get<std::string>()); if (f == ACTS.end()) continue; gl = f->second.glyph; lb = f->second.label; cmd = f->second.cmd; }
        else if (it.is_object()) { gl = opts(it, "icon", "\U000F0241"); lb = opts(it, "label", "Дія"); cmd = opts(it, "cmd", ""); if (cmd.empty()) continue; }
        else continue;
        GtkWidget* b = gtk_button_new();
        gtk_widget_add_css_class(b, "dash-act"); gtk_widget_add_css_class(b, "live");
        GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        gtk_box_append(GTK_BOX(v), label(gl, "dash-aglyph", 0.5));
        GtkWidget* l = label(lb, "dash-alabel", 0.5); gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(v), l);
        gtk_button_set_child(GTK_BUTTON(b), v);
        gtk_widget_set_tooltip_text(b, lb.c_str());
        if (!g_preview)
            g_signal_connect_data(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { spawn((const char*)d); }), g_strdup(cmd.c_str()),
                                  [](gpointer d, GClosure*) { g_free(d); }, (GConnectFlags)0);
        gtk_grid_attach(GTK_GRID(g), b, i % 3, i / 3, 1, 1);
        i++;
    }
    gtk_box_append(GTK_BOX(c), g);
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- календар ----------
static void w_calendar(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Календар", "\U000F00ED");
    GtkWidget* cal = gtk_calendar_new();
    gtk_widget_add_css_class(cal, "dash-cal");
    gtk_box_append(GTK_BOX(c), cal);
    // після півночі — на сьогодні
    g_updaters.push_back([cal](guint64 sec) {
        if (sec % 60) return;
        static int last = -1;
        time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
        if (tm.tm_yday != last) {
            last = tm.tm_yday;
            GDateTime* d = g_date_time_new_now_local();
            gtk_calendar_select_day(GTK_CALENDAR(cal), d);
            g_date_time_unref(d);
        }
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- спарклайн (історія з ~/.cache/mgos/sensors.csv, пише ~/.local/bin/mgos-sensors) ----------
struct Spark { std::vector<std::pair<double, double>> a, b; double ra = 0.04, ga = 0.52, ba = 1.0; };
static void spark_draw(GtkDrawingArea*, cairo_t* cr, int W, int H, gpointer d) {
    auto* s = (Spark*)d;
    auto line = [&](const std::vector<std::pair<double, double>>& v, double r, double g, double b, bool fill, double lo, double hi) {
        if (v.size() < 2) return;
        double t0 = v.front().first, t1 = v.back().first;
        auto X = [&](double t) { return (t - t0) / std::max(1.0, t1 - t0) * (W - 2) + 1; };
        auto Y = [&](double y) { return H - 2 - (y - lo) / std::max(1.0, hi - lo) * (H - 6); };
        cairo_new_path(cr);
        cairo_move_to(cr, X(v[0].first), Y(v[0].second));
        for (size_t i = 1; i < v.size(); i++) {
            double mx = (X(v[i - 1].first) + X(v[i].first)) / 2;
            cairo_curve_to(cr, mx, Y(v[i - 1].second), mx, Y(v[i].second), X(v[i].first), Y(v[i].second));
        }
        if (fill) {
            cairo_path_t* p = cairo_copy_path(cr);
            cairo_line_to(cr, X(v.back().first), H); cairo_line_to(cr, X(v[0].first), H); cairo_close_path(cr);
            cairo_pattern_t* gr = cairo_pattern_create_linear(0, 0, 0, H);
            cairo_pattern_add_color_stop_rgba(gr, 0, r, g, b, 0.35); cairo_pattern_add_color_stop_rgba(gr, 1, r, g, b, 0);
            cairo_set_source(cr, gr); cairo_fill(cr); cairo_pattern_destroy(gr);
            cairo_new_path(cr); cairo_append_path(cr, p); cairo_path_destroy(p);
        }
        cairo_set_source_rgba(cr, r, g, b, 0.95); cairo_set_line_width(cr, 1.8);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND); cairo_stroke(cr);
    };
    double lo = 1e9, hi = -1e9;
    for (auto* v : {&s->a, &s->b}) for (auto& p : *v) { lo = std::min(lo, p.second); hi = std::max(hi, p.second); }
    if (hi < lo) return;
    if (hi - lo < 12) { double m = (hi + lo) / 2; lo = m - 6; hi = m + 6; }
    line(s->b, 0.19, 0.82, 0.35, false, lo, hi);           // GPU — зелена
    line(s->a, s->ra, s->ga, s->ba, true, lo, hi);         // CPU — акцент
}
// останні ~N рядків sensors.csv → (ts, cpu), (ts, gpu)
static void sensors_tail(Spark* s, int col_a, int col_b, double since) {
    std::ifstream f(sysui::home() + "/.cache/mgos/sensors.csv", std::ios::binary);
    if (!f) return;
    f.seekg(0, std::ios::end); long long sz = f.tellg();
    long long from = std::max(0LL, sz - 24000);
    f.seekg(from);
    std::string line; if (from) std::getline(f, line);
    s->a.clear(); s->b.clear();
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream l(line); std::vector<std::string> v; std::string x;
        while (l >> x) v.push_back(x);
        if ((int)v.size() <= std::max(col_a, col_b)) continue;
        double ts = atof(v[0].c_str()); if (ts < since) continue;
        if (v[col_a] != "-") s->a.push_back({ts, atof(v[col_a].c_str())});
        if (col_b > 0 && v[col_b] != "-") s->b.push_back({ts, atof(v[col_b].c_str())});
    }
    // усереднення по ~90 кошиках: сирі точки раз на 15 с дають «пилку» з коливань на 2–3°
    auto smooth = [&](std::vector<std::pair<double, double>>& v) {
        if (v.size() < 4) return;
        double t0 = v.front().first, step = std::max(1.0, (v.back().first - t0) / 90);
        std::vector<std::pair<double, double>> o; double st = 0, sv = 0; int n = 0, cur = 0;
        for (auto& p : v) {
            int b = (int)((p.first - t0) / step);
            if (b != cur && n) { o.push_back({st / n, sv / n}); st = sv = 0; n = 0; }
            cur = b; st += p.first; sv += p.second; n++;
        }
        if (n) o.push_back({st / n, sv / n});
        v.swap(o);
    };
    smooth(s->a); smooth(s->b);
}
static std::string hwmon_read(const char* name, const char* file) {   // перший hwmon з такою назвою
    if (GDir* d = g_dir_open("/sys/class/hwmon", 0, nullptr)) {
        while (const char* n = g_dir_read_name(d)) {
            std::string b = std::string("/sys/class/hwmon/") + n;
            if (trim_s(read_file(b + "/name")) == name) { g_dir_close(d); return trim_s(read_file(b + "/" + file)); }
        }
        g_dir_close(d);
    }
    return "";
}
static int cpu_temp_now() {
    for (const char* n : {"coretemp", "k10temp", "zenpower"}) { std::string v = hwmon_read(n, "temp1_input"); if (!v.empty()) return atoi(v.c_str()) / 1000; }
    return 0;
}
static int cpu_fan_now() {
    for (const char* n : {"nct6687", "nct6683", "nct6775", "nct6798", "it87", "acpi_fan"}) { std::string v = hwmon_read(n, "fan1_input"); if (!v.empty()) return atoi(v.c_str()); }
    return 0;
}
static const char* temp_cls(int t) { return t >= 80 ? "dash-hot" : t >= 65 ? "dash-warm" : "dash-cool"; }
static void set_cls(GtkWidget* w, const char* cls) {
    for (const char* c : {"dash-hot", "dash-warm", "dash-cool"}) if (strcmp(c, cls)) gtk_widget_remove_css_class(w, c);
    gtk_widget_add_css_class(w, cls);
}

// ---------- продуктивність: CPU / GPU / памʼять+zram / кулер + графік температур ----------
static void w_perf(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Продуктивність", "\U000F04C5");
    struct P { GtkWidget *cpu_t, *cpu_s, *gpu_t, *gpu_s, *ram_t, *ram_s, *fan_t, *fan_s, *rb, *spark;
               Spark sp; unsigned long long pi = 0, pt = 0; bool busy = false; bool has_gpu = false; };
    auto* p = new P;
    p->has_gpu = g_find_program_in_path("nvidia-smi") != nullptr;
    GtkWidget* g = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 8); gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(g), TRUE);
    auto tile = [&](int i, const char* name, GtkWidget** big, GtkWidget** sub) {
        GtkWidget* t = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_add_css_class(t, "dash-tile");
        gtk_box_append(GTK_BOX(t), label(name, "dash-tname"));
        *big = label("…", "dash-tbig"); *sub = label("", "dash-tsub");
        gtk_label_set_ellipsize(GTK_LABEL(*sub), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(t), *big); gtk_box_append(GTK_BOX(t), *sub);
        gtk_grid_attach(GTK_GRID(g), t, i % 2, i / 2, 1, 1);
    };
    tile(0, "ПРОЦЕСОР", &p->cpu_t, &p->cpu_s);
    tile(1, "ВІДЕОКАРТА", &p->gpu_t, &p->gpu_s);
    tile(2, "ПАМʼЯТЬ", &p->ram_t, &p->ram_s);
    tile(3, "CPU-КУЛЕР", &p->fan_t, &p->fan_s);
    gtk_box_append(GTK_BOX(c), g);
    p->rb = gtk_progress_bar_new(); gtk_widget_add_css_class(p->rb, "dash-bar");
    gtk_box_append(GTK_BOX(c), p->rb);
    GtkWidget* sh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* sl = label("Температури за годину", "dash-sub"); gtk_widget_set_hexpand(sl, TRUE);
    gtk_box_append(GTK_BOX(sh), sl);
    gtk_box_append(GTK_BOX(sh), label("● CPU", "dash-legend-a"));
    if (p->has_gpu) gtk_box_append(GTK_BOX(sh), label("● GPU", "dash-legend-b"));
    gtk_box_append(GTK_BOX(c), sh);
    p->spark = gtk_drawing_area_new();
    gtk_widget_set_size_request(p->spark, -1, 54);
    sysui::accent();
    { std::string A = sysui::accent(); unsigned v = 0; sscanf(A.c_str() + 1, "%06x", &v);
      p->sp.ra = ((v >> 16) & 255) / 255.0; p->sp.ga = ((v >> 8) & 255) / 255.0; p->sp.ba = (v & 255) / 255.0; }
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->spark), spark_draw, &p->sp, nullptr);
    gtk_box_append(GTK_BOX(c), p->spark);
    if (!p->has_gpu) { set_text(p->gpu_t, "—"); set_text(p->gpu_s, "немає даних"); }
    g_object_set_data_full(G_OBJECT(c), "st", p, [](gpointer x) { delete (P*)x; });
    g_updaters.push_back([p](guint64 sec) {
        if (sec % 2 == 0) {   // CPU: навантаження + температура
            std::istringstream f(read_file("/proc/stat"));
            std::string cpu; unsigned long long v[8] = {0}; f >> cpu; for (auto& x : v) f >> x;
            unsigned long long idle = v[3] + v[4], tot = 0; for (auto x : v) tot += x;
            int t = cpu_temp_now();
            set_text(p->cpu_t, t ? std::to_string(t) + "°" : "—"); set_cls(p->cpu_t, temp_cls(t));
            if (p->pt && tot > p->pt) set_text(p->cpu_s, fmt("навантаження %.0f%%", std::clamp(1.0 - double(idle - p->pi) / double(tot - p->pt), 0.0, 1.0) * 100));
            p->pi = idle; p->pt = tot;
            int rpm = cpu_fan_now();
            set_text(p->fan_t, rpm ? std::to_string(rpm) : "стоїть"); set_text(p->fan_s, rpm ? "об/хв" : "");
        }
        if (sec % 4 == 0) {   // памʼять + zram
            std::istringstream f(read_file("/proc/meminfo"));
            std::string k, unit; double v, tot = 0, av = 0;
            while (f >> k >> v >> unit) { if (k == "MemTotal:") tot = v; else if (k == "MemAvailable:") av = v; }
            if (tot > 0) {
                set_text(p->ram_t, fmt("%.0f%%", (tot - av) / tot * 100));
                std::istringstream z(read_file("/sys/block/zram0/mm_stat")); double orig = 0, compr = 0; z >> orig >> compr;
                set_text(p->ram_s, human_bytes((tot - av) * 1024) + " з " + human_bytes(tot * 1024) + (orig > 0 ? " · zram " + human_bytes(orig) : ""));
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->rb), (tot - av) / tot);
            }
        }
        if (p->has_gpu && sec % 3 == 0 && !p->busy) {
            p->busy = true;
            run_async({"nvidia-smi", "--query-gpu=temperature.gpu,utilization.gpu,memory.used,memory.total,power.draw",
                       "--format=csv,noheader,nounits"}, [p](const std::string& out) {
                p->busy = false;
                // розбір без локалі: у uk_UA sscanf("%lf") чекає десяткову кому, а nvidia-smi пише крапку
                std::vector<double> f; { std::stringstream ss(out); std::string x;
                    while (std::getline(ss, x, ',')) f.push_back(g_ascii_strtod(trim_s(x).c_str(), nullptr)); }
                double t = f.size() > 0 ? f[0] : 0, u = f.size() > 1 ? f[1] : 0, mu = f.size() > 2 ? f[2] : 0,
                       mt = f.size() > 3 ? f[3] : 0, pw = f.size() > 4 ? f[4] : 0;
                if (f.size() >= 4) {
                    set_text(p->gpu_t, fmt("%.0f°", t)); set_cls(p->gpu_t, temp_cls((int)t));
                    set_text(p->gpu_s, fmt("%.0f%%", u) + " · " + fmt("%.1f", mu / 1024) + "/" + fmt("%.0f ГБ", mt / 1024) + (pw > 0 ? fmt(" · %.0f Вт", pw) : ""));
                }
            });
        }
        if (sec % 15 == 0) {   // графік з історії
            sensors_tail(&p->sp, 1, 2, time(nullptr) - 3600);
            gtk_widget_queue_draw(p->spark);
        }
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- обкладинка (локальний файл або завантаження з мережі в кеш) ----------
static void set_picture_from_url(GtkWidget* pic, const std::string& url, std::string* last) {
    if (url == *last) return;
    *last = url;
    if (url.empty()) { gtk_image_clear(GTK_IMAGE(pic)); gtk_widget_set_visible(pic, FALSE); return; }
    auto load = [pic](const std::string& path) {
        GdkTexture* t = gdk_texture_new_from_filename(path.c_str(), nullptr);
        if (t) gtk_image_set_from_paintable(GTK_IMAGE(pic), GDK_PAINTABLE(t)); else gtk_image_clear(GTK_IMAGE(pic));
        gtk_widget_set_visible(pic, t != nullptr);
        if (t) g_object_unref(t);
    };
    if (url.rfind("file://", 0) == 0) {
        gchar* fn = g_filename_from_uri(url.c_str(), nullptr, nullptr);
        if (fn) { load(fn); g_free(fn); }
        return;
    }
    if (url.rfind("http", 0) != 0) return;
    std::string dir = sysui::home() + "/.cache/mgos/dash_art";
    g_mkdir_with_parents(dir.c_str(), 0755);
    gchar* h = g_compute_checksum_for_string(G_CHECKSUM_SHA1, url.c_str(), -1);
    std::string path = dir + "/" + h + ".img"; g_free(h);
    if (g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) { load(path); return; }
    run_async({"curl", "-sf", "-m", "10", "-o", path, url}, [load, path, pic, last, url](const std::string&) {
        if (*last == url && g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) load(path);
    });
}

// ---------- музика (з обкладинкою і прогресом) ----------
static std::string mmss(double s) {
    int t = (int)s; char b[24];
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60); else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}
static void w_music(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Музика", "\U000F075A");
    struct M { GtkWidget *title, *artist, *pp, *art, *prog, *pos, *len; std::string art_url; bool busy = false; };
    auto* m = new M;
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    m->art = gtk_image_new();   // GtkImage з pixel_size: GtkPicture тягнув картку до розміру самої картинки
    gtk_image_set_pixel_size(GTK_IMAGE(m->art), 64); gtk_widget_add_css_class(m->art, "dash-art");
    gtk_widget_set_overflow(m->art, GTK_OVERFLOW_HIDDEN); gtk_widget_set_visible(m->art, FALSE);
    gtk_widget_set_valign(m->art, GTK_ALIGN_CENTER);
    GtkWidget* tv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2); gtk_widget_set_valign(tv, GTK_ALIGN_CENTER); gtk_widget_set_hexpand(tv, TRUE);
    m->title = label("Нічого не грає", "dash-big2"); m->artist = label("", "dash-sub");
    gtk_label_set_ellipsize(GTK_LABEL(m->title), PANGO_ELLIPSIZE_END); gtk_label_set_ellipsize(GTK_LABEL(m->artist), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(m->title), 20);
    gtk_box_append(GTK_BOX(tv), m->title); gtk_box_append(GTK_BOX(tv), m->artist);
    gtk_box_append(GTK_BOX(top), m->art); gtk_box_append(GTK_BOX(top), tv);
    m->prog = gtk_progress_bar_new(); gtk_widget_add_css_class(m->prog, "dash-bar");
    GtkWidget* times = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    m->pos = label("", "dash-tsub"); m->len = label("", "dash-tsub", 1); gtk_widget_set_hexpand(m->len, TRUE);
    gtk_box_append(GTK_BOX(times), m->pos); gtk_box_append(GTK_BOX(times), m->len);
    GtkWidget* btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
    auto mk = [&](const char* g, const char* cmd) {
        GtkWidget* b = gtk_button_new_with_label(g);
        gtk_widget_add_css_class(b, "dash-mbtn"); gtk_widget_add_css_class(b, "live");
        g_signal_connect_data(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { spawn((const char*)d); }), g_strdup(cmd),
                              [](gpointer d, GClosure*) { g_free(d); }, (GConnectFlags)0);
        gtk_box_append(GTK_BOX(btns), b);
        return b;
    };
    mk("\U000F04AE", "playerctl previous");
    m->pp = mk("\U000F040A", "playerctl play-pause");
    mk("\U000F04AD", "playerctl next");
    gtk_box_append(GTK_BOX(c), top); gtk_box_append(GTK_BOX(c), m->prog); gtk_box_append(GTK_BOX(c), times); gtk_box_append(GTK_BOX(c), btns);
    g_object_set_data_full(G_OBJECT(c), "st", m, [](gpointer p) { delete (M*)p; });
    g_updaters.push_back([m](guint64) {
        if (m->busy) return;
        m->busy = true;
        run_async({"playerctl", "metadata", "--format", "{{status}}\n{{xesam:title}}\n{{xesam:artist}}\n{{mpris:artUrl}}\n{{position}}\n{{mpris:length}}"},
                  [m](const std::string& out) {
            m->busy = false;
            std::istringstream f(out); std::string st, ti, ar, art, pos, len;
            std::getline(f, st); std::getline(f, ti); std::getline(f, ar); std::getline(f, art); std::getline(f, pos); std::getline(f, len);
            bool on = !st.empty();
            if (!on) { set_text(m->title, "Нічого не грає"); set_text(m->artist, ""); }
            else { set_text(m->title, ti.empty() ? "Без назви" : ti); set_text(m->artist, ar); }
            set_picture_from_url(m->art, on ? art : "", &m->art_url);
            double P = atof(pos.c_str()) / 1e6, L = atof(len.c_str()) / 1e6;
            bool hasp = on && L > 1;
            gtk_widget_set_visible(m->prog, hasp); gtk_widget_set_visible(gtk_widget_get_parent(m->pos), hasp);
            if (hasp) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m->prog), std::clamp(P / L, 0.0, 1.0));
                set_text(m->pos, mmss(P)); set_text(m->len, "−" + mmss(std::max(0.0, L - P)));
            }
            gtk_button_set_label(GTK_BUTTON(m->pp), st == "Playing" ? "\U000F03E4" : "\U000F040A");
        });
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- ігри: останні зіграні встановлені ігри Steam ----------
struct Game { std::string id, name; long long last = 0; };
static std::vector<Game> steam_recent(int n) {
    std::string root = sysui::home() + "/.local/share/Steam";
    // найсвіжіший акаунт
    std::string cfg; long long best = 0;
    if (GDir* d = g_dir_open((root + "/userdata").c_str(), 0, nullptr)) {
        while (const char* u = g_dir_read_name(d)) {
            std::string f = root + "/userdata/" + u + "/config/localconfig.vdf";
            long long m = sysui::file_mtime(f);
            if (m > best) { best = m; cfg = f; }
        }
        g_dir_close(d);
    }
    std::vector<std::string> libs = {root};
    { std::istringstream f(read_file(root + "/steamapps/libraryfolders.vdf")); std::string l;
      while (std::getline(f, l)) { auto p = l.find("\"path\""); if (p == std::string::npos) continue;
          auto a = l.find('"', p + 6); auto b = a == std::string::npos ? a : l.find('"', a + 1);
          if (b != std::string::npos) { std::string s = l.substr(a + 1, b - a - 1); if (s != root) libs.push_back(s); } } }
    std::vector<Game> out;
    std::string s = read_file(cfg);
    // «"<appid>" { … "LastPlayed" "<ts>" … }» — простий розбір без повного VDF
    std::map<std::string, long long> lastp;
    size_t pos = 0;
    while ((pos = s.find("\"LastPlayed\"", pos)) != std::string::npos) {
        size_t ob = s.rfind('{', pos), q2 = ob == std::string::npos ? ob : s.rfind('"', ob), q1 = q2 == std::string::npos || q2 == 0 ? std::string::npos : s.rfind('"', q2 - 1);
        size_t va = s.find('"', pos + 12), vb = va == std::string::npos ? va : s.find('"', va + 1);
        if (q1 != std::string::npos && vb != std::string::npos) {
            std::string id = s.substr(q1 + 1, q2 - q1 - 1);
            if (!id.empty() && std::all_of(id.begin(), id.end(), ::isdigit)) lastp[id] = std::max(lastp[id], atoll(s.substr(va + 1, vb - va - 1).c_str()));
        }
        pos += 12;
    }
    for (auto& [id, ts] : lastp) {
        for (auto& lib : libs) {
            std::string m = read_file(lib + "/steamapps/appmanifest_" + id + ".acf");
            if (m.empty()) continue;
            auto p = m.find("\"name\""); if (p == std::string::npos) break;
            auto a = m.find('"', p + 6), b = m.find('"', a + 1);
            std::string name = m.substr(a + 1, b - a - 1);
            if (name.find("Proton") == 0 || name.find("Steam Linux Runtime") == 0 || name.find("Steamworks") == 0) break;
            out.push_back({id, name, ts});
            break;
        }
    }
    std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return a.last > b.last; });
    if ((int)out.size() > n) out.resize(n);
    return out;
}
// обкладинка: <id>/header.jpg (старий Steam), <id>/<хеш>/library_header.jpg (новий), далі hero/capsule
static std::string steam_cover(const std::string& dir, const std::string& legacy) {
    for (const char* n : {"header.jpg", "library_header.jpg", "library_hero.jpg", "library_capsule.jpg"}) {
        std::string p = dir + "/" + n;
        if (g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) return p;
        if (GDir* d = g_dir_open(dir.c_str(), 0, nullptr)) {
            std::string hit;
            while (const char* e = g_dir_read_name(d)) {
                std::string q = dir + "/" + e + "/" + n;
                if (g_file_test(q.c_str(), G_FILE_TEST_EXISTS)) { hit = q; break; }
            }
            g_dir_close(d);
            if (!hit.empty()) return hit;
        }
    }
    return g_file_test(legacy.c_str(), G_FILE_TEST_EXISTS) ? legacy : "";
}
static std::string ago_ua(long long ts) {
    long long d = time(nullptr) - ts;
    if (d < 3600) return "щойно";
    if (d < 86400) return std::to_string(d / 3600) + " год тому";
    if (d < 2 * 86400) return "вчора";
    if (d < 30 * 86400) return std::to_string(d / 86400) + " дн тому";
    return std::to_string(d / (30 * 86400)) + " міс тому";
}
static void w_games(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Ігри", "\U000F0297");
    GtkWidget* g = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 8); gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(g), TRUE);
    int n = std::clamp(o.value("count", 4), 2, 8);
    auto games = steam_recent(n);
    std::string cache = sysui::home() + "/.local/share/Steam/appcache/librarycache/";
    int i = 0;
    for (auto& gm : games) {
        GtkWidget* b = gtk_button_new();
        gtk_widget_add_css_class(b, "dash-game"); gtk_widget_add_css_class(b, "live");
        GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget* pic = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
        gtk_widget_set_size_request(pic, -1, 62); gtk_widget_set_overflow(pic, GTK_OVERFLOW_HIDDEN);
        gtk_widget_add_css_class(pic, "dash-gpic");
        std::string cover = steam_cover(cache + gm.id, cache + gm.id + "_header.jpg");
        if (!cover.empty()) {   // зменшуємо при завантаженні: природний розмір картинки (460 px) роздував колонку
            if (GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(cover.c_str(), 240, -1, TRUE, nullptr)) {
                GdkTexture* t = gdk_texture_new_for_pixbuf(pb);
                gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(t));
                g_object_unref(t); g_object_unref(pb);
            }
        }
        gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
        GtkWidget* nm = label(gm.name, "dash-gname"); gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);
        GtkWidget* ag = label(ago_ua(gm.last), "dash-tsub");
        gtk_box_append(GTK_BOX(v), pic); gtk_box_append(GTK_BOX(v), nm); gtk_box_append(GTK_BOX(v), ag);
        gtk_button_set_child(GTK_BUTTON(b), v);
        gtk_widget_set_tooltip_text(b, ("Запустити " + gm.name).c_str());
        if (!g_preview)
            g_signal_connect_data(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { spawn((const char*)d); }),
                                  g_strdup(("steam steam://rungameid/" + gm.id).c_str()), [](gpointer d, GClosure*) { g_free(d); }, (GConnectFlags)0);
        gtk_grid_attach(GTK_GRID(g), b, i % 2, i / 2, 1, 1);
        i++;
    }
    if (games.empty()) gtk_box_append(GTK_BOX(c), label("Steam: поки немає зіграних встановлених ігор", "dash-sub"));
    else gtk_box_append(GTK_BOX(c), g);
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- повітряна тривога (ubilling.net.ua/aerialalerts — публічні дані, без ключа) ----------
static void w_alerts(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Повітряна тривога", "\U000F0026");
    struct A { GtkWidget *big, *sub, *list; std::string region; bool busy = false; };
    auto* a = new A;
    a->region = trim_s(opts(o, "region", ""));
    a->big = label("…", "dash-big2"); a->sub = label("", "dash-sub"); a->list = label("", "dash-sub");
    gtk_label_set_wrap(GTK_LABEL(a->list), TRUE); gtk_label_set_wrap(GTK_LABEL(a->sub), TRUE);
    gtk_box_append(GTK_BOX(c), a->big); gtk_box_append(GTK_BOX(c), a->sub); gtk_box_append(GTK_BOX(c), a->list);
    gtk_widget_add_css_class(c, "dash-alert-card");
    g_object_set_data_full(G_OBJECT(c), "st", a, [](gpointer p) { delete (A*)p; });
    GtkWidget* card_w = c;
    g_updaters.push_back([a, card_w](guint64 sec) {
        if ((sec % 30 != 0) || a->busy) return;
        a->busy = true;
        run_async({"curl", "-sf", "-m", "10", "-A", "curl", "https://ubilling.net.ua/aerialalerts/"}, [a, card_w](const std::string& out) {
            a->busy = false;
            json d; try { d = json::parse(out); } catch (...) { set_text(a->sub, "Немає звʼязку з сервісом тривог"); return; }
            if (!d.contains("states")) return;
            std::vector<std::string> on;
            bool mine = false, found = false; std::string since;
            for (auto& [k, v] : d["states"].items()) {
                bool al = v.value("alertnow", false);
                if (k == "Севастополь" || k == "АР Крим") continue;   // постійна тривога на окупованих — не рахуємо
                if (al) on.push_back(k);
                if (!a->region.empty() && k == a->region) { found = true; mine = al; since = v.value("changed", ""); }
            }
            gtk_widget_remove_css_class(card_w, "dash-alert-on"); gtk_widget_remove_css_class(card_w, "dash-alert-off");
            if (!a->region.empty()) {
                gtk_widget_add_css_class(card_w, mine ? "dash-alert-on" : "dash-alert-off");
                set_text(a->big, !found ? "Область не знайдено" : mine ? "\U000F0026  ТРИВОГА" : "\U000F012C  Відбій");
                std::string hm = since.size() >= 16 ? since.substr(11, 5) : "";
                set_text(a->sub, a->region + (hm.empty() ? "" : (mine ? " · з " : " · відбій о ") + hm));
            } else {
                set_text(a->big, on.empty() ? "\U000F012C  Тривог немає" : "Тривога: " + std::to_string(on.size()) + " обл.");
                set_text(a->sub, "Свою область — у Налаштування → Дашборд → ⚙");
            }
            std::string l;
            for (size_t i = 0; i < on.size() && i < 8; i++) l += (i ? ", " : "") + on[i];
            if (on.size() > 8) l += " та ще " + std::to_string(on.size() - 8);
            set_text(a->list, on.empty() ? "" : l);
        });
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- курс валют НБУ ----------
static void w_rates(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Курс НБУ", "\U000F0116");
    struct Rt { GtkWidget* box; std::vector<std::string> cc; std::string cache; bool busy = false; std::map<std::string, std::pair<GtkWidget*, GtkWidget*>> lbl; };
    auto* r = new Rt;
    r->cache = sysui::home() + "/.cache/mgos/dash_rates.json";
    if (o.contains("currencies") && o["currencies"].is_array()) { for (auto& x : o["currencies"]) if (x.is_string()) r->cc.push_back(x.get<std::string>()); }
    if (r->cc.empty()) r->cc = {"USD", "EUR", "PLN"};
    GtkWidget* g = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(g), 8); gtk_grid_set_column_homogeneous(GTK_GRID(g), TRUE);
    for (size_t i = 0; i < r->cc.size(); i++) {
        GtkWidget* t = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1); gtk_widget_add_css_class(t, "dash-tile");
        gtk_box_append(GTK_BOX(t), label(r->cc[i], "dash-tname"));
        GtkWidget* v = label("…", "dash-tbig"); GtkWidget* dl = label("", "dash-tsub");
        gtk_box_append(GTK_BOX(t), v); gtk_box_append(GTK_BOX(t), dl);
        r->lbl[r->cc[i]] = {v, dl};
        gtk_grid_attach(GTK_GRID(g), t, i, 0, 1, 1);
    }
    gtk_box_append(GTK_BOX(c), g);
    g_object_set_data_full(G_OBJECT(c), "st", r, [](gpointer p) { delete (Rt*)p; });
    auto show = [r](const json& cj) {
        for (auto& cc : r->cc) {
            if (!cj["now"].contains(cc)) continue;
            double now = cj["now"][cc].get<double>();
            set_text(r->lbl[cc].first, fmt("%.2f", now));
            if (cj.contains("prev") && cj["prev"].contains(cc)) {
                double d = now - cj["prev"][cc].get<double>();
                set_text(r->lbl[cc].second, std::fabs(d) < 0.005 ? "без змін" : (d > 0 ? "▲ " : "▼ ") + fmt("%.2f", std::fabs(d)));
            } else set_text(r->lbl[cc].second, "грн");
        }
    };
    g_updaters.push_back([r, show](guint64 sec) {
        if ((sec % 300 != 0) || r->busy) return;
        json cj; try { std::ifstream f(r->cache); if (f.good()) f >> cj; } catch (...) {}
        if (cj.is_object() && cj.contains("now")) show(cj);
        char day[16]; time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm); strftime(day, sizeof day, "%Y-%m-%d", &tm);
        if (cj.is_object() && cj.value("day", "") == day) return;   // раз на день
        r->busy = true;
        run_async({"curl", "-sf", "-m", "10", "https://bank.gov.ua/NBUStatService/v1/statdirectory/exchange?json"},
                  [r, show, cj, day = std::string(day)](const std::string& out) mutable {
            r->busy = false;
            json d; try { d = json::parse(out); } catch (...) { return; }
            json now = json::object();
            for (auto& x : d) if (x.contains("cc") && x.contains("rate")) now[x["cc"].get<std::string>()] = x["rate"].get<double>();
            if (now.empty()) return;
            json nc{{"day", day}, {"now", now}};
            if (cj.is_object() && cj.contains("now")) nc["prev"] = cj["now"];   // вчорашній курс — для стрілок
            g_mkdir_with_parents((sysui::home() + "/.cache/mgos").c_str(), 0755);
            { std::ofstream o(r->cache); o << nc.dump(); }
            show(nc);
        });
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- оновлення системи (pacman + AUR) ----------
static void w_updates(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Оновлення", "\U000F06B0");
    struct U { GtkWidget *big, *sub; std::string cache; bool busy = false; };
    auto* u = new U;
    u->cache = sysui::home() + "/.cache/mgos/dash_updates.txt";
    GtkWidget* h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2); gtk_widget_set_hexpand(v, TRUE);
    u->big = label("Перевіряю…", "dash-big2"); u->sub = label("", "dash-sub");
    gtk_label_set_wrap(GTK_LABEL(u->sub), TRUE);
    gtk_box_append(GTK_BOX(v), u->big); gtk_box_append(GTK_BOX(v), u->sub);
    GtkWidget* b = gtk_button_new_with_label("Оновити");
    gtk_widget_add_css_class(b, "dash-pill"); gtk_widget_add_css_class(b, "live"); gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    if (!g_preview) g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        spawn("kitty --class mgos-update -e bash -c 'yay; rm -f ~/.cache/mgos/dash_updates.txt; read -rp \"Готово. Enter — закрити\"'"); }), nullptr);
    gtk_box_append(GTK_BOX(h), v); gtk_box_append(GTK_BOX(h), b);
    gtk_box_append(GTK_BOX(c), h);
    g_object_set_data_full(G_OBJECT(c), "st", u, [](gpointer p) { delete (U*)p; });
    auto show = [u](const std::string& s) {
        std::istringstream f(s); long long ts = 0; int rep = 0, aur = 0; f >> ts >> rep >> aur;
        std::string names; std::getline(f, names); names = trim_s(names);
        int n = rep + aur;
        set_text(u->big, n ? std::to_string(n) + " " + (n % 10 == 1 && n % 100 != 11 ? "оновлення" : (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 10 || n % 100 >= 20)) ? "оновлення" : "оновлень")
                           : "\U000F012C  Система оновлена");
        set_text(u->sub, n ? (aur ? std::to_string(rep) + " з репозиторіїв · " + std::to_string(aur) + " з AUR" : names)
                           : "перевірено " + ago_ua(ts));
    };
    g_updaters.push_back([u, show](guint64 sec) {
        if ((sec % 120 != 0) || u->busy) return;
        std::string s = read_file(u->cache);
        long long ts = atoll(s.c_str());
        if (!s.empty()) show(s);
        if (ts && time(nullptr) - ts < 3 * 3600) return;   // раз на 3 год
        u->busy = true;
        run_async({"bash", "-c", "r=$(checkupdates 2>/dev/null); a=$(yay -Qua 2>/dev/null); "
                                 "printf '%s %d %d %s\\n' \"$(date +%s)\" \"$(printf '%s' \"$r\" | grep -c .)\" \"$(printf '%s' \"$a\" | grep -c .)\" "
                                 "\"$(printf '%s\\n%s' \"$r\" \"$a\" | awk 'NF{print $1}' | head -4 | paste -sd, -)\""},
                  [u, show](const std::string& out) {
            u->busy = false;
            if (out.empty()) return;
            g_mkdir_with_parents((sysui::home() + "/.cache/mgos").c_str(), 0755);
            { std::ofstream o(u->cache); o << out; }
            show(out);
        });
    });
    gtk_box_append(GTK_BOX(col), c);
}

// ---------- стиль ----------
static bool card_bg() { return g_cfg.value("card_bg", false); }   // false — віджети просто на шпалерах, без підкладки
// Яскравість розмитих шпалер (запечений блюр монітора) 0..1 — під неї підбираємо тон скла, як macOS:
// на світлих шпалерах скло темнішає, щоб білий текст читався; на темних — майже прозоре.
static long long g_lum_mtime = -1; static double g_lum = 0.4;
static double wall_lum() {
    std::string f; long long best = 0;
    if (GDir* d = g_dir_open((sysui::home() + "/.cache/mgos").c_str(), 0, nullptr)) {
        while (const char* n = g_dir_read_name(d)) {
            std::string s = n;
            if (s.rfind("baked_blur_", 0) || s.size() < 16 || s.compare(s.size() - 4, 4, ".png")) continue;
            if (!g_monitor.empty() && s != "baked_blur_" + g_monitor + ".png") continue;
            std::string p = sysui::home() + "/.cache/mgos/" + s; long long m = sysui::file_mtime_ns(p);
            if (m > best) { best = m; f = p; }
        }
        g_dir_close(d);
    }
    if (f.empty() || best == g_lum_mtime) return g_lum;
    g_lum_mtime = best;
    if (GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(f.c_str(), 48, 27, FALSE, nullptr)) {
        int n = gdk_pixbuf_get_n_channels(pb), rs = gdk_pixbuf_get_rowstride(pb), w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
        const guchar* px = gdk_pixbuf_read_pixels(pb); double sum = 0;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            const guchar* q = px + y * rs + x * n; sum += (0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2]) / 255.0;
        }
        g_lum = sum / (w * h);
        g_object_unref(pb);
    }
    return g_lum;
}
static void apply_css() {
    g_light = sysui::is_light_theme();
    bool nobg = !card_bg() && !g_preview;
    std::string fg = g_light ? "#1c1c1e" : "#ffffff";
    // «без фону» = скло: розмиті шпалери (запечена підкладка) майже без тону, лише тонка рамка
    double lum = wall_lum();
    // тон скла під яскравість шпалер: 0.74 (світле небо) → ~0.40, 0.3 (темні) → ~0.10
    double tint = std::clamp((lum - 0.25) * 0.65, 0.08, 0.46);
    std::string tint_s = fmt("%.2f", tint); for (auto& ch : tint_s) if (ch == ',') ch = '.';   // uk_UA: кома в printf ламає CSS
    std::string bg = nobg ? (g_light ? "rgba(255,255,255,0.10)" : "rgba(14,16,26," + tint_s + ")") : g_light ? "rgba(255,255,255,0.55)" : "rgba(22,22,28,0.42)";
    std::string bd = nobg ? (g_light ? "rgba(255,255,255,0.35)" : "rgba(255,255,255,0.16)") : g_light ? "rgba(0,0,0,0.08)" : "rgba(255,255,255,0.09)";
    bool bright = nobg && !g_light && lum > 0.5;   // світлі шпалери — другорядний текст яскравіший
    // без підкладки внутрішні плитки/кнопки трохи темніші — читаються на будь-яких шпалерах
    std::string btn = nobg ? (g_light ? "rgba(255,255,255,0.30)" : "rgba(255,255,255,0.08)") : g_light ? "rgba(0,0,0,0.06)" : "rgba(255,255,255,0.08)";
    std::string trough = g_light ? "rgba(0,0,0,0.10)" : "rgba(255,255,255,0.12)";
    std::string css =
        "window.dash-win { background: transparent; }\n"
        "window.dash-preview { background: linear-gradient(135deg, #2b1d48, #0f1a2e); }\n"
        ".dash-card { background: " + bg + "; border: 1px solid " + bd + "; border-radius: 22px; padding: 16px 18px; color: " + fg + "; }\n"
        ".dash-card label { color: " + fg + "; }\n"
        + std::string(nobg ? (g_light ? ".dash-card label { text-shadow: 0 1px 3px rgba(255,255,255,0.7); }\n"
                                      : ".dash-card label { text-shadow: 0 1px 2px rgba(0,0,0,0.45); }\n"
                                        ".dash-card { box-shadow: none; }\n") : "") +
        ".dash-title { font-size: 11px; font-weight: 700; letter-spacing: 1px; opacity: " + std::string(bright ? "0.85" : "0.6") + "; }\n"
        ".dash-hglyph { font-size: 13px; color: @ACCENT@; }\n"
        ".dash-clock { padding: 18px 22px; }\n"
        ".dash-time { font-size: 64px; font-weight: 800; letter-spacing: -1px; }\n"
        ".dash-date { font-size: 17px; font-weight: 600; opacity: 0.75; }\n"
        ".dash-big { font-size: 38px; font-weight: 700; }\n"
        ".dash-big2 { font-size: 17px; font-weight: 700; }\n"
        ".dash-key { font-size: 13px; font-weight: 600; }\n"
        ".dash-val { font-size: 12px; opacity: " + std::string(bright ? "0.9" : "0.7") + "; }\n"
        ".dash-sub { font-size: 12px; opacity: " + std::string(bright ? "0.88" : "0.65") + "; }\n"
        ".dash-wicon { font-size: 42px; color: @ACCENT@; }\n"
        "progressbar.dash-bar trough { min-height: 6px; border-radius: 4px; background: " + trough + "; border: none; }\n"
        "progressbar.dash-bar progress { min-height: 6px; border-radius: 4px; background: @ACCENT@; border: none; }\n"
        ".dash-mbtn { font-size: 22px; min-width: 44px; min-height: 40px; border-radius: 14px; background: " + btn + "; border: none; box-shadow: none; color: " + fg + "; }\n"
        ".dash-mbtn:hover, .dash-act:hover { background: @ACCENT_A30@; }\n"
        ".dash-act { padding: 8px 4px; border-radius: 14px; background: " + btn + "; border: none; box-shadow: none; color: " + fg + "; }\n"
        ".dash-aglyph { font-size: 22px; color: @ACCENT@; }\n"
        ".dash-alabel { font-size: 11px; font-weight: 600; }\n"
        "textview.dash-notes, textview.dash-notes text { background: transparent; color: " + fg + "; font-size: 14px; caret-color: @ACCENT@; }\n"
        "calendar.dash-cal { background: transparent; border: none; color: " + fg + "; }\n"
        "calendar.dash-cal > grid > label.today { background: @ACCENT@; color: white; border-radius: 8px; box-shadow: none; }\n"
        "calendar.dash-cal > grid > label.day-number:selected { background: @ACCENT_A30@; border-radius: 8px; }\n"
        "calendar.dash-cal > header { border: none; }\n"
        "calendar.dash-cal button { background: transparent; border: none; box-shadow: none; color: " + fg + "; }\n"
        ".dash-tile { background: " + btn + "; border-radius: 14px; padding: 9px 11px; }\n"
        ".dash-tname { font-size: 10px; font-weight: 700; letter-spacing: 0.8px; opacity: " + std::string(bright ? "0.8" : "0.55") + "; }\n"
        ".dash-tbig { font-size: 24px; font-weight: 800; }\n"
        ".dash-tsub { font-size: 11px; opacity: " + std::string(bright ? "0.88" : "0.65") + "; }\n"
        ".dash-cool { color: #30d158; } .dash-warm { color: #ff9f0a; } .dash-hot { color: #ff453a; }\n"
        ".dash-legend-a { font-size: 11px; color: @ACCENT@; } .dash-legend-b { font-size: 11px; color: #30d158; }\n"
        ".dash-art { border-radius: 12px; }\n"
        ".dash-game { padding: 6px; border-radius: 14px; background: " + btn + "; border: none; box-shadow: none; color: " + fg + "; }\n"
        ".dash-game:hover { background: @ACCENT_A30@; }\n"
        ".dash-gpic { border-radius: 10px; }\n"
        ".dash-gname { font-size: 12px; font-weight: 700; }\n"
        ".dash-pill { padding: 6px 14px; border-radius: 999px; background: @ACCENT@; color: white; border: none; box-shadow: none; font-weight: 700; }\n"
        ".dash-pill label { color: white; }\n"
        ".dash-alert-on { background: rgba(255,69,58,0.30); border-color: rgba(255,69,58,0.55); }\n"
        ".dash-alert-off .dash-big2 { color: #30d158; }\n";
    sysui::live_css(css);
}

// ---------- побудова ----------
static void tick_stop() { if (g_tick) { g_source_remove(g_tick); g_tick = 0; } }
static gboolean tick(gpointer);
static void tick_start() {
    if (g_tick) return;
    g_sec = 0;
    tick(nullptr);
    g_tick = g_timeout_add_seconds(1, tick, nullptr);
}

static void build() {
    g_gen++;
    g_cards.clear(); g_updaters.clear();
    if (g_root) gtk_box_remove(GTK_BOX(gtk_widget_get_parent(g_root)), g_root);
    int ncol = std::clamp(g_cfg.value("columns", 3), 1, 5);
    g_root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_set_homogeneous(GTK_BOX(g_root), TRUE);   // колонки однакової ширини
    gtk_widget_set_valign(g_root, GTK_ALIGN_START);
    std::vector<GtkWidget*> cols;
    for (int i = 0; i < ncol; i++) {
        GtkWidget* c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_widget_set_valign(c, GTK_ALIGN_START);
        cols.push_back(c);
    }
    int i = 0;
    for (auto& w : g_cfg["widgets"]) {
        if (!w.is_object() || !opt(w, "enabled", true)) continue;
        std::string t = opts(w, "type", "");
        // колонка: «col» з розкладки (Налаштування → Дашборд, перетягування), інакше — по черзі
        int ci = w.contains("col") && w["col"].is_number_integer() ? std::clamp(w["col"].get<int>(), 0, ncol - 1) : i % ncol;
        GtkWidget* col = cols[ci];
        if (t == "clock") w_clock(col, w);
        else if (t == "system") w_system(col, w);
        else if (t == "network") w_network(col, w);
        else if (t == "weather") w_weather(col, w);
        else if (t == "music") w_music(col, w);
        else if (t == "notes") w_notes(col, w);
        else if (t == "actions") w_actions(col, w);
        else if (t == "calendar") w_calendar(col, w);
        else if (t == "perf") w_perf(col, w);
        else if (t == "games") w_games(col, w);
        else if (t == "alerts") w_alerts(col, w);
        else if (t == "rates") w_rates(col, w);
        else if (t == "updates") w_updates(col, w);
        else continue;
        i++;
    }
    for (auto* c : cols) if (gtk_widget_get_first_child(c)) gtk_box_append(GTK_BOX(g_root), c);
    GtkWidget* holder = (GtkWidget*)g_object_get_data(G_OBJECT(g_win), "holder");
    gtk_box_append(GTK_BOX(holder), g_root);
    if (!g_preview) {
        std::string pos = opts(g_cfg, "position", "center");
        gtk_layer_set_anchor(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_LEFT, pos == "left");
        gtk_layer_set_anchor(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_RIGHT, pos == "right");
        gtk_layer_set_margin(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_LEFT, 48);
        gtk_layer_set_margin(GTK_WINDOW(g_win), GTK_LAYER_SHELL_EDGE_RIGHT, 48);
    }
    if (g_tick) { tick_stop(); tick_start(); }
}

static void reload() {
    load_cfg();
    if (!cfg_enabled() && !g_preview) { g_application_quit(g_application_get_default()); return; }
    apply_css();   // «без фону» / з фоном — з конфігу
    build();
}

static gboolean tick(gpointer) {
    if (g_sec % 2 == 0 && sysui::file_mtime_ns(cfg_path()) != g_cfg_mtime) { reload(); if (!g_win) return G_SOURCE_REMOVE; }
    if (sysui::is_light_theme() != g_light) apply_css();
    if (g_sec % 20 == 0) { long long m = g_lum_mtime; double l = g_lum; wall_lum();   // змінились шпалери — новий тон
                           if (g_lum_mtime != m && std::fabs(g_lum - l) > 0.03) apply_css(); }
    for (auto& u : g_updaters) u(g_sec);
    g_sec++;
    return G_SOURCE_CONTINUE;
}

// ---------- видимість: лише робочий стіл 1 ----------
static void update_vis() {
    if (g_preview) return;
    bool show = false; std::string mon;
    try {
        auto j = json::parse(sysui::hypr_request("j/activeworkspace"));
        int id = j.value("id", -1), wins = j.value("windows", 0);
        mon = j.value("monitor", "");
        show = id == 1 && (!g_cfg.value("only_empty", false) || wins == 0);
    } catch (...) {}
    if (show && mon != g_monitor) {
        GdkDisplay* d = gdk_display_get_default();
        GListModel* ms = gdk_display_get_monitors(d);
        for (guint i = 0; i < g_list_model_get_n_items(ms); i++) {
            GdkMonitor* m = GDK_MONITOR(g_list_model_get_item(ms, i));
            const char* c = gdk_monitor_get_connector(m);
            if (c && mon == c) {
                if (gtk_widget_get_visible(g_win)) gtk_widget_set_visible(g_win, FALSE);
                gtk_layer_set_monitor(GTK_WINDOW(g_win), m);
                g_monitor = mon;
            }
            g_object_unref(m);
        }
    }
    gtk_layer_set_layer(GTK_WINDOW(g_win), GTK_LAYER_SHELL_LAYER_BOTTOM);  // як віджети робочого столу: завжди під вікнами
    if (show == (bool)gtk_widget_get_visible(g_win)) return;
    if (show) {
        if (sysui::file_mtime_ns(cfg_path()) != g_cfg_mtime) { reload(); if (!cfg_enabled()) return; }
        gtk_widget_set_visible(g_win, TRUE);
        tick_start();
    } else {
        tick_stop();
        gtk_widget_set_visible(g_win, FALSE);
    }
}

static std::atomic<bool> g_vis_pending{false};
static void listen_hypr() {
    const char* xdg = getenv("XDG_RUNTIME_DIR"); const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!xdg || !sig) return;
    std::string path = std::string(xdg) + "/hypr/" + sig + "/.socket2.sock";
    while (true) {
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) { sleep(1); continue; }
        sockaddr_un a{}; a.sun_family = AF_UNIX; strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
        if (connect(s, (sockaddr*)&a, sizeof a) < 0) { close(s); sleep(1); continue; }
        char buf[2048]; std::string acc;
        while (true) {
            int n = read(s, buf, sizeof buf);
            if (n <= 0) break;
            acc.append(buf, n);
            size_t p;
            while ((p = acc.find('\n')) != std::string::npos) {
                std::string l = acc.substr(0, p); acc.erase(0, p + 1);
                if (!l.rfind("workspace>>", 0) || !l.rfind("focusedmon>>", 0) || !l.rfind("openwindow>>", 0) ||
                    !l.rfind("closewindow>>", 0) || !l.rfind("movewindow>>", 0) || !l.rfind("changefloatingmode>>", 0)) {
                    if (!g_vis_pending.exchange(true))
                        g_idle_add([](gpointer) -> gboolean { g_vis_pending = false; update_vis(); return G_SOURCE_REMOVE; }, nullptr);
                }
            }
        }
        close(s); sleep(1);
    }
}

static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    if (g_win) return;
    load_cfg();
    if (!cfg_enabled() && !g_preview) { g_application_quit(G_APPLICATION(app)); return; }
    g_win = gtk_application_window_new(app);
    gtk_widget_add_css_class(g_win, g_preview ? "dash-preview" : "dash-win");
    GtkWidget* holder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_set_data(G_OBJECT(g_win), "holder", holder);
    if (g_preview) {
        gtk_window_set_title(GTK_WINDOW(g_win), "mgOS dashboard preview");
        gtk_widget_set_margin_start(holder, 40); gtk_widget_set_margin_end(holder, 40);
        gtk_widget_set_margin_top(holder, 40); gtk_widget_set_margin_bottom(holder, 40);
        gtk_window_set_child(GTK_WINDOW(g_win), holder);
    } else {
        gtk_layer_init_for_window(GTK_WINDOW(g_win));
        gtk_layer_set_layer(GTK_WINDOW(g_win), GTK_LAYER_SHELL_LAYER_BOTTOM);
        gtk_layer_set_namespace(GTK_WINDOW(g_win), "mgos-dashboard");
        gtk_layer_set_keyboard_mode(GTK_WINDOW(g_win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        gtk_window_set_child(GTK_WINDOW(g_win),
            sysui::bb::underlay(g_win, holder, "mgos-dashboard", 22, [](std::vector<GtkWidget*>& v) {
                v = g_cards; }, true, false, true));   // запечений блюр — завжди: дашборд під вікнами, і коли поверх щось відкрите
        sysui::release_on_hide(g_win);
        sysui::pop_on_show(g_win, holder);
    }
    apply_css();
    build();
    if (g_preview) { gtk_window_present(GTK_WINDOW(g_win)); tick_start(); return; }
    g_application_hold(G_APPLICATION(app));
    g_unix_signal_add(SIGUSR1, [](gpointer) -> gboolean { reload(); update_vis(); return G_SOURCE_CONTINUE; }, nullptr);
    std::thread(listen_hypr).detach();
    update_vis();
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--window")) g_preview = true;
    sysui::mem_init();
    GtkApplication* app = gtk_application_new(g_preview ? "com.SYSui.dashboard_preview" : "com.SYSui.dashboard",
                                              g_preview ? G_APPLICATION_NON_UNIQUE : G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    char* av[] = {argv[0], nullptr};
    int st = g_application_run(G_APPLICATION(app), 1, av);
    g_object_unref(app);
    return st;
}
