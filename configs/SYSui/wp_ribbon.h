// wp_ribbon.h — «Стрічка шпалер»: клік по бульбашці годинника в topbar → бульбашка пружинно
// розростається в скляну панель на верхню половину екрана з каруселлю нахилених карток.
// Підключається в wallpapers.cpp (після apply_wallpaper/get_hash). Жодних фонових потоків, поки
// панель закрита: завантажувач мініатюр живе лише між відкриттям і закриттям, пам'ять звільняється.
// Протокол: у wallpapers.sock — «ribbon X Y W H CONNECTOR» (геометрія бульбашки в координатах монітора).
#pragma once
#include <condition_variable>
#include <deque>
#include <memory>
#include <regex>
#include <set>
#include <map>
#include <cmath>

namespace ribbon {

enum Kind { K_IMG, K_VID, K_DYN, K_PIX };
struct Item {
    std::string path;          // файл / тека (динамічні) / "pixel:<id>"
    std::string name, fold;    // назва і її casefold для пошуку
    Kind kind = K_IMG;
    int color = -1;            // індекс кружечка палітри (-1 — ще невідомо)
    int dom = -1;              // упакований домінантний колір (див. classify), не залежить від палітри
    cairo_surface_t* surf = nullptr;
    GdkTexture* tex = nullptr;   // та сама картинка на GPU (шар карток) — створюється при першому показі
    bool requested = false, failed = false;
};
struct Loader {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<int, bool>> q;               // (індекс, потрібна картинка; false — лише колір)
    std::atomic<bool> stop{false};
    std::vector<std::pair<std::string, int>> jobs;   // копія (шлях, тип) — потік не чіпає items
    std::string theme, accent;
};
struct Result { std::shared_ptr<Loader> L; int idx; cairo_surface_t* s; int color; bool want; };

static const int WIN = 8;   // скільки карток з кожного боку тримати декодованими
static const double SKEW = 0.2;
// Кружечки-фільтри = палітра pywal (~/.cache/wal/colors, color1..color7) + акцент системи.
// Домінантний колір картинки кешується як «відтінок + світлість» (не кошик), тож категорія
// перераховується під будь-яку палітру: найближчий відтінок, сірий — за низькою насиченістю.
static void hex_rgb(const std::string& h, double& r, double& g, double& b);
struct PalC { double r, g, b, hue, sat, lum; bool gray; std::string hex, name; };
static std::vector<PalC> pal;
static int pal_gray = -1;          // індекс сірого кружечка
static time_t pal_mtime = -1; static std::string pal_accent;
static const int DOM_GRAY = 400;   // упакований домінант: код (0..359 відтінок, 400 сірий) + 1000*світлість(0..100)
static void rgb_hsl(double r, double g, double b, double& h, double& s, double& l) {
    double mx = std::max({r, g, b}), mn = std::min({r, g, b}), c = mx - mn;
    l = (mx + mn) / 2; s = c < 1e-6 ? 0 : c / (1 - std::fabs(2 * l - 1));
    if (c < 1e-6) h = 0;
    else if (mx == r) h = 60.0 * std::fmod((g - b) / c + 6, 6);
    else if (mx == g) h = 60.0 * ((b - r) / c + 2);
    else h = 60.0 * ((r - g) / c + 4);
}
static std::string hue_name(const PalC& c) {   // назва за відтінком, українською
    if (c.gray) return c.lum > 0.85 ? "Білі" : c.lum < 0.18 ? "Темні" : "Сірі";
    double h = c.hue;
    const char* n = h < 12 || h >= 345 ? "Червоні" : h < 34 ? (c.lum > 0.7 ? "Персикові" : "Помаранчеві") : h < 68 ? (c.lum > 0.75 ? "Кремові" : "Жовті")
                  : h < 160 ? "Зелені" : h < 195 ? "Бірюзові" : h < 255 ? "Сині" : h < 285 ? "Фіолетові" : h < 320 ? "Бузкові" : "Рожеві";
    return n;
}
static double hue_d(double a, double b) { double d = std::fabs(a - b); return d > 180 ? 360 - d : d; }
static PalC mk_pal(const std::string& hex) {
    PalC c{}; c.hex = hex; hex_rgb(hex, c.r, c.g, c.b);
    rgb_hsl(c.r, c.g, c.b, c.hue, c.sat, c.lum);
    c.gray = c.sat < 0.12 || (std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b})) < 0.06;
    return c;
}
// true — палітра змінилась (перечитує лише при зміні файлу wal чи акценту)
static bool load_palette(bool force = false) {
    std::string wf = sysui::home() + "/.cache/wal/colors";
    time_t mt = sysui::file_mtime(wf); std::string acc = sysui::accent();
    if (!force && mt == pal_mtime && acc == pal_accent && !pal.empty()) return false;
    pal_mtime = mt; pal_accent = acc;
    std::vector<std::string> hx;
    { std::ifstream f(wf); std::string l; int n = 0;
      while (std::getline(f, l) && n < 8) { if (n++ >= 1 && l.size() >= 7 && l[0] == '#') hx.push_back(l.substr(0, 7)); } }
    pal.clear(); pal_gray = -1;
    auto add = [](const std::string& h, double tol) {
        PalC c = mk_pal(h);
        for (auto& o : pal) if (std::fabs(o.r - c.r) + std::fabs(o.g - c.g) + std::fabs(o.b - c.b) < tol) return;   // дубль / майже дубль
        if (c.gray && pal_gray >= 0) return;   // сірий — лише один
        if (c.gray) pal_gray = (int)pal.size();
        pal.push_back(c);
    };
    for (auto& h : hx) add(h, 0.08);
    if (acc.size() >= 7) add(acc.substr(0, 7), 0.3);   // акцент — лише якщо помітно відрізняється від палітри
    if (pal_gray < 0) {   // у палітрі немає сірого — додаємо знебарвлений color7 (або середній сірий)
        PalC b = hx.size() >= 7 ? mk_pal(hx[6]) : mk_pal("#9a9a9e");
        double v = (b.r + b.g + b.b) / 3; char buf[8];
        snprintf(buf, sizeof buf, "#%02x%02x%02x", (int)(v * 255), (int)(v * 255), (int)(v * 255) + 4 > 255 ? 255 : (int)(v * 255) + 4);
        add(buf, 0);
    }
    for (auto& c : pal) c.name = hue_name(c);
    return true;
}
// упакований домінант → індекс кружечка (-1 — ще невідомо)
static int classify(int dom) {
    if (dom < 0 || pal.empty()) return -1;
    int code = dom % 1000; double lum = (dom / 1000) / 100.0;
    if (code == DOM_GRAY) return pal_gray;
    int best = -1; double bd = 1e9;
    for (size_t i = 0; i < pal.size(); i++) {
        if (pal[i].gray) continue;
        double d = hue_d(code, pal[i].hue) + 25 * std::fabs(lum - pal[i].lum);   // світлість — лише як розв'язка близьких відтінків
        if (d < bd) { bd = d; best = (int)i; }
    }
    return best < 0 ? pal_gray : best;
}

static GtkWidget *win = nullptr, *area = nullptr;
// Два шари: area — скло, пілюля, події миші (між кадрами прокрутки не змінюється — GTK бере готову растеризацію);
// cards_area — лише смуга каруселі. Раніше одна область на весь екран 4K перерастеризовувалась щокадру (~30 к/с).
static GtkWidget* cards_area = nullptr;
static int layer = 0;              // 0 — малюємо area, 1 — cards_area
static double cards_y = 0;   // зсув шару карток відносно area (зараз 0 — шар на весь екран)
static bool settled_now = false;
struct CarGeo { double cx = 0, cy = 0, Hc = 0, gap = 16, tx = 0, ty = 0, tw = 0, th = 0, px = 0, py = 0, pw = 0, ph = 0, pr = 0; };
static CarGeo cg;
static void qd() { if (area) gtk_widget_queue_draw(area); if (cards_area) gtk_widget_queue_draw(cards_area); }
static std::vector<Item> items;
static std::vector<int> view, vpos;
static std::shared_ptr<Loader> loader;
static double bx = 0, by = 6, bw = 120, bh = 34;
static double p = 0, pv = 0, p_target = 0;
static double pos = 0, posv = 0, target = 0;
static guint tick_id = 0, snap_id = 0;
static gint64 last_t = 0;
static int last_center = -1;
static bool opened = false, light = false, colors_dirty = false;
static double acc_r = 0.04, acc_g = 0.52, acc_b = 1;
static std::string accent_hex = "#0a84ff";
static bool f_recent = false; static int f_kind = -1, f_color = -1;
static std::string query; static bool search_on = false;
static std::vector<std::string> recent;
static std::string current;
static double mx = -1, my = -1;
static int hover_card = -1, hover_btn = -1;
struct Hit { double x, y, w, h; int id; };
static std::vector<Hit> hits;
struct Geo { int vi; double xc, cy, w, h; };
static std::vector<Geo> geos;   // у порядку малювання (останній — найближчий)
static double drag_start = 0, step_px = 300; static bool dragged = false;

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static std::string sq(const std::string& s) {   // для одинарних лапок у shell
    std::string o; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o;
}
static std::string thumb_dir() { return sysui::home() + "/.cache/mgos/wp_ribbon"; }
// v2: «відтінок+світлість» замість старих 8 кошиків (colors.txt більше не читається)
static std::string colors_file() { return sysui::home() + "/.cache/mgos/wp_ribbon/colors_v2.txt"; }
static std::string recent_file() { return sysui::home() + "/.cache/mgos/wp_recent.txt"; }
static std::string fold(const std::string& s) {
    gchar* f = g_utf8_casefold(s.c_str(), -1); std::string r = f ? f : s; g_free(f); return r;
}
static void hex_rgb(const std::string& h, double& r, double& g, double& b) {
    unsigned v = 0x8a8a8e; if (h.size() >= 7) v = strtoul(h.c_str() + 1, nullptr, 16);
    r = ((v >> 16) & 255) / 255.0; g = ((v >> 8) & 255) / 255.0; b = (v & 255) / 255.0;
}

