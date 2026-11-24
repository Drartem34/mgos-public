// ===================== ТРИ ПАЛЬЦІ ВНИЗ → ВІКНО В ДОК (анімація «джин», як на macOS) =====================
// Підключається в dock.cpp після сховища (потрібні g_dock_state, g_window, g_icon_size, DOCK_STASH).
//
// Hyprland не дає ні прогресу жесту, ні «джин»-анімації, тому жест читаємо самі: окремий libinput-контекст
// лише на тачпад (evdev дозволяє кількох читачів, Hyprland нічого не помічає). Свайп 3 пальцями вниз:
//   1. помічник genie_osd (окремий процес на Vulkan — док малює через Cairo, а GTK обирає рендерер раз на процес;
//      саме через Cairo раніше замість вікна був рожевий квадрат) знімає вікно grim'ом і кладе знімок рівно на його місце;
//   2. справжнє вікно стає прозорим, а знімок стікає «лійкою» в іконку доку рівно настільки, наскільки провели пальці
//      (туди й назад, genie_paint.h);
//   3. відпустили до порога → знімок пружинить назад, вікно повертається (захист від випадкового згортання);
//      довели далі порога або швидко змахнули вниз → знімок стікає в іконку, вікно їде в special:mgos-dock.
// Режим «Налаштування → Трекпад → Три пальці вниз» (tp_swipe_down): minimize | runner | off.
// У режимах runner/off libinput-пристрій закритий — док не прокидається від тачпада взагалі.

#include <libinput.h>
#include <set>
#include <libudev.h>
#include <fcntl.h>

namespace genie {

static const double COMMIT = 0.55;          // частка жесту, після якої вікно ховається (менше — повертається)
static const double LOCK_PX = 14;           // скільки провести, поки визначаємо напрямок
static const double FLICK = 0.9;            // px/мс вниз — швидкий змах ховає вікно навіть з коротким рухом
static const double UP_PX = 45;             // три пальці вгору: скільки провести, щоб спрацювало (захист від випадкових)

enum State { IDLE, PENDING, ACTIVE, IGNORED, UP_WAIT, GROW };   // UP_WAIT — пальці йдуть угору, чекаємо UP_PX; GROW — вікно виростає з іконки
static State st = IDLE;
static double acc_x = 0, acc_y = 0;          // накопичений зсув пальців
static double target_p = 0;
static double vel = 0; static uint64_t last_us = 0;   // швидкість пальців (px/мс), для змаху
static bool released = false, committed = false, shown = false, waiting_done = false;
static std::string win_addr, win_cls;
static int mon_w = 1920, mon_h = 1080;
static bool hidden_real = false;
static unsigned gen = 0;                         // покоління жесту (старі відповіді помічника ігноруємо)
static guint target_timer = 0;
static double last_tx = -1, last_ty = -1, last_s = -1;

static libinput* li = nullptr; static guint li_watch = 0;

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ---------- куди летіти: іконка програми в доці ----------
static void target(double& tx, double& ty, double& s) {
    s = g_icon_size; tx = mon_w / 2.0; ty = mon_h - s / 2 - 10;
    if (!g_dock_state || !g_window || !gtk_widget_get_mapped(g_window)) return;
    auto it = g_dock_state->current_items.find(win_cls);
    if (it == g_dock_state->current_items.end() || !gtk_widget_get_mapped(it->second)) return;
    graphene_point_t in = GRAPHENE_POINT_INIT((float)gtk_widget_get_width(it->second) / 2, (float)gtk_widget_get_height(it->second) / 2), out;
    if (!gtk_widget_compute_point(it->second, g_window, &in, &out)) return;
    int dw = gtk_widget_get_width(g_window), dh = gtk_widget_get_height(g_window);
    tx = (mon_w - dw) / 2.0 + out.x; ty = mon_h - dh + out.y;
}

// ---------- помічник genie_osd (GPU) ----------
// Док малює через Cairo (CPU), а GTK обирає рендерер раз на процес — тому анімацію малює окремий процес на Vulkan.
// Запускається на початку свайпу 3 пальцями, сам завершується після 90 с без жестів.
static GSubprocess* osd = nullptr; static GOutputStream* osd_in = nullptr; static GDataInputStream* osd_out = nullptr;

// «здача» у живу іконку: CSS-пружина (.genie-squash) на кнопці програми; без анімацій/еко — нічого
static bool anim_on() {
    const auto& S = sysui::settings();
    return S.animations && !S.eco_effects;   // анімація взагалі (джин або плавне зменшення)
}
static void squash_icon(const std::string& cls) {
    // CSS-transform (.genie-squash) давав рожевий квадрат (GSK error-вузол), а -gtk-icon-transform не видно —
    // тому пружину малює layout доку тим самим gsk-трансформом, що й збільшення (див. squash_scale у dock.cpp)
    if (!anim_on() || !sysui::settings().genie_squash || !g_dock_state) return;
    auto it = g_dock_state->current_items.find(cls);
    if (it == g_dock_state->current_items.end()) return;
    Mag* m = mag_of(it->second);
    if (!m) return;
    m->sq_t0 = g_get_monotonic_time();
    if (GtkWidget* p = gtk_widget_get_parent(it->second)) gtk_widget_queue_allocate(p);
    mag_kick();
}

static void finish();
static void on_osd_line(const std::string& l);
static void set_real_opacity(bool visible);
static void anim_back(const std::string& addr);

static void osd_dead() {
    if (osd_in) { g_object_unref(osd_in); osd_in = nullptr; }
    if (osd_out) { g_object_unref(osd_out); osd_out = nullptr; }
    if (osd) { g_object_unref(osd); osd = nullptr; }
    if (st == ACTIVE || waiting_done) {   // помічник упав посеред жесту — повертаємо вікно як було
        if (hidden_real && !win_addr.empty()) { set_real_opacity(true); anim_back(win_addr); }
        finish();
    }
}

static void osd_read() {
    if (!osd_out) return;
    g_data_input_stream_read_line_async(osd_out, G_PRIORITY_HIGH, nullptr, [](GObject* s, GAsyncResult* r, gpointer) {
        gsize n = 0;
        char* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(s), r, &n, nullptr);
        if (!line) { if ((GObject*)osd_out == s) osd_dead(); return; }
        std::string l(line); g_free(line);
        on_osd_line(l);
        osd_read();
    }, nullptr);
}

