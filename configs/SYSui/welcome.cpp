// welcome.cpp — привітання mgOS на першому вході після встановлення (як «Hello» у macOS).
//
//   1. Чорний екран → піксельне сяйво (стиль «Аврора») проявляється дизером згори вниз;
//      «Привіт» різними мовами з'являються й розсипаються пікселями; «Ласкаво просимо до Megumin OS».
//   2. Перше налаштування: Вигляд (світла/темна, акцент) → Wi-Fi → Шпалери (піксельні або з
//      ~/Pictures/wallpapers, вибір одразу видно у фоні) → Робочий стіл (блюр, звуки, док, живлення,
//      шрифт, піксельні віджети) → «Усе готово» (гарячі клавіші). Початкові значення — з settings.json,
//      який інсталятор кладе з mgOS (sync_from_system.sh), тож привітання стартує з тих самих налаштувань.
//   Картка — «скло»: під нею розмита копія фону (зменшення + box blur, малюється в draw()).
//   3. Увесь шар плавно гасне — під ним уже робочий стіл (панелі стартують під привітанням).
//
// Запуск: інсталятор лишає ~/.local/state/mgos-installer/welcome-pending → gui/welcome.sh → цей файл.
//   welcome --preview   — показати будь-коли (Налаштування → Про систему → «Показати привітання»).
// Шар OVERLAY на весь екран, клавіатура ексклюзивна; нічого не пише, поки користувач не вибере.
#include "sysui_common.h"
#include "pixel_waves.h"
#include <pango/pangocairo.h>
#include <functional>
#include <set>
#include <deque>
#include <filesystem>
#include <thread>
namespace fs = std::filesystem;

static pw::Waves g_wv;
static GtkWidget *g_win, *g_area, *g_ui, *g_stack, *g_dots, *g_back, *g_next, *g_card;
static double g_t = 0;            // час з початку, с
static gint64 g_t0 = 0;
static double g_ui_shown = -1;    // коли показали картку налаштування
static double g_leave = -1;       // коли почали гаснути
static bool g_intro_done = false;
static bool g_preview = false;    // --preview: Esc закриває, позначку «показано» не пишемо
static int g_start_page = -1;     // --page N (з --preview): одразу N-й екран налаштування
static std::string g_page = "look";
static const char* PAGES[] = {"look", "wifi", "wallpaper", "desktop", "done"};
static const int NPAGES = 5;
// фон: піксельний стиль (g_wv) або вибрані шпалери-зображення
static GdkTexture* g_bg_tex = nullptr;         // вибрані шпалери-зображення (cover, розмір екрана)
static double g_bg_switch = -10;               // коли змінили фон — проявлення з темряви
static double g_mouse_x = -1e6, g_mouse_y = -1e6;
static std::string g_test_bg;

// ---------- інтро: «Привіт» різними мовами ----------
struct Word { const char* text; const char* font; };
static const Word WORDS[] = {
    {"Привіт", "SF Pro Display Bold"}, {"Hello", "SF Pro Display Bold"}, {"Bonjour", "SF Pro Display Bold"},
    {"Hola", "SF Pro Display Bold"},   {"こんにちは", "Noto Sans CJK JP Bold"}, {"Hallo", "SF Pro Display Bold"},
    {"Ciao", "SF Pro Display Bold"},   {"Cześć", "SF Pro Display Bold"},
};
static const int NW = sizeof(WORDS) / sizeof(WORDS[0]);
static const double FADE_IN = 0.9;      // робочий стіл плавно темніє (а не чорний екран одразу)
static const double INTRO_DARK = 1.2, REVEAL = 1.6, WORD_T = 1.05;   // секунди
static double words_start() { return INTRO_DARK + REVEAL * 0.6; }
static double welcome_start() { return words_start() + NW * WORD_T; }

static const int B8[64] = {0,32,8,40,2,34,10,42,48,16,56,24,50,18,58,26,12,44,4,36,14,46,6,38,60,28,52,20,62,30,54,22,
                           3,35,11,43,1,33,9,41,51,19,59,27,49,17,57,25,15,47,7,39,13,45,5,37,63,31,55,23,61,29,53,21};

// Текст з піксельним розчиненням: alpha<1 → видно лише клітинки, де поріг Байєра < alpha.
// Те саме, але для GPU: напис рендериться раз (кеш), щокадру лише дизер-розчинення маленької
// картинки тексту; сяйво під ним — GSK-тінь, а не 6 проходів маски на процесорі.
struct TextCache { std::string key; cairo_surface_t* s = nullptr; };
static TextCache g_tc[4];
static cairo_surface_t* text_surface(const std::string& text, const char* font, double px, double scale) {
    char k[64]; snprintf(k, sizeof k, "|%s|%.1f|%.3f", font, px, scale);
    std::string key = text + k;
    for (auto& c : g_tc) if (c.s && c.key == key) return c.s;
    static int rr = 0; TextCache& c = g_tc[rr++ % 4];
    if (c.s) cairo_surface_destroy(c.s);
    cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* t = cairo_create(tmp);
    PangoLayout* l = pango_cairo_create_layout(t);
    PangoFontDescription* fd = pango_font_description_from_string(font);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, text.c_str(), -1);
    int tw, th; pango_layout_get_pixel_size(l, &tw, &th);
    int sw = (int)(tw * scale) + 8, sh = (int)(th * scale) + 8;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    cairo_t* cr = cairo_create(s);
    cairo_scale(cr, scale, scale);
    cairo_set_source_rgb(cr, 1, 1, 1);
    pango_cairo_update_layout(cr, l);
    cairo_move_to(cr, 4 / scale, 4 / scale);
    pango_cairo_show_layout(cr, l);
    cairo_destroy(cr);
    pango_font_description_free(fd); g_object_unref(l); cairo_destroy(t); cairo_surface_destroy(tmp);
    c.key = key; c.s = s;
    return s;
}
static void pixel_text_gpu(GtkSnapshot* snap, int W, const std::string& text, const char* font, double px,
                           double alpha, double scale, double cy, double cell);
static double ease(double x) { x = std::clamp(x, 0.0, 1.0); return 1 - std::pow(1 - x, 3); }
static void pixel_text_gpu(GtkSnapshot* snap, int W, const std::string& text, const char* font, double px,
                           double alpha, double scale, double cy, double cell) {
    if (alpha <= 0.001) return;
    // масштаб квантуємо (кеш не роздувається на кожен кадр «наїзду»)
    scale = std::round(scale * 200) / 200;
    cairo_surface_t* src = text_surface(text, font, px, scale);
    int sw = cairo_image_surface_get_width(src), sh = cairo_image_surface_get_height(src), st = cairo_image_surface_get_stride(src);
    cairo_surface_flush(src);
    GBytes* by;
    if (alpha < 0.999) {                                  // піксельне розчинення — на копії
        guchar* d = (guchar*)g_malloc((gsize)st * sh);
        memcpy(d, cairo_image_surface_get_data(src), (size_t)st * sh);
        for (int y = 0; y < sh; y++) {
            uint32_t* row = (uint32_t*)(d + y * st);
            int cy8 = (int)(y / cell) & 7;
            for (int x = 0; x < sw; x++)
                if ((B8[cy8 * 8 + ((int)(x / cell) & 7)] + 0.5) / 64.0 >= alpha) row[x] = 0;
        }
        by = g_bytes_new_take(d, (gsize)st * sh);
    } else by = g_bytes_new(cairo_image_surface_get_data(src), (gsize)st * sh);
    GdkTexture* tex = gdk_memory_texture_new(sw, sh, GDK_MEMORY_DEFAULT, by, st);
    g_bytes_unref(by);
    pw::RGB a = g_wv.accent_now();
    GskShadow sh_{{(float)a.r, (float)a.g, (float)a.b, (float)(0.6 * alpha)}, 0, 0, 7};
    graphene_rect_t r = GRAPHENE_RECT_INIT((float)((W - sw) / 2.0), (float)(cy - sh / 2.0), (float)sw, (float)sh);
    gtk_snapshot_push_shadow(snap, &sh_, 1);
    gtk_snapshot_append_texture(snap, tex, &r);
    gtk_snapshot_pop(snap);
    g_object_unref(tex);
}

