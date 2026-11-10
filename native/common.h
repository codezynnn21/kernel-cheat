// common.h — shared types between injector (root) and payload (in-game)
#pragma once
#include <android/log.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#define CODM_TAG "codm"
#define CODM_CFG_PATH "/data/adb/codm/codm.cfg"
#define CODM_STATE_DIR "/data/adb/codm"

static const char *kTargets[] = {
    "com.activision.callofduty.shooter",  // global
    "com.garena.game.codm",               // garena / SEA
    "com.tencent.tmgp.cod",               // china
    "com.vng.codm",                       // vietnam
};

inline bool is_target_package(const char *name)
{
    for (const char *t : kTargets)
        if (strcmp(t, name) == 0)
            return true;
    return false;
}

struct EspConfig {
    int   enable     = 1;
    int   box        = 1;
    int   health     = 1;
    int   distance   = 1;
    int   max_dist   = 400;
    int   menu       = 1;
    float head_offset = 1.75f;
    float box_color[4]  = {1.0f, 0.15f, 0.15f, 1.0f};
    float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // skin / outfit item id on the player object
    int   skin_draw      = 1;
    int   skin_set       = 0;
    int   skin_set_id    = 0;
    int   skin_rva       = 0;      // SetSkinID RVA, e.g. 0x64A41DC
    char  skin_filter[128] = "";   // "1001,1002" — empty draws every skin
    char  ent_skin[48]     = "skin";
    char  ent_skin_off[24] = "";   // raw hex override, e.g. 0xEC

    char  ent_image[64] = "Assembly-CSharp.dll";
    char  game_class[96] = "GameEngine.GamePlay";
    char  game_field[48] = "s_Game";
    char  root_class[96] = "GameBase.BaseGame";
    char  ent_class[96] = "GameBase.Pawn";
    char  ent_list[96]  = "AllPawns";
    char  ent_pos_mode[16] = "transform";
    char  ent_pos[64]   = "m_Transform";
    char  ent_hp[64]    = "m_AttackableInfo.m_Health";
    char  ent_hpmax[64] = "m_AttackableInfo.m_MaxHealth";
    char  ent_team[64]  = "m_AttackableInfo.m_Camp";
    char  ent_alive[48] = "m_IsAlive";
    int   enemy_only    = 1;
};

struct AimConfig {
    int enable = 0;
    int fov    = 120;
    int smooth = 8;
    int bone   = 0;
};

struct CheatConfig {
    EspConfig esp;
    AimConfig aim;
};

// ---- config file -------------------------------------------------------------

class CfgFile {
public:
    explicit CfgFile(const char *path) { load(path); }

    int geti(const char *key, int def) const
    {
        const char *v = find(key);
        return v ? (int)strtol(v, nullptr, 0) : def;
    }
    float getf(const char *key, float def) const
    {
        const char *v = find(key);
        return v ? strtof(v, nullptr) : def;
    }
    std::string gets(const char *key, const char *def) const
    {
        const char *v = find(key);
        return v ? std::string(v) : std::string(def);
    }
    void getc(const char *key, float out[4]) const
    {
        char k[96];
        for (int i = 0; i < 4; i++) {
            snprintf(k, sizeof(k), "%s.%d", key, i);
            const char *v = find(k);
            if (v)
                out[i] = strtof(v, nullptr);
        }
    }

private:
    struct Entry { std::string k, v; };
    std::vector<Entry> entries_;

    void load(const char *path)
    {
        FILE *f = fopen(path, "r");
        if (!f)
            return;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || *p == 0)
                continue;
            char *eq = strchr(p, '=');
            if (!eq)
                continue;
            *eq = 0;
            char *val = eq + 1;
            char *end = val + strlen(val);
            while (end > val && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
                *--end = 0;
            char *ks = p, *ke = eq;
            while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t'))
                *--ke = 0;
            entries_.push_back({ks, val});
        }
        fclose(f);
    }

    const char *find(const char *key) const
    {
        for (const auto &e : entries_)
            if (e.k == key)
                return e.v.c_str();
        return nullptr;
    }
};

// ---- logging / runtime paths -------------------------------------------------
// The payload runs inside the game's sandbox: it cannot write /data/adb.
// Payload logs go to logcat ("codm"), the dumper writes into the app's own
// files dir so the game's uid owns the file.

inline void codm_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, CODM_TAG, fmt, ap);
    va_end(ap);
}

inline bool codm_self_pkg(char *out, size_t cap)
{
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f)
        return false;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    if (n == 0)
        return false;
    out[n] = 0;
    return true;
}

