// pixel_wallpaper.cpp — живі піксельні шпалери mgOS (стилі з pixel_waves.h: хвилі, горизонт, вірус, чорна діра…).
//
//   pixel_wallpaper                 — показати шпалери (стиль із settings.json → wp_pixel_style)
//   pixel_wallpaper --set horizon   — зробити піксельні шпалери поточними (last_wallpaper.txt = pixel:horizon),
//                                     прибрати mpvpaper/awww і запуститись
//   pixel_wallpaper --poster out.png [WxH] — один кадр у файл (постер для входу й SDDM)
//
// Економія: малює ~15 кадрів/с у сітці «пікселів» 6×6, на весь екран кадр замирає,
// від батареї (mgos-power) — 5 кадрів/с (або стоп / повна швидкість, Налаштування → wp_pixel_battery); колір — акцент mgOS (sysui::accent).
#include "sysui_common.h"
#include "pixel_waves.h"
#include <csignal>
#include <array>
#include <complex>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <sys/prctl.h>
#include <gio/gunixinputstream.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>

// ---- Профіль (PW_PROF=1): раз на хвилину в stderr — p50/p99/max часу кадру, таймера і пауз між кадрами ----
static const bool g_prof = getenv("PW_PROF") != nullptr;
struct ProfStat {
    std::vector<float> v;
    void add(double ms) { if (g_prof) v.push_back((float)ms); }
    std::string rep() {
        if (v.empty()) return "-";
        std::sort(v.begin(), v.end());
        char b[96]; snprintf(b, sizeof b, "n=%zu p50=%.2f p99=%.2f max=%.2f", v.size(), v[v.size() / 2], v[v.size() * 99 / 100], v.back());
        v.clear(); return b;
    }
};
static ProfStat g_pf_render, g_pf_sec, g_pf_gap;
static double prof_ms(gint64 a) { return (g_get_monotonic_time() - a) / 1000.0; }

static pw::Waves g_wv;
static GtkWidget* g_area = nullptr;
static double g_t = 0;
static gint64 g_last_us = 0;
static bool g_frozen = false;
static bool g_slow = false;    // від батареї: 5 кадрів/с
static int g_fps = 12;         // Налаштування → «Кадрів за секунду» (wp_pixel_fps, 2..30)

static std::string current_style() {
    try {
        std::ifstream f(sysui::settings_path());
        nlohmann::json j; f >> j;
        if (j.contains("wp_pixel_style") && j["wp_pixel_style"].is_string()) return j["wp_pixel_style"].get<std::string>();
    } catch (...) {}
    std::ifstream l(sysui::home() + "/.cache/ignis/last_wallpaper.txt");
    std::string s; std::getline(l, s);
    if (s.rfind("pixel:", 0) == 0) return s.substr(6);
    return "aurora";
}

// Перевизначення для прев'ю в Налаштуваннях: --style X --color #hex --theme light|dark
static std::string o_style, o_color, o_theme;
// «Колір з обкладинки альбому»: акцент системи і що зараз показано (головний потік плавно веде одне до одного)
static double g_sys_h = 280, g_sys_s = 0.7, g_sys_l = 0.55;
static double g_cur_h = -1, g_cur_s = 0, g_cur_l = 0;

static std::string setting_str(const char* k) {
    try { std::ifstream f(sysui::settings_path()); nlohmann::json j; f >> j;
          if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>(); } catch (...) {}
    return "";
}

static void apply_theme() {
    // колір шпалер: єдиний акцент системи (accent_mode/accent_color); --color лише для прев'ю
    bool own = sysui::valid_hex(o_color);
    g_wv.set_accent(own ? o_color : sysui::accent());
    if (!own && sysui::settings().accent_mode != "custom") g_wv.set_palette_css(sysui::wal_css_path());
    g_sys_h = g_wv.hue0; g_sys_s = g_wv.sat0; g_sys_l = g_wv.light0;
    if (g_cur_h >= 0) { g_wv.hue0 = g_cur_h; g_wv.sat0 = g_cur_s; g_wv.light0 = g_cur_l; }   // перехід продовжує alb_update()
    // тема: auto — як у системі
    std::string th = !o_theme.empty() ? o_theme : setting_str("wp_pixel_theme");
    if (th != "light" && th != "dark") th = sysui::color_scheme() == "prefer-dark" ? "dark" : "light";
    g_wv.light = th == "light";
    g_wv.style = !o_style.empty() ? o_style : current_style();
}

static bool on_battery() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    return rt && access((std::string(rt) + "/mgos-power/battery").c_str(), F_OK) == 0;
}

static bool fullscreen_now() {
    try {
        auto ws = nlohmann::json::parse(sysui::hypr_request("j/activeworkspace"));
        return ws.value("hasfullscreen", false);
    } catch (...) { return false; }
}

