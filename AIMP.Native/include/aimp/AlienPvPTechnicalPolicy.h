#pragma once
#include "aimp/AlienPvPSpatialPolicy.h"
#include <array>
#include <cstdint>

namespace aimp::alien_pvp::technical {
struct Identity final {
    std::uintptr_t character{},skeleton{};std::uint32_t runtime{},generation{};
    bool operator==(const Identity&) const noexcept=default;
    bool Valid() const noexcept {return character && skeleton && runtime && generation;}
};
// No raw pointer is dereferenced by this class. Native caller must revalidate
// the identity and living world before every physical write, including restore.
class Parking final {
public:
    bool Prepare(Identity id,const std::array<float,3>& home,float yaw,bool eligible) noexcept {
        if(prepared_)return eligible && id==id_;
        std::array<float,3> target{};
        if(!eligible || !id.Valid() || !std::isfinite(yaw) || !spatial::ParkingPosition(home,target))return false;
        id_=id;home_=home;target_=target;yaw_=yaw;prepared_=true;return true;
    }
    bool Target(Identity id,std::array<float,3>& out) const noexcept {
        if(!prepared_ || id!=id_)return false;
        out=target_;return true;
    }
    void Applied(Identity id) noexcept {if(prepared_ && id==id_)parked_=true;}
    bool Restore(Identity id,std::array<float,3>& out,float& yaw) const noexcept {
        if(!parked_ || id!=id_)return false;
        out=home_;out[1]+=0.05F;yaw=yaw_;return true;
    }
    bool Parked() const noexcept {return parked_;}
    void Clear() noexcept {*this={};}
private:
    Identity id_{};std::array<float,3> home_{},target_{};float yaw_{};bool prepared_{},parked_{};
};
} // namespace aimp::alien_pvp::technical
