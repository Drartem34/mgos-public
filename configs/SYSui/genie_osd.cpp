// genie_osd — малює анімацію «джин» (3 пальці вниз → вікно в док) на GPU.
//
// Чому окремий процес: док малює через Cairo (CPU, економія пам'яті — sysui::mem_init), а GTK обирає рендерер
// один раз на процес. Cairo не вміє швидко масштабувати повноекранний знімок 60 разів на секунду (і не вміє 3D —
// звідти був рожевий квадрат). Тут — Vulkan: кадр будується за <1 мс CPU, решту робить відеокарта.
//
// Живе лише навколо жестів: док запускає його на початку свайпу 3 пальцями, після 90 с без жестів процес
// завершується сам (у фоні нічого не висить). Без вікна на екрані — нуль кадрів, нуль CPU.
//
// Протокол (рядки; stdin — команди доку, stdout — відповіді). Усі координати — логічні px монітора:
//   start <gen> <monitor> <gx> <gy> <ww> <wh> <wx> <wy> <radius_px> <scale> <tx> <ty> <s>
//        знімок вікна (grim, глобальні gx,gy), показ на місці вікна → "shown <gen>" (або "fail <gen>")
//   p <gen> <progress> <tx> <ty> <s>        куди ведуть пальці + де зараз іконка
//   end <gen> <commit 0|1> <ms>            доводимо до 1 (в док) або 0 (назад) → "done <gen> <commit>"
//   hide <gen>                              ховаємо оверлей, звільняємо знімок
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <unistd.h>
#include "genie_paint.h"

static GtkWidget* win = nullptr; static GtkWidget* area = nullptr;
static GdkTexture* tex = nullptr;
static unsigned gen = 0;
static bool loading = false, visible = false, want_end = false, end_commit = false, hit_sent = false;
static double end_ms = 300;
static double wx, wy, ww, wh;
static double tx, ty, ts;                 // іконка (ціль), плавно доганяє те, що шле док
static double ntx, nty, nts;
static double target_p = 0, shown_p = 0;
static bool animating = false; static double a_from, a_to, a_ms, a_m = 3, a_hold = 0; static gint64 a_t0, last_t = 0;
static double shown_v = 0;                // швидкість знімка (частка/мс), щоб доводка підхопила рух пальців без ривка
static guint tick_id = 0, idle_id = 0;
static std::string mon_name;
static bool grow_mode = false, shot_blank = false;   // grow: знімок виростає з іконки (p 1 → 0), а не стікає в неї

static void say(const std::string& s) { std::string l = s + "\n"; ssize_t r = write(1, l.data(), l.size()); (void)r; }

// доводка: кубічна крива Ерміта з початковим нахилом m (швидкість, яку мав знімок у мить відпускання) і нульовим
// у кінці. m = 3 — звичайний ease-out-cubic; m ≤ 3 — без перельоту. Раніше крива завжди стартувала з m = 3:
// якщо пальці рухались повільніше/в інший бік — знімок смикався в мить відпускання.
static double ease_from(double t, double m) {
    t = genie_clamp01(t);
    double t2 = t * t, t3 = t2 * t;
    return m * (t - 2 * t2 + t3) + (3 * t2 - 2 * t3);
}

typedef struct { GtkWidget parent; } GenieArea;
typedef struct { GtkWidgetClass parent_class; } GenieAreaClass;
G_DEFINE_TYPE(GenieArea, genie_area, GTK_TYPE_WIDGET)
static void genie_area_snapshot(GtkWidget*, GtkSnapshot* sn) {
    if (tex && visible) genie_paint(sn, tex, shown_p, wx, wy, ww, wh, tx, ty, ts);
}
static void genie_area_class_init(GenieAreaClass* k) { GTK_WIDGET_CLASS(k)->snapshot = genie_area_snapshot; }
static void genie_area_init(GenieArea*) {}

