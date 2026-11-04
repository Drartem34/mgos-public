#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <malloc.h>

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

// ===================== СТАН ТА НАЛАШТУВАННЯ =====================
static GtkWidget* runner_window  = nullptr;
static GtkWidget* search_entry   = nullptr;
static GtkWidget* results_scroll = nullptr;
static GtkWidget* results_list   = nullptr;
static GtkCssProvider* g_css_provider = nullptr;

static bool g_window_was_active = false;
static time_t last_wal_mtime = 0;
static guint file_search_debounce_id = 0;

struct AppItem {
    GtkWidget* row_widget;
    GAppInfo*  app_info;
    std::string name_lower;
    std::string exec_lower;
};
static std::vector<AppItem> g_app_items;
static std::vector<GtkWidget*> g_dynamic_widgets;

static std::string get_socket_path() {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && strlen(xdg) > 0) {
        return std::string(xdg) + "/kiosk_runner.sock";
    }
    return "/tmp/kiosk_runner.sock";
}

// ===================== УТИЛІТИ =====================
static std::string to_lower_fast(const std::string& s) {
    char* lower = g_utf8_strdown(s.c_str(), -1);
    std::string res = lower ? lower : s;
    g_free(lower);
    return res;
}

static void trim_memory() {
    malloc_trim(0);
}

// ===================== СУЧАСНИЙ ДИЗАЙН ТА ЗАКРУГЛЕННЯ (CSS) =====================
static void apply_css(bool force = false) {
    if (!g_css_provider) return;

    std::string wal_path = std::string(getenv("HOME") ? getenv("HOME") : "") + "/.cache/wal/colors.css";
    struct stat st;
    if (!force && stat(wal_path.c_str(), &st) == 0) {
        if (st.st_mtime == last_wal_mtime) return;
        last_wal_mtime = st.st_mtime;
    }

    std::string css = R"(
        window {
            background: transparent;
        }

        * {
            font-family: "JetBrainsMono Nerd Font", "CaskaydiaCove Nerd Font", "SF Pro Display", sans-serif;
            outline: none;
            box-shadow: none;
        }

        /* Головна картка Spotlight */
        .main-container {
            background-color: rgba(13, 13, 17, 0.94);
            border: 1px solid rgba(255, 255, 255, 0.12);
            border-radius: 28px;
            padding: 16px 18px 18px 18px;
            box-shadow: 0 20px 50px rgba(0, 0, 0, 0.65);
        }

        /* Пошукова плашка */
        .search-box {
            background-color: rgba(255, 255, 255, 0.06);
            border: 1px solid rgba(255, 255, 255, 0.10);
            border-radius: 20px;
            padding: 6px 14px;
            margin-bottom: 12px;
            transition: all 0.2s ease;
        }

        .search-box:focus-within {
            background-color: rgba(255, 255, 255, 0.09);
            border: 1px solid rgba(255, 255, 255, 0.22);
        }

        .search-icon {
            color: rgba(255, 255, 255, 0.5);
            font-size: 17px;
            margin-right: 8px;
        }

        /* Прибираємо стандартні GTK прямокутники в середині entry */
        .search-entry,
        .search-entry > text,
        .search-entry text {
            background-color: transparent;
            background: none;
            border: none;
            box-shadow: none;
            outline: none;
            color: #ffffff;
            font-size: 17px;
            font-weight: 500;
            padding: 4px 0px;
        }

        .hint-badge {
            background-color: rgba(255, 255, 255, 0.10);
            border: 1px solid rgba(255, 255, 255, 0.14);
            border-radius: 8px;
            padding: 2px 7px;
            font-size: 11px;
            font-weight: 600;
            color: rgba(255, 255, 255, 0.55);
        }

        /* Контейнер списку */
        scrolledwindow, viewport, flowbox, .results-list {
            background: transparent;
            border: none;
        }

        /* Елементи сітки програм */
        window .main-container .results-list flowboxchild {
            background-color: transparent;
            border: 1px solid transparent;
            border-radius: 18px;
            padding: 10px 6px;
            margin: 4px;
            transition: all 0.12s ease;
        }

        window .main-container .results-list flowboxchild:selected,
        window .main-container .results-list flowboxchild:focus,
        window .main-container .results-list flowboxchild:hover {
            background-color: rgba(255, 255, 255, 0.15);
            border: 1px solid rgba(255, 255, 255, 0.22);
        }

        .app-icon {
            -gtk-icon-shadow: 0 4px 10px rgba(0, 0, 0, 0.35);
        }

        .app-name { 
            color: #ffffff; 
            font-size: 13px; 
            font-weight: 500;
            margin-top: 4px;
            text-shadow: 0 1px 3px rgba(0,0,0,0.5);
        }

        .app-sub {
            color: rgba(255, 255, 255, 0.45);
            font-size: 11px;
            font-weight: 400;
        }

        scrollbar slider {
            background-color: rgba(255, 255, 255, 0.22);
            border-radius: 10px;
            min-width: 6px;
        }
        scrollbar slider:hover {
            background-color: rgba(255, 255, 255, 0.38);
        }
    )";
    
    gtk_css_provider_load_from_string(g_css_provider, css.c_str());
}

