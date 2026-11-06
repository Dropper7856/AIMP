#pragma once
#include <cmath>
#include <cstdint>

namespace aimp::alien_pvp_movement {

// Pure portable math, unit tested independently of retail AI.exe. Directions
// are world-space and unit length; only the native locomotion runtime turns
// them into Character animation/collision.
struct Plan final {
    float worldX{}, worldZ{}, intensity{};
    std::uint32_t gait{};
};
inline bool TryPlan(const int forward,const int side,const float yaw,
    const std::uint32_t gait,Plan& output) noexcept {
    output={};
    if (forward<-1 || forward>1 || side<-1 || side>1 ||
        !std::isfinite(yaw) || yaw<-3.3F || yaw>3.3F || gait>3U ||
        (forward==0 && side==0)) return false;
    const auto length=std::hypot(static_cast<float>(forward),static_cast<float>(side));
    if (length<0.1F) return false;
    const auto f=static_cast<float>(forward)/length;
    const auto s=static_cast<float>(side)/length;
    output.worldX=std::sin(yaw)*f+std::cos(yaw)*s;
    output.worldZ=std::cos(yaw)*f-std::sin(yaw)*s;
    output.gait=gait;
    output.intensity=gait==3U?2.2F:(gait==2U?1.6F:(gait==0U?0.6F:1.1F));
    return std::isfinite(output.worldX) && std::isfinite(output.worldZ);
}
} // namespace aimp::alien_pvp_movement
