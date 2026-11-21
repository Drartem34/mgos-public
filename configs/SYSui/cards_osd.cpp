// cards_osd — шар карток простору (Super+A): підписи, рамки під курсором, кнопки «На робочий стіл».
//
// Чому окремий процес: док малює через Cairo (CPU), а GTK обирає рендерер раз на процес. Повноекранний шар, що
// 60 разів на секунду змінює прозорість і зсув, на Cairo з'їдав ~половину ядра під час кожної появи простору —
// звідти лаги й ривки. Тут — GPU (як genie_osd): кадр — це перетворення вже готових текстур.
//
// Анімацію рахуємо самі, тими ж кривими й тривалістю, що й Hyprland (док передає їх із `hyprctl animations`),
// і на момент, коли кадр ПОКАЖУТЬ (час кадру + період оновлення), — інакше на крутому початку кривої шар
// відставав від вікон на кадр. Анімація Hyprland для цього шару вимкнена (animations.hl: layerrule no_anim).
//
// Протокол — JSON по рядку. stdin (від доку):
//   {"c":"layout", "W","H", "input":[x,y,w,h], "cw","ch", "radius", "move":{bezier+ms}, "cards":[{"a","title","icon","x","y"}]}
//   {"c":"show"|"hide", "t0":мкс CLOCK_MONOTONIC (коли Hyprland почав анімацію), "an":{x0,y0,x1,y1,ms,sx,sy,fade}}
//   {"c":"drop", "a":addr}      — картку повертають на стіл: її підписи зникають одразу
// stdout (доку): "restore <addr>" | "close" | "scroll <кроки>"
// Живе, поки живий док (stdin закрився → вихід). Без анімації — нуль кадрів, нуль CPU.
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <glib-unix.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <cmath>
#include <map>
#include <string>
#include <algorithm>

using json = nlohmann::json;

struct Anim { double x0 = 0, y0 = 0, x1 = 1, y1 = 1, ms = 0, sx = 0, sy = 0; bool fade = true; };
static Anim an_vis, an_move;

static double bezier_at(const Anim& a, double x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double lo = 0, hi = 1, t = x;
    for (int i = 0; i < 40; i++) {
        t = (lo + hi) / 2;
        double bx = 3 * (1 - t) * (1 - t) * t * a.x0 + 3 * (1 - t) * t * t * a.x1 + t * t * t;
        (bx < x ? lo : hi) = t;
    }
    return 3 * (1 - t) * (1 - t) * t * a.y0 + 3 * (1 - t) * t * t * a.y1 + t * t * t;
}
static Anim anim_of(const json& j) {
    Anim a;
    a.x0 = j.value("x0", 0.0); a.y0 = j.value("y0", 0.0); a.x1 = j.value("x1", 1.0); a.y1 = j.value("y1", 1.0);
    a.ms = j.value("ms", 0.0); a.sx = j.value("sx", 0.0); a.sy = j.value("sy", 0.0); a.fade = j.value("fade", true);
    return a;
}

struct Card {
    GtkWidget *title = nullptr, *label = nullptr, *icon = nullptr, *hit = nullptr, *holder = nullptr;
    std::string icon_name;
    double cx = 0, cy = 0, fx = 0, fy = 0, tx = 0, ty = 0;
    gint64 t0 = 0;
};
static std::map<std::string, Card> cards;
static GtkWidget *win = nullptr, *root = nullptr, *inner = nullptr, *empty_lbl = nullptr;
static GtkCssProvider* rad_css = nullptr;
static double W = 1920, H = 1080, CW = 0, CH = 0;
static int in_x = 0, in_y = 0, in_w = 0, in_h = 0;
static int vis_dir = 0;            // 1 — з'являється, -1 — зникає
static gint64 vis_t0 = 0;
static bool shown = false;         // логічний стан: простір відкритий
static guint tick = 0;
static double scroll_acc = 0;
static guint idle_exit = 0;
// 10 хв без появи простору — виходимо (~90 МБ пам'яті не тримаємо даремно); док запустить знову, коли знадобиться
static void idle_arm() {
    if (idle_exit) g_source_remove(idle_exit);
    idle_exit = g_timeout_add_seconds(600, [](gpointer) -> gboolean { if (!shown && !vis_dir) exit(0); idle_exit = 0; idle_arm(); return G_SOURCE_REMOVE; }, nullptr);
}
static const double TITLE = 38;

static void say(const std::string& s) { std::string l = s + "\n"; ssize_t r = write(1, l.data(), l.size()); (void)r; }