// /data/data/<pkg>/files/<name>  — writable by the injected process
inline bool codm_data_path(const char *name, char *out, size_t cap)
{
    char pkg[256] = {0};
    if (!codm_self_pkg(pkg, sizeof(pkg)))
        return false;
    snprintf(out, cap, "/data/data/%s/files/%s", pkg, name);
    return true;
}

inline void config_load(CheatConfig *cfg)
{
    CfgFile f(CODM_CFG_PATH);

    cfg->esp.enable      = f.geti("esp.enable", cfg->esp.enable);
    cfg->esp.box         = f.geti("esp.box", cfg->esp.box);
    cfg->esp.health      = f.geti("esp.health", cfg->esp.health);
    cfg->esp.distance    = f.geti("esp.distance", cfg->esp.distance);
    cfg->esp.max_dist    = f.geti("esp.max_dist", cfg->esp.max_dist);
    cfg->esp.menu        = f.geti("esp.menu", cfg->esp.menu);
    cfg->esp.head_offset = f.getf("esp.head_offset", cfg->esp.head_offset);
    f.getc("esp.box_color", cfg->esp.box_color);
    f.getc("esp.text_color", cfg->esp.text_color);

    cfg->esp.skin_draw   = f.geti("esp.skin", cfg->esp.skin_draw);
    cfg->esp.skin_set    = f.geti("skin.set", cfg->esp.skin_set);
    cfg->esp.skin_set_id = f.geti("skin.set_id", cfg->esp.skin_set_id);
    cfg->esp.skin_rva    = f.geti("skin.set_rva", cfg->esp.skin_rva);

    auto copy = [](char *dst, size_t cap, const std::string &s) {
        if (s.empty() || s.size() >= cap)
            return;
        snprintf(dst, cap, "%s", s.c_str());
    };
    copy(cfg->esp.ent_image, sizeof(cfg->esp.ent_image), f.gets("il2cpp.image", cfg->esp.ent_image));
    copy(cfg->esp.game_class, sizeof(cfg->esp.game_class), f.gets("il2cpp.game_class", cfg->esp.game_class));
    copy(cfg->esp.game_field, sizeof(cfg->esp.game_field), f.gets("il2cpp.game_field", cfg->esp.game_field));
    copy(cfg->esp.root_class, sizeof(cfg->esp.root_class), f.gets("il2cpp.root_class", cfg->esp.root_class));
    copy(cfg->esp.ent_class, sizeof(cfg->esp.ent_class), f.gets("il2cpp.class", cfg->esp.ent_class));
    copy(cfg->esp.ent_list,  sizeof(cfg->esp.ent_list),  f.gets("il2cpp.list", cfg->esp.ent_list));
    copy(cfg->esp.ent_pos_mode, sizeof(cfg->esp.ent_pos_mode), f.gets("il2cpp.pos_mode", cfg->esp.ent_pos_mode));
    copy(cfg->esp.ent_pos,   sizeof(cfg->esp.ent_pos),   f.gets("il2cpp.pos", cfg->esp.ent_pos));
    copy(cfg->esp.ent_hp,    sizeof(cfg->esp.ent_hp),    f.gets("il2cpp.hp", cfg->esp.ent_hp));
    copy(cfg->esp.ent_hpmax, sizeof(cfg->esp.ent_hpmax), f.gets("il2cpp.hpmax", cfg->esp.ent_hpmax));
    copy(cfg->esp.ent_team,  sizeof(cfg->esp.ent_team),  f.gets("il2cpp.team", cfg->esp.ent_team));
    copy(cfg->esp.ent_alive, sizeof(cfg->esp.ent_alive), f.gets("il2cpp.alive", cfg->esp.ent_alive));
    cfg->esp.enemy_only  = f.geti("esp.enemy_only", cfg->esp.enemy_only);
    copy(cfg->esp.ent_skin,  sizeof(cfg->esp.ent_skin),  f.gets("il2cpp.skin", cfg->esp.ent_skin));
    copy(cfg->esp.ent_skin_off, sizeof(cfg->esp.ent_skin_off),
         f.gets("il2cpp.skin_off", cfg->esp.ent_skin_off));
    copy(cfg->esp.skin_filter, sizeof(cfg->esp.skin_filter),
         f.gets("esp.skin_filter", cfg->esp.skin_filter));

    cfg->aim.enable = f.geti("aim.enable", cfg->aim.enable);
    cfg->aim.fov    = f.geti("aim.fov", cfg->aim.fov);
    cfg->aim.smooth = f.geti("aim.smooth", cfg->aim.smooth);
    cfg->aim.bone   = f.geti("aim.bone", cfg->aim.bone);
}
