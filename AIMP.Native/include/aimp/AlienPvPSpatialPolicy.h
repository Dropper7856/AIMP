// Donor-derived movement/parking: AI-Playable-Alien, (c) 2026 pirletik.
// MIT license: third_party/AI-Playable-Alien/LICENSE.
#pragma once
#include "aimp/AlienPvPWire.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace aimp::alien_pvp::spatial {
// ConditionIsBackstage (006EBA60 -> 0077A350) tests BOTH bytes, not
// invisibility 02D9 and not only 02DA. Values are observations, never forced.
inline bool Backstage(unsigned char a,unsigned char b) noexcept {return a!=0U || b!=0U;}
inline bool ManualBackstage(unsigned char cycle,bool backstage,bool actionBusy) noexcept {
    return backstage && !actionBusy && (cycle==1U || cycle==55U || cycle==56U || cycle==59U || cycle==75U || cycle==77U);
}
inline bool VentStageReady(unsigned char cycle,bool backstage,bool entering,bool replica=false) noexcept {
    if(backstage!=entering)return false;
    if(entering)return ManualBackstage(cycle,true,false);
    // A replica's native visibility setter can retain its stationary backstage
    // cycle after frontstage has been restored. It is not an active passage.
    return cycle==1U || cycle==55U || cycle==56U || cycle==59U ||
        (replica && (cycle==57U || cycle==75U || cycle==77U));
}
// Donor vent.cpp::BackstageMoveTick. Physical backstage translation is NOT
// normal locomotion and is never used to synthesize a vent entry/exit.
inline bool BackstageStep(float forward,float side,float yaw,unsigned gait,float dt,
    float& dx,float& dz) noexcept {
    dx=dz=0.0F;
    if(!std::isfinite(forward)||!std::isfinite(side)||!std::isfinite(yaw)||!std::isfinite(dt)||
        std::abs(forward)>1.0F||std::abs(side)>1.0F||gait>3U||dt<=0.0F)return false;
    const float length=std::hypot(forward,side);
    if(length<0.01F)return false;
    constexpr std::array<float,4> speeds{1.2F,2.5F,4.0F,6.0F};
    const float f=forward/length,s=side/length,step=speeds[gait]*std::min(dt,0.05F)*std::min(length,1.0F);
    dx=(std::sin(yaw)*f+std::cos(yaw)*s)*step;
    dz=(std::cos(yaw)*f-std::sin(yaw)*s)*step;return true;
}
// Cap displacement PER STEP, not total error. A 20m error must not silently
// become "no correction" as in v416. The frontstage caller uses a native
// locomotion/path carrier; only the backstage caller moves the body directly.
inline bool BoundedStep(float dx,float dz,float maximum,float& x,float& z) noexcept {
    x=z=0.0F;
    if(!std::isfinite(dx)||!std::isfinite(dz)||!std::isfinite(maximum)||maximum<=0.0F)return false;
    const float distance=std::hypot(dx,dz);
    if(!std::isfinite(distance)||distance<=0.05F)return false;
    const float scale=std::min(1.0F,maximum/distance);x=dx*scale;z=dz*scale;return true;
}
// Donor alien_extra.cpp::AmandaHideTick. Keep the original home separately
// from the parking position, and never use parking as an Alien correction.
inline bool ParkingPosition(const std::array<float,3>& home,std::array<float,3>& out) noexcept {
    for(float v:home)if(!std::isfinite(v)||std::abs(v)>99900.0F)return false;
    out=home;out[1]-=25.0F;return true;
}
} // namespace aimp::alien_pvp::spatial
