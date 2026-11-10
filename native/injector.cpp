// injector.cpp — root daemon: ptrace-inject libcodm.so into the CODM process
//
// The payload is pre-staged into the game's own data dir before the dlopen:
// /data/adb is root-only, and `dlopen` runs as the app uid inside the target.
#include "common.h"
#include "remote.h"

#include <dirent.h>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>

// shipped inside the module; root can read it
static const char *kPayloadSrc = "/data/adb/modules/codm_overlay/assets/libcodm.so";
static const char *kLog        = "/data/adb/codm/injector.log";

static void logline(const char *fmt, ...)
{
    mkdir(CODM_STATE_DIR, 0755);
    FILE *f = fopen(kLog, "a");
    if (!f)
        return;
    char ts[32];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(f, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static pid_t find_pid(const char *pkg)
{
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    pid_t found = -1;
    dirent *de;
    char want[256];
    snprintf(want, sizeof(want), "%s", pkg);
    while ((de = readdir(d))) {
        char *end = nullptr;
        long pid = strtol(de->d_name, &end, 10);
        if (!end || *end || pid <= 0)
            continue;
        char path[64], cmd[256] = {0};
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n <= 0)
            continue;
        cmd[n] = 0;
        if (strcmp(cmd, want) == 0) {
            found = (pid_t)pid;
            break;
        }
    }
    closedir(d);
    return found;
}

static bool injected_before(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[512];
    bool hit = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libcodm")) {
            hit = true;
            break;
        }
    }
    fclose(f);
    return hit;
}

// the app uid owns the payload copy; dlopen then runs with app credentials
static bool read_ids(pid_t pid, uid_t *uid, gid_t *gid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    bool gu = false, gg = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            unsigned r, e, s, fs;
            if (sscanf(line + 4, "%u%u%u%u", &r, &e, &s, &fs) >= 1) {
                *uid = (uid_t)e;
                gu = true;
            }
        } else if (strncmp(line, "Gid:", 4) == 0) {
            unsigned r, e, s, fs;
            if (sscanf(line + 4, "%u%u%u%u", &r, &e, &s, &fs) >= 1) {
                *gid = (gid_t)e;
                gg = true;
            }
        }
        if (gu && gg)
            break;
    }
    fclose(f);
    return gu && gg;
}

static bool copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        close(in);
        return false;
    }
    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        char *p = buf;
        while (n > 0) {
            ssize_t w = write(out, p, (size_t)n);
            if (w <= 0) {
                ok = false;
                break;
            }
            p += w;
            n -= w;
        }
        if (!ok)
            break;
    }
    if (n < 0)
        ok = false;
    close(in);
    close(out);
    return ok;
}

static bool stage_payload(pid_t pid, const char *pkg, char *out, size_t cap)
{
    uid_t uid = 0;
    gid_t gid = 0;
    if (!read_ids(pid, &uid, &gid)) {
        logline("cannot read uid/gid for pid=%d", pid);
        return false;
    }

    char dir[256];
    snprintf(dir, sizeof(dir), "/data/data/%s/code_cache", pkg);
    char pkgdir[256];
    snprintf(pkgdir, sizeof(pkgdir), "/data/data/%s", pkg);
    if (mkdir(pkgdir, 0751) == 0)
        chown(pkgdir, uid, gid);
    if (mkdir(dir, 0771) == 0)
        chown(dir, uid, gid);   // a root-owned code_cache would lock the app out

    snprintf(out, cap, "%s/libcodm.so", dir);
    unlink(out);

    if (!copy_file(kPayloadSrc, out)) {
        logline("payload copy failed: %s -> %s (errno=%d)", kPayloadSrc, out, errno);
        return false;
    }
    if (chown(out, uid, gid) != 0) {
        logline("chown %s -> %u:%u failed (errno=%d)", out, uid, gid, errno);
        unlink(out);
        return false;
    }
    chmod(out, 0644);
    return true;
}

static bool inject(pid_t pid, const char *pkg)
{
    char staged[300] = {0};
    if (!stage_payload(pid, pkg, staged, sizeof(staged)))
        return false;

    RemoteProcess proc;
    if (!proc.attach(pid)) {
        logline("attach failed pid=%d errno=%d", pid, errno);
        return false;
    }

    bool ok = false;
    do {
        uint64_t pathbuf = 0;
        if (!proc.alloc(0x1000, &pathbuf)) {
            logline("remote mmap failed");
            break;
        }
        if (!proc.writeStr(pathbuf, staged)) {
            logline("write path failed");
            break;
        }

        uint64_t dlopen_addr = 0;
        if (!proc.resolve("libdl.so", "__loader_dlopen", &dlopen_addr) &&
            !proc.resolve("libdl.so", "dlopen", &dlopen_addr) &&
            !proc.resolve("libc.so", "__loader_dlopen", &dlopen_addr) &&
            !proc.resolve("libc.so", "dlopen", &dlopen_addr)) {
            logline("dlopen not resolvable");
            break;
        }

        uint64_t args[3] = { pathbuf, RTLD_NOW, 0 };
        uint64_t ret = 0;
        if (!proc.call(dlopen_addr, args, 3, &ret)) {
            logline("remote dlopen call failed");
            break;
        }
        if (!ret) {
            logline("dlopen returned null for %s", staged);
            break;
        }
        logline("injected %s pid=%d handle=0x%llx", pkg, pid, (unsigned long long)ret);
        ok = true;
    } while (0);

    proc.detach();
    return ok;
}

int main(int argc, char **argv)
{
    (void)argc;
    mkdir(CODM_STATE_DIR, 0755);

    const char *only = nullptr;
    bool once = false;
    bool wait_for_maps = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pkg") == 0 && i + 1 < argc)
            only = argv[++i];
        else if (strcmp(argv[i], "--once") == 0)
            once = true;
        else if (strcmp(argv[i], "--now") == 0)
            wait_for_maps = false;
    }

    logline("injector start (payload=%s)", kPayloadSrc);
    if (access(kPayloadSrc, R_OK) != 0) {
        logline("payload missing: %s", kPayloadSrc);
        return 1;
    }

    for (;;) {
        for (const char *pkg : kTargets) {
            if (only && strcmp(only, pkg) != 0)
                continue;
            pid_t pid = find_pid(pkg);
            if (pid <= 0)
                continue;
            if (injected_before(pid))
                continue;

            // the dynamic loader has to be up before we can hijack a call into it
            if (wait_for_maps) {
                char maps[64];
                snprintf(maps, sizeof(maps), "/proc/%d/maps", pid);
                FILE *f = fopen(maps, "r");
                bool ready = false;
                if (f) {
                    char line[512];
                    while (fgets(line, sizeof(line), f))
                        if (strstr(line, "libdl.so") || strstr(line, "linker64")) {
                            ready = true;
                            break;
                        }
                    fclose(f);
                }
                if (!ready) {
                    usleep(50 * 1000);
                    continue;
                }
            }

            logline("target up: %s pid=%d", pkg, pid);
            if (inject(pid, pkg) && once)
                return 0;
        }
        if (once)
            return 1;
        usleep(300 * 1000);
    }
}