// ===================== КЕРУВАННЯ ВІКНОМ =====================
static void hide_runner();

static void launch_app(GAppInfo* app) {
    if (app) {
        GError* err = nullptr;
        GdkDisplay* display = gdk_display_get_default();
        GdkAppLaunchContext* ctx = display ? gdk_display_get_app_launch_context(display) : nullptr;
        g_app_info_launch(app, nullptr, ctx ? G_APP_LAUNCH_CONTEXT(ctx) : nullptr, &err);
        if (ctx) g_object_unref(ctx);
        if (err) {
            const char* cmdline = g_app_info_get_commandline(app);
            if (cmdline) g_spawn_command_line_async(cmdline, nullptr);
            g_error_free(err);
        }
    }
    hide_runner();
}

static void launch_raw(const std::string& cmd) {
    if (!cmd.empty()) {
        g_spawn_command_line_async(cmd.c_str(), nullptr);
    }
    hide_runner();
}

static void hide_runner() {
    if (file_search_debounce_id > 0) {
        g_source_remove(file_search_debounce_id);
        file_search_debounce_id = 0;
    }
    g_window_was_active = false;
    gtk_widget_set_visible(runner_window, FALSE);
    trim_memory();
}

static void update_results(const std::string& query);

static void show_runner() {
    apply_css(false);
    gtk_editable_set_text(GTK_EDITABLE(search_entry), "");
    
    // ВІДРАЗУ показуємо всі програми у гарній сітці з 6 колонок
    update_results("");

    gtk_widget_set_visible(runner_window, TRUE);
    gtk_window_present(GTK_WINDOW(runner_window));
    gtk_widget_grab_focus(search_entry);
}

static void toggle_runner() {
    if (gtk_widget_get_visible(runner_window)) {
        hide_runner();
    } else {
        show_runner();
    }
}

// ===================== АКТИВАЦІЯ РЯДКА =====================
static void on_row_activated(GtkFlowBox*, GtkFlowBoxChild* child, gpointer) {
    if (!child) return;
    GAppInfo* app = G_APP_INFO(g_object_get_data(G_OBJECT(child), "app-info"));
    char* raw_cmd = (char*)g_object_get_data(G_OBJECT(child), "raw-cmd");

    if (app) {
        launch_app(app);
    } else if (raw_cmd) {
        launch_raw(raw_cmd);
    }
}

// ===================== СТВОРЕННЯ КАРТКИ ПРОГРАМИ =====================
static GtkWidget* create_app_widget(GAppInfo* app) {
    GtkWidget* row = gtk_flow_box_child_new();
    gtk_widget_add_css_class(row, "app-row");
    
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);

    GIcon* gicon = g_app_info_get_icon(app);
    GtkWidget* icon = gicon ? gtk_image_new_from_gicon(gicon) : gtk_image_new_from_icon_name("application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    gtk_widget_add_css_class(icon, "app-icon");

    const char* name = g_app_info_get_display_name(app);
    GtkWidget* name_lbl = gtk_label_new(name ? name : "Unknown");
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(name_lbl), 11);
    gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(name_lbl, "app-name");

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), name_lbl);
    
    g_object_set_data(G_OBJECT(row), "app-info", app);
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(row), box);
    return row;
}

static void preload_apps() {
    g_app_items.clear();
    GList* apps = g_app_info_get_all();
    for (GList* l = apps; l != nullptr; l = l->next) {
        GAppInfo* app = G_APP_INFO(l->data);
        if (!g_app_info_should_show(app)) continue;

        const char* name = g_app_info_get_display_name(app);
        const char* exec = g_app_info_get_executable(app);
        if (!name) continue;

        GtkWidget* widget = create_app_widget(app);
        gtk_flow_box_append(GTK_FLOW_BOX(results_list), widget);

        g_object_ref(app);
        g_app_items.push_back({
            widget,
            app,
            to_lower_fast(name),
            exec ? to_lower_fast(exec) : ""
        });
    }
    g_list_free_full(apps, g_object_unref);
}

