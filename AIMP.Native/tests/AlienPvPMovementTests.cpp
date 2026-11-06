#include "aimp/AlienPvPMovementMath.h"
#include <cmath>
#include <cstdio>
#include <limits>

using aimp::alien_pvp_movement::Plan;
using aimp::alien_pvp_movement::TryPlan;
namespace {
bool Near(const float a,const float b) noexcept { return std::abs(a-b)<0.0002F; }
bool Run() {
    Plan v{};
    if (!TryPlan(1,0,0.0F,1U,v) || !Near(v.worldX,0.0F) ||
        !Near(v.worldZ,1.0F) || v.gait!=1U) return false;
    if (!TryPlan(0,1,0.0F,1U,v) || !Near(v.worldX,1.0F) ||
        !Near(v.worldZ,0.0F)) return false;
    if (!TryPlan(0,-1,0.0F,1U,v) || !Near(v.worldX,-1.0F) ||
        !Near(v.worldZ,0.0F)) return false;
    if (!TryPlan(1,0,1.5707963F,1U,v) || !Near(v.worldX,1.0F) ||
        !Near(v.worldZ,0.0F)) return false;
    if (!TryPlan(1,1,0.0F,2U,v) ||
        !Near(v.worldX,0.7071068F) || !Near(v.worldZ,0.7071068F) ||
        !Near(v.intensity,1.6F)) return false;
    if (!TryPlan(-1,0,0.0F,0U,v) || !Near(v.worldZ,-1.0F) ||
        !Near(v.intensity,0.6F)) return false;
    if (!TryPlan(1,0,0.0F,3U,v) || !Near(v.intensity,2.2F)) return false;
    if (TryPlan(0,0,0.0F,1U,v) || TryPlan(2,0,0.0F,1U,v) ||
        TryPlan(1,0,0.0F,4U,v) ||
        TryPlan(1,0,std::numeric_limits<float>::quiet_NaN(),1U,v)) return false;
    return true;
}
}
int main() {
    if (Run()) { std::puts("Alien PvP movement axes: PASS");return 0; }
    std::fputs("Alien PvP movement axes: FAIL\n",stderr);return 1;
}
