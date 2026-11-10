// remote.h — ptrace remote process control for aarch64
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

class RemoteProcess {
public:
    bool attach(pid_t pid);
    void detach();

    pid_t pid() const { return pid_; }

    bool read(uint64_t addr, void *buf, size_t len);
    bool write(uint64_t addr, const void *buf, size_t len);
    bool writeStr(uint64_t addr, const char *s);

    // runtime address of a symbol inside an already-mapped library
    bool resolve(const char *lib_substr, const char *sym, uint64_t *out);
    uint64_t moduleBase(const char *lib_substr);

    // call a function in the target; returns x0
    bool call(uint64_t fn, const uint64_t *args, int nargs, uint64_t *ret);

    // mmap a scratch region in the target
    bool alloc(size_t len, uint64_t *out);

private:
    struct Map { uint64_t start, end, off; std::string path; };
    bool loadMaps();
    bool patchWord(uint64_t addr, uint64_t word);

    pid_t pid_ = -1;
    std::vector<Map> maps_;
    uint64_t libc_base_ = 0;
    uint64_t dl_base_ = 0;
};