// ---- PixelView: кадр cols×rows → GdkTexture → GPU масштабує (NEAREST) на весь екран ----
// Раніше процесор розтягував кадр до 1920×1080 і щокадру слав у Hyprland 8 МБ;
// тепер у композитор іде лише маленька текстура (~230 КБ), масштаб — шейдер відеокарти.
G_DECLARE_FINAL_TYPE(PixelView, pixel_view, PIXEL, VIEW, GtkWidget)
struct _PixelView { GtkWidget parent; };
G_DEFINE_TYPE(PixelView, pixel_view, GTK_TYPE_WIDGET)
static void pixel_view_snapshot(GtkWidget* w, GtkSnapshot* snap) {
    int W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
    if (W <= 0 || H <= 0) return;
    g_wv.t = g_t;
    gint64 p0 = g_get_monotonic_time();
    g_wv.render(W, H);
    g_pf_render.add(prof_ms(p0));
    if (!g_wv.surf) return;
    int cw = cairo_image_surface_get_width(g_wv.surf), ch = cairo_image_surface_get_height(g_wv.surf);
    int st = cairo_image_surface_get_stride(g_wv.surf);
    GBytes* b = g_bytes_new(cairo_image_surface_get_data(g_wv.surf), (gsize)st * ch);
    GdkTexture* tex = gdk_memory_texture_new(cw, ch, GDK_MEMORY_DEFAULT, b, st);   // ARGB32 premultiplied = DEFAULT
    g_bytes_unref(b);
    graphene_rect_t r = GRAPHENE_RECT_INIT(0, 0, (float)(cw * g_wv.cell), (float)(ch * g_wv.cell));
    gtk_snapshot_append_scaled_texture(snap, tex, GSK_SCALING_FILTER_NEAREST, &r);
    g_object_unref(tex);
}
static void pixel_view_class_init(PixelViewClass* k) { GTK_WIDGET_CLASS(k)->snapshot = pixel_view_snapshot; }
static void pixel_view_init(PixelView*) {}

// ---- «Чорна діра»: курсор і вікна поточного робочого столу → привиди, що їх викривляє лінза ----
// Курсор — щокадру (один короткий запит до сокета Hyprland), вікна — кожен 3-й кадр,
// монітор і його робочий стіл — раз на секунду. Для інших стилів нічого не опитуємо.
static int g_mon_x = 0, g_mon_y = 0, g_mon_w = 0, g_mon_h = 0, g_mon_ws = -1;
static void bh_monitor() {
    if (!g_area) return;
    GtkNative* nat = gtk_widget_get_native(g_area);
    GdkSurface* surf = nat ? gtk_native_get_surface(nat) : nullptr;
    if (!surf) return;
    GdkMonitor* m = gdk_display_get_monitor_at_surface(gdk_display_get_default(), surf);
    if (!m) return;
    GdkRectangle g; gdk_monitor_get_geometry(m, &g);
    g_mon_x = g.x; g_mon_y = g.y; g_mon_w = g.width; g_mon_h = g.height;
}
// Яку частку екрана закривають вікна поточного робочого столу (сітка 32×18 точок).
// Як і відео-шпалери: якщо шпалер майже не видно — анімацію ставимо на паузу (кадр лишається).
static double covered_fraction(int mon_x, int mon_y, int mon_w, int mon_h, int ws) {
    if (mon_w <= 0 || ws < 0) return 0;
    std::vector<std::array<double, 4>> r;
    try {
        for (auto& w : nlohmann::json::parse(sysui::hypr_request("j/clients"))) {
            if (!w.value("mapped", true) || w.value("hidden", false)) continue;
            if (w["workspace"].value("id", -999) != ws) continue;
            double x = w["at"][0].get<double>() - mon_x, y = w["at"][1].get<double>() - mon_y;
            r.push_back({x, y, x + w["size"][0].get<double>(), y + w["size"][1].get<double>()});
        }
    } catch (...) { return 0; }
    if (r.empty()) return 0;
    int hit = 0, n = 0;
    for (int j = 0; j < 18; j++)
        for (int i = 0; i < 32; i++, n++) {
            double px = (i + 0.5) * mon_w / 32, py = (j + 0.5) * mon_h / 18;
            for (auto& q : r) if (px >= q[0] && px < q[2] && py >= q[1] && py < q[3]) { hit++; break; }
        }
    return (double)hit / n;
}
// Кольори вікон для «засмоктування»: знімок екрана одразу в сітці клітинок (grim -s 1/cell,
// ~320×180 px), лише коли вікна змінились або раз на 6 с, і лише коли є вікна. Тримається тільки в пам'яті цього процесу.
static bool g_shot_busy = false;
static void bh_capture() {
    if (g_shot_busy || g_mon_w <= 0 || g_wv.bh_wins.empty()) return;
    g_shot_busy = true;
    char geo[64], sc[32];
    snprintf(geo, sizeof geo, "%d,%d %dx%d", g_mon_x, g_mon_y, g_mon_w, g_mon_h);
    snprintf(sc, sizeof sc, "%.4f", 1.0 / std::max(1, g_wv.cell));
    for (char* c = sc; *c; c++) if (*c == ',') *c = '.';
    GSubprocess* p = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr,
                                      "grim", "-t", "ppm", "-s", sc, "-g", geo, "-", nullptr);
    if (!p) { g_shot_busy = false; return; }
    g_subprocess_communicate_async(p, nullptr, nullptr, [](GObject* o, GAsyncResult* r, gpointer) {
        GBytes* out = nullptr;
        if (g_subprocess_communicate_finish(G_SUBPROCESS(o), r, &out, nullptr, nullptr) && out) {
            gsize n = 0; const char* d = (const char*)g_bytes_get_data(out, &n);
            int w = 0, h = 0, mx = 0, off = 0;   // PPM P6: "P6\nW H\n255\n" + RGB
            if (n > 16 && sscanf(d, "P6 %d %d %d%n", &w, &h, &mx, &off) == 3 && w > 0 && h > 0 && (gsize)(off + 1 + w * h * 3) <= n) {
                const unsigned char* px = (const unsigned char*)d + off + 1;
                std::vector<uint32_t> v((size_t)w * h);
                for (int i = 0; i < w * h; i++) v[i] = 0xFF000000u | px[i * 3] << 16 | px[i * 3 + 1] << 8 | px[i * 3 + 2];
                g_wv.bh_shot.swap(v); g_wv.bh_shot_w = w; g_wv.bh_shot_h = h;
            }
            g_bytes_unref(out);
        }
        g_object_unref(o);
        g_shot_busy = false;
    }, nullptr);
}
static void bh_update() {
    static int frame = 0;
    if (g_wv.style != "blackhole") {
        if (!g_wv.bh_wins.empty()) { g_wv.bh_wins.clear(); g_wv.bh_wins.shrink_to_fit(); }
        if (!g_wv.bh_shot.empty()) { g_wv.bh_shot.clear(); g_wv.bh_shot.shrink_to_fit(); }
        return;
    }
    // знімок (~0.1–0.2 с CPU): коли вікна змінились (відкрили, перемістили, змінили розмір) або раз на 6 с
    {
        static gint64 last = 0; static size_t sig = 0;
        size_t h = g_wv.bh_wins.size();
        for (auto& w : g_wv.bh_wins) h = h * 31 + (size_t)(w.x0 * 7 + w.y0 * 13 + w.x1 * 17 + w.y1 * 19);
        gint64 now = g_get_monotonic_time();
        if ((h != sig && now - last > 1500000) || now - last > 6000000) { sig = h; last = now; bh_capture(); }
    }
    double c = std::max(1, g_wv.cell);
    try {
        auto j = nlohmann::json::parse(sysui::hypr_request("j/cursorpos"));
        g_wv.bh_cx = (j.value("x", -1e6) - g_mon_x) / c;
        g_wv.bh_cy = (j.value("y", -1e6) - g_mon_y) / c;
    } catch (...) {}
    if (frame++ % 3) return;
    try {
        std::string act;
        try { act = nlohmann::json::parse(sysui::hypr_request("j/activewindow")).value("address", ""); } catch (...) {}
        std::vector<pw::Waves::BHRect> v;
        for (auto& w : nlohmann::json::parse(sysui::hypr_request("j/clients"))) {
            if (!w.value("mapped", true) || w.value("hidden", false)) continue;
            if (w["workspace"].value("id", -999) != g_mon_ws) continue;
            auto at = w["at"], sz = w["size"];
            double x = at[0].get<double>() - g_mon_x, y = at[1].get<double>() - g_mon_y;
            v.push_back({(float)(x / c), (float)(y / c), (float)((x + sz[0].get<double>()) / c), (float)((y + sz[1].get<double>()) / c),
                         w.value("address", "") == act});
        }
        g_wv.bh_wins.swap(v);
    } catch (...) {}
}