// ---------- мініатюри (потік завантажувача) ----------
static std::string make_thumb(const std::string& path, int kind, const std::string& theme, const std::string& accent) {
    std::error_code ec;
    if (kind == K_PIX) {
        std::string id = path.substr(6), dir = sysui::home() + "/.cache/megumin-os/pixel_previews";
        std::string hex = fold(accent.size() > 1 ? accent.substr(1) : "0a84ff");
        std::string fallback;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            std::string n = e.path().filename().string(), suf = "_" + theme + ".png";
            if (n.size() <= id.size() + suf.size() + 1 || n.compare(0, id.size() + 1, id + "_") ||
                n.compare(n.size() - suf.size(), suf.size(), suf)) continue;
            std::string mid = n.substr(id.size() + 1, n.size() - id.size() - 1 - suf.size());
            if (mid.find('_') != std::string::npos) continue;
            if (fold(mid) == hex) return e.path().string();
            if (fallback.empty()) fallback = e.path().string();
        }
        if (!fallback.empty()) return fallback;
        fs::create_directories(dir, ec);
        std::string out = dir + "/" + id + "_" + hex + "_" + theme + ".png";
        exec_cmd("timeout 20 '" + sysui::home() + "/.config/SYSui/pixel_wallpaper' --poster '" + out + "' 400x224 --style " + id +
                 " --color '#" + hex + "' --theme " + theme);
        return fs::exists(out) ? out : "";
    }
    std::string src = path;
    if (kind == K_DYN) {
        src.clear();
        for (const char* n : {"/day.jpg", "/day.png", "/day.jpeg", "/day.webp"}) if (fs::exists(path + n)) { src = path + n; break; }
        if (src.empty()) for (auto& e : fs::directory_iterator(path, ec))
            if (has_ext(e.path().string(), {"jpg", "jpeg", "png", "webp"})) { src = e.path().string(); break; }
        if (src.empty()) return "";
    }
    std::string out = thumb_dir() + "/" + get_hash(path) + ".jpg";
    if (fs::exists(out) && sysui::file_mtime(out) >= sysui::file_mtime(src)) return out;
    if (kind == K_VID) {
        const std::string vf = " -frames:v 1 -vf 'scale=800:500:force_original_aspect_ratio=increase,crop=800:500' -q:v 4 '" + sq(out) + "' -y";
        exec_cmd("timeout 20 nice -n 10 ffmpeg -loglevel error -ss 1 -i '" + sq(src) + "'" + vf);
        if (!fs::exists(out)) exec_cmd("timeout 20 nice -n 10 ffmpeg -loglevel error -i '" + sq(src) + "'" + vf);
    } else {
        exec_cmd("timeout 20 nice -n 10 magick '" + sq(src) + "[0]' -resize 800x500^ -gravity center -extent 800x500 -quality 85 '" + sq(out) + "'");
    }
    return fs::exists(out) ? out : "";
}

// Декодування + домінантний колір (гістограма відтінків, зважена насиченістю)
static cairo_surface_t* decode(const std::string& f, int& color, bool want) {
    GdkPixbuf* pb = gdk_pixbuf_new_from_file(f.c_str(), nullptr);
    if (!pb) return nullptr;
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), rs = gdk_pixbuf_get_rowstride(pb);
    int nc = gdk_pixbuf_get_n_channels(pb);
    const guchar* px = gdk_pixbuf_read_pixels(pb);
    cairo_surface_t* s = want ? cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h) : nullptr;
    unsigned char* dst = nullptr; int ds = 0;
    if (s) { cairo_surface_flush(s); dst = cairo_image_surface_get_data(s); ds = cairo_image_surface_get_stride(s); }
    double hist[36] = {0}, hl[36] = {0}, chroma = 0, total = 0;
    for (int y = 0; y < h; y++) {
        const guchar* row = px + (size_t)y * rs;
        for (int x = 0; x < w; x++) {
            int r = row[x * nc], g = row[x * nc + 1], b = row[x * nc + 2];
            if (dst) *(uint32_t*)(dst + (size_t)y * ds + x * 4) = 0xff000000u | (r << 16) | (g << 8) | b;
            if ((x | y) & 3) continue;
            total += 1;
            int mxv = std::max({r, g, b}), mnv = std::min({r, g, b}), c = mxv - mnv;
            if (c < 38 || mxv < 45) continue;
            double hue;
            if (mxv == r) hue = 60.0 * std::fmod((double)(g - b) / c + 6, 6);
            else if (mxv == g) hue = 60.0 * ((double)(b - r) / c + 2);
            else hue = 60.0 * ((double)(r - g) / c + 4);
            int k = (int)(hue / 10) % 36;
            double wgt = c / 255.0; hist[k] += wgt; hl[k] += wgt * (mxv + mnv) / 510.0; chroma += wgt;
        }
    }
    if (s) cairo_surface_mark_dirty(s);
    g_object_unref(pb);
    // найсильніше вікно з 3 сусідніх кошиків по 10° → зважений відтінок і світлість
    int best = 0; double bv = -1;
    for (int k = 0; k < 36; k++) { double v = hist[(k + 35) % 36] + hist[k] + hist[(k + 1) % 36]; if (v > bv) { bv = v; best = k; } }
    double sx = 0, sy = 0, sl = 0, sw = 0;
    for (int d = -1; d <= 1; d++) { int k = (best + d + 36) % 36; double a = (k * 10 + 5) * M_PI / 180;
        sx += hist[k] * cos(a); sy += hist[k] * sin(a); sl += hl[k]; sw += hist[k]; }
    if (total <= 0 || chroma / total < 0.07 || sw <= 0) color = DOM_GRAY;
    else {
        int h = (int)lround(atan2(sy, sx) * 180 / M_PI); h = (h % 360 + 360) % 360;
        color = h + 1000 * (int)lround(clamp01(sl / sw) * 100);
    }
    return s;
}

static gboolean on_result(gpointer d);
static void loader_run(std::shared_ptr<Loader> L) {
    for (;;) {
        std::pair<int, bool> job;
        {
            std::unique_lock<std::mutex> lk(L->m);
            L->cv.wait(lk, [&] { return L->stop.load() || !L->q.empty(); });
            if (L->stop) return;
            job = L->q.front(); L->q.pop_front();
        }
        const auto& jb = L->jobs[job.first];
        std::string t = make_thumb(jb.first, jb.second, L->theme, L->accent);
        if (L->stop) return;
        int col = -1;
        cairo_surface_t* s = t.empty() ? nullptr : decode(t, col, job.second);
        g_idle_add(on_result, new Result{L, job.first, s, col, job.second});
    }
}

// ---------- дані ----------
static std::vector<std::pair<std::string, std::string>> pixel_styles() {
    std::ifstream f(sysui::home() + "/.config/ignis/settings.py");
    std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str();
    std::vector<std::pair<std::string, std::string>> out;
    size_t a = s.find("PIXEL_STYLES = [");
    if (a == std::string::npos) return out;
    size_t b = s.find("TP_DEFAULTS", a);
    std::string sub = s.substr(a, b == std::string::npos ? 4000 : b - a);
    static const std::regex re("\\(\"([a-z0-9_]+)\",\\s*\"([^\"]+)\"\\)");
    std::set<std::string> seen;
    for (std::sregex_iterator it(sub.begin(), sub.end(), re), e; it != e; ++it)
        if (seen.insert((*it)[1]).second) out.push_back({(*it)[1], (*it)[2]});
    return out;
}
static void save_recent() {
    std::ofstream o(recent_file()); for (auto& r : recent) o << r << "\n";
}
static void build_items() {
    items.clear();
    std::vector<std::string> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(WALLPAPER_DIR, ec)) {
        std::string p = e.path().string();
        if (has_ext(p, {"png", "jpg", "jpeg", "webp", "mp4", "webm", "gif", "mkv", "mov"}) || fs::is_directory(p)) files.push_back(p);
    }
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    for (auto& f : files) {
        Item it; it.path = f;
        it.kind = fs::is_directory(f) ? K_DYN : has_ext(f, {"mp4", "webm", "gif", "mkv", "mov"}) ? K_VID : K_IMG;
        it.name = fs::path(f).stem().string();
        for (char& c : it.name) if (c == '-' || c == '_') c = ' ';
        items.push_back(it);
    }
    for (auto& [id, name] : pixel_styles()) { Item it; it.path = "pixel:" + id; it.name = name; it.kind = K_PIX; items.push_back(it); }
    std::map<std::string, int> cols;
    { std::ifstream f(colors_file()); std::string l;
      while (std::getline(f, l)) { size_t t = l.find('\t'); if (t != std::string::npos) cols[l.substr(t + 1)] = atoi(l.c_str()); } }
    for (auto& it : items) { it.fold = fold(it.name); auto c = cols.find(it.path); if (c != cols.end()) { it.dom = c->second; it.color = classify(it.dom); } }
    recent.clear();
    { std::ifstream f(recent_file()); std::string l; while (std::getline(f, l)) if (!l.empty()) recent.push_back(l); }
    current.clear();
    { std::ifstream f(LAST_WP_FILE); std::getline(f, current); }
    if (!current.empty() && std::find(recent.begin(), recent.end(), current) == recent.end()) recent.insert(recent.begin(), current);
}
static void save_colors() {
    if (!colors_dirty) return;
    std::ofstream o(colors_file());
    for (auto& it : items) if (it.dom >= 0) o << it.dom << "\t" << it.path << "\n";
    colors_dirty = false;
}
static bool match(const Item& it) {
    if (f_kind == K_VID && it.kind != K_VID) return false;
    if (f_kind == K_PIX && it.kind != K_PIX) return false;
    if (f_color >= 0 && it.color != f_color) return false;
    if (!query.empty() && it.fold.find(fold(query)) == std::string::npos) return false;
    return true;
}
static bool near(int idx) {
    if (idx < 0 || idx >= (int)vpos.size() || vpos[idx] < 0) return false;
    return std::abs(vpos[idx] - (int)lround(pos)) <= WIN;
}
static void update_window() {
    if (!loader) return;
    for (auto& it : items) if (it.surf && !near(&it - &items[0])) { cairo_surface_destroy(it.surf); it.surf = nullptr; g_clear_object(&it.tex); }
    std::vector<int> want;
    int c = (int)lround(pos);
    for (int d = 0; d <= WIN; d++) for (int sg : {1, -1}) {
        if (d == 0 && sg < 0) continue;
        int vi = c + d * sg;
        if (vi < 0 || vi >= (int)view.size()) continue;
        Item& it = items[view[vi]];
        if (!it.surf && !it.requested && !it.failed) { it.requested = true; want.push_back(view[vi]); }
    }
    std::lock_guard<std::mutex> lk(loader->m);
    for (auto i = loader->q.begin(); i != loader->q.end();) {   // далекі запити картинок — геть
        if (i->second && !near(i->first)) { items[i->first].requested = false; i = loader->q.erase(i); } else ++i;
    }
    for (auto i = want.rbegin(); i != want.rend(); ++i) loader->q.push_front({*i, true});
    if (!want.empty()) loader->cv.notify_one();
}
static void refilter() {
    int keep = view.empty() ? -1 : view[std::clamp((int)lround(pos), 0, (int)view.size() - 1)];
    view.clear();
    if (f_recent) {
        for (auto& r : recent) for (size_t i = 0; i < items.size(); i++) if (items[i].path == r && match(items[i])) view.push_back(i);
    } else for (size_t i = 0; i < items.size(); i++) if (match(items[i])) view.push_back(i);
    vpos.assign(items.size(), -1);
    for (size_t v = 0; v < view.size(); v++) vpos[view[v]] = v;
    int np = (keep >= 0 && vpos[keep] >= 0) ? vpos[keep] : 0;
    if (np != (int)lround(pos)) { pos = target = np; posv = 0; }
    target = std::clamp(target, 0.0, std::max(0.0, (double)view.size() - 1));
    last_center = -1;
    update_window();
    qd();
}