// ===================== АСИНХРОННИЙ ПОШУК ФАЙЛІВ =====================
static gboolean perform_file_search_async(gpointer data) {
    char* query_str = (char*)data;
    std::string query = query_str ? query_str : "";
    g_free(query_str);
    file_search_debounce_id = 0;

    if (query.length() < 3) return G_SOURCE_REMOVE;

    std::string cmd = "locate -i -l 4 '" + query + "' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return G_SOURCE_REMOVE;

    char buf[512];
    std::vector<std::string> files;
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line = buf;
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty()) files.push_back(line);
    }
    pclose(pipe);

    for (const auto& f : files) {
        GtkWidget* row = gtk_flow_box_child_new();
        gtk_widget_add_css_class(row, "app-row");
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_set_margin_end(box, 10);

        GtkWidget* icon = gtk_image_new_from_icon_name("text-x-generic");
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 36);

        size_t slash = f.find_last_of('/');
        std::string filename = (slash != std::string::npos) ? f.substr(slash + 1) : f;
        
        GtkWidget* v_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget* lbl = gtk_label_new(filename.c_str());
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 36);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(lbl, "app-name");

        GtkWidget* sub_lbl = gtk_label_new(f.c_str());
        gtk_widget_set_halign(sub_lbl, GTK_ALIGN_START);
        gtk_label_set_max_width_chars(GTK_LABEL(sub_lbl), 45);
        gtk_label_set_ellipsize(GTK_LABEL(sub_lbl), PANGO_ELLIPSIZE_START);
        gtk_widget_add_css_class(sub_lbl, "app-sub");

        gtk_box_append(GTK_BOX(v_box), lbl);
        gtk_box_append(GTK_BOX(v_box), sub_lbl);

        gtk_box_append(GTK_BOX(box), icon);
        gtk_box_append(GTK_BOX(box), v_box);

        std::string open_cmd = "xdg-open '" + f + "'";
        g_object_set_data_full(G_OBJECT(row), "raw-cmd", g_strdup(open_cmd.c_str()), g_free);
        gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(row), box);

        gtk_flow_box_append(GTK_FLOW_BOX(results_list), row);
        g_dynamic_widgets.push_back(row);
    }

    return G_SOURCE_REMOVE;
}

// ===================== ФІЛЬТРАЦІЯ =====================
static void update_results(const std::string& query) {
    for (GtkWidget* w : g_dynamic_widgets) {
        gtk_flow_box_remove(GTK_FLOW_BOX(results_list), w);
    }
    g_dynamic_widgets.clear();

    if (file_search_debounce_id > 0) {
        g_source_remove(file_search_debounce_id);
        file_search_debounce_id = 0;
    }

    std::string lower_query = to_lower_fast(query);
    int match_count = 0;
    bool is_empty = query.empty();

    // Завжди тримаємо сітку 6 колонок
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(results_list), 6);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(results_list), 6);
    gtk_widget_set_halign(results_list, GTK_ALIGN_CENTER);

    for (auto& item : g_app_items) {
        bool visible = is_empty || (item.name_lower.find(lower_query) != std::string::npos ||
                                    item.exec_lower.find(lower_query) != std::string::npos);
        gtk_widget_set_visible(item.row_widget, visible);
        if (visible) match_count++;
    }

    // Системні дії
    if (!is_empty) {
        if (lower_query == "reboot" || lower_query == "poweroff" || lower_query == "shutdown" || lower_query == "lock") {
            GtkWidget* row = gtk_flow_box_child_new();
            gtk_widget_add_css_class(row, "app-row");
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_widget_set_margin_top(box, 8);
            gtk_widget_set_margin_bottom(box, 8);
            gtk_widget_set_margin_start(box, 6);
            gtk_widget_set_margin_end(box, 6);
            gtk_widget_set_halign(box, GTK_ALIGN_CENTER);

            GtkWidget* icon = gtk_image_new_from_icon_name("system-shutdown");
            gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
            gtk_widget_add_css_class(icon, "app-icon");

            std::string action_label = (lower_query == "reboot") ? "Reboot" : 
                                       (lower_query == "lock")   ? "Lock" : "Power Off";
            GtkWidget* lbl = gtk_label_new(action_label.c_str());
            gtk_widget_add_css_class(lbl, "app-name");

            gtk_box_append(GTK_BOX(box), icon);
            gtk_box_append(GTK_BOX(box), lbl);

            std::string cmd = (lower_query == "reboot") ? "systemctl reboot" : 
                              (lower_query == "lock")   ? "hyprctl dispatch dpms off" : "systemctl poweroff";
            g_object_set_data_full(G_OBJECT(row), "raw-cmd", g_strdup(cmd.c_str()), g_free);
            gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(row), box);
            gtk_flow_box_prepend(GTK_FLOW_BOX(results_list), row);
            g_dynamic_widgets.push_back(row);
            match_count++;
        } else if (match_count < 6 && query.length() >= 3) {
            file_search_debounce_id = g_timeout_add(180, perform_file_search_async, g_strdup(query.c_str()));
        }
    }

    // Авто-вибір першого доступного елемента
    if (!g_dynamic_widgets.empty()) {
        gtk_flow_box_select_child(GTK_FLOW_BOX(results_list), GTK_FLOW_BOX_CHILD(g_dynamic_widgets[0]));
    } else {
        for (auto& item : g_app_items) {
            if (gtk_widget_get_visible(item.row_widget)) {
                gtk_flow_box_select_child(GTK_FLOW_BOX(results_list), GTK_FLOW_BOX_CHILD(item.row_widget));
                break;
            }
        }
    }
}