static void idle_arm() {
    if (idle_id) g_source_remove(idle_id);
    idle_id = g_timeout_add_seconds(90, [](gpointer) -> gboolean { if (!visible && !loading) exit(0); return G_SOURCE_CONTINUE; }, nullptr);
}

static void hide_all() {
    visible = false; animating = false; want_end = false;
    if (tick_id) { gtk_widget_remove_tick_callback(area, tick_id); tick_id = 0; }
    gtk_widget_set_visible(win, FALSE);   // unmap → буфер віддано, кадрів нема
    if (tex) { g_object_unref(tex); tex = nullptr; }
    idle_arm();
}

// follow_vel: продовжити поточний рух знімка (відпустили пальці); інакше — старт із спокою (Super+Q)
static void start_anim(double to, double ms, bool follow_vel) {
    animating = true; a_from = shown_p; a_to = to; a_ms = std::max(80.0, ms);
    a_t0 = 0; a_hold = 0;   // відлік — з першого кадру (on_tick): якщо перший кадр забарився, анімацію не «проскочимо» одразу в кінець
    double d = a_to - a_from;
    if (!follow_vel || std::abs(d) < 1e-4) a_m = 3;
    else a_m = std::clamp(shown_v * a_ms / d, 0.6, 3.0);   // мін. 0.6 — щоб після зупинки пальців не «завмирав»
}

static gboolean on_tick(GtkWidget* w, GdkFrameClock* fc, gpointer) {
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    double dt = last_t ? std::min(50.0, (now - last_t) / 1000.0) : 16.0;
    last_t = now;
    bool dirty = false;
    // іконка може ще виїжджати разом з доком — ціль доганяємо плавно
    double kt = 1 - std::exp(-dt / 40.0);
    for (auto [c, n] : {std::pair<double*, double>{&tx, ntx}, {&ty, nty}, {&ts, nts}})
        if (std::abs(n - *c) > 0.05) { *c += (n - *c) * kt; dirty = true; } else if (*c != n) { *c = n; dirty = true; }
    if (animating) {
        if (!a_t0) { a_t0 = now + (gint64)(a_hold * 1000); a_hold = 0; }
        double t = (now - a_t0) / 1000.0 / a_ms;
        shown_p = a_from + (a_to - a_from) * ease_from(t, a_m);
        dirty = true;
        // знімок «влітає» в іконку → док вдавлює її (squash & stretch)
        if (end_commit && !hit_sent && shown_p >= 0.82) { hit_sent = true; say("hit " + std::to_string(gen)); }
        if (t >= 1) {
            // want_end лишаємо true: до "hide" знімок стоїть, де закінчив. Інакше пізні "p" від доку (після росту
            // target_p = 1) знову тягнули його в іконку — поверх уже проявленого вікна з'являвся «привид»-джин
            shown_p = a_to; animating = false;
            say("done " + std::to_string(gen) + " " + (end_commit ? "1" : "0"));
        }
    } else if (!want_end) {
        // за пальцями: експоненційне згладжування, незалежне від частоти кадрів (libinput ~100 Гц, екран 60+)
        double d = target_p - shown_p, prev = shown_p;
        if (d != 0) { shown_p += std::abs(d) < 0.002 ? d : d * (1 - std::exp(-dt / 28.0)); dirty = true; }
        if (dt > 0) shown_v = shown_v * 0.5 + ((shown_p - prev) / dt) * 0.5;
    }
    if (dirty) gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static GdkTexture* load_ppm(const std::string& path, double radius) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    int w = 0, h = 0, mx = 0;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w <= 0 || h <= 0 || mx != 255) { fclose(f); return nullptr; }
    fgetc(f);
    size_t n = (size_t)w * h;
    std::vector<unsigned char> rgb(n * 3);
    if (fread(rgb.data(), 1, rgb.size(), f) != rgb.size()) { fclose(f); return nullptr; }
    fclose(f);
    guchar* px = (guchar*)g_malloc(n * 4);
    const unsigned char* s = rgb.data(); guchar* d = px;
    size_t lit = 0;
    for (size_t i = 0; i < n; i++, s += 3, d += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; lit += (s[0] | s[1] | s[2]) > 8; }
    shot_blank = lit * 50 < n;   // < 2 % не чорних — знімок порожній (вікно ще не намалювалось / прозоре в захопленні)
    // заокруглені кути, як у Hyprland (decoration:rounding) — інакше в кутах «їдуть» шматки шпалер
    int r = (int)std::ceil(radius);
    if (r > 1 && r * 2 < w && r * 2 < h) {
        for (int y = 0; y < r; y++) for (int x = 0; x < r; x++) {
            double dx = r - x - 0.5, dy = r - y - 0.5, dd = std::sqrt(dx * dx + dy * dy);
            double a = genie_clamp01(radius - dd + 0.5);
            if (a >= 1) continue;
            int xs[2] = {x, w - 1 - x}, ys[2] = {y, h - 1 - y};
            for (int yi : ys) for (int xi : xs) {
                guchar* q = px + ((size_t)yi * w + xi) * 4;
                q[0] = (guchar)(q[0] * a); q[1] = (guchar)(q[1] * a); q[2] = (guchar)(q[2] * a); q[3] = (guchar)(255 * a);
            }
        }
    }
    GBytes* bytes = g_bytes_new_take(px, n * 4);
    GdkTexture* t = gdk_memory_texture_new(w, h, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, bytes, (size_t)w * 4);
    g_bytes_unref(bytes);
    return t;
}

