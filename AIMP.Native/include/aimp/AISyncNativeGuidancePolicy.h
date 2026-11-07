#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace aimp::ai_sync::movement::native_guidance {
// Value-only observations of the certified kind1 request and its navigation
// worker. The adapter owns memory access; these rules never invent a native
// completion, clear a worker, change a pose or authorize a Host command.
struct Snapshot {
    std::uint32_t status{},phase{},execution{};
    std::uint8_t flags90{},flags91{};
    std::array<float,3> goal{},pendingGoal{};
    bool workerQuiet{},foreignBlock{},retainsBlock{},retainedReferenceAbsent{};
    // True only after two readable, nonzero pointers prove B+3C == N.sentinel.
    // Missing observations and an empty aggregate node count are not proof.
    bool tailExhausted{};
    // Positive reads of N+48/N+49, not merely an absent worker pointer.
    bool routeUpdateQuiet{};
    // Set only after the entire bounded retained ring was validated. Both
    // false means unknown/malformed, never proof that cleanup is unnecessary.
    bool retainedReferencePresent{};
};
[[nodiscard]] inline bool SameGoal(const std::array<float,3>& a,const std::array<float,3>& b) noexcept {
    for(std::size_t i=0;i<a.size();++i)
        if(std::bit_cast<std::uint32_t>(a[i])!=std::bit_cast<std::uint32_t>(b[i]))return false;
    return true;
}
[[nodiscard]] constexpr bool SchedulerEnded(const Snapshot& s) noexcept {return s.status>=3U&&s.status<=6U;}
[[nodiscard]] constexpr bool AwaitingScheduler(const Snapshot& s) noexcept {
    // 0045CE00 turns phase4/execution1 into scheduler failure and execution3
    // (including phase8) into scheduler completion. Retention preserves that
    // terminal status; it is not permission to bypass the native handoff.
    return s.status<=2U&&(s.phase==4U||s.phase==8U||s.execution==1U||s.execution==3U);
}
[[nodiscard]] constexpr bool RetainedArrival(const Snapshot& s) noexcept {
    return s.status==2U&&s.phase==3U&&!(s.flags90&0x40U)&&!(s.flags91&0x40U)&&(s.flags91&3U)==3U;
}
[[nodiscard]] constexpr bool ExhaustedThroughArrival(const Snapshot& s) noexcept {
    // A through-goal clears authored stop bit0, so local arrival can be 2,
    // not 3. 006C7550 accepts it, but 0044D320 cannot copy XYZ without a tail.
    // Do not treat flags91=2 alone as completion: live routes remain editable.
    return s.tailExhausted&&s.routeUpdateQuiet&&s.status==2U&&s.phase==3U&&s.execution==2U&&
        !(s.flags90&0x40U)&&!(s.flags91&0x40U)&&(s.flags91&3U)==2U;
}
[[nodiscard]] constexpr bool Renewable(const Snapshot& s) noexcept {
    if(!s.workerQuiet||s.foreignBlock||AwaitingScheduler(s))return false;
    // Replacement status5 can remain in the retained scheduler list, and
    // 00683DB0 does not purge that case. Never abandon its cleanup identity.
    if(s.status==5U&&!s.retainedReferenceAbsent)return false;
    return (SchedulerEnded(s)&&!s.retainsBlock)||RetainedArrival(s)||ExhaustedThroughArrival(s);
}
[[nodiscard]] constexpr bool Editable(const Snapshot& s) noexcept {
    return (s.status==1U||s.status==2U)&&s.phase==3U&&!AwaitingScheduler(s)&&
        s.workerQuiet&&!s.foreignBlock&&!(s.flags90&0x40U)&&!(s.flags91&0x40U)&&(s.flags91&3U)!=3U&&
        !ExhaustedThroughArrival(s);
}
enum class GoalResult : std::uint8_t { Waiting, Pending, Consumed, Discarded };
[[nodiscard]] inline GoalResult ObserveGoal(const Snapshot& s,const std::array<float,3>& deposited) noexcept {
    if(s.flags90&0x40U)return GoalResult::Pending;
    if(!Editable(s))return GoalResult::Waiting;
    // 0044D320 can clear pending without copying XYZ (small displacement,
    // missing tail/area, or a protected movement mode). Clear is not success.
    return SameGoal(s.goal,deposited)?GoalResult::Consumed:GoalResult::Discarded;
}
[[nodiscard]] inline bool CanConsumeGoal(const std::array<float,3>& current,const std::array<float,3>& target) noexcept {
    const auto dx=target[0]-current[0],dz=target[2]-current[2];
    return std::isfinite(dx)&&std::isfinite(dz)&&dx*dx+dz*dz>=0.1F;
}
[[nodiscard]] inline bool CoalescibleGoal(const std::array<float,3>& current,const std::array<float,3>& target) noexcept {
    float square{};
    for(std::size_t i=0;i<current.size();++i) {const auto d=target[i]-current[i];square+=d*d;}
    // The native edit threshold is horizontal, but a different floor is not
    // equivalent. Such a target waits for native end and a new factory goal.
    return std::isfinite(square)&&square<0.1F;
}
[[nodiscard]] inline bool MeaningfulTarget(const std::array<float,3>& local,const std::array<float,3>& target,
    float stopDistance,float stoppingDistance) noexcept {
    float square{};
    for(std::size_t i=0;i<local.size();++i) {
        if(!std::isfinite(local[i])||!std::isfinite(target[i]))return false;
        const auto d=target[i]-local[i];square+=d*d;
    }
    const auto radius=std::max(0.35F,std::max(stopDistance,stoppingDistance)+0.15F);
    return std::isfinite(radius)&&square>radius*radius;
}
[[nodiscard]] constexpr std::uint64_t RetryDelay(std::uint32_t attempts) noexcept {
    // Rate bounded, never a lifetime retry ceiling. Actual progress can reset
    // attempts, but an inert Client is not required to move to regain a retry.
    return 500ULL<<std::min(attempts?attempts-1U:0U,3U);
}
[[nodiscard]] constexpr bool RetryReady(std::uint64_t now,std::uint64_t last,std::uint32_t attempts) noexcept {
    return !last||(now>=last&&now-last>=RetryDelay(attempts));
}
} // namespace aimp::ai_sync::movement::native_guidance