// ===================== НАВІГАЦІЯ ТА КЛАВІАТУРА =====================
static void on_search_changed(GtkEditable* editable, gpointer) {
    const char* text = gtk_editable_get_text(editable);
    std::string query(text ? text : "");
    update_results(query);
}

// Допоміжна функція пошуку сусідніх видимих віджетів
static GtkWidget* get_sibling_visible(GtkWidget* current, bool forward, int count = 1) {
    GtkWidget* w = current;
    for (int i = 0; i < count; ++i) {
        GtkWidget* next = w ? (forward ? gtk_widget_get_next_sibling(w) : gtk_widget_get_prev_sibling(w)) : nullptr;
        while (next && !gtk_widget_get_visible(next)) {
            next = forward ? gtk_widget_get_next_sibling(next) : gtk_widget_get_prev_sibling(next);
        }
        if (next) w = next;
        else break;
    }
    return w;
}

static gboolean on_entry_key(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer) {
    if (keyval == GDK_KEY_Escape) {
        hide_runner();
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        GList* selected = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(results_list));
        if (selected) {
            GtkFlowBoxChild* child = GTK_FLOW_BOX_CHILD(selected->data);
            g_signal_emit_by_name(results_list, "child-activated", child);
            g_list_free(selected);
        } else {
            const char* text = gtk_editable_get_text(GTK_EDITABLE(search_entry));
            if (text && strlen(text) > 0) launch_raw(text);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Right || keyval == GDK_KEY_Left || keyval == GDK_KEY_Down || keyval == GDK_KEY_Up) {
        GList* selected = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(results_list));
        GtkWidget* current = selected ? GTK_WIDGET(selected->data) : nullptr;
        if (selected) g_list_free(selected);

        GtkWidget* next = nullptr;
        if (!current) {
            for (auto& item : g_app_items) {
                if (gtk_widget_get_visible(item.row_widget)) { next = item.row_widget; break; }
            }
        } else {
            if (keyval == GDK_KEY_Right) next = get_sibling_visible(current, true, 1);
            else if (keyval == GDK_KEY_Left) next = get_sibling_visible(current, false, 1);
            else if (keyval == GDK_KEY_Down) next = get_sibling_visible(current, true, 6);
            else if (keyval == GDK_KEY_Up) next = get_sibling_visible(current, false, 6);
        }

        if (next && next != current) {
            gtk_flow_box_select_child(GTK_FLOW_BOX(results_list), GTK_FLOW_BOX_CHILD(next));
        }
        return TRUE;
    }
    return FALSE;
}