static std::string shot_path() {
    const char* x = getenv("XDG_RUNTIME_DIR");
    return std::string(x ? x : "/tmp") + "/mgos-genie.ppm";
}

static void place_on_monitor() {
    GListModel* ms = gdk_display_get_monitors(gdk_display_get_default());
    for (guint i = 0; i < g_list_model_get_n_items(ms); i++) {
        GdkMonitor* m = GDK_MONITOR(g_list_model_get_item(ms, i));
        const char* c = gdk_monitor_get_connector(m);
        bool hit = c && mon_name == c;
        if (hit) gtk_layer_set_monitor(GTK_WINDOW(win), m);
        g_object_unref(m);
        if (hit) return;
    }
}

static void on_shot(GObject* src, GAsyncResult* res, gpointer d) {
    unsigned g = GPOINTER_TO_UINT(d);
    double radius = 0; memcpy(&radius, g_object_get_data(src, "radius"), sizeof radius);
    bool ok = g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, nullptr);
    g_object_unref(src);
    if (g != gen) return;   // знімок попереднього жесту
    loading = false;
    if (ok) { if (tex) g_object_unref(tex); tex = load_ppm(shot_path(), radius); }
    unlink(shot_path().c_str());
    if (tex && grow_mode && shot_blank) { g_object_unref(tex); tex = nullptr; }   // композитор віддав порожній кадр
    if (!tex) { say("fail " + std::to_string(g)); idle_arm(); return; }
    visible = true; shown_p = grow_mode ? 1 : 0; shown_v = 0; last_t = 0;
    place_on_monitor();
    gtk_widget_set_visible(win, TRUE);
    if (!tick_id) tick_id = gtk_widget_add_tick_callback(area, on_tick, nullptr, nullptr);
    gtk_widget_queue_draw(area);
    // "shown" — коли кадр зі знімком реально відправлено композитору: тоді док ховає справжнє вікно
    GdkFrameClock* fc = gtk_widget_get_frame_clock(win);
    static gulong h = 0;
    if (h) g_signal_handler_disconnect(fc, h);
    h = g_signal_connect(fc, "after-paint", G_CALLBACK(+[](GdkFrameClock* fc, gpointer d) {
        g_signal_handler_disconnect(fc, h); h = 0;
        if (GPOINTER_TO_UINT(d) == gen && visible) say("shown " + std::to_string(gen));
    }), GUINT_TO_POINTER(g));
    // Super+Q: знімок ~50 мс стоїть рівно на вікні, поки док ховає справжнє (45 мс після "shown") — без подвійного зображення
    if (want_end) { start_anim(end_commit ? 1.0 : 0.0, end_ms, false); a_hold = grow_mode ? 0 : 50; }
}

