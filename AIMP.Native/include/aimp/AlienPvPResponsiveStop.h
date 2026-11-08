#pragma once
#include <array>
#include <cstdint>

namespace aimp::alien_pvp {
// Donor control.cpp::SnapHoldTick: bounded stop hold, cancelled by a new input
// or any passage/pair. No pointer or native action lifetime is stored here.
class ResponsiveStop final {
public:
    void Begin(std::uint64_t now,float x,float z) noexcept {active_=true;until_=now+2000U;idleAt_=0U;point_={x,z};}
    void Cancel() noexcept {active_=false;}
    bool Target(std::uint64_t now,unsigned cycle,bool eligible,std::array<float,2>& point) noexcept {
        if(!active_)return false;
        if(!eligible || now>until_){Cancel();return false;}
        if(cycle==1U){if(!idleAt_)idleAt_=now;}else idleAt_=0U;
        if(idleAt_ && now-idleAt_>150U){Cancel();return false;}
        point=point_;return true;
    }
private:
    bool active_{};
    std::uint64_t until_{},idleAt_{};
    std::array<float,2> point_{};
};
}