// ---- «Еквалайзер» під музику ----
// Лише поки стиль eq видно і грає музика: дочірній parec знімає монітор типового виходу (44.1 кГц моно),
// власний FFT → по смузі на стовпчик стилю. Пауза / інший стиль / кадр замерз — процес зупиняємо (~0 CPU).
// Статус — раз на секунду: /proc/asound (без процесів) + playerctl асинхронно (Bluetooth-виходів нема в /proc/asound).
static constexpr int EQ_N = 2048;
static GSubprocess* g_eq_cap = nullptr;
static int g_eq_fd = -1;
static std::vector<float> g_eq_ring;
static int g_eq_pos = 0, g_eq_odd = -1;
static gint64 g_eq_loud_us = 0;            // коли востаннє звук був не тихий
static float g_eq_peak = -30;              // автопідсилення: найгучніша смуга, дБ
static bool g_eq_playing = false, g_eq_pl_busy = false, g_eq_lock = false;

static void eq_stop() {
    if (!g_eq_cap) return;
    g_subprocess_send_signal(g_eq_cap, SIGTERM);
    g_subprocess_wait_async(g_eq_cap, nullptr, [](GObject* o, GAsyncResult* r, gpointer) {
        g_subprocess_wait_finish(G_SUBPROCESS(o), r, nullptr); g_object_unref(o); }, nullptr);
    g_eq_cap = nullptr; g_eq_fd = -1;
}
static void eq_start() {
    GSubprocessLauncher* l = g_subprocess_launcher_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    // помре разом із шпалерами — жодних осиротілих процесів
    g_subprocess_launcher_set_child_setup(l, [](gpointer) { prctl(PR_SET_PDEATHSIG, SIGTERM); }, nullptr, nullptr);
    g_eq_cap = g_subprocess_launcher_spawn(l, nullptr, "parec", "-d", "@DEFAULT_MONITOR@", "--format=s16le",
                                           "--rate=44100", "--channels=1", "--latency-msec=20", nullptr);
    g_object_unref(l);
    if (!g_eq_cap) return;
    g_eq_fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(g_subprocess_get_stdout_pipe(g_eq_cap)));
    fcntl(g_eq_fd, F_SETFL, fcntl(g_eq_fd, F_GETFL) | O_NONBLOCK);
    g_eq_ring.assign(EQ_N, 0.f); g_eq_pos = 0; g_eq_odd = -1;
    g_eq_loud_us = g_get_monotonic_time();
}
static bool asound_running() {
    glob_t g{};
    bool run = false;
    if (glob("/proc/asound/card*/pcm*p/sub*/status", 0, nullptr, &g) == 0)
        for (size_t i = 0; i < g.gl_pathc && !run; i++) {
            std::ifstream f(g.gl_pathv[i]); std::string s; std::getline(f, s);
            run = s.find("RUNNING") != std::string::npos;
        }
    globfree(&g);
    return run;
}
static void eq_status(bool proc) {   // раз на секунду; proc — /proc/asound RUNNING (з фонового потоку)
    if (g_wv.style != "eq" || g_frozen) { eq_stop(); g_eq_playing = false; return; }
    static int n = 0; n++;
    if (!proc) g_eq_lock = false;
    if (!g_eq_pl_busy && (proc || g_eq_cap || n % 3 == 0)) {
        GSubprocess* p = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                          nullptr, "playerctl", "status", nullptr);
        if (p) {
            g_eq_pl_busy = true;
            g_subprocess_communicate_utf8_async(p, nullptr, nullptr, [](GObject* o, GAsyncResult* r, gpointer) {
                char* out = nullptr;
                g_eq_playing = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(o), r, &out, nullptr, nullptr) && out && strstr(out, "Playing");
                g_free(out); g_object_unref(o); g_eq_pl_busy = false;
            }, nullptr);
        }
    }
    // монітор тримає вихід «живим» → /proc каже RUNNING; після тиші без плеєра — стоп і чекаємо, доки вихід засне
    if (g_eq_cap && !g_eq_playing && g_get_monotonic_time() - g_eq_loud_us > 3000000) { eq_stop(); g_eq_lock = proc; return; }
    bool want = g_eq_playing || (proc && !g_eq_lock);
    if (want && !g_eq_cap) eq_start();
    else if (!want) eq_stop();
}
static void eq_fft(std::vector<std::complex<float>>& a) {   // ітеративний radix-2
    int n = a.size();
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2 * (float)M_PI / len;
        std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (int k = 0; k < len / 2; k++, w *= wl) {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
            }
        }
    }
}
static void eq_update(double dt, int cols) {
    gint64 now = g_get_monotonic_time();
    if (g_eq_fd >= 0) {
        unsigned char buf[8192];
        double e = 0; int cnt = 0;
        for (;;) {
            ssize_t n = read(g_eq_fd, buf, sizeof buf);
            if (n == 0) { eq_stop(); break; }          // parec завершився (змінили вихід) — перезапуск на наступній перевірці
            if (n < 0) break;                          // EAGAIN — більше нема
            for (ssize_t i = 0; i < n; i++) {
                if (g_eq_odd < 0) { g_eq_odd = buf[i]; continue; }
                float s = (int16_t)(g_eq_odd | buf[i] << 8) / 32768.f; g_eq_odd = -1;
                g_eq_ring[g_eq_pos] = s; g_eq_pos = (g_eq_pos + 1) % EQ_N;
                e += s * s; cnt++;
            }
        }
        if (cnt && std::sqrt(e / cnt) > 0.002) g_eq_loud_us = now;
    }
    bool live = g_eq_cap && now - g_eq_loud_us < 1500000;
    g_wv.eq_mix = std::clamp(g_wv.eq_mix + (float)(live ? dt * 4 : -dt * 2), 0.f, 1.f);
    if (g_wv.eq_mix <= 0) { if (!g_wv.eq_lv.empty()) { g_wv.eq_lv.clear(); g_wv.eq_lv.shrink_to_fit(); } return; }
    int nb = std::max(1, (cols + 2) / 3);
    if ((int)g_wv.eq_lv.size() != nb) g_wv.eq_lv.assign(nb, 0.f);
    if (!live) { for (auto& v : g_wv.eq_lv) v = std::max(0.f, v - (float)dt * 1.5f); return; }
    static std::vector<std::complex<float>> a(EQ_N);
    static std::vector<float> mag(EQ_N / 2);
    for (int i = 0; i < EQ_N; i++) {
        float hann = 0.5f - 0.5f * std::cos(2 * (float)M_PI * i / (EQ_N - 1));
        a[i] = {g_eq_ring[(g_eq_pos + i) % EQ_N] * hann, 0};
    }
    eq_fft(a);
    for (int k = 0; k < EQ_N / 2; k++) mag[k] = std::abs(a[k]) / (EQ_N / 4.f);
    const double F0 = 45, F1 = 15000, BIN = 44100.0 / EQ_N;
    float top = -120;
    std::vector<float> db(nb);
    for (int b = 0; b < nb; b++) {   // логарифмічна шкала частот: низькі зліва
        double fl = F0 * std::pow(F1 / F0, (double)b / nb), fh = F0 * std::pow(F1 / F0, (double)(b + 1) / nb);
        int k0 = (int)(fl / BIN), k1 = (int)(fh / BIN);
        float m = 0;
        if (k1 <= k0) { double kc = std::sqrt(fl * fh) / BIN; int k = std::min(EQ_N / 2 - 2, (int)kc); double fr = kc - k;
                        m = mag[k] * (1 - fr) + mag[k + 1] * fr; }
        else for (int k = k0; k <= std::min(k1, EQ_N / 2 - 1); k++) m = std::max(m, mag[k]);
        db[b] = 20 * std::log10(m + 1e-7f) + 3.5f * std::log2((float)std::sqrt(fl * fh) / 1000.f);   // нахил: верхи тихіші
        top = std::max(top, db[b]);
    }
    g_eq_peak = std::max(top, g_eq_peak - (float)dt * 6);   // автопідсилення: повільно відпускає
    g_eq_peak = std::max(g_eq_peak, -45.f);
    for (int b = 0; b < nb; b++) {
        float tg = std::clamp((db[b] - (g_eq_peak - 48)) / 48, 0.f, 1.f);
        tg = tg * tg * (1.4f - 0.4f * tg);   // контраст: тихі смуги нижчі
        float& v = g_wv.eq_lv[b];
        v = tg > v ? v + (tg - v) * 0.75f : std::max(tg, v - (float)dt * 1.6f);   // швидка атака, плавне падіння
    }
}

