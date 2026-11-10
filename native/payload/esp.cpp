// esp.cpp — camera projection + pawn walk, all through the il2cpp API
//
// Chain resolved from dump(GR .59).cs:
//   GameEngine.GamePlay.s_Game            (static BaseGame)
//     BaseGame.AllPawns                   (List<Pawn> subclass @ 0x160)
//       GameBase.Pawn.m_Transform         (0x20, inherited from GameEngine.BaseObject)
//       GameBase.Pawn.m_AttackableInfo.m_Health / .m_MaxHealth / .m_Camp
//       GameBase.Pawn.m_IsAlive           (0x550)
#include "esp.h"
#include "imgui.h"

namespace esp {
namespace {

void *camKlass = nullptr;
void *camMain = nullptr;
void *camW2S = nullptr;
void *camTransform = nullptr;
void *trKlass = nullptr;
void *trPos = nullptr;

void *gameKlass = nullptr;
void *gameField = nullptr;      // FieldInfo: GamePlay.s_Game
void *rootKlass = nullptr;
void *listField = nullptr;      // FieldInfo: BaseGame.AllPawns
void *entKlass = nullptr;

size_t offPos = 0, offHp = 0, offHpMax = 0, offTeam = 0, offSkin = 0, offAlive = 0;
bool posIsTransform = true;
bool g_resolved = false;
int g_last = 0;
int g_localCamp = -1;
std::vector<int> g_skinFilter;

bool skinAllowed(int id)
{
    if (g_skinFilter.empty())
        return true;
    for (int v : g_skinFilter)
        if (v == id)
            return true;
    return false;
}

void parseSkinFilter(const char *s)
{
    g_skinFilter.clear();
    while (s && *s) {
        char *end = nullptr;
        long v = strtol(s, &end, 0);
        if (end == s)
            break;
        g_skinFilter.push_back((int)v);
        s = end;
        while (*s == ',' || *s == ' ' || *s == '\t')
            s++;
    }
}

float norm(Vector3 v)
{
    return __builtin_sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

// il2cpp_runtime_invoke returns a boxed value type: Il2CppObject (0x10) then payload
Vector3 invokeVec3(void *method_, void *obj, void **params)
{
    Vector3 out{0, 0, 0};
    void *res = il2::invoke(method_, obj, params);
    if (res)
        memcpy(&out, (uint8_t *)res + 0x10, sizeof(Vector3));
    return out;
}

void *camera()
{
    return camMain ? il2::invoke(camMain, nullptr, nullptr) : nullptr;
}

Vector3 cameraPos(void *cam)
{
    if (!cam || !camTransform || !trPos)
        return {0, 0, 0};
    void *tr = il2::invoke(camTransform, cam, nullptr);
    return tr ? invokeVec3(trPos, tr, nullptr) : Vector3{0, 0, 0};
}

bool project(void *cam, const Vector3 &world, Vector3 *out)
{
    void *args[1] = { (void *)&world };
    *out = invokeVec3(camW2S, cam, args);
    return out->z > 0.0f;
}

// the live BaseGame instance; s_Game is re-read every frame because it is
// nulled between matches
void *game()
{
    void *g = nullptr;
    return (gameField && il2::staticFieldGet(gameField, &g)) ? g : nullptr;
}

Vector3 pawnPos(void *ent)
{
    if (!posIsTransform) {
        Vector3 v{};
        memcpy(&v, (uint8_t *)ent + offPos, sizeof(Vector3));
        return v;
    }
    void *tr = *(void **)((uint8_t *)ent + offPos);
    return (tr && trPos) ? invokeVec3(trPos, tr, nullptr) : Vector3{0, 0, 0};
}

bool pawnAlive(void *ent)
{
    return offAlive ? *(bool *)((uint8_t *)ent + offAlive) : true;
}

int pawnCamp(void *ent)
{
    return offTeam ? *(int *)((uint8_t *)ent + offTeam) : 0;
}

} // namespace

void init(const CheatConfig &cfg)
{
    il2::attach_thread();

    camKlass = il2::klassPath("UnityEngine.CoreModule.dll", "UnityEngine.Camera");
    if (!camKlass)
        camKlass = il2::klassPath("UnityEngine.dll", "UnityEngine.Camera");
    if (!camKlass) {
        codm_log("esp: UnityEngine.Camera not found — libil2cpp not up yet");
        return;
    }
    camMain      = il2::method(camKlass, "get_main", 0);
    camW2S       = il2::method(camKlass, "WorldToScreenPoint", 1);
    camTransform = il2::method(camKlass, "get_transform", 0);

    trKlass = il2::klassPath("UnityEngine.CoreModule.dll", "UnityEngine.Transform");
    if (trKlass)
        trPos = il2::method(trKlass, "get_position", 0);

    gameKlass = il2::klassPath(cfg.esp.ent_image, cfg.esp.game_class);
    if (gameKlass)
        gameField = il2::field(gameKlass, cfg.esp.game_field);
    rootKlass = il2::klassPath(cfg.esp.ent_image, cfg.esp.root_class);
    if (rootKlass)
        listField = il2::field(rootKlass, cfg.esp.ent_list);

    entKlass = il2::klassPath(cfg.esp.ent_image, cfg.esp.ent_class);
    if (!entKlass)
        codm_log("esp: class '%s' missing — check the mapping in codm.cfg", cfg.esp.ent_class);

    posIsTransform = strcmp(cfg.esp.ent_pos_mode, "transform") == 0;
    offPos   = il2::fieldOffsetPath(entKlass, cfg.esp.ent_pos);
    offHp    = il2::fieldOffsetPath(entKlass, cfg.esp.ent_hp);
    offHpMax = il2::fieldOffsetPath(entKlass, cfg.esp.ent_hpmax);
    offTeam  = il2::fieldOffsetPath(entKlass, cfg.esp.ent_team);
    offAlive = il2::fieldOffsetPath(entKlass, cfg.esp.ent_alive);

    parseSkinFilter(cfg.esp.skin_filter);
    offSkin = cfg.esp.ent_skin_off[0]
                  ? (size_t)strtoul(cfg.esp.ent_skin_off, nullptr, 0)
                  : il2::fieldOffsetPath(entKlass, cfg.esp.ent_skin);

    g_resolved = gameKlass && gameField && listField && entKlass && offPos && camW2S;

    codm_log("esp: game=%s.%s list=%s.%s pawn=%s pos=0x%zx(%s) hp=0x%zx max=0x%zx "
             "camp=0x%zx alive=0x%zx skin=0x%zx resolved=%d",
             cfg.esp.game_class, cfg.esp.game_field, cfg.esp.root_class, cfg.esp.ent_list,
             cfg.esp.ent_class, offPos, cfg.esp.ent_pos_mode, offHp, offHpMax, offTeam,
             offAlive, offSkin, (int)g_resolved);
}

void render(ImDrawList *dl, float sw, float sh, const CheatConfig &cfg)
{
    (void)sw;   // projection is centred on the player, not the viewport
    g_last = 0;
    if (!cfg.esp.enable || !g_resolved)
        return;

    void *cam = camera();
    void *g = game();
    if (!cam || !g)
        return;

    void *list = nullptr;
    if (!il2::instanceFieldGet(listField, g, &list) || !list)
        return;

    int n = il2::listSize(list);
    if (n <= 0 || n > 512)
        return;

    Vector3 eye = cameraPos(cam);

    // pass 1: the pawn sitting on the camera is the local player
    void *nearest = nullptr;
    float nearestDist = 1e9f;
    for (int i = 0; i < n; i++) {
        void *ent = il2::listItem(list, i);
        if (!ent)
            continue;
        Vector3 pos = pawnPos(ent);
        if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f)
            continue;
        float dist = norm(Vector3{pos.x - eye.x, pos.y - eye.y, pos.z - eye.z});
        if (dist < nearestDist) {
            nearestDist = dist;
            nearest = ent;
        }
    }
    g_localCamp = nearest ? pawnCamp(nearest) : -1;

