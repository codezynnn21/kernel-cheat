// il2cpp.cpp
#include "il2cpp.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace il2 {

typedef void *(*t_iter)(void *, void **);
typedef const char *(*t_field_name)(void *);
typedef const char *(*t_method_name)(void *);
typedef void (*t_field_static_get)(void *, void *);
typedef void (*t_field_get)(void *, void *, void *);
typedef uint32_t (*t_field_flags)(void *);
typedef void *(*t_field_get_type)(void *);
typedef void *(*t_class_from_type)(void *);

namespace {

void *g_handle = nullptr;
void *g_domain = nullptr;

t_domain_get                 p_domain_get;
t_thread_attach              p_thread_attach;
t_domain_get_assemblies      p_domain_get_assemblies;
t_assembly_get_image         p_assembly_get_image;
t_image_get_name             p_image_get_name;
t_image_get_class_count      p_image_get_class_count;
t_image_get_class            p_image_get_class;
t_class_get_name             p_class_get_name;
t_class_get_namespace        p_class_get_namespace;
t_class_from_name            p_class_from_name;
t_class_get_field_from_name  p_class_get_field_from_name;
t_field_get_offset           p_field_get_offset;
t_class_get_method_from_name p_class_get_method_from_name;
t_runtime_invoke             p_runtime_invoke;
t_class_get_parent           p_class_get_parent;
t_method_get_param_count     p_method_get_param_count;

t_iter              p_class_get_fields;
t_iter              p_class_get_methods;
t_field_name        p_field_get_name;
t_method_name       p_method_get_name;
t_field_static_get  p_field_static_get_value;
t_field_get         p_field_get_value;
t_field_flags       p_field_get_flags;
t_field_get_type    p_field_get_type;
t_class_from_type   p_class_from_type;

void *sym(const char *n) { return dlsym(g_handle, n); }

bool resolve_all()
{
    p_domain_get             = (t_domain_get)sym("il2cpp_domain_get");
    p_thread_attach          = (t_thread_attach)sym("il2cpp_thread_attach");
    p_domain_get_assemblies  = (t_domain_get_assemblies)sym("il2cpp_domain_get_assemblies");
    p_assembly_get_image     = (t_assembly_get_image)sym("il2cpp_assembly_get_image");
    p_image_get_name         = (t_image_get_name)sym("il2cpp_image_get_name");
    p_image_get_class_count  = (t_image_get_class_count)sym("il2cpp_image_get_class_count");
    p_image_get_class        = (t_image_get_class)sym("il2cpp_image_get_class");
    p_class_get_name         = (t_class_get_name)sym("il2cpp_class_get_name");
    p_class_get_namespace    = (t_class_get_namespace)sym("il2cpp_class_get_namespace");
    p_class_from_name        = (t_class_from_name)sym("il2cpp_class_from_name");
    p_class_get_field_from_name  = (t_class_get_field_from_name)sym("il2cpp_class_get_field_from_name");
    p_field_get_offset       = (t_field_get_offset)sym("il2cpp_field_get_offset");
    p_class_get_method_from_name = (t_class_get_method_from_name)sym("il2cpp_class_get_method_from_name");
    p_runtime_invoke         = (t_runtime_invoke)sym("il2cpp_runtime_invoke");
    p_class_get_parent       = (t_class_get_parent)sym("il2cpp_class_get_parent");
    p_method_get_param_count = (t_method_get_param_count)sym("il2cpp_method_get_param_count");

    p_class_get_fields       = (t_iter)sym("il2cpp_class_get_fields");
    p_class_get_methods      = (t_iter)sym("il2cpp_class_get_methods");
    p_field_get_name         = (t_field_name)sym("il2cpp_field_get_name");
    p_method_get_name        = (t_method_name)sym("il2cpp_method_get_name");
    p_field_static_get_value = (t_field_static_get)sym("il2cpp_field_static_get_value");
    p_field_get_value        = (t_field_get)sym("il2cpp_field_get_value");
    p_field_get_flags        = (t_field_flags)sym("il2cpp_field_get_flags");
    p_field_get_type         = (t_field_get_type)sym("il2cpp_field_get_type");
    p_class_from_type        = (t_class_from_type)sym("il2cpp_class_from_type");

    // required for the ESP path; the dumper-only symbols stay optional so an
    // older il2cpp export table still injects (menu just cannot dump)
    return p_domain_get && p_thread_attach && p_domain_get_assemblies &&
           p_assembly_get_image && p_class_from_name && p_class_get_field_from_name &&
           p_field_get_offset && p_class_get_method_from_name && p_runtime_invoke;
}

} // namespace

