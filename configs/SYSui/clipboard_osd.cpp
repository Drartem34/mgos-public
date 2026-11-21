// SYSui — буфер обміну (Super+V) у стилі «Налаштувань» mgOS.
//  • пошук: просто починайте друкувати;
//  • ↑/↓ — вибір, Enter / клік — вставити у вікно, з якого відкрили (Ctrl+V; у терміналі Ctrl+Shift+V),
//    Shift+Enter / Ctrl+клік — лише скопіювати, Delete / ✕ — видалити запис;
//  • «Очистити» питає підтвердження (другий клік);
//  • мініатюри картинок декодуються у фоні й кешуються в /tmp.
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include "sysui_common.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <cctype>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;

static const std::string CACHE_DIR = "/tmp/ignis_clipboard_cache";
static const int MAX_ITEMS = 150;
static const int WINDOW_WIDTH = 380;
static const int WINDOW_HEIGHT = 520;

static GtkWidget* clip_window = nullptr;
static GtkWidget* clip_container = nullptr;
static GtkWidget* list_box = nullptr;
static GtkWidget* search = nullptr;
static GtkWidget* clear_btn = nullptr;
static GtkWidget* empty_lbl = nullptr;
static std::string target_addr, target_class;   // вікно, куди вставляти
static bool clear_armed = false;
static gint64 shown_at = 0;

struct ClipItem {
    std::string id, text, search;
    bool is_image = false, is_file = false;
};

// ================= УТИЛІТИ =================
static std::string exec(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[4096];
    std::string res;
    while (fgets(buf, sizeof(buf), pipe)) res += buf;
    pclose(pipe);
    return res;
}

static std::string lower(std::string s) {
    // ASCII-нижній регістр + кирилиця через g_utf8_strdown
    gchar* d = g_utf8_strdown(s.c_str(), -1);
    std::string r = d ? d : s;
    g_free(d);
    return r;
}

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n\v\f");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n\v\f");
    return str.substr(first, last - first + 1);
}

static bool valid_id(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), ::isdigit);
}

// ================= ДІЇ =================
static void hide() { gtk_widget_set_visible(clip_window, FALSE); }

static void populate_list();

static void restore_item(const ClipItem* it, bool paste) {
    if (!valid_id(it->id)) return;
    std::string cmd = "cliphist decode " + it->id + " | wl-copy" + (it->is_file ? " -t text/uri-list" : "");
    hide();
    std::string addr = target_addr, cls = lower(target_class);
    std::thread([cmd, addr, cls, paste]() {
        system(cmd.c_str());
        if (!paste || addr.empty()) return;
        usleep(120000);   // фокус повертається у вікно після закриття панелі
        bool term = cls.find("kitty") != std::string::npos || cls.find("term") != std::string::npos ||
                    cls.find("wezterm") != std::string::npos || cls.find("alacritty") != std::string::npos ||
                    cls.find("foot") != std::string::npos || cls.find("konsole") != std::string::npos;
        sysui::hypr_request(std::string("dispatch sendshortcut ") + (term ? "CTRL SHIFT" : "CTRL") + ", V, address:" + addr);
    }).detach();
}

static void delete_item(const ClipItem* it) {
    if (!valid_id(it->id)) return;
    system(("cliphist list | grep -P '^" + it->id + "\\t' | cliphist delete").c_str());
    remove((CACHE_DIR + "/" + it->id + ".png").c_str());
    populate_list();
}

static void clear_history(GtkButton*, gpointer) {
    if (!clear_armed) {
        clear_armed = true;
        gtk_button_set_label(GTK_BUTTON(clear_btn), "Точно очистити?");
        gtk_widget_add_css_class(clear_btn, "armed");
        g_timeout_add_seconds(3, [](gpointer) -> gboolean {
            clear_armed = false;
            gtk_button_set_label(GTK_BUTTON(clear_btn), "Очистити");
            gtk_widget_remove_css_class(clear_btn, "armed");
            return G_SOURCE_REMOVE;
        }, nullptr);
        return;
    }
    clear_armed = false;
    system("cliphist wipe");
    system(("rm -rf '" + CACHE_DIR + "'").c_str());
    mkdir(CACHE_DIR.c_str(), 0755);
    gtk_button_set_label(GTK_BUTTON(clear_btn), "Очистити");
    gtk_widget_remove_css_class(clear_btn, "armed");
    populate_list();
}

