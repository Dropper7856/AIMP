#pragma once

#include <cstddef>
#include <cstdint>

namespace aimp {

inline constexpr std::size_t kNativeParticipantCapacity = 8;
inline constexpr std::size_t kNativeChannelCapacity = 32;
inline constexpr float kMaximumCharacterDirectMoveDirectionMagnitude = 8.0F;

[[nodiscard]] constexpr bool ParticipantSlotAvailable(
    const std::size_t count) noexcept {
    return count < kNativeParticipantCapacity;
}

[[nodiscard]] constexpr bool ChannelSlotAvailable(
    const std::uint32_t freeMask) noexcept {
    return freeMask != 0;
}

} // namespace aimp
