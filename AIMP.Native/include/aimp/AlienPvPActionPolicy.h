#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace aimp::alien_pvp::actions {
// These are gameplay admission limits, not undocumented CATHODE constants.
inline constexpr float kAttackReach=2.6F, kAttackHeight=1.5F;
inline constexpr float kAttackCosine=0.5F; // 60 degrees either side of the look direction
inline constexpr float kVentMatchTolerance=0.02F, kVentEnterReach=8.0F;
// Deliberate PvP selection radius. Actual connectivity remains a native query;
// a portal in front is not evidence that an arbitrary floor is traversable.
inline constexpr float kVentExitReach=32.0F, kVentExitMinimumDistance=1.0F;
inline constexpr std::uint64_t kAttackCooldownMs=600U, kVentCooldownMs=350U;
inline constexpr std::uint64_t kActionTimeoutMs=30000U;
struct Point final { float x{},y{},z{}; bool operator==(const Point&) const noexcept=default; };
inline bool Finite(Point p) noexcept {
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&
        std::abs(p.x)<=100000.0F&&std::abs(p.y)<=100000.0F&&std::abs(p.z)<=100000.0F;
}
inline float DistanceSquared(Point a,Point b) noexcept {
    const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return x*x+y*y+z*z;
}
struct Candidate final {
    std::uint64_t peer{}; Point position{};
    bool alive{},ready{},hidden{},reachable{};
};
// Only Host-certified participants enter this span. The client supplies no
// victim pointer or target ID. Occlusion/native availability is a separate
// positive proof; distance alone never authorizes an attack through a wall.
inline std::uint64_t SelectVictim(Point source,float yaw,std::uint64_t alienPeer,
    std::span<const Candidate> candidates) noexcept {
    if(!Finite(source)||!std::isfinite(yaw)||alienPeer==0U)return 0U;
    const float fx=std::sin(yaw),fz=std::cos(yaw);
    float best=std::numeric_limits<float>::infinity();std::uint64_t peer{};
    for(const auto& c:candidates) {
        if(!c.peer||c.peer==alienPeer||!c.alive||!c.ready||!c.reachable||!Finite(c.position))continue;
        const float dx=c.position.x-source.x,dz=c.position.z-source.z;
        const float d2=dx*dx+dz*dz,dy=std::abs(c.position.y-source.y);
        if(dy>kAttackHeight||d2>kAttackReach*kAttackReach)continue;
        const float d=std::sqrt(d2);
        if(d>0.05F&&(dx*fx+dz*fz)/d<kAttackCosine)continue;
        // Stable tie-breaks prevent registry iteration order from picking a victim.
        const float score=d2+dy*dy;
        if(score<best||(score==best&&(!peer||c.peer<peer))){best=score;peer=c.peer;}
    }
    return peer;
}
struct Vent final { std::uint32_t localIndex{}; Point entrance{},exit{}; };
inline const Vent* NearestVent(Point source,std::span<const Vent> vents,bool entering) noexcept {
    if(!Finite(source))return nullptr;
    const Vent* best{};float score=std::numeric_limits<float>::infinity();
    for(const auto& v:vents) {
        if(!Finite(v.entrance)||!Finite(v.exit))continue;
        const auto p=entering?v.entrance:v.exit;
        const auto d=DistanceSquared(source,p);
        if(entering&&(d>kVentEnterReach*kVentEnterReach||std::abs(source.y-p.y)>3.5F))continue;
        if(d<score||(d==score&&(!best||v.localIndex<best->localIndex))){score=d;best=&v;}
    }
    return best;
}
inline float DirectedExitScore(Point source,float yaw,const Vent& vent) noexcept {
    if(!Finite(source)||!Finite(vent.entrance)||!Finite(vent.exit)||!std::isfinite(yaw))
        return std::numeric_limits<float>::infinity();
    const auto p=vent.exit;
    const float dx=p.x-source.x,dz=p.z-source.z,d2=dx*dx+dz*dz;
    if(d2<kVentExitMinimumDistance*kVentExitMinimumDistance || d2>kVentExitReach*kVentExitReach ||
        std::abs(p.y-source.y)>3.5F)return std::numeric_limits<float>::infinity();
    const float facing=(dx*std::sin(yaw)+dz*std::cos(yaw))/std::sqrt(d2);
    if(facing<kAttackCosine)return std::numeric_limits<float>::infinity();
    // Prefer the nearest eligible exit in the look cone; never synthesize a
    // coordinate on the far side of a wall or silently choose another floor.
    return DistanceSquared(source,p);
}
inline const Vent* DirectedExit(Point source,float yaw,std::span<const Vent> vents) noexcept {
    const Vent* selected{};float best=std::numeric_limits<float>::infinity();
    for(const auto& v:vents){const auto score=DirectedExitScore(source,yaw,v);
        if(std::isfinite(score)&&(score<best || (score==best&&(!selected||v.localIndex<selected->localIndex))))
            {selected=&v;best=score;}}
    if(selected)return selected;
    // No portal ahead: only return through a nearby genuine exit, not the
    // nearest arbitrary portal anywhere in the level.
    const auto* nearest=NearestVent(source,vents,false);
    return nearest&&DistanceSquared(source,nearest->exit)<=kVentEnterReach*kVentEnterReach&&
        std::abs(source.y-nearest->exit.y)<=3.5F?nearest:nullptr;
}
// Vent indices and pointers are process-local. Resolve a Host-selected real
// portal by BOTH endpoints, rejecting ambiguous or missing local resources.
inline const Vent* MatchVent(Point entrance,Point exit,std::span<const Vent> vents) noexcept {
    if(!Finite(entrance)||!Finite(exit))return nullptr;
    const Vent* found{};constexpr auto tolerance=kVentMatchTolerance*kVentMatchTolerance;
    for(const auto& v:vents)if(Finite(v.entrance)&&Finite(v.exit)&&
        DistanceSquared(entrance,v.entrance)<=tolerance&&DistanceSquared(exit,v.exit)<=tolerance) {
        if(found)return nullptr;
        found=&v;
    }
    return found;
}
// An action edge is consumed once even when declined. Retrying an old packet
// cannot bypass cooldowns, change its target, or resubmit an earlier native act.
class EdgeGate final {
public:
    void Reset(std::uint64_t epoch=0U) noexcept { *this=EdgeGate{};epoch_=epoch; }
    bool Consume(std::uint64_t epoch,std::uint64_t hostSequence,std::uint64_t now,
        bool attack,bool nativeBusy) noexcept {
        if(!epoch||!hostSequence||epoch!=epoch_||hostSequence<=lastSequence_)return false;
        lastSequence_=hostSequence;
        const auto delay=attack?kAttackCooldownMs:kVentCooldownMs;
        if(nativeBusy||(hadAction_&&(now<lastAction_||now-lastAction_<delay)))return false;
        lastAction_=now;hadAction_=true;return true;
    }
private:
    std::uint64_t epoch_{},lastSequence_{},lastAction_{};bool hadAction_{};
};
} // namespace aimp::alien_pvp::actions
