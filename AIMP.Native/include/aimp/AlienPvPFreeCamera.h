#pragma once
#include <array>
#include <cmath>

namespace aimp::alien_pvp_local {
// Translation follows camera heading; looking up/down never changes altitude.
inline std::array<float,3> FlightDirection(const std::array<float,3>& look,
    float yaw,float forward,float side,float up) noexcept {
    const float horizontal=std::hypot(look[0],look[2]);
    const float fx=horizontal>0.01F?look[0]/horizontal:std::sin(yaw);
    const float fz=horizontal>0.01F?look[2]/horizontal:std::cos(yaw);
    std::array<float,3> result{fx*forward+fz*side,up,fz*forward-fx*side};
    const float length=std::hypot(std::hypot(result[0],result[2]),result[1]);
    if(length>0.01F)for(auto& axis:result)axis/=length;
    return result;
}
}