// ---------- малювання: усе на GPU ----------
// Раніше кадр (1920×1080) малював процесор через Cairo, а скло під карткою — box blur на CPU.
// Тепер: фон — маленька текстура cols×rows, яку масштабує відеокарта (NEAREST), скло —
// GSK blur-нода над тією ж текстурою, обрізана скругленим прямокутником картки. CPU рахує лише
// сітку пікселів (~60 тис.) і, поки йде інтро, смугу з текстом «Привіт». Без GPU GTK сам
// перейде на програмний рендерер — працює так само, лише повільніше.
G_DECLARE_FINAL_TYPE(WelcomeView, welcome_view, WELCOME, VIEW, GtkWidget)
struct _WelcomeView { GtkWidget parent; };
G_DEFINE_TYPE(WelcomeView, welcome_view, GTK_TYPE_WIDGET)
static void welcome_view_snapshot(GtkWidget* w, GtkSnapshot* snap) {
    int W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
    if (W <= 0 || H <= 0) return;
    graphene_rect_t full = GRAPHENE_RECT_INIT(0, 0, (float)W, (float)H);
    GdkRGBA black = {0, 0, 0, 1};
    gtk_snapshot_append_color(snap, &black, &full);
    // картка — це «вікно» для чорної діри, курсор — теж (привиди лінзуються й засмоктуються)
    graphene_rect_t cb{};
    bool have_card = g_card && gtk_widget_get_visible(g_ui) && gtk_widget_compute_bounds(g_card, w, &cb);
    double wcell = std::max(1, g_wv.cell);
    g_wv.bh_wins.clear();
    if (have_card) g_wv.bh_wins.push_back({(float)(cb.origin.x / wcell), (float)(cb.origin.y / wcell),
                                           (float)((cb.origin.x + cb.size.width) / wcell), (float)((cb.origin.y + cb.size.height) / wcell), true});
    g_wv.bh_cx = g_mouse_x / wcell; g_wv.bh_cy = g_mouse_y / wcell;

    // 1) фон: сяйво / вибраний піксельний стиль / вибрані шпалери
    GdkTexture* tex = nullptr; graphene_rect_t tr = full; GskScalingFilter flt = GSK_SCALING_FILTER_LINEAR;
    double rv = std::clamp((g_t - INTRO_DARK) / REVEAL, 0.0, 1.0);
    if (g_bg_tex) tex = (GdkTexture*)g_object_ref(g_bg_tex);
    else if (rv > 0) {
        g_wv.t = g_t; g_wv.reveal = rv;
        // адаптивна якість: базово ~320 «пікселів» по ширині; якщо кадр рахується довше 5 мс
        // (слабкий ПК, ліміт 8 мс) — пікселі крупніші, поки не вкладемося. Анімація лишається плавною.
        static int extra = 0; static double avg = 0; static int n = 0;
        g_wv.cell = std::max(4, W / 320) + extra;
        gint64 t0 = g_get_monotonic_time();
        g_wv.render(W, H);
        double ms = (g_get_monotonic_time() - t0) / 1000.0;
        avg = n ? avg * 0.9 + ms * 0.1 : ms;
        if (++n > 20 && avg > 8.0 && extra < 6) { extra++; n = 0; }
        if (g_wv.surf) {
            int cw = g_wv.sw, ch = g_wv.sh, st = cairo_image_surface_get_stride(g_wv.surf);
            GBytes* by = g_bytes_new(cairo_image_surface_get_data(g_wv.surf), (gsize)st * ch);
            tex = gdk_memory_texture_new(cw, ch, GDK_MEMORY_DEFAULT, by, st);
            g_bytes_unref(by);
            tr = GRAPHENE_RECT_INIT(0, 0, (float)(cw * g_wv.cell), (float)(ch * g_wv.cell));
            flt = GSK_SCALING_FILTER_NEAREST;
        }
    }
    if (tex) gtk_snapshot_append_scaled_texture(snap, tex, flt, &tr);
    double sw_u = (g_t - g_bg_switch) / 0.7;
    if (sw_u < 1) { GdkRGBA f = {0, 0, 0, (float)(1 - ease(sw_u))}; gtk_snapshot_append_color(snap, &f, &full); }

    // 1б) скло під карткою: та сама текстура, розмита відеокартою
    if (tex && have_card && g_ui_shown >= 0) {
        double a = ease((g_t - g_ui_shown) / 0.7) * (sw_u < 1 ? ease(sw_u) : 1);
        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr, &cb, 26);
        gtk_snapshot_push_rounded_clip(snap, &rr);
        gtk_snapshot_push_opacity(snap, a);
        gtk_snapshot_push_blur(snap, 36);
        gtk_snapshot_append_scaled_texture(snap, tex, GSK_SCALING_FILTER_LINEAR, &tr);
        gtk_snapshot_pop(snap);
        gtk_snapshot_pop(snap);
        gtk_snapshot_pop(snap);
    }
    if (tex) g_object_unref(tex);

    // 2) «Привіт» мовами і 3) «Ласкаво просимо» — лише смуга з текстом, лише під час інтро
    double ws = words_start();
    bool words = g_t >= ws && g_t < welcome_start();
    bool hello = g_t >= welcome_start() && !g_intro_done;
    if (!words && !hello) return;
    double cell = std::max(3.0, W / 480.0);
    if (words) {
        int i = std::min(NW - 1, (int)((g_t - ws) / WORD_T));
        double u = (g_t - ws - i * WORD_T) / WORD_T;      // 0..1 у межах слова
        double a = u < 0.28 ? ease(u / 0.28) : u > 0.72 ? 1 - ease((u - 0.72) / 0.28) : 1;
        double sc = 1.06 - 0.06 * ease(u / 0.5);
        pixel_text_gpu(snap, W, WORDS[i].text, WORDS[i].font, H * 0.13, a, sc, H * 0.45, cell);
    }
    if (hello) {
        double u = (g_t - welcome_start()) / 0.9;
        pixel_text_gpu(snap, W, "Ласкаво просимо", "SF Pro Display Bold", H * 0.095, ease(u), 1.04 - 0.04 * ease(u), H * 0.40, cell);
        pixel_text_gpu(snap, W, "до Megumin OS", "SF Pro Display", H * 0.045, ease(u - 0.35), 1, H * 0.51, cell);
        double hint = 0.55 + 0.45 * std::sin((g_t - welcome_start()) * 2.4);
        if (u > 1.4) pixel_text_gpu(snap, W, "Натисніть Enter або клацніть, щоб почати", "SF Pro Display", H * 0.02,
                                    std::min(1.0, (u - 1.4) * 1.5) * hint, 1, H * 0.80, 2);
    }
}
static void welcome_view_class_init(WelcomeViewClass* k) { GTK_WIDGET_CLASS(k)->snapshot = welcome_view_snapshot; }
static void welcome_view_init(WelcomeView*) {}

// ---------- застосування налаштувань ----------
static void run_bg(const std::string& cmd) {
    g_spawn_command_line_async(("sh -c " + std::string(g_shell_quote(cmd.c_str()))).c_str(), nullptr);
}
static void set_dark(bool dark) {
    run_bg(std::string("gsettings set org.gnome.desktop.interface color-scheme ") + (dark ? "prefer-dark" : "default"));
    sysui::settings_set("dark_theme", dark);
    gtk_widget_remove_css_class(g_ui, dark ? "light" : "dark");
    gtk_widget_add_css_class(g_ui, dark ? "dark" : "light");
}
static void set_accent(const std::string& hex) {
    if (hex.empty()) { sysui::settings_set("accent_mode", "wal"); }
    else { sysui::settings_set("accent_color", hex); sysui::settings_set("accent_mode", "custom"); }
    g_wv.set_accent(sysui::accent());
}

// ---------- навігація ----------
static int page_index() { for (int i = 0; i < NPAGES; i++) if (g_page == PAGES[i]) return i; return 0; }
static void wp_thumbs_start();
static void update_nav() {
    int i = page_index(), k = 0;
    for (GtkWidget* d = gtk_widget_get_first_child(g_dots); d; d = gtk_widget_get_next_sibling(d), k++)
        k == i ? gtk_widget_add_css_class(d, "on") : gtk_widget_remove_css_class(d, "on");
    gtk_widget_set_opacity(g_back, i == 0 ? 0 : 1);
    gtk_widget_set_sensitive(g_back, i > 0);
    gtk_button_set_label(GTK_BUTTON(g_next), i == NPAGES - 1 ? "Почати роботу" : "Далі");
    if (g_page == "wallpaper") wp_thumbs_start();
}
static void finish();
static void go(int dir) {
    int i = page_index() + dir;
    if (i > NPAGES - 1) { finish(); return; }
    if (i < 0) return;
    g_page = PAGES[i];
    gtk_stack_set_transition_type(GTK_STACK(g_stack), dir > 0 ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT : GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT);
    gtk_stack_set_visible_child_name(GTK_STACK(g_stack), g_page.c_str());
    update_nav();
}