static void osd_spawn() {
    if (osd) return;
    std::string bin = sysui::home() + "/.config/SYSui/genie_osd";
    if (access(bin.c_str(), X_OK) != 0) return;
    osd = g_subprocess_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE), nullptr, bin.c_str(), nullptr);
    if (!osd) return;
    osd_in = g_subprocess_get_stdin_pipe(osd); g_object_ref(osd_in);
    osd_out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(osd));
    osd_read();
}

static void osd_send(const std::string& s) {
    if (!osd_in) return;
    std::string l = s + "\n";
    if (!g_output_stream_write_all(osd_in, l.data(), l.size(), nullptr, nullptr, nullptr)) osd_dead();
}

// числа для помічника — завжди з крапкою: snprintf слідує локалі (uk_UA дає «13,0»), а помічник читає «13.0»;
// через це раніше помічник мовчки відкидав «start», і жест/Super+Q зависали
static std::string num(double v) { char b[G_ASCII_DTOSTR_BUF_SIZE]; return g_ascii_formatd(b, sizeof b, "%.4f", v); }

static void send_progress(bool force) {
    double tx, ty, s; target(tx, ty, s);
    if (!force && tx == last_tx && ty == last_ty && s == last_s) return;
    last_tx = tx; last_ty = ty; last_s = s;
    osd_send("p " + std::to_string(gen) + " " + num(target_p) + " " + num(tx) + " " + num(ty) + " " + num(s));
}

// Справжнє вікно під знімком ховаємо/повертаємо МИТТЄВО: без no_anim Hyprland тягне прозорість через fadeSwitch
// (~200 мс) — на старті вікно просвічувало під «лійкою», а при відміні оверлей зникав раніше, ніж вікно проявлялось.
// Усе одним [[BATCH]] — щоб зміни потрапили в один кадр композитора. no_anim знімає anim_back(), коли вже безпечно.
static void set_real_opacity(bool visible) {
    if (win_addr.empty()) return;
    // прозорість сама по собі лишає «привид» з розмиття й тіні — їх теж прибираємо на час жесту
    std::string a = "dispatch setprop address:" + win_addr;
    sysui::hypr_request("[[BATCH]]" + a + " no_anim 1;" +
                        a + (visible ? " opacity 1.0;" : " opacity 0.0 override;") +
                        a + (visible ? " no_blur 0;" : " no_blur 1;") +
                        a + (visible ? " no_shadow 0" : " no_shadow 1"));
    hidden_real = !visible;
}