// ---------- анімація ----------
static void finish_close();
static void spring(double& x, double& v, double to, double k, double c, double dt) {
    for (int i = 0; i < 4; i++) { double h = dt / 4, a = k * (to - x) - c * v; v += a * h; x += v * h; }
}
static gboolean on_tick(GtkWidget*, GdkFrameClock* fc, gpointer) {
    gint64 t = gdk_frame_clock_get_frame_time(fc);
    double dt = last_t ? std::min(0.05, (t - last_t) / 1e6) : 1 / 60.0;
    static const bool prof = getenv("MGOS_RIBBON_PROFILE") != nullptr;
    if (prof && last_t) {   // інтервали кадрів: що бачить людина (растеризація cairo-вузла йде вже після draw)
        static int n = 0, slow = 0; static double sum = 0, mx = 0; double ms = (t - last_t) / 1000.0;
        sum += ms; mx = std::max(mx, ms); slow += ms > 25; 
        if (++n == 60) { fprintf(stderr, "ribbon frames: avg %.1f ms (%.0f fps), max %.1f ms, >25ms: %d/60\n", sum / 60, 60000 / sum, mx, slow); n = slow = 0; sum = mx = 0; }
    }
    last_t = t;
    bool anim = sysui::settings().animations;
    if (!anim) { p = p_target; pv = 0; pos = target; posv = 0; }
    else {
        if (p_target > 0.5) spring(p, pv, p_target, 170, 19, dt);     // розгортання з легким перебігом
        // згортання — та сама затухаюча пружина, що в розширеного годинника (clock_popover.h spring_close:
        // a=6, b=7 за 540 мс → k≈291, c≈22): схлопується, трохи «пролітає» бульбашку і осідає
        else spring(p, pv, p_target, 291, 22.2, dt);
        spring(pos, posv, target, 230, 2 * std::sqrt(230.0) * 0.92, dt);
    }
    if ((int)lround(pos) != last_center) { last_center = (int)lround(pos); update_window(); }
    bool sp = std::fabs(p - p_target) < 0.002 && std::fabs(pv) < 0.02;
    bool ss = std::fabs(pos - target) < 0.002 && std::fabs(posv) < 0.02;
    if (sp) { p = p_target; pv = 0; }
    if (ss) { pos = target; posv = 0; }
    if (settled_now && sp && p_target == 1 && cards_area) gtk_widget_queue_draw(cards_area); else qd();
    if (p_target == 0 && sp) { tick_id = 0; last_t = 0; finish_close(); return G_SOURCE_REMOVE; }
    if (sp && ss) { tick_id = 0; last_t = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static void kick() {
    if (!tick_id && area) { last_t = 0; tick_id = gtk_widget_add_tick_callback(area, on_tick, nullptr, nullptr); }
}
static void go(double t) {
    if (view.empty()) return;
    target = std::clamp(t, 0.0, (double)view.size() - 1);
    kick();
}

// ---------- малювання ----------
struct R { double x, y, w, h, r; };
static R target_rect(double W, double H) {
    double y = std::max(4.0, by);
    return {24, y, W - 48, std::max(330.0, H * 0.5 - y), 30};
}
static R panel_rect(double W, double H) {
    R T = target_rect(W, H);
    if (p < 0) {   // перебіг пружини за бульбашку: стискаємо саму бульбашку навколо її центру (як годинник, до −30%)
        // стрічка в ~45 разів ширша за бульбашку: нахил «як при розгортанні» впирав би перебіг у −30% одразу,
        // і бульбашка «зависала» стиснутою. Фіксований коефіцієнт — плавне «пружне» стискання до ~−18% і назад
        double s = std::max(-0.3, p * 2.6);
        double w = std::max(8.0, bw * (1 + s)), h = std::max(8.0, bh * (1 + s));
        return {bx + (bw - w) / 2, by + (bh - h) / 2, w, h, h / 2};
    }
    double q = p, qh = p - 0.10 * std::sin(M_PI * clamp01(p));   // висота трохи відстає — «стікає» вниз
    return {bx + (T.x - bx) * q, by + (T.y - by) * q,
            bw + (T.w - bw) * q, bh + (T.h - bh) * qh, bh / 2 + (T.r - bh / 2) * clamp01(p)};
}
static void rrect(cairo_t* cr, double x, double y, double w, double h, double r) {
    r = std::max(0.0, std::min({r, w / 2, h / 2}));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}
static void fg(cairo_t* cr, double a) { if (light) cairo_set_source_rgba(cr, 0.11, 0.11, 0.12, a); else cairo_set_source_rgba(cr, 0.96, 0.96, 0.97, a); }
static double text(cairo_t* cr, const std::string& s, double x, double y, double size, bool bold, int align, double maxw = -1, bool draw = true) {
    PangoLayout* l = gtk_widget_create_pango_layout(area, s.c_str());
    PangoFontDescription* fd = pango_font_description_copy(pango_context_get_font_description(pango_layout_get_context(l)));
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    if (maxw > 0) { pango_layout_set_width(l, (int)(maxw * PANGO_SCALE)); pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END); }
    int tw, th; pango_layout_get_pixel_size(l, &tw, &th);
    if (draw) { cairo_move_to(cr, align == 1 ? x - tw / 2.0 : x, y - th / 2.0); pango_cairo_show_layout(cr, l); }
    g_object_unref(l);
    return tw;
}
// Значки пілюлі (власні, прості фігури)
static void icon(cairo_t* cr, int id, double cx, double cy) {
    cairo_save(cr);
    cairo_set_line_width(cr, 1.6); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (id == 2) { for (int i = 0; i < 4; i++) rrect(cr, cx - 6 + (i % 2) * 7, cy - 6 + (i / 2) * 7, 5, 5, 1.3); cairo_fill(cr); }
    else if (id == 3) { cairo_move_to(cr, cx - 4, cy - 6); cairo_line_to(cr, cx + 6, cy); cairo_line_to(cr, cx - 4, cy + 6); cairo_close_path(cr); cairo_fill(cr); }
    else if (id == 4) { for (int i = 0; i < 9; i++) if (i % 2 == 0 || i == 4) { cairo_rectangle(cr, cx - 6 + (i % 3) * 4.3, cy - 6 + (i / 3) * 4.3, 3.4, 3.4); } cairo_fill(cr); }
    else if (id == 5) { cairo_arc(cr, cx, cy, 6.5, 0, 2 * M_PI); cairo_stroke(cr); cairo_move_to(cr, cx, cy - 3.5); cairo_line_to(cr, cx, cy); cairo_line_to(cr, cx + 2.8, cy + 1.6); cairo_stroke(cr); }
    else if (id == 6) { cairo_arc(cr, cx - 1.5, cy - 1.5, 4.6, 0, 2 * M_PI); cairo_stroke(cr); cairo_move_to(cr, cx + 2, cy + 2); cairo_line_to(cr, cx + 6, cy + 6); cairo_stroke(cr); }
    cairo_restore(cr);
}
static int tip_i = -1; static double tip_x = 0, tip_y = 0;
static void refilter();
static void reclassify() {
    accent_hex = sysui::accent(); hex_rgb(accent_hex, acc_r, acc_g, acc_b);
    for (auto& it : items) it.color = classify(it.dom);
    if (f_color >= (int)pal.size()) f_color = -1;
    refilter();
}
static double pill_w(cairo_t* cr, bool draw, double x0, double cy, double a);
static void btn_bg(cairo_t* cr, double x, double y, double w, double h, bool active, int id, double a) {
    if (active) { cairo_set_source_rgba(cr, acc_r, acc_g, acc_b, 0.92 * a); rrect(cr, x, y, w, h, h / 2); cairo_fill(cr); }
    else if (hover_btn == id) { fg(cr, 0.10 * a); rrect(cr, x, y, w, h, h / 2); cairo_fill(cr); }
}
static double pill_w(cairo_t* cr, bool draw, double x0, double cy, double a) {
    double x = x0 + 6, bh2 = 30, y = cy - bh2 / 2;
    // «Нещодавні»
    double tw = text(cr, "Нещодавні", 0, 0, 13, true, 0, -1, false) + 26;
    if (draw) { btn_bg(cr, x, y, tw, bh2, f_recent, 1, a); if (f_recent) cairo_set_source_rgba(cr, 1, 1, 1, a); else fg(cr, 0.85 * a);
                text(cr, "Нещодавні", x + 13, cy, 13, true, 0); hits.push_back({x, y, tw, bh2, 1}); }
    x += tw + 8;
    for (int id : {2, 3, 4, 5}) {
        bool act = (id == 2 && f_kind < 0 && !f_recent) || (id == 3 && f_kind == K_VID) || (id == 4 && f_kind == K_PIX);
        if (draw) { btn_bg(cr, x, y, bh2, bh2, act, id, a); if (act) cairo_set_source_rgba(cr, 1, 1, 1, a); else fg(cr, 0.8 * a);
                    icon(cr, id, x + bh2 / 2, cy); hits.push_back({x, y, bh2, bh2, id}); }
        x += bh2 + 4;
    }
    x += 8;
    for (int i = 0; i < (int)pal.size() && i < 20; i++) {
        double r = 10.5, ccx = x + 13, ccy = cy;
        if (draw) {
            double s = (hover_btn == 10 + i) ? 1.12 : 1.0;
            cairo_set_source_rgba(cr, pal[i].r, pal[i].g, pal[i].b, a); cairo_arc(cr, ccx, ccy, r * s, 0, 2 * M_PI); cairo_fill(cr);
            fg(cr, 0.14 * a); cairo_set_line_width(cr, 1); cairo_arc(cr, ccx, ccy, r * s - 0.5, 0, 2 * M_PI); cairo_stroke(cr);   // обвідка — щоб пастель не зливалась із фоном
            if (f_color == i) { fg(cr, 0.9 * a); cairo_set_line_width(cr, 2); cairo_arc(cr, ccx, ccy, r + 3.5, 0, 2 * M_PI); cairo_stroke(cr); }
            hits.push_back({x, y, 26, bh2, 10 + i});
            if (hover_btn == 10 + i) { tip_i = i; tip_x = ccx; tip_y = y + bh2 + 10; }
        }
        x += 30;
    }
    x += 6;
    bool field = search_on || !query.empty();
    double sw = field ? 190 : bh2;
    if (draw) {
        if (field) { fg(cr, 0.10 * a); rrect(cr, x, y, sw, bh2, bh2 / 2); cairo_fill(cr); }
        else btn_bg(cr, x, y, sw, bh2, false, 6, a);
        fg(cr, 0.8 * a); icon(cr, 6, x + bh2 / 2, cy);
        if (field) {
            std::string shown = query.empty() ? "Пошук…" : query;
            fg(cr, (query.empty() ? 0.45 : 0.9) * a);
            double w2 = text(cr, shown, x + bh2, cy, 13, false, 0, sw - bh2 - 12);
            if (search_on) {
                double cx2 = x + bh2 + (query.empty() ? 0 : w2 + 1);
                cairo_set_source_rgba(cr, acc_r, acc_g, acc_b, a); cairo_rectangle(cr, cx2, cy - 8, 1.6, 16); cairo_fill(cr);
            }
        }
        hits.push_back({x, y, sw, bh2, 6});
    }
    x += sw + 6;
    return x - x0;
}

static double wfac(double a) { return 0.62 + 0.98 * std::exp(-a * 1.25); }
static double sfac(double a) { return 0.70 + 0.30 * std::exp(-a * 0.9); }

static void card_path(cairo_t* cr, double xc, double cy, double w, double h) {
    cairo_save(cr);
    cairo_translate(cr, xc, cy);
    cairo_matrix_t m; cairo_matrix_init(&m, 1, 0, -SKEW, 1, 0, 0);
    cairo_transform(cr, &m);
    cairo_new_path(cr);
    rrect(cr, -w / 2, -h / 2, w, h, std::min(22.0, h * 0.07));
    cairo_restore(cr);
}

static void draw_impl(cairo_t* cr, int W, int H);
// MGOS_RIBBON_PROFILE=1 — середній час кадру в stderr раз на 60 кадрів (діагностика лагів прокрутки)
// ---------- шар карток на GPU (GtkSnapshot) ----------
// Картки — текстури GPU: скошена форма = трансформація GSK + заокруглена обрізка, масштаб — на відеокарті.
// Прокрутка лише міняє координати, CPU пікселів не растеризує (cairo-шар давав ~30 к/с на 4K).
static void snap_tr(GtkSnapshot* sn, double x, double y) { graphene_point_t pt = GRAPHENE_POINT_INIT((float)x, (float)y); gtk_snapshot_translate(sn, &pt); }
static GdkTexture* surf_tex(cairo_surface_t* s) {
    cairo_surface_flush(s);
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s), st = cairo_image_surface_get_stride(s);
    GBytes* b = g_bytes_new(cairo_image_surface_get_data(s), (gsize)st * h);
    GdkTexture* t = gdk_memory_texture_new(w, h, GDK_MEMORY_B8G8R8X8, b, st);
    g_bytes_unref(b);
    return t;
}
static void snap_text(GtkSnapshot* sn, GtkWidget* wd, const std::string& str, double x, double y, double size, bool bold,
                      int align, double maxw, const GdkRGBA& col, double* out_w = nullptr) {
    PangoLayout* l = gtk_widget_create_pango_layout(wd, str.c_str());
    PangoFontDescription* fd = pango_font_description_copy(pango_context_get_font_description(pango_layout_get_context(l)));
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(l, fd); pango_font_description_free(fd);
    if (maxw > 0) { pango_layout_set_width(l, (int)(maxw * PANGO_SCALE)); pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END); }
    int tw, th; pango_layout_get_pixel_size(l, &tw, &th);
    if (out_w) *out_w = tw;
    gtk_snapshot_save(sn);
    snap_tr(sn, (float)(align == 1 ? x - tw / 2.0 : x), (float)(y - th / 2.0));
    gtk_snapshot_append_layout(sn, l, &col);
    gtk_snapshot_restore(sn);
    g_object_unref(l);
}
static void cards_snapshot(GtkWidget* wd, GtkSnapshot* sn) {
    if (!settled_now || !opened || view.empty() || cg.Hc <= 0) return;
    static const bool prof = getenv("MGOS_RIBBON_PROFILE") != nullptr;
    gint64 t0 = prof ? g_get_monotonic_time() : 0;
    gtk_snapshot_save(sn);
    snap_tr(sn, 0, (float)-cards_y);   // координати вікна, як у area
    {   // картки не виходять за скло панелі
        GskRoundedRect pr; graphene_rect_t pb = GRAPHENE_RECT_INIT((float)cg.px, (float)cg.py, (float)cg.pw, (float)cg.ph);
        gsk_rounded_rect_init_from_rect(&pr, &pb, (float)cg.pr);
        gtk_snapshot_push_rounded_clip(sn, &pr);
    }
    const double Hc = cg.Hc, gap = cg.gap, cx = cg.cx, cy = cg.cy;
    static double F[241]; static double fHc = -1;
    if (fHc != Hc) {
        F[0] = 0; fHc = Hc;
        for (int i = 1; i <= 240; i++) { double t = (i - 0.5) * 0.05; F[i] = F[i - 1] + 0.05 * (Hc * sfac(t) * wfac(t) + gap); }
    }
    auto Fa = [&](double a) { double fi = std::min(a, 11.99) / 0.05; int i = (int)fi; return F[i] + (F[i + 1] - F[i]) * (fi - i); };
    step_px = Fa(1);
    int c = (int)lround(pos);
    std::vector<std::pair<double, int>> order;
    for (int vi = std::max(0, c - WIN - 1); vi <= std::min((int)view.size() - 1, c + WIN + 1); vi++) order.push_back({std::fabs(vi - pos), vi});
    std::sort(order.begin(), order.end(), [](auto& A, auto& B) { return A.first > B.first; });
    geos.clear();
    // 2D-скіс (категорія 2D, а не довільна матриця) — під загальною matrix GSK не вміє заокруглену обрізку (рожеві картки)
    const float sdeg = (float)(std::atan(SKEW) * 180 / M_PI);
    GskTransform* sk = gsk_transform_skew(nullptr, -sdeg, 0);
    GskTransform* unsk = gsk_transform_skew(nullptr, sdeg, 0);
    auto rgba = [](double r, double g, double b, double a) { GdkRGBA c{(float)r, (float)g, (float)b, (float)a}; return c; };
    for (auto& [a, vi] : order) {
        double d = vi - pos, h = Hc * sfac(a), w = Hc * sfac(a) * wfac(a);
        double xc = cx + (d < 0 ? -1 : 1) * Fa(a);
        if (xc + w / 2 + SKEW * h < cg.tx || xc - w / 2 - SKEW * h > cg.tx + cg.tw) continue;
        Item& it = items[view[vi]];
        bool hov = hover_card == vi;
        if (hov) { w *= 1.025; h *= 1.025; }
        geos.push_back({vi, xc, cy, w, h});
        double bwid = w + SKEW * h;
        GskRoundedRect rr;
        graphene_rect_t box = GRAPHENE_RECT_INIT((float)(-w / 2), (float)(-h / 2), (float)w, (float)h);
        gsk_rounded_rect_init_from_rect(&rr, &box, (float)std::min(22.0, h * 0.07));
        graphene_rect_t full = GRAPHENE_RECT_INIT((float)(-bwid / 2 - 2), (float)(-h / 2 - 2), (float)(bwid + 4), (float)(h + 4));
        gtk_snapshot_save(sn);
        snap_tr(sn, (float)xc, (float)cy);
        gtk_snapshot_transform(sn, sk);            // трапеція: скошена форма картки
        gtk_snapshot_push_rounded_clip(sn, &rr);
        gtk_snapshot_save(sn);
        gtk_snapshot_transform(sn, unsk);          // картинка — без скосу, скошена лише форма
        if (it.surf) {
            if (!it.tex) it.tex = surf_tex(it.surf);
            double sw = gdk_texture_get_width(it.tex), sh = gdk_texture_get_height(it.tex), sc = std::max(bwid / sw, h / sh);
            graphene_rect_t ir = GRAPHENE_RECT_INIT((float)(-sw * sc / 2), (float)(-sh * sc / 2), (float)(sw * sc), (float)(sh * sc));
            gtk_snapshot_append_scaled_texture(sn, it.tex, GSK_SCALING_FILTER_LINEAR, &ir);
        } else {
            GdkRGBA pc = rgba(acc_r * 0.3 + (light ? 0.6 : 0.1), acc_g * 0.3 + (light ? 0.6 : 0.1), acc_b * 0.3 + (light ? 0.6 : 0.1), 1);
            gtk_snapshot_append_color(sn, &pc, &full);
            GdkRGBA tc = light ? rgba(0.11, 0.11, 0.12, 0.55) : rgba(0.96, 0.96, 0.97, 0.55);
            snap_text(sn, wd, it.failed ? it.name : "…", 0, 0, 13, false, 1, w * 0.7, tc);
        }
        double dim = std::min(a, 3.0) / 3.0 * 0.38;
        if (dim > 0) { GdkRGBA dc = light ? rgba(1, 1, 1, dim * 0.8) : rgba(0, 0, 0, dim); gtk_snapshot_append_color(sn, &dc, &full); }
        if (hov) { GdkRGBA hc = rgba(1, 1, 1, 0.06); gtk_snapshot_append_color(sn, &hc, &full); }
        double la = clamp01(1 - a * 1.6);
        if (la > 0) {   // підпис центральної
            graphene_rect_t gr = GRAPHENE_RECT_INIT((float)(-bwid / 2 - 2), (float)(h * 0.1), (float)(bwid + 4), (float)(h * 0.4 + 2));
            GskColorStop st[2] = {{0, rgba(0, 0, 0, 0)}, {1, rgba(0, 0, 0, 0.6 * la)}};
            graphene_point_t g0 = GRAPHENE_POINT_INIT(0, (float)(h * 0.1)), g1 = GRAPHENE_POINT_INIT(0, (float)(h / 2));
            gtk_snapshot_append_linear_gradient(sn, &gr, &g0, &g1, st, 2);
            const char* tag = it.kind == K_VID ? "Відео" : it.kind == K_DYN ? "Динамічні" : it.kind == K_PIX ? "Піксельні" : "";
            double tx = -bwid / 2 + 26, ty = h / 2 - 26, nw = 0;
            snap_text(sn, wd, it.name, tx, ty, 15, true, 0, w * 0.62, rgba(1, 1, 1, 0.95 * la), &nw);
            if (*tag) {
                double tw2 = 0;
                snap_text(sn, wd, tag, 0, -10000, 11, true, 0, -1, rgba(0, 0, 0, 0), &tw2);   // лише ширина (поза кадром)
                tw2 += 16;
                GskRoundedRect tagr; graphene_rect_t tb = GRAPHENE_RECT_INIT((float)(tx + nw + 10), (float)(ty - 10), (float)tw2, 20);
                gsk_rounded_rect_init_from_rect(&tagr, &tb, 10);
                gtk_snapshot_push_rounded_clip(sn, &tagr);
                GdkRGBA tg = rgba(1, 1, 1, 0.22 * la); gtk_snapshot_append_color(sn, &tg, &tb);
                gtk_snapshot_pop(sn);
                snap_text(sn, wd, tag, tx + nw + 18, ty, 11, true, 0, -1, rgba(1, 1, 1, 0.95 * la));
            }
        }
        gtk_snapshot_restore(sn);
        gtk_snapshot_pop(sn);                       // обрізка
        // рамка (у скошеному просторі)
        float bwd; GdkRGBA bc;
        if (it.path == current) { bwd = 3; bc = rgba(acc_r, acc_g, acc_b, 1); }
        else if (hov) { bwd = 1.6f; bc = light ? rgba(0.11, 0.11, 0.12, 0.55) : rgba(0.96, 0.96, 0.97, 0.55); }
        else { bwd = 1; bc = light ? rgba(0, 0, 0, 0.10) : rgba(1, 1, 1, 0.12); }
        float bw4[4] = {bwd, bwd, bwd, bwd}; GdkRGBA bc4[4] = {bc, bc, bc, bc};
        gtk_snapshot_append_border(sn, &rr, bw4, bc4);
        gtk_snapshot_restore(sn);
    }
    gsk_transform_unref(sk); gsk_transform_unref(unsk);
    // лічильник
    char cnt[32]; snprintf(cnt, sizeof cnt, "%d / %zu", std::clamp(c, 0, (int)view.size() - 1) + 1, view.size());
    snap_text(sn, wd, cnt, cg.tx + cg.tw - 28 - 60, cg.ty + cg.th - 13, 11, false, 0, -1,
              light ? rgba(0.11, 0.11, 0.12, 0.45) : rgba(0.96, 0.96, 0.97, 0.45));
    gtk_snapshot_pop(sn);
    gtk_snapshot_restore(sn);
    if (prof) { static gint64 sum = 0; static int n = 0; sum += g_get_monotonic_time() - t0;
        if (++n == 60) { fprintf(stderr, "cards snapshot: avg %.2f ms\n", sum / 60.0 / 1000); sum = 0; n = 0; } }
}
typedef struct { GtkWidget parent; } RibbonCards;
typedef struct { GtkWidgetClass parent_class; } RibbonCardsClass;
G_DEFINE_TYPE(RibbonCards, ribbon_cards, GTK_TYPE_WIDGET)
static void ribbon_cards_class_init(RibbonCardsClass* k) { GTK_WIDGET_CLASS(k)->snapshot = cards_snapshot; }
static void ribbon_cards_init(RibbonCards*) {}