static void start_setup() {
    if (g_intro_done) return;
    if (g_t < words_start() + 1.0) return;           // перше слово ще не показали — не пропускаємо зовсім
    g_intro_done = true;
    g_ui_shown = g_t;
    gtk_widget_set_visible(g_ui, TRUE);
    gtk_widget_add_css_class(g_ui, "in");
}

static void finish() {
    if (g_leave >= 0) return;
    g_leave = g_t;
    if (!g_preview) {
        std::string st = sysui::home() + "/.local/state/mgos";
        g_mkdir_with_parents(st.c_str(), 0755);
        { std::ofstream o(st + "/welcome_done"); o << "1"; }
    }
    run_bg(sysui::home() + "/.local/bin/mgos-sound desktop-login");
    // панелі ховаємо, поки шар ще непрозорий; після згасання вони «виїжджають» заново
    // (анімація шарів Hyprland layersIn = slide). Сторожа CC попереджаємо — це не збій.
    std::string d = sysui::home() + "/.config/SYSui";
    run_bg("for m in topbar dock; do echo \"expect|$m\" | socat - UNIX-CLIENT:" + d + "/cc.sock; done 2>/dev/null; "
           "pkill -x topbar; pkill -x dock; true");
}

// ---------- сторінки ----------
static GtkWidget* label(const char* t, const char* cls, float x = 0.5f) {
    GtkWidget* l = gtk_label_new(t); gtk_widget_add_css_class(l, cls); gtk_label_set_xalign(GTK_LABEL(l), x);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE); gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    return l;
}
static GtkWidget* page_box(const char* title, const char* sub) {
    GtkWidget* b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(b), label(title, "w-title"));
    gtk_box_append(GTK_BOX(b), label(sub, "w-sub"));
    return b;
}

static GtkWidget* g_light_card, *g_dark_card;
static GtkWidget* build_look() {
    GtkWidget* b = page_box("Вигляд", "Світле чи темне оформлення і колір акценту. Можна змінити будь-коли в Налаштуваннях.");
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(row, 10);
    auto card = [&](const char* name, bool dark) {
        GtkWidget* btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "theme-card");
        GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget* pv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(pv, dark ? "pv-dark" : "pv-light");
        gtk_widget_set_size_request(pv, 170, 104);
        gtk_box_append(GTK_BOX(v), pv);
        gtk_box_append(GTK_BOX(v), label(name, "w-card-name"));
        gtk_button_set_child(GTK_BUTTON(btn), v);
        g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
            bool dk = GPOINTER_TO_INT(d);
            set_dark(dk);
            gtk_widget_set_state_flags(dk ? g_dark_card : g_light_card, GTK_STATE_FLAG_CHECKED, FALSE);
            gtk_widget_unset_state_flags(dk ? g_light_card : g_dark_card, GTK_STATE_FLAG_CHECKED);
        }), GINT_TO_POINTER(dark));
        gtk_box_append(GTK_BOX(row), btn);
        return btn;
    };
    g_light_card = card("Світла", false);
    g_dark_card = card("Темна", true);
    bool dark = sysui::color_scheme() == "prefer-dark";
    gtk_widget_set_state_flags(dark ? g_dark_card : g_light_card, GTK_STATE_FLAG_CHECKED, FALSE);
    gtk_box_append(GTK_BOX(b), row);

    GtkWidget* acc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(acc, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(acc, 12);
    static const char* COLS[] = {"", "#0a84ff", "#5e5ce6", "#a550a7", "#f74f9e", "#ff5257", "#f7821b", "#ffc600", "#62ba46"};
    // підпис під кружечками: що робить вибраний (або той, над яким курсор)
    static const char* CAPS[] = {
        "Зі шпалер — акцент підбирається автоматично під кожні нові шпалери",
        "Синій — сталий акцент, не залежить від шпалер", "Індиго — сталий акцент, не залежить від шпалер",
        "Фіолетовий — сталий акцент, не залежить від шпалер", "Рожевий — сталий акцент, не залежить від шпалер",
        "Червоний — сталий акцент, не залежить від шпалер", "Помаранчевий — сталий акцент, не залежить від шпалер",
        "Жовтий — сталий акцент, не залежить від шпалер", "Зелений — сталий акцент, не залежить від шпалер"};
    static GtkWidget* cap; cap = label("", "acc-cap");
    static int cap_sel = 0;
    for (int ci = 0; ci < 9; ci++) {
        const char* c = COLS[ci];
        GtkWidget* sw = gtk_button_new();
        gtk_widget_add_css_class(sw, "swatch");
        gtk_widget_set_tooltip_text(sw, CAPS[ci]);
        g_object_set_data(G_OBJECT(sw), "ci", GINT_TO_POINTER(ci));
        GtkEventController* hv = gtk_event_controller_motion_new();
        g_signal_connect(hv, "enter", G_CALLBACK(+[](GtkEventControllerMotion*, double, double, gpointer d) {
            gtk_label_set_text(GTK_LABEL(cap), CAPS[GPOINTER_TO_INT(d)]); }), GINT_TO_POINTER(ci));
        g_signal_connect(hv, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer) {
            gtk_label_set_text(GTK_LABEL(cap), CAPS[cap_sel]); }), nullptr);
        gtk_widget_add_controller(sw, hv);
        if (!*c) { gtk_widget_add_css_class(sw, "swatch-wal"); }
        else {
            GtkCssProvider* p = gtk_css_provider_new();
            std::string css = std::string("button { background: ") + c + "; }";
            gtk_css_provider_load_from_string(p, css.c_str());
            gtk_style_context_add_provider(gtk_widget_get_style_context(sw), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
            g_object_unref(p);
        }
        // початковий вибір — як у settings.json (його кладе інсталятор)
        {
            const auto& st = sysui::settings();
            bool on = !*c ? st.accent_mode != "custom" : (st.accent_mode == "custom" && g_ascii_strcasecmp(st.accent_color.c_str(), c) == 0);
            if (on) { gtk_widget_set_state_flags(sw, GTK_STATE_FLAG_CHECKED, FALSE); cap_sel = ci; }
        }
        g_signal_connect(sw, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer d) {
            for (GtkWidget* s = gtk_widget_get_first_child(gtk_widget_get_parent(GTK_WIDGET(b))); s; s = gtk_widget_get_next_sibling(s))
                gtk_widget_unset_state_flags(s, GTK_STATE_FLAG_CHECKED);
            gtk_widget_set_state_flags(GTK_WIDGET(b), GTK_STATE_FLAG_CHECKED, FALSE);
            cap_sel = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "ci"));
            gtk_label_set_text(GTK_LABEL(cap), CAPS[cap_sel]);
            set_accent((const char*)d);
        }), (gpointer)c);
        gtk_box_append(GTK_BOX(acc), sw);
    }
    gtk_box_append(GTK_BOX(b), acc);
    gtk_label_set_text(GTK_LABEL(cap), CAPS[cap_sel]);
    gtk_box_append(GTK_BOX(b), cap);
    return b;
}