// ---- Раз на секунду — в окремому потоці: запити до сокета Hyprland (таймаут до 1 с!), settings.json,
// /proc/asound. Раніше це йшло в головному потоці й інколи зупиняло кадри (фрізи). Головний потік лише читає результат.
struct SecState { bool fs = false, covered = false, bat = false, asound = false, pause_cov = true, alb = false; std::string bm; int fps = -1, ws = -1; };
static std::mutex g_sec_mx;
static SecState g_sec;
static std::atomic<int> g_geo_x{0}, g_geo_y{0}, g_geo_w{0}, g_geo_h{0};
static std::atomic<bool> g_want_asound{false};
static void sec_worker() {
    for (;;) {
        SecState st;
        try { std::ifstream f(sysui::settings_path()); nlohmann::json j; f >> j;
              if (j.contains("wp_pixel_fps") && j["wp_pixel_fps"].is_number()) st.fps = std::clamp((int)j["wp_pixel_fps"].get<double>(), 2, 30);
              if (j.contains("wp_pixel_battery") && j["wp_pixel_battery"].is_string()) st.bm = j["wp_pixel_battery"].get<std::string>();
              if (j.contains("wp_pause_covered") && j["wp_pause_covered"].is_boolean()) st.pause_cov = j["wp_pause_covered"].get<bool>();
              if (j.contains("wp_pixel_album_color") && j["wp_pixel_album_color"].is_boolean()) st.alb = j["wp_pixel_album_color"].get<bool>(); } catch (...) {}
        st.bat = on_battery();
        int gx = g_geo_x, gy = g_geo_y, gw = g_geo_w, gh = g_geo_h;
        if (gw > 0) try {
            for (auto& mo : nlohmann::json::parse(sysui::hypr_request("j/monitors")))
                if (mo.value("x", 0) == gx && mo.value("y", 0) == gy) st.ws = mo["activeWorkspace"].value("id", -1);
        } catch (...) {}
        st.fs = fullscreen_now();
        st.covered = st.pause_cov && !st.fs && covered_fraction(gx, gy, gw, gh, st.ws) >= 0.85;
        if (g_want_asound) st.asound = asound_running();
        { std::lock_guard<std::mutex> lk(g_sec_mx); g_sec = st; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ---- «Колір з обкладинки альбому» (Налаштування → wp_pixel_album_color, за замовчуванням вимк.) ----
// Раз на 3 с (лише коли опція ввімкнена і кадр не заморожено) — асинхронно `playerctl metadata` (статус + artUrl).
// Нова обкладинка → разовий фоновий потік: file:// читаємо одразу, http(s) — curl у ~/.cache/megumin-os/album_art
// (лишаємо 8 найновіших), gdk-pixbuf 48×48 → гістограма відтінків по насичених пікселях (сірі/чорні/білі відкидаємо).
// Головний потік лише плавно (≈1.5 с) веде hue0/sat0/light0 до кольору обкладинки, а на паузі — назад до акценту.
struct AlbRes { int gen = 0; bool ok = false; double h = 0, s = 0, l = 0; };
static std::mutex g_alb_mx;
static AlbRes g_alb_res;                       // під g_alb_mx
static int g_alb_gen = 0;
static bool g_alb_on = false, g_alb_busy = false;
static std::string g_alb_url;                  // artUrl трека, що грає ("" — пауза/нема обкладинки)

using pw::RGB; using pw::rgb_hsl;
static bool alb_dominant(const std::string& path, double& H, double& S, double& L) {
    GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), 48, 48, FALSE, nullptr);
    if (!pb) return false;
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), rs = gdk_pixbuf_get_rowstride(pb), nc = gdk_pixbuf_get_n_channels(pb);
    const guchar* px = gdk_pixbuf_read_pixels(pb);
    double wt[36] = {}, cr[36] = {}, cg[36] = {}, cb[36] = {};
    int tot = 0, sat_n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const guchar* p = px + y * rs + x * nc;
            if (nc == 4 && p[3] < 128) continue;
            tot++;
            RGB c{p[0] / 255.0, p[1] / 255.0, p[2] / 255.0};
            double hh, s, l; rgb_hsl(c, hh, s, l);
            if (s < 0.25 || l < 0.12 || l > 0.88) continue;
            sat_n++;
            double k = s * (1 - std::fabs(l - 0.5) * 1.4);
            int b = ((int)(hh / 10) % 36 + 36) % 36;
            wt[b] += k; cr[b] += k * c.r; cg[b] += k * c.g; cb[b] += k * c.b;
        }
    g_object_unref(pb);
    if (!tot || sat_n < tot * 0.03) return false;     // майже сіра обкладинка — лишаємо акцент системи
    int best = 0; double bw = -1;
    for (int b = 0; b < 36; b++) {
        double v = wt[(b + 35) % 36] * 0.5 + wt[b] + wt[(b + 1) % 36] * 0.5;
        if (v > bw) { bw = v; best = b; }
    }
    double W = 0, R = 0, G = 0, B = 0;
    for (int d = -1; d <= 1; d++) { int b = (best + d + 36) % 36; W += wt[b]; R += cr[b]; G += cg[b]; B += cb[b]; }
    if (W <= 0) return false;
    rgb_hsl(RGB{R / W, G / W, B / W}, H, S, L);
    S = std::clamp(S, 0.45, 0.95); L = std::clamp(L, 0.42, 0.62);
    return true;
}