static void draw(GtkDrawingArea*, cairo_t* cr, int W, int H, gpointer) {
    static const bool prof = getenv("MGOS_RIBBON_PROFILE") != nullptr;
    gint64 t0 = prof ? g_get_monotonic_time() : 0;
    layer = 0;
    draw_impl(cr, W, H);
    if (prof) {
        static gint64 sum = 0, mx = 0; static int n = 0;
        gint64 d = g_get_monotonic_time() - t0; sum += d; mx = std::max(mx, d);
        if (++n == 60) { double sx = 1, sy = 1; cairo_surface_get_device_scale(cairo_get_target(cr), &sx, &sy);
            fprintf(stderr, "ribbon draw: avg %.2f ms, max %.2f ms (%dx%d @%.2f)\n", sum / 60.0 / 1000, mx / 1000.0, W, H, sx);
            sum = mx = 0; n = 0; }
    }
}
static void draw_impl(cairo_t* cr, int W, int H) {
    if (layer == 0) { hits.clear(); tip_i = -1; }
    if (!opened) return;
    {   // палітра wal/акцент змінились, поки панель відкрита — перечитати (не частіше ніж раз на секунду)
        static gint64 last_chk = 0; gint64 now = g_get_monotonic_time();
        if (now - last_chk > 1000000) { last_chk = now; if (load_palette()) reclassify(); }
    }
    R P = panel_rect(W, H), T = target_rect(W, H);
    double pc = clamp01(p);
    double ta = clamp01(1 - p * 3);
    double ca = clamp01((p - 0.4) / 0.45);
    double slide = (1 - ca) * -14;
    double pw = 0, ph = 42, px = 0, py = 0;
    if (ca > 0) { pw = pill_w(cr, false, 0, 0, ca); px = T.x + (T.w - pw) / 2; py = T.y + 16 + slide; }
    // Панель розгорнута й стоїть: скло, тінь і пілюля фільтрів між кадрами прокрутки не змінюються — малюємо
    // їх раз у кеш (фізичні пікселі) і далі лише копіюємо. Раніше ~8 повноекранних проходів cairo на кадр
    // давали ~25 к/с прокрутки на 4K.
    bool settled = p_target == 1 && std::fabs(p - 1) < 0.002 && ta <= 0;
    if (layer == 0) {
        bool was = settled_now; settled_now = settled && cards_area;
        if (was != settled_now && cards_area) gtk_widget_queue_draw(cards_area);
    }
    double sc = 1;
    if (GtkNative* nat = area ? gtk_widget_get_native(area) : nullptr)
        if (GdkSurface* su = gtk_native_get_surface(nat)) sc = gdk_surface_get_scale(su);
    char keyb[512];
    snprintf(keyb, sizeof keyb, "%d %d %.3f %d %s %d %d %d %d %d %zu %s %p %ld", W, H, sc, light, accent_hex.c_str(), hover_btn,
             f_recent, f_kind, f_color, search_on, pal.size(), query.c_str(), (void*)sysui::bb::cache().s, (long)sysui::bb::cache().mtime);
    static cairo_surface_t* chrome = nullptr; static std::string chrome_key; static std::vector<Hit> chrome_hits;
    static int chrome_tip = -1; static double chrome_tx = 0, chrome_ty = 0;
    auto draw_chrome = [&](cairo_t* c) {
        // м'яка тінь
        for (int i = 1; i <= 4; i++) { cairo_set_source_rgba(c, 0, 0, 0, 0.045 * pc); rrect(c, P.x - i * 3, P.y - i * 2 + 6, P.w + i * 6, P.h + i * 5, P.r + i * 3); cairo_fill(c); }
        // скло: запечений блюр + тонування
        cairo_save(c); cairo_translate(c, P.x, P.y);
        bool baked = sysui::bb::paint(c, P.x, P.y, P.w, P.h, P.r);
        cairo_restore(c);
        rrect(c, P.x, P.y, P.w, P.h, P.r);
        if (light) cairo_set_source_rgba(c, 0.97, 0.97, 0.98, baked ? 0.55 : 0.93);
        else cairo_set_source_rgba(c, 0.08, 0.08, 0.10, baked ? 0.48 : 0.90);
        cairo_fill_preserve(c);
        cairo_pattern_t* gl = cairo_pattern_create_linear(0, P.y, 0, P.y + P.h);
        cairo_pattern_add_color_stop_rgba(gl, 0, acc_r, acc_g, acc_b, 0.13 * pc);
        cairo_pattern_add_color_stop_rgba(gl, 0.6, acc_r, acc_g, acc_b, 0);
        cairo_set_source(c, gl); cairo_fill_preserve(c); cairo_pattern_destroy(gl);
        if (light) cairo_set_source_rgba(c, 0, 0, 0, 0.10); else cairo_set_source_rgba(c, 1, 1, 1, 0.13);
        cairo_set_line_width(c, 1); cairo_stroke(c);
        cairo_save(c);
        rrect(c, P.x, P.y, P.w, P.h, P.r); cairo_clip(c);
        if (ta > 0) {   // годинник бульбашки тане в перші миті
            time_t now = time(nullptr); char buf[16]; strftime(buf, sizeof buf, "%H:%M", localtime(&now));
            fg(c, ta); text(c, buf, P.x + P.w / 2, P.y + std::min(P.h, bh) / 2, bh * 0.42, true, 1);
        }
        if (ca > 0) {   // пілюля фільтрів
            rrect(c, px, py, pw, ph, ph / 2);
            if (light) cairo_set_source_rgba(c, 1, 1, 1, 0.55 * ca); else cairo_set_source_rgba(c, 0, 0, 0, 0.30 * ca);
            cairo_fill_preserve(c);
            if (light) cairo_set_source_rgba(c, 0, 0, 0, 0.06 * ca); else cairo_set_source_rgba(c, 1, 1, 1, 0.08 * ca);
            cairo_stroke(c);
            pill_w(c, true, px, py + ph / 2, ca);
        }
        cairo_restore(c);
    };
    if (layer == 1) {}
    else if (settled) {
        if (!chrome || chrome_key != keyb) {
            if (chrome) cairo_surface_destroy(chrome);
            chrome = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)std::ceil(W * sc), (int)std::ceil(H * sc));
            cairo_surface_set_device_scale(chrome, sc, sc);
            cairo_t* c2 = cairo_create(chrome);
            hits.clear(); tip_i = -1;
            draw_chrome(c2);
            cairo_destroy(c2);
            chrome_key = keyb; chrome_hits = hits; chrome_tip = tip_i; chrome_tx = tip_x; chrome_ty = tip_y;
        } else {
            hits = chrome_hits; tip_i = chrome_tip; tip_x = chrome_tx; tip_y = chrome_ty;
        }
        cairo_set_source_surface(cr, chrome, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
        cairo_paint(cr);
    } else {
        if (chrome) { cairo_surface_destroy(chrome); chrome = nullptr; chrome_key.clear(); }   // анімація — пам'ять кешу не тримаємо
        draw_chrome(cr);
    }
    cairo_save(cr);
    rrect(cr, P.x, P.y, P.w, P.h, P.r); cairo_clip(cr);
    if (ca > 0) {

        // карусель
        bool here = settled_now ? layer == 1 : layer == 0;
        if (here) geos.clear();
        double top = py + ph + 22, bot = T.y + T.h - 24, Hc = std::max(80.0, bot - top), cy = (top + bot) / 2 - slide * 0.5;
        double cx = T.x + T.w / 2, gap = 16;
        if (layer == 0) cg = {cx, cy, Hc, gap, T.x, T.y, T.w, T.h, P.x, P.y, P.w, P.h, P.r};
        if (!here) {}
        else if (view.empty()) {
            fg(cr, 0.55 * ca);
            text(cr, f_color >= 0 ? std::string("Немає шпалер у кольорі: ") + (f_color < (int)pal.size() ? pal[f_color].name : "") + " (кольори ще визначаються…)" : "Нічого не знайдено", cx, cy, 15, false, 1);
        } else {
            // F(a) — відстань від центру до центру картки з відступом a (інтеграл ширин)
            static double F[241];
            F[0] = 0;
            for (int i = 1; i <= 240; i++) { double t = (i - 0.5) * 0.05; F[i] = F[i - 1] + 0.05 * (Hc * sfac(t) * wfac(t) + gap); }
            auto Fa = [&](double a) { double fi = std::min(a, 11.99) / 0.05; int i = (int)fi; return F[i] + (F[i + 1] - F[i]) * (fi - i); };
            step_px = Fa(1);
            int c = (int)lround(pos);
            std::vector<std::pair<double, int>> order;
            for (int vi = std::max(0, c - WIN - 1); vi <= std::min((int)view.size() - 1, c + WIN + 1); vi++) order.push_back({std::fabs(vi - pos), vi});
            std::sort(order.begin(), order.end(), [](auto& A, auto& B) { return A.first > B.first; });
            double zoom = 0.86 + 0.14 * ca;
            static const bool nocards = getenv("MGOS_RIBBON_NOCARDS") != nullptr;
            if (nocards) order.clear();
            for (auto& [a, vi] : order) {
                double d = vi - pos, h = Hc * sfac(a) * zoom, w = Hc * sfac(a) * wfac(a) * zoom;
                double xc = cx + (d < 0 ? -1 : 1) * Fa(a) * zoom;
                if (xc + w / 2 + SKEW * h < T.x || xc - w / 2 - SKEW * h > T.x + T.w) continue;
                Item& it = items[view[vi]];
                bool hov = hover_card == vi;
                if (hov) { w *= 1.025; h *= 1.025; }
                geos.push_back({vi, xc, cy, w, h});
                double bwid = w + SKEW * h, bx0 = xc - bwid / 2, by0 = cy - h / 2;
                double dim = std::min(a, 3.0) / 3.0 * 0.38;
                double la = clamp01(1 - a * 1.6) * ca;
                // Швидкий шлях (панель розгорнута): картинка заливається прямо у форму картки, без маски
                // обрізки; у русі — найшвидший фільтр (різниці не видно), щойно стрічка стала — якісний.
                // Раніше кожна картка = cairo_clip(скошена форма) + масштаб BILINEAR + ще прохід затемнення:
                // ~30 к/с прокрутки на 4K.
                if (it.surf && ca >= 0.999 && la <= 0) {
                    double sw = cairo_image_surface_get_width(it.surf), sh = cairo_image_surface_get_height(it.surf);
                    double sc = std::max(bwid / sw, h / sh);
                    cairo_pattern_t* pat = cairo_pattern_create_for_surface(it.surf);
                    cairo_matrix_t m; cairo_matrix_init_scale(&m, 1 / sc, 1 / sc);
                    cairo_matrix_translate(&m, -(xc - sw * sc / 2), -(cy - sh * sc / 2));
                    cairo_pattern_set_matrix(pat, &m);
                    bool moving = std::fabs(posv) > 0.01 || std::fabs(pos - target) > 0.002;
                    cairo_pattern_set_filter(pat, moving ? CAIRO_FILTER_FAST : CAIRO_FILTER_BILINEAR);
                    card_path(cr, xc, cy, w, h);
                    cairo_set_source(cr, pat); cairo_fill_preserve(cr); cairo_pattern_destroy(pat);
                    double ov = 0;   // затемнення далеких і підсвітка наведеної — одна заливка
                    if (hov) { cairo_set_source_rgba(cr, 1, 1, 1, 0.06); ov = 1; }
                    else if (dim > 0) { if (light) cairo_set_source_rgba(cr, 1, 1, 1, dim * 0.8); else cairo_set_source_rgba(cr, 0, 0, 0, dim); ov = 1; }
                    if (ov) cairo_fill_preserve(cr);
                    cairo_new_path(cr);
                } else {
                cairo_save(cr);
                card_path(cr, xc, cy, w, h);
                cairo_clip(cr);
                if (it.surf) {
                    double sw = cairo_image_surface_get_width(it.surf), sh = cairo_image_surface_get_height(it.surf);
                    double sc = std::max(bwid / sw, h / sh);
                    cairo_save(cr);
                    cairo_translate(cr, xc - sw * sc / 2, cy - sh * sc / 2);
                    cairo_scale(cr, sc, sc);
                    cairo_set_source_surface(cr, it.surf, 0, 0);
                    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                    cairo_paint_with_alpha(cr, ca);
                    cairo_restore(cr);
                } else {
                    cairo_set_source_rgba(cr, acc_r, acc_g, acc_b, 0.16 * ca); cairo_paint(cr);
                    fg(cr, 0.06 * ca); cairo_paint(cr);
                    fg(cr, 0.55 * ca); text(cr, it.failed ? it.name : "…", xc, cy, 13, false, 1, w * 0.7);
                }
                // глибина: далекі картки притемнені
                if (dim > 0) { if (light) cairo_set_source_rgba(cr, 1, 1, 1, dim * 0.8 * ca); else cairo_set_source_rgba(cr, 0, 0, 0, dim * ca); cairo_paint(cr); }
                if (hov) { cairo_set_source_rgba(cr, 1, 1, 1, 0.06 * ca); cairo_paint(cr); }
                // підпис центральної
                if (la > 0) {
                    cairo_pattern_t* g = cairo_pattern_create_linear(0, by0 + h * 0.6, 0, by0 + h);
                    cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, 0);
                    cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0.6 * la);
                    cairo_set_source(cr, g); cairo_paint(cr); cairo_pattern_destroy(g);
                    const char* tag = it.kind == K_VID ? "Відео" : it.kind == K_DYN ? "Динамічні" : it.kind == K_PIX ? "Піксельні" : "";
                    double tx = bx0 + 26, ty = by0 + h - 26;
                    cairo_set_source_rgba(cr, 1, 1, 1, 0.95 * la);
                    double nw = text(cr, it.name, tx, ty, 15, true, 0, w * 0.62);
                    if (*tag) {
                        double tw = text(cr, tag, 0, 0, 11, true, 0, -1, false) + 16;
                        cairo_set_source_rgba(cr, 1, 1, 1, 0.22 * la); rrect(cr, tx + nw + 10, ty - 10, tw, 20, 10); cairo_fill(cr);
                        cairo_set_source_rgba(cr, 1, 1, 1, 0.95 * la); text(cr, tag, tx + nw + 18, ty, 11, true, 0);
                    }
                }
                cairo_restore(cr);
                }
                // рамка
                card_path(cr, xc, cy, w, h);
                if (it.path == current) { cairo_set_source_rgba(cr, acc_r, acc_g, acc_b, ca); cairo_set_line_width(cr, 3); }
                else if (hov) { fg(cr, 0.55 * ca); cairo_set_line_width(cr, 1.6); }
                else { if (light) cairo_set_source_rgba(cr, 0, 0, 0, 0.10 * ca); else cairo_set_source_rgba(cr, 1, 1, 1, 0.12 * ca); cairo_set_line_width(cr, 1); }
                cairo_stroke(cr);
            }
            // лічильник
            fg(cr, 0.45 * ca);
            char cnt[32]; snprintf(cnt, sizeof cnt, "%d / %zu", std::clamp(c, 0, (int)view.size() - 1) + 1, view.size());
            text(cr, cnt, T.x + T.w - 28 - 60, T.y + T.h - 13, 11, false, 0);
        }
    }
    cairo_restore(cr);
    // підказка над кружечком палітри: назва за відтінком + hex кольору wal
    if (layer == 0 && tip_i >= 0 && tip_i < (int)pal.size() && hover_btn == 10 + tip_i && pc > 0.9) {
        std::string t = pal[tip_i].name + "  " + pal[tip_i].hex;
        double tw = text(cr, t, 0, 0, 11, true, 0, -1, false) + 18, th = 22, tx = tip_x - tw / 2, ty = tip_y;
        if (light) cairo_set_source_rgba(cr, 1, 1, 1, 0.92); else cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, 0.92);
        rrect(cr, tx, ty, tw, th, th / 2); cairo_fill(cr);
        cairo_set_source_rgba(cr, pal[tip_i].r, pal[tip_i].g, pal[tip_i].b, 1); cairo_set_line_width(cr, 1.2);
        rrect(cr, tx + 0.6, ty + 0.6, tw - 1.2, th - 1.2, th / 2); cairo_stroke(cr);
        fg(cr, 0.92); text(cr, t, tx + 9, ty + th / 2, 11, true, 0);
    }
}

