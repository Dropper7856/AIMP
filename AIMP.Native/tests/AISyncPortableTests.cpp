#include "aimp/AISyncExecutionPolicy.h"
#include "aimp/AISyncNavigation.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <iostream>

int main() {
    using namespace aimp::ai_sync;
    for (const auto type : {1U, 2U, 3U, 4U, 6U, 7U, 9U}) assert(AICharacterClass(type));
    for (const auto type : {0U, 5U, 8U, 10U, 0xFFFFFFFFU}) assert(!AICharacterClass(type));
    // The same NPC has different local lifetime tokens on Host and Client.
    // A hit carries the Host-issued incarnation, and each side checks only
    // its own local token. Reallocation and delayed hits must fail closed.
    const std::uint32_t hostRuntime = 3U, clientRuntime = 91U, hostBirth = 7U;
    assert(ImpactTargetLifetimeMatches(hostBirth, hostBirth, clientRuntime, clientRuntime));
    assert(ImpactTargetLifetimeMatches(hostBirth, hostBirth, hostRuntime, hostRuntime));
    assert(!ImpactTargetLifetimeMatches(clientRuntime, hostBirth, hostRuntime, hostRuntime));
    assert(!ImpactTargetLifetimeMatches(6U, hostBirth, hostRuntime, hostRuntime));
    assert(!ImpactTargetLifetimeMatches(hostBirth, hostBirth, 92U, clientRuntime));
    assert(!ImpactTargetLifetimeMatches(0U, 0U, clientRuntime, clientRuntime));
    // A native stop completes locally before a same-revision Host heartbeat.
    // Its old acknowledgment must not keep navigation/steering suspended.
    assert(ExecutionCycleOwnsMovement(0x00EABEC0U, 0x3BU, 0x00EABEC0U, 0x3BU, 0U));
    assert(!ExecutionCycleOwnsMovement(0x00EABEC0U, 0x3BU, 0x00EABEC0U, 0x3BU, 2U));
    assert(!ExecutionCycleOwnsMovement(0x00EABEC0U, 0x3BU, 0x00EABEC0U, 0x3BU, 4U));
    assert(!ExecutionCycleOwnsMovement(0x00EABEC0U, 0x3BU, 0x00EABC00U, 1U, 0U));
    assert(!ExecutionCycleOwnsMovement(0U, 0U, 0U, 0U, 0U));
    assert(PairedPreparationEntry(0U) == 0x002638D0U);
    assert(PairedPreparationEntry(1U) == 0x002638D0U);
    assert(PairedPreparationEntry(4U) == 0x00263900U);
    for (const auto stage : {2U, 3U, 5U, 6U, 0xFFFFFFFFU}) assert(PairedPreparationEntry(stage) == 0U);
    // The reported Client timeline: 4E disappears before a 4F is acquired.
    assert(UnclaimedPairLost(false, false, false, false));
    assert(!UnclaimedPairLost(true, false, false, false)); // native worker owns admission
    assert(!UnclaimedPairLost(false, true, false, false)); // source still preparing
    assert(!UnclaimedPairLost(false, false, true, false)); // genuine victim graph
    assert(!UnclaimedPairLost(false, false, false, true)); // irreversible result consumed
    State stopped{};
    stopped.stateFlags = ExecutionValid;
    stopped.executionCycleTypeRva = 0x00EABEC0U;
    stopped.executionCycleState = 0x3BU;
    stopped.navigation.flags = 1U;
    assert(HostSuspendsNavigation(stopped)); // pending goal cannot undo Host stop
    stopped.executionCycleState = 0x38U;
    assert(!HostSuspendsNavigation(stopped)); // same goal resumes on new execution
    stopped.executionCycleState = 0x3BU;
    stopped.stateFlags |= Moving;
    assert(!HostSuspendsNavigation(stopped));
    // Ownership through a real replication timeline, including rejected and
    // successfully replayed execution, pair admission and observer handoff.
    assert(ChooseMovementOwner(false, false, false, false) == MovementOwner::Intent);
    // A move-to-goal is interrupted by a Host pair. Binding and victim
    // preparation may not be available until a later engine frame: motion
    // must remain reserved even before the local transition is acknowledged.
    State attack{};
    attack.stateFlags = InteractionValid;
    attack.interactionPairRevision = 7U;
    attack.interactionTransition = 1U;
    attack.interactionTransitionRevision = 9U;
    attack.interactionFlags = InteractionCycleActive | InteractionNativeOutcomeRequired;
    assert(PairAdmissionPending(attack, 0U, 0U)); // waiting for prepared outcome
    attack.outcome.revision = 1U;
    attack.outcome.flags = OutcomePrepared;
    assert(PairAdmissionPending(attack, 7U, 0U)); // pair carrier accepted, transition pending
    assert(!PairAdmissionPending(attack, 7U, 9U)); // native pair now owns execution
    attack.interactionFlags = 0U;
    assert(PairAdmissionPending(attack, 0U, 0U)); // ordered outcome still needs admission
    attack.outcome = {};
    assert(!PairAdmissionPending(attack, 0U, 0U)); // stale, inactive pair-only carrier
    attack.interactionFlags = InteractionCycleActive;
    attack.interactionTransition = 0U;
    assert(!PairAdmissionPending(attack, 0U, 0U)); // unresolved initial pair header
    attack.interactionTransition = 1U;
    attack.stateFlags = 0U;
    assert(!PairAdmissionPending(attack, 0U, 0U)); // no interaction authority
    assert(ChooseMovementOwner(false, false, false, true) == MovementOwner::Navigation);
    // Closing a goal during the old pair cannot release the neutral handoff;
    // a fresh live Host goal can, even if its resolved steering is unchanged.
    assert(!NavigationAuthorityAdvanced(0U, 11U, 8U, 10U, 7U));
    assert(!NavigationAuthorityAdvanced(1U, 10U, 7U, 10U, 7U));
    assert(NavigationAuthorityAdvanced(1U, 11U, 7U, 10U, 7U));
    assert(NavigationAuthorityAdvanced(1U, 10U, 8U, 10U, 7U));
    assert(HostLeftPairedExecution(true, false, false, false));
    assert(!HostLeftPairedExecution(true, true, false, false));
    assert(!HostLeftPairedExecution(true, false, false, true));
    assert(!HostLeftPairedExecution(true, false, true, false));
    assert(!HostLeftPairedExecution(false, false, false, false));
    assert(FindExecutionTransition(0x00EADB58U, 0x12U) == nullptr);
    assert(ChooseMovementOwner(false, false, false, true) == MovementOwner::Navigation);
    const auto* stop = FindExecutionTransition(0x00EABEC0U, 0x3BU);
    assert(stop != nullptr && stop->entryRva == 0x002625E0U && stop->ignoredStackArgument);
    assert(FindExecutionTransition(0x00EABEC0U, 0x12U) == nullptr);
    assert(ChooseMovementOwner(false, false, true, true) == MovementOwner::Execution);
    assert(ChooseMovementOwner(true, false, true, true) == MovementOwner::Paired);
    assert(ChooseMovementOwner(false, true, true, true) == MovementOwner::Neutral);
    assert(ChooseMovementOwner(false, false, false, true) == MovementOwner::Navigation);
    NavigationGoalState goal{9U, 1U, {2.0F, 3.0F, -4.0F}, 1.0F, 2U, 7U};
    assert(navigation::ValidGoal(goal));
    auto invalid = goal; invalid.revision = 0U; assert(!navigation::ValidGoal(invalid));
    invalid = goal; invalid.requestRevision = 0U; assert(!navigation::ValidGoal(invalid));
    invalid = goal; invalid.flags = 2U; assert(!navigation::ValidGoal(invalid));
    invalid = goal; invalid.moveType = 4U; assert(!navigation::ValidGoal(invalid));
    invalid = goal; invalid.stoppingDistance = -2.0F; assert(!navigation::ValidGoal(invalid));
    invalid = goal; invalid.stoppingDistance = std::numeric_limits<float>::quiet_NaN();
    assert(!navigation::ValidGoal(invalid));
    invalid = goal; invalid.position[1] = std::numeric_limits<float>::infinity();
    assert(!navigation::ValidGoal(invalid));
    invalid = goal; invalid.position[0] = 100001.0F; assert(!navigation::ValidGoal(invalid));
    goal.flags = 0U; goal.revision = goal.requestRevision = 0U;
    assert(navigation::ValidGoal(goal)); // no active native request
    static_assert(sizeof(State) == 288U && offsetof(State, navigation) == 256U);
    static_assert(offsetof(State, outcome) == 192U && offsetof(State, executionRevision) == 240U);
    static_assert(sizeof(ImpactEvent) == 64U && ImpactEvent{}.version == 2U);
    std::cout << "AISync ownership, navigation admission and v11 wire layout passed\n";
}