bool init()
{
    if (g_handle)
        return true;
    g_handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_handle)
        return false;
    if (!resolve_all())
        return false;
    g_domain = p_domain_get();
    if (!g_domain)
        return false;
    p_thread_attach(g_domain);
    return true;
}

bool ready() { return g_handle != nullptr; }

// every thread that touches managed objects must be attached first; the render
// thread that runs the overlay is native, so it needs its own attach
void attach_thread()
{
    if (g_domain && p_thread_attach)
        p_thread_attach(g_domain);
}

void *image(const char *name)
{
    if (!g_domain)
        return nullptr;
    size_t count = 0;
    void **asmblies = (void **)p_domain_get_assemblies(g_domain, &count);
    for (size_t i = 0; i < count; i++) {
        void *img = p_assembly_get_image(asmblies[i]);
        const char *n = p_image_get_name(img);
        if (n && strcmp(n, name) == 0)
            return img;
    }
    return nullptr;
}

void *klass(const char *imageName, const char *ns, const char *name)
{
    void *img = image(imageName);
    if (!img)
        return nullptr;
    return p_class_from_name(img, ns, name);
}

size_t fieldOffset(void *klass_, const char *field)
{
    if (!klass_)
        return 0;
    void *f = p_class_get_field_from_name(klass_, field);
    return f ? p_field_get_offset(f) : 0;
}

void *klassPath(const char *imageName, const char *qualified)
{
    if (!qualified || !*qualified)
        return nullptr;
    const char *dot = strrchr(qualified, '.');
    if (!dot)
        return klass(imageName, "", qualified);
    char ns[160];
    size_t len = (size_t)(dot - qualified);
    if (len >= sizeof(ns))
        return nullptr;
    memcpy(ns, qualified, len);
    ns[len] = 0;
    return klass(imageName, ns, dot + 1);
}

void *field(void *klass_, const char *name)
{
    if (!klass_ || !name || !*name)
        return nullptr;
    return p_class_get_field_from_name(klass_, name);
}

size_t fieldOffsetPath(void *klass_, const char *path)
{
    if (!klass_ || !path || !*path)
        return 0;
    size_t total = 0;
    void *k = klass_;
    const char *seg = path;
    while (seg && *seg) {
        const char *dot = strchr(seg, '.');
        size_t len = dot ? (size_t)(dot - seg) : strlen(seg);
        if (len == 0 || len >= 96)
            return 0;
        char name[96];
        memcpy(name, seg, len);
        name[len] = 0;
        void *f = p_class_get_field_from_name(k, name);
        if (!f)
            return 0;
        total += p_field_get_offset(f);
        if (!dot)
            break;
        if (!p_field_get_type || !p_class_from_type)
            return 0;
        void *type = p_field_get_type(f);
        k = type ? p_class_from_type(type) : nullptr;
        if (!k)
            return 0;
        seg = dot + 1;
    }
    return total;
}

bool staticFieldGet(void *field_, void *out)
{
    if (!field_ || !out || !p_field_static_get_value)
        return false;
    p_field_static_get_value(field_, out);
    return true;
}

bool instanceFieldGet(void *field_, void *obj, void *out)
{
    if (!field_ || !obj || !out || !p_field_get_value)
        return false;
    p_field_get_value(obj, field_, out);
    return true;
}

void *method(void *klass_, const char *name, int argc)
{
    return klass_ ? p_class_get_method_from_name(klass_, name, argc) : nullptr;
}

void *invoke(void *method_, void *obj, void **params)
{
    if (!method_)
        return nullptr;
    void *exc = nullptr;
    return p_runtime_invoke(method_, obj, params, &exc);
}

int listSize(void *list)
{
    return list ? *(int *)((uint8_t *)list + 0x18) : 0;
}

void *listItem(void *list, int i)
{
    if (!list || i < 0 || i >= listSize(list))
        return nullptr;
    void *items = *(void **)((uint8_t *)list + 0x10);
    return items ? *(void **)((uint8_t *)items + 0x20 + (size_t)i * sizeof(void *)) : nullptr;
}