// ===================== UNIX СОКЕТ =====================
static gboolean on_socket_message(GIOChannel* source, GIOCondition, gpointer) {
    int server_fd = g_io_channel_unix_get_fd(source);
    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd >= 0) {
        char buf[64] = {0};
        int bytes = read(client_fd, buf, sizeof(buf) - 1);
        if (bytes > 0) {
            std::string cmd(buf);
            if (cmd.find("toggle") != std::string::npos) toggle_runner();
            else if (cmd.find("show") != std::string::npos) show_runner();
            else if (cmd.find("hide") != std::string::npos) hide_runner();
        }
        close(client_fd);
    }
    return G_SOURCE_CONTINUE;
}

static void setup_ipc_socket() {
    std::string path = get_socket_path();
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 5) < 0) {
        close(fd);
        return;
    }
    
    GIOChannel* ch = g_io_channel_unix_new(fd);
    g_io_add_watch(ch, G_IO_IN, on_socket_message, nullptr);
}

// ===================== ГОЛОВНИЙ UI =====================
static void activate(GtkApplication* app, gpointer) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    setup_ipc_socket();

    g_css_provider = gtk_css_provider_new();
    apply_css(true);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(g_css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    runner_window = gtk_application_window_new(app);
    gtk_layer_init_for_window(GTK_WINDOW(runner_window));
    gtk_layer_set_namespace(GTK_WINDOW(runner_window), "runner");
    gtk_layer_set_layer(GTK_WINDOW(runner_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(runner_window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(runner_window), GTK_LAYER_SHELL_EDGE_TOP, 130);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(runner_window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    // Закриття при кліку поза вікном
    g_signal_connect(runner_window, "notify::is-active", G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer) {
        if (gtk_window_is_active(GTK_WINDOW(obj))) {
            g_window_was_active = true;
        } else if (g_window_was_active && gtk_widget_get_visible(GTK_WIDGET(obj))) {
            hide_runner();
        }
    }), nullptr);

    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(main_box, "main-container");
    gtk_widget_set_size_request(main_box, 820, -1);
    gtk_widget_set_halign(main_box, GTK_ALIGN_CENTER);

    // Пошукова плашка
    GtkWidget* search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(search_box, "search-box");

    GtkWidget* search_icon = gtk_label_new("󰍉");
    gtk_widget_add_css_class(search_icon, "search-icon");

    search_entry = gtk_entry_new();
    gtk_widget_set_hexpand(search_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "Шукати програму, файл або дію...");
    gtk_widget_add_css_class(search_entry, "search-entry");

    GtkWidget* esc_hint = gtk_label_new("ESC");
    gtk_widget_add_css_class(esc_hint, "hint-badge");

    GtkEventController* entry_keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(entry_keys, GTK_PHASE_CAPTURE);
    g_signal_connect(entry_keys, "key-pressed", G_CALLBACK(on_entry_key), nullptr);
    gtk_widget_add_controller(search_entry, entry_keys);
    g_signal_connect(search_entry, "changed", G_CALLBACK(on_search_changed), nullptr);

    gtk_box_append(GTK_BOX(search_box), search_icon);
    gtk_box_append(GTK_BOX(search_box), search_entry);
    gtk_box_append(GTK_BOX(search_box), esc_hint);

    // Область списку програм (ВІДРАЗУ РОЗГОРНУТА)
    results_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(results_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(results_scroll), 450);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(results_scroll), TRUE);

    results_list = gtk_flow_box_new();
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(results_list), 6);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(results_list), 6);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(results_list), GTK_SELECTION_SINGLE);
    gtk_widget_set_halign(results_list, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(results_list, "results-list");
    g_signal_connect(results_list, "child-activated", G_CALLBACK(on_row_activated), nullptr);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(results_scroll), results_list);

    gtk_box_append(GTK_BOX(main_box), search_box);
    gtk_box_append(GTK_BOX(main_box), results_scroll);
    gtk_window_set_child(GTK_WINDOW(runner_window), main_box);

    preload_apps();
    gtk_widget_set_visible(runner_window, FALSE);
    trim_memory();
}

// ===================== КЛІЄНТ IPC =====================
static int send_client_command(const char* cmd) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::string path = get_socket_path();
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return 1;
    }

    write(sock, cmd, strlen(cmd));
    close(sock);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--toggle" || arg == "-t") return send_client_command("toggle");
        if (arg == "--show"   || arg == "-s") return send_client_command("show");
        if (arg == "--hide"   || arg == "-h") return send_client_command("hide");
    }

    if (send_client_command("toggle") == 0) {
        return 0;
    }

    GtkApplication* app = gtk_application_new("com.kiosk.runner", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    return status;
}
