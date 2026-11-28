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

// ---------- музика ----------
static void w_music(GtkWidget* col, const json& o) {
    GtkWidget* c = card(o, "Музика", "\U000F075A");
    struct M { GtkWidget *title, *artist, *pp; bool busy = false; };
    auto* m = new M;
    m->title = label("Нічого не грає", "dash-big2"); m->artist = label("", "dash-sub");
    gtk_label_set_ellipsize(GTK_LABEL(m->title), PANGO_ELLIPSIZE_END); gtk_label_set_ellipsize(GTK_LABEL(m->artist), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(m->title), 24);
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
    gtk_box_append(GTK_BOX(c), m->title); gtk_box_append(GTK_BOX(c), m->artist); gtk_box_append(GTK_BOX(c), btns);
    g_object_set_data_full(G_OBJECT(c), "st", m, [](gpointer p) { delete (M*)p; });
    g_updaters.push_back([m](guint64 sec) {
        if (sec % 2 || m->busy) return;
        m->busy = true;
        run_async({"playerctl", "metadata", "--format", "{{status}}\n{{xesam:title}}\n{{xesam:artist}}"}, [m](const std::string& out) {
            m->busy = false;
            std::istringstream f(out); std::string st, ti, ar;
            std::getline(f, st); std::getline(f, ti); std::getline(f, ar);
            if (st.empty()) { set_text(m->title, "Нічого не грає"); set_text(m->artist, ""); }
            else { set_text(m->title, ti.empty() ? "Без назви" : ti); set_text(m->artist, ar); }
            gtk_button_set_label(GTK_BUTTON(m->pp), st == "Playing" ? "\U000F03E4" : "\U000F040A");
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

// ---------- стиль ----------
static void apply_css() {
    g_light = sysui::is_light_theme();
    std::string fg = g_light ? "#1c1c1e" : "#ffffff";
    std::string bg = g_light ? "rgba(255,255,255,0.55)" : "rgba(22,22,28,0.42)";
    std::string bd = g_light ? "rgba(0,0,0,0.08)" : "rgba(255,255,255,0.09)";
    std::string btn = g_light ? "rgba(0,0,0,0.06)" : "rgba(255,255,255,0.08)";
    std::string trough = g_light ? "rgba(0,0,0,0.10)" : "rgba(255,255,255,0.12)";
    std::string css =
        "window.dash-win { background: transparent; }\n"
        "window.dash-preview { background: linear-gradient(135deg, #2b1d48, #0f1a2e); }\n"
        ".dash-card { background: " + bg + "; border: 1px solid " + bd + "; border-radius: 22px; padding: 16px 18px; color: " + fg + "; }\n"
        ".dash-card label { color: " + fg + "; }\n"
        ".dash-title { font-size: 11px; font-weight: 700; letter-spacing: 1px; opacity: 0.6; }\n"
        ".dash-hglyph { font-size: 13px; color: @ACCENT@; }\n"
        ".dash-clock { padding: 18px 22px; }\n"
        ".dash-time { font-size: 64px; font-weight: 800; letter-spacing: -1px; }\n"
        ".dash-date { font-size: 17px; font-weight: 600; opacity: 0.75; }\n"
        ".dash-big { font-size: 38px; font-weight: 700; }\n"
        ".dash-big2 { font-size: 17px; font-weight: 700; }\n"
        ".dash-key { font-size: 13px; font-weight: 600; }\n"
        ".dash-val { font-size: 12px; opacity: 0.7; }\n"
        ".dash-sub { font-size: 12px; opacity: 0.65; }\n"
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
        "calendar.dash-cal button { background: transparent; border: none; box-shadow: none; color: " + fg + "; }\n";
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
        GtkWidget* col = cols[i % ncol];
        if (t == "clock") w_clock(col, w);
        else if (t == "system") w_system(col, w);
        else if (t == "network") w_network(col, w);
        else if (t == "weather") w_weather(col, w);
        else if (t == "music") w_music(col, w);
        else if (t == "notes") w_notes(col, w);
        else if (t == "actions") w_actions(col, w);
        else if (t == "calendar") w_calendar(col, w);
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
    build();
}

static gboolean tick(gpointer) {
    if (g_sec % 2 == 0 && sysui::file_mtime_ns(cfg_path()) != g_cfg_mtime) { reload(); if (!g_win) return G_SOURCE_REMOVE; }
    if (sysui::is_light_theme() != g_light) apply_css();
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
    haptic::attach(g_win);         // перетягування віджетів: тіки + «клац»
    haptic::attach_panel(g_win);
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
            sysui::bb::underlay(g_win, holder, "mgos-dashboard", 22, [](std::vector<GtkWidget*>& v) { v = g_cards; }, true));
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