// Wi-Fi: список мереж (nmcli), клік → пароль → підключення
static GtkWidget *g_wifi_list, *g_wifi_status;
static void wifi_scan();
static void sh_async(const std::string& cmd, std::function<void(const std::string&, bool)> cb) {
    GSubprocess* p = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE),
                                      nullptr, "sh", "-c", cmd.c_str(), nullptr);
    if (!p) { cb("", false); return; }
    auto* f = new std::function<void(const std::string&, bool)>(std::move(cb));
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, [](GObject* s, GAsyncResult* r, gpointer d) {
        auto* f = (std::function<void(const std::string&, bool)>*)d;
        char* out = nullptr;
        gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(s), r, &out, nullptr, nullptr);
        bool good = ok && g_subprocess_get_if_exited(G_SUBPROCESS(s)) && g_subprocess_get_exit_status(G_SUBPROCESS(s)) == 0;
        (*f)(out ? out : "", good);
        g_free(out); g_object_unref(s); delete f;
    }, f);
}
static void wifi_connect(const std::string& ssid, const std::string& pass) {
    gtk_label_set_text(GTK_LABEL(g_wifi_status), ("Підключення до «" + ssid + "»…").c_str());
    char* qs = g_shell_quote(ssid.c_str()); char* qp = g_shell_quote(pass.c_str());
    std::string cmd = "timeout 30 nmcli dev wifi connect " + std::string(qs) + (pass.empty() ? "" : " password " + std::string(qp));
    g_free(qs); g_free(qp);
    sh_async(cmd, [ssid](const std::string&, bool ok) {
        gtk_label_set_text(GTK_LABEL(g_wifi_status), ok ? ("Підключено до «" + ssid + "»").c_str() : "Не вдалося — перевірте пароль");
        if (ok) wifi_scan();
    });
}
static void wifi_scan() {
    sh_async("nmcli radio wifi on >/dev/null 2>&1; timeout 12 nmcli -t -f IN-USE,SIGNAL,SECURITY,SSID dev wifi list --rescan auto",
             [](const std::string& out, bool) {
        while (GtkWidget* c = gtk_widget_get_first_child(g_wifi_list)) gtk_widget_unparent(c);
        std::istringstream ss(out); std::string line; std::set<std::string> seen; int n = 0;
        while (std::getline(ss, line) && n < 7) {
            // IN-USE:SIGNAL:SECURITY:SSID (у SSID двокрапки екрановані \:)
            std::vector<std::string> f; std::string cur;
            for (size_t i = 0; i < line.size(); i++) {
                if (line[i] == '\\' && i + 1 < line.size()) { cur += line[++i]; continue; }
                if (line[i] == ':' && f.size() < 3) { f.push_back(cur); cur.clear(); continue; }
                cur += line[i];
            }
            f.push_back(cur);
            if (f.size() < 4 || f[3].empty() || seen.count(f[3])) continue;
            seen.insert(f[3]); n++;
            bool used = f[0] == "*", sec = !f[2].empty() && f[2] != "--";
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            GtkWidget* btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "wifi-row");
            if (used) gtk_widget_add_css_class(btn, "active");
            GtkWidget* h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            int sig = atoi(f[1].c_str());
            const char* ic = sig > 66 ? "network-wireless-signal-excellent-symbolic" : sig > 40 ? "network-wireless-signal-good-symbolic"
                           : "network-wireless-signal-weak-symbolic";
            gtk_box_append(GTK_BOX(h), gtk_image_new_from_icon_name(ic));
            GtkWidget* nm = label(f[3].c_str(), "w-row", 0); gtk_widget_set_hexpand(nm, TRUE);
            gtk_box_append(GTK_BOX(h), nm);
            if (used) gtk_box_append(GTK_BOX(h), label("Підключено", "w-row-sub", 1));
            else if (sec) gtk_box_append(GTK_BOX(h), gtk_image_new_from_icon_name("changes-prevent-symbolic"));
            gtk_button_set_child(GTK_BUTTON(btn), h);
            GtkWidget* rev = gtk_revealer_new();
            GtkWidget* pr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget* en = gtk_password_entry_new();
            gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(en), TRUE);
            gtk_widget_set_hexpand(en, TRUE);
            GtkWidget* go_b = gtk_button_new_with_label("Підключити");
            gtk_widget_add_css_class(go_b, "primary");
            gtk_box_append(GTK_BOX(pr), en); gtk_box_append(GTK_BOX(pr), go_b);
            gtk_revealer_set_child(GTK_REVEALER(rev), pr);
            gtk_box_append(GTK_BOX(row), btn); gtk_box_append(GTK_BOX(row), rev);
            g_object_set_data_full(G_OBJECT(btn), "ssid", g_strdup(f[3].c_str()), g_free);
            g_object_set_data(G_OBJECT(btn), "rev", rev);
            g_object_set_data(G_OBJECT(btn), "sec", GINT_TO_POINTER(sec && !used));
            g_object_set_data(G_OBJECT(go_b), "entry", en);
            g_object_set_data_full(G_OBJECT(go_b), "ssid", g_strdup(f[3].c_str()), g_free);
            g_object_set_data_full(G_OBJECT(en), "ssid", g_strdup(f[3].c_str()), g_free);
            g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
                std::string ssid = (const char*)g_object_get_data(G_OBJECT(b), "ssid");
                if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "sec"))) {
                    GtkWidget* r = (GtkWidget*)g_object_get_data(G_OBJECT(b), "rev");
                    gtk_revealer_set_reveal_child(GTK_REVEALER(r), !gtk_revealer_get_reveal_child(GTK_REVEALER(r)));
                } else wifi_connect(ssid, "");
            }), nullptr);
            auto submit = +[](GtkWidget* w, gpointer) {
                GtkWidget* e = (GtkWidget*)g_object_get_data(G_OBJECT(w), "entry");
                if (!e) e = w;
                wifi_connect((const char*)g_object_get_data(G_OBJECT(w), "ssid"), gtk_editable_get_text(GTK_EDITABLE(e)));
            };
            g_signal_connect(go_b, "clicked", G_CALLBACK(submit), nullptr);
            g_signal_connect(en, "activate", G_CALLBACK(submit), nullptr);
            gtk_box_append(GTK_BOX(g_wifi_list), row);
        }
        if (!n) gtk_box_append(GTK_BOX(g_wifi_list), label("Мереж не знайдено", "w-sub"));
    });
}
static GtkWidget* build_wifi() {
    GtkWidget* b = page_box("Підключення до мережі", "Виберіть Wi-Fi. Можна пропустити — дротова мережа підключається сама.");
    g_wifi_status = label("Пошук мереж…", "w-row-sub");
    gtk_box_append(GTK_BOX(b), g_wifi_status);
    g_wifi_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(g_wifi_list, "w-list");
    gtk_widget_set_size_request(g_wifi_list, 460, -1);
    gtk_widget_set_halign(g_wifi_list, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(b), g_wifi_list);
    return b;
}