static ClipItem* row_item(GtkListBoxRow* row) {
    return row ? (ClipItem*)g_object_get_data(G_OBJECT(row), "clip") : nullptr;
}

// ================= МІНІАТЮРИ (у фоні) =================
struct ThumbJob { std::string id; GtkWidget* pic; };

static void load_thumb_async(const std::string& id, GtkWidget* pic) {
    g_object_ref(pic);
    std::thread([id, pic]() {
        std::string path = CACHE_DIR + "/" + id + ".png";
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            system(("cliphist decode " + id + " > '" + path + "' 2>/dev/null").c_str());
        auto* job = new ThumbJob{path, pic};
        g_idle_add([](gpointer d) -> gboolean {
            auto* j = (ThumbJob*)d;
            if (gtk_widget_get_root(j->pic)) {
                GdkTexture* t = sysui::load_texture_cover(j->id.c_str(), 680, 240);
                if (t) { gtk_picture_set_paintable(GTK_PICTURE(j->pic), GDK_PAINTABLE(t)); g_object_unref(t); }
            }
            g_object_unref(j->pic);
            delete j;
            return G_SOURCE_REMOVE;
        }, job);
    }).detach();
}

// ================= СПИСОК =================
static GtkWidget* make_row(ClipItem* it) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(box, "clip-item");

    if (it->is_image) {
        GtkWidget* pic = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
        gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
        gtk_widget_set_size_request(pic, -1, 110);
        gtk_widget_set_hexpand(pic, TRUE);
        gtk_widget_add_css_class(pic, "clip-img");
        gtk_box_append(GTK_BOX(box), pic);
        load_thumb_async(it->id, pic);
    } else {
        GtkWidget* icon = gtk_label_new(it->is_file ? "\U000F0214" : "\U000F09EE");
        gtk_widget_add_css_class(icon, it->is_file ? "clip-icon-file" : "clip-icon");
        gtk_widget_set_valign(icon, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), icon);

        GtkWidget* vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_hexpand(vb, TRUE);
        GtkWidget* lbl = gtk_label_new(it->text.c_str());
        gtk_widget_add_css_class(lbl, it->is_file ? "clip-file-text" : "clip-text");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        if (it->is_file) {
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
        } else {
            gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
            gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_lines(GTK_LABEL(lbl), 3);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        }
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 34);
        gtk_box_append(GTK_BOX(vb), lbl);
        if (it->is_file) {
            GtkWidget* sub = gtk_label_new("Файл");
            gtk_widget_add_css_class(sub, "clip-sub");
            gtk_widget_set_halign(sub, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(vb), sub);
        }
        gtk_box_append(GTK_BOX(box), vb);
    }

    GtkWidget* del = gtk_button_new_with_label("\U000F0156");
    gtk_widget_add_css_class(del, "clip-del");
    gtk_widget_set_valign(del, GTK_ALIGN_START);
    gtk_widget_set_focus_on_click(del, FALSE);
    gtk_widget_set_tooltip_text(del, "Видалити (Delete)");
    g_signal_connect(del, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) { delete_item((ClipItem*)d); }), it);
    gtk_box_append(GTK_BOX(box), del);

    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data_full(G_OBJECT(row), "clip", it, [](gpointer p) { delete (ClipItem*)p; });
    gtk_widget_set_cursor_from_name(row, "pointer");
    return row;
}

static void select_first_visible() {
    for (GtkWidget* c = gtk_widget_get_first_child(list_box); c; c = gtk_widget_get_next_sibling(c))
        if (GTK_IS_LIST_BOX_ROW(c) && gtk_widget_get_child_visible(c)) {
            gtk_list_box_select_row(GTK_LIST_BOX(list_box), GTK_LIST_BOX_ROW(c));
            return;
        }
}

