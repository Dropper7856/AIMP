#pragma once
#include "aimp/PlayerHidingPresentation.h"
#include <cstdint>
namespace aimp::alien_pvp::hiding {
inline bool Searchable(const player_hiding_presentation::State& s) noexcept {
    return player_hiding_presentation::Valid(s) && s.phase==2U && (s.kind==1U || s.kind==2U);
}
class Lease final {
public:
    bool Begin(std::uint64_t epoch,const player_hiding_presentation::State& s,std::uint64_t now) noexcept {
        if(!epoch || !Searchable(s))return false;
        epoch_=epoch;kind_=s.kind;entity_=s.entityShortGuid;hierarchy_=s.hierarchyHash;
        transition_=s.transition;last_=now;live_=true;entered_=released_=false;return true;
    }
    bool Same(std::uint64_t epoch,const player_hiding_presentation::State& s) const noexcept {
        return live_ && epoch==epoch_ && Searchable(s) && kind_==s.kind && entity_==s.entityShortGuid &&
            hierarchy_==s.hierarchyHash && transition_==s.transition;
    }
    bool Refresh(std::uint64_t epoch,const player_hiding_presentation::State& s,std::uint64_t now) noexcept {
        if(!Same(epoch,s) || released_ || entered_)return false;
        last_=now;return true;
    }
    void Entered() noexcept {entered_=true;}
    void Release() noexcept {released_=true;}
    bool Visible(std::uint64_t now) const noexcept {
        return live_ && (entered_ || (!released_ && now>=last_ && now-last_<=350U));
    }
    bool ShouldRetire(std::uint64_t now) const noexcept {
        return live_ && (entered_ || released_ || now<last_ || now-last_>350U);
    }
private:
    std::uint64_t epoch_{},transition_{},last_{};std::uint32_t kind_{},entity_{},hierarchy_{};
    bool live_{},entered_{},released_{};
};
} // namespace aimp::alien_pvp::hiding