// ---------- відкриття / закриття ----------
static void close_ribbon() { if (!opened || p_target == 0) return; p_target = 0; search_on = false; kick(); }
static void finish_close() {
    gtk_widget_set_visible(win, FALSE);
    opened = false;
    if (loader) { { std::lock_guard<std::mutex> lk(loader->m); loader->stop = true; loader->q.clear(); } loader->cv.notify_all(); loader.reset(); }
    save_colors();
    for (auto& it : items) { if (it.surf) cairo_surface_destroy(it.surf); g_clear_object(&it.tex); }
    items.clear(); items.shrink_to_fit(); view.clear(); vpos.clear(); geos.clear(); hits.clear();
    if (snap_id) { g_source_remove(snap_id); snap_id = 0; }
    sysui::bb::release();
    sysui::trim();
}
static void open_ribbon(double x, double y, double w, double h, const std::string& conn) {
    if (opened && p_target > 0.5) { close_ribbon(); return; }
    sysui::close_other_overlays("wallpapers");   // стрічка відкривається — решта оверлеїв згортаються
    if (opened) { p_target = 1; kick(); return; }   // передумали посеред згортання
    if (w > 4 && h > 4) { bx = x; by = y; bw = w; bh = h; }
    GdkDisplay* dpy = gdk_display_get_default();
    GListModel* mons = gdk_display_get_monitors(dpy);
    GdkMonitor* mon = nullptr;
    for (guint i = 0; i < g_list_model_get_n_items(mons); i++) {
        GdkMonitor* m = GDK_MONITOR(g_list_model_get_item(mons, i));
        const char* c = gdk_monitor_get_connector(m);
        if (!mon || (c && conn == c)) mon = m;
        g_object_unref(m);
    }
    if (mon) { gtk_layer_set_monitor(GTK_WINDOW(win), mon); sysui::bb::set_monitor(mon); }
    if (!(w > 4 && h > 4) && mon) {   // без геометрії (Super+W тощо) — уявна бульбашка вгорі по центру
        GdkRectangle g; gdk_monitor_get_geometry(mon, &g); bw = 120; bh = 34; bx = (g.width - bw) / 2; by = 6;
    }
    light = sysui::is_light_theme();
    accent_hex = sysui::accent(); hex_rgb(accent_hex, acc_r, acc_g, acc_b);
    load_palette(true); if (f_color >= (int)pal.size()) f_color = -1;
    std::error_code ec; fs::create_directories(thumb_dir(), ec);
    build_items();
    query.clear(); search_on = false; hover_card = hover_btn = -1;
    if (f_recent && recent.empty()) f_recent = false;
    pos = target = 0; view.clear();
    loader = std::make_shared<Loader>();
    loader->theme = light ? "light" : "dark"; loader->accent = accent_hex;
    for (auto& it : items) loader->jobs.push_back({it.path, (int)it.kind});
    refilter();
    for (size_t v = 0; v < view.size(); v++) if (items[view[v]].path == current) { pos = target = v; break; }
    last_center = -1; update_window();
    { std::lock_guard<std::mutex> lk(loader->m);
      for (size_t i = 0; i < items.size(); i++) if (items[i].dom < 0) loader->q.push_back({(int)i, false}); }
    std::thread(loader_run, loader).detach();
    loader->cv.notify_one();
    opened = true; p = 0; pv = 0; p_target = 1; posv = 0;
    gtk_widget_set_visible(win, TRUE);
    // MGOS_RIBBON_BENCH=1 — автопрокрутка на 40 карток (вимірювання разом з MGOS_RIBBON_PROFILE)
    if (getenv("MGOS_RIBBON_BENCH")) {
        static int left; left = 40;
        g_timeout_add(180, [](gpointer) -> gboolean { if (!opened || --left < 0) return G_SOURCE_REMOVE; go(target + 1); return G_SOURCE_CONTINUE; }, nullptr);
    }
    gtk_window_present(GTK_WINDOW(win));
    qd();
    kick();
}
static gboolean on_result(gpointer d) {
    Result* r = (Result*)d;
    if (r->L == loader && r->idx >= 0 && r->idx < (int)items.size()) {
        Item& it = items[r->idx];
        if (r->color >= 0 && it.dom != r->color) {
            it.dom = r->color; colors_dirty = true;
            int c = classify(it.dom);
            if (c != it.color) { bool rf = f_color >= 0 && (f_color == c || f_color == it.color); it.color = c; if (rf) refilter(); }
        }
        if (r->want) {
            it.requested = false;
            if (r->s && near(r->idx)) { if (it.surf) cairo_surface_destroy(it.surf); g_clear_object(&it.tex); it.surf = r->s; r->s = nullptr; }
            else if (!r->s) it.failed = true;
        }
        qd();
    }
    if (r->s) cairo_surface_destroy(r->s);
    delete r;
    return G_SOURCE_REMOVE;
}
static void apply_item(int idx) {
    if (idx < 0 || idx >= (int)items.size()) return;
    Item it = items[idx];
    if (it.kind == K_PIX) exec_cmd("setsid -f '" + sysui::home() + "/.config/SYSui/pixel_wallpaper' --set " + it.path.substr(6));
    else apply_wallpaper(it.path);
    current = it.path;
    recent.erase(std::remove(recent.begin(), recent.end(), it.path), recent.end());
    recent.insert(recent.begin(), it.path);
    if (recent.size() > 40) recent.resize(40);
    save_recent();
    qd();
    g_timeout_add(160, [](gpointer) -> gboolean { close_ribbon(); return G_SOURCE_REMOVE; }, nullptr);
}