static void anim_back(const std::string& addr) {
    if (!addr.empty()) sysui::hypr_request("dispatch setprop address:" + addr + " no_anim 0");
}

static void finish();
// ріст не вдався (нема знімка/помічника) — вікно з'являється миттєво, як без анімацій
static void grow_reveal() {
    std::string a = win_addr;
    set_real_opacity(true);
    finish();
    g_timeout_add_full(G_PRIORITY_DEFAULT, 120, [](gpointer s) -> gboolean { anim_back((char*)s); return G_SOURCE_REMOVE; },
                       g_strdup(a.c_str()), g_free);
}

static void finish() {
    if (osd && st != IDLE) osd_send("hide " + std::to_string(gen));
    st = IDLE; released = false; committed = false; shown = false; waiting_done = false;
    g_genie_force_dock = false;
    if (target_timer) { g_source_remove(target_timer); target_timer = 0; }
    win_addr.clear();
}

static void on_osd_line(const std::string& l) {
    std::istringstream is(l);
    std::string c; unsigned g = 0; is >> c >> g;
    if (g != gen || st == IDLE) return;
    if (c == "shown") {
        shown = true;
        // after-paint ≠ «композитор уже показав кадр»: ховаємо справжнє вікно лише за 2–3 кадри, інакше на мить
        // видно плейсхолдер (рожевий квадрат) — оверлей ще не мапнувся, а вікно вже прозоре
        g_timeout_add(45, [](gpointer d) -> gboolean {
            if (GPOINTER_TO_UINT(d) == gen && st != IDLE && shown && !hidden_real) set_real_opacity(false);
            return G_SOURCE_REMOVE;
        }, GUINT_TO_POINTER(gen));
    } else if (c == "hit") {
        squash_icon(win_cls);
    } else if (c == "fail") {
        // знімка нема (grim недоступний) — без анімації
        if (st == GROW) { grow_reveal(); return; }
        if (released) {
            if (committed && !win_addr.empty())
                stash_window(win_addr);
            finish();
        } else { finish(); st = IGNORED; }   // решту жесту ігноруємо; END поверне IDLE (адресу вже очищено — наступні жести не блокуються)
    } else if (c == "done") {
        int cm = 0; is >> cm;
        if (target_timer) { g_source_remove(target_timer); target_timer = 0; }   // анімацію закінчено — позиція іконки вже не потрібна
        if (cm) {
            // знімок «втік» в іконку → переносимо вікно в сховище й повертаємо непрозорість (там його не видно).
            // Перенос — поки ще діє no_anim: інакше Hyprland 600 мс анімує відхід вікна, а повернута непрозорість
            // робила його знову видимим — вікно «блимало» після анімації й лише потім зникало.
            // Одним пакетом, no_anim знімаємо вже в сховищі — повернення з доку анімується як звичайно.
            // Непрозорість НЕ повертаємо тут: картки простору зараз зроблять вікно плаваючим і зменшать — це мусить
            // відбутися невидимо. veil_keep поверне все, коли стихне (або одразу, якщо відкриють простір).
            stash_window(win_addr);
            veil_keep(win_addr);
            hidden_real = false;
            finish();
        } else {
            // пружина назад: вікно миттєво (no_anim) проявляється під знімком, що стоїть рівно на ньому;
            // знімок ховаємо через кілька кадрів, коли композитор уже точно намалював справжнє вікно
            set_real_opacity(true);
            unsigned gg = gen;
            st = IGNORED;
            g_timeout_add(70, [](gpointer d) -> gboolean {
                if (GPOINTER_TO_UINT(d) == gen && st == IGNORED) {
                    std::string a = win_addr;
                    finish();
                    // no_anim знімаємо ще трохи пізніше — після того, як оверлей зник
                    g_timeout_add_full(G_PRIORITY_DEFAULT, 120, [](gpointer s) -> gboolean { anim_back((char*)s); return G_SOURCE_REMOVE; },
                                       g_strdup(a.c_str()), g_free);
                }
                return G_SOURCE_REMOVE;
            }, GUINT_TO_POINTER(gg));
        }
    }
}

