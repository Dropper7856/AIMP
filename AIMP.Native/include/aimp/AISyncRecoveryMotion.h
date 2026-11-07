#pragma once

#include "aimp/AISync.h"
#include <cmath>

namespace aimp::ai_sync {
struct RecoveryControl {
    float velocityX{}, velocityZ{}, intensity{};
};

// Measured velocity controls travel magnitude. Animation intensity is a
// separate Host carrier; using speed for both couples animation demand to
// noisy displacement samples and injects speed spikes at gait transitions.
[[nodiscard]] inline bool TryBuildRecoveryControl(const State& pose, RecoveryControl& out) noexcept {
    if ((pose.motionFlags & 1U) == 0U || (pose.motionFlags & 6U) != 0U || pose.moveType > 3U ||
        !std::isfinite(pose.motionVelocity[0]) || !std::isfinite(pose.motionVelocity[2]) ||
        !std::isfinite(pose.yaw) || std::fabs(pose.yaw) > 3.3F ||
        !std::isfinite(pose.locomotionIntensity) || pose.locomotionIntensity < 0.0F ||
        pose.locomotionIntensity > 64.0F) return false;
    out = {pose.motionVelocity[0], pose.motionVelocity[2], pose.locomotionIntensity};
    return true;
}
} // namespace aimp::ai_sync