// ---------- ввід ----------
static int card_at(double x, double y) {
    for (auto g = geos.rbegin(); g != geos.rend(); ++g) {
        double ly = y - g->cy, lx = (x - g->xc) - SKEW * (-ly);
        if (std::fabs(ly) <= g->h / 2 && std::fabs(lx) <= g->w / 2) return g->vi;
    }
    return -1;
}
static int btn_at(double x, double y) {
    for (auto& h : hits) if (x >= h.x && x <= h.x + h.w && y >= h.y && y <= h.y + h.h) return h.id;
    return -1;
}
static void on_motion(GtkEventControllerMotion*, double x, double y, gpointer) {
    mx = x; my = y;
    int hc = card_at(x, y), hb = hc < 0 ? btn_at(x, y) : -1;
    if (hc != hover_card || hb != hover_btn) { hover_card = hc; hover_btn = hb; qd(); }
}
static void on_leave(GtkEventControllerMotion*, gpointer) { hover_card = hover_btn = -1; qd(); }
static void press_btn(int id) {
    if (id == 1) f_recent = !f_recent;
    else if (id == 2) { f_kind = -1; f_recent = false; f_color = -1; query.clear(); }
    else if (id == 3) f_kind = f_kind == K_VID ? -1 : K_VID;
    else if (id == 4) f_kind = f_kind == K_PIX ? -1 : K_PIX;
    else if (id == 5) { close_ribbon(); exec_cmd("pkill -USR1 -x topbar"); return; }   // календар/таймер годинника
    else if (id == 6) search_on = !search_on;
    else if (id >= 10 && id < 10 + (int)pal.size()) f_color = f_color == id - 10 ? -1 : id - 10;
    refilter();
    if (search_on) kick();
}
static void on_release(GtkGestureClick* g, int n, double x, double y, gpointer) {
    if (dragged) { dragged = false; return; }
    if (!opened || p_target == 0) return;
    int W = gtk_widget_get_width(area), H = gtk_widget_get_height(area);
    R T = target_rect(W, H);
    guint b = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (x < T.x || x > T.x + T.w || y < T.y || y > T.y + T.h) { close_ribbon(); return; }
    if (b != 1) return;
    int bt = btn_at(x, y);
    if (bt >= 0) { press_btn(bt); return; }
    int c = card_at(x, y);
    if (c >= 0) { go(c); apply_item(view[c]); }
}
static void on_drag_begin(GtkGestureDrag*, double, double, gpointer) { drag_start = target; dragged = false; }
static void on_drag_update(GtkGestureDrag*, double dx, double, gpointer) {
    if (std::fabs(dx) > 8) dragged = true;
    if (dragged) go(drag_start - dx / std::max(80.0, step_px));
}
static void on_drag_end(GtkGestureDrag*, double, double, gpointer) { if (dragged) go(std::round(target)); }
static gboolean on_scroll(GtkEventControllerScroll* c, double dx, double dy, gpointer) {
    if (view.empty()) return TRUE;
    if (gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_WHEEL) {
        double v = dy != 0 ? dy : dx;
        go(std::round(target) + (v > 0 ? 1 : -1));
    } else {   // тачпад: плавно, після паузи — доводимо до картки
        go(target + (dx + dy) / std::max(60.0, step_px * 0.45));
        if (snap_id) g_source_remove(snap_id);
        snap_id = g_timeout_add(140, [](gpointer) -> gboolean { snap_id = 0; go(std::round(target)); return G_SOURCE_REMOVE; }, nullptr);
    }
    return TRUE;
}
static gboolean on_key(GtkEventControllerKey*, guint kv, guint, GdkModifierType st, gpointer) {
    if (!opened) return FALSE;
    switch (kv) {
    case GDK_KEY_Escape:
        if (search_on || !query.empty()) { query.clear(); search_on = false; refilter(); } else close_ribbon();
        return TRUE;
    case GDK_KEY_Left: case GDK_KEY_Up: go(std::round(target) - 1); return TRUE;
    case GDK_KEY_Right: case GDK_KEY_Down: go(std::round(target) + 1); return TRUE;
    case GDK_KEY_Page_Up: go(std::round(target) - 5); return TRUE;
    case GDK_KEY_Page_Down: go(std::round(target) + 5); return TRUE;
    case GDK_KEY_Home: go(0); return TRUE;
    case GDK_KEY_End: go(view.size()); return TRUE;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_space:
        if (kv == GDK_KEY_space && search_on) break;
        if (!view.empty()) apply_item(view[std::clamp((int)lround(target), 0, (int)view.size() - 1)]);
        return TRUE;
    case GDK_KEY_BackSpace:
        if (!query.empty()) {
            const char* s = query.c_str(); const char* e = g_utf8_find_prev_char(s, s + query.size());
            query.resize(e ? e - s : 0); refilter();
        }
        return TRUE;
    }
    if (st & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return FALSE;
    gunichar u = gdk_keyval_to_unicode(kv);
    if (u >= 32 && g_unichar_isprint(u)) {
        char buf[8] = {0}; g_unichar_to_utf8(u, buf);
        query += buf; search_on = true; refilter(); kick();
        return TRUE;
    }
    return FALSE;
}

static void init(GtkApplication* app) {
    win = gtk_application_window_new(app);
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_namespace(GTK_WINDOW(win), "wp_ribbon");
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    for (auto e : {GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM, GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT})
        gtk_layer_set_anchor(GTK_WINDOW(win), e, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
    gtk_widget_add_css_class(win, "wp-ribbon");
    GtkCssProvider* cp = gtk_css_provider_new();
    gtk_css_provider_load_from_string(cp, "window.wp-ribbon { background: transparent; box-shadow: none; border: none; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 20);
    area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area, TRUE); gtk_widget_set_vexpand(area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, nullptr, nullptr);
    GtkWidget* ov = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(ov), area);
    cards_area = GTK_WIDGET(g_object_new(ribbon_cards_get_type(), nullptr));
    gtk_widget_set_valign(cards_area, GTK_ALIGN_FILL);   // на весь екран: GPU-шар не растеризує порожнечу
    gtk_widget_set_halign(cards_area, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(cards_area, FALSE);   // події — area (координати ті самі)
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), cards_area);
    gtk_window_set_child(GTK_WINDOW(win), ov);
    GtkEventController* mo = gtk_event_controller_motion_new();
    g_signal_connect(mo, "motion", G_CALLBACK(on_motion), nullptr);
    g_signal_connect(mo, "leave", G_CALLBACK(on_leave), nullptr);
    gtk_widget_add_controller(area, mo);
    GtkGesture* cl = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(cl), 0);
    g_signal_connect(cl, "released", G_CALLBACK(on_release), nullptr);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(cl));
    GtkGesture* dr = gtk_gesture_drag_new();
    g_signal_connect(dr, "drag-begin", G_CALLBACK(on_drag_begin), nullptr);
    g_signal_connect(dr, "drag-update", G_CALLBACK(on_drag_update), nullptr);
    g_signal_connect(dr, "drag-end", G_CALLBACK(on_drag_end), nullptr);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(dr));
    GtkEventController* sc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(sc, "scroll", G_CALLBACK(on_scroll), nullptr);
    gtk_widget_add_controller(area, sc);
    GtkEventController* kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), nullptr);
    gtk_widget_add_controller(win, kc);
    gtk_widget_realize(win);
    gtk_widget_set_visible(win, FALSE);
}