// відпустили пальці: вирішуємо, куди доводити (force — Super+Q: завжди в док, без пальців)
static void release(bool force = false) {
    released = true;
    if (force) committed = true;
    else if (vel < -0.3) committed = false;                               // змах угору — відміна
    else committed = target_p >= COMMIT || (vel > FLICK && target_p > 0.12);
    waiting_done = true;
    if (target_timer) { g_source_remove(target_timer); target_timer = 0; }
    send_progress(true);
    // тривалість: довести решту шляху; після швидкого змаху — швидше
    double rest = committed ? 1 - target_p : target_p;
    double ms = (committed ? 260 : 200) + 300 * rest;
    if (force) ms = sysui::settings().genie_anim ? 520 : 250;                                                 // як «джин» у macOS (~0,5 с)
    else if (std::abs(vel) > FLICK) ms *= 0.75;
    ms = std::clamp(ms * 100.0 / std::clamp(sysui::settings().genie_speed, 50, 200), 120.0, 1600.0);
    osd_send("end " + std::to_string(gen) + " " + (committed ? "1" : "0") + " " + std::to_string((int)ms));
    // поки док виїжджає, іконка рухається — доносимо її позицію до кінця анімації
    target_timer = g_timeout_add(33, [](gpointer) -> gboolean {
        double tx, ty, s; target(tx, ty, s);
        if (tx != last_tx || ty != last_ty || s != last_s) {
            last_tx = tx; last_ty = ty; last_s = s;
            osd_send("p " + std::to_string(gen) + " " + num(target_p) + " " + num(tx) + " " + num(ty) + " " + num(s));
        }
        return G_SOURCE_CONTINUE;
    }, nullptr);
}

static bool runner_open() {
    try {
        json L = json::parse(sysui::hypr_request("j/layers"));
        for (auto& [mon, m] : L.items()) {
            if (!m.contains("levels") || !m["levels"].is_object()) continue;
            const json& lv = m["levels"];   // не тимчасовий об'єкт: ітерація по value(...) падала (SIGSEGV)
            for (auto& [lvl, arr] : lv.items())
                for (auto& l : arr) if (l.value("namespace", "") == "runner") return true;
        }
    } catch (...) {}
    return false;
}

static void runner_hide() {
    std::string p = sysui::home() + "/.config/SYSui/runner.sock";
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    sockaddr_un a{}; a.sun_family = AF_UNIX; strncpy(a.sun_path, p.c_str(), sizeof(a.sun_path) - 1);
    if (connect(fd, (sockaddr*)&a, sizeof(a)) == 0) { ssize_t r = write(fd, "hide\n", 5); (void)r; }
    close(fd);
}

// напрямок визначився: вниз → починаємо
static void begin_minimize() {
    if (runner_open()) { runner_hide(); st = IGNORED; return; }
    json aw, mons;
    try { aw = json::parse(sysui::hypr_request("j/activewindow")); mons = json::parse(sysui::hypr_request("j/monitors")); } catch (...) { st = IGNORED; return; }
    std::string addr = aw.value("address", "");
    std::string ws = aw.contains("workspace") ? aw["workspace"].value("name", "") : "";
    if (addr.empty() || ws == DOCK_STASH || aw.value("fullscreen", 0) != 0 || !aw.contains("at") || !aw.contains("size")) { st = IGNORED; return; }
    double mx = 0, my = 0, scale = 1; mon_w = 1920; mon_h = 1080;
    std::string mon_name;
    int mid = aw.value("monitor", -1);
    for (auto& m : mons) if (m.value("id", -2) == mid || (mid < 0 && m.value("focused", false))) {
        mx = m.value("x", 0); my = m.value("y", 0); scale = m.value("scale", 1.0); mon_name = m.value("name", "");
        int t = m.value("transform", 0);
        double pw = m.value("width", 1920), ph = m.value("height", 1080);
        if (t % 2) std::swap(pw, ph);
        mon_w = (int)std::lround(pw / scale); mon_h = (int)std::lround(ph / scale);
    }
    int ax = aw["at"][0], ay = aw["at"][1], sw_ = aw["size"][0], sh = aw["size"][1];
    if (sw_ < 20 || sh < 20) { st = IGNORED; return; }
    win_addr = addr; win_cls = to_lower(aw.value("class", ""));
    st = ACTIVE; released = false; committed = false; shown = false; waiting_done = false; target_p = 0;
    g_genie_force_dock = true;
    gen++;
    osd_spawn();
    if (!osd) { st = IGNORED; g_genie_force_dock = false; win_addr.clear(); return; }
    double radius = 0;
    try { radius = json::parse(sysui::hypr_request("j/getoption decoration:rounding")).value("int", 0); } catch (...) {}
    double tx, ty, s; target(tx, ty, s);
    last_tx = tx; last_ty = ty; last_s = s;
    osd_send(std::string("plain ") + (sysui::settings().genie_anim ? "0" : "1"));
    osd_send("start " + std::to_string(gen) + " " + (mon_name.empty() ? "-" : mon_name) + " " + std::to_string(ax) + " " + std::to_string(ay) +
             " " + std::to_string(sw_) + " " + std::to_string(sh) + " " + num(ax - mx) + " " + num(ay - my) + " " + num(radius) + " " +
             num(scale) + " " + num(tx) + " " + num(ty) + " " + num(s));
    // запобіжник: помічник не відповів (завис, не зміг зняти знімок) — не лишаємо док у стані «анімація триває»
    g_timeout_add(1500, [](gpointer d) -> gboolean {
        unsigned g = GPOINTER_TO_UINT(d);
        if (g == gen && st != IDLE && !shown) on_osd_line("fail " + std::to_string(g));
        return G_SOURCE_REMOVE;
    }, GUINT_TO_POINTER(gen));
    // док виїжджає з-під краю — іконка рухається; поки пальці на тачпаді, ціль оновлює send_progress
    target_timer = g_timeout_add(33, [](gpointer) -> gboolean { send_progress(false); return G_SOURCE_CONTINUE; }, nullptr);
}