static void populate_list() {
    gtk_list_box_remove_all(GTK_LIST_BOX(list_box));
    std::string out = exec("cliphist list 2>/dev/null | head -n " + std::to_string(MAX_ITEMS));
    std::istringstream ss(out);
    std::string line;
    int n = 0;
    while (std::getline(ss, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        auto* it = new ClipItem();
        it->id = line.substr(0, tab);
        std::string raw = line.substr(tab + 1);
        if (!valid_id(it->id)) { delete it; continue; }
        it->is_image = raw.rfind("[[ binary data", 0) == 0;
        if (it->is_image) {
            it->text = "Зображення";
            it->search = "зображення image картинка фото";
        } else {
            std::string t = trim(raw);
            if (t.rfind("file://", 0) == 0 || (t[0] == '/' && t.find('\n') == std::string::npos && access(t.c_str(), F_OK) == 0)) {
                it->is_file = true;
                std::string p = t.rfind("file://", 0) == 0 ? t.substr(7) : t;
                size_t sp = p.find(' ');
                if (sp != std::string::npos) p = p.substr(0, sp);     // кілька файлів — показуємо перший
                size_t sl = p.find_last_of('/');
                it->text = sl == std::string::npos ? p : p.substr(sl + 1);
                if (sp != std::string::npos) it->text += " та інші";
            } else {
                if (t.size() > 600) {                       // по межі UTF-8
                    size_t cut = 600;
                    while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) cut--;
                    t = t.substr(0, cut);
                }
                it->text = t;
            }
            it->search = lower(raw);
        }
        gtk_list_box_append(GTK_LIST_BOX(list_box), make_row(it));
        n++;
    }
    gtk_widget_set_visible(empty_lbl, n == 0);
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(list_box));
    select_first_visible();
}

static gboolean filter_func(GtkListBoxRow* row, gpointer) {
    ClipItem* it = row_item(row);
    if (!it || !search) return TRUE;
    std::string q = lower(gtk_editable_get_text(GTK_EDITABLE(search)));
    q = trim(q);
    return q.empty() || it->search.find(q) != std::string::npos;
}

// ================= IPC =================
static gboolean handle_ipc(gpointer) {
    gtk_widget_set_visible(clip_window, !gtk_widget_get_visible(clip_window));
    return G_SOURCE_REMOVE;
}

static void ipc_server_thread() {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return;
    std::string sock_path = sysui::home() + "/.config/SYSui/clipboard.sock";
    unlink(sock_path.c_str());
    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, sock_path.c_str(), sizeof(address.sun_path) - 1);
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) return;
    listen(server_fd, 5);
    while (true) {
        int client = accept(server_fd, nullptr, nullptr);
        if (client < 0) continue;
        char buffer[256] = {0};
        ssize_t r = read(client, buffer, sizeof(buffer) - 1); (void)r;
        if (strstr(buffer, "toggle")) g_idle_add(handle_ipc, nullptr);
        else if (strstr(buffer, "hide")) g_idle_add([](gpointer) -> gboolean { if (gtk_widget_get_visible(clip_window)) gtk_widget_set_visible(clip_window, FALSE); return G_SOURCE_REMOVE; }, nullptr);
        close(client);
    }
}