// ---------- Шпалери: живі піксельні або зображення/відео з ~/Pictures/wallpapers ----------
// Вибір одразу застосовується до системи і показується фоном привітання.
static std::string wp_current() {
    std::ifstream f(sysui::home() + "/.cache/ignis/last_wallpaper.txt");
    std::string s; std::getline(f, s); return s;
}
static bool is_video(const std::string& p) {
    std::string e = p.substr(p.find_last_of('.') + 1);
    for (auto& c : e) c = g_ascii_tolower(c);
    return e == "mp4" || e == "webm" || e == "gif" || e == "mkv" || e == "mov";
}
// фон із зображення: декодуємо в окремому потоці (4K jpeg ~0.1–0.3 с), на екран — у головному
static gint64 g_bg_req = 0;
static void set_bg_image(const std::string& src) {
    int W = gtk_widget_get_width(g_area), H = gtk_widget_get_height(g_area);
    gint64 req = ++g_bg_req;
    std::thread([src, W, H, req]() {
        std::string path = src;
        if (fs::is_directory(path)) { path = src + "/day.jpg"; if (!fs::exists(path)) path = src + "/day.png"; }
        if (is_video(path)) {   // перший кадр відео
            std::string out = sysui::home() + "/.cache/megumin-os/welcome_frame.png";
            char* q = g_shell_quote(path.c_str()); char* qo = g_shell_quote(out.c_str());
            std::string cmd = std::string("ffmpeg -loglevel error -y -i ") + q + " -frames:v 1 " + qo + " >/dev/null 2>&1";
            g_free(q); g_free(qo);
            if (system(cmd.c_str()) != 0) return;
            path = out;
        }
        int fw = 0, fh = 0;
        if (!gdk_pixbuf_get_file_info(path.c_str(), &fw, &fh) || fw <= 0) return;
        double sc = std::max((double)W / fw, (double)H / fh);
        GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), std::max(W, (int)(fw * sc + 0.5)), std::max(H, (int)(fh * sc + 0.5)), FALSE, nullptr);
        if (!pb) return;
        // pixbuf (RGB/RGBA) → cairo ARGB32, з обрізанням по центру
        // обрізаємо по центру до розміру екрана; текстуру зробить головний потік
        int pw_ = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb);
        GdkPixbuf* crop = gdk_pixbuf_new_subpixbuf(pb, std::max(0, (pw_ - W) / 2), std::max(0, (ph - H) / 2), std::min(W, pw_), std::min(H, ph));
        GdkPixbuf* own = gdk_pixbuf_copy(crop);
        g_object_unref(crop); g_object_unref(pb);
        struct R { GdkPixbuf* pb; gint64 req; };
        g_idle_add([](gpointer d) -> gboolean {
            R* r = (R*)d;
            if (r->req == g_bg_req) {
                GdkPixbuf* p = r->pb;
                GBytes* by = gdk_pixbuf_read_pixel_bytes(p);
                GdkTexture* t = gdk_memory_texture_new(gdk_pixbuf_get_width(p), gdk_pixbuf_get_height(p),
                                                       gdk_pixbuf_get_has_alpha(p) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                       by, gdk_pixbuf_get_rowstride(p));
                g_bytes_unref(by);
                if (g_bg_tex) g_object_unref(g_bg_tex);
                g_bg_tex = t; g_bg_switch = g_t;
            }
            g_object_unref(r->pb); delete r; return G_SOURCE_REMOVE;
        }, new R{own, req});
    }).detach();
}
static void set_bg_pixel(const std::string& style) {
    ++g_bg_req;
    if (g_bg_tex) { g_object_unref(g_bg_tex); g_bg_tex = nullptr; }
    g_wv.style = style; g_wv.reveal = 1; g_bg_switch = g_t;
}
static void wp_select(GtkWidget* card) {
    // знімаємо позначку з усіх карток обох вкладок
    GtkWidget* root = (GtkWidget*)g_object_get_data(G_OBJECT(card), "root");
    std::vector<GtkWidget*> todo{root};
    while (!todo.empty()) {
        GtkWidget* w = todo.back(); todo.pop_back();
        if (gtk_widget_has_css_class(w, "theme-card")) gtk_widget_unset_state_flags(w, GTK_STATE_FLAG_CHECKED);
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) todo.push_back(c);
    }
    gtk_widget_set_state_flags(card, GTK_STATE_FLAG_CHECKED, FALSE);
}
static GtkWidget* wp_card(GtkWidget* root, const std::string& name, const std::string& id, bool checked, GtkWidget** pic_out) {
    GtkWidget* btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "theme-card");
    gtk_widget_add_css_class(btn, "wp-card");
    GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
    gtk_widget_set_size_request(pic, 150, 84);
    gtk_widget_add_css_class(pic, "wp-pic");
    gtk_box_append(GTK_BOX(v), pic);
    GtkWidget* l = label(name.c_str(), "w-card-name");
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 18);
    gtk_label_set_wrap(GTK_LABEL(l), FALSE);
    gtk_box_append(GTK_BOX(v), l);
    gtk_button_set_child(GTK_BUTTON(btn), v);
    g_object_set_data(G_OBJECT(btn), "root", root);
    g_object_set_data_full(G_OBJECT(btn), "wp-id", g_strdup(id.c_str()), g_free);
    if (checked) gtk_widget_set_state_flags(btn, GTK_STATE_FLAG_CHECKED, FALSE);
    g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
        wp_select(GTK_WIDGET(b));
        std::string id = (const char*)g_object_get_data(G_OBJECT(b), "wp-id");
        if (id.rfind("pixel:", 0) == 0) {
            set_bg_pixel(id.substr(6));
            run_bg(sysui::home() + "/.config/SYSui/pixel_wallpaper --set " + id.substr(6));
        } else {
            set_bg_image(id);
            // застосовує демон шпалер (той самий шлях, що й Super+W: awww/mpvpaper, pywal, SDDM)
            char* q = g_shell_quote(("set " + id).c_str());
            run_bg(std::string("printf '%s' ") + q + " | socat - UNIX-CONNECT:" + sysui::home() + "/.config/SYSui/wallpapers.sock");
            g_free(q);
        }
    }), nullptr);
    *pic_out = pic;
    return btn;
}
// Мініатюри: декодування — у фоновому потоці, у GTK — лише готова маленька текстура.
// Черги дві: піксельні прев'ю (коли відкрили сторінку) і зображення (коли відкрили їхню вкладку),
// обробка по одній — інтерфейс не замерзає навіть на слабкому ПК.
// Ім'я мініатюри — як у wallpapers.cpp (std::hash шляху), тож кеш спільний із Super+W.
struct ThumbJob { std::string path; GtkWidget* pic; };
static std::deque<ThumbJob> g_pix_q, g_img_q;
static bool g_pix_on = false, g_img_on = false, g_thumb_busy = false;
static void thumb_next();
static void pic_load_async(GtkWidget* pic, const std::string& f) {   // забирає посилання на pic
    std::thread([pic, f]() {
        GdkPixbuf* pb = nullptr;
        if (!f.empty() && access(f.c_str(), R_OK) == 0) {
            int fw = 0, fh = 0;
            if (gdk_pixbuf_get_file_info(f.c_str(), &fw, &fh) && fw > 0) {
                double sc = std::max(300.0 / fw, 168.0 / fh);
                GdkPixbuf* s = gdk_pixbuf_new_from_file_at_scale(f.c_str(), std::max(300, (int)(fw * sc + .5)), std::max(168, (int)(fh * sc + .5)), FALSE, nullptr);
                if (s) {
                    int w = gdk_pixbuf_get_width(s), h = gdk_pixbuf_get_height(s);
                    GdkPixbuf* sub = gdk_pixbuf_new_subpixbuf(s, std::max(0, (w - 300) / 2), std::max(0, (h - 168) / 2), std::min(300, w), std::min(168, h));
                    pb = gdk_pixbuf_copy(sub); g_object_unref(sub); g_object_unref(s);
                }
            }
        }
        struct R { GtkWidget* pic; GdkPixbuf* pb; };
        g_idle_add([](gpointer d) -> gboolean {
            R* r = (R*)d;
            if (r->pb) {
                GBytes* by = gdk_pixbuf_read_pixel_bytes(r->pb);
                GdkTexture* t = gdk_memory_texture_new(gdk_pixbuf_get_width(r->pb), gdk_pixbuf_get_height(r->pb),
                                                       gdk_pixbuf_get_has_alpha(r->pb) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                       by, gdk_pixbuf_get_rowstride(r->pb));
                g_bytes_unref(by);
                gtk_picture_set_paintable(GTK_PICTURE(r->pic), GDK_PAINTABLE(t));
                g_object_unref(t); g_object_unref(r->pb);
            }
            g_object_unref(r->pic); delete r;
            g_thumb_busy = false;
            thumb_next();
            return G_SOURCE_REMOVE;
        }, new R{pic, pb});
    }).detach();
}
static void thumb_next() {
    if (g_thumb_busy) return;
    std::deque<ThumbJob>* q = g_pix_on && !g_pix_q.empty() ? &g_pix_q : g_img_on && !g_img_q.empty() ? &g_img_q : nullptr;
    if (!q) return;
    ThumbJob j = q->front(); q->pop_front();
    g_thumb_busy = true;
    std::string f, cmd;
    if (j.path.rfind("pixel:", 0) == 0) {   // прев'ю піксельного стилю — один кадр від pixel_wallpaper
        std::string id = j.path.substr(6);
        f = sysui::home() + "/.cache/megumin-os/pixel_previews/welcome_" + id + ".png";
        cmd = sysui::home() + "/.config/SYSui/pixel_wallpaper --poster '" + f + "' 384x216 --style " + id;
    } else {
        std::string cache = sysui::home() + "/.cache/ignis/wallpaper_thumbs", src = j.path;
        if (fs::is_directory(src)) { src = j.path + "/day.jpg"; if (!fs::exists(src)) src = j.path + "/day.png"; }
        bool vid = is_video(src);
        f = cache + "/" + std::to_string(std::hash<std::string>{}(j.path)) + (vid ? "_c.png" : ".png");
        g_mkdir_with_parents(cache.c_str(), 0755);
        char* qs = g_shell_quote(src.c_str()); char* qt = g_shell_quote(f.c_str());
        cmd = vid ? std::string("ffmpeg -loglevel error -i ") + qs + " -frames:v 1 -vf 'scale=350:350:force_original_aspect_ratio=increase,crop=350:350' " + qt + " -y"
                  : std::string("magick ") + qs + "[0] -resize 350x350^ -gravity center -extent 350x350 " + qt;
        g_free(qs); g_free(qt);
    }
    if (access(f.c_str(), R_OK) == 0) { pic_load_async(j.pic, f); return; }
    sh_async("nice -n 10 " + cmd, [j, f](const std::string&, bool) { pic_load_async(j.pic, f); });
}
static void wp_thumbs_start() { if (!g_pix_on) { g_pix_on = true; thumb_next(); } }
static void img_thumbs_start() { if (!g_img_on) { g_img_on = true; thumb_next(); } }
static GtkWidget* build_wallpaper() {
    GtkWidget* b = page_box("Шпалери", "Живі піксельні підлаштовуються під колір акценту. Вибране одразу видно у фоні.");
    std::string cur = wp_current();
    // перемикач вкладок
    GtkWidget* seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(seg, "seg");
    gtk_widget_set_halign(seg, GTK_ALIGN_CENTER);
    GtkWidget* st = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(st), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    auto tab = [&](const char* name, const char* id, bool on) {
        GtkWidget* t = gtk_toggle_button_new_with_label(name);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), on);
        g_object_set_data(G_OBJECT(t), "stack", st);
        g_object_set_data(G_OBJECT(t), "page", (gpointer)id);
        g_signal_connect(t, "toggled", G_CALLBACK(+[](GtkToggleButton* t, gpointer) {
            if (gtk_toggle_button_get_active(t) && !strcmp((const char*)g_object_get_data(G_OBJECT(t), "page"), "images")) img_thumbs_start();
            if (gtk_toggle_button_get_active(t))
                gtk_stack_set_visible_child_name(GTK_STACK(g_object_get_data(G_OBJECT(t), "stack")), (const char*)g_object_get_data(G_OBJECT(t), "page"));
        }), nullptr);
        gtk_box_append(GTK_BOX(seg), t);
        return t;
    };
    // на чистому ПК картинок немає — система пропонує живі піксельні; вкладка зображень — якщо вони є
    bool pix_now = true;
    GtkWidget* t1 = tab("Живі піксельні", "pixel", pix_now);
    GtkWidget* t2 = tab("Зображення і відео", "images", !pix_now);
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(t2), GTK_TOGGLE_BUTTON(t1));
    gtk_box_append(GTK_BOX(b), seg);

    auto flow = [](void) {
        GtkWidget* f = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(f), GTK_SELECTION_NONE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(f), 4);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(f), 4);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(f), TRUE);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(f), 4); gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(f), 4);
        GtkWidget* sc = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), f);
        gtk_widget_set_size_request(sc, 700, 290);
        return std::make_pair(sc, f);
    };
    // піксельні
    auto [sc1, f1] = flow();
    std::string cache = sysui::home() + "/.cache/megumin-os/pixel_previews";
    g_mkdir_with_parents(cache.c_str(), 0755);
    struct Opt { const char* id; const char* name; };
    static const Opt OPTS[] = {{"aurora", "Аврора"}, {"neoncity", "Неонове місто"}, {"rooftop", "Дах"}, {"tower", "Вежа"}, {"blackhole", "Чорна діра"}, {"horizon", "Захід сонця"}, {"waves", "Хвилі"},
                               {"rain", "Піксельний дощ"}, {"eq", "Еквалайзер"}, {"virus", "Вірус"}, {"life", "Гра «Життя»"},
                               {"riftportal", "Розлом"}, {"voidfield", "Зоряна безодня"}, {"skyline", "Skyline"}, {"torus", "Бублик"}, {"knot", "Вузол"}, {"clouds", "Хмари"}};
    for (auto& o : OPTS) {
        GtkWidget* pic;
        GtkWidget* c = wp_card(b, o.name, std::string("pixel:") + o.id, cur == std::string("pixel:") + o.id, &pic);
        gtk_flow_box_append(GTK_FLOW_BOX(f1), c);
        g_object_ref(pic);
        g_pix_q.push_back({std::string("pixel:") + o.id, pic});   // прев'ю — у черзі, коли відкриють сторінку
    }
    gtk_stack_add_named(GTK_STACK(st), sc1, "pixel");
    // зображення і відео
    auto [sc2, f2] = flow();
    std::vector<std::string> files;
    std::string dir = sysui::home() + "/Pictures/wallpapers";
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string p = e.path().string(), x = p.substr(p.find_last_of('.') + 1);
        for (auto& c : x) c = g_ascii_tolower(c);
        bool img = x == "png" || x == "jpg" || x == "jpeg" || x == "webp";
        if (img || is_video(p) || (e.is_directory() && (fs::exists(p + "/day.jpg") || fs::exists(p + "/day.png")))) files.push_back(p);
    }
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    for (auto& p : files) {
        std::string nm = fs::path(p).stem().string();
        if (is_video(p)) nm = "▶ " + nm;
        GtkWidget* pic;
        GtkWidget* c = wp_card(b, nm, p, p == cur, &pic);
        gtk_flow_box_append(GTK_FLOW_BOX(f2), c);
        g_object_ref(pic);
        g_img_q.push_back({p, pic});
    }
    if (files.empty()) gtk_widget_set_visible(seg, FALSE);
    gtk_stack_add_named(GTK_STACK(st), sc2, "images");
    gtk_stack_set_visible_child_name(GTK_STACK(st), pix_now ? "pixel" : "images");
    gtk_box_append(GTK_BOX(b), st);
    return b;
}