static void place(Card& c) {
    gtk_fixed_move(GTK_FIXED(inner), c.title, c.cx, c.cy - TITLE);
    gtk_fixed_move(GTK_FIXED(inner), c.hit, c.cx, c.cy);
    gtk_fixed_move(GTK_FIXED(inner), c.holder, c.cx, c.cy + CH + 12);
}

static void input_region() {
    if (!gtk_widget_get_realized(win)) return;
    cairo_rectangle_int_t r = {in_x, in_y, in_w, in_h};
    cairo_region_t* reg = cairo_region_create_rectangle(&r);
    gdk_surface_set_input_region(gtk_native_get_surface(GTK_NATIVE(win)), reg);
    cairo_region_destroy(reg);
}

// один кадр: t — момент, коли його покажуть
static bool frame(gint64 t) {
    bool busy = false;
    double off = 0, alpha = 1;
    if (vis_dir) {
        double p = an_vis.ms > 0 ? (t - vis_t0) / 1000.0 / an_vis.ms : 1;
        double e = bezier_at(an_vis, std::clamp(p, 0.0, 1.0));
        double k = vis_dir > 0 ? 1 - e : e;
        off = k; alpha = an_vis.fade ? 1 - k : 1;
        if (p >= 1) {
            if (vis_dir < 0) { vis_dir = 0; gtk_widget_set_visible(win, FALSE); return false; }
            vis_dir = 0;
        } else busy = true;
    }
    graphene_point_t sh = GRAPHENE_POINT_INIT((float)(off * an_vis.sx * W), (float)(off * an_vis.sy * H));
    GskTransform* tr = gsk_transform_translate(nullptr, &sh);
    gtk_fixed_set_child_transform(GTK_FIXED(root), inner, tr);
    gsk_transform_unref(tr);
    gtk_widget_set_opacity(inner, alpha);
    for (auto& [a, c] : cards) {
        if (c.cx == c.tx && c.cy == c.ty) continue;
        double p = an_move.ms > 0 ? (t - c.t0) / 1000.0 / an_move.ms : 1;
        double e = bezier_at(an_move, std::clamp(p, 0.0, 1.0));
        c.cx = p >= 1 ? c.tx : c.fx + (c.tx - c.fx) * e;
        c.cy = p >= 1 ? c.ty : c.fy + (c.ty - c.fy) * e;
        if (p < 1) busy = true;
        place(c);
    }
    return busy;
}

static void kick() {
    if (tick) return;
    tick = gtk_widget_add_tick_callback(win, [](GtkWidget*, GdkFrameClock* fc, gpointer) -> gboolean {
        gint64 refresh = 0, presented = 0, ft = gdk_frame_clock_get_frame_time(fc);
        gdk_frame_clock_get_refresh_info(fc, ft, &refresh, &presented);
        if (refresh <= 0 || refresh > 100000) refresh = 16667;
        if (frame(std::max(ft, g_get_monotonic_time()) + refresh)) return G_SOURCE_CONTINUE;
        tick = 0;
        return G_SOURCE_REMOVE;
    }, nullptr, nullptr);
}

static GtkWidget* icon_widget(const std::string& name) {
    GtkWidget* img;
    if (!name.empty() && name[0] == '/') { img = gtk_picture_new_for_filename(name.c_str()); gtk_widget_set_size_request(img, 22, 22); }
    else { img = gtk_image_new_from_icon_name(name.empty() ? "application-x-executable" : name.c_str()); gtk_image_set_pixel_size(GTK_IMAGE(img), 22); }
    return img;
}

static Card& card(const std::string& a) {
    auto it = cards.find(a);
    if (it != cards.end()) return it->second;
    Card& c = cards[a];
    c.title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(c.title, "card-title");
    c.label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(c.label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(c.label), 1);
    gtk_label_set_xalign(GTK_LABEL(c.label), 0);
    gtk_widget_set_hexpand(c.label, TRUE);
    gtk_box_append(GTK_BOX(c.title), c.label);

    c.hit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);   // прозора рамка поверх вікна: підсвітка й клік «відкрити»
    gtk_widget_add_css_class(c.hit, "card-hit");
    gtk_widget_set_cursor_from_name(c.hit, "pointer");
    g_object_set_data_full(G_OBJECT(c.hit), "addr", g_strdup(a.c_str()), g_free);
    GtkGesture* gc = gtk_gesture_click_new();
    g_signal_connect(gc, "released", G_CALLBACK(+[](GtkGestureClick* g, int, double, double, gpointer) {
        say(std::string("restore ") + (const char*)g_object_get_data(G_OBJECT(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g))), "addr"));
    }), nullptr);
    gtk_widget_add_controller(c.hit, GTK_EVENT_CONTROLLER(gc));

    c.holder = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* btn = gtk_button_new_with_label("На робочий стіл");
    gtk_widget_add_css_class(btn, "card-btn");
    gtk_widget_set_cursor_from_name(btn, "pointer");
    gtk_widget_set_halign(btn, GTK_ALIGN_CENTER); gtk_widget_set_hexpand(btn, TRUE);
    g_object_set_data_full(G_OBJECT(btn), "addr", g_strdup(a.c_str()), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
        say(std::string("restore ") + (const char*)g_object_get_data(G_OBJECT(b), "addr"));
    }), nullptr);
    gtk_box_append(GTK_BOX(c.holder), btn);
    for (GtkWidget* w : {c.title, c.hit, c.holder}) gtk_fixed_put(GTK_FIXED(inner), w, 0, 0);
    c.cx = c.tx = NAN;
    return c;
}