// ---------- зворотна «здача»: запуск з доку → вікно виростає з іконки ----------
// Справжнє вікно док тримає прозорим, тому grim -g (знімок екрана) його не бачить — знімаємо саме вікно.
// grim -T (ext-image-copy-capture) Hyprland 0.53 не підтримує — тому toplevel_shot.py (hyprland_toplevel_export_v1).
static std::string grow_addr, grow_cls;
static double grow_radius = 0;
static void grow_shoot(unsigned g) {
    std::string out = shot_path();
    std::string py = std::string(g_get_home_dir()) + "/.config/SYSui/toplevel_shot.py";
    GSubprocess* sp = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr,
                                       "python3", "-I", py.c_str(), grow_addr.c_str(), out.c_str(), nullptr);
    if (!sp) { loading = false; say("fail " + std::to_string(g)); return; }
    double* rp = g_new(double, 1); *rp = grow_radius;
    g_object_set_data_full(G_OBJECT(sp), "radius", rp, g_free);
    g_subprocess_wait_check_async(sp, nullptr, on_shot, GUINT_TO_POINTER(g));
}
static void grow_cmd(unsigned g, std::istringstream& is) {
    double radius, scale;
    is >> mon_name >> ww >> wh >> wx >> wy >> radius >> scale >> ntx >> nty >> nts >> end_ms >> grow_addr >> grow_cls;
    if (is.fail()) { say("fail " + std::to_string(g)); return; }
    tx = ntx; ty = nty; ts = nts;
    if (tex && visible) hide_all();
    if (idle_id) { g_source_remove(idle_id); idle_id = 0; }
    gen = g; loading = true; grow_mode = true; want_end = true; end_commit = false; hit_sent = true;
    animating = false; target_p = 1; shown_p = 1; shown_v = 0;
    grow_radius = radius * scale;
    grow_shoot(g);
}

static void cmd(const std::string& line) {
    std::istringstream is(line);
    std::string c; unsigned g = 0;
    is >> c >> g;
    if (c == "plain") { genie_plain = g != 0; return; }
    if (c == "grow") { grow_cmd(g, is); return; }
    if (c == "start") {
        grow_mode = false;
        int gx, gy; double radius, scale;
        is >> mon_name >> gx >> gy >> ww >> wh >> wx >> wy >> radius >> scale >> ntx >> nty >> nts;
        if (is.fail()) { say("fail " + std::to_string(g)); return; }   // нерозібрана команда — док не має чекати вічно
        tx = ntx; ty = nty; ts = nts;
        if (tex && visible) hide_all();
        if (idle_id) { g_source_remove(idle_id); idle_id = 0; }
        gen = g; loading = true; want_end = false; hit_sent = false; animating = false; target_p = 0; shown_p = 0; shown_v = 0;
        char geo[96]; snprintf(geo, sizeof geo, "%d,%d %dx%d", gx, gy, (int)ww, (int)wh);
        std::string out = shot_path();
        GSubprocess* sp = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr,
                                           "grim", "-t", "ppm", "-g", geo, out.c_str(), nullptr);
        if (!sp) { loading = false; say("fail " + std::to_string(g)); return; }
        double* rp = g_new(double, 1); *rp = radius * scale;
        g_object_set_data_full(G_OBJECT(sp), "radius", rp, g_free);
        g_subprocess_wait_check_async(sp, nullptr, on_shot, GUINT_TO_POINTER(g));
    } else if (g != gen) {
        return;
    } else if (c == "p") {
        double p; is >> p >> ntx >> nty >> nts;
        if (!is.fail() && !want_end) target_p = genie_clamp01(p);
    } else if (c == "end") {
        int cm = 0; is >> cm >> end_ms;
        want_end = true; end_commit = cm != 0;
        if (visible) start_anim(end_commit ? 1.0 : 0.0, end_ms, true);
    } else if (c == "hide") {
        hide_all();
    }
}