// ---------- Робочий стіл: те, що найчастіше міняють у Налаштуваннях ----------
// Значення — з settings.json; застосування — `settings.py --apply ключ` (блюр-конфіг, звуки, шрифт, живлення
// робить той самий код, що й у Налаштуваннях), панелі й док підхоплюють settings.json самі.
static nlohmann::json g_cfg;
static nlohmann::json cfg_get(const char* k, nlohmann::json d) {
    return g_cfg.is_object() && g_cfg.contains(k) ? g_cfg[k] : d;
}
static void cfg_set(const std::string& k, const nlohmann::json& v, bool apply) {
    sysui::settings_set(k, v);
    if (apply) run_bg("python3 " + sysui::home() + "/.config/ignis/settings.py --apply " + k);
}
static GtkWidget* opt_row(const char* title, const char* sub, GtkWidget* ctl) {
    GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(r, "opt-row");
    GtkWidget* tv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(tv, TRUE);
    gtk_widget_set_valign(tv, GTK_ALIGN_CENTER);
    GtkWidget* t = label(title, "w-row", 0); gtk_label_set_wrap(GTK_LABEL(t), FALSE); gtk_label_set_justify(GTK_LABEL(t), GTK_JUSTIFY_LEFT);
    gtk_box_append(GTK_BOX(tv), t);
    if (sub && *sub) { GtkWidget* s = label(sub, "w-row-sub", 0); gtk_label_set_justify(GTK_LABEL(s), GTK_JUSTIFY_LEFT); gtk_box_append(GTK_BOX(tv), s); }
    gtk_box_append(GTK_BOX(r), tv);
    gtk_widget_set_valign(ctl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(r), ctl);
    return r;
}
static GtkWidget* sw_ctl(const char* key, bool def, bool apply) {
    GtkWidget* w = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(w), cfg_get(key, def).is_boolean() ? cfg_get(key, def).get<bool>() : def);
    g_object_set_data(G_OBJECT(w), "key", (gpointer)key);
    g_object_set_data(G_OBJECT(w), "apply", GINT_TO_POINTER(apply));
    g_signal_connect(w, "notify::active", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        cfg_set((const char*)g_object_get_data(o, "key"), (bool)gtk_switch_get_active(GTK_SWITCH(o)), GPOINTER_TO_INT(g_object_get_data(o, "apply")));
    }), nullptr);
    return w;
}
struct Choice { const char* id; const char* name; };
static GtkWidget* drop_ctl(const char* key, std::vector<Choice> opts, const char* def, bool apply) {
    std::vector<const char*> names; for (auto& o : opts) names.push_back(o.name); names.push_back(nullptr);
    GtkWidget* d = gtk_drop_down_new_from_strings(names.data());
    std::string cur = cfg_get(key, def).is_string() ? cfg_get(key, def).get<std::string>() : def;
    for (size_t i = 0; i < opts.size(); i++) if (cur == opts[i].id) gtk_drop_down_set_selected(GTK_DROP_DOWN(d), i);
    auto* ids = new std::vector<std::string>(); for (auto& o : opts) ids->push_back(o.id);
    g_object_set_data_full(G_OBJECT(d), "ids", ids, [](gpointer p) { delete (std::vector<std::string>*)p; });
    g_object_set_data(G_OBJECT(d), "key", (gpointer)key);
    g_object_set_data(G_OBJECT(d), "apply", GINT_TO_POINTER(apply));
    g_signal_connect(d, "notify::selected", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        auto* ids = (std::vector<std::string>*)g_object_get_data(o, "ids");
        guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
        if (i >= ids->size()) return;
        std::string key = (const char*)g_object_get_data(o, "key");
        cfg_set(key, (*ids)[i], GPOINTER_TO_INT(g_object_get_data(o, "apply")));
        if (key == "ui_font") sysui::settings_set("panel_font", (*ids)[i]);   // панелі — тим самим шрифтом
    }), nullptr);
    gtk_widget_set_size_request(d, 190, -1);
    return d;
}
static GtkWidget* build_desktop() {
    try { std::ifstream f(sysui::settings_path()); f >> g_cfg; } catch (...) { g_cfg = nlohmann::json::object(); }
    GtkWidget* b = page_box("Робочий стіл", "Найпотрібніше з Налаштувань. Усе інше — Super + U.");
    GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(list, "opt-list");
    gtk_widget_set_size_request(list, 600, -1);
    gtk_widget_set_halign(list, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(list), opt_row("Розмиття і скло", "Скляні панелі та вікна; без нього — економніше", sw_ctl("hyprland_blur", true, true)));
    gtk_box_append(GTK_BOX(list), opt_row("Запечений блюр", "Віджети малюють розмиті шпалери самі — те саме скло, але майже без навантаження на GPU", sw_ctl("baked_blur", true, false)));
    gtk_box_append(GTK_BOX(list), opt_row("Системні звуки", "Вхід, сповіщення, знімки екрана", sw_ctl("sounds_enabled", true, true)));
    gtk_box_append(GTK_BOX(list), opt_row("Збільшення іконок доку", "Іконки ростуть під курсором", sw_ctl("dock_magnify", true, false)));
    gtk_box_append(GTK_BOX(list), opt_row("Кнопка вимкнення у верхній панелі", "Меню живлення лишається на Super + O", sw_ctl("topbar_power", true, false)));
    gtk_box_append(GTK_BOX(list), opt_row("Режим живлення", "",
        drop_ctl("power_profile", {{"power-saver", "Економ"}, {"balanced", "Баланс"}, {"performance", "Максимум"}}, "balanced", true)));
    gtk_box_append(GTK_BOX(list), opt_row("Анімація піксельних віджетів", "Шапки «Про систему», Control Center",
        drop_ctl("pixel_style", {{"waves", "Хвилі"}, {"aurora", "Аврора"}, {"horizon", "Захід сонця"}, {"virus", "Вірус"},
                                 {"eq", "Еквалайзер"}, {"rain", "Піксельний дощ"}, {"life", "Гра «Життя»"},
                                 {"lava", "Плазма-лампа"}, {"abyss", "Біолюмінесценція"}, {"skyline", "Skyline"}}, "waves", false)));
    // шрифти — лише встановлені
    std::vector<Choice> fonts;
    {
        static std::vector<std::string> keep;
        PangoFontMap* fm = pango_cairo_font_map_get_default();
        PangoFontFamily** fams = nullptr; int n = 0;
        pango_font_map_list_families(fm, &fams, &n);
        std::set<std::string> have;
        for (int i = 0; i < n; i++) have.insert(pango_font_family_get_name(fams[i]));
        g_free(fams);
        for (const char* f : {"SF Pro Display", "Inter", "Noto Sans", "Cantarell", "Ubuntu", "JetBrains Mono"})
            if (have.count(f)) keep.push_back(f);
        for (auto& k : keep) fonts.push_back({k.c_str(), k.c_str()});
    }
    if (!fonts.empty()) {
        std::string def = cfg_get("ui_font", cfg_get("panel_font", "SF Pro Display")).get<std::string>();
        static std::string def_s; def_s = def;
        gtk_box_append(GTK_BOX(list), opt_row("Шрифт інтерфейсу", "Панелі та програми", drop_ctl("ui_font", fonts, def_s.c_str(), true)));
    }
    gtk_box_append(GTK_BOX(b), list);
    return b;
}