static void alb_load(std::string url, int gen) {     // у фоновому потоці
    std::string path;
    if (url.rfind("file://", 0) == 0) {
        if (char* p = g_filename_from_uri(url.c_str(), nullptr, nullptr)) { path = p; g_free(p); }
    } else if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        std::string dir = std::string(g_get_user_cache_dir()) + "/megumin-os/album_art";
        g_mkdir_with_parents(dir.c_str(), 0755);
        char* sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, url.c_str(), -1);
        path = dir + "/" + sum + ".img"; g_free(sum);
        if (access(path.c_str(), F_OK) != 0) {
            std::string tmp = path + ".part";
            const char* av[] = {"curl", "-sfL", "--max-time", "8", "--max-filesize", "8000000", "-o", tmp.c_str(), url.c_str(), nullptr};
            int st = 0;
            if (g_spawn_sync(nullptr, (char**)av, nullptr, (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                             nullptr, nullptr, nullptr, nullptr, &st, nullptr) && g_spawn_check_wait_status(st, nullptr))
                rename(tmp.c_str(), path.c_str());
            else unlink(tmp.c_str());
            namespace fs = std::filesystem;               // прибирання: лишаємо 8 найновіших
            std::vector<std::pair<fs::file_time_type, fs::path>> v; std::error_code ec;
            for (auto& d : fs::directory_iterator(dir, ec)) v.push_back({d.last_write_time(ec), d.path()});
            if (v.size() > 8) {
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
                for (size_t i = 8; i < v.size(); i++) fs::remove(v[i].second, ec);
            }
        }
    }
    AlbRes r; r.gen = gen;
    r.ok = !path.empty() && alb_dominant(path, r.h, r.s, r.l);
    std::lock_guard<std::mutex> lk(g_alb_mx);
    if (gen >= g_alb_res.gen) g_alb_res = r;
}

