// haptic.h — haptic-відгук Magic Trackpad на дії в інтерфейсі SYSui (як macOS).
// Без фонових процесів: hidraw відкривається ліниво, запис неблокуючий, помилки тихо ігноруються.
// Вмикається прапорцем "ui": true у ~/.config/mgos/haptic.json (за замовчуванням false); файл
// перечитується за mtime раз на ~3 с. Звичайний клік трекпада (click) цей файл не чіпає.
// Протокол — як у ~/.local/bin/mgos-haptic: F2 53 01 SS 78 02 CC 24 30 06 01 DD 18 48 12.
#pragma once
#include <gtk/gtk.h>
#include <glib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>

namespace haptic {

struct State {
    int fd = -1;
    bool tried = false;          // пошук пристрою вже робили
    gint64 next_try = 0;         // коли знову шукати, якщо не знайшли
    bool ui = false;
    int strength = 50;
    time_t mtime = 0;
    gint64 cfg_check = 0;
    gint64 last_tap = 0, last_tick = 0;
};
inline State& st() { static State s; return s; }

inline void load_cfg() {
    State& s = st();
    gint64 now = g_get_monotonic_time();
    if (now < s.cfg_check) return;
    s.cfg_check = now + 3000000;
    std::string p = std::string(g_get_home_dir()) + "/.config/mgos/haptic.json";
    struct stat sb;
    if (stat(p.c_str(), &sb) != 0) { s.ui = false; s.mtime = 0; return; }
    if (sb.st_mtime == s.mtime) return;
    s.mtime = sb.st_mtime;
    s.ui = false; s.strength = 50;
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return;
    char buf[1024]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    if (const char* k = strstr(buf, "\"ui\"")) {
        const char* c = strchr(k, ':');
        if (c) { ++c; while (*c == ' ') ++c; s.ui = strncmp(c, "true", 4) == 0; }
    }
    if (const char* k = strstr(buf, "\"strength\"")) {
        const char* c = strchr(k, ':');
        if (c) { int v = atoi(c + 1); if (v < 0) v = 0; if (v > 100) v = 100; s.strength = v; }
    }
}

inline void find_dev() {
    State& s = st();
    if (s.fd >= 0) return;
    gint64 now = g_get_monotonic_time();
    if (s.tried && now < s.next_try) return;
    s.tried = true; s.next_try = now + 30000000;   // нема трекпада — шукаємо раз на 30 с
    for (int i = 0; i < 16; ++i) {
        char p[96]; snprintf(p, sizeof p, "/sys/class/hidraw/hidraw%d/device/uevent", i);
        FILE* f = fopen(p, "r");
        if (!f) continue;
        char b[512]; size_t n = fread(b, 1, sizeof(b) - 1, f); b[n] = 0; fclose(f);
        for (char* c = b; *c; ++c) *c = toupper((unsigned char)*c);
        if (!strstr(b, "HID_ID=0005:0000004C:00000265")) continue;
        snprintf(p, sizeof p, "/dev/hidraw%d", i);
        s.fd = open(p, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (s.fd >= 0) return;
    }
}

inline void write_pkt(int ss, int cc, int dd) {
    State& s = st();
    if (s.fd < 0) return;
    if (cc > 0x20) cc = 0x20;
    if (dd > 0x40) dd = 0x40;
    unsigned char b[15] = {0xF2, 0x53, 0x01, (unsigned char)ss, 0x78, 0x02, (unsigned char)cc,
                           0x24, 0x30, 0x06, 0x01, (unsigned char)dd, 0x18, 0x48, 0x12};
    ssize_t r = write(s.fd, b, sizeof b);
    if (r < 0 && errno != EAGAIN) { close(s.fd); s.fd = -1; }   // пристрій зник — перевідкриємо пізніше
}

inline gboolean release_cb(gpointer) { write_pkt(0x14, 0, 0); return G_SOURCE_REMOVE; }

inline bool enabled() { load_cfg(); return st().ui && st().strength > 0; }

// Один імпульс: ss — режим (0x13 легкий, 0x15 середній), amp — відносна сила (масштабується slider strength).
inline void raw_pulse(int ss, double amp) {
    State& s = st();
    find_dev();
    if (s.fd < 0) return;
    int v = (int)(amp * s.strength / 50.0 + 0.5);
    if (v < 1) v = 1;
    write_pkt(ss, v, v);
    g_timeout_add(30, release_cb, nullptr);
}

// Серія імпульсів зі зростанням/спадом: {амплітуда, пауза до наступного, мс}
struct Step { double amp; int gap; };
struct Seq { const Step* steps; int n, i; int ss; };
inline gboolean seq_cb(gpointer d) {
    Seq* q = (Seq*)d;
    raw_pulse(q->ss, q->steps[q->i].amp);
    int gap = q->steps[q->i].gap;
    if (++q->i >= q->n) { delete q; return G_SOURCE_REMOVE; }
    g_timeout_add(gap, seq_cb, q);
    return G_SOURCE_REMOVE;
}
template <int N> inline void play(const Step (&steps)[N], int ss) { seq_cb(new Seq{steps, N, 0, ss}); }

// Іменовані візерунки. Троттл тіків ~80 мс; неблокуюче; лише коли ui:true.
inline void pattern(const char* name) {
    if (!enabled()) return;
    State& s = st();
    gint64 now = g_get_monotonic_time();
    static const Step minimize[] = {{1.5, 45}, {3, 45}, {5, 45}, {3, 45}, {1.5, 0}};   // «втягування»: ↑↓
    static const Step restore[]  = {{1.5, 40}, {3, 40}, {5, 40}, {3, 40}, {1.5, 0}};
    static const Step drop[]     = {{6, 0}};
    static const Step open[]     = {{1.5, 55}, {3, 55}, {2, 0}};                       // м'яке «виринання»
    static const Step close[]    = {{3, 55}, {1.5, 0}};
    static const Step wsp[]      = {{2, 50}, {4, 0}};                                  // короткий «свайп»
    if (!strcmp(name, "drag_tick") || !strcmp(name, "resize_tick")) {
        if (now - s.last_tick < 80000) return;
        s.last_tick = now;
        raw_pulse(0x13, !strcmp(name, "drag_tick") ? 1.5 : 2.5);
        return;
    }
    if (strcmp(name, "drop") && now - s.last_tap < 120000) return;
    s.last_tap = now;
    if      (!strcmp(name, "minimize"))    play(minimize, 0x13);
    else if (!strcmp(name, "restore"))     play(restore, 0x13);
    else if (!strcmp(name, "drop"))        play(drop, 0x15);
    else if (!strcmp(name, "panel_open"))  play(open, 0x13);
    else if (!strcmp(name, "panel_close")) play(close, 0x13);
    else if (!strcmp(name, "workspace"))   play(wsp, 0x13);
}

inline bool is_interactive(GtkWidget* w, GtkWidget* root) {
    for (; w && w != root; w = gtk_widget_get_parent(w)) {
        if (GTK_IS_BUTTON(w) || GTK_IS_SWITCH(w) || GTK_IS_CHECK_BUTTON(w) || GTK_IS_SCALE(w) ||
            GTK_IS_LIST_BOX_ROW(w) || GTK_IS_DROP_DOWN(w))
            return true;
        if (gtk_widget_has_css_class(w, "haptic")) return true;
        // віджет із власним GtkGestureClick (карточки, іконки док-у)
        GListModel* m = gtk_widget_observe_controllers(w);
        guint n = g_list_model_get_n_items(m);
        bool hit = false;
        for (guint i = 0; i < n && !hit; ++i) {
            GObject* c = (GObject*)g_list_model_get_item(m, i);
            if (GTK_IS_GESTURE_CLICK(c) || GTK_IS_DRAG_SOURCE(c) || GTK_IS_GESTURE_DRAG(c)) hit = true;
            g_object_unref(c);
        }
        g_object_unref(m);
        if (hit) return true;
    }
    return false;
}

struct DragData { bool active = false; };

inline void on_drag_begin(GtkGestureDrag*, double, double, gpointer d) { ((DragData*)d)->active = false; }
inline void on_drag_update(GtkGestureDrag* g, double ox, double oy, gpointer d) {
    DragData* dd = (DragData*)d;
    if (!dd->active) {
        if (ox * ox + oy * oy < 64) return;      // поріг 8 px
        if (!enabled()) return;
        double sx, sy; gtk_gesture_drag_get_start_point(g, &sx, &sy);
        GtkWidget* root = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
        GtkWidget* w = gtk_widget_pick(root, sx, sy, GTK_PICK_DEFAULT);
        if (!w || !is_interactive(w, root)) return;
        dd->active = true;
    }
    pattern("drag_tick");
}
inline void on_drag_end(GtkGestureDrag*, double, double, gpointer d) {
    DragData* dd = (DragData*)d;
    if (dd->active) { dd->active = false; pattern("drop"); }
}

// Відкриття/закриття панелі: вікно показали/сховали. Перші 3 с життя процесу ігноруються.
inline void attach_panel(GtkWidget* win) {
    if (!win) return;
    static gint64 t0 = g_get_monotonic_time();
    g_signal_connect(win, "map", G_CALLBACK(+[](GtkWidget*, gpointer) {
        if (g_get_monotonic_time() - t0 > 3000000) pattern("panel_open"); }), nullptr);
    g_signal_connect(win, "unmap", G_CALLBACK(+[](GtkWidget*, gpointer) {
        if (g_get_monotonic_time() - t0 > 3000000) pattern("panel_close"); }), nullptr);
}

// Підключає haptic-відгук до вікна/кореня: натиск на кнопки/пункти/картки + перетягування.
// Жести в фазі CAPTURE і ніколи не «забирають» послідовність — поведінку віджетів не змінюють.
inline void attach(GtkWidget* root) {
    if (!root) return;
    GtkGesture* d = gtk_gesture_drag_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(d), GTK_PHASE_CAPTURE);
    DragData* dd = new DragData();
    g_signal_connect(d, "drag-begin", G_CALLBACK(on_drag_begin), dd);
    g_signal_connect(d, "drag-update", G_CALLBACK(on_drag_update), dd);
    g_signal_connect(d, "drag-end", G_CALLBACK(on_drag_end), dd);
    gtk_widget_add_controller(root, GTK_EVENT_CONTROLLER(d));
}

}  // namespace haptic