static GtkWidget* build_done() {
    GtkWidget* b = page_box("Усе готово", "Megumin OS налаштовано. Док — унизу, Control Center — клік по батареї.\n"
                                          "Кілька гарячих клавіш на початок:");
    GtkWidget* keys = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(keys), 8); gtk_grid_set_column_spacing(GTK_GRID(keys), 18);
    gtk_widget_set_halign(keys, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(keys, 10);
    gtk_grid_set_column_spacing(GTK_GRID(keys), 14);
    // ті самі бінди, що в hyprland.conf mgOS
    static const char* K[][2] = {{"Alt + Space", "Пошук програм"}, {"Super + U", "Налаштування"},
                                 {"Super + K", "Термінал"},        {"Super + Q", "Закрити вікно"},
                                 {"Super + V", "Буфер обміну"},    {"Super + W", "Шпалери"},
                                 {"Super + Z", "Control Center"},  {"Super + O", "Меню живлення"},
                                 {"Alt + Tab", "Перемикання вікон"}, {"Super + Shift + S", "Знімок області"},
                                 {"Super + F", "На весь екран"},   {"3 пальці", "Робочі столи"}};
    int r = 0;
    for (auto& k : K) {
        int col = (r % 2) * 3;
        GtkWidget* kc = label(k[0], "keycap", 0.5f); gtk_widget_set_halign(kc, GTK_ALIGN_END); gtk_label_set_wrap(GTK_LABEL(kc), FALSE);
        gtk_grid_attach(GTK_GRID(keys), kc, col, r / 2, 1, 1);
        GtkWidget* d = label(k[1], "w-row", 0); gtk_label_set_wrap(GTK_LABEL(d), FALSE);
        gtk_grid_attach(GTK_GRID(keys), d, col + 1, r / 2, 1, 1);
        if (r % 2 == 0) { GtkWidget* sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_set_size_request(sp, 20, -1); gtk_grid_attach(GTK_GRID(keys), sp, 2, r / 2, 1, 1); }
        r++;
    }
    gtk_box_append(GTK_BOX(b), keys);
    return b;
}