static void alb_status(bool on) {     // раз на секунду з головного потоку
    g_alb_on = on;
    static int n = 0;
    if (!on) { g_alb_url.clear(); return; }
    if (g_frozen || g_alb_busy || ++n % 3) return;
    GSubprocess* p = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr,
                                      "playerctl", "metadata", "--format", "{{status}}\t{{mpris:artUrl}}", nullptr);
    if (!p) return;
    g_alb_busy = true;
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, [](GObject* o, GAsyncResult* r, gpointer) {
        char* out = nullptr;
        std::string s;
        if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(o), r, &out, nullptr, nullptr) && out) s = out;
        g_free(out); g_object_unref(o); g_alb_busy = false;
        std::string url;
        auto tab = s.find('\t');
        if (s.rfind("Playing", 0) == 0 && tab != std::string::npos) {
            url = s.substr(tab + 1);
            while (!url.empty() && isspace((unsigned char)url.back())) url.pop_back();
        }
        if (!g_alb_on || url == g_alb_url) return;
        g_alb_url = url;
        int gen = ++g_alb_gen;
        if (!url.empty()) std::thread(alb_load, url, gen).detach();
    }, nullptr);
}

static void alb_update(double dt) {   // щокадру: плавний перехід кольору
    double th = g_sys_h, ts = g_sys_s, tl = g_sys_l;
    if (g_alb_on && !g_alb_url.empty()) {
        std::lock_guard<std::mutex> lk(g_alb_mx);
        if (g_alb_res.ok && g_alb_res.gen == g_alb_gen) { th = g_alb_res.h; ts = g_alb_res.s; tl = g_alb_res.l; }
    }
    if (g_cur_h < 0) { if (!g_alb_on) return; g_cur_h = g_sys_h; g_cur_s = g_sys_s; g_cur_l = g_sys_l; }
    double d = std::fmod(th - g_cur_h + 540.0, 360.0) - 180.0;
    if (std::fabs(d) < 0.3 && std::fabs(ts - g_cur_s) < 0.003 && std::fabs(tl - g_cur_l) < 0.003) {
        g_cur_h = th; g_cur_s = ts; g_cur_l = tl;
    } else {
        double k = 1 - std::exp(-dt * 3.0);          // ≈1.5 с до 99%
        g_cur_h = std::fmod(g_cur_h + d * k + 360.0, 360.0);
        g_cur_s += (ts - g_cur_s) * k; g_cur_l += (tl - g_cur_l) * k;
    }
    g_wv.hue0 = g_cur_h; g_wv.sat0 = g_cur_s; g_wv.light0 = g_cur_l;
}