    ImU32 boxCol = ImGui::ColorConvertFloat4ToU32(*(ImVec4 *)cfg.esp.box_color);
    ImU32 txtCol = ImGui::ColorConvertFloat4ToU32(*(const ImVec4 *)cfg.esp.text_color);
    const float fontSz = ImGui::GetFontSize();   // 1.92: ImFont::FontSize is gone

    for (int i = 0; i < n; i++) {
        void *ent = il2::listItem(list, i);
        if (!ent || ent == nearest)
            continue;
        if (!pawnAlive(ent))
            continue;

        Vector3 pos = pawnPos(ent);
        if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f)
            continue;

        float dist = norm(Vector3{pos.x - eye.x, pos.y - eye.y, pos.z - eye.z});
        if (cfg.esp.max_dist > 0 && dist > (float)cfg.esp.max_dist)
            continue;
        if (cfg.esp.enemy_only && g_localCamp >= 0 && pawnCamp(ent) == g_localCamp)
            continue;

        uint32_t skinId = 0;
        const bool haveSkin = offSkin != 0;
        if (haveSkin) {
            memcpy(&skinId, (uint8_t *)ent + offSkin, sizeof(skinId));
            if (!skinAllowed((int)skinId))
                continue;
        }

        Vector3 feetScr{}, headScr{};
        if (!project(cam, pos, &feetScr))
            continue;
        Vector3 head = pos;
        head.y += cfg.esp.head_offset;
        if (!project(cam, head, &headScr))
            continue;