static void drop(const std::string& a) {
    auto it = cards.find(a);
    if (it == cards.end()) return;
    for (GtkWidget* w : {it->second.title, it->second.hit, it->second.holder}) gtk_fixed_remove(GTK_FIXED(inner), w);
    cards.erase(it);
}

static void on_layout(const json& j) {
    W = j.value("W", 1920.0); H = j.value("H", 1080.0); CW = j.value("cw", 0.0); CH = j.value("ch", 0.0);
    if (j.contains("input") && j["input"].size() == 4) { in_x = j["input"][0]; in_y = j["input"][1]; in_w = j["input"][2]; in_h = j["input"][3]; }
    if (j.contains("move")) an_move = anim_of(j["move"]);
    static int last_r = -1;
    int r = j.value("radius", 0);
    if (r != last_r) {
        last_r = r;
        std::string c = ".card-hit { border-radius: " + std::to_string(r) + "px; }";
        gtk_css_provider_load_from_string(rad_css, c.c_str());
    }
    gint64 now = g_get_monotonic_time();
    std::map<std::string, bool> keep;
    const json cl = j.value("cards", json::array());
    for (auto& cj : cl) {
        std::string a = cj.value("a", "");
        if (a.empty()) continue;
        keep[a] = true;
        Card& c = card(a);
        gtk_label_set_text(GTK_LABEL(c.label), cj.value("title", "").c_str());
        std::string ic = cj.value("icon", "");
        if (ic != c.icon_name || !c.icon) {
            if (c.icon) gtk_box_remove(GTK_BOX(c.title), c.icon);
            c.icon = icon_widget(ic); c.icon_name = ic;
            gtk_box_prepend(GTK_BOX(c.title), c.icon);
        }
        gtk_widget_set_size_request(c.title, (int)CW, (int)TITLE - 8);
        gtk_widget_set_size_request(c.hit, (int)CW, (int)CH);
        gtk_widget_set_size_request(c.holder, (int)CW, -1);
        for (GtkWidget* w : {c.title, c.hit, c.holder}) gtk_widget_set_visible(w, TRUE);
        double tx = cj.value("x", 0.0), ty = cj.value("y", 0.0);
        if (std::isnan(c.cx) || !shown) { c.cx = c.fx = c.tx = tx; c.cy = c.fy = c.ty = ty; }   // закрито — одразу на місце
        else if (tx != c.tx || ty != c.ty) { c.fx = c.cx; c.fy = c.cy; c.tx = tx; c.ty = ty; c.t0 = now; }
        place(c);
    }
    for (auto it = cards.begin(); it != cards.end(); ) { auto a = it->first; ++it; if (!keep.count(a)) drop(a); }
    gtk_widget_set_visible(empty_lbl, cards.empty());
    if (cards.empty()) { gtk_widget_set_size_request(empty_lbl, (int)W, -1); gtk_fixed_move(GTK_FIXED(inner), empty_lbl, 0, H / 2 - 20); }
    input_region();
    if (shown) kick();
}

static void cmd(const std::string& line) {
    json j;
    try { j = json::parse(line); } catch (...) { return; }
    std::string c = j.value("c", "");
    idle_arm();
    if (c == "layout") on_layout(j);
    else if (c == "show" || c == "hide") {
        an_vis = anim_of(j.value("an", json::object()));
        gint64 t0 = j.value("t0", (gint64)0);
        if (!t0) t0 = g_get_monotonic_time();
        if (c == "show") {
            shown = true; vis_dir = 1; vis_t0 = t0;
            // перший кадр шару покажуть не раніше ніж за ~2 кадри (map поверхні)
            frame(g_get_monotonic_time() + 2 * 16667);
            gtk_widget_set_visible(win, TRUE);
            input_region();
            kick();
        } else if (gtk_widget_get_visible(win)) {
            shown = false; vis_dir = -1; vis_t0 = t0;
            kick();
        } else shown = false;
    } else if (c == "drop") {
        auto it = cards.find(j.value("a", ""));
        if (it != cards.end()) for (GtkWidget* w : {it->second.title, it->second.hit, it->second.holder}) gtk_widget_set_visible(w, FALSE);
    }
}

