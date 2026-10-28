#pragma once

#include "aimp/AISync.h"
#include <cstdint>
#include <cmath>
#include <string>

namespace aimp::ai_sync::navigation {

struct Counters final {
    std::uint64_t captured{}, submitted{}, updated{}, completed{}, failed{}, cancelled{}, expired{};
    std::uint64_t gaitApplied{};
    std::uint64_t directedCaptured{}, endpointsCaptured{}, directedSubmitted{};
    std::uint64_t turnCaptured{}, stanceCaptured{};
    std::uint64_t submissionsCaptured{}, submissionsDropped{};
    std::uint64_t pathGoalsCaptured{};
};
struct HeadObservation final {
    std::uint32_t count{}, typeRva{}, kind{0xFFFFFFFFU}, status{0xFFFFFFFFU};
    bool readable{};
};
// Local observation only. The portable goal owns no Ref, action or request
// address; Character/runtimeId are remapped to an Actor birth before transport.
// Read-only diagnostics. All executable navigation belongs to AISyncNativeRequestAdapter.
bool Initialize(std::uintptr_t base, std::string& error);
HeadObservation InspectHead(std::uintptr_t character) noexcept;
Counters Snapshot() noexcept; // Reserved diagnostic fields from older snapshots.
inline constexpr std::uint32_t kLiveGoal = 1U, kWorldEndpoint = 2U;
inline constexpr std::uint32_t kPointRequest = 1U, kTurnRequest = 4U, kStanceRequest = 7U,
    kDirectedRequest = 9U;
inline constexpr std::size_t kNavigationCapacity = 256U;
inline constexpr bool SameLeaseOwner(const bool actualConvergence, const std::uint32_t actualRevision,
    const bool expectedConvergence, const std::uint32_t expectedRevision) noexcept {
    return actualConvergence == expectedConvergence && actualRevision == expectedRevision;
}
inline bool HasWorldEndpoint(const NavigationGoalState& goal) noexcept {
    return (goal.flags == kLiveGoal && goal.kind == kPointRequest) ||
        (goal.flags == (kLiveGoal | kWorldEndpoint) &&
            (goal.kind == kDirectedRequest || goal.kind == kTurnRequest));
}
inline std::uint32_t LocalRequestKind(const NavigationGoalState& goal) noexcept {
    // A turn-to-point remains a turn. Only a directed MOVE with a proven
    // world endpoint is promoted to local native pathfinding.
    return goal.kind == kDirectedRequest && (goal.flags & kWorldEndpoint) != 0U
        ? kPointRequest : goal.kind;
}
inline bool SameNativeCommand(const NavigationGoalState& left, const NavigationGoalState& right) noexcept {
    const auto kind = LocalRequestKind(left);
    if (kind != LocalRequestKind(right)) return false;
    const bool samePoint = left.position[0] == right.position[0] &&
        left.position[1] == right.position[1] && left.position[2] == right.position[2];
    if (kind == kPointRequest)
        return samePoint && left.stoppingDistance == right.stoppingDistance && left.moveType == right.moveType;
    if (kind == kStanceRequest) return left.moveType == right.moveType;
    if (kind == kTurnRequest) {
        const auto endpoint = left.flags & kWorldEndpoint;
        return endpoint == (right.flags & kWorldEndpoint) && left.moveType == right.moveType &&
            left.turnTolerance == right.turnTolerance &&
            (endpoint != 0U ? samePoint : left.heading == right.heading);
    }
    // Origin identifies a new Host rebase of a direction-only command. It is
    // compared for renewal, but never written to the Client executor, which
    // rebases at its own Character. A proven world endpoint takes the point
    // branch above and therefore ignores these steering/provenance fields.
    return kind == kDirectedRequest && left.heading == right.heading &&
        left.turnTolerance == right.turnTolerance && left.distance == right.distance &&
        left.origin[0] == right.origin[0] && left.origin[1] == right.origin[1] &&
        left.origin[2] == right.origin[2];
}
inline bool ZeroPoint(const float (&point)[3]) noexcept {
    return point[0] == 0.0F && point[1] == 0.0F && point[2] == 0.0F;
}
inline bool ValidGoal(const NavigationGoalState& goal) noexcept {
    if ((goal.flags & ~(kLiveGoal | kWorldEndpoint)) != 0U || goal.moveType > 3U ||
        !std::isfinite(goal.stoppingDistance) || goal.stoppingDistance < -1.0F ||
        goal.stoppingDistance > 1000.0F ||
        (goal.flags != 0U && (goal.revision == 0U || goal.requestRevision == 0U))) return false;
    for (const auto v : goal.position)
        if (!std::isfinite(v) || std::abs(v) > 100000.0F) return false;
    for (const auto v : goal.origin)
        if (!std::isfinite(v) || std::abs(v) > 100000.0F) return false;
    if (!std::isfinite(goal.heading) || std::abs(goal.heading) > 6.4F ||
        !std::isfinite(goal.turnTolerance) || goal.turnTolerance < 0.0F || goal.turnTolerance > 6.4F ||
        !std::isfinite(goal.distance) || goal.distance < 0.0F || goal.distance > 100000.0F) return false;
    if (goal.flags == 0U)
        return goal.kind == 0U && goal.moveType == 0U && goal.stoppingDistance == 0.0F &&
            goal.heading == 0.0F && goal.turnTolerance == 0.0F && goal.distance == 0.0F &&
            goal.position[0] == 0.0F && goal.position[1] == 0.0F && goal.position[2] == 0.0F &&
            goal.origin[0] == 0.0F && goal.origin[1] == 0.0F && goal.origin[2] == 0.0F;
    if (goal.kind == kPointRequest)
        return goal.flags == kLiveGoal && goal.heading == 0.0F && goal.turnTolerance == 0.0F &&
            goal.distance == 0.0F && ZeroPoint(goal.origin);
    if (goal.kind == kStanceRequest)
        return goal.flags == kLiveGoal && goal.moveType <= 2U && goal.stoppingDistance == 0.0F &&
            goal.heading == 0.0F && goal.turnTolerance == 0.0F && goal.distance == 0.0F &&
            ZeroPoint(goal.position) && ZeroPoint(goal.origin);
    if (goal.kind == kTurnRequest)
        return (goal.flags & kLiveGoal) != 0U &&
            goal.stoppingDistance == 0.0F && goal.distance == 0.0F && ZeroPoint(goal.origin) &&
            (((goal.flags & kWorldEndpoint) != 0U && goal.heading == 0.0F) ||
                ((goal.flags & kWorldEndpoint) == 0U && ZeroPoint(goal.position)));
    if (goal.kind != kDirectedRequest || (goal.flags & kLiveGoal) == 0U) return false;
    if ((goal.flags & kWorldEndpoint) == 0U &&
        (goal.position[0] != 0.0F || goal.position[1] != 0.0F || goal.position[2] != 0.0F)) return false;
    return true;
}

} // namespace aimp::ai_sync::navigation
