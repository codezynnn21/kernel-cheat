// entry.cpp — payload: eglSwapBuffers hook, ImGui GLES3 overlay, touch input, menu
#include <android/input.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <shadowhook.h>
#include <time.h>
#include <unistd.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include "common.h"
#include "esp.h"

namespace {

CheatConfig g_cfg;
bool        g_imgui = false;
EGLDisplay  g_dpy = nullptr;
EGLSurface  g_surf = nullptr;
int         g_w = 0, g_h = 0;
double      g_lastTime = 0.0;
bool        g_menu = true;
bool        g_dumpRequested = false;
double      g_lastResolve = 0.0;
pthread_mutex_t g_ioMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_initMutex = PTHREAD_MUTEX_INITIALIZER;

typedef EGLBoolean (*swap_fn)(EGLDisplay, EGLSurface);
swap_fn g_origSwap = nullptr;

// Unity's EGL wrapper calls this instead of eglSwapBuffers on many drivers
typedef EGLBoolean (*swap_damage_fn)(EGLDisplay, EGLSurface, EGLint *, EGLint);
swap_damage_fn g_origSwapDamage = nullptr;

// AInputQueue_getEvent(AInputQueue *, AInputEvent **) -> int
typedef int (*getevent_fn)(void *, AInputEvent **);
getevent_fn g_origGetEvent = nullptr;

double now()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void draw_menu()
{
    if (!g_menu)
        return;

    ImGui::SetNextWindowSize(ImVec2(430, 680), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("codm", &g_menu, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Text("targets: %d", esp::lastTargets());
    ImGui::SameLine();
    ImGui::TextDisabled("(3-finger tap = menu)");
    ImGui::Separator();

    ImGui::Checkbox("ESP", (bool *)&g_cfg.esp.enable);
    ImGui::SameLine();
    ImGui::Checkbox("Box", (bool *)&g_cfg.esp.box);
    ImGui::SameLine();
    ImGui::Checkbox("Health", (bool *)&g_cfg.esp.health);
    ImGui::Checkbox("Distance", (bool *)&g_cfg.esp.distance);
    ImGui::SameLine();
    ImGui::Checkbox("Enemies only", (bool *)&g_cfg.esp.enemy_only);
    ImGui::SliderInt("Max distance", &g_cfg.esp.max_dist, 25, 800);
    ImGui::SliderFloat("Head offset", &g_cfg.esp.head_offset, 1.2f, 2.2f);
    ImGui::ColorEdit4("Box color", g_cfg.esp.box_color);

    ImGui::Separator();
    ImGui::Text("il2cpp mapping (shipped resolved, see codm.cfg)");

    ImGui::InputText("image", g_cfg.esp.ent_image, sizeof(g_cfg.esp.ent_image));
    ImGui::InputText("game class", g_cfg.esp.game_class, sizeof(g_cfg.esp.game_class));
    ImGui::InputText("game static", g_cfg.esp.game_field, sizeof(g_cfg.esp.game_field));
    ImGui::InputText("root class", g_cfg.esp.root_class, sizeof(g_cfg.esp.root_class));
    ImGui::InputText("class", g_cfg.esp.ent_class, sizeof(g_cfg.esp.ent_class));
    ImGui::InputText("list field", g_cfg.esp.ent_list, sizeof(g_cfg.esp.ent_list));
    ImGui::InputText("pos mode", g_cfg.esp.ent_pos_mode, sizeof(g_cfg.esp.ent_pos_mode));
    ImGui::InputText("pos field", g_cfg.esp.ent_pos, sizeof(g_cfg.esp.ent_pos));
    ImGui::InputText("hp field", g_cfg.esp.ent_hp, sizeof(g_cfg.esp.ent_hp));
    ImGui::InputText("hp max", g_cfg.esp.ent_hpmax, sizeof(g_cfg.esp.ent_hpmax));
    ImGui::InputText("team field", g_cfg.esp.ent_team, sizeof(g_cfg.esp.ent_team));
    ImGui::InputText("alive field", g_cfg.esp.ent_alive, sizeof(g_cfg.esp.ent_alive));

    if (ImGui::Button("Apply", ImVec2(110, 0)))
        esp::init(g_cfg);
    ImGui::SameLine();
    if (ImGui::Button("Reload cfg", ImVec2(110, 0))) {
        config_load(&g_cfg);
        esp::init(g_cfg);
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump il2cpp", ImVec2(120, 0)))
        g_dumpRequested = true;

    if (ImGui::CollapsingHeader("Skin")) {
        ImGui::Checkbox("Draw skin id", (bool *)&g_cfg.esp.skin_draw);
        ImGui::InputText("skin field", g_cfg.esp.ent_skin, sizeof(g_cfg.esp.ent_skin));
        ImGui::InputText("skin offset", g_cfg.esp.ent_skin_off, sizeof(g_cfg.esp.ent_skin_off),
                         ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::InputText("filter (ids, comma)", g_cfg.esp.skin_filter,
                         sizeof(g_cfg.esp.skin_filter));
        ImGui::Checkbox("Set skin", (bool *)&g_cfg.esp.skin_set);
        ImGui::InputInt("skin id", &g_cfg.esp.skin_set_id);
        if (ImGui::Button("Apply skin mapping"))
            esp::init(g_cfg);
    }

    ImGui::End();
}

void render_frame()
{
    if (!g_imgui || !g_dpy || !g_surf)
        return;

    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &g_w);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &g_h);
    if (g_w <= 0 || g_h <= 0)
        return;

    // input events arrive on the UI thread, frames on the GL thread; ImGui's
    // event queue is not internally locked
    pthread_mutex_lock(&g_ioMutex);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_w, (float)g_h);
    double t = now();
    io.DeltaTime = g_lastTime > 0.0 ? (float)(t - g_lastTime) : 1.0f / 60.0f;
    g_lastTime = t;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    // Assembly-CSharp can finish loading long after libil2cpp appears; retry the
    // mapping on the render thread so the ESP comes alive without a manual Apply.
    if (!esp::ready() && t - g_lastResolve > 5.0) {
        g_lastResolve = t;
        esp::init(g_cfg);
    }

    esp::render(ImGui::GetBackgroundDrawList(), (float)g_w, (float)g_h, g_cfg);
    draw_menu();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    pthread_mutex_unlock(&g_ioMutex);

    if (g_dumpRequested) {
        g_dumpRequested = false;
        char path[300];
        if (codm_data_path("codm_dump.txt", path, sizeof(path))) {
            il2::dump(path);
            codm_log("dumped il2cpp -> %s", path);
        } else {
            il2::dump("/data/local/tmp/codm_dump.txt");
            codm_log("dumped il2cpp -> /data/local/tmp/codm_dump.txt");
        }
    }
}

void ensure_imgui(EGLDisplay dpy, EGLSurface surf)
{
    pthread_mutex_lock(&g_initMutex);
    if (g_imgui) {
        pthread_mutex_unlock(&g_initMutex);
        return;
    }
    g_dpy = dpy;
    g_surf = surf;
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig fc;
    fc.SizePixels = 28.0f;          // readable on a 1080p+ phone panel
    io.Fonts->AddFontDefault(&fc);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    il2::attach_thread();           // render thread is native, not managed
    g_imgui = true;
    pthread_mutex_unlock(&g_initMutex);
    codm_log("imgui up (gles3)");
}

EGLBoolean hook_swap(EGLDisplay dpy, EGLSurface surf)
{
    ensure_imgui(dpy, surf);
    render_frame();
    return g_origSwap(dpy, surf);
}

EGLBoolean hook_swap_damage(EGLDisplay dpy, EGLSurface surf, EGLint *rects, EGLint n)
{
    ensure_imgui(dpy, surf);
    render_frame();
    return g_origSwapDamage(dpy, surf, rects, n);
}

// Unity polls the input queue; hooking getEvent sees every touch before the game does.
int hook_get_event(void *queue, AInputEvent **outEvent)
{
    if (!g_imgui || !outEvent)
        return g_origGetEvent(queue, outEvent);

    int rc = g_origGetEvent(queue, outEvent);
    if (rc != 0 || !*outEvent)
        return rc;

    AInputEvent *ev = *outEvent;
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION)
        return rc;

    int32_t action = AMotionEvent_getAction(ev);
    int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    int32_t pointers = (int32_t)AMotionEvent_getPointerCount(ev);
    float x = AMotionEvent_getX(ev, 0);
    float y = AMotionEvent_getY(ev, 0);

    if (masked == AMOTION_EVENT_ACTION_POINTER_DOWN && pointers == 3) {
        g_menu = !g_menu;
        codm_log("menu %s", g_menu ? "open" : "closed");
    }

    pthread_mutex_lock(&g_ioMutex);
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);