static const char* CSS = R"(
window { background: transparent; }
.w-ui { opacity: 0; transition: opacity 700ms ease; }
.w-ui.in { opacity: 1; }
.w-card { background: rgba(14,14,20,0.50); border-radius: 26px; padding: 34px 40px 22px 40px;
          border: 1px solid rgba(255,255,255,0.10); box-shadow: 0 30px 80px rgba(0,0,0,0.5); color: #f2f2f7; }
.w-ui.light .w-card { background: rgba(250,250,252,0.66); border-color: rgba(0,0,0,0.08); color: #1c1c1e; }
.w-title { font-size: 34px; font-weight: 800; }
.w-sub { font-size: 15px; opacity: 0.85; }
.w-row { font-size: 15px; }
.w-row-sub { font-size: 13px; opacity: 0.8; }
.w-card-name { font-size: 14px; font-weight: 600; }
.theme-card { background: transparent; border-radius: 18px; padding: 10px; border: 2px solid transparent; transition: all 200ms ease; }
.theme-card:hover { background: alpha(currentColor, 0.06); }
.theme-card:checked { border-color: @ACCENT@; background: alpha(@ACCENT@, 0.12); }
.pv-light { border-radius: 12px; background: linear-gradient(160deg, #f5f5f7 0%, #dcdce4 100%); border: 1px solid rgba(0,0,0,0.1); }
.pv-dark  { border-radius: 12px; background: linear-gradient(160deg, #2c2c34 0%, #0e0e14 100%); border: 1px solid rgba(255,255,255,0.1); }
.wp-pic { border-radius: 12px; }
.swatch { min-width: 26px; min-height: 26px; padding: 0; border-radius: 99px; border: 2px solid rgba(255,255,255,0.25); transition: all 160ms ease; }
.swatch:hover { transform: scale(1.12); }
.swatch:checked { border: 3px solid white; box-shadow: 0 0 0 2px @ACCENT@; }
.swatch-wal { background: linear-gradient(135deg, #ff5f9e, #a56bff, #3fb6ff); }
.wifi-row { background: alpha(currentColor, 0.06); border-radius: 12px; padding: 10px 14px; }
.wifi-row:hover { background: alpha(currentColor, 0.11); }
.wifi-row.active { background: alpha(@ACCENT@, 0.22); }
.primary, .w-next { background: @ACCENT@; color: white; border-radius: 12px; padding: 8px 22px; font-weight: 700; font-size: 15px; }
.w-next:hover { filter: brightness(1.1); }
.w-back { background: alpha(currentColor, 0.08); border-radius: 12px; padding: 8px 18px; font-size: 15px; color: inherit; }
.dot { min-width: 8px; min-height: 8px; border-radius: 99px; background: alpha(currentColor, 0.25); transition: all 300ms ease; }
.dot.on { min-width: 22px; background: @ACCENT@; }
.acc-cap { font-size: 13px; opacity: 0.85; margin-top: 4px; }
.keycap { font-size: 13px; font-weight: 700; padding: 3px 10px; border-radius: 8px; background: alpha(currentColor, 0.10); }
entry, passwordentry { border-radius: 10px; min-height: 34px; }
.wp-card { padding: 6px; border-radius: 14px; }
.wp-card .w-card-name { font-size: 13px; font-weight: 600; }
scrolledwindow, scrolledwindow > viewport, flowbox, flowboxchild { background: transparent; }
flowboxchild { padding: 0; }
.seg { background: alpha(currentColor, 0.08); border-radius: 10px; padding: 3px; }
.seg > button { background: transparent; border: none; border-radius: 8px; padding: 4px 16px; font-size: 13px; color: inherit; box-shadow: none; }
.seg > button:checked { background: @ACCENT@; color: white; }
.opt-list { background: alpha(currentColor, 0.05); border-radius: 14px; padding: 2px 14px; }
.opt-row { padding: 7px 0; border-bottom: 1px solid alpha(currentColor, 0.07); }
.opt-row:last-child { border-bottom: none; }
switch:checked { background: @ACCENT@; }
dropdown > button { border-radius: 9px; }
)";

int main(int argc, char** argv) {
    // НЕ sysui::mem_init(): він вмикає Cairo-рендерер (CPU), а тут кадр масштабує і розмиває відеокарта
    mallopt(M_ARENA_MAX, 2);
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    bool once = false, preview = true;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--once") { once = true; preview = false; }
        else if (a == "--preview") preview = true;
        else if (a == "--bg" && i + 1 < argc) g_test_bg = argv[++i];   // перевірка: фон-зображення і «запечений» блюр під карткою
        else if (a == "--page" && i + 1 < argc) g_start_page = std::clamp(atoi(argv[++i]), 0, NPAGES - 1);
    }
    std::string done = sysui::home() + "/.local/state/mgos/welcome_done";
    if (once && access(done.c_str(), F_OK) == 0) return 0;

    GtkApplication* app = gtk_application_new("com.megumin.welcome", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication* app, gpointer) {
        sysui::install_shared_style();
        g_wv.style = "aurora";
        g_wv.set_accent(sysui::accent());
        if (sysui::settings().accent_mode != "custom") g_wv.set_palette_css(sysui::wal_css_path());
        g_wv.fade_from = 2;

        g_win = gtk_application_window_new(app);
        gtk_layer_init_for_window(GTK_WINDOW(g_win));
        gtk_layer_set_namespace(GTK_WINDOW(g_win), "mgos_welcome");
        gtk_layer_set_layer(GTK_WINDOW(g_win), GTK_LAYER_SHELL_LAYER_OVERLAY);
        for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
            gtk_layer_set_anchor(GTK_WINDOW(g_win), e, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(g_win), -1);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(g_win), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

        sysui::live_css(CSS);

        GtkWidget* ov = gtk_overlay_new();
        g_area = (GtkWidget*)g_object_new(welcome_view_get_type(), nullptr);
        gtk_widget_set_hexpand(g_area, TRUE); gtk_widget_set_vexpand(g_area, TRUE);
        gtk_overlay_set_child(GTK_OVERLAY(ov), g_area);

        g_ui = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(g_ui, "w-ui");
        gtk_widget_add_css_class(g_ui, sysui::color_scheme() == "prefer-dark" ? "dark" : "light");
        gtk_widget_set_halign(g_ui, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(g_ui, GTK_ALIGN_CENTER);
        gtk_widget_set_visible(g_ui, FALSE);
        GtkWidget* card = g_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
        gtk_widget_add_css_class(card, "w-card");
        gtk_widget_set_size_request(card, 800, 580);
        gtk_widget_set_hexpand(card, FALSE); gtk_widget_set_vexpand(card, FALSE);
        gtk_widget_set_halign(card, GTK_ALIGN_CENTER); gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
        g_stack = gtk_stack_new();
        gtk_stack_set_transition_duration(GTK_STACK(g_stack), 420);
        gtk_stack_set_hhomogeneous(GTK_STACK(g_stack), TRUE);
        gtk_widget_set_vexpand(g_stack, TRUE);
        // сторінки будуємо після першого кадру (див. нижче) — чорний екран з'являється одразу
        gtk_box_append(GTK_BOX(card), g_stack);
        GtkWidget* nav = gtk_center_box_new();
        g_back = gtk_button_new_with_label("Назад");
        gtk_widget_add_css_class(g_back, "w-back");
        g_next = gtk_button_new_with_label("Далі");
        gtk_widget_add_css_class(g_next, "w-next");
        g_dots = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign(g_dots, GTK_ALIGN_CENTER);
        for (int i = 0; i < NPAGES; i++) { GtkWidget* d = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_add_css_class(d, "dot"); gtk_box_append(GTK_BOX(g_dots), d); }
        gtk_center_box_set_start_widget(GTK_CENTER_BOX(nav), g_back);
        gtk_center_box_set_center_widget(GTK_CENTER_BOX(nav), g_dots);
        gtk_center_box_set_end_widget(GTK_CENTER_BOX(nav), g_next);
        gtk_box_append(GTK_BOX(card), nav);
        g_signal_connect(g_back, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { go(-1); }), nullptr);
        g_signal_connect(g_next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { go(+1); }), nullptr);
        gtk_box_append(GTK_BOX(g_ui), card);
        gtk_overlay_add_overlay(GTK_OVERLAY(ov), g_ui);
        update_nav();

        gtk_window_set_child(GTK_WINDOW(g_win), ov);

        // клік / Enter на інтро → налаштування; Esc у --preview — вийти
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer) { start_setup(); }), nullptr);
        gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(click));
        GtkEventController* keys = gtk_event_controller_key_new();
        g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint kv, guint, GdkModifierType, gpointer) -> gboolean {
            if (!g_intro_done && (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter || kv == GDK_KEY_space)) { start_setup(); return TRUE; }
            if (kv == GDK_KEY_Escape && g_preview) { finish(); return TRUE; }
            return FALSE;
        }), nullptr);
        gtk_widget_add_controller(g_win, keys);
        GtkEventController* mot = gtk_event_controller_motion_new();   // курсор — для «Чорної діри»
        g_signal_connect(mot, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double x, double y, gpointer) { g_mouse_x = x; g_mouse_y = y; }), nullptr);
        gtk_widget_add_controller(g_win, mot);

        gtk_widget_set_opacity(g_win, 0);   // проявляється у таймері — без різкого чорного кадру
        gtk_window_present(GTK_WINDOW(g_win));
        g_t0 = g_get_monotonic_time();
        g_idle_add([](gpointer) -> gboolean {
            gtk_stack_add_named(GTK_STACK(g_stack), build_look(), "look");
            gtk_stack_add_named(GTK_STACK(g_stack), build_wifi(), "wifi");
            gtk_stack_add_named(GTK_STACK(g_stack), build_wallpaper(), "wallpaper");
            gtk_stack_add_named(GTK_STACK(g_stack), build_desktop(), "desktop");
            gtk_stack_add_named(GTK_STACK(g_stack), build_done(), "done");
            if (g_start_page >= 0) g_page = PAGES[g_start_page];   // --page N: одразу потрібний екран, без гортання
            gtk_stack_set_transition_type(GTK_STACK(g_stack), GTK_STACK_TRANSITION_TYPE_NONE);
            gtk_stack_set_visible_child_name(GTK_STACK(g_stack), g_page.c_str());
            update_nav();
            wifi_scan();
            if (!g_test_bg.empty()) set_bg_image(g_test_bg);
            return G_SOURCE_REMOVE;
        }, nullptr);
        if (g_start_page >= 0) {                     // перегляд конкретного екрана: інтро вже «минуло»
            g_t0 -= (gint64)((welcome_start() + 2) * 1e6);
            g_t = welcome_start() + 2;
            start_setup();
        }

        // ~30 кадрів/с: час, автоперехід до налаштування, згасання в кінці
        g_timeout_add(33, [](gpointer) -> gboolean {
            g_t = (g_get_monotonic_time() - g_t0) / 1e6;
            if (g_leave < 0 && g_start_page < 0) gtk_widget_set_opacity(g_win, ease(g_t / FADE_IN));
            else if (g_leave < 0) gtk_widget_set_opacity(g_win, ease((g_t - (welcome_start() + 2)) / 0.5));
            if (!g_intro_done && g_t > welcome_start() + 9) start_setup();   // не чекаємо вічно
            if (g_ui_shown >= 0 && !g_intro_done) {}
            if (g_leave >= 0) {
                double u = (g_t - g_leave) / 1.1;
                static bool panels = false;
                if (!panels && u >= 0.55) {   // ще трохи темно — панелі виїжджають разом із останнім проявленням
                    panels = true;
                    std::string d = sysui::home() + "/.config/SYSui";
                    run_bg("hyprctl dispatch exec " + d + "/topbar >/dev/null; sleep 0.18; hyprctl dispatch exec " + d + "/dock >/dev/null");
                }
                gtk_widget_set_opacity(g_win, 1 - ease(u));
                if (u >= 1) { gtk_window_destroy(GTK_WINDOW(g_win)); return G_SOURCE_REMOVE; }
            }
            // статичний фон (зображення) і нічого не рухається — кадр не перемальовуємо
            bool still = g_bg_tex && g_intro_done && g_leave < 0 && g_t - g_bg_switch > 0.8 && g_t - g_ui_shown > 0.8;
            static double last_mx = 0, last_my = 0;
            if (!still || last_mx != g_mouse_x || last_my != g_mouse_y) gtk_widget_queue_draw(g_area);
            last_mx = g_mouse_x; last_my = g_mouse_y;
            return G_SOURCE_CONTINUE;
        }, nullptr);
    }), nullptr);
    g_preview = preview;
    int r = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return r;
}