// Super+Q (mgos-close → SIGUSR2 без файлу mgos-dock-restore): активне вікно стікає в док тим самим «джином», але одразу до кінця.
// Якщо анімація неможлива (повноекранне вікно, нема помічника) — старе згасання й перенос у сховище.
static void plain_hide(const std::string& addr) {
    sysui::hypr_request("dispatch setprop address:" + addr + " opacity 0.0 override");
    g_timeout_add_full(G_PRIORITY_DEFAULT, 200, [](gpointer d) -> gboolean {
        // вікно вже згасло — переносимо без анімації відходу (інакше повернута непрозорість його «блимала»)
        std::string a((char*)d), sp = "dispatch setprop address:" + a;
        stash_window(a, sp + " no_anim 1;" + sp + " no_blur 1;" + sp + " no_shadow 1;");
        veil_keep(a);
        return G_SOURCE_REMOVE;
    }, g_strdup(addr.c_str()), g_free);
}

static void minimize_active() {
    if (st == ACTIVE || st == PENDING || waiting_done) return;   // уже щось летить — не перебиваємо
    std::string addr, ws;
    try {
        json aw = json::parse(sysui::hypr_request("j/activewindow"));
        addr = aw.value("address", "");
        ws = aw.contains("workspace") ? aw["workspace"].value("name", "") : "";
    } catch (...) { return; }
    if (addr.empty() || ws == DOCK_STASH) return;
    st = IDLE; vel = 0;
    if (!anim_on()) { plain_hide(addr); return; }   // вимкнено / без анімацій / еко — просто зникає
    begin_minimize();
    if (st != ACTIVE) { st = IDLE; plain_hide(addr); return; }
    target_p = 0;
    release(true);
}

static bool fs_gesture() { const auto& m = sysui::settings().tp_swipe_up; return m == "fullscreen" || m == "maximize"; }

// напрямок — вниз. Повноекранне вікно: короткий змах лише виходить з повного екрана (без «джина»);
// звичайне — «вікно в док», якщо так налаштовано
static void begin_down() {
    if (runner_open()) { runner_hide(); st = IGNORED; return; }
    if (fs_gesture()) {
        json aw;
        try { aw = json::parse(sysui::hypr_request("j/activewindow")); } catch (...) {}
        std::string ws = aw.contains("workspace") ? aw["workspace"].value("name", "") : "";
        if (!aw.value("address", "").empty() && ws != DOCK_STASH && aw.value("fullscreen", 0) != 0) {
            // 0 0 — і для Hyprland, і для самої програми (відео в Firefox теж виходить з повного екрана)
            sysui::hypr_request("dispatch fullscreenstate 0 0");
            st = IGNORED;
            return;
        }
    }
    if (sysui::settings().tp_swipe_down == "minimize") begin_minimize(); else st = IGNORED;
}