// ================= CSS =================
static void apply_css() {
    const bool light = sysui::is_light_theme();
    const std::string panel = light ? "rgba(246,246,248,0.95)" : "rgba(30,30,32,0.93)";
    const std::string border = light ? "rgba(0,0,0,0.12)" : "rgba(255,255,255,0.12)";
    const std::string fg = light ? "#1d1d1f" : "#f5f5f7";
    const std::string fg2 = light ? "#86868b" : "#98989f";
    const std::string field = light ? "rgba(0,0,0,0.06)" : "rgba(118,118,128,0.24)";
    const std::string card = light ? "rgba(255,255,255,0.9)" : "rgba(255,255,255,0.05)";
    std::string css = R"(
window, window.background { background: transparent; }
.clip-container, .clip-container * { font-family: "SF Pro Text", "Inter", sans-serif; outline: none; }
.clip-container {
    background-color: )" + panel + R"(; border: 1px solid )" + border + R"(;
    border-radius: 22px; padding: 14px 12px 12px 12px; color: )" + fg + R"(;
    box-shadow: 0 16px 44px rgba(0,0,0,0.42);
}
.clip-title { font-size: 15px; font-weight: 700; color: )" + fg + R"(; }
.clip-hint { font-size: 11px; color: )" + fg2 + R"(; }
button.clear-btn {
    background: none; border: none; box-shadow: none; color: )" + fg2 + R"(;
    font-size: 12px; font-weight: 500; padding: 3px 10px; border-radius: 99px; min-height: 0;
}
button.clear-btn:hover { background-color: )" + field + R"(; color: )" + fg + R"(; }
button.clear-btn.armed { background-color: rgba(255,69,58,0.9); color: white; }
entry.clip-search {
    background-color: )" + field + R"(; color: )" + fg + R"(; border: none; box-shadow: none;
    border-radius: 9px; min-height: 30px; padding: 0 10px; margin: 10px 2px 10px 2px; font-size: 13px;
}
entry.clip-search > text > placeholder { color: )" + fg2 + R"(; }
list.clip-list, list.clip-list > row { background: transparent; }
list.clip-list > row { padding: 0; margin: 0 0 6px 0; border-radius: 12px; }
.clip-item {
    padding: 9px 8px 9px 12px; border-radius: 12px; background-color: )" + card + R"(;
    border: 1px solid transparent;
    transition: background-color 150ms ease, border-color 150ms ease;
}
list.clip-list > row:hover .clip-item { background-color: )" + (light ? "rgba(0,0,0,0.05)" : "rgba(255,255,255,0.09)") + R"(; }
list.clip-list > row:selected .clip-item { border-color: @ACCENT_PH@; background-color: @ACCENT_SOFT_PH@; }
.clip-text, .clip-file-text { color: )" + fg + R"(; font-size: 13px; }
.clip-file-text { font-weight: 600; }
.clip-sub { color: )" + fg2 + R"(; font-size: 11px; }
.clip-icon, .clip-icon-file { font-family: "JetBrainsMono Nerd Font"; font-size: 15px; color: )" + fg2 + R"(; margin-top: 1px; }
.clip-icon-file { color: #ffd60a; }
.clip-img { border-radius: 9px; }
button.clip-del {
    background: none; border: none; box-shadow: none; padding: 0; min-width: 22px; min-height: 22px;
    border-radius: 99px; font-family: "JetBrainsMono Nerd Font"; font-size: 12px; color: )" + fg2 + R"(; opacity: 0;
}
list.clip-list > row:hover button.clip-del, list.clip-list > row:selected button.clip-del { opacity: 1; }
button.clip-del:hover { background-color: rgba(255,69,58,0.9); color: white; }
.empty-lbl { color: )" + fg2 + R"(; margin: 40px 10px; font-size: 13px; }
scrollbar { background: transparent; }
scrollbar slider { min-width: 5px; background-color: )" + (light ? "rgba(0,0,0,0.25)" : "rgba(255,255,255,0.22)") + R"(; border-radius: 99px; }
)";
    std::string a = sysui::accent();
    auto rep = [&](const std::string& k, const std::string& v) {
        size_t p = 0;
        while ((p = css.find(k, p)) != std::string::npos) { css.replace(p, k.size(), v); p += v.size(); }
    };
    rep("@ACCENT_SOFT_PH@", sysui::rgba(a, 0.20));
    rep("@ACCENT_PH@", sysui::rgba(a, 0.85));
    static GtkCssProvider* provider = nullptr;
    if (!provider) {
        provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string(provider, css.c_str());
}

// ================= КЛАВІШІ =================
static gboolean on_key_pressed(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer) {
    GtkListBoxRow* sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_box));
    if (keyval == GDK_KEY_Escape) { hide(); return TRUE; }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (ClipItem* it = row_item(sel)) restore_item(it, !(state & GDK_SHIFT_MASK));
        return TRUE;
    }
    if (keyval == GDK_KEY_Delete && sel) {
        if (ClipItem* it = row_item(sel)) delete_item(it);
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Up) {
        int dir = keyval == GDK_KEY_Down ? 1 : -1;
        GtkWidget* c = sel ? GTK_WIDGET(sel) : nullptr;
        GtkWidget* nxt = c;
        do {
            nxt = nxt ? (dir > 0 ? gtk_widget_get_next_sibling(nxt) : gtk_widget_get_prev_sibling(nxt))
                      : gtk_widget_get_first_child(list_box);
        } while (nxt && !(GTK_IS_LIST_BOX_ROW(nxt) && gtk_widget_get_child_visible(nxt)));
        if (nxt) {
            gtk_list_box_select_row(GTK_LIST_BOX(list_box), GTK_LIST_BOX_ROW(nxt));
            // прокрутити до вибраного
            GtkWidget* sw = gtk_widget_get_ancestor(list_box, GTK_TYPE_SCROLLED_WINDOW);
            graphene_rect_t b;
            if (sw && gtk_widget_compute_bounds(nxt, list_box, &b)) {
                GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
                double v = gtk_adjustment_get_value(adj), pg = gtk_adjustment_get_page_size(adj);
                if (b.origin.y < v) gtk_adjustment_set_value(adj, b.origin.y);
                else if (b.origin.y + b.size.height > v + pg) gtk_adjustment_set_value(adj, b.origin.y + b.size.height - pg);
            }
        }
        return TRUE;
    }
    return FALSE;   // решта — у поле пошуку
}