static gboolean on_stdin(gint fd, GIOCondition, gpointer) {
    static std::string buf;
    char tmp[8192];
    ssize_t n = read(fd, tmp, sizeof tmp);
    if (n <= 0) exit(0);   // док закрився — ми теж
    buf.append(tmp, n);
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) { cmd(buf.substr(0, pos)); buf.erase(0, pos + 1); }
    return G_SOURCE_CONTINUE;
}

static const char* CSS = R"(
window.cards { background: transparent; }
.card-title, .card-title label, .card-btn, .card-btn label, .card-empty { font-family: "SF Pro Display", "Inter", sans-serif; }
.card-title { padding: 0 4px; }
.card-title label { font-size: 13px; font-weight: 600; color: #ffffff; text-shadow: 0 1px 4px rgba(0,0,0,0.8); }
.card-hit { background: transparent; border: 2px solid transparent; transition: background 150ms ease, border-color 150ms ease; }
.card-hit:hover { background: rgba(255,255,255,0.06); border-color: rgba(255,255,255,0.35); }
button.card-btn { border-radius: 16px; padding: 6px 18px; background: rgba(255,255,255,0.16); color: #ffffff;
                  border: 1px solid rgba(255,255,255,0.18); font-weight: 600; box-shadow: 0 6px 20px rgba(0,0,0,0.25); }
button.card-btn:hover { background: rgba(255,255,255,0.28); }
.card-empty { font-size: 15px; font-weight: 600; color: rgba(255,255,255,0.75); text-shadow: 0 1px 4px rgba(0,0,0,0.8); }
)";

int main() {
    const char* r = getenv("GSK_RENDERER");   // явно GPU: GSK_RENDERER=cairo від доку тут шкодить
    if (r && !strcmp(r, "cairo")) unsetenv("GSK_RENDERER");
    setvbuf(stdout, nullptr, _IONBF, 0);
    gtk_init();
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    rad_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(rad_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

    win = gtk_window_new();
    gtk_widget_add_css_class(win, "cards");
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_namespace(GTK_WINDOW(win), "dock_cards");
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_TOP);
    for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
        gtk_layer_set_anchor(GTK_WINDOW(win), e, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    root = gtk_fixed_new();
    inner = gtk_fixed_new();
    gtk_fixed_put(GTK_FIXED(root), inner, 0, 0);
    gtk_window_set_child(GTK_WINDOW(win), root);
    empty_lbl = gtk_label_new("Простір порожній — сховайте вікно трьома пальцями вниз або Super+Q");
    gtk_widget_add_css_class(empty_lbl, "card-empty");
    gtk_fixed_put(GTK_FIXED(inner), empty_lbl, 0, 0);

    // клік повз картки й кнопки — закрити простір (як тап по порожньому місцю в «Недавніх» Android)
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
    g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick*, int, double x, double y, gpointer) {
        for (GtkWidget* w = gtk_widget_pick(win, x, y, GTK_PICK_DEFAULT); w; w = gtk_widget_get_parent(w))
            if (GTK_IS_BUTTON(w) || gtk_widget_has_css_class(w, "card-hit")) return;
        if (shown && vis_dir >= 0) say("close");
    }), nullptr);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(click));

    // гортання: колесо — по картці на клацання, тачпад — по картці на ~70 px
    GtkEventController* sc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(sc, "scroll", G_CALLBACK(+[](GtkEventControllerScroll* c, double dx, double dy, gpointer) -> gboolean {
        bool wheel = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_WHEEL;
        if (getenv("MGOS_DEBUG")) fprintf(stderr, "scroll dx=%.2f dy=%.2f wheel=%d\n", dx, dy, wheel);
        scroll_acc += (std::abs(dx) > std::abs(dy) ? dx : dy) / (wheel ? 1.0 : 70.0);
        int steps = (int)scroll_acc;
        if (steps) { scroll_acc -= steps; say("scroll " + std::to_string(steps)); }
        return TRUE;
    }), nullptr);
    gtk_widget_add_controller(win, sc);
    g_signal_connect(win, "map", G_CALLBACK(+[](GtkWidget*, gpointer) { input_region(); }), nullptr);

    gtk_widget_realize(win);   // рендерер готовий заздалегідь — перша поява без затримки
    g_unix_fd_add(0, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), on_stdin, nullptr);
    say("ready");
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
}