static gboolean tick(gpointer);
// Кадри — рівним кроком 1/fps. Раніше опитування кожні 20 мс давало нерівні проміжки (80/100 мс при 12 к/с).
// Коли кадр заморожено — прокидаємось лише 4 рази на секунду.
static guint g_frame_iv = 0;
static gboolean frame_cb(gpointer) {
    tick(nullptr);
    int fps = g_slow ? std::max(2, g_fps / 2) : g_fps;   // від батареї «Повільно» — половина вибраного
    if (g_eq_cap && !g_slow) fps = std::max(fps, 30);    // «Еквалайзер» під музику — у такт
    guint iv = g_frozen ? 250 : 1000 / std::max(1, fps);
    if (iv != g_frame_iv) { g_frame_iv = iv; g_timeout_add_full(G_PRIORITY_DEFAULT, iv, frame_cb, nullptr, nullptr); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static gboolean tick(gpointer) {
    gint64 now = g_get_monotonic_time();
    double dt = g_last_us ? std::min(0.1, (now - g_last_us) / 1e6) : 0;
    if (!g_frozen && g_last_us) g_pf_gap.add((now - g_last_us) / 1000.0);
    g_last_us = now;
    if (!g_frozen) {
        g_t += dt;
        int w = gtk_widget_get_width(g_area);
        if (w > 0) g_wv.cell = std::max(4, w / 320);
        if (g_eq_cap || g_wv.eq_mix > 0) eq_update(dt, (w + g_wv.cell - 1) / std::max(1, g_wv.cell));
        bh_update();
        alb_update(dt);
        gtk_widget_queue_draw(g_area);
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char** argv) {
    // НЕ sysui::mem_init(): він вмикає Cairo-рендерер (CPU). Тут потрібен GPU (ngl) — він масштабує кадр.
    mallopt(M_ARENA_MAX, 2);
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    std::string a1 = argc > 1 ? argv[1] : "";
    for (int i = 1; i + 1 < argc; i++) {
        std::string k = argv[i];
        if (k == "--style") o_style = argv[i + 1];
        else if (k == "--color") o_color = argv[i + 1];
        else if (k == "--theme") o_theme = argv[i + 1];
    }

    if (a1 == "--set" && argc > 2) {
        std::string st = argv[2];
        sysui::settings_set("wp_pixel_style", st);
        { std::ofstream o(sysui::home() + "/.cache/ignis/last_wallpaper.txt"); o << "pixel:" << st; }
        system("pkill -9 mpvpaper; pkill -x awww-daemon; rm -f /tmp/mpv-socket");
        // інші копії — але не себе (pkill -x pixel_wallpaper убив би і цей процес)
        std::string me = std::to_string(getpid());
        system(("for p in $(pgrep -x pixel_wallpaper); do [ \"$p\" != " + me + " ] && kill $p; done").c_str());
        std::string self = sysui::home() + "/.config/SYSui/pixel_wallpaper";
        execl(self.c_str(), self.c_str(), (char*)nullptr);
        return 1;
    }

    if (a1 == "--poster" && argc > 2) {
        int W = 1920, H = 1080;
        if (argc > 3 && argv[3][0] != '-') sscanf(argv[3], "%dx%d", &W, &H);
        apply_theme();
        g_wv.fade_from = 2; g_wv.cell = std::max(4, W / 320);
        g_wv.t = 4.0;
        for (int i = 3; i + 1 < argc; i++) if (std::string(argv[i]) == "--time") g_wv.t = atof(argv[i + 1]);   // момент часу постера
        if (g_wv.style == "blackhole") {   // прев'ю: два «вікна» і курсор біля діри — видно лінзу і засмоктування
            double c = g_wv.cell, cols = W / c, rows = H / c;
            g_wv.bh_wins = {{(float)(cols * 0.06), (float)(rows * 0.12), (float)(cols * 0.36), (float)(rows * 0.46), true},
                            {(float)(cols * 0.66), (float)(rows * 0.58), (float)(cols * 0.95), (float)(rows * 0.9), false}};
            g_wv.bh_cx = cols * 0.62; g_wv.bh_cy = rows * 0.34;
            g_wv.bh_draw_wins = true;
            // прогін симуляції: пікселі вікон уже встигли відірватися і закрутитися до діри
            cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
            for (double tt = 0; tt < 7.3; tt += 0.05) { g_wv.t = tt; g_wv.render(W, H); }
            cairo_surface_destroy(tmp);
            g_wv.t = 7.3;
        }
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
        cairo_t* cr = cairo_create(s);
        if (g_wv.light) cairo_set_source_rgb(cr, 1, 1, 1); else cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        // клітинкові стилі (вірус, «Життя») мають «розвинутись» — проганяємо симуляцію до t
        if (g_wv.style == "virus" || g_wv.style == "life") {
            cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
            cairo_t* tc = cairo_create(tmp);
            for (double tt = 0; tt < 4.0; tt += 0.25) { g_wv.t = tt; g_wv.draw(tc, W, H); }
            cairo_destroy(tc); cairo_surface_destroy(tmp);
            g_wv.t = 4.0;
        }
        if (g_wv.style == "neoncity" || g_wv.style == "rooftop" || g_wv.style == "tower") {
            // сцени мають деталі фіксованого розміру в клітинках (вивіски, персонаж) — на крихітній сітці прев'ю
            // вони б заповнили кадр. Рендеримо як справжні шпалери (сітка 1920 / клітинка 6) і плавно зменшуємо.
            int RW = 1920, RH = std::max(1, 1920 * H / W);
            g_wv.cell = 6;
            cairo_surface_t* big = cairo_image_surface_create(CAIRO_FORMAT_RGB24, RW, RH);
            cairo_t* bc = cairo_create(big);
            g_wv.draw(bc, RW, RH);
            cairo_destroy(bc);
            cairo_save(cr);
            cairo_scale(cr, (double)W / RW, (double)H / RH);
            cairo_set_source_surface(cr, big, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_surface_destroy(big);
        } else g_wv.draw(cr, W, H);
        cairo_destroy(cr);
        int r = cairo_surface_write_to_png(s, argv[2]);
        cairo_surface_destroy(s);
        return r == CAIRO_STATUS_SUCCESS ? 0 : 1;
    }

    GtkApplication* app = gtk_application_new("com.megumin.pixelwallpaper", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication* app, gpointer) {
        sysui::install_shared_style();
        apply_theme();
        g_wv.fade_from = 2;          // на весь екран — без розсипання донизу
        g_wv.reveal = 1;
        GtkWidget* win = gtk_application_window_new(app);
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_namespace(GTK_WINDOW(win), "pixel_wallpaper");
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_BACKGROUND);
        for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
            gtk_layer_set_anchor(GTK_WINDOW(win), e, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        g_area = (GtkWidget*)g_object_new(pixel_view_get_type(), nullptr);
        gtk_widget_set_hexpand(g_area, TRUE); gtk_widget_set_vexpand(g_area, TRUE);
        gtk_window_set_child(GTK_WINDOW(win), g_area);
        gtk_window_present(GTK_WINDOW(win));
        // розмір «пікселя» — під роздільність (≈320 клітинок по ширині, як у playhorizon.dev)
        g_signal_connect(win, "notify::default-width", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) {}), nullptr);
        // ~15 кадрів/с; «Хвилі» і «Захід сонця» важчі на весь екран — 10 кадрів/с (≈10% одного ядра)
        frame_cb(nullptr);   // далі сам перезапускається з потрібним кроком
        sysui::settings_path(); sysui::hypr_request("j/version");   // статичні шляхи ініціалізуємо до потоку
        std::thread(sec_worker).detach();
        // раз на секунду: тема/стиль, повноекранне вікно, батарея → заморозити кадр
        g_timeout_add_seconds(1, [](gpointer) -> gboolean {
            static long long stamp = -1;
            gint64 p0 = g_get_monotonic_time();
            long long st = sysui::theme_stamp();
            if (st != stamp) { stamp = st; apply_theme(); gtk_widget_queue_draw(g_area); }
            // на весь екран шпалер не видно — стоп; від батареї — як у Налаштуваннях:
            // slow (за замовчуванням) — 5 к/с, freeze — стоп, full — повна швидкість
            bh_monitor();
            g_geo_x = g_mon_x; g_geo_y = g_mon_y; g_geo_w = g_mon_w; g_geo_h = g_mon_h;
            g_want_asound = g_wv.style == "eq";
            SecState ss; { std::lock_guard<std::mutex> lk(g_sec_mx); ss = g_sec; }
            if (ss.fps > 0) g_fps = ss.fps;
            g_mon_ws = ss.ws;
            g_frozen = ss.fs || ss.covered || (ss.bat && ss.bm == "freeze");
            g_slow = ss.bat && ss.bm != "full";
            eq_status(ss.asound);
            alb_status(ss.alb);
            g_pf_sec.add(prof_ms(p0));
            static int pn = 0;
            if (g_prof && ++pn % 60 == 0)
                fprintf(stderr, "[pw] %s eq=%d frozen=%d\n  render %s\n  gap    %s\n  1s     %s\n", g_wv.style.c_str(), g_eq_cap != nullptr, (int)g_frozen,
                        g_pf_render.rep().c_str(), g_pf_gap.rep().c_str(), g_pf_sec.rep().c_str());
            return G_SOURCE_CONTINUE;
        }, nullptr);
    }), nullptr);
    int r = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    return r;
}