static void on_window_show(GObject*, GParamSpec*, gpointer) {
    if (!gtk_widget_get_visible(clip_window)) return;
    shown_at = g_get_monotonic_time();
    // куди потім вставляти: вікно, активне в момент відкриття
    target_addr.clear(); target_class.clear();
    try {
        auto aw = json::parse(sysui::hypr_request("j/activewindow"));
        target_addr = aw.value("address", "");
        target_class = aw.value("class", "");
    } catch (...) {}
    apply_css();
    gtk_editable_set_text(GTK_EDITABLE(search), "");
    populate_list();
    gtk_widget_grab_focus(search);

    // фактичний розмір панелі (довгі рядки можуть зробити її ширшою за WINDOW_WIDTH)
    int pw = WINDOW_WIDTH, ph = WINDOW_HEIGHT;
    {
        int mn = 0, nat = 0;
        gtk_widget_measure(clip_container, GTK_ORIENTATION_HORIZONTAL, -1, &mn, &nat, nullptr, nullptr);
        pw = std::max(pw, nat);
        gtk_widget_measure(clip_container, GTK_ORIENTATION_VERTICAL, pw, &mn, &nat, nullptr, nullptr);
        ph = std::max(ph, mn);
    }
    int tx = 100, ty = 100;
    try {
        auto cur = json::parse(sysui::hypr_request("j/cursorpos"));
        auto mons = json::parse(sysui::hypr_request("j/monitors"));
        json am = mons[0];
        for (auto& m : mons) if (m.value("focused", false)) { am = m; break; }
        double sc = am.value("scale", 1.0);
        int mx = am["x"], my = am["y"];
        int mw = (int)(am.value("width", 1920) / sc), mh = (int)(am.value("height", 1080) / sc);
        // поля layer-shell рахуються від краю робочої області (без топбару/доку з exclusive zone)
        int rl = 0, rt = 0, rr = 0, rb = 0;
        if (am.contains("reserved") && am["reserved"].size() == 4) {
            rl = am["reserved"][0]; rt = am["reserved"][1]; rr = am["reserved"][2]; rb = am["reserved"][3];
        }
        mx += rl; my += rt; mw -= rl + rr; mh -= rt + rb;
        tx = ((int)cur["x"] - mx) + 12;
        ty = ((int)cur["y"] - my) + 12;
        if (tx + pw > mw - 12) tx = mw - pw - 12;
        if (ty + ph > mh - 12) ty = mh - ph - 12;
        tx = std::max(12, tx); ty = std::max(12, ty);
    } catch (...) {}
    gtk_layer_set_margin(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_LEFT, tx);
    gtk_layer_set_margin(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_TOP, ty);
}

