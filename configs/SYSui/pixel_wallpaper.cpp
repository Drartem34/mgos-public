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

static std::string setting_str(const char* k) {
    try { std::ifstream f(sysui::settings_path()); nlohmann::json j; f >> j;
          if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>(); } catch (...) {}
    return "";
}

static void apply_theme() {
    // колір шпалер: свій (wp_pixel_color) або акцент системи
    std::string col = !o_color.empty() ? o_color : setting_str("wp_pixel_color");
    bool own = sysui::valid_hex(col);
    g_wv.set_accent(own ? col : sysui::accent());
    if (!own && sysui::settings().accent_mode != "custom") g_wv.set_palette_css(sysui::wal_css_path());
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
    g_wv.render(W, H);
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
    try {
        for (auto& mo : nlohmann::json::parse(sysui::hypr_request("j/monitors")))
            if (mo.value("x", 0) == g.x && mo.value("y", 0) == g.y) g_mon_ws = mo["activeWorkspace"].value("id", -1);
    } catch (...) {}
}
// Яку частку екрана закривають вікна поточного робочого столу (сітка 32×18 точок).
// Як і відео-шпалери: якщо шпалер майже не видно — анімацію ставимо на паузу (кадр лишається).
static double covered_fraction() {
    if (g_mon_w <= 0 || g_mon_ws < 0) return 0;
    std::vector<std::array<double, 4>> r;
    try {
        for (auto& w : nlohmann::json::parse(sysui::hypr_request("j/clients"))) {
            if (!w.value("mapped", true) || w.value("hidden", false)) continue;
            if (w["workspace"].value("id", -999) != g_mon_ws) continue;
            double x = w["at"][0].get<double>() - g_mon_x, y = w["at"][1].get<double>() - g_mon_y;
            r.push_back({x, y, x + w["size"][0].get<double>(), y + w["size"][1].get<double>()});
        }
    } catch (...) { return 0; }
    if (r.empty()) return 0;
    int hit = 0, n = 0;
    for (int j = 0; j < 18; j++)
        for (int i = 0; i < 32; i++, n++) {
            double px = (i + 0.5) * g_mon_w / 32, py = (j + 0.5) * g_mon_h / 18;
            for (auto& q : r) if (px >= q[0] && px < q[2] && py >= q[1] && py < q[3]) { hit++; break; }
        }
    return (double)hit / n;
}
static void bh_update() {
    static int frame = 0;
    if (g_wv.style != "blackhole") {
        if (!g_wv.bh_wins.empty()) { g_wv.bh_wins.clear(); g_wv.bh_wins.shrink_to_fit(); }
        return;
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

static gboolean tick(gpointer) {
    gint64 now = g_get_monotonic_time();
    double dt = g_last_us ? std::min(0.1, (now - g_last_us) / 1e6) : 0;
    g_last_us = now;
    if (!g_frozen) {
        g_t += dt;
        int w = gtk_widget_get_width(g_area);
        if (w > 0) g_wv.cell = std::max(4, w / 320);
        bh_update();
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
        if (g_wv.style == "blackhole") {   // прев'ю: два «вікна» і курсор біля діри — видно лінзу і засмоктування
            double c = g_wv.cell, cols = W / c, rows = H / c;
            g_wv.bh_wins = {{(float)(cols * 0.06), (float)(rows * 0.12), (float)(cols * 0.36), (float)(rows * 0.46), true},
                            {(float)(cols * 0.66), (float)(rows * 0.58), (float)(cols * 0.95), (float)(rows * 0.9), false}};
            g_wv.bh_cx = cols * 0.62; g_wv.bh_cy = rows * 0.34;
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
        g_wv.draw(cr, W, H);
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
        g_timeout_add(20, [](gpointer) -> gboolean {
            static gint64 last = 0;
            gint64 now = g_get_monotonic_time();
            int fps = g_slow ? std::max(2, g_fps / 2) : g_fps;   // від батареї «Повільно» — половина вибраного
            int interval = 1000000 / std::max(1, fps);
            if (now - last >= interval - 10000) { last = now; tick(nullptr); }
            return G_SOURCE_CONTINUE;
        }, nullptr);
        // раз на секунду: тема/стиль, повноекранне вікно, батарея → заморозити кадр
        g_timeout_add_seconds(1, [](gpointer) -> gboolean {
            static long long stamp = -1;
            long long st = sysui::theme_stamp();
            if (st != stamp) { stamp = st; apply_theme(); gtk_widget_queue_draw(g_area); }
            // на весь екран шпалер не видно — стоп; від батареї — як у Налаштуваннях:
            // slow (за замовчуванням) — 5 к/с, freeze — стоп, full — повна швидкість
            std::string bm = setting_str("wp_pixel_battery");
            try { std::ifstream f(sysui::settings_path()); nlohmann::json j; f >> j;
                  if (j.contains("wp_pixel_fps") && j["wp_pixel_fps"].is_number()) g_fps = std::clamp((int)j["wp_pixel_fps"].get<double>(), 2, 30); } catch (...) {}
            bool bat = on_battery();
            bh_monitor();
            bool cover_pause = true;
            try { std::ifstream f(sysui::settings_path()); nlohmann::json j; f >> j;
                  if (j.contains("wp_pixel_pause_covered") && j["wp_pixel_pause_covered"].is_boolean()) cover_pause = j["wp_pixel_pause_covered"].get<bool>(); } catch (...) {}
            bool covered = cover_pause && covered_fraction() >= 0.85;
            g_frozen = fullscreen_now() || covered || (bat && bm == "freeze");
            g_slow = bat && bm != "full";
            return G_SOURCE_CONTINUE;
        }, nullptr);
    }), nullptr);
    int r = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    return r;
}