static gboolean on_stdin(gint fd, GIOCondition cond, gpointer) {
    static std::string buf;
    char tmp[4096];
    ssize_t n = read(fd, tmp, sizeof tmp);
    if (n <= 0) exit(0);   // док закрився — ми теж
    buf.append(tmp, n);
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) { cmd(buf.substr(0, pos)); buf.erase(0, pos + 1); }
    (void)cond;
    return G_SOURCE_CONTINUE;
}

int main() {
    // явно GPU: якщо хтось успадкував GSK_RENDERER=cairo — тут він шкодить
    const char* r = getenv("GSK_RENDERER");
    if (r && !strcmp(r, "cairo")) unsetenv("GSK_RENDERER");
    setvbuf(stdout, nullptr, _IONBF, 0);
    gtk_init();
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window.genie { background: transparent; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    win = gtk_window_new();
    gtk_widget_add_css_class(win, "genie");
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_namespace(GTK_WINDOW(win), "mgos-genie");
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
        gtk_layer_set_anchor(GTK_WINDOW(win), e, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    area = GTK_WIDGET(g_object_new(genie_area_get_type(), nullptr));
    gtk_widget_set_hexpand(area, TRUE); gtk_widget_set_vexpand(area, TRUE);
    gtk_window_set_child(GTK_WINDOW(win), area);
    g_signal_connect(win, "realize", G_CALLBACK(+[](GtkWidget* w, gpointer) {   // кліки проходять крізь оверлей
        cairo_region_t* rg = cairo_region_create();
        gdk_surface_set_input_region(gtk_native_get_surface(GTK_NATIVE(w)), rg);
        cairo_region_destroy(rg);
    }), nullptr);
    gtk_widget_realize(win);   // рендерер і Vulkan готові заздалегідь — перший кадр жесту без затримки
    // прогрів: ті самі вузли (текстура, масштаб, кліп, opacity) один раз поза екраном — шейдери/конвеєри Vulkan
    // компілюються зараз, а не на першому кадрі жесту
    if (GskRenderer* rr = gtk_native_get_renderer(GTK_NATIVE(win))) {
        guchar* px = (guchar*)g_malloc0(16 * 16 * 4);
        GBytes* b = g_bytes_new_take(px, 16 * 16 * 4);
        GdkTexture* t = gdk_memory_texture_new(16, 16, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, b, 16 * 4);
        g_bytes_unref(b);
        GtkSnapshot* s = gtk_snapshot_new();
        genie_paint(s, t, 0.0, 0, 0, 16, 16, 8, 60, 4);
        genie_paint(s, t, 0.5, 0, 0, 16, 16, 8, 60, 4);
        genie_paint(s, t, 0.95, 0, 0, 16, 16, 8, 60, 4);
        genie_plain = true; genie_paint(s, t, 0.7, 0, 0, 16, 16, 8, 60, 4); genie_plain = false;
        if (GskRenderNode* n = gtk_snapshot_free_to_node(s)) {
            graphene_rect_t vp = GRAPHENE_RECT_INIT(0, 0, 32, 64);
            GdkTexture* o = gsk_renderer_render_texture(rr, n, &vp);
            if (o) g_object_unref(o);
            gsk_render_node_unref(n);
        }
        g_object_unref(t);
    }
    g_unix_fd_add(0, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), on_stdin, nullptr);
    say("ready");
    idle_arm();
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
}