        // Unity screen space is bottom-left origin; ImGui is top-left
        float topY = sh - headScr.y;
        float botY = sh - feetScr.y;
        float h = botY - topY;
        if (h < 4.0f || h > sh * 0.95f)
            continue;
        float w = h * 0.55f;
        float cx = feetScr.x;

        if (cfg.esp.box)
            dl->AddRect(ImVec2(cx - w * 0.5f, topY), ImVec2(cx + w * 0.5f, botY),
                        boxCol, 0, 0, 1.6f);

        if (cfg.esp.health && offHp) {
            float hp = *(float *)((uint8_t *)ent + offHp);
            float mx = offHpMax ? *(float *)((uint8_t *)ent + offHpMax) : 100.0f;
            if (mx <= 0.0f)
                mx = 100.0f;
            if (hp < 0)
                hp = 0;
            if (hp > mx)
                hp = mx;
            float frac = hp / mx;
            ImU32 hpCol = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f - frac, frac, 0.1f, 1.0f));
            dl->AddRectFilled(ImVec2(cx - w * 0.5f - 6, topY),
                              ImVec2(cx - w * 0.5f - 2, botY),
                              IM_COL32(0, 0, 0, 160));
            dl->AddRectFilled(ImVec2(cx - w * 0.5f - 6, botY - h * frac),
                              ImVec2(cx - w * 0.5f - 2, botY), hpCol);
        }

        if (cfg.esp.distance) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%dm", (int)(dist + 0.5f));
            dl->AddText(ImVec2(cx - ImGui::CalcTextSize(buf).x * 0.5f, topY - fontSz - 2.0f),
                        txtCol, buf);
        }

        if (cfg.esp.skin_draw && haveSkin) {
            char sbuf[32];
            snprintf(sbuf, sizeof(sbuf), "S%u", skinId);
            dl->AddText(ImVec2(cx - ImGui::CalcTextSize(sbuf).x * 0.5f, botY + 2.0f),
                        txtCol, sbuf);
        }

        g_last++;
    }

    // client-side write on the local pawn only; the server keeps the real value
    if (cfg.esp.skin_set && nearest && nearestDist < 2.5f && cfg.esp.skin_set_id > 0) {
        static void *lastEnt = nullptr;
        static uint32_t lastWant = 0;
        const uint32_t want = (uint32_t)cfg.esp.skin_set_id;
        if (nearest != lastEnt || want != lastWant) {
            if (cfg.esp.skin_rva) {
                if (il2::callRva((uint32_t)cfg.esp.skin_rva, nearest, want))
                    codm_log("skin: SetSkinID(%u) via rva 0x%x", want, cfg.esp.skin_rva);
                else
                    codm_log("skin: libil2cpp base missing, rva call skipped");
            } else if (offSkin) {
                memcpy((uint8_t *)nearest + offSkin, &want, sizeof(want));
                codm_log("skin: wrote %u at 0x%zx", want, offSkin);
            }
            lastEnt = nearest;
            lastWant = want;
        }
    }
}

int lastTargets() { return g_last; }

bool ready() { return g_resolved; }

} // namespace esp