// напрямок — угору, пройдено UP_PX. У просторі — повернути картку на стіл; інакше — вікно на весь екран
static void do_up() {
    const auto& m = sysui::settings().tp_swipe_up;
    if (m == "runner") return;                                 // це робить жест Hyprland (touchpad.conf)
    if (cards_gesture_restore()) return;
    if (!fs_gesture()) return;
    if (runner_open()) { runner_hide(); return; }
    json aw;
    try { aw = json::parse(sysui::hypr_request("j/activewindow")); } catch (...) { return; }
    std::string ws = aw.contains("workspace") ? aw["workspace"].value("name", "") : "";
    if (aw.value("address", "").empty() || ws == DOCK_STASH || aw.value("fullscreen", 0) != 0) return;
    sysui::hypr_request(m == "maximize" ? "dispatch fullscreen 1" : "dispatch fullscreen 0");
}

static double dist() { return std::max(120, std::min(800, sysui::settings().tp_min_dist)); }

static void on_event(libinput_event* ev) {
    auto t = libinput_event_get_type(ev);
    if (t != LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN && t != LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE && t != LIBINPUT_EVENT_GESTURE_SWIPE_END) return;
    auto* g = libinput_event_get_gesture_event(ev);
    if (libinput_event_gesture_get_finger_count(g) != 3) return;
    if (t == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN) {
        if (st == ACTIVE || waiting_done) return;   // попередній ще доводиться — не перебиваємо
        if (st == IGNORED && !win_addr.empty()) return;   // чекаємо, поки вікно проявиться після відміни
        st = PENDING; acc_x = acc_y = 0; target_p = 0; vel = 0; last_us = libinput_event_gesture_get_time_usec(g);
        osd_spawn();   // холодний старт помічника ~150 мс — ховаємо його за час, поки визначається напрямок
        return;
    }
    if (t == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE) {
        double dy = libinput_event_gesture_get_dy(g);
        acc_x += libinput_event_gesture_get_dx(g); acc_y += dy;
        uint64_t us = libinput_event_gesture_get_time_usec(g);
        double dt = (us - last_us) / 1000.0; last_us = us;
        if (dt > 0 && dt < 100) vel = vel * 0.6 + (dy / dt) * 0.4;   // згладжена швидкість для змаху
        if (st == PENDING && std::hypot(acc_x, acc_y) >= LOCK_PX) {
            if (std::abs(acc_y) <= std::abs(acc_x) * 1.2) st = IGNORED;   // горизонталь — столи (Hyprland)
            else if (acc_y > 0) begin_down();
            else st = UP_WAIT;
        }
        if (st == UP_WAIT && acc_y <= -UP_PX) { st = IGNORED; do_up(); }
        if (st == ACTIVE && !released) {
            double p = clamp01(acc_y / dist());
            if (p != target_p) { target_p = p; send_progress(true); }
        }
        return;
    }
    // END
    bool cancelled = libinput_event_gesture_get_cancelled(g);
    if (st == ACTIVE && !released) { if (cancelled) target_p = 0; release(); }
    else if (st == PENDING || st == UP_WAIT || (st == IGNORED && win_addr.empty())) st = IDLE;
}

// ---------- libinput лише на тачпад ----------
static int open_restricted(const char* path, int flags, void*) { int fd = open(path, flags | O_CLOEXEC); return fd < 0 ? -errno : fd; }
static void close_restricted(int fd, void*) { close(fd); }
static const libinput_interface IFACE = {open_restricted, close_restricted};

static gboolean on_li(gint, GIOCondition cond, gpointer) {
    if (cond & (G_IO_ERR | G_IO_HUP)) return G_SOURCE_CONTINUE;
    libinput_dispatch(li);
    while (libinput_event* ev = libinput_get_event(li)) { on_event(ev); libinput_event_destroy(ev); }
    return G_SOURCE_CONTINUE;
}

static std::set<std::string> li_added;   // вузли, які вже слухаємо (Bluetooth-тачпад/Magic Trackpad може з'явитися пізніше)

static void li_close() {
    if (li_watch) { g_source_remove(li_watch); li_watch = 0; }
    if (li) { libinput_unref(li); li = nullptr; li_added.clear(); }
}