    bool down = masked == AMOTION_EVENT_ACTION_DOWN ||
                masked == AMOTION_EVENT_ACTION_MOVE ||
                masked == AMOTION_EVENT_ACTION_POINTER_DOWN;
    if (masked == AMOTION_EVENT_ACTION_UP || masked == AMOTION_EVENT_ACTION_CANCEL)
        down = false;
    io.AddMouseButtonEvent(0, down);
    const bool captured = g_menu && io.WantCaptureMouse;
    pthread_mutex_unlock(&g_ioMutex);

    // while the menu is open the game must not also receive the touch
    if (captured) {
        *outEvent = nullptr;
        return -1;
    }
    return rc;
}

void *boot(void *)
{
    char pkg[256] = {0};
    if (!codm_self_pkg(pkg, sizeof(pkg)) || !is_target_package(pkg)) {
        codm_log("not a target process: %s", pkg);
        return nullptr;
    }
    codm_log("payload attached to %s", pkg);

    for (int i = 0; i < 900; i++) {
        if (il2::init())
            break;
        usleep(100 * 1000);
    }
    if (!il2::ready()) {
        codm_log("libil2cpp never appeared — bailing");
        return nullptr;
    }

    config_load(&g_cfg);
    g_menu = g_cfg.esp.menu != 0;
    esp::init(g_cfg);

    int rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    if (rc != SHADOWHOOK_ERRNO_OK && rc != SHADOWHOOK_ERRNO_PENDING) {
        codm_log("shadowhook init failed: %d (%s)", rc, shadowhook_to_errmsg(rc));
        return nullptr;
    }

    void *stub = shadowhook_hook_sym_name("libEGL.so", "eglSwapBuffers",
                                          (void *)hook_swap, (void **)&g_origSwap);
    if (!stub || !g_origSwap) {
        int err = shadowhook_get_errno();
        codm_log("eglSwapBuffers hook failed: %d (%s)", err, shadowhook_to_errmsg(err));
        return nullptr;
    }

    stub = shadowhook_hook_sym_name("libEGL.so", "eglSwapBuffersWithDamageKHR",
                                    (void *)hook_swap_damage, (void **)&g_origSwapDamage);
    if (!stub || !g_origSwapDamage)
        codm_log("eglSwapBuffersWithDamageKHR hook failed: %d (plain swap still hooked)",
                 shadowhook_get_errno());

    stub = shadowhook_hook_sym_name("libandroid.so", "AInputQueue_getEvent",
                                    (void *)hook_get_event, (void **)&g_origGetEvent);
    if (!stub || !g_origGetEvent)
        codm_log("input hook failed: %d (overlay renders, touch dead)",
                 shadowhook_get_errno());

    codm_log("payload armed");
    return nullptr;
}

} // namespace

__attribute__((constructor)) static void payload_main()
{
    pthread_t th;
    pthread_create(&th, nullptr, boot, nullptr);
    pthread_detach(th);
}
