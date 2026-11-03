#pragma once
#include <cstddef>

namespace aimp::replication_core {
inline constexpr std::size_t kWorldIntentQueueCapacity = 4096U;

// Publish only boundaries already fixed by the historical end-of-turn split:
// a full max-sized segment, or a group followed by a different causal group.
// The final short group stays reserved until more roots or end-of-turn.
template<class Roots, class SameGroup>
std::size_t StableCausalPublicationPrefix(const Roots& roots, std::size_t maximumRoots,
    SameGroup sameGroup) noexcept {
    if (maximumRoots == 0U) return 0U;
    std::size_t stable = 0U;
    for (std::size_t start = 0U; start < roots.size();) {
        auto end = start + 1U;
        while (end < roots.size() && end - start < maximumRoots && sameGroup(roots[start], roots[end])) ++end;
        if (end == roots.size() && end - start < maximumRoots) break;
        stable = end;
        start = end;
    }
    return stable;
}
}
