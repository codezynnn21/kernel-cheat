// esp.h
#pragma once
#include "common.h"
#include "il2cpp.h"

struct ImDrawList;

namespace esp {
void init(const CheatConfig &cfg);
bool ready();
void render(ImDrawList *dl, float sw, float sh, const CheatConfig &cfg);
int  lastTargets();
}
