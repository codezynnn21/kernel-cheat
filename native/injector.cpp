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
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>

// shipped inside the module; root can read it
static const char *kPayloadSrc = "/data/adb/modules/codm_overlay/assets/libcodm.so";
static const char *kLog        = "/data/adb/codm/injector.log";
static const char *kPidFile    = "/data/adb/codm/injector.pid";

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

// A daemon that ptrace-attaches to a game is worth exactly as much on `ps` as
// its name says. comm + argv[0] get rewritten; setsid drops the controlling tty.
static void disguise(int argc, char **argv, const char *name)
{
    setsid();
    prctl(PR_SET_NAME, name, 0UL, 0UL, 0UL);
    if (argc > 0 && argv && argv[0]) {
        size_t cap = strlen(argv[0]);
        size_t n = strlen(name);
        if (n > cap)
            n = cap;
        memset(argv[0], 0, cap);
        memcpy(argv[0], name, n);
    }
}

// CODM spawns helper processes (`pkg:push`, remote config, crash reporter).
// The engine normally lives in the main process, but on some builds Unity ends
// up in a child, so match the package and every `pkg:*` subprocess.
static int find_pids(const char *pkg, pid_t *out, int max)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    size_t plen = strlen(pkg);
    int n = 0;
    dirent *de;
    while ((de = readdir(d)) && n < max) {
        char *end = nullptr;
        long pid = strtol(de->d_name, &end, 10);
        if (!end || *end || pid <= 0)
            continue;
        char path[64], cmd[256] = {0};
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t r = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (r <= 0)
            continue;
        cmd[r] = 0;
        if (strncmp(cmd, pkg, plen) == 0 && (cmd[plen] == 0 || cmd[plen] == ':'))
            out[n++] = (pid_t)pid;
    }
    closedir(d);
    return n;
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

    // the app uid cannot traverse /data/adb, so drop a copy of the cfg next to
    // the payload; the payload reads code_cache/codm.cfg and the original both
    char cfgdst[300];
    snprintf(cfgdst, sizeof(cfgdst), "%s/codm.cfg", dir);
    if (copy_file(CODM_CFG_PATH, cfgdst)) {
        chown(cfgdst, uid, gid);
        chmod(cfgdst, 0600);
    } else {
        logline("cfg mirror %s failed (payload keeps defaults)", CODM_CFG_PATH);
    }
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
    mkdir(CODM_STATE_DIR, 0755);

    // single instance: codm.sh restarts us while the Magisk watchdog may also
    // respawn. whoever gets the lock writes the pidfile; the loser exits before
    // it can fight over a target.
    int lockfd = open(CODM_STATE_DIR "/injector.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockfd >= 0) {
        struct flock fl{};
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        if (fcntl(lockfd, F_SETLK, &fl) != 0)
            return 0;
    }

    // written by the lock holder only, atomically, so status/stop always point
    // at a live daemon even after the comm/argv disguise lands
    {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s.tmp", kPidFile);
        FILE *pf = fopen(tmp, "w");
        if (pf) {
            fprintf(pf, "%d\n", (int)getpid());
            fclose(pf);
            rename(tmp, kPidFile);
        }
    }

    // stealth knobs that belong to the root side: process identity + silence
    CfgFile boot_cfg(CODM_CFG_PATH);
    if (boot_cfg.geti("stealth.quiet", 0))
        codm_set_quiet(true);
    if (boot_cfg.geti("stealth.enable", 1) && boot_cfg.geti("stealth.hide_proc", 1)) {
        std::string name = boot_cfg.gets("stealth.proc_name", "kworker/u16:2");
        disguise(argc, argv, name.c_str());
    }

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
            pid_t pids[8];
            int np = find_pids(pkg, pids, 8);
            for (int i = 0; i < np; i++) {
                pid_t pid = pids[i];
                if (injected_before(pid))
                    continue;

                // the dynamic loader has to be up before we can hijack a call into it.
                // the payload now waits for libil2cpp indefinitely, so injecting
                // early costs one sleeping thread and nothing else
                if (wait_for_maps) {
                    char maps[64];
                    snprintf(maps, sizeof(maps), "/proc/%d/maps", pid);
                    FILE *f = fopen(maps, "r");
                    bool ready = false;
                    if (f) {
                        char line[512];
                        while (fgets(line, sizeof(line), f))
                            if (strstr(line, "libdl.so") || strstr(line, "linker64") ||
                                strstr(line, "libil2cpp.so")) {
                                ready = true;
                                break;
                            }
                        fclose(f);
                    }
                    if (!ready)
                        continue;
                }

                logline("target up: %s pid=%d", pkg, pid);
                if (inject(pid, pkg) && once)
                    return 0;
            }
        }
        if (once)
            return 1;
        usleep(300 * 1000);
    }
}
