// remote.cpp — aarch64 ptrace injection primitives
#include "remote.h"

#include <elf.h>
#include <asm/ptrace.h>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

// kernel layout for NT_PRSTATUS on arm64 comes from <asm/ptrace.h>: x0..x30,
// sp, pc, pstate.
#if !defined(__aarch64__)
#error "injector host must be aarch64"
#endif

static const uint64_t kTrapLr = 0xDEADBEEF0ull;

static bool getregs(pid_t pid, user_pt_regs *r)
{
    struct iovec iov = { r, sizeof(*r) };
    return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) == 0;
}

static bool setregs(pid_t pid, user_pt_regs *r)
{
    struct iovec iov = { r, sizeof(*r) };
    return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov) == 0;
}

bool RemoteProcess::attach(pid_t pid)
{
    pid_ = pid;
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) != 0)
        return false;
    int status = 0;
    if (waitpid(pid, &status, __WALL) < 0)
        return false;
    if (!loadMaps())
        return false;

    libc_base_ = moduleBase("libc.so");
    dl_base_   = moduleBase("libdl.so");
    if (!dl_base_)
        dl_base_ = moduleBase("libc.so");   // bionic folds dl into libc on newer builds
    return dl_base_ != 0;
}

void RemoteProcess::detach()
{
    if (pid_ > 0) {
        ptrace(PTRACE_DETACH, pid_, nullptr, nullptr);
        pid_ = -1;
    }
}

bool RemoteProcess::loadMaps()
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid_);
    FILE *f = fopen(path, "re");
    if (!f)
        return false;
    maps_.clear();
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start, end, off;
        char perms[8] = {0}, dev[16] = {0};
        unsigned long inode = 0;
        char name[256] = {0};
        int n = sscanf(line, "%llx-%llx %7s %llx %15s %lu %255[^\n]",
                       &start, &end, perms, &off, dev, &inode, name);
        if (n < 6)
            continue;
        char *p = name;
        while (*p == ' ')
            p++;
        Map m{start, end, off, p};
        maps_.push_back(m);
    }
    fclose(f);
    return !maps_.empty();
}

uint64_t RemoteProcess::moduleBase(const char *lib_substr)
{
    for (const auto &m : maps_) {
        if (m.path.find(lib_substr) != std::string::npos && m.off == 0)
            return m.start;
    }
    return 0;
}

bool RemoteProcess::read(uint64_t addr, void *buf, size_t len)
{
    struct iovec local  = { buf, len };
    struct iovec remote = { (void *)addr, len };
    ssize_t n = process_vm_readv(pid_, &local, 1, &remote, 1, 0);
    if (n == (ssize_t)len)
        return true;

    // fallback: word-wise ptrace peek for the tail
    uint8_t *out = (uint8_t *)buf;
    size_t done = n > 0 ? (size_t)n : 0;
    for (size_t i = done; i < len; i += sizeof(long)) {
        errno = 0;
        long w = ptrace(PTRACE_PEEKDATA, pid_, (void *)(addr + i), nullptr);
        if (errno)
            return false;
        size_t chunk = sizeof(long);
        if (i + chunk > len)
            chunk = len - i;
        memcpy(out + i, &w, chunk);
    }
    return true;
}

bool RemoteProcess::write(uint64_t addr, const void *buf, size_t len)
{
    struct iovec local  = { (void *)buf, len };
    struct iovec remote = { (void *)addr, len };
    ssize_t n = process_vm_writev(pid_, &local, 1, &remote, 1, 0);
    if (n == (ssize_t)len)
        return true;

    const uint8_t *in = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i += sizeof(long)) {
        long w = 0;
        size_t chunk = sizeof(long);
        if (i + chunk > len)
            chunk = len - i;
        if (chunk != sizeof(long) && !read(addr + i, &w, sizeof(long)))
            return false;
        memcpy(&w, in + i, chunk);
        if (!patchWord(addr + i, (uint64_t)w))
            return false;
    }
    return true;
}

bool RemoteProcess::patchWord(uint64_t addr, uint64_t word)
{
    errno = 0;
    if (ptrace(PTRACE_POKEDATA, pid_, (void *)addr, (void *)word) != 0)
        return false;
    return true;
}

bool RemoteProcess::writeStr(uint64_t addr, const char *s)
{
    return write(addr, s, strlen(s) + 1);
}

bool RemoteProcess::alloc(size_t len, uint64_t *out)
{
    uint64_t mmap_addr = 0;
    if (!resolve("libc.so", "mmap", &mmap_addr))
        return false;
    uint64_t args[6] = {
        0, (uint64_t)len, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0
    };
    uint64_t ret = 0;
    if (!call(mmap_addr, args, 6, &ret))
        return false;
    if ((int64_t)ret < 0)
        return false;
    *out = ret;
    return true;
}

