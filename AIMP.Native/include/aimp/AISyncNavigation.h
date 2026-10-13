#pragma once

#include "aimp/AISync.h"
#include <cstdint>
#include <cmath>
#include <string>

namespace aimp::ai_sync::navigation {

struct Sample final {
    NavigationGoalState goal{};
    std::uintptr_t request{}; // local observation only
};
struct Counters final {
    std::uint64_t captured{}, submitted{}, updated{}, completed{}, failed{}, cancelled{}, expired{};
    std::uint64_t gaitApplied{};
};

// Engine-thread only. References are owned independently of Character lifetime.
bool Initialize(std::uintptr_t base, std::string& error);
Sample Capture(std::uintptr_t character) noexcept;
bool Apply(std::uintptr_t character, const NavigationGoalState& goal,
    std::uint64_t expiresAt, std::string& error) noexcept;
// Retail movement producers author this separately from their Move request.
// Keep local pathfinding/steering; replicate only the Host-selected gait.
bool ApplyGait(std::uintptr_t character, std::uint32_t moveType) noexcept;
void Cancel(std::uintptr_t character) noexcept;
void Reset(bool cancelNative) noexcept;
void Pump(std::uint64_t now) noexcept;
Counters Snapshot() noexcept;
inline bool ValidGoal(const NavigationGoalState& goal) noexcept {
    if ((goal.flags & ~1U) != 0U || goal.moveType > 3U ||
        !std::isfinite(goal.stoppingDistance) || goal.stoppingDistance < -1.0F ||
        goal.stoppingDistance > 1000.0F ||
        (goal.flags != 0U && (goal.revision == 0U || goal.requestRevision == 0U))) return false;
    for (const auto v : goal.position)
        if (!std::isfinite(v) || std::abs(v) > 100000.0F) return false;
    return true;
}

} // namespace aimp::ai_sync::navigation