static void activate(GtkApplication* app, gpointer) {
    sysui::mem_ready();
    sysui::install_shared_style();
    mkdir(CACHE_DIR.c_str(), 0755);
    apply_css();

    clip_window = gtk_application_window_new(app);
    sysui::exclusive_overlay(clip_window, "clipboard");   // показ → інші оверлеї згортаються
    sysui::release_on_hide(clip_window);
    sysui::pop_on_show(clip_window, nullptr, "sysui-pop-down");
    gtk_layer_init_for_window(GTK_WINDOW(clip_window));
    gtk_layer_set_namespace(GTK_WINDOW(clip_window), "clipboard");
    gtk_layer_set_anchor(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_layer(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(clip_window), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    gtk_widget_set_size_request(clip_window, WINDOW_WIDTH, WINDOW_HEIGHT);
    gtk_widget_set_visible(clip_window, FALSE);

    GtkWidget* container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(container, "clip-container");
    clip_container = container;

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* tbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(tbox, TRUE);
    GtkWidget* title = gtk_label_new("Буфер обміну");
    gtk_widget_add_css_class(title, "clip-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    GtkWidget* hint = gtk_label_new("Enter — вставити · Shift+Enter — копіювати · Del — видалити");
    gtk_widget_add_css_class(hint, "clip-hint");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 30);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(tbox), title);
    gtk_box_append(GTK_BOX(tbox), hint);
    gtk_box_append(GTK_BOX(header), tbox);
    clear_btn = gtk_button_new_with_label("Очистити");
    gtk_widget_add_css_class(clear_btn, "clear-btn");
    gtk_widget_set_valign(clear_btn, GTK_ALIGN_START);
    gtk_widget_set_focus_on_click(clear_btn, FALSE);
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(clear_history), nullptr);
    gtk_box_append(GTK_BOX(header), clear_btn);
    gtk_box_append(GTK_BOX(container), header);

    search = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Пошук в історії…");
    gtk_widget_add_css_class(search, "clip-search");
    g_signal_connect(search, "changed", G_CALLBACK(+[](GtkEditable*, gpointer) {
        gtk_list_box_invalidate_filter(GTK_LIST_BOX(list_box));
        select_first_visible();
    }), nullptr);
    gtk_box_append(GTK_BOX(container), search);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    list_box = gtk_list_box_new();
    gtk_widget_add_css_class(list_box, "clip-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list_box), TRUE);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(list_box), filter_func, nullptr, nullptr);
    gtk_widget_set_focusable(list_box, FALSE);
    g_signal_connect(list_box, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer) {
        GdkModifierType st = (GdkModifierType)0;
        if (GdkDisplay* d = gdk_display_get_default())
            if (GdkSeat* seat = gdk_display_get_default_seat(d))
                if (GdkDevice* kb = gdk_seat_get_keyboard(seat)) st = gdk_device_get_modifier_state(kb);
        if (ClipItem* it = row_item(row)) restore_item(it, !(st & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)));
    }), nullptr);
    empty_lbl = gtk_label_new("Історія порожня — скопіюйте текст, файл чи картинку");
    gtk_widget_add_css_class(empty_lbl, "empty-lbl");
    gtk_label_set_wrap(GTK_LABEL(empty_lbl), TRUE);
    gtk_box_append(GTK_BOX(inner), empty_lbl);
    gtk_box_append(GTK_BOX(inner), list_box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), inner);
    gtk_box_append(GTK_BOX(container), scroll);

    gtk_window_set_child(GTK_WINDOW(clip_window), sysui::bb::underlay(clip_window, container, "clipboard", 22));   // запечений блюр

    GtkEventController* key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), nullptr);
    gtk_widget_add_controller(clip_window, key_ctrl);

    // клік в інше вікно — панель ховається (як спливне меню), але не в перші пів секунди після появи
    g_signal_connect(clip_window, "notify::is-active", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        if (gtk_window_is_active(GTK_WINDOW(o)) || !gtk_widget_get_visible(GTK_WIDGET(o))) return;
        if (g_get_monotonic_time() - shown_at < 500000) return;
        hide();
    }), nullptr);
    g_signal_connect(clip_window, "notify::visible", G_CALLBACK(on_window_show), nullptr);

    std::thread(ipc_server_thread).detach();
}

int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.clipboard", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