bool RemoteProcess::call(uint64_t fn, const uint64_t *args, int nargs, uint64_t *ret)
{
    user_pt_regs saved{}, r{};
    if (!getregs(pid_, &saved))
        return false;
    r = saved;

    for (int i = 0; i < nargs && i < 8; i++)
        r.regs[i] = args[i];
    r.regs[30] = kTrapLr;      // lr
    r.pc       = fn;

    if (!setregs(pid_, &r))
        return false;

    if (ptrace(PTRACE_CONT, pid_, nullptr, nullptr) != 0)
        return false;

    int status = 0;
    if (waitpid(pid_, &status, __WALL) < 0)
        return false;

    bool ok = false;
    if (WIFSTOPPED(status)) {
        user_pt_regs after{};
        if (getregs(pid_, &after) && after.pc == kTrapLr) {
            *ret = after.regs[0];
            ok = true;
        }
    }

    // swallow whatever signal stopped us; the target must not see it
    setregs(pid_, &saved);
    return ok;
}

// ---- remote ELF symbol lookup -------------------------------------------------

struct DynInfo {
    uint64_t bias = 0;
    uint64_t symtab = 0, strtab = 0, strsz = 0, gnu_hash = 0, hash = 0;
    uint64_t syment = 24;
    uint64_t filesz = 0;
    bool ok = false;
};

bool RemoteProcess::resolve(const char *lib_substr, const char *sym, uint64_t *out)
{
    // the ELF header lives in the mapping whose file offset is 0
    RemoteProcess::Map base{};
    bool found = false;
    for (const auto &m : maps_) {
        if (m.path.find(lib_substr) != std::string::npos && m.off == 0) {
            base = m;
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    // header is mapped at base.start + (file offset 0), so e_phoff indexes directly
    Elf64_Ehdr eh{};
    if (!read(base.start, &eh, sizeof(eh)))
        return false;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_machine != EM_AARCH64)
        return false;

    uint64_t first_vaddr = 0, first_off = 0;
    uint64_t dyn_vaddr = 0;
    for (int i = 0; i < eh.e_phnum && i < 64; i++) {
        Elf64_Phdr ph{};
        if (!read(base.start + eh.e_phoff + (uint64_t)i * eh.e_phentsize, &ph, sizeof(ph)))
            return false;
        if (ph.p_type == PT_LOAD && first_vaddr == 0) {
            first_vaddr = ph.p_vaddr;
            first_off   = ph.p_offset;
        }
        if (ph.p_type == PT_DYNAMIC)
            dyn_vaddr = ph.p_vaddr;
    }
    if (first_vaddr == 0 && first_off != 0)
        return false;
    const uint64_t bias = base.start - (first_vaddr - first_off);

    if (!dyn_vaddr)
        return false;

    DynInfo di{};
    di.bias = bias;
    for (int i = 0; i < 512; i++) {
        Elf64_Dyn d{};
        if (!read(bias + dyn_vaddr + (uint64_t)i * sizeof(d), &d, sizeof(d)))
            return false;
        if (d.d_tag == DT_NULL)
            break;
        switch (d.d_tag) {
        case DT_SYMTAB:   di.symtab   = d.d_un.d_ptr; break;
        case DT_STRTAB:   di.strtab   = d.d_un.d_ptr; break;
        case DT_STRSZ:    di.strsz    = d.d_un.d_val; break;
        case DT_GNU_HASH: di.gnu_hash = d.d_un.d_ptr; break;
        case DT_HASH:     di.hash     = d.d_un.d_ptr; break;
        case DT_SYMENT:   di.syment   = d.d_un.d_val; break;
        default: break;
        }
    }
    if (!di.symtab || !di.strtab)
        return false;
    if (!di.syment)
        di.syment = sizeof(Elf64_Sym);

    auto lookupGnu = [&](const char *name) -> uint64_t {
        if (!di.gnu_hash)
            return 0;
        uint32_t nbuckets = 0, symoffset = 0, bloom_size = 0, bloom_shift = 0;
        uint64_t h = bias + di.gnu_hash;
        if (!read(h, &nbuckets, 4) || !read(h + 4, &symoffset, 4) ||
            !read(h + 8, &bloom_size, 4) || !read(h + 12, &bloom_shift, 4))
            return 0;
        (void)bloom_shift;

        uint32_t hash = 5381;
        for (const uint8_t *p = (const uint8_t *)name; *p; p++)
            hash = hash * 33 + *p;

        uint64_t buckets = h + 16 + (uint64_t)bloom_size * 8;
        uint32_t idx = 0;
        if (!read(buckets + (uint64_t)(hash % nbuckets) * 4, &idx, 4))
            return 0;
        if (idx < symoffset)
            return 0;

        uint64_t chain = buckets + (uint64_t)nbuckets * 4;
        for (uint32_t i = idx; i < idx + 4096; i++) {
            uint32_t ch = 0;
            if (!read(chain + (uint64_t)(i - symoffset) * 4, &ch, 4))
                return 0;
            if ((ch | 1) == (hash | 1)) {
                Elf64_Sym s{};
                if (!read(bias + di.symtab + (uint64_t)i * di.syment, &s, sizeof(s)))
                    return 0;
                char nbuf[256] = {0};
                if (read(bias + di.strtab + s.st_name, nbuf, sizeof(nbuf) - 1) &&
                    strcmp(nbuf, name) == 0)
                    return bias + s.st_value;
            }
            if (ch & 1)
                break;
        }
        return 0;
    };

    uint64_t addr = lookupGnu(sym);
    if (addr) {
        *out = addr;
        return true;
    }
    return false;
}
