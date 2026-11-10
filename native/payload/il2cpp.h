// il2cpp.h — runtime reflection against libil2cpp.so from inside the game
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Vector3 { float x, y, z; };
struct Vector2 { float x, y; };

typedef void *(*t_domain_get)();
typedef void *(*t_thread_attach)(void *);
typedef void *(*t_domain_get_assemblies)(void *, size_t *);
typedef void *(*t_assembly_get_image)(void *);
typedef const char *(*t_image_get_name)(void *);
typedef size_t (*t_image_get_class_count)(void *);
typedef void *(*t_image_get_class)(void *, size_t);
typedef const char *(*t_class_get_name)(void *);
typedef const char *(*t_class_get_namespace)(void *);
typedef void *(*t_class_from_name)(void *image, const char *ns, const char *name);
typedef void *(*t_class_get_field_from_name)(void *klass, const char *name);
typedef size_t (*t_field_get_offset)(void *field);
typedef void *(*t_class_get_method_from_name)(void *klass, const char *name, int argc);
typedef void *(*t_runtime_invoke)(void *method, void *obj, void **params, void **exc);
typedef void *(*t_class_get_parent)(void *klass);
typedef size_t (*t_method_get_param_count)(void *method);

namespace il2 {

bool init();
bool ready();
void attach_thread();
void dump(const char *outPath);

void *image(const char *name);                       // "Assembly-CSharp.dll"
void *klass(const char *imageName, const char *ns, const char *name);
void *klassPath(const char *imageName, const char *qualified);  // "GameBase.Pawn"
void *field(void *klass, const char *name);
size_t fieldOffset(void *klass, const char *field);
// "m_AttackableInfo.m_Health" — sums the segment offsets, dereferencing a
// struct field through il2cpp_field_get_type when the path has dots
size_t fieldOffsetPath(void *klass, const char *path);
bool staticFieldGet(void *field, void *out);
bool instanceFieldGet(void *field, void *obj, void *out);
void *method(void *klass, const char *name, int argc);
void *invoke(void *method, void *obj, void **params);
void *staticField(void *klass, const char *field);
void *instanceField(void *obj, void *klass, const char *field);

// module base of libil2cpp.so and a direct call into a managed method by RVA
// (RVAs come from Il2CppDumper's dump.cs; they move on every game update)
uintptr_t base();
bool callRva(uint32_t rva, void *obj, uint32_t arg);

// object helpers — List<T> / arrays, stable across Unity versions
int listSize(void *list);
void *listItem(void *list, int i);

std::string className(void *klass);

} // namespace il2
