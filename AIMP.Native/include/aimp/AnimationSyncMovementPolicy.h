#pragma once
#include <cstdint>

namespace aimp::animation_sync {

// The observer's Character owns one scripted playback at a time. Native
// full-body clips keep the retail translation/rotation; only FP fallback
// clips chase the player's replicated Character path. Locker clips have an
// independent render-only path and never physically chase the owner.
enum class MovementPolicy : std::uint8_t {
    None = 0U,
    NativeRoot = 1U,
    FirstPersonPath = 2U,
    LockerVisual = 3U,
};
[[nodiscard]] constexpr MovementPolicy SelectMovementPolicy(
    const bool ownsCycle, const bool nativeClip, const bool lockerClip) noexcept {
    if (!ownsCycle) return MovementPolicy::None;
    if (lockerClip) return MovementPolicy::LockerVisual;
    return nativeClip ? MovementPolicy::NativeRoot : MovementPolicy::FirstPersonPath;
}
[[nodiscard]] constexpr bool FollowNetworkPath(const MovementPolicy policy) noexcept {
    return policy == MovementPolicy::FirstPersonPath;
}
}
