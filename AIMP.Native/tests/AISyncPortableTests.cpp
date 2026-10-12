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
    // Ownership through a real replication timeline, including rejected and
    // successfully replayed execution, pair admission and observer handoff.
    assert(ChooseMovementOwner(false, false, false, false) == MovementOwner::Intent);
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
    std::cout << "AISync ownership, navigation admission and v11 wire layout passed\n";
}