static std::vector<std::string> li_scan_nodes() {
    std::vector<std::string> nodes;
    udev* u = udev_new();
    if (!u) return nodes;
    udev_enumerate* en = udev_enumerate_new(u);
    udev_enumerate_add_match_subsystem(en, "input");
    udev_enumerate_add_match_property(en, "ID_INPUT_TOUCHPAD", "1");
    udev_enumerate_scan_devices(en);
    udev_list_entry* e;
    udev_list_entry_foreach(e, udev_enumerate_get_list_entry(en)) {
        udev_device* d = udev_device_new_from_syspath(u, udev_list_entry_get_name(e));
        const char* n = d ? udev_device_get_devnode(d) : nullptr;
        if (n && strstr(n, "/event")) nodes.push_back(n);
        if (d) udev_device_unref(d);
    }
    udev_enumerate_unref(en);
    udev_unref(u);
    return nodes;
}


// додає нові тачпади до вже відкритого контексту (хот-плаг); повертає, скільки додано
static int li_add_new() {
    int added = 0;
    for (auto& n : li_scan_nodes()) {
        if (li_added.count(n)) continue;
        if (libinput_path_add_device(li, n.c_str())) { li_added.insert(n); added++; }
    }
    return added;
}

static bool li_open() {
    auto nodes = li_scan_nodes();
    if (nodes.empty()) return false;
    li = libinput_path_create_context(&IFACE, nullptr);
    if (!li) return false;
    li_added.clear();
    if (!li_add_new()) { libinput_unref(li); li = nullptr; return false; }
    li_watch = g_unix_fd_add(libinput_get_fd(li), (GIOCondition)(G_IO_IN | G_IO_ERR | G_IO_HUP), on_li, nullptr);
    on_li(0, G_IO_IN, nullptr);   // з'їсти DEVICE_ADDED
    return true;
}

// вмикаємо/вимикаємо разом із налаштуванням (перевірка раз на 2 с — mtime settings.json кешується в sysui)
static gboolean sync_setting(gpointer) {
    const auto& S = sysui::settings();
    bool want = S.tp_swipe_down == "minimize" || fs_gesture() || S.dock_stash_cards;   // хоч один жест читаємо самі
    if (want && !li) li_open();   // тачпада ще нема — спробуємо через 2 с
    else if (want && li && li_add_new()) on_li(0, G_IO_IN, nullptr);   // підключили ще один тачпад (Magic Trackpad)
    else if (!want && li) {
        li_close();
        if (st != IDLE) { if (hidden_real) { set_real_opacity(true); anim_back(win_addr); } finish(); }
        if (osd) g_subprocess_force_exit(osd);   // помічник більше не потрібен
    }
    return G_SOURCE_CONTINUE;
}

// ---------- запуск з доку: вікно «виростає» з іконки (зворотна «здача») ----------
// Клік по іконці незапущеної програми → чекаємо її перше вікно (openwindow) до 10 с. Вікно одразу робимо прозорим
// (no_anim — без «popin» Hyprland), за ~0,25 с, коли програма намалювала вміст, помічник знімає саме вікно (grim -T)
// і вирощує знімок з іконки до місця вікна; потім вікно миттєво проявляється під знімком (як відміна жесту).
// Перемикач «Відкриття з іконки» (genie_grow); без анімацій / еко — нічого не робимо, вікно з'являється як зазвичай.
static std::string grow_want; static gint64 grow_deadline = 0;

static void grow_expect(const std::string& id) {
    if (!anim_on() || !sysui::settings().genie_grow || id.empty()) return;
    grow_want = to_lower(id);
    grow_deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
}

