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
    // Pursuit (2), local rejoin (1), and Host route (non-convergence) can
    // replace each other in one tick. Old cleanup must leave the successor alive.
    static_assert(navigation::SameLeaseOwner(true, 2U, true, 2U));
    static_assert(!navigation::SameLeaseOwner(true, 1U, true, 2U));
    static_assert(!navigation::SameLeaseOwner(false, 2U, true, 2U));
    static_assert(!navigation::SameLeaseOwner(true, 2U, true, 1U));
    static_assert(!AlienSuppressionRequested(Role::Host, 0U));
    static_assert(AlienSuppressionRequested(Role::Host, kDisableAlien));
    static_assert(!AlienSuppressionRequested(Role::Retail, kDisableAlien));
    static_assert(!AlienSuppressionRequested(Role::Client, kDisableAlien));
    static_assert(!AlienSuppressionRequested(Role::FrozenClient, kDisableAlien));
    ParticipantRouteRetry routeRetry{};
    assert(!routeRetry.Observe(true, false, false, 0U, true, 100U));
    assert(!routeRetry.Observe(true, false, false, 0U, true, 2099U));
    assert(routeRetry.Observe(true, false, false, 0U, true, 2100U));
    // A long outstanding request must never be cancelled/restarted. Once it
    // completes empty, the retry delay starts anew rather than expiring in flight.
    assert(!routeRetry.Observe(true, true, false, 0U, true, 10000U));
    assert(!routeRetry.Observe(true, false, false, 0U, true, 10001U));
    assert(routeRetry.Observe(true, false, false, 0U, true, 12001U));
    assert(!routeRetry.Observe(true, false, false, 12U, true, 15000U));
    assert(!routeRetry.Observe(true, false, true, 0U, true, 20000U));
    assert(!routeRetry.Observe(false, false, false, 0U, true, 25000U));
    assert(!routeRetry.Observe(true, false, false, 0U, false, 30000U));
    State relocated{};
    relocated.lifecycleFlags = LifecycleSpatialDiscontinuity;
    relocated.lifecycleRevision = 7U;
    assert(LifecycleBlocksMovementRecovery(relocated, 6U));
    assert(!LifecycleBlocksMovementRecovery(relocated, 7U));
    // Repeated snapshots retain the event flag after placement. A new event
    // must block again; an ongoing traversal never grants route ownership.
    relocated.lifecycleRevision = 8U;
    assert(LifecycleBlocksMovementRecovery(relocated, 7U));
    relocated.lifecycleFlags |= LifecycleTraversalAction;
    assert(LifecycleBlocksMovementRecovery(relocated, 8U));
    relocated.lifecycleFlags = 0U;
    assert(!LifecycleBlocksMovementRecovery(relocated, 7U));
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
    stopped.navigation.kind = navigation::kPointRequest;
    assert(HostSuspendsNavigation(stopped)); // pending goal cannot undo Host stop
    stopped.executionCycleState = 0x38U;
    assert(!HostSuspendsNavigation(stopped)); // same goal resumes on new execution
    stopped.executionCycleState = 0x3BU;
    stopped.stateFlags |= Moving;
    assert(!HostSuspendsNavigation(stopped));
    // A physical locomotion stop is not a cancellation of a separately
    // admitted turn/posture decision. These requests own their retail cycles.
    stopped.stateFlags &= ~Moving;
    stopped.navigation.kind = navigation::kTurnRequest;
    assert(!HostSuspendsNavigation(stopped));
    stopped.navigation.kind = navigation::kStanceRequest;
    assert(!HostSuspendsNavigation(stopped));
    stopped.navigation.kind = navigation::kDirectedRequest;
    assert(HostSuspendsNavigation(stopped));
    // A live point goal with an idle/stopped Host Move executor pauses the
    // Client proxy, but only a Move cycle: turn/stance/other cycles keep ownership.
    State paused{};
    paused.stateFlags = ExecutionValid;
    paused.executionCycleTypeRva = 0x00EABF80U;
    paused.executionCycleState = 0x38U;
    paused.navigation.flags = 1U;
    paused.navigation.kind = navigation::kPointRequest;
    assert(HostPausesPointNavigation(paused));
    paused.stateFlags |= Moving;
    paused.locomotionIntensity = 1.0F;
    assert(!HostPausesPointNavigation(paused)); // Host is genuinely moving
    paused.locomotionIntensity = 0.0F;
    assert(HostPausesPointNavigation(paused)); // Moving flag without intensity
    paused.navigation.kind = navigation::kTurnRequest;
    assert(!HostPausesPointNavigation(paused));
    paused.navigation.kind = navigation::kDirectedRequest;
    paused.navigation.flags = 3U; // directed MOVE with a world endpoint
    assert(HostPausesPointNavigation(paused));
    paused.executionCycleState = 0x3BU;
    assert(!HostPausesPointNavigation(paused));
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
    assert(NavigationAuthorityAdvanced(3U, 11U, 7U, 10U, 7U));
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
    goal.kind = navigation::kPointRequest;
    assert(navigation::ValidGoal(goal));
    assert(navigation::HasWorldEndpoint(goal));
    assert(navigation::LocalRequestKind(goal) == navigation::kPointRequest);
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
    // Closing a request clears its payload. A stale endpoint under flags=0
    // must not be reinterpreted as a new native command after an IPC stall.
    invalid = goal; invalid.flags = 0U;
    assert(!navigation::ValidGoal(invalid));
    NavigationGoalState inactive{};
    inactive.revision = 12U; inactive.requestRevision = 8U;
    assert(navigation::ValidGoal(inactive));
    assert(!navigation::HasWorldEndpoint(inactive));
    NavigationGoalState directed{};
    directed.revision = 17U; directed.requestRevision = 9U;
    directed.flags = navigation::kLiveGoal; directed.kind = navigation::kDirectedRequest;
    directed.heading = 0.4F; directed.turnTolerance = 1.5707964F;
    directed.distance = 6.0F; directed.origin[0] = 12.0F; directed.origin[2] = -8.0F;
    assert(navigation::ValidGoal(directed));
    assert(!navigation::HasWorldEndpoint(directed));
    assert(navigation::LocalRequestKind(directed) == navigation::kDirectedRequest);
    // Host-derived heading/distance is not a portable XYZ vector. A world
    // endpoint exists only after the upstream producer supplied all 3 axes.
    invalid = directed; invalid.position[0] = 1.0F;
    assert(!navigation::ValidGoal(invalid));
    directed.flags |= navigation::kWorldEndpoint;
    directed.position[0] = 25.0F; directed.position[1] = 4.0F; directed.position[2] = -11.0F;
    directed.stoppingDistance = 0.1F; directed.moveType = 2U;
    assert(navigation::ValidGoal(directed));
    assert(navigation::HasWorldEndpoint(directed));
    assert(navigation::LocalRequestKind(directed) == navigation::kPointRequest);
    invalid = directed; invalid.origin[2] = std::numeric_limits<float>::quiet_NaN();
    assert(!navigation::ValidGoal(invalid));
    invalid = directed; invalid.flags = navigation::kWorldEndpoint;
    assert(!navigation::ValidGoal(invalid));
    invalid = directed; invalid.distance = -1.0F;
    assert(!navigation::ValidGoal(invalid));
    NavigationGoalState turn{};
    turn.revision = 1U; turn.requestRevision = 4U;
    turn.flags = navigation::kLiveGoal; turn.kind = navigation::kTurnRequest;
    turn.heading = 2.0F; turn.turnTolerance = 0.3F;
    // Native +34 style and +38 aim flag are two value bits, independent of gait.
    turn.moveType = 3U;
    assert(navigation::ValidGoal(turn));
    assert(navigation::LocalRequestKind(turn) == navigation::kTurnRequest);
    turn.flags |= navigation::kWorldEndpoint; turn.heading = 0.0F;
    turn.position[0] = -3.0F; turn.position[2] = 7.0F;
    assert(navigation::ValidGoal(turn));
    assert(navigation::HasWorldEndpoint(turn));
    // A look-at-point must never become a move-to-point simply because it has XYZ.
    assert(navigation::LocalRequestKind(turn) == navigation::kTurnRequest);
    invalid = turn; invalid.heading = 1.0F;
    assert(!navigation::ValidGoal(invalid));
    invalid = turn; invalid.turnTolerance = std::numeric_limits<float>::infinity();
    assert(!navigation::ValidGoal(invalid));
    NavigationGoalState stance{};
    stance.revision = 1U; stance.requestRevision = 5U;
    stance.flags = navigation::kLiveGoal; stance.kind = navigation::kStanceRequest;
    for (const auto posture : {0U, 1U, 2U}) {
        stance.moveType = posture;
        assert(navigation::ValidGoal(stance));
        assert(navigation::LocalRequestKind(stance) == navigation::kStanceRequest);
    }
    invalid = stance; invalid.moveType = 3U;
    assert(!navigation::ValidGoal(invalid));
    invalid = stance; invalid.flags |= navigation::kWorldEndpoint;
    assert(!navigation::ValidGoal(invalid));
    invalid = stance; invalid.distance = 1.0F;
    assert(!navigation::ValidGoal(invalid));
    invalid = goal; invalid.kind = 2U; // native path-handle layout is not a point request
    assert(!navigation::ValidGoal(invalid));
    // A completed posture is not re-created by a metadata-only revision.
    auto repeated = stance; repeated.revision += 1U;
    assert(navigation::SameNativeCommand(stance, repeated));
    auto endpoint = goal; endpoint.kind = 9U; endpoint.flags = 3U;
    repeated = endpoint; repeated.heading = 1.0F; repeated.origin[0] = 2.0F;
    assert(navigation::SameNativeCommand(endpoint, repeated));
    repeated.position[0] += 1.0F;
    assert(!navigation::SameNativeCommand(endpoint, repeated));
    endpoint.flags = 1U; endpoint.position[0] = endpoint.position[1] = endpoint.position[2] = 0.0F;
    repeated = endpoint; repeated.origin[0] = 2.0F;
    assert(!navigation::SameNativeCommand(endpoint, repeated));
    health::State alive{1U, health::Present, 100U, 1U};
    assert(health::Valid(alive) && !health::Terminal(alive));
    auto dead = alive; dead.hp = 0U; dead.healthState = 4U;
    assert(health::Valid(dead) && health::Terminal(dead));
    auto invalidHealth = dead; invalidHealth.healthState = 1U;
    assert(!health::Valid(invalidHealth));
    invalidHealth = alive; invalidHealth.hp = 65536U;
    assert(!health::Valid(invalidHealth));
    health::DamageEvent hit{};
    hit.revision = 1U; hit.flags = health::Present; hit.damage = 100U;
    hit.hpBefore = 100U; hit.healthStateBefore = 1U; hit.result = dead;
    assert(health::Valid(hit));
    hit.flags |= health::CommitOnly; assert(!health::Valid(hit));
    hit.damage = 0U; assert(health::Valid(hit));
    hit.flags = 0U; assert(!health::Valid(hit)); // inactive records cannot hide a hit
    combat::State aim{};
    combat::State holstered{4U}; assert(combat::Valid(holstered));
    combat::ShotEvent inactiveShot{}; inactiveShot.state = holstered;
    assert(!combat::Valid(inactiveShot));
    aim.revision = 1U; aim.flags = combat::Present | combat::AimPoint | combat::Aiming;
    aim.slot = 1U; aim.descriptor = 2U; aim.aim[0] = 4.0F;
    combat::ShotEvent shot{1U, 1U, aim, 6U};
    assert(combat::Valid(aim) && combat::Valid(shot));
    shot.ammoBefore = 0U; assert(!combat::Valid(shot));
    aim.flags = combat::Present | combat::Aiming; assert(!combat::Valid(aim));
    static_assert(navigation::kNavigationCapacity >= 256U);
    static_assert(sizeof(NavigationGoalState) == 60U && offsetof(State, navigation) == 256U);
    static_assert(sizeof(State) == 640U && State{}.version == 12U);
    static_assert(offsetof(State, outcome) == 192U && offsetof(State, executionRevision) == 240U);
    static_assert(sizeof(ImpactEvent) == 64U && ImpactEvent{}.version == 2U);
    static_assert(offsetof(State, health) == 316U && offsetof(State, damage) == 356U);
    std::cout << "AISync ownership, typed navigation, health/combat and v12 wire layout passed\n";
}