// Перевірка без показу на екрані: рендер кадрів розгортання у PNG + замір часу кадру.
// wallpapers --ribbon-shot /tmp/rb dark|light
static int shot(const std::string& prefix, const std::string& theme) {
    gtk_init();
    area = gtk_drawing_area_new(); g_object_ref_sink(area);
    const int W = 1920, H = 1080;
    bw = 116; bh = 34; bx = (W - bw) / 2; by = 6;
    light = theme == "light"; accent_hex = sysui::accent(); hex_rgb(accent_hex, acc_r, acc_g, acc_b);
    GdkDisplay* dpy = gdk_display_get_default();
    if (dpy) { GListModel* ms = gdk_display_get_monitors(dpy); if (g_list_model_get_n_items(ms)) { GdkMonitor* m = GDK_MONITOR(g_list_model_get_item(ms, 0)); sysui::bb::set_monitor(m); g_object_unref(m); } }
    load_palette(true);
    std::error_code ec; fs::create_directories(thumb_dir(), ec);
    build_items(); refilter();
    for (size_t v = 0; v < view.size(); v++) if (items[view[v]].path == current) { pos = target = v; break; }
    gint64 t0 = g_get_monotonic_time();
    int c = (int)lround(pos);
    for (int vi = std::max(0, c - WIN); vi <= std::min((int)view.size() - 1, c + WIN); vi++) {
        Item& it = items[view[vi]];
        std::string t = make_thumb(it.path, it.kind, theme, accent_hex);
        int col; if (!t.empty()) it.surf = decode(t, col, true); else it.failed = true;
        if (!t.empty()) { it.dom = col; it.color = classify(col); colors_dirty = true; }
    }
    printf("мініатюри: %.0f мс\n", (g_get_monotonic_time() - t0) / 1000.0);
    opened = true;
    cairo_surface_t* bg = nullptr;
    { std::string bp = sysui::bb::resolve(); GdkPixbuf* pb = bp.empty() ? nullptr : gdk_pixbuf_new_from_file_at_scale(bp.c_str(), W, H, FALSE, nullptr);
      if (pb) { bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H); cairo_t* b = cairo_create(bg); gdk_cairo_set_source_pixbuf(b, pb, 0, 0); cairo_paint(b); cairo_destroy(b); g_object_unref(pb); } }
    double ps[] = {0, 0.12, 0.25, 0.4, 0.55, 0.7, 0.85, 1.0, 1.03};
    for (int i = 0; i < 9; i++) {
        p = ps[i];
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
        cairo_t* cr = cairo_create(s);
        if (bg) { cairo_set_source_surface(cr, bg, 0, 0); cairo_paint(cr); } else { cairo_set_source_rgb(cr, 0.3, 0.32, 0.36); cairo_paint(cr); }
        draw(nullptr, cr, W, H, nullptr);
        cairo_destroy(cr);
        char fn[512]; snprintf(fn, sizeof fn, "%s_%s_%d.png", prefix.c_str(), theme.c_str(), i);
        cairo_surface_write_to_png(s, fn); cairo_surface_destroy(s);
    }
    // замір: кадр каруселі в русі (p = 1, pos між картками)
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    p = 1; t0 = g_get_monotonic_time();
    for (int i = 0; i < 30; i++) { pos = c + (i % 10) * 0.05; cairo_t* cr = cairo_create(s); cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR); cairo_paint(cr); cairo_set_operator(cr, CAIRO_OPERATOR_OVER); draw(nullptr, cr, W, H, nullptr); cairo_destroy(cr); }
    printf("кадр: %.2f мс (cairo, %zu карток у фільтрі)\n", (g_get_monotonic_time() - t0) / 30000.0, view.size());
    pos = c + 0.5; p = 1;
    { cairo_t* cr = cairo_create(s); if (bg) { cairo_set_source_surface(cr, bg, 0, 0); cairo_paint(cr); } draw(nullptr, cr, W, H, nullptr); cairo_destroy(cr);
      cairo_surface_write_to_png(s, (prefix + "_" + theme + "_mid.png").c_str()); }
    cairo_surface_destroy(s);
    { std::vector<int> n(pal.size(), 0); for (auto& it : items) if (it.color >= 0 && it.color < (int)n.size()) n[it.color]++;
      fprintf(stderr, "палітра:"); for (size_t k = 0; k < pal.size(); k++) fprintf(stderr, " %s %s=%d;", pal[k].name.c_str(), pal[k].hex.c_str(), n[k]); fprintf(stderr, "\n"); }
    save_colors();
    return 0;
}

}  // namespace ribbon
