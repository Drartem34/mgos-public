// mgos-pageout — «стиснути» сховану програму: її анонімна пам'ять іде в swap (zram, стискання zstd).
//
//   mgos-pageout PID...          — лише ці процеси
//   mgos-pageout --tree PID...   — процес і всі його нащадки (вкладки Firefox, процеси Electron)
//
// Ядро дозволяє process_madvise(MADV_PAGEOUT) для чужого процесу лише з CAP_SYS_NICE, тому
// бінарник ставиться в /usr/local/bin з `setcap cap_sys_nice+ep` (mgos-zram setup / установщик).
// Обмеження: тільки процеси ТОГО САМОГО користувача, що викликав (реальний UID), і лише підказка
// ядру «ці сторінки можна витіснити» — дані не губляться, при зверненні вони повертаються з zram.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_process_madvise
#define SYS_process_madvise 440
#endif

#define MAXP 4096
static pid_t pids[MAXP];
static int npids = 0;

static int is_num(const char* s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static int owned(pid_t pid) {   // процес належить тому, хто нас викликав
    char p[64];
    struct stat st;
    snprintf(p, sizeof p, "/proc/%d", pid);
    return stat(p, &st) == 0 && st.st_uid == getuid();
}

static pid_t ppid_of(pid_t pid) {
    char p[64], buf[512];
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    FILE* f = fopen(p, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char* rp = strrchr(buf, ')');   // ім'я процесу може містити пробіли й дужки
    if (!rp) return -1;
    char st; int pp;
    if (sscanf(rp + 2, "%c %d", &st, &pp) != 2) return -1;
    return pp;
}

static void add(pid_t pid) {
    for (int i = 0; i < npids; i++) if (pids[i] == pid) return;
    if (npids < MAXP) pids[npids++] = pid;
}

static void add_tree(pid_t root) {
    add(root);
    int changed = 1;
    while (changed) {   // нащадки будь-якої глибини
        changed = 0;
        DIR* d = opendir("/proc");
        if (!d) return;
        struct dirent* e;
        while ((e = readdir(d))) {
            if (!is_num(e->d_name)) continue;
            pid_t p = atoi(e->d_name), pp = ppid_of(p);
            if (pp <= 0) continue;
            int have_parent = 0, have_self = 0;
            for (int i = 0; i < npids; i++) { if (pids[i] == pp) have_parent = 1; if (pids[i] == p) have_self = 1; }
            if (have_parent && !have_self && owned(p)) { add(p); changed = 1; }
        }
        closedir(d);
    }
}

// анонімна приватна пам'ять: [heap], [stack], [anon:...] і безіменні rw-p відображення
static int wanted(const char* perms, const char* path) {
    if (perms[3] != 'p') return 0;                       // лише приватні (shared/shmem не чіпаємо)
    if (!*path) return 1;
    if (!strcmp(path, "[heap]") || !strncmp(path, "[stack", 6) || !strncmp(path, "[anon:", 6)) return 1;
    return 0;
}

static long long pageout(pid_t pid) {
    if (!owned(pid)) return -1;
    int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pfd < 0) return -1;
    char p[64], line[4096];
    snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE* f = fopen(p, "r");
    if (!f) { close(pfd); return -1; }
    long long total = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char perms[8] = "", path[4096] = "";
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %4095[^\n]", &a, &b, perms, path) < 3) continue;
        char* s = path;
        while (*s == ' ') s++;
        if (!wanted(perms, s)) continue;
        struct iovec iov = {(void*)a, b - a};
        // по одному відображенню: заблоковані (mlock) ядро відхилить, решта пройде
        if (syscall(SYS_process_madvise, pfd, &iov, 1, MADV_PAGEOUT, 0) >= 0) total += (long long)(b - a);
    }
    fclose(f);
    close(pfd);
    return total;
}

static long rss_anon_kb(pid_t pid) {   // RssAnon з /proc/PID/status (кБ), -1 — недоступно
    char p[64], line[256];
    snprintf(p, sizeof p, "/proc/%d/status", pid);
    FILE* f = fopen(p, "r");
    if (!f) return -1;
    long v = -1;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "RssAnon:", 8)) { v = atol(line + 8); break; }
    fclose(f);
    return v;
}

int main(int argc, char** argv) {
    int tree = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tree")) { tree = 1; continue; }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            puts("mgos-pageout [--tree] PID...  — витіснити анонімну пам'ять процесів у swap (zram)");
            return 0;
        }
        if (!is_num(argv[i])) { fprintf(stderr, "не PID: %s\n", argv[i]); return 2; }
        pid_t pid = atoi(argv[i]);
        if (pid <= 1 || !owned(pid)) { fprintf(stderr, "пропуск %d: не ваш процес\n", pid); continue; }
        if (tree) add_tree(pid); else add(pid);
    }
    long long freed_kb = 0;
    int ok = 0;
    for (int i = 0; i < npids; i++) {
        long before = rss_anon_kb(pids[i]);
        long long r = pageout(pids[i]);   // MADV_PAGEOUT витісняє синхронно
        if (r >= 0) {
            ok++;
            long after = rss_anon_kb(pids[i]);
            if (before > 0 && after >= 0 && before > after) freed_kb += before - after;
        }
    }
    printf("процесів: %d, витіснено в zram: %lld МБ\n", ok, freed_kb >> 10);
    return ok ? 0 : 1;
}
