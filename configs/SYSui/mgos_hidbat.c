// mgos-hidbat — заряд Bluetooth-периферії Apple (Magic Mouse / Magic Keyboard / Magic Trackpad).
//
// Ядро (hid-input) питає заряд звітом типу FEATURE, а ці пристрої віддають звіт 0x90 лише як INPUT —
// тому /sys/class/power_supply/hid-<MAC>-battery по Bluetooth показує 0 %, доки пристрій сам не надішле звіт
// (а надсилає він рідко). Тут робимо той запит, який пристрій розуміє: HIDIOCGINPUT(0x90) → [0x90, флаги, заряд %].
//
// Ставиться в /usr/local/bin з cap_dac_override (крок інсталятора), щоб topbar міг питати без root.
// Тому навмисно нічого не бере ззовні: ні аргументів, ні змінних оточення; шляхи — лише з sysfs, лише пристрої
// Apple по Bluetooth (0005:004C:*), лише один запит заряду. Сирий HID (і натискання клавіш) нікому не відкриває.
// Вивід: <ім'я power_supply>\t<відсоток>\t<1 — заряджається>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

int main(void) {
    DIR* ps = opendir("/sys/class/power_supply");
    if (!ps) return 1;
    struct dirent* e;
    while ((e = readdir(ps))) {
        if (strncmp(e->d_name, "hid-", 4) != 0) continue;
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof link, "/sys/class/power_supply/%s/device", e->d_name);
        if (!realpath(link, real)) continue;
        const char* base = strrchr(real, '/');
        if (!base || strncmp(base + 1, "0005:004C:", 10) != 0) continue;   // лише Apple по Bluetooth
        char hdir[PATH_MAX + 16];
        snprintf(hdir, sizeof hdir, "%s/hidraw", real);
        DIR* hd = opendir(hdir);
        if (!hd) continue;
        struct dirent* h;
        while ((h = readdir(hd))) {
            if (strncmp(h->d_name, "hidraw", 6) != 0 || !isdigit((unsigned char)h->d_name[6])) continue;
            const char* p = h->d_name + 6;
            while (*p && isdigit((unsigned char)*p)) p++;
            if (*p) continue;
            char dev[300];
            snprintf(dev, sizeof dev, "/dev/%s", h->d_name);
            int fd = open(dev, O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            unsigned char b[16] = {0x90};
            int r = ioctl(fd, HIDIOCGINPUT(sizeof b), b);
            close(fd);
            if (r >= 3 && b[0] == 0x90) {
                int pct = b[2] > 100 ? 100 : b[2];
                printf("%s\t%d\t%d\n", e->d_name, pct, (b[1] & 0x02) ? 1 : 0);
            }
            break;
        }
        closedir(hd);
    }
    closedir(ps);
    return 0;
}
