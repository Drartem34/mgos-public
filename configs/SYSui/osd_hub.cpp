// osd_hub.cpp — УСІ спливаючі OSD mgOS в ОДНОМУ процесі:
//   гучність (volume.sock), яскравість (brightness.sock), сповіщення (notify.sock), музика (music.sock).
// Раніше це були 4 окремі GTK-процеси (~210 МБ RAM разом); тепер один (~60 МБ).
//
// Кожен модуль лишається окремим файлом (volume_osd.cpp, brightness_osd.cpp, notify_osd.cpp, osd.cpp)
// і підключається сюди у власному просторі імен — їхні глобальні змінні й функції не перетинаються.
// CSS-класи модулів унікальні (vol-*, bri-*, ntf-*, mus-*), бо стилі в одному процесі спільні.
// Сокети, простори імен шарів Hyprland (volume_osd, brightness_osd, notify_osd, megumin_osd) — ті самі,
// тож гарячі клавіші, dunst-хук і правила розмиття працюють без змін.

// Усі системні заголовки — ДО просторів імен (інакше вони потрапили б усередину namespace)
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include "sysui_common.h"
#include <gio/gdesktopappinfo.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>
#include <thread>
#include <map>
#include <set>
#include <deque>
#include <chrono>
#include <ctime>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>

namespace vol {
#include "volume_osd.cpp"
}
namespace bri {
#include "brightness_osd.cpp"
}
namespace ntf {
#include "notify_osd.cpp"
}
namespace mus {
#include "osd.cpp"
}

static void hub_activate(GtkApplication* app, gpointer) {
    vol::activate(app, nullptr);
    bri::activate(app, nullptr);
    ntf::activate(app, nullptr);
    mus::activate(app, nullptr);
}

int main(int argc, char** argv) {
    sysui::mem_init();
    GtkApplication* app = gtk_application_new("com.megumin.osdhub", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(hub_activate), nullptr);
    g_application_hold(G_APPLICATION(app));
    return g_application_run(G_APPLICATION(app), argc, argv);
}
