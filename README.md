# CODM Overlay — il2cpp ESP kit

A port of the architecture behind the `葫芦娃PUBG.sh` reference cheat to **Call of Duty Mobile**.
Same shape (root daemon → in-process payload → ImGui overlay over the game's GLES swap),
different engine: CODM is **Unity IL2CPP**, not UE4, so entities are read through
`libil2cpp.so` reflection instead of `UWorld`/`GNames` pointer walks.

No Zygisk, no Riru, no Xposed. One `ptrace` attach at process start, one `dlopen`, done.

```
Magisk module
  ├── bin/codm-inject        root daemon (aarch64)
  │     ├── find PID by package name
  │     ├── stage payload into /data/data/<pkg>/code_cache/libcodm.so
  │     │     (chown to the app uid — the dlopen runs with app credentials)
  │     ├── PTRACE_ATTACH + remote mmap + remote __loader_dlopen
  │     └── loop: reinject on every game restart
  └── assets/libcodm.so      payload, runs inside com.*.codm
        ├── il2cpp: image → class → field/method resolution + dumper
        ├── eglSwapBuffers / eglSwapBuffersWithDamageKHR hook (ShadowHook)
        │     → ImGui GLES3 overlay
        ├── AInputQueue_getEvent hook → touch for the menu
        └── ESP: Camera::WorldToScreenPoint projection, box + health + distance
```

## Layout

```
codm-kit/
├── CMakeLists.txt
├── build.sh                 Linux / macOS / WSL
├── build.ps1                Windows
├── magisk/                  module payload (copied into the zip root)
│   ├── module.prop
│   ├── customize.sh         perms + seed /data/adb/codm/codm.cfg
│   ├── post-fs-data.sh
│   ├── service.sh           boot-wait + watchdog loop
│   ├── uninstall.sh
│   ├── sepolicy.rule        allow magisk → appdomain ptrace
│   ├── codm.sh              status|start|stop|restart|log|pull|cfg
│   └── codm.cfg             default config
└── native/
    ├── common.h             config schema, target packages, logcat logging
    ├── remote.h/.cpp        aarch64 ptrace RPC: read/write/call/mmap/ELF symtab
    ├── injector.cpp         the daemon
    └── payload/
        ├── il2cpp.h/.cpp    reflection + full image/class/field/method dumper
        ├── esp.h/.cpp       projection and entity walk
        └── entry.cpp        hooks, ImGui menu, touch, dump trigger
```

## Requirements

| | |
|---|---|
| Device | arm64, rooted (Magisk 24+), Android 12+ |
| Build | Android NDK r26+, CMake 3.22+, Ninja |
| Fetched at configure time | Dear ImGui `v1.92.6`, ShadowHook `v1.0.10` |
| CODM | any of `com.activision.callofduty.shooter`, `com.garena.game.codm`, `com.tencent.tmgp.cod`, `com.vng.codm` |

## Build

Windows:

```powershell
.\build.ps1 -Ndk "C:\Users\you\AppData\Local\Android\Sdk\ndk\27.2.12479018"
```

Linux / WSL:

```bash
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/27.2.12479018
./build.sh
```

Output: `out/codm_overlay.zip` (module.prop at zip root, flashable).

**Verified:** built clean with NDK r27c (Clang 18.0.3, `-Wall -Wextra -Werror`) and
CMake 4.4.4 / Ninja: `codm_overlay.zip` = 446 KB, `libcodm.so` = 789,480 B,
`codm-inject` = 232,680 B, both ELF64 AArch64, `libcodm.so` linked only against
`libandroid libEGL libGLESv3 liblog libdl libm libc`. Same script CI runs.

## CI

`.github/workflows/build.yml` runs on every push touching `codm-kit/` and on manual dispatch.
It expects the kit at `codm-kit/` in the repo root.

- resolves the runner's newest NDK (`ANDROID_NDK_LATEST_HOME`, else `$ANDROID_SDK_ROOT/ndk/*`)
- caches the ImGui / ShadowHook downloads in `.fc/`
- runs `build.sh` — the same path as a local Linux build
- asserts the zip layout: `module.prop` at the root, both binaries present
- asserts both ELFs are aarch64 and that `libcodm.so` has **no** `libshadowhook.so` NEEDED entry
- uploads `codm_overlay` as a 30-day artifact

What CI cannot do is test injection — that needs the phone.

## Install

1. Flash `out/codm_overlay.zip` in Magisk, reboot.
2. `su -c 'sh /data/adb/modules/codm_overlay/codm.sh status'` → daemon running, waiting for the game.
3. Launch CODM. Within a few seconds:
   ```
   su -c 'tail -f /data/adb/codm/injector.log'
   # [..] target up: com.activision.callofduty.shooter pid=12345
   # [..] injected com.activision.callofduty.shooter pid=12345 handle=0x...
   adb logcat -s codm
   # imgui up (gles3)
   # payload armed
   ```
4. Three-finger tap toggles the menu.

## Mapping the game

Resolved against `dump(GR .59).cs` (MXFXKZY dump, 2026-11-05). Every name below is looked
up through il2cpp reflection at runtime, so offsets are not hardcoded anywhere:

```
GameEngine.GamePlay.s_Game                          static BaseGame
  BaseGame.AllPawns                              0x160  System.Collections.Generic.PawnList : List<Pawn>
    GameBase.Pawn
      m_Transform                                0x20   GameEngine.BaseObject -> UnityEngine.Transform
      m_AttackableInfo                           0x78   GameEngine.AttackableTarget (struct)
        m_Health                                 0x34   -> absolute 0xAC  float
        m_MaxHealth                              0x38   -> absolute 0xB0  float
        m_Camp                                   0x58   -> absolute 0xD0  int
        m_ActorID                                0x30   -> absolute 0xA8  uint
      m_IsAlive                                  0x550  bool
```

`BaseGame` also carries `AllPawnsDict` (`Dictionary<uint, Pawn>`, 0x168) and
`EnemyPawns` (`PawnList`, 0x178) — swap `il2cpp.list` if you would rather walk one of those.

`codm.cfg` ships with this mapping already filled in:

```ini
il2cpp.game_class = GameEngine.GamePlay
il2cpp.game_field = s_Game
il2cpp.root_class = GameBase.BaseGame
il2cpp.list       = AllPawns
il2cpp.class      = GameBase.Pawn
il2cpp.pos_mode   = transform
il2cpp.pos        = m_Transform
il2cpp.hp         = m_AttackableInfo.m_Health
il2cpp.hpmax      = m_AttackableInfo.m_MaxHealth
il2cpp.team       = m_AttackableInfo.m_Camp
il2cpp.alive      = m_IsAlive
```

A `.` in a field path walks into a struct: the resolver adds the segment offsets
(`m_AttackableInfo` 0x78 + `m_Health` 0x34) through `il2cpp_field_get_type` →
`il2cpp_class_from_type`. `pos_mode = transform` invokes `Transform.get_position`;
`pos_mode = field` reads a `Vector3` field directly (e.g. `m_CachedPosition` at 0x30).

`logcat -s codm` prints what resolved:

```
esp: game=GameEngine.GamePlay.s_Game list=GameBase.BaseGame.AllPawns pawn=GameBase.Pawn
     pos=0x20(transform) hp=0xac max=0xb0 camp=0xd0 alive=0x550 skin=0xa8 resolved=1
```

`resolved=1` plus `targets: N` in the menu means the walk is live. If a class or field is
renamed by a patch, that one line names the key that failed.

## Skin offsets

Straight answer first: **there is no player-outfit id to write.** Character appearance is
server-sent proto data (`msg.RoleAppearance` → `EquippedItem suit/hat/clothes/wing`), and the
client composes the model from asset lists in `GameEngine.AvatarComposer`. A single field
write cannot swap an operator skin; anything that claims otherwise will just desync the model.
This is why the earlier `OriginalSkinId // 0xEC` grep resolves to `msg.ShopConfig`, not to a pawn.

What is a plain field write, all client-side visual only:

| field | offset | type |
|---|---|---|
| `GameBase.Weapon.GlobalSkinID` | 0xA8 | int |
| `GameBase.Weapon.m_SkinID_MainWeaponRoot` | 0x970 | int |
| `GameBase.Weapon.m_SkinID_Sight/Sto/Iro/Rai/Grip/Muzzle/Mag` | 0x938..0x970 | int[] |
| `GameEngine.SwitchRoleComponent.m_InitAvatarID_3P` | 0x88 | int (3P model id) |
| `GameEngine.SkiingComponent.m_PhysSkisSkinID` | 0x2C | uint |

The draw-path uses `il2cpp.skin` as a per-pawn id under the box; the shipped default is
`m_AttackableInfo.m_ActorID` because every pawn has a stable, non-zero one.

```ini
il2cpp.skin      = m_AttackableInfo.m_ActorID
il2cpp.skin_off  =                # raw hex wins over the name, e.g. 0xA8
esp.skin         = 1              # draw S<id> under the box
esp.skin_filter  = 1001,1002      # optional: only draw these ids
```

Client-side override on the local pawn only:

```ini
skin.set     = 1
skin.set_id  = 3104101
skin.set_rva = 0                  # 0 = write il2cpp.skin; else call this RVA with (pawn, id)
```

- the local player is the pawn with the smallest distance to the camera (`< 2.5 m`)
- applied once per object/id change, not every frame
- dump RVAs shift with every CODM update; re-dump after a patch

## Config reference

| key | default | meaning |
|---|---|---|
| `esp.enable` | 1 | master switch |
| `esp.box` | 1 | bounding box |
| `esp.health` | 1 | health bar (left of the box) |
| `esp.distance` | 1 | distance text |
| `esp.max_dist` | 400 | metres; entities further out are skipped |
| `esp.menu` | 1 | menu visible on inject |
| `esp.head_offset` | 1.75 | metres added to the feet position to get the head point |
| `esp.box_color.0..3` | red | RGBA 0..1 |
| `esp.enemy_only` | 1 | skip pawns whose `m_Camp` matches the local pawn |
| `il2cpp.image` | `Assembly-CSharp.dll` | assembly holding the game classes |
| `il2cpp.game_class` | `GameEngine.GamePlay` | namespace-qualified static holder |
| `il2cpp.game_field` | `s_Game` | static `BaseGame` reference on that holder |
| `il2cpp.root_class` | `GameBase.BaseGame` | class carrying the pawn list |
| `il2cpp.list` | `AllPawns` | instance field on the root class, `List<Pawn>` |
| `il2cpp.class` | `GameBase.Pawn` | namespace-qualified pawn class |
| `il2cpp.pos_mode` | `transform` | `transform` = `Transform.get_position`, `field` = `Vector3` field |
| `il2cpp.pos` | `m_Transform` | transform or `Vector3` field on the pawn |
| `il2cpp.hp` | `m_AttackableInfo.m_Health` | float field path |
| `il2cpp.hpmax` | `m_AttackableInfo.m_MaxHealth` | float field path, 100 when unset |
| `il2cpp.team` | `m_AttackableInfo.m_Camp` | int camp/team id |
| `il2cpp.alive` | `m_IsAlive` | bool, dead pawns are skipped |
| `il2cpp.skin` | `m_AttackableInfo.m_ActorID` | per-pawn id drawn under the box |
| `il2cpp.skin_off` | — | raw hex offset for the id, wins over the name |
| `esp.skin` | 1 | draw `S<id>` under the box |
| `esp.skin_filter` | — | comma list of ids; anything else is not drawn |
| `skin.set` | 0 | client-side override on the local player |
| `skin.set_id` | 0 | skin id to apply |
| `skin.set_rva` | 0 | `SetSkinID` RVA; 0 writes the field instead of calling |
| `aim.*` | off | parsed, not wired to touch injection yet |

## Troubleshooting

| symptom | cause | fix |
|---|---|---|
| `attach failed ... errno=1` | SELinux blocked ptrace | `magiskpolicy --live "allow magisk appdomain process { ptrace getattr }"`, then `codm.sh restart`. The module's `sepolicy.rule` already grants it at boot |
| `payload copy failed ... errno=13` | magisk domain cannot write app data | confirm the module is enabled, `su -c 'ls -ld /data/data/<pkg>/code_cache'` |
| `dlopen returned null` | payload staged with wrong uid, or namespace rejected the path | check `ls -l /data/data/<pkg>/code_cache/libcodm.so` — owner must be the app uid, mode 644; SELinux also needs the `execute` rule that `sepolicy.rule` installs |
| `payload armed` but no overlay | game is on Vulkan: check `su -c 'cat /proc/$(pidof <pkg>)/maps \| grep -c vulkan'` — if that is non-zero, no EGL swap happens and the GLES overlay never draws. GLES path is what this kit ships; the Vulkan `vkQueuePresentKHR` backend is the next plate |
| overlay draws, touch dead | `AInputQueue_getEvent` not exported on this ROM | check `getprop ro.build.version.sdk`; the fallback symbol is `android::InputQueue::dispatchEvent` |
| `class '...' missing` | wrong assembly or obfuscated name changed with the game update | re-dump, re-map |
| `targets: 0` with `resolved=1` | static list is on a different class, or the field is a `Dictionary`/custom container | map the container's value list instead, or wrap `listSize`/`listItem` for its layout |
| box mirrored / offset | projection expectation differs | ESP already flips Y only; adjust in `native/payload/esp.cpp:render` |

## What came from the PUBG reference, and what changed

| reference (`葫芦娃PUBG.sh`, 41 MB aarch64 ELF) | this kit |
|---|---|
| Dear ImGui 1.92.6 WIP, `imgui_impl_vulkan` + `vkQueuePresentKHR` | ImGui 1.92.6, `imgui_impl_opengl3` + `eglSwapBuffers` / `eglSwapBuffersWithDamageKHR` (CODM's default path on this device class is GLES3) |
| `A64HookFunction` inline hooks | ShadowHook 1.0.10, compiled static into the payload |
| `ptrace` + `/proc/%d/maps` + `/proc/%d/fd/%d` used for self-protection | same primitives, used to inject and to resolve remote symbols |
| UE4: `libUE4.so`, `GNames`, `GWorld`, `AActor**`, bone arrays | Unity IL2CPP: `libil2cpp.so`, `il2cpp_class_from_name`, `il2cpp_field_get_offset`, boxed `Vector3` returns |
| bone ESP, aimbot, recoil control, gyro aim, touch aim (`GyroAimFps`, `crossbowAutoAimLimitDistance`, …) | box + health + distance first; aim/gyro/touch are the next layer on the same hooks |
| six PUBG packages hardcoded | four CODM packages, same detection loop |

Gyro: `陀螺仪模块.zip` (`gronsensor` 3.0.1) hooks `sensorservice` and is game-agnostic — it
feeds the game's own gyro path, so it needs no port. Only a gyro *aim* feature would,
and that belongs to the aim layer, not the sensor hook.

Kernel module (`5.10.sh`): not required for this build. That `.ko` is a generic
read/write-physical-memory helper with vermagic `5.10.198-00001-gcab9fa53e761-dirty`;
`codm-inject` uses `process_vm_readv`/`writev` plus `ptrace`, which needs no module on a
Mahiru 5.10.247 kernel. If you later want kernel-side ESP reads, the module path is
`insmod /data/adb/modules/codm_overlay/bin/nh.ko` replacing the userspace reads.