std::string className(void *klass_)
{
    if (!klass_)
        return "<null>";
    const char *ns = p_class_get_namespace ? p_class_get_namespace(klass_) : "";
    const char *n  = p_class_get_name ? p_class_get_name(klass_) : "";
    std::string out;
    if (ns && *ns) {
        out += ns;
        out += "::";
    }
    out += (n && *n) ? n : "<anon>";
    return out;
}

void *staticField(void *klass_, const char *field)
{
    if (!klass_ || !p_field_static_get_value)
        return nullptr;
    void *f = p_class_get_field_from_name(klass_, field);
    if (!f)
        return nullptr;
    void *value = nullptr;
    p_field_static_get_value(f, &value);
    return value;
}

void *instanceField(void *obj, void *klass_, const char *field)
{
    if (!obj || !klass_ || !p_field_get_value)
        return nullptr;
    void *f = p_class_get_field_from_name(klass_, field);
    if (!f)
        return nullptr;
    void *value = nullptr;
    p_field_get_value(obj, f, &value);
    return value;
}

uintptr_t base()
{
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f)
        return 0;
    char line[512];
    uintptr_t out = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "libil2cpp.so"))
            continue;
        unsigned long long start = 0, end = 0, off = 0;
        if (sscanf(line, "%llx-%llx %*s %llx", &start, &end, &off) == 3 && off == 0) {
            out = (uintptr_t)start;
            break;
        }
    }
    fclose(f);
    return out;
}

bool callRva(uint32_t rva, void *obj, uint32_t arg)
{
    uintptr_t b = base();
    if (!b || !rva || !obj)
        return false;
    auto fn = (void (*)(void *, uint32_t))(b + rva);
    fn(obj, arg);
    return true;
}

void dump(const char *outPath)
{
    if (!g_domain)
        return;
    FILE *f = fopen(outPath, "w");
    if (!f)
        return;
    time_t now = time(nullptr);
    fprintf(f, "# il2cpp dump %s", ctime(&now));
    fprintf(f, "# format: IMAGE / CLASS / FIELD+offset / METHOD+argc\n\n");

    if (!p_image_get_class || !p_image_get_class_count) {
        fprintf(f, "# il2cpp_image_get_class[_count] missing from this libil2cpp\n"
                   "# dump the classes with Il2CppDumper against global-metadata.dat instead\n");
        fclose(f);
        return;
    }

    size_t count = 0;
    void **asmblies = (void **)p_domain_get_assemblies(g_domain, &count);
    for (size_t i = 0; i < count; i++) {
        void *img = p_assembly_get_image(asmblies[i]);
        if (!img)
            continue;
        const char *imgName = p_image_get_name(img);
        size_t classes = p_image_get_class_count ? p_image_get_class_count(img) : 0;
        fprintf(f, "IMAGE %s classes=%zu\n", imgName ? imgName : "?", classes);

        for (size_t c = 0; c < classes; c++) {
            void *k = p_image_get_class(img, c);
            if (!k)
                continue;
            fprintf(f, "  CLASS %s\n", className(k).c_str());
            void *parent = p_class_get_parent ? p_class_get_parent(k) : nullptr;
            if (parent)
                fprintf(f, "    PARENT %s\n", className(parent).c_str());

            void *iter = nullptr;
            if (p_class_get_fields && p_field_get_name) {
                while (void *fld = p_class_get_fields(k, &iter)) {
                    const char *fn = p_field_get_name(fld);
                    uint32_t flags = p_field_get_flags ? p_field_get_flags(fld) : 0;
                    // FIELD_ATTRIBUTE_STATIC = 0x0010; statics carry no instance offset
                    if (flags & 0x0010)
                        fprintf(f, "    FIELD %s STATIC\n", fn ? fn : "?");
                    else
                        fprintf(f, "    FIELD %s 0x%zx\n", fn ? fn : "?",
                                (size_t)p_field_get_offset(fld));
                }
            }
            iter = nullptr;
            if (p_class_get_methods && p_method_get_name) {
                while (void *m = p_class_get_methods(k, &iter)) {
                    const char *mn = p_method_get_name(m);
                    int argc = p_method_get_param_count ? (int)p_method_get_param_count(m) : -1;
                    fprintf(f, "    METHOD %s argc=%d\n", mn ? mn : "?", argc);
                }
            }
        }
        fflush(f);
    }
    fclose(f);
}

} // namespace il2