static gboolean grow_go(gpointer d) {
    if (GPOINTER_TO_UINT(d) != gen || st != GROW) return G_SOURCE_REMOVE;
    json cl, mons;
    try { cl = json::parse(sysui::hypr_request("j/clients")); mons = json::parse(sysui::hypr_request("j/monitors")); } catch (...) { grow_reveal(); return G_SOURCE_REMOVE; }
    const json* w = nullptr;
    for (auto& c : cl) if (c.value("address", "") == win_addr) { w = &c; break; }
    if (!w || w->value("fullscreen", 0) != 0 || !w->contains("at") || !w->contains("size")) { grow_reveal(); return G_SOURCE_REMOVE; }
    double mx = 0, my = 0, scale = 1; mon_w = 1920; mon_h = 1080;
    std::string mon_name;
    int mid = w->value("monitor", -1);
    for (auto& m : mons) if (m.value("id", -2) == mid) {
        mx = m.value("x", 0); my = m.value("y", 0); scale = m.value("scale", 1.0); mon_name = m.value("name", "");
        double pw = m.value("width", 1920), ph = m.value("height", 1080);
        if (m.value("transform", 0) % 2) std::swap(pw, ph);
        mon_w = (int)std::lround(pw / scale); mon_h = (int)std::lround(ph / scale);
    }
    int ax = (*w)["at"][0], ay = (*w)["at"][1], sw_ = (*w)["size"][0], sh = (*w)["size"][1];
    if (sw_ < 20 || sh < 20) { grow_reveal(); return G_SOURCE_REMOVE; }
    double radius = 0;
    try { radius = json::parse(sysui::hypr_request("j/getoption decoration:rounding")).value("int", 0); } catch (...) {}
    double tx, ty, s; target(tx, ty, s);
    last_tx = tx; last_ty = ty; last_s = s;
    double ms = std::clamp(420 * 100.0 / std::clamp(sysui::settings().genie_speed, 50, 200), 150.0, 1400.0);
    osd_send(std::string("plain ") + (sysui::settings().genie_anim ? "0" : "1"));
    osd_send("grow " + std::to_string(gen) + " " + (mon_name.empty() ? "-" : mon_name) + " " + std::to_string(sw_) + " " + std::to_string(sh) +
             " " + num(ax - mx) + " " + num(ay - my) + " " + num(radius) + " " + num(scale) + " " + num(tx) + " " + num(ty) + " " + num(s) +
             " " + std::to_string((int)ms) + " " + win_addr + " " + (win_cls.empty() ? "-" : win_cls));
    // док ще може виїжджати — доносимо позицію іконки до кінця росту
    target_timer = g_timeout_add(33, [](gpointer) -> gboolean { send_progress(false); return G_SOURCE_CONTINUE; }, nullptr);
    return G_SOURCE_REMOVE;
}

static void grow_begin(const std::string& addr, const std::string& c) {
    if (!anim_on() || !sysui::settings().genie_grow) return;
    if (st != IDLE || waiting_done) return;   // триває жест — вікно просто з'являється
    win_addr = addr; win_cls = c;
    set_real_opacity(false);   // одразу, до першого кадру «popin»
    st = GROW; waiting_done = true; released = true; committed = false; shown = false; target_p = 1;
    g_genie_force_dock = true;   // іконка має бути видно — з неї росте вікно
    gen++;
    osd_spawn();
    if (!osd) { grow_reveal(); return; }
    g_timeout_add(260, grow_go, GUINT_TO_POINTER(gen));
    // запобіжник: помічник мовчить — вікно не лишається невидимим
    g_timeout_add(3000, [](gpointer d) -> gboolean {
        unsigned g = GPOINTER_TO_UINT(d);
        if (g == gen && st == GROW && !shown) grow_reveal();
        return G_SOURCE_REMOVE;
    }, GUINT_TO_POINTER(gen));
}

static void grow_on_open(const std::string& addr, const std::string& ws, const std::string& cls) {
    if (grow_want.empty()) return;
    if (g_get_monotonic_time() > grow_deadline) { grow_want.clear(); return; }
    std::string c = to_lower(cls);
    if (c.empty() || (c != grow_want && c.find(grow_want) == std::string::npos && grow_want.find(c) == std::string::npos)) return;
    grow_want.clear();
    if (ws == DOCK_STASH) return;
    grow_begin(addr, c);
}

// повернення вікна зі сховища (клік по згорнутій іконці): теж росте з іконки
static void grow_restore(const std::string& addr) {
    std::string cls;
    try { for (auto& c : json::parse(sysui::hypr_request("j/clients"))) if (c.value("address", "") == addr) { cls = to_lower(c.value("class", "")); break; } } catch (...) {}
    if (cls.empty()) return;
    grow_begin(addr, cls);
}
// SIGUSR2 ділять Alt+Tab (switcher.py пише адресу у mgos-dock-restore — dock.cpp повертає вікно зі сховища) і Super+Q
// (mgos-close стирає цей файл перед сигналом). Є файл — це Alt+Tab, ховати активне вікно не треба.
static void genie_usr2() {
    const char* rd = getenv("XDG_RUNTIME_DIR");
    if (access((std::string(rd ? rd : "/tmp") + "/mgos-dock-restore").c_str(), F_OK) == 0) return;
    minimize_active();
}
static void init() {
    g_unix_signal_add(SIGUSR2, [](gpointer) -> gboolean { genie_usr2(); return G_SOURCE_CONTINUE; }, nullptr);
    sync_setting(nullptr);
    g_timeout_add_seconds(2, sync_setting, nullptr);
}

}  // namespace genie
