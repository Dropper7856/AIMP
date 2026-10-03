#include "aimp/AISync.h"
#include "aimp/BuildProfile.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <type_traits>

namespace aimp::ai_sync {
namespace {
static_assert(sizeof(void*) == 4U);
static_assert(std::is_trivially_copyable_v<State>);
constexpr std::size_t kActors = 32U, kRing = 256U;
constexpr ULONGLONG kSendPeriod = 50ULL, kRefreshPeriod = 250ULL, kTimeout = 1500ULL;
constexpr ULONGLONG kManagerDormancyPeriod = 500ULL;
constexpr ULONGLONG kDeadTargetGracePeriod = 1500ULL;
constexpr std::uint32_t kStateFlags = PoseValid | Moving | ActionValid | InteractionValid | RoleTaskValid;
constexpr std::uint32_t kInteractionFlags = InteractionActorIsSource |
    InteractionTargetIsParticipant | InteractionPairValid | InteractionCycleActive;
constexpr std::uint32_t kLifecycleFlags = LifecycleSpatialDiscontinuity | LifecycleTraversalAction |
    LifecycleWithdrawn | LifecycleWithdrawState2 | LifecycleHostDormant;
constexpr float kLifecycleDiscontinuityDistance = 3.5F;
constexpr float kLifecycleVerticalDiscontinuity = 2.5F;
constexpr std::uintptr_t kPositionOffset = 0x70U, kYawOffset = 0x174U;
constexpr std::uintptr_t kInteractionManagerGlobalRva = 0x012F0C88U;
constexpr std::uintptr_t kRoleTaskCleanupRva = 0x00361A90U;
constexpr auto kCharacterRegistry = character_direct_step_runtime::NativeCharacterRegistry();
constexpr float kMotionThreshold = 0.02F, kMaximumVelocity = 20.0F;
using ManagerUpdate = void(__thiscall*)(void*, std::uint32_t, float);
using Coordinator = void(__thiscall*)(void*, std::uint32_t);
using CharacterInteraction = std::uint32_t(__thiscall*)(void*, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint32_t, std::uint64_t, std::int32_t);
using PairedTransitionA = void(__thiscall*)(void*, std::uintptr_t, std::int32_t, std::uint32_t);
using PairedTransitionB = void(__thiscall*)(void*, std::uintptr_t, std::int32_t,
    std::uint32_t, std::uint32_t);
using CharacterImpact = void(__thiscall*)(void*, int*);
using SensesCandidates = void(__thiscall*)(void*);
using SenseEvent = void(__thiscall*)(void*, std::uintptr_t);
using SenseCharacterResolver = std::uintptr_t(__thiscall*)(void*, const std::uint32_t*);
// FUN_0065FB80: ECX = action cycle, two stack arguments, RET 8, AL result.
using PairedInitialize = std::uint8_t(__thiscall*)(void*, const std::uint32_t*, std::int32_t);
// Victim transition 0065C450 takes the source Character, parameters and source
// cycle on the stack (RET 0C). Victim initializer 00675F10 takes ECX only.
using PairedVictimTransition = void(__thiscall*)(void*, std::uintptr_t, const void*, std::uintptr_t);
using PairedVictimInitialize = void(__thiscall*)(void*);
// 0064E6C0: ECX = source 4E cycle, target Character and victim flag, RET 8.
using PairedVictimConvergence = void(__thiscall*)(void*, std::uintptr_t, std::uint32_t);
// Native message 17 handler: ECX = original 0x40-byte packet, Character argument,
// RET 4. Keep the packet produced by 00662670, including native convergence.
using PairedVictimEvent = void(__thiscall*)(const void*, void*);
using CharacterMessage = void(__thiscall*)(void*, const void*);
using Hostility = std::uint32_t(__thiscall*)(void*, std::uintptr_t);
using ThePlayerResolver = std::uintptr_t(__cdecl*)();
// Both certified functions take ECX only and use plain RET. The record
// validator returns AL (true means discard), not a defined full EAX.
using TargetEligibility = std::uint32_t(__thiscall*)(void*);
using RecordInvalid = std::uint8_t(__thiscall*)(void*);
constexpr ULONGLONG kParticipantProbePeriod = 1000ULL;
constexpr std::size_t kParticipantProbeRing = 256U;
struct ParticipantProbe final {
    std::uint64_t sessionCookie{}, peer{}, selectedPeer{}, tick{};
    std::uint32_t nativeGeneration{}, worldGeneration{}, entityA{}, entityB{}, characterClass{};
    std::uint32_t retailEligibility{}, executionStateA{}, executionStateB{}, executionPhase{};
    std::uint32_t activeSenseMask{}, primarySenseIndex{0xFFFFFFFFU}, recentSenseIndex{0xFFFFFFFFU};
    std::uint32_t threatState{0xFFFFFFFFU};
    std::uint8_t executionEnabled{}, recordActive{}, recordSelected{}, recordForced{};
    std::uint8_t ownerEnabled{}, ownerSenseBlocked{}, sensesDisabled{};
    std::uint32_t ownerRetailEligibility{};
    bool ownerExecutionReadable{}, ownerGatesReadable{};
    bool living{}, pairReadable{}, paired{}, executionReadable{}, sensesReadable{}, recordPresent{};
    bool recordReadable{}, selectedReadable{}, selectedPresent{}, threatReadable{}, threatPresent{};
    float sightInput{}, sightTimestamp{};
    bool signalRegistryReadable{}, signalRegistryFound{}, signalRegistryMatches{};
    bool visibilityFlagsReadable{};
    std::uint8_t ownerHidden{}, targetHidden{};
};
// Engine-thread producer, serialized worker consumers. Samples hold values,
// never pointers to Characters/records that a later worker might dereference.
std::array<ParticipantProbe, kParticipantProbeRing> gParticipantProbes{};
std::atomic<std::uint32_t> gParticipantProbeWrite{}, gParticipantProbeRead{};
std::mutex gParticipantProbeConsumerMutex;
struct Actor {
    std::uintptr_t character{}, tree{}, lastRoleTask{};
    std::uint32_t runtimeId{}, entityA{}, entityB{}, characterClass{}, birth{};
    State desired{}, previous{};
    ULONGLONG lastSend{}, lastReceive{}, lastManagerUpdate{};
    ULONGLONG lastPoseSample{}, lastProxyRefresh{}, lastInteractionAttempt{}, lastInteractionCapture{};
    ULONGLONG deadTargetSince{}, stationarySince{}, lastParticipantProbe{};
    std::uint32_t participantProbeCursor{};
    std::uint32_t completedRevision{}, completedInteractionRevision{};
    std::uint32_t completedActionRevision{}, completedRoleTaskRevision{}, completedLifecycleRevision{};
    std::uint32_t interactionActionRevision{}, interactionActionPathHash{};
    std::uint32_t completedPairRevision{}, completedTransitionRevision{}, recoveredInteractionRevision{};
    std::uint32_t resolvedPairRevision{}, localPairFlagsLow{}, localPairFlagsHigh{};
    std::int32_t localPairAttachment{-1};
    std::uintptr_t resolvedPairTarget{};
    std::int32_t resolvedSourceVariant{};
    std::uint32_t remoteBirth{}, remoteRevision{};
    float lastPosition[3]{};
    bool blocked{}, duplicate{}, faulted{}, hasLastPosition{}, interactionOwnsCycle{};
    bool traversalAction{}, hostDormant{}, withdrawKnown{};
    std::uint32_t withdrawState{};
};
std::array<Actor, kActors> gActors{}; // engine thread only
// Engine-published identities for retail worker callbacks. Every field is
// atomic: a sequence check over plain fields would still be a C++ data race.
struct PairBinding final {
    std::atomic<std::uint32_t> sequence{}, runtimeId{}, entityA{}, entityB{}, type{}, generation{}, epoch{};
    std::atomic<std::uintptr_t> character{}, animated{};
    std::atomic<std::uint64_t> peer{};
};
struct PairIdentity final {
    std::uintptr_t character{}, animated{};
    std::uint32_t runtimeId{}, entityA{}, entityB{}, type{}, generation{}, epoch{};
    std::uint64_t peer{};
};
std::array<PairBinding, kActors> gPairSources{};
std::array<PairBinding, kActors + 1U> gPairParticipants{};
std::array<std::atomic<bool>, kActors> gPairSourceBusy{};
std::atomic<std::uint32_t> gPairBindingEpoch{1U};
thread_local std::uintptr_t gVictimAdmissionSource{}, gVictimAdmissionTarget{}, gVictimAdmissionCycle{};
thread_local std::uint32_t gVictimAdmissionDepth{};
struct NativeVictimSubmission final {
    alignas(16) std::array<std::byte, 0x40U> packet{};
    std::uintptr_t sourceCycle{}, source{}, victim{};
    PairIdentity victimBefore{};
    std::uint8_t sourceSerial{};
    bool livingBefore{}, captured{};
};
thread_local NativeVictimSubmission* gNativeVictimSubmission{};
// NPC-backed Characters created by AIMP represent remote human players. Their
// addresses are known only after CharacterCreate returns, while their runtime
// spawn keys are known before construction and close the re-entrant admission
// window inside the retail constructor.
std::array<std::atomic<std::uintptr_t>, kActors> gOwned{};
std::array<std::atomic<std::uint64_t>, kActors> gOwnedGeneration{};
std::array<std::atomic<std::uint32_t>, kActors> gOwnedSpawnKeys{};
std::array<std::atomic<std::uint64_t>, kActors> gOwnedSpawnKeyGeneration{};
std::array<State, kRing> gOutgoing{}; // single engine-thread producer, IPC consumer
std::atomic<std::uint32_t> gWrite{}, gRead{};
std::mutex gMailboxMutex;
Configuration gRequested{};
bool gConfigurationPending{};
std::array<State, kActors> gIncoming{};
std::array<bool, kActors> gIncomingUsed{};
std::array<ULONGLONG, kActors> gIncomingAt{};
std::array<ULONGLONG, kActors> gNextIdentityProbe{};
Configuration gCurrent{}; // engine thread only
std::atomic<Role> gRole{Role::Retail};
std::atomic<bool> gInstalled{}, gStopping{};
std::atomic<void*> gUpdateOriginal{}, gCoordinatorOriginal{}, gInteractionOriginal{};
std::atomic<void*> gPairedTransitionAOriginal{}, gPairedTransitionBOriginal{};
std::atomic<void*> gCharacterImpactOriginal{}, gCharacterImpactSecondaryOriginal{};
std::atomic<void*> gPrepassOriginal{};
std::atomic<void*> gCandidatesOriginal{}, gHostilityOriginal{}, gThePlayerOriginal{};
std::atomic<void*> gRecordInvalidOriginal{};
std::atomic<void*> gSenseEventOriginal{}, gSoundEventOriginal{};
std::atomic<void*> gPairedInitializeOriginal{};
void* gPairedInitializeTrampoline{};
std::atomic<void*> gPairedVictimTransitionOriginal{}, gPairedVictimInitializeOriginal{};
void* gPairedVictimTransitionTrampoline{}, *gPairedVictimInitializeTrampoline{};
std::atomic<void*> gPairedVictimEventOriginal{}, gPairedVictimSubmitPublished{};
void* gPairedVictimEventTrampoline{}, *gPairedVictimSubmitTrampoline{}, *gPairedVictimSubmitContinue{};
std::atomic<void*> gPairedVictimPrepareOriginal{};
void* gPairedVictimPrepareTrampoline{};
void* gSenseEventTrampoline{}, *gSoundEventTrampoline{};
void* gRecordInvalidTrampoline{};
void* gCandidatesTrampoline{}, *gHostilityTrampoline{}, *gThePlayerTrampoline{};
void* gPrepassTrampoline{};
void* gUpdateTrampoline{}, *gCoordinatorTrampoline{}, *gInteractionTrampoline{};
void* gPairedTransitionATrampoline{}, *gPairedTransitionBTrampoline{};
void* gCharacterImpactTrampoline{}, *gCharacterImpactSecondaryTrampoline{};
void* gBranchTrampoline{}, *gBranchExit{};
std::atomic<void*> gBranchPublished{};
std::atomic<std::uint32_t> gShutdownEngineThread{};
std::uintptr_t gBase{};
LevelGenerationTracker* gGenerations{};
GameThreadDispatcher* gDispatcher{};
std::uint64_t gGeneration{};
std::uint32_t gNextBirth{1U};
std::uint32_t gLivingPlayerAlliance{};
bool gLivingPlayerAllianceKnown{};
thread_local Actor* gUpdating{};
// Only AI execution scopes set this. It prevents the multiplayer compatibility
// resolver from changing THE_PLAYER for camera/UI/inventory/mission code.
thread_local std::uintptr_t gAiPlayerQueryOwner{};
// Only the certified sense-event dispatchers set this scope. CATHODE's
// global entity resolver and unrelated callers keep their retail semantics.
thread_local std::uintptr_t gSenseEventOwner{};
// Only the common native paired-cycle initializer opens this exact victim
// scope. The two redirected resolver CALLs retain their distinct return types.
thread_local std::uintptr_t gPairedResolveOwner{};
thread_local const std::uint32_t* gPairedResolveIdentity{};
// Exact callers supply the owner; a record also supplies its exact candidate.
// Do not adapt unrelated lifecycle/animation/BT uses of FUN_0078B380.
thread_local std::uintptr_t gCandidateEligibilityOwner{};
thread_local std::uintptr_t gRecordEligibilityOwner{}, gRecordEligibilityTarget{};
// Some retail AI paths bypass FUN_0078BAF0 and read the first player slot
// directly. During a Host AI callback only, keep AISync's own identity logic
// anchored to the real local player while the retail slot can be projected to
// a living multiplayer participant. Nested AI callbacks restore stack-wise.
thread_local std::uintptr_t gAiPlayerRealLocal{};
thread_local std::uint32_t gAiPlayerContextDepth{};
std::atomic<std::uint64_t> gObserved{}, gApplied{}, gDropped{}, gRefused{}, gSuspended{}, gFaults{};
std::atomic<std::uint64_t> gMissing{};
std::atomic<std::uint64_t> gManagerUpdates{}, gAdmitted{}, gIdentityRejected{}, gTreeUnavailable{}, gOwnerMismatch{};
std::atomic<std::uint64_t> gRemoteQueued{};
std::atomic<std::uint64_t> gPoseCaptured{}, gPoseApplied{}, gPoseRejected{};
std::atomic<std::uint64_t> gActionCaptured{}, gActionTransitions{};
std::atomic<std::uint64_t> gRoleTaskCaptured{}, gRoleTaskTransitions{};
std::atomic<std::uint64_t> gLifecycleCaptured{}, gLifecycleApplied{}, gLifecycleSnaps{}, gExecutionBarriers{};
std::atomic<std::uint64_t> gWithdrawSamples{}, gWithdrawTransitions{}, gWithdrawApplied{};
std::atomic<std::uint64_t> gInteractionCaptured{}, gInteractionApplied{}, gInteractionRejected{};
std::atomic<std::uint64_t> gInteractionRemapped{};
std::atomic<std::uint64_t> gPairedTransitionCaptured{}, gPairedTransitionApplied{};
std::atomic<std::uint64_t> gPlayerRepresentationsRegistered{};
std::atomic<std::uint64_t> gProvisionalPlayerActorsRetired{};
std::atomic<std::uint64_t> gParticipantCandidates{}, gParticipantTargetRejected{}, gDeadTargetRecoveries{};
std::atomic<std::uint64_t> gParticipantCandidatePasses{};
std::atomic<std::uint64_t> gSenseEventPasses{};
std::atomic<std::uint64_t> gParticipantSignalLookups{};
std::atomic<std::uint64_t> gParticipantSignalRemaps{};
std::atomic<std::uint64_t> gParticipantSignalRetailMissing{};
std::atomic<std::uint64_t> gParticipantSignalRetailMismatch{};
std::atomic<std::uint64_t> gParticipantSignalAmbiguous{};
std::atomic<std::uint64_t> gLastParticipantSignalPeer{};
std::atomic<std::uint64_t> gParticipantPairLookups{}, gParticipantPairRemaps{};
std::atomic<std::uint64_t> gParticipantPairRetailMissing{}, gParticipantPairAmbiguous{};
std::atomic<std::uint64_t> gLastParticipantPairPeer{};
std::atomic<std::uint64_t> gPairedInitializeSucceeded{}, gPairedInitializeFailed{};
std::atomic<std::uint64_t> gPairedCycleReleased{};
std::atomic<std::uint64_t> gPairedVictimTransitions{}, gPairedVictimTransitionRejected{};
std::atomic<std::uint64_t> gPairedVictimInitializeAborted{};
std::atomic<std::uint64_t> gPairedVictimEventsRejected{}, gPairedVictimDuplicates{}, gPairedVictimBusy{};
std::atomic<std::uint64_t> gPairedVictimNativeDispatches{}, gPairedVictimNativeStarted{};
std::atomic<std::uint64_t> gPairedVictimAvailabilityOverrides{};
std::atomic<std::uint64_t> gPairedVictimLocalDispatches{}, gPairedVictimLocalStarted{};
std::atomic<std::uint64_t> gPairedVictimConvergenceDetached{}, gPairedVictimSnapshotsDeferred{};
std::atomic<std::uint32_t> gLastRejectedPairedReason{};
std::atomic<std::uint64_t> gPairedVictimHealthContinuations{}, gPairedVictimWorkerDispatches{};
std::atomic<std::uint64_t> gPairedVictimLocalAvailabilityOverrides{}, gPairedDeadTargetRejected{};
std::atomic<std::uint64_t> gPairedRecoveryDeferred{};
std::atomic<std::uint32_t> gLastVictimAdmissionHp{}, gLastVictimAdmissionHealth{}, gLastVictimAdmissionEnabled{};
std::atomic<std::uint32_t> gLastRejectedPairedSourceState{0xFFFFFFFFU};
std::atomic<std::uint64_t> gCandidateEligibilityChecks{}, gCandidateEligibilityOverrides{};
std::atomic<std::uint64_t> gRecordEligibilityChecks{}, gRecordEligibilityOverrides{};
std::atomic<std::uint64_t> gParticipantRecordsValidated{}, gParticipantRecordsRetained{}, gParticipantRecordsRejected{};
std::atomic<std::uint64_t> gParticipantPerceptionSamples{}, gParticipantPerceptionDropped{};
std::atomic<std::uint64_t> gAiPlayerQueryRemaps{}, gAiPlayerSlotRemaps{}, gAiPlayerSlotRestoreFailures{};
std::atomic<std::uint64_t> gPostDeathDecisionResets{}, gPostDeathDecisionResetRejected{}, gPostDeathRoleTaskResets{};
std::atomic<std::uint64_t> gInteractionBindingsResolved{}, gInteractionBindingRejected{};
std::atomic<std::uint32_t> gLastHostAttachment{}, gLastLocalAttachment{}, gLastTransitionVariant{};
std::atomic<std::uint32_t> gActiveNativeGeneration{}, gActiveWorldGeneration{};
std::atomic<std::uint32_t> gLastEntityA{}, gLastEntityB{}, gLastCharacterClass{};
std::atomic<std::uint32_t> gLastActionTypeRva{}, gLastActionPathHash{}, gLastActionStatus{};
std::atomic<std::uint32_t> gLastRoleTaskTypeRva{}, gLastRoleTaskKeyA{}, gLastRoleTaskKeyB{};
std::atomic<std::uint32_t> gLastLifecycleFlags{};
std::atomic<std::uint32_t> gLastWithdrawState{};
std::atomic<std::uint32_t> gLastInteractionTargetA{}, gLastInteractionTargetB{};
std::atomic<std::uint32_t> gLastInteractionTargetMode{}, gLastAppliedInteractionTargetA{};
std::atomic<std::uint32_t> gLastAppliedInteractionTargetB{};
std::atomic<std::uint64_t> gLastInteractionTargetPeer{}, gLastAppliedInteractionTargetPeer{};
std::atomic<std::uint64_t> gLastAiPlayerQueryPeer{}, gLastAiPlayerSlotPeer{};
std::atomic<std::uint32_t> gLastInteractionTransition{};

// Native callbacks supply owner lifetimes. Worker adaptations additionally
// require published generation/runtime/GUID identities; no Actor/mailbox read.
// SEH avoids VirtualQuery on the per-frame path; no heap scan.
template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    if (address < 0x10000U || address > 0xFFFF0000U) return false;
    __try { value = *reinterpret_cast<const T*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Write(const std::uintptr_t address, const T& value) noexcept {
    if (address < 0x10000U || address > 0xFFFF0000U) return false;
    __try { *reinterpret_cast<T*>(address) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool EngineThread() noexcept {
    if (gStopping.load(std::memory_order_acquire))
        return gShutdownEngineThread.load(std::memory_order_acquire) == GetCurrentThreadId();
    return gDispatcher != nullptr && gDispatcher->IsCertifiedEngineThread(GetCurrentThreadId());
}
bool Client() noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    return role == Role::Client || role == Role::FrozenClient;
}
bool Stable() noexcept {
    return gGenerations != nullptr && gGenerations->IsStableGeneration(gCurrent.nativeGeneration);
}
bool FiniteCoordinate(const float value) noexcept {
    return std::isfinite(value) && std::abs(value) <= 100000.0F;
}
// Certified retail NPC Character families currently admitted by AISync. Keep
// this authority boundary fail-closed. The native Character class enum is an
// appearance/runtime family, not a network participant identity: class 0 is
// PLAYER, while 2/7 are ANDROID/ANDROID_HEAVY. Participant ownership is
// resolved separately from the real local player pointer and PlayerSync's
// generation-bound remote Character registry.
bool AICharacterClass(const std::uint32_t type) noexcept {
    switch (type) {
    case 1U:
    case 3U:
    case 4U:
    case 6U:
    case 9U:
        return true;
    default:
        return false;
    }
}
bool Valid(const State& s) noexcept {
    if (s.version != 6U || s.nativeGeneration == 0U || s.sessionCookie == 0U ||
        s.worldGeneration == 0U || (s.entityA == 0U && s.entityB == 0U) ||
        s.birth == 0U || s.revision == 0U || !AICharacterClass(s.characterClass) ||
        (s.stateFlags & ~kStateFlags) != 0U || s.moveType > 3U ||
        s.actionStatus > 10U || s.actionDepth > 64U ||
        (s.interactionFlags & ~kInteractionFlags) != 0U ||
        s.interactionTransition > 2U || (s.interactionTransitionFlags & ~0xFFFFU) != 0U ||
        s.interactionVariant < -1 || s.interactionVariant > 0xFFFF ||
        s.interactionTransitionVariant < -1 || s.interactionTransitionVariant > 0xFFFF ||
        (s.lifecycleFlags & ~kLifecycleFlags) != 0U) return false;
    for (const auto coordinate : s.position)
        if (!FiniteCoordinate(coordinate)) return false;
    for (const auto coordinate : s.interactionTargetPosition)
        if (!FiniteCoordinate(coordinate)) return false;
    if (!std::isfinite(s.yaw) || std::abs(s.yaw) > 3.3F ||
        !std::isfinite(s.velocityX) || !std::isfinite(s.velocityZ) ||
        std::abs(s.velocityX) > kMaximumVelocity || std::abs(s.velocityZ) > kMaximumVelocity)
        return false;
    if ((s.stateFlags & PoseValid) == 0U && (s.stateFlags & Moving) != 0U) return false;
    if ((s.stateFlags & ActionValid) != 0U &&
        (s.actionTypeRva == 0U || s.actionDepth == 0U || s.actionRevision == 0U)) return false;
    if ((s.stateFlags & InteractionValid) != 0U &&
        (s.interactionRevision == 0U ||
         s.interactionPairRevision == 0U || (s.interactionFlags & InteractionPairValid) == 0U ||
         ((s.interactionFlags & InteractionTargetIsParticipant) != 0U) != (s.interactionTargetPeer != 0U) ||
         (s.interactionTargetA == 0U && s.interactionTargetB == 0U))) return false;
    if ((s.stateFlags & InteractionValid) == 0U && s.interactionTransition != 0U) return false;
    if (s.interactionTransition != 0U && s.interactionTransitionRevision == 0U) return false;
    if ((s.stateFlags & RoleTaskValid) != 0U &&
        (s.roleTaskRevision == 0U || s.roleTaskTypeRva == 0U)) return false;
    if (s.lifecycleFlags != 0U && s.lifecycleRevision == 0U) return false;
    return true;
}
bool RegisteredPlayerRepresentation(const std::uintptr_t character) noexcept {
    if (character == 0U || gGenerations == nullptr) return false;
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i)
        if (gOwned[i].load(std::memory_order_acquire) == character &&
            gOwnedGeneration[i].load(std::memory_order_relaxed) == generation) return true;
    return false;
}
bool RegisteredPlayerSpawnKey(const std::uint32_t spawnKey) noexcept {
    if (spawnKey == 0U || gGenerations == nullptr) return false;
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i)
        if (gOwnedSpawnKeys[i].load(std::memory_order_acquire) == spawnKey &&
            gOwnedSpawnKeyGeneration[i].load(std::memory_order_relaxed) == generation) return true;
    return false;
}
bool PlayerRepresentation(const std::uintptr_t character) noexcept {
    if (RegisteredPlayerRepresentation(character)) return true;
    std::uint32_t runtimeId{};
    return Read(character + 0x50U, runtimeId) && RegisteredPlayerSpawnKey(runtimeId);
}
bool Eligible(const std::uintptr_t c, std::uint32_t& type) noexcept {
    // PlayerSync owns every dynamically created remote-player Character. Test
    // the explicit ownership registry first, then require a certified NPC class
    // as an independent fail-closed boundary in case a spawn reservation is
    // cleared/reordered during retail lifecycle callbacks.
    if (PlayerRepresentation(c)) return false;
    std::uintptr_t characterTemplate{};
    if (!Read(c + 0x64U, characterTemplate) ||
        !Read(characterTemplate + 4U, type)) return false;
    // +1F is an execution gate, not proof that the NPC owns an initialized
    // manager/tree. Admit still validates the actual tree and Character owner.
    // AISync deliberately owns only the retail NPC families certified by the
    // reverse-engineering audit; unknown/player classes stay with their native
    // subsystem instead of being guessed into AI authority.
    return AICharacterClass(type);
}
bool CharacterIdentity(const std::uintptr_t character, std::uint32_t& entityA,
    std::uint32_t& entityB, std::uint32_t& type) noexcept {
    std::uintptr_t characterTemplate{};
    return character != 0U && Read(character + 0x54U, entityA) &&
        Read(character + 0x58U, entityB) && (entityA != 0U || entityB != 0U) &&
        Read(character + 0x64U, characterTemplate) &&
        Read(characterTemplate + 4U, type);
}
template<class Visitor>
bool ForEachCharacter(Visitor&& visitor) noexcept {
    if (gBase == 0U) return false;
    const auto table = gBase + kCharacterRegistry.table;
    for (std::size_t index = 0U; index < kCharacterRegistry.capacity; ++index) {
        const auto entry = table + index * kCharacterRegistry.stride;
        std::uintptr_t character{};
        std::uint8_t active{};
        if (!Read(entry + kCharacterRegistry.character, character) ||
            !Read(entry + kCharacterRegistry.active, active)) return false;
        if (active != 0U && character != 0U && !visitor(character)) return true;
    }
    return true;
}
std::uintptr_t FindCharacterByIdentity(const std::uint32_t entityA,
    const std::uint32_t entityB) noexcept {
    std::uintptr_t found{};
    static_cast<void>(ForEachCharacter([&](const std::uintptr_t character) noexcept {
        std::uint32_t candidateA{}, candidateB{}, type{};
        if (CharacterIdentity(character, candidateA, candidateB, type) &&
            candidateA == entityA && candidateB == entityB) {
            found = character;
            return false;
        }
        return true;
    }));
    return found;
}
bool ResolvePrimaryPlayerSlot(std::uintptr_t& slot, std::uintptr_t& player) noexcept {
    slot = 0U;
    player = 0U;
    std::uintptr_t root{}, context{}, players{};
    std::uint32_t count{};
    if (!Read(gBase + kInteractionManagerGlobalRva, root) ||
        !Read(root + 8U, context) || !Read(context + 0x84U, count) || count == 0U ||
        !Read(context + 0x80U, players) || players == 0U || !Read(players, player)) return false;
    slot = players;
    return player != 0U;
}
std::uintptr_t LocalPlayer() noexcept {
    // AISync identity must never mistake a temporary retail compatibility
    // projection for the actual local participant.
    if (gAiPlayerContextDepth != 0U && gAiPlayerRealLocal != 0U) return gAiPlayerRealLocal;
    std::uintptr_t slot{}, player{};
    return ResolvePrimaryPlayerSlot(slot, player) ? player : 0U;
}
bool LivingCharacter(const std::uintptr_t character) noexcept {
    std::uint16_t hp{};
    std::uint32_t health{};
    std::uint8_t enabled{};
    return character != 0U && Read(character + 0x33CU, hp) && hp != 0U &&
        Read(character + 0x32CU, health) && health < 3U &&
        Read(character + 0x2FAU, enabled) && enabled != 0U;
}
std::uintptr_t LivingAiParticipant(std::uintptr_t owner, std::uint64_t& peer) noexcept;

bool TerminalDeadCharacter(const std::uintptr_t character) noexcept {
    std::uint16_t hp{};
    std::uint32_t health{};
    return character != 0U && Read(character + 0x33CU, hp) && hp == 0U &&
        Read(character + 0x32CU, health) && health == 4U;
}

struct AiPlayerSlotScope final {
    std::uintptr_t slot{}, previous{};
    bool active{}, changed{};

    explicit AiPlayerSlotScope(const std::uintptr_t owner) noexcept {
        if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() ||
            !Stable() || owner == 0U) return;

        std::uintptr_t current{};
        if (!ResolvePrimaryPlayerSlot(slot, current)) return;
        const auto realLocal = gAiPlayerContextDepth != 0U && gAiPlayerRealLocal != 0U
            ? gAiPlayerRealLocal : current;
        // Do not alter THE_PLAYER during the kill itself. Only the certified
        // terminal state used by PlayerLifecycle opens this compatibility scope.
        if (!TerminalDeadCharacter(realLocal)) return;

        std::uint64_t peer{};
        const auto survivor = LivingAiParticipant(owner, peer);
        if (survivor == 0U || peer == 0U) return;

        if (gAiPlayerContextDepth == 0U) gAiPlayerRealLocal = realLocal;
        ++gAiPlayerContextDepth;
        active = true;
        previous = current;
        if (current == survivor) return;
        if (!Write(slot, survivor)) {
            --gAiPlayerContextDepth;
            if (gAiPlayerContextDepth == 0U) gAiPlayerRealLocal = 0U;
            active = false;
            slot = 0U;
            return;
        }
        changed = true;
        gAiPlayerSlotRemaps.fetch_add(1U, std::memory_order_relaxed);
        gLastAiPlayerSlotPeer.store(peer, std::memory_order_relaxed);
    }

    ~AiPlayerSlotScope() noexcept {
        if (!active) return;
        if (changed && !Write(slot, previous))
            gAiPlayerSlotRestoreFailures.fetch_add(1U, std::memory_order_relaxed);
        if (gAiPlayerContextDepth != 0U) --gAiPlayerContextDepth;
        if (gAiPlayerContextDepth == 0U) gAiPlayerRealLocal = 0U;
    }

    AiPlayerSlotScope(const AiPlayerSlotScope&) = delete;
    AiPlayerSlotScope& operator=(const AiPlayerSlotScope&) = delete;
};

std::uintptr_t ParticipantForPeer(const std::uint64_t peer) noexcept {
    if (peer == 0U) return 0U;
    if (peer == gCurrent.localPeer) return LocalPlayer();
    for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i) {
        const auto& binding = gCurrent.participants[i];
        // Addresses are a local IPC hint, never a lifetime proof. PlayerSync's
        // generation-bound ownership must still certify the exact Character.
        if (binding.peer == peer && RegisteredPlayerRepresentation(binding.character))
            return binding.character;
    }
    return 0U;
}
std::uint64_t PeerForParticipant(const std::uintptr_t character) noexcept {
    if (character != 0U && character == LocalPlayer()) return gCurrent.localPeer;
    for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i)
        if (gCurrent.participants[i].character == character &&
            RegisteredPlayerRepresentation(character)) return gCurrent.participants[i].peer;
    return 0U;
}

std::uintptr_t LivingAiParticipant(const std::uintptr_t owner,
    std::uint64_t& peer) noexcept {
    peer = 0U;
    std::uintptr_t selected{};
    bool selectedHasDistance{};
    float bestDistance{};
    float ownerX{}, ownerY{}, ownerZ{};
    const bool ownerPosition = owner != 0U && Read(owner + 0x70U, ownerX) &&
        Read(owner + 0x74U, ownerY) && Read(owner + 0x78U, ownerZ) &&
        FiniteCoordinate(ownerX) && FiniteCoordinate(ownerY) && FiniteCoordinate(ownerZ);

    for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i) {
        const auto candidatePeer = gCurrent.participants[i].peer;
        const auto candidate = ParticipantForPeer(candidatePeer);
        if (!LivingCharacter(candidate)) continue;

        if (selected == 0U) {
            selected = candidate;
            peer = candidatePeer;
        }
        if (!ownerPosition) continue;

        float x{}, y{}, z{};
        if (!Read(candidate + 0x70U, x) || !Read(candidate + 0x74U, y) ||
            !Read(candidate + 0x78U, z) || !FiniteCoordinate(x) ||
            !FiniteCoordinate(y) || !FiniteCoordinate(z)) continue;
        const auto dx = x - ownerX;
        const auto dy = y - ownerY;
        const auto dz = z - ownerZ;
        const auto distance = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(distance)) continue;
        if (!selectedHasDistance || distance < bestDistance) {
            selectedHasDistance = true;
            bestDistance = distance;
            selected = candidate;
            peer = candidatePeer;
        }
    }
    return selected;
}

std::uintptr_t __cdecl ThePlayerDetour() noexcept {
    const auto original = reinterpret_cast<ThePlayerResolver>(
        gThePlayerOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    const auto retailPlayer = original();
    if (gRole.load(std::memory_order_acquire) != Role::Host ||
        !EngineThread() || !Stable() || gAiPlayerQueryOwner == 0U ||
        retailPlayer == 0U || !TerminalDeadCharacter(retailPlayer)) return retailPlayer;

    std::uint64_t peer{};
    const auto survivor = LivingAiParticipant(gAiPlayerQueryOwner, peer);
    if (survivor == 0U || peer == 0U) return retailPlayer;

    // This is not target forcing. It only adapts single-player-only AI helpers
    // which ask for THE_PLAYER while the real local Host Character is dead.
    // Senses/hostility/BT arbitration still choose the actual target.
    gAiPlayerQueryRemaps.fetch_add(1U, std::memory_order_relaxed);
    gLastAiPlayerQueryPeer.store(peer, std::memory_order_relaxed);
    return survivor;
}
void CacheLivingPlayerAlliance() noexcept {
    if (gLivingPlayerAllianceKnown) return;
    const auto player = LocalPlayer();
    std::uint32_t alliance{};
    if (LivingCharacter(player) && Read(player + 0x2E4U, alliance) && alliance < 32U) {
        gLivingPlayerAlliance = alliance;
        gLivingPlayerAllianceKnown = true;
    }
}
bool ReadPairedCycleActivity(const std::uintptr_t character, std::uintptr_t& cycle,
    std::uint8_t& state, bool& active) noexcept {
    active = false;
    std::uintptr_t owner{}, target{};
    std::uint8_t flags{};
    std::uint32_t targetBirth{}, currentBirth{}, stage{};
    if (!Read(character + 0x36CU, cycle) || cycle == 0U ||
        !Read(cycle + 4U, owner) || owner != character || !Read(cycle + 0x1AU, state)) return false;
    if (state != 0x42U && state != 0x4EU) return true;
    if (!Read(cycle + 8U, flags) || !Read(cycle + 0x44U, target)) return false;
    // Native completion/interruption flags survive while the persistent cycle
    // still has its paired type and victim GUID. A failed initializer also
    // leaves that type behind with a null cached victim.
    if ((flags & 6U) != 0U || target == 0U) return true;
    if (!Read(cycle + 0x48U, targetBirth) || !Read(target + 0x50U, currentBirth)) return false;
    if (targetBirth != currentBirth) return true;
    // FUN_00663E00 no longer advances stage 6; both failed admission and the
    // natural end can reach it without changing +1A or setting +8 flags.
    if (state == 0x4EU) {
        if (!Read(cycle + 0xE0U, stage)) return false;
        if (stage >= 6U) return true;
    }
    active = true;
    return true;
}
bool PairedCycleMatches(const std::uintptr_t character, const State& interaction) noexcept {
    std::uintptr_t cycle{};
    std::uintptr_t owner{};
    std::uint8_t state{};
    std::uint32_t targetA{}, targetB{};
    return Read(character + 0x36CU, cycle) && Read(cycle + 0x1AU, state) &&
        Read(cycle + 4U, owner) && owner == character &&
        (state == 0x42U || state == 0x4EU) &&
        Read(cycle + (state == 0x4EU ? 0x100U : 0xD8U), targetA) &&
        Read(cycle + (state == 0x4EU ? 0x104U : 0xDCU), targetB) &&
        targetA == interaction.interactionTargetA && targetB == interaction.interactionTargetB;
}
bool NativeVictimFlowRunning(std::uintptr_t source, std::uintptr_t victim) noexcept;
std::uint32_t __fastcall HostilityDetour(void* logical, void*, const std::uintptr_t target) noexcept {
    const auto original = reinterpret_cast<Hostility>(gHostilityOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    std::uintptr_t owner{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() ||
        !Read(reinterpret_cast<std::uintptr_t>(logical), owner) || !Eligible(owner, type) ||
        PeerForParticipant(target) == 0U) return original(logical, target);
    if (!LivingCharacter(target) && !NativeVictimFlowRunning(owner, target)) return 0U;
    if (NativeVictimFlowRunning(owner, target)) return 1U;
    if (!RegisteredPlayerRepresentation(target)) return original(logical, target);
    CacheLivingPlayerAlliance();
    std::uintptr_t root{}, matrix{};
    std::uint32_t alliance{}, row{};
    // FUN_0077A800's exact alliance predicate. Treat an AIMP human visual as
    // the living player's alliance, without editing templates/global stances
    // or resurrecting THE_PLAYER (death changes its alliance to 7).
    if (!gLivingPlayerAllianceKnown ||
        !Read(gBase + kInteractionManagerGlobalRva, root) || !Read(root + 0x34U, matrix) ||
        !Read(reinterpret_cast<std::uintptr_t>(logical) + 0x64U, alliance) || alliance >= 32U ||
        !Read(matrix + alliance * 4U, row)) return 0U;
    return (row & (1U << gLivingPlayerAlliance)) != 0U ? 1U : 0U;
}
std::uintptr_t SenseEventOwner(void* context, const std::uintptr_t sensesOffset) noexcept {
    std::uintptr_t senses{}, owner{}, ownedSenses{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() ||
        !Read(reinterpret_cast<std::uintptr_t>(context) + sensesOffset, senses) ||
        !Read(senses + 0x8CU, owner) || !Eligible(owner, type) ||
        !Read(owner + 0x34D4U, ownedSenses) || ownedSenses != senses) return 0U;
    return owner;
}
void DispatchSenseEvent(void* context, const std::uintptr_t event,
    const std::uintptr_t sensesOffset, const SenseEvent original) noexcept {
    if (original == nullptr) return;
    const auto previousOwner = gSenseEventOwner;
    gSenseEventOwner = SenseEventOwner(context, sensesOffset);
    if (gSenseEventOwner != 0U) gSenseEventPasses.fetch_add(1U, std::memory_order_relaxed);
    __try { original(context, event); }
    __finally { gSenseEventOwner = previousOwner; }
}
void __fastcall SenseEventDetour(void* context, void*, const std::uintptr_t event) noexcept {
    DispatchSenseEvent(context, event, 0x14U,
        reinterpret_cast<SenseEvent>(gSenseEventOriginal.load(std::memory_order_acquire)));
}
void __fastcall SoundEventDetour(void* context, void*, const std::uintptr_t event) noexcept {
    DispatchSenseEvent(context, event, 0x10U,
        reinterpret_cast<SenseEvent>(gSoundEventOriginal.load(std::memory_order_acquire)));
}
std::uintptr_t __fastcall SenseCharacterResolverDetour(void* resources, void*,
    const std::uint32_t* identity) noexcept {
    const auto retail = reinterpret_cast<SenseCharacterResolver>(gBase + 0x002545C0U)(resources, identity);
    if (gSenseEventOwner == 0U || gRole.load(std::memory_order_acquire) != Role::Host ||
        !EngineThread() || !Stable()) return retail;
    const auto address = reinterpret_cast<std::uintptr_t>(identity);
    std::uint32_t entityA{}, entityB{};
    if (!Read(address, entityA) || !Read(address + 4U, entityB) ||
        (entityA == 0U && entityB == 0U)) return retail;
    std::uintptr_t mapped{};
    std::uint64_t peer{};
    for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i) {
        const auto candidatePeer = gCurrent.participants[i].peer;
        const auto target = ParticipantForPeer(candidatePeer);
        std::uint32_t a{}, b{}, type{};
        if (target == 0U || target == gSenseEventOwner ||
            !RegisteredPlayerRepresentation(target) || !LivingCharacter(target) ||
            !CharacterIdentity(target, a, b, type) || a != entityA || b != entityB) continue;
        if (mapped != 0U && (mapped != target || peer != candidatePeer)) {
            gParticipantSignalAmbiguous.fetch_add(1U, std::memory_order_relaxed);
            return retail;
        }
        mapped = target;
        peer = candidatePeer;
    }
    if (mapped == 0U) return retail;
    // An identity already belonging to another live native Character is not a
    // missing registration. Fail closed on that collision instead of rebinding it.
    if (retail != 0U && retail != mapped) {
        std::uint32_t a{}, b{}, type{};
        if (CharacterIdentity(retail, a, b, type) && a == entityA && b == entityB) {
            gParticipantSignalAmbiguous.fetch_add(1U, std::memory_order_relaxed);
            return retail;
        }
    }
    gParticipantSignalLookups.fetch_add(1U, std::memory_order_relaxed);
    gLastParticipantSignalPeer.store(peer, std::memory_order_relaxed);
    if (retail == mapped) return retail;
    if (retail == 0U) gParticipantSignalRetailMissing.fetch_add(1U, std::memory_order_relaxed);
    else gParticipantSignalRetailMismatch.fetch_add(1U, std::memory_order_relaxed);
    gParticipantSignalRemaps.fetch_add(1U, std::memory_order_relaxed);
    // Restore the exact source of an existing native event, not a sight/threat
    // value or forced target. Native strength, occlusion and arbitration remain.
    return mapped;
}
bool PairGenerationStable() noexcept {
    const auto generation = gActiveNativeGeneration.load(std::memory_order_acquire);
    return !gStopping.load(std::memory_order_acquire) && generation != 0U &&
        gGenerations != nullptr && gGenerations->IsStableGeneration(generation);
}
bool LoadPairBinding(const PairBinding& slot, PairIdentity& value) noexcept {
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        const auto before = slot.sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;
        value.character = slot.character.load(std::memory_order_acquire);
        value.animated = slot.animated.load(std::memory_order_acquire);
        value.runtimeId = slot.runtimeId.load(std::memory_order_acquire);
        value.entityA = slot.entityA.load(std::memory_order_acquire);
        value.entityB = slot.entityB.load(std::memory_order_acquire);
        value.type = slot.type.load(std::memory_order_acquire);
        value.generation = slot.generation.load(std::memory_order_acquire);
        value.epoch = slot.epoch.load(std::memory_order_acquire);
        value.peer = slot.peer.load(std::memory_order_acquire);
        if (before != slot.sequence.load(std::memory_order_acquire)) continue;
        return value.character != 0U && PairGenerationStable() &&
            value.generation == gActiveNativeGeneration.load(std::memory_order_acquire) &&
            value.epoch == gPairBindingEpoch.load(std::memory_order_acquire);
    }
    return false;
}
bool PairIdentityLive(const PairIdentity& value) noexcept {
    std::uint32_t id{}, a{}, b{}, type{};
    return Read(value.character + 0x50U, id) && id == value.runtimeId &&
        CharacterIdentity(value.character, a, b, type) && a == value.entityA &&
        b == value.entityB && type == value.type;
}
bool PublishedPairParticipant(const std::uintptr_t character, PairIdentity& value) noexcept {
    for (const auto& slot : gPairParticipants) {
        PairIdentity candidate{};
        if (!LoadPairBinding(slot, candidate) || candidate.character != character ||
            candidate.peer == 0U || !PairIdentityLive(candidate)) continue;
        if (&slot != &gPairParticipants[0] && !RegisteredPlayerRepresentation(character)) return false;
        value = candidate;
        return true;
    }
    return false;
}
std::size_t PublishedPairSource(const std::uintptr_t character, PairIdentity& value) noexcept {
    for (std::size_t i = 0U; i < kActors; ++i) {
        PairIdentity candidate{};
        std::uint32_t type{};
        if (LoadPairBinding(gPairSources[i], candidate) && candidate.character == character &&
            PairIdentityLive(candidate) && Eligible(character, type) && type == candidate.type) {
            value = candidate;
            return i;
        }
    }
    return kActors;
}
void InvalidatePairCharacter(const std::uintptr_t character) noexcept {
    for (auto& slot : gPairSources) {
        auto expected = character;
        slot.character.compare_exchange_strong(expected, 0U, std::memory_order_acq_rel);
    }
    for (auto& slot : gPairParticipants) {
        auto expected = character;
        slot.character.compare_exchange_strong(expected, 0U, std::memory_order_acq_rel);
    }
}
void PublishPairBinding(PairBinding& slot, const std::uintptr_t character,
    const std::uint64_t peer) noexcept {
    // Single engine-thread publisher; getters are never invoked by IPC workers.
    PairIdentity value{};
    value.character = character; value.peer = peer;
    value.generation = gCurrent.nativeGeneration;
    value.epoch = gPairBindingEpoch.load(std::memory_order_acquire);
    if (character != 0U && (!Read(character + 0x50U, value.runtimeId) || value.runtimeId == 0U ||
        !CharacterIdentity(character, value.entityA, value.entityB, value.type))) value.character = 0U;
    if (value.character != 0U) {
        PairIdentity previous{};
        if (LoadPairBinding(slot, previous) && previous.character == value.character &&
            previous.runtimeId == value.runtimeId && previous.entityA == value.entityA &&
            previous.entityB == value.entityB && previous.type == value.type && previous.peer == peer &&
            previous.animated != 0U)
            return;
        __try {
            std::uintptr_t table{}, getter{};
            if (Read(character, table) && Read(table + 0x1CU, getter) &&
                getter >= gBase + 0x1000U && getter < gBase + 0x00D9F000U)
                value.animated = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(
                    reinterpret_cast<void*>(character));
        } __except (EXCEPTION_EXECUTE_HANDLER) { value.animated = 0U; }
    }
    slot.sequence.fetch_add(1U, std::memory_order_acq_rel);
    slot.character.store(0U, std::memory_order_release);
    slot.runtimeId.store(value.runtimeId, std::memory_order_release);
    slot.entityA.store(value.entityA, std::memory_order_release);
    slot.entityB.store(value.entityB, std::memory_order_release);
    slot.type.store(value.type, std::memory_order_release);
    slot.generation.store(value.generation, std::memory_order_release);
    slot.epoch.store(value.epoch, std::memory_order_release);
    slot.peer.store(value.peer, std::memory_order_release);
    slot.animated.store(value.animated, std::memory_order_release);
    slot.character.store(value.character, std::memory_order_release);
    slot.sequence.fetch_add(1U, std::memory_order_release);
}
void PublishPairSource(Actor& actor) noexcept {
    const auto index = static_cast<std::size_t>(&actor - gActors.data());
    PublishPairBinding(gPairSources[index], !actor.faulted && !actor.duplicate ? actor.character : 0U, 0U);
}
void RefreshPairBindings() noexcept {
    PublishPairBinding(gPairParticipants[0], LocalPlayer(), gCurrent.localPeer);
    for (std::size_t i = 0U; i < kActors; ++i) {
        const auto& participant = gCurrent.participants[i];
        const bool admitted = i < gCurrent.participantCount &&
            RegisteredPlayerRepresentation(participant.character);
        PublishPairBinding(gPairParticipants[i + 1U], admitted ? participant.character : 0U,
            admitted ? participant.peer : 0U);
        PublishPairSource(gActors[i]);
    }
}
bool PairSourceBusy(const std::uintptr_t character) noexcept {
    for (std::size_t i = 0U; i < kActors; ++i)
        if (gPairSources[i].character.load(std::memory_order_acquire) == character &&
            gPairSourceBusy[i].load(std::memory_order_acquire)) return true;
    return false;
}
std::uintptr_t ResolvePairedParticipant(void* resources, const std::uint32_t* identity,
    const bool animated) noexcept {
    const auto resolver = reinterpret_cast<SenseCharacterResolver>(
        gBase + (animated ? 0x002545C0U : 0x002544E0U));
    const auto retail = resolver(resources, identity);
    const auto role = gRole.load(std::memory_order_acquire);
    if (gPairedResolveOwner == 0U || identity != gPairedResolveIdentity ||
        (role != Role::Host && role != Role::Client) || !PairGenerationStable()) return retail;
    const auto address = reinterpret_cast<std::uintptr_t>(identity);
    std::uint32_t entityA{}, entityB{};
    if (!Read(address, entityA) || !Read(address + 4U, entityB) ||
        (entityA == 0U && entityB == 0U)) return retail;
    PairIdentity target{};
    for (std::size_t i = 1U; i < gPairParticipants.size(); ++i) {
        PairIdentity candidate{};
        if (!LoadPairBinding(gPairParticipants[i], candidate) || candidate.peer == 0U ||
            candidate.character == gPairedResolveOwner || candidate.entityA != entityA ||
            candidate.entityB != entityB || !RegisteredPlayerRepresentation(candidate.character) ||
            !PairIdentityLive(candidate) || !LivingCharacter(candidate.character)) continue;
        if (target.character != 0U &&
            (target.character != candidate.character || target.peer != candidate.peer)) {
            gParticipantPairAmbiguous.fetch_add(1U, std::memory_order_relaxed);
            return retail;
        }
        target = candidate;
    }
    if (target.character == 0U) return retail;
    gParticipantPairLookups.fetch_add(1U, std::memory_order_relaxed);
    gLastParticipantPairPeer.store(target.peer, std::memory_order_relaxed);
    // Only the two common-initializer CALLs; fill a missing native registration.
    if (retail != 0U) return retail;
    gParticipantPairRetailMissing.fetch_add(1U, std::memory_order_relaxed);
    const auto mapped = animated ? target.animated : target.character;
    if (mapped == 0U) return retail;
    gParticipantPairRemaps.fetch_add(1U, std::memory_order_relaxed);
    return mapped;
}
std::uintptr_t __fastcall PairedCharacterResolverDetour(void* resources, void*,
    const std::uint32_t* identity) noexcept {
    return ResolvePairedParticipant(resources, identity, false);
}
std::uintptr_t __fastcall PairedAnimatedResolverDetour(void* resources, void*,
    const std::uint32_t* identity) noexcept {
    return ResolvePairedParticipant(resources, identity, true);
}
bool ParticipantPairBusy(const std::uintptr_t target, bool& busy) noexcept {
    std::uintptr_t cycle{}, owner{};
    std::uint8_t state{};
    if (!Read(target + 0x36CU, cycle) || cycle == 0U ||
        !Read(cycle + 4U, owner) || owner != target || !Read(cycle + 0x1AU, state)) return false;
    busy = state == 0x42U || state == 0x4EU;
    return true;
}
bool EligibilityParticipant(void* logical, const std::uintptr_t owner,
    std::uintptr_t& target) noexcept {
    std::uint32_t type{};
    const auto address = reinterpret_cast<std::uintptr_t>(logical);
    return gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        owner != 0U && Eligible(owner, type) && Read(address, target) && target != owner &&
        address == target + 0x280U && RegisteredPlayerRepresentation(target) &&
        PeerForParticipant(target) != 0U;
}
std::uint32_t AdaptParticipantEligibility(void* logical, const std::uintptr_t owner,
    const std::uintptr_t expectedTarget, std::atomic<std::uint64_t>& checks,
    std::atomic<std::uint64_t>& overrides) noexcept {
    // The original predicate stays intact, including its full-EAX return.
    // Zero is the accepted value at these TWO callers, not a global meaning.
    const auto retail = reinterpret_cast<TargetEligibility>(gBase + 0x0038B380U)(logical);
    std::uintptr_t target{};
    if (!EligibilityParticipant(logical, owner, target) ||
        (expectedTarget != 0U && target != expectedTarget)) return retail;
    checks.fetch_add(1U, std::memory_order_relaxed);
    // A dead proxy can still pass the physical execution predicate. Preserve
    // an already running native death pair, then exclude it from new choices.
    if (NativeVictimFlowRunning(owner, target)) return 0U;
    if (TerminalDeadCharacter(target)) {
        gPairedDeadTargetRejected.fetch_add(1U, std::memory_order_relaxed);
        return 1U;
    }
    bool paired{};
    if (retail == 0U || !LivingCharacter(target) ||
        !ParticipantPairBusy(target, paired) || paired) return retail;
    // Only perception target eligibility is adapted. Never edit Character
    // execution fields, wake its proxy brain, or override sense/target choice.
    overrides.fetch_add(1U, std::memory_order_relaxed);
    return 0U;
}
std::uint32_t __fastcall CandidateEligibilityDetour(void* logical, void*) noexcept {
    return AdaptParticipantEligibility(logical, gCandidateEligibilityOwner, 0U,
        gCandidateEligibilityChecks, gCandidateEligibilityOverrides);
}
std::uint32_t __fastcall RecordEligibilityDetour(void* logical, void*) noexcept {
    return AdaptParticipantEligibility(logical, gRecordEligibilityOwner, gRecordEligibilityTarget,
        gRecordEligibilityChecks, gRecordEligibilityOverrides);
}
std::uint8_t __fastcall RecordInvalidDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<RecordInvalid>(gRecordInvalidOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 1U;
    const auto record = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t owner{}, target{};
    std::uint32_t type{};
    const bool participant = gRole.load(std::memory_order_acquire) == Role::Host &&
        EngineThread() && Stable() && Read(record + 8U, owner) && Eligible(owner, type) &&
        Read(record + 0x42CU, target) && RegisteredPlayerRepresentation(target) &&
        PeerForParticipant(target) != 0U && target != owner;
    const auto previousOwner = gRecordEligibilityOwner;
    const auto previousTarget = gRecordEligibilityTarget;
    // Clear a nested non-participant scope instead of inheriting its caller.
    gRecordEligibilityOwner = participant ? owner : 0U;
    gRecordEligibilityTarget = participant ? target : 0U;
    auto invalid = original(context);
    if (participant && TerminalDeadCharacter(target) && !NativeVictimFlowRunning(owner, target)) {
        invalid = 1U;
        gPairedDeadTargetRejected.fetch_add(1U, std::memory_order_relaxed);
    }
    gRecordEligibilityOwner = previousOwner;
    gRecordEligibilityTarget = previousTarget;
    if (participant) {
        gParticipantRecordsValidated.fetch_add(1U, std::memory_order_relaxed);
        (invalid == 0U ? gParticipantRecordsRetained : gParticipantRecordsRejected)
            .fetch_add(1U, std::memory_order_relaxed);
    }
    // Lifetime and hostility checks still run in the original validator.
    return invalid;
}
bool AdmitParticipantCandidate(const std::uintptr_t senses, const std::uintptr_t target) noexcept {
    // Mirror the ordinary Character candidate admission. The retail helper
    // owns allocation, insertion, Character lifetime and reference counting.
    // Its forced-perception flag (+493) is removed for a newly admitted record;
    // normal sight/hearing/occlusion still decide whether this human is sensed.
    __try {
        auto node = *reinterpret_cast<const std::uintptr_t*>(senses + 0xCU);
        for (std::size_t i = 0U; node != 0U && i < 1024U; ++i) {
            const auto record = *reinterpret_cast<const std::uintptr_t*>(node);
            if (*reinterpret_cast<const std::uintptr_t*>(record + 0x42CU) == target) {
                *reinterpret_cast<std::uint8_t*>(record + 0x490U) = 1U;
                return true;
            }
            node = *reinterpret_cast<const std::uintptr_t*>(node + 8U);
        }
        if (node != 0U) return false;
        std::uintptr_t record{};
        using Ensure = std::uintptr_t*(__thiscall*)(void*, std::uintptr_t*, std::uintptr_t);
        reinterpret_cast<Ensure>(gBase + 0x0032AE00U)(
            reinterpret_cast<void*>(senses), &record, target);
        if (record == 0U) return false;
        *reinterpret_cast<std::uint8_t*>(record + 0x493U) = 0U;
        auto* refs = reinterpret_cast<std::int32_t*>(record + 0x14U);
        if (--*refs == 0) {
            const auto wrapper = record + 0x10U;
            const auto table = *reinterpret_cast<const std::uintptr_t*>(wrapper);
            reinterpret_cast<void(__thiscall*)(void*)>(
                *reinterpret_cast<const std::uintptr_t*>(table + 4U))(
                    reinterpret_cast<void*>(wrapper));
        }
        gParticipantCandidates.fetch_add(1U, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}
void __fastcall SensesCandidatesDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<SensesCandidates>(gCandidatesOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto senses = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t owner{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        Read(senses + 0x8CU, owner) && Eligible(owner, type)) {
        gParticipantCandidatePasses.fetch_add(1U, std::memory_order_relaxed);
        for (std::uint32_t i = 0U; i < gCurrent.participantCount; ++i) {
            const auto target = ParticipantForPeer(gCurrent.participants[i].peer);
            if (target == 0U || target == owner || !LivingCharacter(target)) continue;
            if ((HostilityDetour(reinterpret_cast<void*>(owner + 0x280U), nullptr, target) & 0xFFU) == 0U)
                continue;
            if (!AdmitParticipantCandidate(senses, target))
                gParticipantTargetRejected.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    const auto previousQueryOwner = gAiPlayerQueryOwner;
    const auto previousCandidateOwner = gCandidateEligibilityOwner;
    const bool hostOwner = gRole.load(std::memory_order_acquire) == Role::Host &&
        EngineThread() && Stable() && owner != 0U && Eligible(owner, type);
    gCandidateEligibilityOwner = hostOwner ? owner : 0U;
    if (hostOwner) gAiPlayerQueryOwner = owner;
    {
        AiPlayerSlotScope playerContext(hostOwner ? owner : 0U);
        original(context);
    }
    gCandidateEligibilityOwner = previousCandidateOwner;
    gAiPlayerQueryOwner = previousQueryOwner;
}
bool SuspendExistingTree(Actor& actor) noexcept {
    __try {
        // Retail cleanup, not memset of AI state. Order: selection -> pending
        // -> active; the promotion helpers then swap EMPTY vector roles.
        for (const auto offset : {0x44U, 0x48U, 0x4CU}) {
            const auto header = *reinterpret_cast<const std::uintptr_t*>(actor.tree + offset);
            const auto begin = *reinterpret_cast<const std::uintptr_t*>(header);
            const auto end = *reinterpret_cast<const std::uintptr_t*>(header + 4U);
            if (end < begin || (end - begin) % 4U != 0U || (end - begin) / 4U > 64U) return false;
        }
        using Clear = void(__thiscall*)(void*);
        reinterpret_cast<Clear>(gBase + 0x002FE080U)(reinterpret_cast<void*>(actor.tree));
        reinterpret_cast<Clear>(gBase + 0x002FE5C0U)(reinterpret_cast<void*>(actor.tree));
        reinterpret_cast<Clear>(gBase + 0x002FE690U)(reinterpret_cast<void*>(actor.tree));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

std::uint32_t HashActionWord(std::uint32_t hash, const std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= 16777619U;
    }
    return hash;
}

bool TraversalActionType(const std::uint32_t typeRva) noexcept {
    // Certified Alien traversal/backstage leaf vtables from the current retail build.
    // These are only lifecycle hints: the network never serializes Alien-only state.
    switch (typeRva) {
    case 0x00DBC498U: // MOVE_TO_BACKSTAGE_VIA_VENT_CLOSEST_TO_ALIEN
    case 0x00DBC8E0U: // MOVE_TO_FRONTSTAGE_VIA_FLANKED_VENT_CLOSEST_TO_PLAYER
    case 0x00EBC8B8U: // BACKSTAGE_AREA_SWEEP
    case 0x00ECA338U: // BACKSTAGE_ALIEN_RESPONSE
        return true;
    default:
        return false;
    }
}

std::uint32_t SemanticLifecycleFlags(const Actor& actor) noexcept {
    std::uint32_t flags{};
    if (actor.hostDormant) flags |= LifecycleHostDormant;
    if (actor.withdrawKnown && actor.withdrawState != 0U) {
        flags |= LifecycleWithdrawn;
        if (actor.withdrawState == 2U) flags |= LifecycleWithdrawState2;
    }
    return flags;
}

void RefreshLifecycleFlags(Actor& actor, const std::uint32_t eventFlags) noexcept {
    actor.desired.lifecycleFlags = (eventFlags &
        (LifecycleSpatialDiscontinuity | LifecycleTraversalAction)) |
        SemanticLifecycleFlags(actor);
}

void MarkLifecycle(Actor& actor, const std::uint32_t flags) noexcept {
    ++actor.desired.lifecycleRevision;
    if (actor.desired.lifecycleRevision == 0U) actor.desired.lifecycleRevision = 1U;
    RefreshLifecycleFlags(actor, flags);
    gLifecycleCaptured.fetch_add(1U, std::memory_order_relaxed);
    gLastLifecycleFlags.store(actor.desired.lifecycleFlags, std::memory_order_relaxed);
}

bool ResolveAlienWithdrawContext(const std::uintptr_t character,
    std::uintptr_t& context, std::uint32_t& state) noexcept {
    context = 0U;
    state = 0U;
    std::uintptr_t alienData{};
    if (character == 0U || !Read(character + 0x34F0U, alienData) || alienData == 0U ||
        !Read(alienData + 0x18U, context) || context == 0U || !Read(context, state) || state > 2U)
        return false;
    return true;
}

void CaptureWithdrawState(Actor& actor) noexcept {
    std::uintptr_t context{};
    std::uint32_t state{};
    if (!ResolveAlienWithdrawContext(actor.character, context, state)) return;
    gWithdrawSamples.fetch_add(1U, std::memory_order_relaxed);
    gLastWithdrawState.store(state, std::memory_order_relaxed);
    if (!actor.withdrawKnown) {
        actor.withdrawKnown = true;
        actor.withdrawState = state;
        RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
        return;
    }
    if (actor.withdrawState == state) {
        RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
        return;
    }
    actor.withdrawState = state;
    gWithdrawTransitions.fetch_add(1U, std::memory_order_relaxed);
    MarkLifecycle(actor, 0U);
}

std::uint32_t DesiredWithdrawState(const State& state) noexcept {
    if ((state.lifecycleFlags & LifecycleWithdrawState2) != 0U) return 2U;
    if ((state.lifecycleFlags & (LifecycleWithdrawn | LifecycleHostDormant)) != 0U) return 1U;
    return 0U;
}

bool ApplyAlienWithdrawEntry(const std::uintptr_t context,
    const std::uint32_t desiredState) noexcept {
    __try {
        using Reset = void(__fastcall*)(void*);
        using Set = void(__thiscall*)(void*, int);
        const auto reset = reinterpret_cast<Reset>(gBase + 0x003349D0U); // FUN_007349D0
        const auto set = reinterpret_cast<Set>(gBase + 0x0032FC20U);     // FUN_0072FC20
        if (desiredState == 0U) {
            set(reinterpret_cast<void*>(context), 0);
            reset(reinterpret_cast<void*>(context));
        } else {
            reset(reinterpret_cast<void*>(context));
            set(reinterpret_cast<void*>(context), static_cast<int>(desiredState));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ApplyAuthoritativeWithdraw(Actor& actor, const State& state) noexcept {
    std::uintptr_t context{};
    std::uint32_t current{};
    if (!ResolveAlienWithdrawContext(actor.character, context, current)) return true;
    const auto desired = DesiredWithdrawState(state);
    if (current == desired) return true;
    if (!ApplyAlienWithdrawEntry(context, desired)) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    gWithdrawApplied.fetch_add(1U, std::memory_order_relaxed);
    gLastWithdrawState.store(desired, std::memory_order_relaxed);
    return true;
}

bool ProxyWithdrawn(const State& state) noexcept {
    return (state.lifecycleFlags & (LifecycleWithdrawn | LifecycleHostDormant)) != 0U;
}

bool ResolveRoleTaskExecutor(const std::uintptr_t character,
    std::uintptr_t& roleManager, std::uintptr_t& executor) noexcept {
    roleManager = 0U;
    executor = 0U;
    // Retail role/task ownership is not embedded directly at Character+424.
    // Character+34DC owns the role manager, whose +424 slot points at the
    // active role-task executor. This is the exact chain used by the retail
    // role/task queries around FUN_006E87B0 and FUN_0076F080.
    return character != 0U && Read(character + 0x34DCU, roleManager) && roleManager != 0U &&
        Read(roleManager + 0x424U, executor) && executor != 0U;
}

bool ClearRoleTaskExecutor(Actor& actor) noexcept {
    std::uintptr_t roleManager{}, executor{};
    if (!ResolveRoleTaskExecutor(actor.character, roleManager, executor)) return true;
    std::uintptr_t currentTask{};
    if (!Read(executor + 8U, currentTask) || currentTask == 0U) return true;
    __try {
        // FUN_00761A90 releases the admitted task and its movement requests.
        // Retail callers pass ECX plus a task-handle address on the stack;
        // the certified body ignores that argument but still ends in RET 4.
        // The decompiler's ECX-only __fastcall prototype drops it and corrupts
        // the wrapper's saved registers on return. Keep the full retail ABI.
        using Cleanup = void(__thiscall*)(void*, const void*);
        reinterpret_cast<Cleanup>(gBase + kRoleTaskCleanupRva)(
            reinterpret_cast<void*>(executor), reinterpret_cast<const void*>(executor + 8U));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

bool QuiesceProxyExecution(Actor& actor) noexcept {
    if (actor.character == 0U) return false;
    static_cast<void>(character_direct_step_runtime::Stop(actor.character));
    const bool tree = actor.tree == 0U || SuspendExistingTree(actor);
    const bool task = ClearRoleTaskExecutor(actor);
    actor.completedRevision = 0U;
    actor.lastProxyRefresh = 0U;
    gExecutionBarriers.fetch_add(1U, std::memory_order_relaxed);
    return tree && task;
}

bool TeleportCharacterEntry(const std::uintptr_t entry, const float* position) noexcept {
    // Keep SEH in a trivial helper. MSVC rejects __try in callers that require
    // C++ object unwinding (SnapAuthoritativePose owns std::array/std::string).
    __try {
        using Teleport = void(__thiscall*)(void*, const float*);
        reinterpret_cast<Teleport>(gBase +
            character_direct_step_runtime::NativeMovementRvas().teleportCharacter)(
                reinterpret_cast<void*>(entry), position);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

bool SnapAuthoritativePose(Actor& actor, const State& state) noexcept {
    if ((state.stateFlags & PoseValid) == 0U || actor.character == 0U) return false;
    const auto table = gBase + kCharacterRegistry.table;
    std::uintptr_t entry{};
    for (std::size_t index = 0U; index < kCharacterRegistry.capacity; ++index) {
        const auto candidate = table + index * kCharacterRegistry.stride;
        std::uintptr_t character{};
        std::uint8_t active{};
        if (!Read(candidate + kCharacterRegistry.character, character) ||
            !Read(candidate + kCharacterRegistry.active, active)) return false;
        if (active != 0U && character == actor.character) { entry = candidate; break; }
    }
    if (entry == 0U) return false;
    alignas(16) const std::array<float, 4> position{
        state.position[0], state.position[1], state.position[2], 0.0F};
    if (!TeleportCharacterEntry(entry, position.data())) return false;
    std::string error;
    if (!character_direct_step_runtime::ApplyReplicatedYaw(actor.character, state.yaw, error))
        return false;
    actor.completedRevision = 0U;
    actor.lastProxyRefresh = 0U;
    gLifecycleSnaps.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

void CaptureRoleTask(Actor& actor) noexcept {
    std::uint32_t typeRva{}, keyA{}, keyB{};
    std::uintptr_t roleManager{}, executor{}, task{}, table{}, descriptor{};
    bool valid = ResolveRoleTaskExecutor(actor.character, roleManager, executor) &&
        Read(executor + 8U, task) && task != 0U && Read(task, table) &&
        table >= gBase && table - gBase <= 0xFFFFFFFFULL;
    if (valid) {
        typeRva = static_cast<std::uint32_t>(table - gBase);
        if (Read(task + 0x18U, descriptor) && descriptor != 0U) {
            static_cast<void>(Read(descriptor + 4U, keyA));
            static_cast<void>(Read(descriptor + 8U, keyB));
        }
    }
    const bool wasValid = (actor.desired.stateFlags & RoleTaskValid) != 0U;
    // Revision follows admission/replacement of the actual task object, not
    // mutable task context fields. Pointer identity is kept Host-local only.
    const bool changed = valid != wasValid || task != actor.lastRoleTask;
    if (changed) {
        ++actor.desired.roleTaskRevision;
        if (actor.desired.roleTaskRevision == 0U) actor.desired.roleTaskRevision = 1U;
        gRoleTaskTransitions.fetch_add(1U, std::memory_order_relaxed);
    }
    actor.lastRoleTask = valid ? task : 0U;
    actor.desired.roleTaskTypeRva = valid ? typeRva : 0U;
    actor.desired.roleTaskKeyA = valid ? keyA : 0U;
    actor.desired.roleTaskKeyB = valid ? keyB : 0U;
    if (valid) {
        actor.desired.stateFlags |= RoleTaskValid;
        gRoleTaskCaptured.fetch_add(1U, std::memory_order_relaxed);
        gLastRoleTaskTypeRva.store(typeRva, std::memory_order_relaxed);
        gLastRoleTaskKeyA.store(keyA, std::memory_order_relaxed);
        gLastRoleTaskKeyB.store(keyB, std::memory_order_relaxed);
    } else {
        actor.desired.stateFlags &= ~RoleTaskValid;
    }
}

void CaptureActiveAction(Actor& actor) noexcept {
    std::uint32_t typeRva{}, pathHash{}, status{}, depth{};
    std::uintptr_t header{}, begin{}, end{};
    if (Read(actor.tree + 0x44U, header) && Read(header, begin) && Read(header + 4U, end) &&
        end >= begin && (end - begin) % 4U == 0U && (end - begin) / 4U <= 64U) {
        depth = static_cast<std::uint32_t>((end - begin) / 4U);
        pathHash = 2166136261U;
        for (auto entry = begin; entry < end && depth != 0U; entry += 4U) {
            std::uintptr_t state{}, node{}, table{};
            std::uint8_t selectedChild{};
            if (!Read(entry, state) || !Read(state + 0x10U, node) || !Read(node, table) ||
                table < gBase || table - gBase > 0xFFFFFFFFULL ||
                !Read(state + 0x1CU, selectedChild)) {
                depth = 0U; break;
            }
            const auto rva = static_cast<std::uint32_t>(table - gBase);
            pathHash = HashActionWord(pathHash, rva);
            pathHash = HashActionWord(pathHash, selectedChild);
            if (entry + 4U == end) {
                typeRva = rva;
                if (!Read(state + 4U, status)) depth = 0U;
            }
        }
    }
    const bool valid = depth != 0U && typeRva != 0U && status <= 10U;
    const bool wasValid = (actor.desired.stateFlags & ActionValid) != 0U;
    const bool changed = valid != wasValid || actor.desired.actionTypeRva != typeRva ||
        actor.desired.actionPathHash != pathHash || actor.desired.actionStatus != status ||
        actor.desired.actionDepth != depth;
    if (changed) {
        ++actor.desired.actionRevision;
        if (actor.desired.actionRevision == 0U) actor.desired.actionRevision = 1U;
        gActionTransitions.fetch_add(1U, std::memory_order_relaxed);
    }
    const bool traversal = valid && TraversalActionType(typeRva);
    if (traversal != actor.traversalAction) {
        actor.traversalAction = traversal;
        MarkLifecycle(actor, traversal ? LifecycleTraversalAction : 0U);
    }
    actor.desired.actionTypeRva = valid ? typeRva : 0U;
    actor.desired.actionPathHash = valid ? pathHash : 0U;
    actor.desired.actionStatus = valid ? status : 0U;
    actor.desired.actionDepth = valid ? depth : 0U;
    if (valid) {
        actor.desired.stateFlags |= ActionValid;
        gActionCaptured.fetch_add(1U, std::memory_order_relaxed);
        gLastActionTypeRva.store(typeRva, std::memory_order_relaxed);
        gLastActionPathHash.store(pathHash, std::memory_order_relaxed);
        gLastActionStatus.store(status, std::memory_order_relaxed);
    } else {
        actor.desired.stateFlags &= ~ActionValid;
    }
}

std::uint32_t MoveTypeForSpeed(const float speed) noexcept {
    if (speed < 0.65F) return 0U;
    if (speed < 1.8F) return 1U;
    if (speed < 3.2F) return 2U;
    return 3U;
}

void CapturePose(Actor& actor, const ULONGLONG now) noexcept {
    // An unchanged snapshot can be examined again before it is sent. Sampling
    // against lastSend then differentiated one-frame/sub-5ms steps and emitted
    // zero velocity between moving samples. Keep an independent 20Hz clock.
    if (actor.hasLastPosition && now - actor.lastPoseSample < kSendPeriod) return;
    float position[3]{}, yaw{};
    if (!Read(actor.character + kPositionOffset, position[0]) ||
        !Read(actor.character + kPositionOffset + 4U, position[1]) ||
        !Read(actor.character + kPositionOffset + 8U, position[2]) ||
        !Read(actor.character + kYawOffset, yaw) ||
        !FiniteCoordinate(position[0]) || !FiniteCoordinate(position[1]) ||
        !FiniteCoordinate(position[2]) || !std::isfinite(yaw) || std::abs(yaw) > 3.3F) {
        actor.desired.stateFlags &= ~(PoseValid | Moving);
        gPoseRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    float velocityX{}, velocityZ{};
    bool discontinuity = false;
    if (actor.hasLastPosition && now > actor.lastPoseSample) {
        const auto elapsed = now - actor.lastPoseSample;
        const auto dx = position[0] - actor.lastPosition[0];
        const auto dy = position[1] - actor.lastPosition[1];
        const auto dz = position[2] - actor.lastPosition[2];
        const auto distance = std::hypot(dx, dz);
        const auto spatialDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (elapsed <= 500ULL && std::isfinite(spatialDistance) &&
            (spatialDistance >= kLifecycleDiscontinuityDistance ||
             std::abs(dy) >= kLifecycleVerticalDiscontinuity)) {
            discontinuity = true;
            MarkLifecycle(actor, LifecycleSpatialDiscontinuity |
                (actor.traversalAction ? LifecycleTraversalAction : 0U));
        }
        if (elapsed >= 5ULL && elapsed <= 250ULL && std::isfinite(distance) && distance < 2.0F) {
            const auto scale = 1000.0F / static_cast<float>(elapsed);
            velocityX = dx * scale; velocityZ = dz * scale;
            if (!std::isfinite(velocityX) || !std::isfinite(velocityZ) ||
                std::abs(velocityX) > kMaximumVelocity || std::abs(velocityZ) > kMaximumVelocity) {
                velocityX = 0.0F; velocityZ = 0.0F;
            }
        }
    }
    const auto measuredSpeed = std::hypot(velocityX, velocityZ);
    if (!discontinuity && measuredSpeed >= 0.05F) {
        actor.stationarySince = 0U;
        // Two samples damp gait oscillation without extrapolating a teleport.
        velocityX = velocityX * 0.7F + actor.desired.velocityX * 0.3F;
        velocityZ = velocityZ * 0.7F + actor.desired.velocityZ * 0.3F;
    } else if (!discontinuity) {
        if (actor.stationarySince == 0U) actor.stationarySince = now;
        if (now - actor.stationarySince < 100ULL) {
            velocityX = actor.desired.velocityX;
            velocityZ = actor.desired.velocityZ;
        } else { velocityX = 0.0F; velocityZ = 0.0F; }
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        actor.lastPosition[i] = position[i]; actor.desired.position[i] = position[i];
    }
    actor.hasLastPosition = true; actor.lastPoseSample = now;
    actor.desired.yaw = yaw;
    actor.desired.velocityX = velocityX; actor.desired.velocityZ = velocityZ;
    const auto speed = std::hypot(velocityX, velocityZ);
    std::uintptr_t movement{};
    std::uint32_t nativeGait{};
    // +198 is retail's resolved gait; +194 is the requested gait. Prefer the
    // actual gait over speed thresholds (especially during Alien transitions).
    if (Read(actor.character + 0x42A8U, movement) && movement != 0U &&
        Read(movement + 0x198U, nativeGait) && nativeGait <= 3U)
        actor.desired.moveType = nativeGait;
    else
        actor.desired.moveType = MoveTypeForSpeed(speed);
    actor.desired.stateFlags |= PoseValid;
    if (std::isfinite(speed) && speed >= kMotionThreshold)
        actor.desired.stateFlags |= Moving;
    else
        actor.desired.stateFlags &= ~Moving;
    gPoseCaptured.fetch_add(1U, std::memory_order_relaxed);
}

Actor* Find(const std::uintptr_t c) noexcept {
    for (auto& actor : gActors) if (actor.character == c) return &actor;
    return nullptr;
}
std::uint8_t __fastcall PairedInitializeDetour(void* context, void*,
    const std::uint32_t* identity, const std::int32_t variant) noexcept {
    const auto original = reinterpret_cast<PairedInitialize>(
        gPairedInitializeOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    const auto role = gRole.load(std::memory_order_acquire);
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t owner{}, ownedCycle{};
    std::uint8_t state{};
    PairIdentity binding{};
    if ((role != Role::Host && role != Role::Client) || !PairGenerationStable() ||
        !Read(cycle + 4U, owner) || PublishedPairSource(owner, binding) == kActors ||
        !Read(owner + 0x36CU, ownedCycle) || ownedCycle != cycle ||
        !Read(cycle + 0x1AU, state) || (state != 0x42U && state != 0x4EU))
        return original(context, identity, variant);
    if (identity != reinterpret_cast<const std::uint32_t*>(
            cycle + (state == 0x4EU ? 0x100U : 0xD8U)))
        return original(context, identity, variant);
    const auto previousOwner = gPairedResolveOwner;
    const auto* previousIdentity = gPairedResolveIdentity;
    gPairedResolveOwner = owner;
    gPairedResolveIdentity = identity;
    std::uint8_t result{};
    __try { result = original(context, identity, variant); }
    __finally {
        gPairedResolveOwner = previousOwner;
        gPairedResolveIdentity = previousIdentity;
    }
    if (result != 0U) gPairedInitializeSucceeded.fetch_add(1U, std::memory_order_relaxed);
    else gPairedInitializeFailed.fetch_add(1U, std::memory_order_relaxed);
    return result;
}
bool ManagedPairedVictim(const std::uintptr_t victim, const std::uintptr_t source) noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    if (role != Role::Host && role != Role::Client && role != Role::FrozenClient) return false;
    // A remote participant stays protected even if its queued source has been
    // destroyed/reused or its source metadata is temporarily unavailable.
    if (RegisteredPlayerRepresentation(victim)) return true;
    PairIdentity participant{};
    std::uint32_t type{};
    if (!PublishedPairParticipant(victim, participant)) return false;
    if (Eligible(source, type)) return true;
    for (const auto& slot : gPairSources)
        if (slot.character.load(std::memory_order_acquire) == source && source != 0U) return true;
    return false;
}
// Diagnostic reasons: 1=bindings, 2=source layout/lifetime, 3=target identity,
// 4=animation definition, 5=inactive source, 6=victim already dead.
bool RejectPairedVictim(const std::uint32_t state, const std::uint32_t reason) noexcept {
    gLastRejectedPairedSourceState.store(state, std::memory_order_relaxed);
    gLastRejectedPairedReason.store(reason, std::memory_order_relaxed);
    return false;
}
bool ValidPairedVictimLink(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle, std::uint32_t& rejectedState) noexcept {
    rejectedState = 0xFFFFFFFFU;
    std::uintptr_t current{}, owner{}, table{}, target{}, definition{}, getter{};
    std::uint8_t state{};
    std::uint32_t targetA{}, targetB{}, targetBirth{}, definitionKind{}, definitionIndex{};
    PairIdentity sourceIdentity{}, victimIdentity{};
    if (Read(sourceCycle + 0x1AU, state)) rejectedState = state;
    if (!PairGenerationStable() || PublishedPairSource(source, sourceIdentity) == kActors ||
        !PublishedPairParticipant(victim, victimIdentity))
        return RejectPairedVictim(rejectedState, 1U);
    // Source addresses persist across Move/Idle/impact and new paired cycles.
    // Neither address equality nor the 4E type alone proves a usable source.
    if (state != 0x4EU || !Read(source + 0x36CU, current) || current != sourceCycle ||
        !Read(sourceCycle + 4U, owner) || owner != source ||
        !Read(sourceCycle, table) || table != gBase + 0x00EAE5D0U)
        return RejectPairedVictim(rejectedState, 2U);
    if (!Read(sourceCycle + 0x44U, target) || target != victim ||
        !Read(sourceCycle + 0x48U, targetBirth) || targetBirth != victimIdentity.runtimeId ||
        !Read(sourceCycle + 0x100U, targetA) || targetA != victimIdentity.entityA ||
        !Read(sourceCycle + 0x104U, targetB) || targetB != victimIdentity.entityB)
        return RejectPairedVictim(rejectedState, 3U);
    // 00675FAD reads animation+8; 0064E6DF reads animation+4. A still-typed
    // source can have lost its definition after an interrupted/finished pair.
    // Validate the retail interface without choosing a clip or animation kind.
    if (!Read(sourceCycle + 0x4CU, definition) || definition == 0U ||
        !Read(definition, table) || !Read(table + 4U, getter) ||
        getter < gBase + 0x1000U || getter >= gBase + 0x00D9F000U ||
        !Read(definition + 4U, definitionIndex) || !Read(definition + 8U, definitionKind))
        return RejectPairedVictim(rejectedState, 4U);
    return true;
}
bool VictimHealthAllowsAdmission(const std::uintptr_t victim) noexcept {
    std::uint16_t hp{};
    std::uint32_t health{};
    std::uint8_t enabled{};
    const bool readable = Read(victim + 0x33CU, hp) && Read(victim + 0x32CU, health);
    static_cast<void>(Read(victim + 0x2FAU, enabled));
    gLastVictimAdmissionHp.store(hp, std::memory_order_relaxed);
    gLastVictimAdmissionHealth.store(health, std::memory_order_relaxed);
    gLastVictimAdmissionEnabled.store(enabled, std::memory_order_relaxed);
    // 2FA is not a victim admission contract: the paired flow takes control of
    // a player while preparing its graph. Keep LivingCharacter for perception.
    return readable && hp != 0U && health < 3U;
}
bool PreparedVictimHealthContinuation(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle) noexcept {
    const auto* submission = gNativeVictimSubmission;
    std::uint8_t serial{};
    return submission != nullptr && submission->livingBefore && submission->source == source &&
        submission->sourceCycle == sourceCycle && submission->victimBefore.character == victim &&
        PairIdentityLive(submission->victimBefore) &&
        Read(sourceCycle + 0x19U, serial) && serial == submission->sourceSerial;
}
bool ValidPairedVictimSource(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle, std::uint32_t& rejectedState) noexcept {
    if (!ValidPairedVictimLink(victim, source, sourceCycle, rejectedState)) return false;
    std::uintptr_t current{};
    std::uint8_t state{};
    bool active{};
    if (!ReadPairedCycleActivity(source, current, state, active) || !active)
        return RejectPairedVictim(rejectedState, 5U);
    if (!VictimHealthAllowsAdmission(victim)) {
        // The certified producer can inflict native damage BEFORE message 17.
        // Continue only its exact same originally living victim/cycle; never
        // admit a stale event or a new attack against a pre-existing corpse.
        if (!PreparedVictimHealthContinuation(victim, source, sourceCycle))
            return RejectPairedVictim(rejectedState, 6U);
        gPairedVictimHealthContinuations.fetch_add(1U, std::memory_order_relaxed);
    }
    return true;
}
bool LocalClientVictim(const std::uintptr_t victim) noexcept {
    PairIdentity local{};
    return gRole.load(std::memory_order_acquire) == Role::Client &&
        LoadPairBinding(gPairParticipants[0], local) && local.character == victim &&
        local.peer != 0U && PairIdentityLive(local);
}
bool NativeVictimFlowRunning(const std::uintptr_t source, const std::uintptr_t victim) noexcept {
    PairIdentity participant{}, sourceIdentity{};
    std::uintptr_t sourceCycle{}, victimCycle{}, owner{}, table{}, linked{}, linkedSource{};
    std::uint32_t cachedBirth{}, rejectedState{};
    std::uint8_t state{}, acquired{}, ended{}, flags{};
    if (!PublishedPairParticipant(victim, participant) ||
        PublishedPairSource(source, sourceIdentity) == kActors ||
        !Read(source + 0x36CU, sourceCycle) ||
        !Read(victim + 0x36CU, victimCycle) || victimCycle == 0U ||
        !Read(victimCycle + 4U, owner) || owner != victim ||
        !Read(victimCycle, table) || table != gBase + 0x00EAE690U ||
        !Read(victimCycle + 0x1AU, state) || state != 0x4FU ||
        !Read(victimCycle + 8U, flags) || (flags & 6U) != 0U ||
        !Read(victimCycle + 0xD9U, acquired) || acquired == 0U ||
        !Read(victimCycle + 0xD8U, ended) || ended != 0U ||
        !Read(victimCycle + 0x68U, linkedSource) || linkedSource != source ||
        !Read(victimCycle + 0x6CU, cachedBirth) || cachedBirth != sourceIdentity.runtimeId ||
        !Read(victimCycle + 0xB0U, linked) || linked != sourceCycle) return false;
    // Health changes at the beginning of a fatal 4F graph, not its end.
    return ValidPairedVictimLink(victim, source, sourceCycle, rejectedState);
}
bool LocalClientVictimOwnsPair(const std::uintptr_t source) noexcept {
    PairIdentity local{};
    std::uintptr_t cycle{};
    std::uint8_t interactive{};
    return gRole.load(std::memory_order_acquire) == Role::Client &&
        LoadPairBinding(gPairParticipants[0], local) && PairIdentityLive(local) &&
        NativeVictimFlowRunning(source, local.character) &&
        Read(local.character + 0x36CU, cycle) && Read(cycle + 0xDAU, interactive) &&
        (interactive == 0U || TerminalDeadCharacter(local.character));
}
void __fastcall PairedVictimConvergenceDetour(void* context, void* victimContext,
    const std::uintptr_t victim, const std::uint32_t victimFlag) noexcept {
    const auto original = reinterpret_cast<PairedVictimConvergence>(gBase + 0x0024E6C0U);
    const auto sourceCycle = reinterpret_cast<std::uintptr_t>(context);
    const auto victimCycle = reinterpret_cast<std::uintptr_t>(victimContext);
    std::uintptr_t source{}, current{}, owner{}, table{}, linked{};
    std::uint8_t state{};
    if (!Read(victimCycle + 0x68U, source)) return;
    if (!ManagedPairedVictim(victim, source)) { original(context, victim, victimFlag); return; }
    // Only the certified 4F update CALL passes its ESI cycle. Preserve the
    // victim's native graph/death cleanup, but expire an unusable source link
    // exactly as 00655BDE already does for an absent or non-4E source.
    if (!Read(victim + 0x36CU, current) || current != victimCycle ||
        !Read(victimCycle + 4U, owner) || owner != victim ||
        !Read(victimCycle, table) || table != gBase + 0x00EAE690U ||
        !Read(victimCycle + 0x1AU, state) || state != 0x4FU ||
        !Read(victimCycle + 0xB0U, linked) || linked != sourceCycle) return;
    PairIdentity sourceIdentity{};
    std::uint32_t cachedBirth{}, rejectedState{};
    if (PublishedPairSource(source, sourceIdentity) == kActors ||
        !Read(victimCycle + 0x6CU, cachedBirth) || cachedBirth != sourceIdentity.runtimeId ||
        !ValidPairedVictimLink(victim, source, sourceCycle, rejectedState)) {
        if (Write(victimCycle + 0xB0U, std::uintptr_t{}))
            gPairedVictimConvergenceDetached.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    original(context, victim, victimFlag);
}
// CALL 00655BD7: ECX=source, ESI=victim 4F cycle; two arguments already on
// the stack. Fastcall consumes the original target/flag with the same RET 8.
__declspec(naked) void PairedVictimConvergenceShim() {
    __asm {
        mov edx, esi
        jmp PairedVictimConvergenceDetour
    }
}
bool VictimAdmissionMatches(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle) noexcept {
    return gVictimAdmissionDepth != 0U && gVictimAdmissionTarget == victim &&
        gVictimAdmissionSource == source && gVictimAdmissionCycle == sourceCycle;
}
bool BeginVictimAdmission(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uintptr_t sourceCycle, std::size_t& slot) noexcept {
    PairIdentity binding{};
    slot = PublishedPairSource(source, binding);
    bool expected = false;
    if (slot == kActors) return false;
    if (!gPairSourceBusy[slot].compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        gPairedVictimBusy.fetch_add(1U, std::memory_order_relaxed);
        slot = kActors;
        return false;
    }
    std::uint32_t state{};
    std::uint8_t initialized{};
    if (!ValidPairedVictimSource(victim, source, sourceCycle, state)) {
        gLastRejectedPairedSourceState.store(state, std::memory_order_relaxed);
        gPairSourceBusy[slot].store(false, std::memory_order_release);
        slot = kActors;
        return false;
    }
    // Native constructor resets E7; victim initialization sets it. Serialize
    // admission and consume that native handshake once for this source cycle.
    if (!Read(sourceCycle + 0xE7U, initialized) || initialized != 0U) {
        gPairedVictimDuplicates.fetch_add(1U, std::memory_order_relaxed);
        gPairSourceBusy[slot].store(false, std::memory_order_release);
        slot = kActors;
        return false;
    }
    return true;
}
void __fastcall PairedVictimEventDetour(const void* packet, void*, void* character) noexcept {
    const auto original = reinterpret_cast<PairedVictimEvent>(gPairedVictimEventOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto victim = reinterpret_cast<std::uintptr_t>(character);
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    std::uintptr_t source{}, sourceCycle{};
    if (!Read(address, source)) return;
    if (!ManagedPairedVictim(victim, source)) { original(packet, character); return; }
    std::size_t slot = kActors;
    std::uint32_t rejectedState{};
    const bool readable = Read(address + 0x30U, sourceCycle);
    const bool nested = readable && VictimAdmissionMatches(victim, source, sourceCycle);
    std::uint8_t initialized{};
    if (nested && (!Read(sourceCycle + 0xE7U, initialized) || initialized != 0U)) {
        gPairedVictimDuplicates.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (!readable || (nested ? !ValidPairedVictimSource(victim, source, sourceCycle, rejectedState)
                           : !BeginVictimAdmission(victim, source, sourceCycle, slot))) {
        // Reject before the event handler resets the victim's animation layer.
        gPairedVictimEventsRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const auto previousSource = gVictimAdmissionSource, previousTarget = gVictimAdmissionTarget;
    const auto previousCycle = gVictimAdmissionCycle;
    const auto previousDepth = gVictimAdmissionDepth;
    gVictimAdmissionSource = source; gVictimAdmissionTarget = victim;
    gVictimAdmissionCycle = sourceCycle; gVictimAdmissionDepth = previousDepth + 1U;
    __try { original(packet, character); }
    __finally {
        gVictimAdmissionSource = previousSource; gVictimAdmissionTarget = previousTarget;
        gVictimAdmissionCycle = previousCycle; gVictimAdmissionDepth = previousDepth;
        if (slot != kActors) gPairSourceBusy[slot].store(false, std::memory_order_release);
    }
}
void __fastcall PairedVictimTransitionDetour(void* context, void*,
    const std::uintptr_t source, const void* parameters, const std::uintptr_t sourceCycle) noexcept {
    const auto original = reinterpret_cast<PairedVictimTransition>(
        gPairedVictimTransitionOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t victim{}, ownedCycle{};
    if (!Read(cycle + 4U, victim)) return;
    if (!ManagedPairedVictim(victim, source)) { original(context, source, parameters, sourceCycle); return; }
    std::uint32_t state{};
    std::size_t slot = kActors;
    const bool nested = VictimAdmissionMatches(victim, source, sourceCycle);
    if (!Read(victim + 0x36CU, ownedCycle) || ownedCycle != cycle ||
        (nested ? !ValidPairedVictimSource(victim, source, sourceCycle, state)
                : !BeginVictimAdmission(victim, source, sourceCycle, slot))) {
        gPairedVictimTransitionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const auto previousSource = gVictimAdmissionSource, previousTarget = gVictimAdmissionTarget;
    const auto previousCycle = gVictimAdmissionCycle;
    const auto previousDepth = gVictimAdmissionDepth;
    gVictimAdmissionSource = source; gVictimAdmissionTarget = victim;
    gVictimAdmissionCycle = sourceCycle; gVictimAdmissionDepth = previousDepth + 1U;
    gPairedVictimTransitions.fetch_add(1U, std::memory_order_relaxed);
    __try { original(context, source, parameters, sourceCycle); }
    __finally {
        gVictimAdmissionSource = previousSource; gVictimAdmissionTarget = previousTarget;
        gVictimAdmissionCycle = previousCycle; gVictimAdmissionDepth = previousDepth;
        if (slot != kActors) gPairSourceBusy[slot].store(false, std::memory_order_release);
    }
}
void __fastcall PairedVictimInitializeDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<PairedVictimInitialize>(
        gPairedVictimInitializeOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t victim{}, source{}, sourceCycle{}, ownedCycle{}, table{};
    std::uint8_t state{};
    if (Read(cycle + 4U, victim) && Read(cycle + 0x68U, source) &&
        ManagedPairedVictim(victim, source) &&
        Read(victim + 0x36CU, ownedCycle) && ownedCycle == cycle &&
        Read(cycle + 0x1AU, state) && state == 0x4FU &&
        Read(cycle, table) && table == gBase + 0x00EAE690U) {
        std::uint32_t rejectedState{0xFFFFFFFFU};
        if (!Read(cycle + 0xB0U, sourceCycle) ||
            !VictimAdmissionMatches(victim, source, sourceCycle) ||
            !ValidPairedVictimSource(victim, source, sourceCycle, rejectedState)) {
            // Recheck after the victim's previous-cycle destructor: native
            // callbacks can change the source during that transition too.
            // Mark only this newly constructed cycle as failed using retail
            // completion flags. The 4F destructor must not finalize a grapple
            // which was never initialized (D8=ended, D9=no animation acquired).
            std::uint8_t flags{};
            if (Read(cycle + 8U, flags))
                static_cast<void>(Write(cycle + 8U, static_cast<std::uint8_t>(flags | 0x46U)));
            static_cast<void>(Write(cycle + 0xB0U, std::uintptr_t{}));
            static_cast<void>(Write(cycle + 0xB4U, std::uintptr_t{}));
            static_cast<void>(Write(cycle + 0xD8U, std::uint8_t{1U}));
            static_cast<void>(Write(cycle + 0xD9U, std::uint8_t{}));
            gLastRejectedPairedSourceState.store(rejectedState, std::memory_order_relaxed);
            gPairedVictimInitializeAborted.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
    }
    original(context);
    std::uint8_t acquired{};
    if (VictimAdmissionMatches(victim, source, sourceCycle) &&
        Read(cycle + 0xD9U, acquired) && acquired != 0U) {
        gPairedVictimNativeStarted.fetch_add(1U, std::memory_order_relaxed);
        if (LocalClientVictim(victim))
            gPairedVictimLocalStarted.fetch_add(1U, std::memory_order_relaxed);
    }
}
void __fastcall PairedVictimSubmitDetour(void* character, void* dispatch, const void* packet) noexcept {
    const auto victim = reinterpret_cast<std::uintptr_t>(character);
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    std::uintptr_t source{}, sourceCycle{};
    std::uint32_t state{};
    // Capture the original 0x40-byte message only inside its certified producer.
    // Dispatch after that producer returns: victim death callbacks must not
    // re-enter a half-prepared source animation. No synthetic packet/clip.
    if (gNativeVictimSubmission != nullptr &&
        !gNativeVictimSubmission->captured && Read(address, source) &&
        Read(address + 0x30U, sourceCycle) && sourceCycle == gNativeVictimSubmission->sourceCycle &&
        (RegisteredPlayerRepresentation(victim) || LocalClientVictim(victim)) &&
        ManagedPairedVictim(victim, source) && ValidPairedVictimSource(victim, source, sourceCycle, state) &&
        crash_guard::TryCopy(gNativeVictimSubmission->packet.data(), packet,
            gNativeVictimSubmission->packet.size())) {
        gNativeVictimSubmission->victim = victim;
        gNativeVictimSubmission->captured = true;
        return;
    }
    if (dispatch != nullptr) reinterpret_cast<CharacterMessage>(dispatch)(character, packet);
}
void __fastcall PairedVictimPrepareDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<PairedVictimInitialize>(
        gPairedVictimPrepareOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    NativeVictimSubmission submission{};
    submission.sourceCycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t victim{};
    std::size_t slot = kActors;
    // Retail animation workers also invoke this producer. Only atomic identity
    // publications/TLS/native reads are used here, never engine-owned tables.
    // Lease the exact source through preparation AND the native victim handler.
    if (!Read(submission.sourceCycle + 4U, submission.source) ||
        !Read(submission.sourceCycle + 0x44U, victim) ||
        !ManagedPairedVictim(victim, submission.source) ||
        !PublishedPairParticipant(victim, submission.victimBefore) ||
        !Read(submission.sourceCycle + 0x19U, submission.sourceSerial) ||
        !VictimHealthAllowsAdmission(victim) ||
        !BeginVictimAdmission(victim, submission.source, submission.sourceCycle, slot)) {
        original(context);
        return;
    }
    submission.livingBefore = true;
    auto* previous = gNativeVictimSubmission;
    const auto previousSource = gVictimAdmissionSource, previousTarget = gVictimAdmissionTarget;
    const auto previousCycle = gVictimAdmissionCycle, previousDepth = gVictimAdmissionDepth;
    gNativeVictimSubmission = &submission;
    gVictimAdmissionSource = submission.source; gVictimAdmissionTarget = victim;
    gVictimAdmissionCycle = submission.sourceCycle; gVictimAdmissionDepth = previousDepth + 1U;
    __try {
        original(context);
        // The original packet/variant/geometry are complete now. Run the same
        // native message handler before the source can be reused, including
        // when native preparation has just changed its victim's health.
        if (submission.captured) {
            gPairedVictimNativeDispatches.fetch_add(1U, std::memory_order_relaxed);
            if (!EngineThread()) gPairedVictimWorkerDispatches.fetch_add(1U, std::memory_order_relaxed);
            if (LocalClientVictim(submission.victim))
                gPairedVictimLocalDispatches.fetch_add(1U, std::memory_order_relaxed);
            PairedVictimEventDetour(submission.packet.data(), nullptr, reinterpret_cast<void*>(submission.victim));
        }
    } __finally {
        gNativeVictimSubmission = previous;
        gVictimAdmissionSource = previousSource; gVictimAdmissionTarget = previousTarget;
        gVictimAdmissionCycle = previousCycle; gVictimAdmissionDepth = previousDepth;
        gPairSourceBusy[slot].store(false, std::memory_order_release);
    }
}
// Interior boundary 00662C61: the packet argument is already on the stack,
// ECX is the victim, EDX its virtual +2C dispatcher. The shim preserves the
// retail SEH state write, CALL/RET 4 stack effect and callee-saved registers.
__declspec(naked) void PairedVictimSubmitShim() {
    __asm {
        mov dword ptr [esp + 0F0h], ebx
        call PairedVictimSubmitDetour
        jmp dword ptr [gPairedVictimSubmitContinue]
    }
}
bool RemoteVictimPresentationReady(const std::uintptr_t victim) noexcept {
    PairIdentity binding{};
    std::uintptr_t graph{}, controller{};
    std::uint8_t enabled{}, playerController{};
    if (!RegisteredPlayerRepresentation(victim) || !PublishedPairParticipant(victim, binding) ||
        binding.animated == 0U || !Read(binding.animated + 0x2CF4U, enabled) || enabled == 0U ||
        !Read(binding.animated + 0x34BCU, graph) || graph == 0U ||
        !Read(victim + 0x31CU, controller)) return false;
    return controller == 0U || (Read(controller + 0x48U, playerController) && playerController == 0U);
}
bool RemoteVictimPairPresent(const std::uintptr_t victim, const std::uintptr_t nativeCycle) noexcept {
    std::uintptr_t victimCycle{}, source{}, sourceCycle{}, current{}, owner{}, target{}, table{};
    std::uint32_t birth{}, cachedBirth{}, a{}, b{};
    std::uint8_t state{};
    PairIdentity sourceIdentity{}, victimIdentity{};
    if (!PublishedPairParticipant(victim, victimIdentity) ||
        !Read(nativeCycle + 4U, owner) || !Read(nativeCycle + 0x1AU, state)) return false;
    if (state == 0x4EU && owner != victim) {
        // Source destructor: the victim can already be in its death/idle cycle.
        // ESI still supplies the exact source whose native flags are cleaned.
        source = owner; sourceCycle = nativeCycle;
    } else if (state == 0x4FU && owner == victim) {
        if (!Read(victim + 0x36CU, victimCycle) || victimCycle != nativeCycle ||
            !Read(nativeCycle, table) || table != gBase + 0x00EAE690U ||
            !Read(nativeCycle + 0x68U, source) ||
            !Read(nativeCycle + 0x6CU, cachedBirth) ||
            !Read(nativeCycle + 0xB0U, sourceCycle)) return false;
    } else return false;
    if (PublishedPairSource(source, sourceIdentity) == kActors ||
        (state == 0x4FU && cachedBirth != sourceIdentity.runtimeId) ||
        !Read(source + 0x36CU, current) || current != sourceCycle ||
        !Read(sourceCycle + 4U, owner) || owner != source ||
        !Read(sourceCycle + 0x1AU, state) || state != 0x4EU ||
        !Read(sourceCycle, table) || table != gBase + 0x00EAE5D0U ||
        !Read(sourceCycle + 0x44U, target) || target != victim ||
        !Read(sourceCycle + 0x48U, birth) || birth != victimIdentity.runtimeId ||
        !Read(sourceCycle + 0x100U, a) || a != victimIdentity.entityA ||
        !Read(sourceCycle + 0x104U, b) || b != victimIdentity.entityB) return false;
    return true;
}
bool LocalVictimInitializationReady(const std::uintptr_t victim, const std::uintptr_t nativeCycle) noexcept {
    PairIdentity binding{};
    std::uintptr_t source{}, sourceCycle{}, graph{}, controller{};
    std::uint8_t enabled{}, player{}, state{};
    // Only the original 4F initializer inside a leased Client admission. The
    // real player's destructors and unrelated execution predicates stay retail.
    return LocalClientVictim(victim) && PublishedPairParticipant(victim, binding) &&
        binding.animated != 0U && Read(binding.animated + 0x2CF4U, enabled) && enabled != 0U &&
        Read(binding.animated + 0x34BCU, graph) && graph != 0U &&
        Read(victim + 0x31CU, controller) && controller != 0U &&
        Read(controller + 0x48U, player) && player != 0U &&
        Read(nativeCycle + 0x1AU, state) && state == 0x4FU &&
        Read(nativeCycle + 0x68U, source) && Read(nativeCycle + 0xB0U, sourceCycle) &&
        VictimAdmissionMatches(victim, source, sourceCycle);
}
std::uint32_t __fastcall PairedVictimAvailabilityDetour(void* logical, void*, const std::uintptr_t nativeCycle) noexcept {
    const auto retail = reinterpret_cast<TargetEligibility>(gBase + 0x0038B330U)(logical);
    if (retail == 0U) return retail;
    std::uintptr_t victim{};
    if (!Read(reinterpret_cast<std::uintptr_t>(logical), victim) ||
        reinterpret_cast<std::uintptr_t>(logical) != victim + 0x280U ||
        (!RemoteVictimPresentationReady(victim) && !LocalVictimInitializationReady(victim, nativeCycle)) ||
        !RemoteVictimPairPresent(victim, nativeCycle)) return retail;
    // Only the victim initializer and the two paired destructors call here.
    // A loaded remote presentation actor may have proxy execution phase 1/2/3;
    // permit its native victim/death graph, never its autonomous brain.
    gPairedVictimAvailabilityOverrides.fetch_add(1U, std::memory_order_relaxed);
    if (LocalClientVictim(victim))
        gPairedVictimLocalAvailabilityOverrides.fetch_add(1U, std::memory_order_relaxed);
    return 0U;
}
// All three certified callers keep their current paired cycle in ESI.
// ECX-only predicate: pass ESI to the adaptation, consume it with RET 4,
// then return to the original caller with its stack unchanged.
__declspec(naked) void PairedVictimAvailabilityShim() {
    __asm {
        push esi
        call PairedVictimAvailabilityDetour
        ret
    }
}
bool Release(Actor& actor) noexcept;
Actor* Admit(const std::uintptr_t c) noexcept {
    // CharacterCreate may run a manager update before its detour can publish
    // the resulting address. The pre-reserved spawn key rejects that update;
    // the address check also retires an actor admitted by an older build/path.
    if (PlayerRepresentation(c)) {
        if (auto* actor = Find(c); actor != nullptr) {
            // CharacterDirectStep belongs to PlayerSync for this object. Do not
            // call Release(), which would stop the remote-player controller.
            *actor = {};
            gProvisionalPlayerActorsRetired.fetch_add(1U, std::memory_order_relaxed);
        }
        return nullptr;
    }
    if (auto* actor = Find(c); actor != nullptr) {
        std::uint32_t id{};
        std::uintptr_t tree{};
        if (!Read(c + 0x50U, id) || !Read(c + 0x34D8U, tree)) return nullptr;
        if (id == actor->runtimeId) {
            if (tree != actor->tree) {
                static_cast<void>(Release(*actor));
                actor->tree = tree; actor->completedRevision = 0U;
                std::uintptr_t owner{};
                actor->faulted = tree == 0U || !Read(tree + 0xCU, owner) || owner != c;
                if (!actor->faulted && Client()) actor->faulted = !SuspendExistingTree(*actor);
                if (actor->faulted) gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed);
            }
            PublishPairSource(*actor);
            return actor;
        }
        // Address reuse without a destruction notification: do not touch a
        // request belonging to the old Character. Keep this visible in diagnostics.
        *actor = {};
    }
    std::uint32_t type{}, id{}, a{}, b{};
    std::uintptr_t tree{};
    if (!Eligible(c, type)) return nullptr;
    if (!Read(c + 0x50U, id) || id == 0U ||
        !Read(c + 0x54U, a) || !Read(c + 0x58U, b) || (a == 0U && b == 0U)) {
        gIdentityRejected.fetch_add(1U, std::memory_order_relaxed); return nullptr;
    }
    if (!Read(c + 0x34D8U, tree) || tree == 0U) {
        gTreeUnavailable.fetch_add(1U, std::memory_order_relaxed); return nullptr;
    }
    std::uintptr_t owner{};
    if (!Read(tree + 0xCU, owner) || owner != c) {
        gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed); return nullptr;
    }
    for (auto& actor : gActors) {
        if (actor.character != 0U) continue;
        actor = {};
        actor.character = c; actor.tree = tree; actor.runtimeId = id;
        actor.entityA = a; actor.entityB = b; actor.characterClass = type;
        actor.birth = gNextBirth++;
        gAdmitted.fetch_add(1U, std::memory_order_relaxed);
        gLastEntityA.store(a, std::memory_order_relaxed);
        gLastEntityB.store(b, std::memory_order_relaxed);
        gLastCharacterClass.store(type, std::memory_order_relaxed);
        actor.desired.nativeGeneration = gCurrent.nativeGeneration;
        actor.desired.sessionCookie = gCurrent.sessionCookie;
        actor.desired.worldGeneration = gCurrent.worldGeneration;
        actor.desired.entityA = a; actor.desired.entityB = b;
        actor.desired.characterClass = type; actor.desired.birth = actor.birth;
        actor.desired.revision = 1U;
        if (Client()) actor.faulted = !SuspendExistingTree(actor);
        for (auto& other : gActors) {
            if (&other != &actor && other.character != 0U &&
                other.entityA == a && other.entityB == b) {
                actor.duplicate = true; other.duplicate = true;
                InvalidatePairCharacter(other.character);
                gRefused.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        PublishPairSource(actor);
        return &actor;
    }
    gRefused.fetch_add(1U, std::memory_order_relaxed);
    return nullptr;
}
Actor* FindActorByIdentity(const std::uint32_t entityA, const std::uint32_t entityB) noexcept {
    for (auto& actor : gActors)
        if (actor.character != 0U && !actor.duplicate &&
            actor.entityA == entityA && actor.entityB == entityB) return &actor;
    return nullptr;
}
void CaptureInteraction(const std::uint32_t sourceA, const std::uint32_t sourceB,
    const std::uint32_t targetA, const std::uint32_t targetB,
    const std::uint64_t payload, const std::int32_t variant) noexcept {
    if ((sourceA == 0U && sourceB == 0U) || (targetA == 0U && targetB == 0U) ||
        variant < -1 || variant > 0xFFFF) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    // AISync authors only interactions initiated by an admitted authoritative
    // NPC. Player-originated impacts keep their existing Player/World channels.
    Actor* actor = FindActorByIdentity(sourceA, sourceB);
    if (actor == nullptr) {
        const auto source = FindCharacterByIdentity(sourceA, sourceB);
        if (source != 0U) actor = Admit(source);
    }
    if (actor == nullptr || actor->faulted || actor->duplicate) return;
    const auto mappedTargetA = targetA;
    const auto mappedTargetB = targetB;
    const auto target = FindCharacterByIdentity(mappedTargetA, mappedTargetB);
    if (target == 0U || target == actor->character) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    // Participant identity is pointer ownership, never Character class. The
    // local player is the engine-resolved THE_PLAYER Character; remote humans
    // are only the generation-certified PlayerSync representations. This also
    // keeps Android classes on the ordinary NPC identity path.
    const bool participant = target == LocalPlayer() || RegisteredPlayerRepresentation(target);
    const auto targetPeer = participant ? PeerForParticipant(target) : 0U;
    if (participant && targetPeer == 0U) {
        gParticipantTargetRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    float position[3]{};
    if (!Read(target + kPositionOffset, position[0]) ||
        !Read(target + kPositionOffset + 4U, position[1]) ||
        !Read(target + kPositionOffset + 8U, position[2]) ||
        !FiniteCoordinate(position[0]) || !FiniteCoordinate(position[1]) ||
        !FiniteCoordinate(position[2])) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    auto& state = actor->desired;
    ++state.interactionRevision;
    if (state.interactionRevision == 0U) state.interactionRevision = 1U;
    state.interactionTargetA = mappedTargetA;
    state.interactionTargetB = mappedTargetB;
    state.interactionFlags = InteractionActorIsSource | InteractionPairValid;
    if (participant) state.interactionFlags |= InteractionTargetIsParticipant;
    state.interactionTargetPeer = targetPeer;
    state.interactionPairRevision = state.interactionRevision;
    state.interactionTransitionRevision = 0U;
    state.interactionTransition = 0U;
    state.interactionTransitionFlags = 0U;
    state.interactionTransitionVariant = 0;
    state.interactionPayloadLow = static_cast<std::uint32_t>(payload);
    state.interactionPayloadHigh = static_cast<std::uint32_t>(payload >> 32U);
    state.interactionVariant = variant;
    for (std::size_t i = 0U; i < 3U; ++i) state.interactionTargetPosition[i] = position[i];
    state.stateFlags |= InteractionValid;
    actor->lastInteractionCapture = GetTickCount64();
    actor->deadTargetSince = 0U;
    gInteractionCaptured.fetch_add(1U, std::memory_order_relaxed);
    gLastInteractionTargetA.store(mappedTargetA, std::memory_order_relaxed);
    gLastInteractionTargetB.store(mappedTargetB, std::memory_order_relaxed);
    gLastInteractionTargetMode.store(participant ? 1U : 0U, std::memory_order_relaxed);
    gLastInteractionTargetPeer.store(targetPeer, std::memory_order_relaxed);
}
Actor* FindActorByActionManager(const std::uintptr_t manager) noexcept {
    if (manager == 0U) return nullptr;
    for (auto& actor : gActors) {
        std::uintptr_t candidate{};
        if (actor.character != 0U && Read(actor.character + 0x36CU, candidate) &&
            candidate == manager) return &actor;
    }
    std::uintptr_t character{};
    static_cast<void>(ForEachCharacter([&](const std::uintptr_t candidate) noexcept {
        std::uintptr_t actionManager{};
        if (Read(candidate + 0x36CU, actionManager) && actionManager == manager) {
            character = candidate;
            return false;
        }
        return true;
    }));
    return character == 0U ? nullptr : Admit(character);
}
void CapturePairedTransition(const std::uintptr_t manager, const std::uintptr_t target,
    const std::int32_t variant, const std::uint32_t transition,
    const std::uint32_t firstFlag, const std::uint32_t secondFlag) noexcept {
    if (variant < -1 || variant > 0xFFFF || firstFlag > 0xFFU || secondFlag > 0xFFU ||
        transition == 0U || transition > 2U) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    auto* actor = FindActorByActionManager(manager);
    std::uint32_t targetA{}, targetB{}, targetType{};
    if (actor == nullptr || actor->faulted || actor->duplicate || target == actor->character ||
        !CharacterIdentity(target, targetA, targetB, targetType)) return;
    const auto now = GetTickCount64();
    auto& state = actor->desired;
    const bool augmentsPair = (state.stateFlags & InteractionValid) != 0U &&
        (state.interactionFlags & InteractionPairValid) != 0U &&
        state.interactionTargetA == targetA && state.interactionTargetB == targetB &&
        actor->lastInteractionCapture != 0U && now - actor->lastInteractionCapture <= 250ULL;
    if (!augmentsPair) {
        // A transition alone is not proof of an admitted pair. Never fabricate
        // an attack with a zero payload or infer its victim by spatial proximity.
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    ++state.interactionRevision;
    if (state.interactionRevision == 0U) state.interactionRevision = 1U;
    state.interactionTransition = transition;
    state.interactionTransitionVariant = variant;
    state.interactionTransitionRevision = state.interactionRevision;
    state.interactionFlags |= InteractionCycleActive;
    state.interactionTransitionFlags = (firstFlag & 0xFFU) | ((secondFlag & 0xFFU) << 8U);
    actor->lastInteractionCapture = now;
    gPairedTransitionCaptured.fetch_add(1U, std::memory_order_relaxed);
    gLastInteractionTransition.store(transition, std::memory_order_relaxed);
    gLastInteractionTargetA.store(targetA, std::memory_order_relaxed);
    gLastInteractionTargetB.store(targetB, std::memory_order_relaxed);
}
bool CharacterReady(const Actor& actor) noexcept {
    std::uint8_t alive{};
    if (!Read(actor.character + 0x2FAU, alive) || alive == 0U) return false;
    __try {
        // Exactly the admission guard in FUN_007342A0; prevents proxy requests
        // during death/disabled-character/script exclusivity states.
        return reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(gBase + 0x0038B380U)(
            reinterpret_cast<void*>(actor.character + 0x280U)) == 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Release(Actor& actor) noexcept {
    if (PairSourceBusy(actor.character)) return true;
    if (actor.character != 0U && Client()) {
        static_cast<void>(character_direct_step_runtime::Stop(actor.character));
        // A Client role/task can outlive the BT leaf that created it. Always
        // retire that executor through the retail cleanup before authority is
        // released, otherwise an old task can keep fighting future PoseSync.
        static_cast<void>(ClearRoleTaskExecutor(actor));
    }
    actor.lastProxyRefresh = 0U;
    actor.completedRevision = 0U;
    // A timeout releases motion, not the acknowledgement of an irreversible
    // pair/transition. A later heartbeat must not replay the same attack.
    actor.completedActionRevision = 0U;
    actor.completedRoleTaskRevision = 0U;
    actor.completedLifecycleRevision = 0U;
    actor.lastInteractionAttempt = 0U;
    actor.interactionActionRevision = 0U;
    actor.interactionActionPathHash = 0U;
    return true;
}

bool ResolveLocalPairBinding(Actor& actor, const std::uintptr_t target) noexcept {
    const auto& state = actor.desired;
    if (actor.resolvedPairRevision == state.interactionPairRevision &&
        actor.resolvedPairTarget == target &&
        actor.resolvedSourceVariant == state.interactionTransitionVariant) return true;
    if (state.interactionTransition == 0U) return false;
    __try {
        std::uintptr_t animationManager{}, table{}, getter{};
        if (!Read(actor.character + 0x34B4U, animationManager) || animationManager == 0U ||
            !Read(animationManager, table) || !Read(table + 0x34U, getter) ||
            getter < gBase + 0x1000U || getter >= gBase + 0x00D9F000U) return false;
        using Definition = std::uintptr_t(__thiscall*)(void*, std::uint32_t);
        const auto definition = reinterpret_cast<Definition>(getter)(
            reinterpret_cast<void*>(animationManager),
            static_cast<std::uint32_t>(state.interactionTransitionVariant));
        if (definition == 0U) return false;
        // Native 0067DCD0 / 006DC310 use the chosen source definition's +20
        // bone name, then resolve it against the ACTUAL victim's skeleton.
        // interactionVariant is that local bone index, not an animation ID.
        char boneName[128]{};
        bool terminated = false;
        for (std::size_t i = 0U; i < sizeof(boneName); ++i) {
            if (!Read(definition + 0x20U + i, boneName[i])) return false;
            if (boneName[i] == '\0') { terminated = true; break; }
        }
        if (!terminated) return false;
        std::uint32_t definitionKind{};
        if (!Read(definition + 8U, definitionKind)) return false;
        // Machine contract is RET 8, including the second zero argument
        // omitted from Ghidra's prototype. Preserve the caller's zero value.
        using Bone = std::int32_t(__thiscall*)(void*, const char*, std::uint32_t);
        const auto attachment = reinterpret_cast<Bone>(gBase + 0x002B4BA0U)(
            reinterpret_cast<void*>(target + 0x3F0U), boneName, 0U);
        if (attachment < -1 || attachment > 0xFFFF) return false;
        actor.localPairAttachment = attachment;
        actor.localPairFlagsLow = attachment > 0 ? 1U : 0U;
        actor.localPairFlagsHigh = state.characterClass != 1U && definitionKind == 0U ? 1U : 0U;
        actor.resolvedPairRevision = state.interactionPairRevision;
        actor.resolvedPairTarget = target;
        actor.resolvedSourceVariant = state.interactionTransitionVariant;
        gLastHostAttachment.store(static_cast<std::uint32_t>(state.interactionVariant), std::memory_order_relaxed);
        gLastLocalAttachment.store(static_cast<std::uint32_t>(attachment), std::memory_order_relaxed);
        gLastTransitionVariant.store(static_cast<std::uint32_t>(state.interactionTransitionVariant), std::memory_order_relaxed);
        gInteractionBindingsResolved.fetch_add(1U, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}
bool ApplyAuthoritativeInteraction(Actor& actor, const ULONGLONG now) noexcept {
    if (PairSourceBusy(actor.character)) return true;
    const auto& state = actor.desired;
    if ((state.stateFlags & InteractionValid) == 0U ||
        actor.completedInteractionRevision == state.interactionRevision) return true;
    // A pair-only packet lacks the source definition needed to bind its bone.
    // Retain it until the following resolved transition arrives; no fabrication.
    if (state.interactionTransition == 0U) return true;
    // A completed Host attack is an acknowledgement, not an instruction to
    // start a stale kill when its last snapshot finally reaches this process.
    if (state.interactionTransition != 0U &&
        (state.interactionFlags & InteractionCycleActive) == 0U) {
        actor.completedInteractionRevision = state.interactionRevision;
        return true;
    }
    if (actor.lastInteractionAttempt != 0U && now - actor.lastInteractionAttempt < 250ULL) return true;
    actor.lastInteractionAttempt = now;
    const bool participant = (state.interactionFlags & InteractionTargetIsParticipant) != 0U;
    const auto target = participant ? ParticipantForPeer(state.interactionTargetPeer)
        : FindCharacterByIdentity(state.interactionTargetA, state.interactionTargetB);
    std::uint32_t targetA{}, targetB{}, targetType{};
    if (target == 0U || target == actor.character ||
        !CharacterIdentity(target, targetA, targetB, targetType)) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return true; // keep the event until the exact participant is bound
    }
    const auto actorIsSource = (state.interactionFlags & InteractionActorIsSource) != 0U;
    const auto sourceA = actorIsSource ? actor.entityA : targetA;
    const auto sourceB = actorIsSource ? actor.entityB : targetB;
    const auto destinationA = actorIsSource ? targetA : actor.entityA;
    const auto destinationB = actorIsSource ? targetB : actor.entityB;
    if (!ResolveLocalPairBinding(actor, target)) {
        gInteractionBindingRejected.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    // Change-driven interaction telemetry: periodic diagnostics expose the
    // authoritative participant PeerId (or zero for NPC) and the exact local
    // Character identity selected before the retail pair is invoked.
    gLastAppliedInteractionTargetPeer.store(
        participant ? state.interactionTargetPeer : 0U, std::memory_order_relaxed);
    gLastAppliedInteractionTargetA.store(targetA, std::memory_order_relaxed);
    gLastAppliedInteractionTargetB.store(targetB, std::memory_order_relaxed);
    const auto payload = static_cast<std::uint64_t>(actor.localPairFlagsLow) |
        (static_cast<std::uint64_t>(actor.localPairFlagsHigh) << 32U);
    __try {
        if (actor.completedPairRevision != state.interactionPairRevision) {
            // The latest-state mailbox can skip the old pair's inactive frame
            // when Host immediately attacks another player. Retire our prior
            // native pair before admitting the replacement source/target pair.
            if (actor.interactionOwnsCycle) {
                if (!character_direct_step_runtime::FinishReplicatedInteraction(actor.character)) return true;
                actor.interactionOwnsCycle = false;
            }
            std::uintptr_t root{}, manager{};
            const auto original = reinterpret_cast<CharacterInteraction>(
                gInteractionOriginal.load(std::memory_order_acquire));
            // Retail 0067DD44..0067DDA1 loads [global], then [root], then +90.
            // root+90 is a different object; using it could corrupt unrelated
            // memory even when the call did not raise an immediate exception.
            if (original == nullptr || !Read(gBase + kInteractionManagerGlobalRva, root) ||
                !Read(root, manager) || manager == 0U) return false;
            if ((original(reinterpret_cast<void*>(manager + 0x90U),
                    sourceA, sourceB, destinationA, destinationB, payload,
                    actor.localPairAttachment) & 0xFFU) == 0U) {
                gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
                return true; // no transition after a refused pair; bounded retry
            }
            actor.completedPairRevision = state.interactionPairRevision;
            if (participant && (targetA != state.interactionTargetA || targetB != state.interactionTargetB))
                gInteractionRemapped.fetch_add(1U, std::memory_order_relaxed);
        }
        if (state.interactionTransition != 0U &&
            actor.completedTransitionRevision != state.interactionTransitionRevision) {
            std::uintptr_t actionManager{};
            if (!Read(actor.character + 0x36CU, actionManager) || actionManager == 0U) return false;
            static_cast<void>(character_direct_step_runtime::Stop(actor.character));
            if (RegisteredPlayerRepresentation(target))
                static_cast<void>(character_direct_step_runtime::Stop(target));
            const auto firstFlag = state.interactionTransitionFlags & 0xFFU;
            const auto secondFlag = (state.interactionTransitionFlags >> 8U) & 0xFFU;
            // Machine-verified thiscall ABIs: A RET 0C, B RET 10. Ghidra's
            // unaff_retaddr is the first real argument (the target Character).
            if (state.interactionTransition == 1U) {
                const auto transition = reinterpret_cast<PairedTransitionA>(
                    gPairedTransitionAOriginal.load(std::memory_order_acquire));
                if (transition == nullptr) return false;
                transition(reinterpret_cast<void*>(actionManager), target,
                    state.interactionTransitionVariant, firstFlag);
            } else {
                const auto transition = reinterpret_cast<PairedTransitionB>(
                    gPairedTransitionBOriginal.load(std::memory_order_acquire));
                if (transition == nullptr) return false;
                transition(reinterpret_cast<void*>(actionManager), target,
                    state.interactionTransitionVariant, firstFlag, secondFlag);
            }
            actor.completedTransitionRevision = state.interactionTransitionRevision;
            actor.interactionOwnsCycle = true;
            gPairedTransitionApplied.fetch_add(1U, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    actor.completedInteractionRevision = state.interactionRevision;
    gInteractionApplied.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
bool PairedTransitionOwnsCycle(Actor& actor, const ULONGLONG) noexcept {
    // Do not reuse the source layout while a retail worker admits its victim.
    if (PairSourceBusy(actor.character)) return true;
    if (!actor.interactionOwnsCycle) return false;
    if (LocalClientVictimOwnsPair(actor.character)) return true;
    // BT revisions and a paired type alone do not prove an animation is still
    // running. A completed/failed local cycle must not hold PoseSync forever
    // while waiting for the next authoritative completion snapshot.
    std::uintptr_t cycle{};
    std::uint8_t state{};
    bool localActive{};
    if ((actor.desired.interactionFlags & InteractionCycleActive) != 0U &&
        (!ReadPairedCycleActivity(actor.character, cycle, state, localActive) || localActive)) return true;
    if (!character_direct_step_runtime::FinishReplicatedInteraction(actor.character)) return true;
    actor.interactionOwnsCycle = false;
    gPairedCycleReleased.fetch_add(1U, std::memory_order_relaxed);
    actor.lastProxyRefresh = 0U;
    actor.completedRevision = 0U;
    return false;
}
void CaptureInteractionCycle(Actor& actor) noexcept {
    auto& state = actor.desired;
    if ((state.stateFlags & InteractionValid) == 0U || state.interactionTransition == 0U) return;
    std::uintptr_t cycle{};
    std::uint8_t nativeState{};
    bool running{};
    const bool active = PairedCycleMatches(actor.character, state) &&
        ReadPairedCycleActivity(actor.character, cycle, nativeState, running) && running;
    if (active) state.interactionFlags |= InteractionCycleActive;
    else state.interactionFlags &= ~InteractionCycleActive;
}
void RecoverDeadParticipantTarget(Actor& actor, const ULONGLONG now) noexcept {
    if (PairSourceBusy(actor.character)) return;
    const auto& state = actor.desired;
    // The supplied Host remains in ATOMIC_MELEE_ATTACK_TARGET (EC B770),
    // status 2, for 25s after THE_PLAYER reaches Health.state 4. Only leave
    // that certified stale attack; never cancel traversal or a live victim.
    if (state.interactionTargetPeer == 0U || state.actionTypeRva != 0x00ECB770U ||
        !PairedCycleMatches(actor.character, state) ||
        actor.recoveredInteractionRevision == state.interactionRevision) {
        actor.deadTargetSince = 0U;
        return;
    }
    const auto target = ParticipantForPeer(state.interactionTargetPeer);
    std::uint16_t hp{};
    std::uint32_t health{};
    if (target == 0U || !Read(target + 0x33CU, hp) || hp != 0U ||
        !Read(target + 0x32CU, health) || health != 4U) {
        actor.deadTargetSince = 0U;
        return;
    }
    if (NativeVictimFlowRunning(actor.character, target)) {
        // Native fatal initialization has already set HP=0. This is still the
        // current death graph; do not retire/reconstruct its attacker early.
        actor.deadTargetSince = 0U;
        gPairedRecoveryDeferred.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (actor.deadTargetSince == 0U) actor.deadTargetSince = now;
    if (now - actor.deadTargetSince < kDeadTargetGracePeriod) return;
    bool survivor = LivingCharacter(LocalPlayer());
    for (std::uint32_t i = 0U; !survivor && i < gCurrent.participantCount; ++i)
        survivor = LivingCharacter(ParticipantForPeer(gCurrent.participants[i].peer));
    if (!survivor || !character_direct_step_runtime::FinishReplicatedInteraction(actor.character)) return;

    // A paired kill can leave both the selected BT path and the admitted role
    // task anchored to the participant which has just died. Retire both through
    // their certified retail cleanup helpers. On the very next manager frame,
    // normal arbitration rebuilds from current senses/roles and the scoped
    // multiplayer player context; AIMP still injects no target or action.
    const bool hadRoleTask = actor.lastRoleTask != 0U;
    const bool treeReset = SuspendExistingTree(actor);
    const bool roleReset = ClearRoleTaskExecutor(actor);
    if (roleReset) {
        if (hadRoleTask) gPostDeathRoleTaskResets.fetch_add(1U, std::memory_order_relaxed);
        actor.lastRoleTask = 0U;
    }
    if (treeReset && roleReset)
        gPostDeathDecisionResets.fetch_add(1U, std::memory_order_relaxed);
    else
        gPostDeathDecisionResetRejected.fetch_add(1U, std::memory_order_relaxed);

    actor.recoveredInteractionRevision = state.interactionRevision;
    actor.deadTargetSince = 0U;
    gDeadTargetRecoveries.fetch_add(1U, std::memory_order_relaxed);
}

bool ApplyAuthoritativePose(Actor& actor, const ULONGLONG now) noexcept {
    const auto& state = actor.desired;
    if (PairSourceBusy(actor.character)) return true;
    if ((state.stateFlags & PoseValid) == 0U) return false;
    if (actor.completedRevision == state.revision && actor.lastProxyRefresh != 0U &&
        now - actor.lastProxyRefresh < 250ULL) return true;
    std::string error;
    const bool moving = (state.stateFlags & Moving) != 0U;
    // This supplies only spatial/locomotion intent. CharacterDirectStep enters
    // the retail Move/Idle cycle, which owns gait selection and animation; no
    // animation phase, clip or bone transform is transported by AISync.
    const bool applied = moving
        ? character_direct_step_runtime::ArmWorldReplication(actor.character,
            state.velocityX, state.velocityZ, 1.1F,
            state.position[0], state.position[1], state.position[2], state.yaw,
            state.moveType, 0xFFFFFFFFU, error)
        : character_direct_step_runtime::HoldWorldReplicationIdle(actor.character,
            state.position[0], state.position[1], state.position[2], state.yaw,
            state.moveType, error);
    if (!applied || !character_direct_step_runtime::UseAIReplicationProfile(actor.character)) {
        gPoseRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    actor.completedRevision = state.revision;
    actor.lastProxyRefresh = now;
    gPoseApplied.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
bool SameState(const State& a, const State& b) noexcept {
    if (a.stateFlags != b.stateFlags || a.moveType != b.moveType ||
        a.actionTypeRva != b.actionTypeRva || a.actionPathHash != b.actionPathHash ||
        a.actionRevision != b.actionRevision || a.actionStatus != b.actionStatus ||
        a.actionDepth != b.actionDepth ||
        a.roleTaskRevision != b.roleTaskRevision || a.roleTaskTypeRva != b.roleTaskTypeRva ||
        a.lifecycleRevision != b.lifecycleRevision || a.lifecycleFlags != b.lifecycleFlags ||
        a.interactionRevision != b.interactionRevision ||
        a.interactionTargetA != b.interactionTargetA ||
        a.interactionTargetB != b.interactionTargetB ||
        a.interactionFlags != b.interactionFlags ||
        a.interactionTransition != b.interactionTransition ||
        a.interactionTransitionFlags != b.interactionTransitionFlags ||
        a.interactionPayloadLow != b.interactionPayloadLow ||
        a.interactionPayloadHigh != b.interactionPayloadHigh ||
        a.interactionVariant != b.interactionVariant) return false;
    if (a.interactionTargetPeer != b.interactionTargetPeer ||
        a.interactionTransitionVariant != b.interactionTransitionVariant ||
        a.interactionPairRevision != b.interactionPairRevision ||
        a.interactionTransitionRevision != b.interactionTransitionRevision) return false;
    for (std::size_t i = 0U; i < 3U; ++i)
        if (std::abs(a.position[i] - b.position[i]) > 0.01F ||
            std::abs(a.interactionTargetPosition[i] - b.interactionTargetPosition[i]) > 0.01F)
            return false;
    return std::abs(a.yaw - b.yaw) <= 0.01F &&
        std::abs(a.velocityX - b.velocityX) <= 0.05F &&
        std::abs(a.velocityZ - b.velocityZ) <= 0.05F;
}
void Publish(Actor& actor) noexcept {
    if (PairSourceBusy(actor.character)) return;
    const auto now = GetTickCount64();
    if (now - actor.lastSend < kSendPeriod) return;
    CaptureInteractionCycle(actor);
    if ((actor.desired.stateFlags & InteractionValid) != 0U &&
        (actor.desired.interactionFlags & InteractionCycleActive) == 0U &&
        actor.lastInteractionCapture != 0U && now - actor.lastInteractionCapture > kTimeout) {
        actor.desired.stateFlags &= ~InteractionValid;
        actor.desired.interactionFlags = 0U;
        actor.desired.interactionTransition = 0U;
        actor.desired.interactionTransitionFlags = 0U;
    }
    CaptureWithdrawState(actor);
    CapturePose(actor, now);
    CaptureActiveAction(actor);
    CaptureRoleTask(actor);
    const bool changed = !SameState(actor.desired, actor.previous);
    if (!changed && now - actor.lastSend < kRefreshPeriod) return;
    auto state = actor.desired;
    if (changed) ++state.revision;
    const auto write = gWrite.load(std::memory_order_relaxed);
    if (write - gRead.load(std::memory_order_acquire) >= kRing) {
        gDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    actor.desired.revision = state.revision;
    actor.previous = state; actor.lastSend = now;
    gOutgoing[write % kRing] = state;
    gWrite.store(write + 1U, std::memory_order_release);
    gObserved.fetch_add(1U, std::memory_order_relaxed);
}

// BehaviourManager is not guaranteed to tick while an Alien is withdrawn,
// backstage or traversing a vent. AISync authority must therefore not depend on
// that manager callback for transport liveness. The CharacterManager pump is
// certified every frame and keeps the last admitted Host actor authoritative
// while its AI manager is dormant.
void PublishDormantHostActor(Actor& actor, const ULONGLONG now,
    const bool characterPresent) noexcept {
    if (PairSourceBusy(actor.character) || now - actor.lastSend < kRefreshPeriod) return;

    if (characterPresent) CaptureWithdrawState(actor);
    if (!actor.hostDormant) {
        actor.hostDormant = true;
        MarkLifecycle(actor, LifecycleSpatialDiscontinuity |
            (actor.traversalAction ? LifecycleTraversalAction : 0U));
    } else {
        RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
    }

    // If the same Character remains registered (usual backstage path), keep
    // its actual authoritative transform fresh even though BehaviourManager is
    // sleeping. If retail temporarily removes it from the active registry,
    // retain the last safe pose solely as an authority heartbeat.
    if (characterPresent) {
        CapturePose(actor, now);
    }
    actor.desired.stateFlags &= ~Moving;
    actor.desired.velocityX = 0.0F;
    actor.desired.velocityZ = 0.0F;

    const auto changed = !SameState(actor.desired, actor.previous);
    auto state = actor.desired;
    if (changed) ++state.revision;
    const auto write = gWrite.load(std::memory_order_relaxed);
    if (write - gRead.load(std::memory_order_acquire) >= kRing) {
        gDropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    actor.desired.revision = state.revision;
    actor.previous = state;
    actor.lastSend = now;
    gOutgoing[write % kRing] = state;
    gWrite.store(write + 1U, std::memory_order_release);
    gObserved.fetch_add(1U, std::memory_order_relaxed);
}

void PumpHostAuthority() noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Host || !Stable()) return;
    const auto now = GetTickCount64();
    CacheLivingPlayerAlliance();

    for (auto& actor : gActors) {
        if (actor.character == 0U || actor.duplicate || actor.faulted) continue;
        if (actor.lastManagerUpdate != 0U &&
            now - actor.lastManagerUpdate <= kManagerDormancyPeriod) continue;

        // Resolve through stable retail identity rather than trusting a stale
        // Character pointer. This also detects the rare case where a backstage
        // transition replaces the Character object while retaining the same
        // logical NPC identity.
        const auto current = FindCharacterByIdentity(actor.entityA, actor.entityB);
        if (current == actor.character) {
            PublishDormantHostActor(actor, now, true);
            continue;
        }
        if (current == 0U) {
            PublishDormantHostActor(actor, now, false);
            continue;
        }

        // Same logical NPC, new Character object. Re-admit it with a new birth
        // so Clients perform one lifecycle barrier/snap instead of remaining
        // bound to the stale pre-vent instance.
        const auto previousActor = actor;
        actor = {};
        auto* rebound = Admit(current);
        if (rebound == nullptr) {
            actor = previousActor;
            PublishDormantHostActor(actor, now, false);
            continue;
        }
        rebound->lastManagerUpdate = now;
        MarkLifecycle(*rebound, LifecycleSpatialDiscontinuity);
        Publish(*rebound);
    }
}

// Interior hook at 0073433E. The original preamble 007342A0..0073433D still
// runs, including +4234/+4238 updates and Character housekeeping. EDI is on
// the retail stack at this point. Both paths restore all registers/flags.
// Client skips ONLY 0073433E..007344D3 (senses/targeting/BT/roles/AI adapters),
// then takes the retail POP ESI / RET 8 epilogue at 007344D4.
int __cdecl BypassBranch(const std::uintptr_t manager) noexcept {
    if (gUpdating != nullptr && gUpdating->blocked) return 1;
    std::uintptr_t owner{};
    std::uint32_t type{};
    // ESI is the retail manager on every native thread. This decision reads
    // only generation-bound ownership atomics and native owner/class fields.
    return Read(manager + 0x128U, owner) &&
        (PairSourceBusy(owner) || PlayerRepresentation(owner) ||
            (Client() && Eligible(owner, type))) ? 1 : 0;
}
__declspec(naked) void BranchDetour() {
    __asm {
        pushfd
        pushad
        push esi
        call BypassBranch
        add esp, 4
        test eax, eax
        jnz bypass
        popad
        popfd
        jmp dword ptr [gBranchTrampoline]
    bypass:
        popad
        popfd
        pop edi
        jmp dword ptr [gBranchExit]
    }
}
bool RecordMatches(const std::uintptr_t record, const std::uintptr_t owner,
    const std::uintptr_t target) noexcept {
    std::uintptr_t recordOwner{}, recordTarget{};
    std::uint32_t birth{}, currentBirth{};
    // FUN_00783B00 returns Character+50; retain its lifetime check when reading.
    return Read(record + 8U, recordOwner) && recordOwner == owner &&
        Read(record + 0x42CU, recordTarget) && recordTarget == target &&
        Read(record + 0x430U, birth) && Read(target + 0x50U, currentBirth) && birth == currentBirth;
}
bool ReadCharacterExecution(const std::uintptr_t target, ParticipantProbe& sample) noexcept {
    __try {
        std::uintptr_t table{}, getter{};
        if (!Read(target, table) || !Read(table + 0x1CU, getter) ||
            getter < gBase + 0x1000U || getter >= gBase + 0x00D9E13BU) return false;
        // FUN_0078B380 reads the animated Character returned by vfunc+1C,
        // not necessarily the logical Character at the participant address.
        const auto animated = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(
            reinterpret_cast<void*>(target));
        return animated != 0U && Read(animated + 0x2CF4U, sample.executionEnabled) &&
            Read(animated + 0x2CD0U + 0x20U, sample.executionStateA) &&
            Read(animated + 0x2CD0U + 0xE4U, sample.executionStateB) &&
            Read(animated + 0x2F90U, sample.executionPhase);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ProbeParticipantSignalRegistry(const std::uintptr_t target, ParticipantProbe& sample) noexcept {
    std::uintptr_t resources{};
    std::uint32_t entityA{}, entityB{}, type{};
    if (!Read(gBase + 0x0134EF40U, resources) || resources == 0U ||
        !CharacterIdentity(target, entityA, entityB, type)) return false;
    const std::array<std::uint32_t, 2U> identity{entityA, entityB};
    __try {
        const auto retail = reinterpret_cast<SenseCharacterResolver>(gBase + 0x002545C0U)(
            reinterpret_cast<void*>(resources), identity.data());
        sample.signalRegistryFound = retail != 0U;
        sample.signalRegistryMatches = retail == target;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void SampleParticipantPerception(Actor& actor, const std::uintptr_t manager, const ULONGLONG now) noexcept {
    if (gCurrent.participantCount == 0U || now - actor.lastParticipantProbe < kParticipantProbePeriod) return;
    actor.lastParticipantProbe = now;
    // Rotate peers, one sample per actor per second. All loops and the queue
    // are bounded; diagnostics never allocate, log, or wait on this thread.
    const auto index = actor.participantProbeCursor++ % gCurrent.participantCount;
    const auto peer = gCurrent.participants[index].peer;
    const auto target = ParticipantForPeer(peer);
    if (target == 0U) return;
    ParticipantProbe sample{};
    sample.sessionCookie = gCurrent.sessionCookie; sample.peer = peer; sample.tick = now;
    sample.nativeGeneration = gCurrent.nativeGeneration; sample.worldGeneration = gCurrent.worldGeneration;
    sample.entityA = actor.entityA; sample.entityB = actor.entityB; sample.characterClass = actor.characterClass;
    sample.living = LivingCharacter(target);
    sample.signalRegistryReadable = ProbeParticipantSignalRegistry(target, sample);
    sample.visibilityFlagsReadable = Read(actor.character + 0x2DAU, sample.ownerHidden) &&
        Read(target + 0x2DAU, sample.targetHidden);
    sample.pairReadable = ParticipantPairBusy(target, sample.paired);
    sample.executionReadable = ReadCharacterExecution(target, sample);
    if (sample.executionReadable) {
        sample.retailEligibility = sample.executionEnabled == 0U ||
            ((sample.executionStateA == 9U || sample.executionStateB == 9U) && sample.executionPhase == 3U)
            ? 1U : 0U;
    }
    // The manager counter is entry-only; its retail execution guard can
    // still stop the decision slice. Report that separately from target gates.
    ParticipantProbe ownerExecution{};
    sample.ownerExecutionReadable = ReadCharacterExecution(actor.character, ownerExecution);
    if (sample.ownerExecutionReadable) {
        sample.ownerRetailEligibility = ownerExecution.executionEnabled == 0U ||
            ((ownerExecution.executionStateA == 9U || ownerExecution.executionStateB == 9U) &&
                ownerExecution.executionPhase == 3U) ? 1U : 0U;
    }
    std::uintptr_t characterTemplate{};
    sample.ownerGatesReadable = Read(actor.character + 0x2FAU, sample.ownerEnabled) &&
        Read(actor.character + 0x64U, characterTemplate) && characterTemplate != 0U &&
        Read(characterTemplate + 0x1CU, sample.ownerSenseBlocked);
    std::uintptr_t senses{}, sensesOwner{}, node{};
    if (Read(manager + 0xF4U, senses) && senses != 0U &&
        Read(senses + 0x8CU, sensesOwner) && sensesOwner == actor.character && Read(senses + 0xCU, node)) {
        sample.sensesReadable = Read(senses + 0xC5U, sample.sensesDisabled);
        for (std::size_t i = 0U; node != 0U && i < 1024U; ++i) {
            std::uintptr_t record{}, next{};
            if (!Read(node, record) || !Read(node + 8U, next)) { sample.sensesReadable = false; break; }
            if (RecordMatches(record, actor.character, target)) {
                sample.recordPresent = true;
                sample.recordReadable = Read(record + 0x490U, sample.recordActive) &&
                    Read(record + 0x491U, sample.recordSelected) && Read(record + 0x493U, sample.recordForced) &&
                    Read(record + 0x474U, sample.sightInput) && std::isfinite(sample.sightInput) &&
                    Read(record + 0x478U, sample.sightTimestamp) && std::isfinite(sample.sightTimestamp) &&
                    Read(record + 0x434U, sample.primarySenseIndex) && Read(record + 0x438U, sample.recentSenseIndex);
                for (std::size_t sense = 0U; sense < 8U; ++sense) {
                    std::uint8_t active{};
                    if (!Read(record + 0x88U + sense * 0x74U, active)) { sample.recordReadable = false; break; }
                    if (active != 0U) sample.activeSenseMask |= 1U << sense;
                }
                node = 0U;
                break;
            }
            node = next;
        }
        if (node != 0U) sample.sensesReadable = false;
    }
    // Read the ordinary selected-target record (FUN_0040A3D0), not a BT task
    // key or THE_PLAYER projection. Observation has no effect on arbitration.
    std::uintptr_t selection{}, selectedRecord{}, selectedTarget{};
    if (Read(actor.character + 0x34A8U, selection) && selection != 0U && Read(selection + 0x10U, selectedRecord)) {
        sample.selectedReadable = selectedRecord == 0U;
        if (selectedRecord != 0U && Read(selectedRecord + 0x42CU, selectedTarget) &&
            selectedTarget != 0U && RecordMatches(selectedRecord, actor.character, selectedTarget)) {
            sample.selectedReadable = true;
            sample.selectedPresent = true;
            sample.selectedPeer = PeerForParticipant(selectedTarget);
        }
    }
    // Generic threat-manager entries: +18/count+1C, stride 6C, Character+4,
    // native state+58 (FUN_0071AEC0 / FUN_0071AFE0). No state is rewritten.
    std::uintptr_t threats{}, entries{};
    std::uint32_t count{};
    if (Read(actor.character + 0x34E0U, threats) && threats != 0U &&
        Read(threats + 0x18U, entries) && Read(threats + 0x1CU, count) &&
        count <= 256U && (count == 0U || entries != 0U)) {
        sample.threatReadable = true;
        for (std::uint32_t i = 0U; i < count; ++i) {
            std::uintptr_t entryTarget{};
            const auto entry = entries + i * 0x6CU;
            if (!Read(entry + 4U, entryTarget)) { sample.threatReadable = false; break; }
            if (entryTarget != target) continue;
            sample.threatPresent = true;
            sample.threatReadable = Read(entry + 0x58U, sample.threatState);
            break;
        }
    }
    const auto write = gParticipantProbeWrite.load(std::memory_order_relaxed);
    if (write - gParticipantProbeRead.load(std::memory_order_acquire) >= kParticipantProbeRing) {
        gParticipantPerceptionDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    gParticipantProbes[write % kParticipantProbeRing] = sample;
    gParticipantProbeWrite.store(write + 1U, std::memory_order_release);
    gParticipantPerceptionSamples.fetch_add(1U, std::memory_order_relaxed);
}
void __fastcall UpdateDetour(void* manager, void*, std::uint32_t previousTime, float delta) noexcept {
    const auto original = reinterpret_cast<ManagerUpdate>(gUpdateOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (!EngineThread()) { original(manager, previousTime, delta); return; }

    std::uintptr_t c{};
    if (!Read(reinterpret_cast<std::uintptr_t>(manager) + 0x128U, c) || c == 0U) {
        if (OwnsAI()) gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed);
        original(manager, previousTime, delta); return;
    }

    // PlayerSync Characters are retail NPC-backed presentation proxies. They
    // must never run an autonomous retail brain on ANY peer: PlayerSync owns
    // their locomotion/equipment/actions, while the local BT would otherwise
    // choose targets/scripts independently. Keep Character housekeeping but
    // bypass the same decision slice used by AISync clients. This remains
    // active even when AISync traffic is isolated/off.
    if (PlayerRepresentation(c)) {
        Actor playerProxy{};
        playerProxy.character = c;
        playerProxy.blocked = true;
        auto* previous = gUpdating;
        gUpdating = &playerProxy;
        gSuspended.fetch_add(1U, std::memory_order_relaxed);
        original(manager, previousTime, delta);
        gUpdating = previous;
        return;
    }

    if (!OwnsAI()) { original(manager, previousTime, delta); return; }
    if (!Client() && !Stable()) { original(manager, previousTime, delta); return; }
    gManagerUpdates.fetch_add(1U, std::memory_order_relaxed);

    Actor* actor = Admit(c);
    // A capacity/key refusal must NOT silently re-enable an eligible Client brain.
    Actor refused{};
    std::uint32_t type{};
    if (actor == nullptr && Client() && Eligible(c, type)) {
        refused.character = c; refused.blocked = true; actor = &refused;
    }
    auto* previous = gUpdating;
    const auto previousQueryOwner = gAiPlayerQueryOwner;
    gUpdating = actor;
    if (actor != nullptr) {
        actor->blocked = Client();
        if (gRole.load(std::memory_order_acquire) == Role::Host) {
            gAiPlayerQueryOwner = actor->character;
            // Preserve the retail player's pre-death alliance before a kill can
            // move THE_PLAYER to the dead alliance used by the solo game.
            CacheLivingPlayerAlliance();
        }
        if (actor->blocked) gSuspended.fetch_add(1U, std::memory_order_relaxed);
        else RecoverDeadParticipantTarget(*actor, GetTickCount64());
    }
    {
        AiPlayerSlotScope playerContext(actor != nullptr && !actor->blocked ? actor->character : 0U);
        original(manager, previousTime, delta);
    }
    gAiPlayerQueryOwner = previousQueryOwner;
    gUpdating = previous;
    if (actor == nullptr || actor == &refused || actor->character != c) return;
    if (actor->duplicate) { static_cast<void>(Release(*actor)); return; }
    if (gRole.load(std::memory_order_acquire) == Role::Host) {
        const auto now = GetTickCount64();
        if (actor->hostDormant) {
            actor->hostDormant = false;
            CaptureWithdrawState(*actor);
            MarkLifecycle(*actor, LifecycleSpatialDiscontinuity |
                (actor->traversalAction ? LifecycleTraversalAction : 0U));
            // Do not let the dormant heartbeat defer the first live snapshot.
            actor->lastSend = 0U;
        }
        actor->lastManagerUpdate = now;
        SampleParticipantPerception(*actor, reinterpret_cast<std::uintptr_t>(manager), now);
        Publish(*actor);
    } else if (Client()) {
        // First-person and NPC victim graphs can finish at different times.
        // Keep retail housekeeping above, but do not cut the real player's
        // native death graph with Host completion/withdraw/pose convergence.
        if (LocalClientVictimOwnsPair(actor->character)) return;
        const auto now = GetTickCount64();
        const bool withdrawn = ProxyWithdrawn(actor->desired);
        if (!Stable() || gRole.load(std::memory_order_acquire) == Role::FrozenClient ||
            actor->faulted || now - actor->lastReceive > kTimeout ||
            (!withdrawn && ((actor->desired.stateFlags & PoseValid) == 0U ||
                !CharacterReady(*actor)))) {
            static_cast<void>(Release(*actor));
        } else if (!ApplyAuthoritativeWithdraw(*actor, actor->desired)) {
            static_cast<void>(Release(*actor));
        } else if (withdrawn) {
            // Host has no frontstage AI to converge against. Keep the proxy
            // quiesced and let the retail Alien withdraw controller own its
            // visibility/backstage presentation until Host returns.
            static_cast<void>(character_direct_step_runtime::Stop(actor->character));
            actor->lastProxyRefresh = now;
        } else if (!ApplyAuthoritativeInteraction(*actor, now) ||
            (!PairedTransitionOwnsCycle(*actor, now) &&
                !ApplyAuthoritativePose(*actor, now))) {
            static_cast<void>(Release(*actor));
        }
    }
}
void __fastcall PrepassDetour(void* context, void*) noexcept {
    std::uintptr_t manager{}, owner{};
    std::uint32_t type{};
    // FUN_00733850 assigns (Character+34AC)->+4 to the embedded AI manager.
    // FUN_00712020 invokes task arbitration BEFORE the normal manager frame.
    // Player representations are presentation-only on Host and Clients, so
    // their task arbitration is always suppressed.
    if (Read(reinterpret_cast<std::uintptr_t>(context) + 4U, manager) &&
        Read(manager + 0x128U, owner)) {
        if (PairSourceBusy(owner)) return;
        if (PlayerRepresentation(owner)) return;
        if (Client() && Eligible(owner, type)) return;
    }
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(gPrepassOriginal.load());
    if (original == nullptr) return;
    const auto previousQueryOwner = gAiPlayerQueryOwner;
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        owner != 0U && Eligible(owner, type)) gAiPlayerQueryOwner = owner;
    {
        AiPlayerSlotScope playerContext(gAiPlayerQueryOwner == owner ? owner : 0U);
        original(context);
    }
    gAiPlayerQueryOwner = previousQueryOwner;
}
void __fastcall CoordinatorDetour(void* tree, void*, std::uint32_t execute) noexcept {
    const auto original = reinterpret_cast<Coordinator>(gCoordinatorOriginal.load(std::memory_order_acquire));
    // CATHODE forced-target/relance also call this outside the ordinary AI frame.
    std::uintptr_t owner{};
    std::uint32_t type{};
    if (Read(reinterpret_cast<std::uintptr_t>(tree) + 0xCU, owner)) {
        if (PairSourceBusy(owner)) return;
        if (PlayerRepresentation(owner)) return;
        if (Client() && Eligible(owner, type)) return;
    }
    if (original == nullptr) return;
    const auto previousQueryOwner = gAiPlayerQueryOwner;
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        owner != 0U && Eligible(owner, type)) gAiPlayerQueryOwner = owner;
    {
        AiPlayerSlotScope playerContext(gAiPlayerQueryOwner == owner ? owner : 0U);
        original(tree, execute);
    }
    gAiPlayerQueryOwner = previousQueryOwner;
}

bool SuppressClientAIImpact(void* receiver) noexcept {
    if (!Client() || receiver == nullptr) return false;
    std::uintptr_t victim{};
    std::uint32_t type{};
    // Both certified Character impact receivers keep their owning Character at
    // +0x24. On a Client, AISync-owned NPC consequences are Host-authoritative:
    // local/remote weapon presentation may run, but it must not independently
    // damage or retarget the replicated AI.
    return Read(reinterpret_cast<std::uintptr_t>(receiver) + 0x24U, victim) &&
        victim != 0U && Eligible(victim, type);
}
void __fastcall CharacterImpactDetour(void* receiver, void*, int* hit) noexcept {
    const auto original = reinterpret_cast<CharacterImpact>(
        gCharacterImpactOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (SuppressClientAIImpact(receiver)) return;
    original(receiver, hit);
}
void __fastcall CharacterImpactSecondaryDetour(void* receiver, void*, int* hit) noexcept {
    const auto original = reinterpret_cast<CharacterImpact>(
        gCharacterImpactSecondaryOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (SuppressClientAIImpact(receiver)) return;
    original(receiver, hit);
}
bool RefuseDeadPairedTarget(const std::uintptr_t source, const std::uintptr_t target) noexcept {
    PairIdentity sourceIdentity{}, targetIdentity{};
    if (gRole.load(std::memory_order_acquire) != Role::Host ||
        !PublishedPairParticipant(target, targetIdentity) ||
        PublishedPairSource(source, sourceIdentity) == kActors || !TerminalDeadCharacter(target)) return false;
    gPairedDeadTargetRejected.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
std::uint32_t __fastcall InteractionDetour(void* manager, void*,
    const std::uint32_t sourceA, const std::uint32_t sourceB,
    const std::uint32_t targetA, const std::uint32_t targetB,
    const std::uint64_t payload, const std::int32_t variant) noexcept {
    const auto original = reinterpret_cast<CharacterInteraction>(
        gInteractionOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable()) {
        PairIdentity sourceIdentity{}, targetIdentity{};
        std::uintptr_t source{}, target{};
        // Use published exact GUIDs; no entity scan, proximity or species rule.
        for (const auto& binding : gPairSources)
            if (LoadPairBinding(binding, sourceIdentity) && sourceIdentity.entityA == sourceA &&
                sourceIdentity.entityB == sourceB) { source = sourceIdentity.character; break; }
        for (const auto& binding : gPairParticipants)
            if (LoadPairBinding(binding, targetIdentity) && targetIdentity.entityA == targetA &&
                targetIdentity.entityB == targetB) { target = targetIdentity.character; break; }
        if (RefuseDeadPairedTarget(source, target)) return 0U;
    }
    const auto result = original(manager, sourceA, sourceB, targetA, targetB, payload, variant);
    if ((result & 0xFFU) != 0U && gRole.load(std::memory_order_acquire) == Role::Host &&
        EngineThread() && Stable()) {
        CaptureInteraction(sourceA, sourceB, targetA, targetB, payload, variant);
    }
    return result;
}
void __fastcall PairedTransitionADetour(void* manager, void*,
    const std::uintptr_t target, const std::int32_t variant,
    const std::uint32_t firstFlag) noexcept {
    const auto original = reinterpret_cast<PairedTransitionA>(
        gPairedTransitionAOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{};
    if (Read(reinterpret_cast<std::uintptr_t>(manager) + 4U, source) &&
        RefuseDeadPairedTarget(source, target)) return;
    if (EngineThread() && RegisteredPlayerRepresentation(target))
        static_cast<void>(character_direct_step_runtime::Stop(target));
    original(manager, target, variant, firstFlag);
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable())
        CapturePairedTransition(reinterpret_cast<std::uintptr_t>(manager), target,
            variant, 1U, firstFlag, 0U);
}
void __fastcall PairedTransitionBDetour(void* manager, void*,
    const std::uintptr_t target, const std::int32_t variant,
    const std::uint32_t firstFlag, const std::uint32_t secondFlag) noexcept {
    const auto original = reinterpret_cast<PairedTransitionB>(
        gPairedTransitionBOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{};
    if (Read(reinterpret_cast<std::uintptr_t>(manager) + 4U, source) &&
        RefuseDeadPairedTarget(source, target)) return;
    if (EngineThread() && RegisteredPlayerRepresentation(target))
        static_cast<void>(character_direct_step_runtime::Stop(target));
    original(manager, target, variant, firstFlag, secondFlag);
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable())
        CapturePairedTransition(reinterpret_cast<std::uintptr_t>(manager), target,
            variant, 2U, firstFlag, secondFlag);
}
bool Hook(HookManager& hooks, const char* name, std::uintptr_t rva, void* detour,
    std::initializer_list<unsigned int> prefix, void** trampoline,
    std::atomic<void*>* published, std::string& error,
    const std::uint32_t relocatedAbsoluteRva = 0U) {
    MaskedBytes bytes;
    for (const auto byte : prefix) bytes.bytes.push_back(static_cast<std::uint8_t>(byte));
    if (relocatedAbsoluteRva != 0U) {
        // Retail HIGHLOW relocations adjust the
        // absolute SEH-handler operand in PUSH -1; PUSH handler. Match the
        // actual loaded address, without wildcarding the certified prefix.
        if (bytes.bytes.size() != 7U || bytes.bytes[0] != 0x6AU ||
            bytes.bytes[1] != 0xFFU || bytes.bytes[2] != 0x68U) {
            error = std::string(name) + ": unexpected relocated prefix shape";
            return false;
        }
        const auto absolute = static_cast<std::uint32_t>(gBase) + relocatedAbsoluteRva;
        std::memcpy(bytes.bytes.data() + 3U, &absolute, sizeof(absolute));
    }
    bytes.mask.assign(bytes.bytes.size(), 'x');
    HookRequest request;
    request.name = name; request.target = reinterpret_cast<void*>(gBase + rva);
    request.detour = detour; request.patchLength = bytes.bytes.size();
    request.expected = bytes; request.relocationSafePrefix = true;
    request.trampolineOut = trampoline; request.publishedTrampolineOut = published;
    return hooks.Install(request, error);
}

bool MatchRetailBytes(const std::uintptr_t rva, std::initializer_list<std::uint8_t> expected) noexcept {
    std::array<std::uint8_t, 64U> actual{};
    if (expected.size() > actual.size() || !crash_guard::TryCopy(actual.data(),
            reinterpret_cast<const void*>(gBase + rva), expected.size())) return false;
    return std::memcmp(actual.data(), expected.begin(), expected.size()) == 0;
}
bool ValidateRoleTaskCleanupAbi(std::string& error) {
    // Validate the ECX receiver, saved-register epilogue/RET 4, and the
    // explicit stack argument at all three direct retail CALL sites. These
    // bytes contain no PE HIGHLOW operands and remain valid after ASLR.
    if (!MatchRetailBytes(kRoleTaskCleanupRva + 0x18U,
            {0x53,0x56,0x8B,0xF1,0x8B,0x46,0x0C,0x8B,0x4E,0x08}) ||
        !MatchRetailBytes(kRoleTaskCleanupRva + 0x119U,
            {0x8B,0x4C,0x24,0x14,0x5F,0x5E,0x5B,0x64,0x89,0x0D,0x00,0x00,0x00,0x00,
             0x83,0xC4,0x14,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00366D54U,
            {0x8B,0x88,0x24,0x04,0x00,0x00,0x57,0xE8,0x30,0xAD,0xFF,0xFF}) ||
        !MatchRetailBytes(0x00366F74U,
            {0x57,0x8B,0xCB,0xE8,0x14,0xAB,0xFF,0xFF}) ||
        !MatchRetailBytes(0x0036735BU,
            {0x56,0xE8,0x2F,0xA7,0xFF,0xFF})) {
        error = "aisync role-task cleanup: certified ECX/stack argument/RET 4 bytes mismatch";
        return false;
    }
    return true;
}
bool HookParticipantSenseSignals(HookManager& hooks, std::string& error) {
    // FUN_00731060 and FUN_00730C30 are ECX + one argument, RET 4.
    // Their instruction-aligned prefixes contain no relative/ASLR operands.
    if (!Hook(hooks, "aisync_sense_event_scope", 0x00331060U,
            reinterpret_cast<void*>(&SenseEventDetour), {0x64,0xA1,0x00,0x00,0x00,0x00},
            &gSenseEventTrampoline, &gSenseEventOriginal, error) ||
        !Hook(hooks, "aisync_sound_event_scope", 0x00330C30U,
            reinterpret_cast<void*>(&SoundEventDetour), {0x83,0xEC,0x08,0x53,0x8B,0x5C,0x24,0x10},
            &gSoundEventTrampoline, &gSoundEventOriginal, error)) return false;
    // Every resolver CALL inside these two certified dispatchers, including
    // positive/lost sight and sound. Never redirect the global resolver itself.
    constexpr std::array<std::uintptr_t, 16U> calls{
        0x003310A5U, 0x0033112BU, 0x003311C8U, 0x003313CEU,
        0x0033141CU, 0x003314C2U, 0x00331551U, 0x003315D5U,
        0x00331657U, 0x00331705U, 0x0033177CU, 0x00331828U,
        0x00331918U, 0x00330C5AU, 0x00330E57U, 0x00330ECCU};
    for (const auto rva : calls) {
        CallRedirectRequest request;
        request.name = "aisync_participant_sense_identity_" + std::to_string(rva);
        request.callSite = reinterpret_cast<void*>(gBase + rva);
        request.expectedTarget = reinterpret_cast<void*>(gBase + 0x002545C0U);
        request.detour = reinterpret_cast<void*>(&SenseCharacterResolverDetour);
        if (!hooks.InstallCallRedirect(request, error)) return false;
    }
    return true;
}

bool HookParticipantPairedIdentity(HookManager& hooks, std::string& error) {
    // The shared initializer is used by both certified paired cycle families.
    // Complete prologue instructions have no relative or HIGHLOW operands.
    if (!MatchRetailBytes(0x0025FBBCU, {0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x0025FC6FU, {0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x0025FC7CU, {0xC2,0x08,0x00})) {
        error = "aisync paired initialization: certified ECX/two arguments/RET 8 bytes mismatch";
        return false;
    }
    if (!Hook(hooks, "aisync_paired_initialize_scope", 0x0025FB80U,
            reinterpret_cast<void*>(&PairedInitializeDetour), {0x55,0x8B,0x6C,0x24,0x08,0x56},
            &gPairedInitializeTrampoline, &gPairedInitializeOriginal, error)) return false;
    CallRedirectRequest character;
    character.name = "aisync_paired_character_identity";
    character.callSite = reinterpret_cast<void*>(gBase + 0x0025FB98U);
    character.expectedTarget = reinterpret_cast<void*>(gBase + 0x002544E0U);
    character.detour = reinterpret_cast<void*>(&PairedCharacterResolverDetour);
    if (!hooks.InstallCallRedirect(character, error)) return false;
    CallRedirectRequest animated;
    animated.name = "aisync_paired_animated_identity";
    animated.callSite = reinterpret_cast<void*>(gBase + 0x0025FC1EU);
    animated.expectedTarget = reinterpret_cast<void*>(gBase + 0x002545C0U);
    animated.detour = reinterpret_cast<void*>(&PairedAnimatedResolverDetour);
    return hooks.InstallCallRedirect(animated, error);
}

bool HookPairedVictimGuards(HookManager& hooks, std::string& error) {
    // Both the ABI and unsafe source-animation load are independently checked
    // against the certified executable before installing these two guards.
    if (!MatchRetailBytes(0x0025C49DU,
            {0x8B,0x44,0x24,0x24,0x8B,0x4C,0x24,0x20,0x8B,0x54,0x24,0x1C,0x50,0x51,0x52}) ||
        !MatchRetailBytes(0x0025C4DBU, {0xC2,0x0C,0x00}) ||
        !MatchRetailBytes(0x00275FA3U,
            {0x8B,0x86,0xB4,0x00,0x00,0x00,0x3B,0xC3,0x74,0x1D,0x8B,0x40,0x08}) ||
        !MatchRetailBytes(0x002763DFU, {0x5F,0x5E,0x5B,0x81,0xC4,0x88,0x00,0x00,0x00,0xC3})) {
        error = "aisync paired victim: certified RET 0C/ECX-only initializer bytes mismatch";
        return false;
    }
    if (!Hook(hooks, "aisync_paired_victim_transition", 0x0025C450U,
            reinterpret_cast<void*>(&PairedVictimTransitionDetour),
            {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedVictimTransitionTrampoline, &gPairedVictimTransitionOriginal, error,
            0x00D23FC1U)) return false;
    // MOVSD xmm0,[absolute time]; SUB ESP,88. The complete HIGHLOW operand
    // stays valid in the trampoline and is matched at its loaded ASLR address.
    MaskedBytes bytes;
    bytes.bytes = {0xF2,0x0F,0x10,0x05,0x00,0x00,0x00,0x00,
                   0x81,0xEC,0x88,0x00,0x00,0x00};
    const auto absolute = static_cast<std::uint32_t>(gBase + 0x012410A8U);
    std::memcpy(bytes.bytes.data() + 4U, &absolute, sizeof(absolute));
    bytes.mask.assign(bytes.bytes.size(), 'x');
    HookRequest request;
    request.name = "aisync_paired_victim_initialize";
    request.target = reinterpret_cast<void*>(gBase + 0x00275F10U);
    request.detour = reinterpret_cast<void*>(&PairedVictimInitializeDetour);
    request.patchLength = bytes.bytes.size(); request.expected = bytes;
    request.relocationSafePrefix = true;
    request.trampolineOut = &gPairedVictimInitializeTrampoline;
    request.publishedTrampolineOut = &gPairedVictimInitializeOriginal;
    if (!hooks.Install(request, error)) return false;
    if (!MatchRetailBytes(0x00378F2BU,
            {0x8B,0x46,0x30,0x8B,0x16,0x50,0x8D,0x4E,0x10,0x51,0x8B,0x8F,0x6C,0x03,0x00,0x00,0x52}) ||
        !MatchRetailBytes(0x00378F41U, {0x5F,0x5E,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00262C51U,
            {0x8B,0x4E,0x44,0x8B,0x11,0x8B,0x52,0x2C,0x8D,0x84,0x24,0x90,0x00,0x00,0x00,0x50}) ||
        !MatchRetailBytes(0x00262C6AU, {0x8B,0x4E,0x4C,0x8B,0x11,0x8B,0x42,0x04})) {
        error = "aisync native victim message: certified packet/dispatcher/RET 4 bytes mismatch";
        return false;
    }
    if (!Hook(hooks, "aisync_paired_victim_event", 0x00378EF0U,
            reinterpret_cast<void*>(&PairedVictimEventDetour), {0x56,0x57,0x8B,0x7C,0x24,0x0C},
            &gPairedVictimEventTrampoline, &gPairedVictimEventOriginal, error)) return false;
    if (!Hook(hooks, "aisync_paired_victim_prepare", 0x00262670U,
            reinterpret_cast<void*>(&PairedVictimPrepareDetour), {0x55,0x8B,0xEC,0x83,0xE4,0xF0},
            &gPairedVictimPrepareTrampoline, &gPairedVictimPrepareOriginal, error)) return false;
    gPairedVictimSubmitContinue = reinterpret_cast<void*>(gBase + 0x00262C6AU);
    if (!Hook(hooks, "aisync_paired_victim_native_message", 0x00262C61U,
            reinterpret_cast<void*>(&PairedVictimSubmitShim), {0x89,0x9C,0x24,0xF0,0x00,0x00,0x00,0xFF,0xD2},
            &gPairedVictimSubmitTrampoline, &gPairedVictimSubmitPublished, error)) return false;
    if (!MatchRetailBytes(0x00275F5EU, {0x8B,0x4E,0x04}) ||
        !MatchRetailBytes(0x0025B6B7U, {0x8B,0x4E,0x04,0x81,0xC1,0x80,0x02,0x00,0x00}) ||
        !MatchRetailBytes(0x0025B207U, {0x8D,0x88,0x80,0x02,0x00,0x00})) {
        error = "aisync paired availability: ESI owner/ECX logical-character mismatch";
        return false;
    }
    if (!MatchRetailBytes(0x0025B04FU, {0xFE,0x46,0x19}) ||
        !MatchRetailBytes(0x002627B1U, {0xE8,0x4A,0xA1,0x16,0x00}) ||
        !MatchRetailBytes(0x002627B6U, {0x8B,0x46,0x44,0x0F,0xB7,0x80,0x3C,0x03,0x00,0x00}) ||
        !MatchRetailBytes(0x00255B39U, {0x8B,0xF1}) ||
        !MatchRetailBytes(0x00255BCCU, {0x80,0x79,0x1A,0x4E,0x75,0x0C,0x8B,0x56,0x04,0x53,0x52}) ||
        !MatchRetailBytes(0x00255BDCU, {0xEB,0x06,0x89,0xAE,0xB0,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x0024E6D8U, {0x8B,0x43,0x4C}) ||
        !MatchRetailBytes(0x0024E6DFU, {0x83,0x78,0x04,0x0B}) ||
        !MatchRetailBytes(0x0024E8ABU, {0xC2,0x08,0x00})) {
        error = "aisync victim convergence: certified ESI/source animation/RET 8 bytes mismatch";
        return false;
    }
    CallRedirectRequest convergence;
    convergence.name = "aisync_paired_victim_convergence";
    convergence.callSite = reinterpret_cast<void*>(gBase + 0x00255BD7U);
    convergence.expectedTarget = reinterpret_cast<void*>(gBase + 0x0024E6C0U);
    convergence.detour = reinterpret_cast<void*>(&PairedVictimConvergenceShim);
    if (!hooks.InstallCallRedirect(convergence, error)) return false;
    constexpr std::array<std::uintptr_t, 3U> availabilityCalls{0x00275F75U, 0x0025B6C0U, 0x0025B20DU};
    for (const auto rva : availabilityCalls) {
        CallRedirectRequest call;
        call.name = "aisync_paired_victim_availability_" + std::to_string(rva);
        call.callSite = reinterpret_cast<void*>(gBase + rva);
        call.expectedTarget = reinterpret_cast<void*>(gBase + 0x0038B330U);
        call.detour = reinterpret_cast<void*>(&PairedVictimAvailabilityShim);
        if (!hooks.InstallCallRedirect(call, error)) return false;
    }
    return true;
}

bool HookParticipantEligibility(HookManager& hooks, std::string& error) {
    // Fixed, independently certified CALL sites. Never scan every byte of
    // .text or change all callers of a shared physical-state predicate.
    if (!MatchRetailBytes(0x0038B380U, {
            0x8BU,0x09U,0x8BU,0x01U,0x8BU,0x50U,0x1CU,0x56U,0xFFU,0xD2U,0x8BU,0xF0U,
            0x80U,0xBEU,0xF4U,0x2CU,0x00U,0x00U,0x00U,0x74U,0x1CU,0x8DU,0x8EU,0xD0U,
            0x2CU,0x00U,0x00U,0xE8U,0xC0U,0xE4U,0xEEU,0xFFU,0x84U,0xC0U,0x74U,0x09U,
            0x83U,0xBEU,0x90U,0x2FU,0x00U,0x00U,0x03U,0x74U,0x04U,0x33U,0xC0U,0x5EU,
            0xC3U,0xB8U,0x01U,0x00U,0x00U,0x00U,0x5EU,0xC3U}) ||
        !MatchRetailBytes(0x0032ACA3U,
            {0x8D,0x8D,0x80,0x02,0x00,0x00,0xE8,0xD2,0x06,0x06,0x00,0x84,0xC0,0x0F,0x85,0x1C,0x01,0x00,0x00}) ||
        !MatchRetailBytes(0x002CDB49U,
            {0x8D,0x89,0x80,0x02,0x00,0x00,0xE8,0x2C,0xD8,0x0B,0x00,0x84,0xC0,0x75,0x1C})) {
        error = "aisync participant eligibility: certified target-validation bytes mismatch";
        return false;
    }
    CallRedirectRequest candidate;
    candidate.name = "aisync_candidate_target_eligibility";
    candidate.callSite = reinterpret_cast<void*>(gBase + 0x0032ACA9U);
    candidate.expectedTarget = reinterpret_cast<void*>(gBase + 0x0038B380U);
    candidate.detour = reinterpret_cast<void*>(&CandidateEligibilityDetour);
    if (!hooks.InstallCallRedirect(candidate, error)) return false;
    CallRedirectRequest record;
    record.name = "aisync_record_target_eligibility";
    record.callSite = reinterpret_cast<void*>(gBase + 0x002CDB4FU);
    record.expectedTarget = reinterpret_cast<void*>(gBase + 0x0038B380U);
    record.detour = reinterpret_cast<void*>(&RecordEligibilityDetour);
    return hooks.InstallCallRedirect(record, error);
}

bool HookThePlayerResolver(HookManager& hooks, std::string& error) {
    // FUN_0078BAF0 starts with A1 <GlobalStateRoot>. The complete displaced
    // instruction carries a PE HIGHLOW absolute already relocated in memory,
    // so copying those five bytes to the trampoline is safe.
    MaskedBytes bytes;
    bytes.bytes = {0xA1U, 0U, 0U, 0U, 0U};
    const auto absolute = static_cast<std::uint32_t>(gBase + kInteractionManagerGlobalRva);
    std::memcpy(bytes.bytes.data() + 1U, &absolute, sizeof(absolute));
    bytes.mask.assign(bytes.bytes.size(), 'x');

    HookRequest request;
    request.name = "aisync_ai_player_context";
    request.target = reinterpret_cast<void*>(gBase + 0x0038BAF0U);
    request.detour = reinterpret_cast<void*>(&ThePlayerDetour);
    request.patchLength = 5U;
    request.expected = bytes;
    request.relocationSafePrefix = true;
    request.trampolineOut = &gThePlayerTrampoline;
    request.publishedTrampolineOut = &gThePlayerOriginal;
    return hooks.Install(request, error);
}

} // namespace

bool Install(const std::uintptr_t base, HookManager& hooks, LevelGenerationTracker& generations,
    GameThreadDispatcher& dispatcher, std::string& error) {
    if (base < 0x10000U || !hooks.MutationAllowed() ||
        !character_direct_step_runtime::Installed()) {
        error = "AISync V2 requires the certified retail x86 executable and the generic Character replication runtime.";
        return false;
    }
    gBase = base; gGenerations = &generations; gDispatcher = &dispatcher;
    if (!ValidateRoleTaskCleanupAbi(error)) return false;
    gBranchExit = reinterpret_cast<void*>(base + 0x003344D4U);
    const auto first = hooks.InstalledCount();
    // Complete instructions; no relative CALL/Jcc in a prefix. The PUSH
    // handler operands below are certified PE HIGHLOW relocations; their loaded
    // absolute values remain valid when copied into the trampoline.
    const bool installed =
        HookParticipantSenseSignals(hooks, error) &&
        HookParticipantPairedIdentity(hooks, error) &&
        HookPairedVictimGuards(hooks, error) &&
        HookParticipantEligibility(hooks, error) &&
        Hook(hooks, "aisync_participant_record_validation", 0x002CDB00U,
            reinterpret_cast<void*>(&RecordInvalidDetour),
            {0x56,0x8B,0xF1,0x83,0xBE,0x2C,0x04,0x00,0x00,0x00},
            &gRecordInvalidTrampoline, &gRecordInvalidOriginal, error) &&
        Hook(hooks, "aisync_brain_branch", 0x0033433EU, reinterpret_cast<void*>(&BranchDetour),
            {0x8B,0x8E,0xF4,0x00,0x00,0x00,0x5F}, &gBranchTrampoline, &gBranchPublished, error) &&
        Hook(hooks, "aisync_manager_update", 0x003342A0U, reinterpret_cast<void*>(&UpdateDetour),
            {0x56,0x8B,0xF1,0x8B,0x8E,0x28,0x01,0x00,0x00}, &gUpdateTrampoline, &gUpdateOriginal, error) &&
        Hook(hooks, "aisync_coordinator", 0x0032B880U, reinterpret_cast<void*>(&CoordinatorDetour),
            {0x83,0xEC,0x08,0x53,0x55}, &gCoordinatorTrampoline, &gCoordinatorOriginal, error) &&
        Hook(hooks, "aisync_task_arbitration", 0x00312020U, reinterpret_cast<void*>(&PrepassDetour),
            {0x6A,0xFF,0x68,0xE8,0xC2,0x12,0x01}, &gPrepassTrampoline, &gPrepassOriginal, error,
            0x00D2C2E8U) &&
        Hook(hooks, "aisync_character_interaction", 0x00254820U,
            reinterpret_cast<void*>(&InteractionDetour),
            {0x83,0xEC,0x24,0x53,0x8B,0x5C,0x24,0x2C,0x56,0x8B,0xF1},
            &gInteractionTrampoline, &gInteractionOriginal, error) &&
        Hook(hooks, "aisync_paired_transition_a", 0x0025C4E0U,
            reinterpret_cast<void*>(&PairedTransitionADetour),
            {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedTransitionATrampoline, &gPairedTransitionAOriginal, error,
            0x00D23FC1U) &&
        Hook(hooks, "aisync_paired_transition_b", 0x0025BF60U,
            reinterpret_cast<void*>(&PairedTransitionBDetour),
            {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedTransitionBTrampoline, &gPairedTransitionBOriginal, error,
            0x00D23FC1U) &&
        Hook(hooks, "aisync_character_impact", 0x003CDCF0U,
            reinterpret_cast<void*>(&CharacterImpactDetour),
            {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x64},
            &gCharacterImpactTrampoline, &gCharacterImpactOriginal, error) &&
        Hook(hooks, "aisync_character_impact_secondary", 0x003CE020U,
            reinterpret_cast<void*>(&CharacterImpactSecondaryDetour),
            {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x24},
            &gCharacterImpactSecondaryTrampoline, &gCharacterImpactSecondaryOriginal, error) &&
        Hook(hooks, "aisync_participant_candidates", 0x0032AC00U,
            reinterpret_cast<void*>(&SensesCandidatesDetour),
            {0x6A,0xFF,0x68,0x13,0xAD,0x12,0x01},
            &gCandidatesTrampoline, &gCandidatesOriginal, error, 0x00D2AD13U) &&
        Hook(hooks, "aisync_participant_hostility", 0x0037A800U,
            reinterpret_cast<void*>(&HostilityDetour),
            {0x8B,0x54,0x24,0x04,0x85,0xD2},
            &gHostilityTrampoline, &gHostilityOriginal, error) &&
        HookThePlayerResolver(hooks, error);
    if (!installed) {
        std::string rollback;
        static_cast<void>(hooks.UninstallFrom(first, rollback));
        if (!rollback.empty()) error += "; rollback: " + rollback;
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    return true;
}
bool Configure(const std::span<const std::byte> payload) noexcept {
    Configuration value{};
    if (payload.size() != sizeof(value) || !Installed()) return false;
    std::memcpy(&value, payload.data(), sizeof(value));
    if (value.version != 2U || value.participantCount > kActors || value.reserved != 0U ||
        static_cast<std::uint32_t>(value.role) > 3U ||
        (value.role != Role::Retail && value.sessionCookie == 0U) ||
        (value.role != Role::Retail && value.localPeer == 0U) ||
        (value.role != Role::Retail && value.role != Role::FrozenClient &&
            (value.nativeGeneration == 0U || value.worldGeneration == 0U))) return false;
    for (std::uint32_t i = 0U; i < value.participantCount; ++i) {
        const auto& binding = value.participants[i];
        if (binding.peer == 0U || binding.peer == value.localPeer ||
            binding.character < 0x10000U || binding.reserved != 0U) return false;
        for (std::uint32_t j = 0U; j < i; ++j)
            if (value.participants[j].peer == binding.peer ||
                value.participants[j].character == binding.character) return false;
    }
    std::lock_guard lock(gMailboxMutex);
    if (std::memcmp(&gRequested, &value, sizeof(value)) == 0) return true;
    const bool contextChanged = value.role != gRequested.role ||
        value.nativeGeneration != gRequested.nativeGeneration ||
        value.worldGeneration != gRequested.worldGeneration ||
        value.sessionCookie != gRequested.sessionCookie || value.localPeer != gRequested.localPeer;
    gRequested = value; gConfigurationPending = true;
    if (contextChanged) gIncomingUsed.fill(false);
    return true;
}
bool QueueRemote(const std::span<const std::byte> payload) noexcept {
    State value{};
    if (payload.size() != sizeof(value) || !Installed()) return false;
    std::memcpy(&value, payload.data(), sizeof(value));
    if (!Valid(value)) return false;
    std::lock_guard lock(gMailboxMutex);
    const auto& configuration = gRequested;
    if (configuration.role != Role::Client || value.sessionCookie != configuration.sessionCookie ||
        value.worldGeneration != configuration.worldGeneration) {
        gDropped.fetch_add(1U, std::memory_order_relaxed); return true;
    }
    gRemoteQueued.fetch_add(1U, std::memory_order_relaxed);
    std::size_t free = kActors;
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (!gIncomingUsed[i]) { if (free == kActors) free = i; continue; }
        const auto& old = gIncoming[i];
        if (old.entityA != value.entityA || old.entityB != value.entityB) continue;
        if (value.birth < old.birth || (value.birth == old.birth && value.revision < old.revision)) return true;
        gIncoming[i] = value; gIncomingAt[i] = GetTickCount64(); return true;
    }
    if (free == kActors) { gDropped.fetch_add(1U, std::memory_order_relaxed); return true; }
    gIncoming[free] = value; gIncomingUsed[free] = true; gIncomingAt[free] = GetTickCount64();
    gNextIdentityProbe[free] = 0ULL;
    return true;
}
bool TryPopObserved(State& state) noexcept {
    const auto read = gRead.load(std::memory_order_relaxed);
    if (read == gWrite.load(std::memory_order_acquire)) return false;
    state = gOutgoing[read % kRing];
    gRead.store(read + 1U, std::memory_order_release);
    return true;
}
void PumpOnEngineThread() noexcept {
    if (!Installed() || !EngineThread()) return;
    std::unique_lock lock(gMailboxMutex, std::try_to_lock);
    if (!lock.owns_lock()) return; // never wait for IPC on the engine thread
    if (gConfigurationPending) {
        if (gRequested.role == Role::Retail || gRequested.sessionCookie != gCurrent.sessionCookie ||
            gRequested.worldGeneration != gCurrent.worldGeneration) {
            for (auto& actor : gActors) {
                if (actor.character != 0U && gGeneration == gGenerations->CurrentGeneration())
                    static_cast<void>(Release(actor));
                actor = {};
            }
            // Keep births monotonic across live OFF/ON toggles. A Client may
            // retain an older Host birth while only the Host is being isolated.
        }
        if (gRequested.sessionCookie != gCurrent.sessionCookie ||
            gRequested.worldGeneration != gCurrent.worldGeneration ||
            gRequested.nativeGeneration != gCurrent.nativeGeneration ||
            (gRequested.role == Role::Retail) != (gCurrent.role == Role::Retail))
            gPairBindingEpoch.fetch_add(1U, std::memory_order_acq_rel);
        gCurrent = gRequested; gConfigurationPending = false;
        gActiveNativeGeneration.store(gCurrent.nativeGeneration, std::memory_order_relaxed);
        gActiveWorldGeneration.store(gCurrent.worldGeneration, std::memory_order_relaxed);
        gRole.store(gCurrent.role, std::memory_order_release);
    }
    const auto generation = gGenerations->CurrentGeneration();
    if (generation != gGeneration) {
        // Normal destruction hooks release states BEFORE Character/BT destruction.
        // Never dereference an old-generation Character on an exceptional missed hook.
        for (auto& actor : gActors) {
            actor = {};
        }
        gPairBindingEpoch.fetch_add(1U, std::memory_order_acq_rel);
        gGeneration = generation;
        gLivingPlayerAllianceKnown = false;
        gIncomingUsed.fill(false);
    }
    if (Stable()) RefreshPairBindings();
    // Host publication must survive AI-manager dormancy (vent/backstage).
    // CharacterManager invokes this pump independently of each NPC brain.
    PumpHostAuthority();

    if (!Stable() || !Client()) return;
    const auto now = GetTickCount64();
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (!gIncomingUsed[i]) continue;
        if (now - gIncomingAt[i] > kTimeout) {
            gIncomingUsed[i] = false; gMissing.fetch_add(1U, std::memory_order_relaxed); continue;
        }
        const auto value = gIncoming[i];
        if (value.sessionCookie != gCurrent.sessionCookie || value.worldGeneration != gCurrent.worldGeneration) {
            gIncomingUsed[i] = false; continue;
        }
        if (now < gNextIdentityProbe[i]) continue;
        Actor* found = nullptr;
        for (auto& actor : gActors)
            if (actor.character != 0U && !actor.duplicate && actor.entityA == value.entityA &&
                actor.entityB == value.entityB && actor.characterClass == value.characterClass) found = &actor;
        // Spawn can legitimately follow an incoming snapshot. Retain one latest
        // snapshot per identity; no spawn fallback or matching by list index.
        if (found == nullptr) {
            gMissing.fetch_add(1U, std::memory_order_relaxed);
            gNextIdentityProbe[i] = now + kSendPeriod;
            continue;
        }
        auto& actor = *found;
        // Retain this latest snapshot until the worker admission has returned;
        // birth/withdraw/task cleanup must not reconstruct its source cycle.
        if (PairSourceBusy(actor.character)) continue;
        if (value.birth == actor.remoteBirth && LocalClientVictimOwnsPair(actor.character)) {
            // Retain the latest Host snapshot until the exact native victim
            // ends. Do not let a new target, withdrawal or task cleanup reuse
            // the source while it still belongs to this first-person pair.
            actor.lastReceive = now;
            gPairedVictimSnapshotsDeferred.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        gIncomingUsed[i] = false;
        if (value.birth < actor.remoteBirth ||
            (value.birth == actor.remoteBirth && value.revision < actor.remoteRevision)) continue;

        const bool hadRemote = actor.remoteBirth != 0U;
        const bool birthChanged = value.birth != actor.remoteBirth;
        const bool taskChanged = hadRemote && value.roleTaskRevision != actor.desired.roleTaskRevision;
        const bool lifecycleChanged = hadRemote && value.lifecycleRevision != actor.desired.lifecycleRevision;
        const bool poseMissing = (value.stateFlags & PoseValid) == 0U;
        const auto persistent = LifecycleWithdrawn | LifecycleWithdrawState2 | LifecycleHostDormant;
        const bool placementChanged = birthChanged || (lifecycleChanged &&
            ((value.lifecycleFlags & LifecycleSpatialDiscontinuity) != 0U ||
             ((value.lifecycleFlags ^ actor.desired.lifecycleFlags) & persistent) != 0U));

        if (birthChanged) {
            if (actor.interactionOwnsCycle)
                static_cast<void>(character_direct_step_runtime::FinishReplicatedInteraction(actor.character));
            actor.interactionOwnsCycle = false;
            actor.completedInteractionRevision = 0U;
            actor.completedPairRevision = 0U;
            actor.completedTransitionRevision = 0U;
            actor.resolvedPairRevision = 0U;
        }
        const bool pairedPlacement = actor.interactionOwnsCycle &&
            (value.interactionFlags & InteractionCycleActive) != 0U;

        // A BT path/status change is metadata, not a Move/Idle or animation
        // boundary. Only a real birth/withdraw/placement change stops the
        // follower. Task cleanup still excludes stale autonomous execution.
        if (placementChanged) {
            if (!QuiesceProxyExecution(actor)) {
                actor.faulted = true;
                gFaults.fetch_add(1U, std::memory_order_relaxed);
            }
        } else if (taskChanged && !ClearRoleTaskExecutor(actor)) {
            actor.faulted = true;
        } else if (poseMissing) {
            static_cast<void>(Release(actor));
        }

        // Backstage/withdraw is a real Alien runtime state, not a pose. Mirror
        // the retail withdraw controller first. While Host is withdrawn/dormant
        // the Client must NOT keep forcing the last corridor transform: doing so
        // leaves a visible, brainless Alien fighting the backstage subsystem.
        if (!actor.faulted && placementChanged &&
            !ApplyAuthoritativeWithdraw(actor, value)) {
            actor.faulted = true;
            gFaults.fetch_add(1U, std::memory_order_relaxed);
        }

        // On frontstage re-entry, leave withdraw first, then perform exactly one
        // authoritative placement. Normal PoseSync resumes only after this snap.
        if (!actor.faulted && !ProxyWithdrawn(value) &&
            (value.stateFlags & PoseValid) != 0U &&
            placementChanged && !pairedPlacement) {
            if (!SnapAuthoritativePose(actor, value)) {
                actor.faulted = true;
                gFaults.fetch_add(1U, std::memory_order_relaxed);
            } else {
                gLifecycleApplied.fetch_add(1U, std::memory_order_relaxed);
            }
        } else if (!actor.faulted && ProxyWithdrawn(value) &&
            placementChanged) {
            gLifecycleApplied.fetch_add(1U, std::memory_order_relaxed);
        }

        actor.remoteBirth = value.birth; actor.remoteRevision = value.revision;
        actor.desired = value; actor.lastReceive = now;
        actor.completedActionRevision = value.actionRevision;
        actor.completedRoleTaskRevision = value.roleTaskRevision;
        actor.completedLifecycleRevision = value.lifecycleRevision;
        gApplied.fetch_add(1U, std::memory_order_relaxed);
    }
}
void ObservePlayerRepresentationSpawn(const std::uint32_t spawnKey) noexcept {
    if (spawnKey == 0U || gGenerations == nullptr) return;
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (gOwnedSpawnKeys[i].load(std::memory_order_acquire) == spawnKey &&
            gOwnedSpawnKeyGeneration[i].load(std::memory_order_relaxed) == generation) return;
    }
    for (std::size_t i = 0U; i < kActors; ++i) {
        const auto value = gOwnedSpawnKeys[i].load(std::memory_order_relaxed);
        const auto valueGeneration = gOwnedSpawnKeyGeneration[i].load(std::memory_order_relaxed);
        if (value != 0U && valueGeneration == generation) continue;
        // Clear the publication before recycling a stale-generation slot.
        gOwnedSpawnKeys[i].store(0U, std::memory_order_release);
        gOwnedSpawnKeyGeneration[i].store(generation, std::memory_order_relaxed);
        gOwnedSpawnKeys[i].store(spawnKey, std::memory_order_release);
        gPlayerRepresentationsRegistered.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
}
void ObservePlayerRepresentation(const std::uintptr_t c) noexcept {
    if (c == 0U || gGenerations == nullptr) return;
    std::uint32_t runtimeId{};
    if (Read(c + 0x50U, runtimeId)) ObservePlayerRepresentationSpawn(runtimeId);
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i) {
        auto& owned = gOwned[i];
        auto value = owned.load(std::memory_order_relaxed);
        if (value == c || ((value == 0U || gOwnedGeneration[i].load() != generation) &&
                owned.compare_exchange_strong(value, c))) {
            gOwnedGeneration[i].store(generation, std::memory_order_release);
            break;
        }
    }
    if (!EngineThread()) return;
    if (auto* actor = Find(c); actor != nullptr) {
        // Never stop CharacterDirectStep here: this slot is owned by PlayerSync.
        *actor = {};
        gProvisionalPlayerActorsRetired.fetch_add(1U, std::memory_order_relaxed);
    }
}
void ForgetCharacter(const std::uintptr_t c) noexcept {
    InvalidatePairCharacter(c);
    const bool playerRepresentation = RegisteredPlayerRepresentation(c);
    std::uint32_t runtimeId{};
    const bool hasRuntimeId = playerRepresentation && Read(c + 0x50U, runtimeId);
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (gOwned[i].load(std::memory_order_relaxed) != c) continue;
        gOwned[i].store(0U, std::memory_order_release);
        gOwnedGeneration[i].store(0U, std::memory_order_relaxed);
    }
    if (hasRuntimeId) {
        for (std::size_t i = 0U; i < kActors; ++i) {
            if (gOwnedSpawnKeys[i].load(std::memory_order_relaxed) != runtimeId) continue;
            gOwnedSpawnKeys[i].store(0U, std::memory_order_release);
            gOwnedSpawnKeyGeneration[i].store(0U, std::memory_order_relaxed);
        }
    }
    if (!EngineThread()) return;
    if (auto* actor = Find(c); actor != nullptr) {
        if (!playerRepresentation) static_cast<void>(Release(*actor));
        *actor = {};
    }
}
void ForgetSpawnKey(const std::uint32_t id) noexcept {
    if (id == 0U) return;
    for (auto& slot : gPairSources)
        if (slot.runtimeId.load(std::memory_order_acquire) == id) slot.character.store(0U, std::memory_order_release);
    for (auto& slot : gPairParticipants)
        if (slot.runtimeId.load(std::memory_order_acquire) == id) slot.character.store(0U, std::memory_order_release);
    const auto generation = gGenerations == nullptr ? 0U : gGenerations->CurrentGeneration();
    const bool playerRepresentation = RegisteredPlayerSpawnKey(id);
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (gOwnedSpawnKeys[i].load(std::memory_order_relaxed) != id ||
            (generation != 0U && gOwnedSpawnKeyGeneration[i].load() != generation)) continue;
        gOwnedSpawnKeys[i].store(0U, std::memory_order_release);
        gOwnedSpawnKeyGeneration[i].store(0U, std::memory_order_relaxed);
    }
    for (std::size_t i = 0U; i < kActors; ++i) {
        const auto character = gOwned[i].load(std::memory_order_relaxed);
        std::uint32_t runtimeId{};
        if (character == 0U || !Read(character + 0x50U, runtimeId) || runtimeId != id) continue;
        gOwned[i].store(0U, std::memory_order_release);
        gOwnedGeneration[i].store(0U, std::memory_order_relaxed);
    }
    if (!EngineThread()) return;
    for (auto& actor : gActors) {
        if (actor.character == 0U || actor.runtimeId != id) continue;
        if (!playerRepresentation) static_cast<void>(Release(actor));
        actor = {};
    }
}
void TransportLost() noexcept {
    std::lock_guard lock(gMailboxMutex);
    if (gRequested.role == Role::Client || Client()) gRequested.role = Role::FrozenClient;
    else gRequested.role = Role::Retail;
    gConfigurationPending = true; gIncomingUsed.fill(false);
}
void BeginLogicalShutdown() noexcept {
    if (gDispatcher != nullptr) gShutdownEngineThread.store(gDispatcher->EngineThreadId());
    gStopping.store(true, std::memory_order_release);
    TransportLost();
    gRole.store(Client() ? Role::FrozenClient : Role::Retail, std::memory_order_release);
}
bool OwnsAI() noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    return Installed() && role != Role::Retail;
}
bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }
Counters DiagnosticsSnapshot() noexcept {
    Counters value{gObserved.load(), gApplied.load(), gDropped.load(), gRefused.load(),
        gSuspended.load(), gFaults.load(), gMissing.load()};
    value.managerUpdates = gManagerUpdates.load(); value.admitted = gAdmitted.load();
    value.identityRejected = gIdentityRejected.load(); value.treeUnavailable = gTreeUnavailable.load();
    value.ownerMismatch = gOwnerMismatch.load(); value.remoteQueued = gRemoteQueued.load();
    value.poseCaptured = gPoseCaptured.load(); value.poseApplied = gPoseApplied.load();
    value.poseRejected = gPoseRejected.load(); value.actionCaptured = gActionCaptured.load();
    value.actionTransitions = gActionTransitions.load();
    value.roleTaskCaptured = gRoleTaskCaptured.load();
    value.roleTaskTransitions = gRoleTaskTransitions.load();
    value.lifecycleCaptured = gLifecycleCaptured.load();
    value.lifecycleApplied = gLifecycleApplied.load();
    value.lifecycleSnaps = gLifecycleSnaps.load();
    value.executionBarriers = gExecutionBarriers.load();
    value.withdrawSamples = gWithdrawSamples.load();
    value.withdrawTransitions = gWithdrawTransitions.load();
    value.withdrawApplied = gWithdrawApplied.load();
    value.interactionCaptured = gInteractionCaptured.load();
    value.interactionApplied = gInteractionApplied.load();
    value.interactionRejected = gInteractionRejected.load();
    value.interactionRemapped = gInteractionRemapped.load();
    value.pairedTransitionCaptured = gPairedTransitionCaptured.load();
    value.pairedTransitionApplied = gPairedTransitionApplied.load();
    value.playerRepresentationsRegistered = gPlayerRepresentationsRegistered.load();
    value.provisionalPlayerActorsRetired = gProvisionalPlayerActorsRetired.load();
    value.participantCandidates = gParticipantCandidates.load();
    value.participantTargetRejected = gParticipantTargetRejected.load();
    value.participantCandidatePasses = gParticipantCandidatePasses.load();
    value.senseEventPasses = gSenseEventPasses.load();
    value.participantSignalLookups = gParticipantSignalLookups.load();
    value.participantSignalRemaps = gParticipantSignalRemaps.load();
    value.participantSignalRetailMissing = gParticipantSignalRetailMissing.load();
    value.participantSignalRetailMismatch = gParticipantSignalRetailMismatch.load();
    value.participantSignalAmbiguous = gParticipantSignalAmbiguous.load();
    value.lastParticipantSignalPeer = gLastParticipantSignalPeer.load();
    value.participantPairLookups = gParticipantPairLookups.load();
    value.participantPairRemaps = gParticipantPairRemaps.load();
    value.participantPairRetailMissing = gParticipantPairRetailMissing.load();
    value.participantPairAmbiguous = gParticipantPairAmbiguous.load();
    value.lastParticipantPairPeer = gLastParticipantPairPeer.load();
    value.pairedInitializeSucceeded = gPairedInitializeSucceeded.load();
    value.pairedInitializeFailed = gPairedInitializeFailed.load();
    value.pairedCycleReleased = gPairedCycleReleased.load();
    value.pairedVictimTransitions = gPairedVictimTransitions.load();
    value.pairedVictimTransitionRejected = gPairedVictimTransitionRejected.load();
    value.pairedVictimInitializeAborted = gPairedVictimInitializeAborted.load();
    value.pairedVictimEventsRejected = gPairedVictimEventsRejected.load();
    value.pairedVictimDuplicates = gPairedVictimDuplicates.load();
    value.pairedVictimBusy = gPairedVictimBusy.load();
    value.pairedVictimNativeDispatches = gPairedVictimNativeDispatches.load();
    value.pairedVictimNativeStarted = gPairedVictimNativeStarted.load();
    value.pairedVictimAvailabilityOverrides = gPairedVictimAvailabilityOverrides.load();
    value.pairedVictimLocalDispatches = gPairedVictimLocalDispatches.load();
    value.pairedVictimLocalStarted = gPairedVictimLocalStarted.load();
    value.pairedVictimConvergenceDetached = gPairedVictimConvergenceDetached.load();
    value.pairedVictimSnapshotsDeferred = gPairedVictimSnapshotsDeferred.load();
    value.pairedVictimHealthContinuations = gPairedVictimHealthContinuations.load();
    value.pairedVictimWorkerDispatches = gPairedVictimWorkerDispatches.load();
    value.pairedVictimLocalAvailabilityOverrides = gPairedVictimLocalAvailabilityOverrides.load();
    value.pairedDeadTargetRejected = gPairedDeadTargetRejected.load();
    value.pairedRecoveryDeferred = gPairedRecoveryDeferred.load();
    value.lastVictimAdmissionHp = gLastVictimAdmissionHp.load();
    value.lastVictimAdmissionHealth = gLastVictimAdmissionHealth.load();
    value.lastVictimAdmissionEnabled = gLastVictimAdmissionEnabled.load();
    value.lastRejectedPairedReason = gLastRejectedPairedReason.load();
    value.lastRejectedPairedSourceState = gLastRejectedPairedSourceState.load();
    value.candidateEligibilityChecks = gCandidateEligibilityChecks.load();
    value.candidateEligibilityOverrides = gCandidateEligibilityOverrides.load();
    value.recordEligibilityChecks = gRecordEligibilityChecks.load();
    value.recordEligibilityOverrides = gRecordEligibilityOverrides.load();
    value.participantRecordsValidated = gParticipantRecordsValidated.load();
    value.participantRecordsRetained = gParticipantRecordsRetained.load();
    value.participantRecordsRejected = gParticipantRecordsRejected.load();
    value.participantPerceptionSamples = gParticipantPerceptionSamples.load();
    value.participantPerceptionDropped = gParticipantPerceptionDropped.load();
    value.deadTargetRecoveries = gDeadTargetRecoveries.load();
    value.aiPlayerQueryRemaps = gAiPlayerQueryRemaps.load();
    value.aiPlayerSlotRemaps = gAiPlayerSlotRemaps.load();
    value.aiPlayerSlotRestoreFailures = gAiPlayerSlotRestoreFailures.load();
    value.postDeathDecisionResets = gPostDeathDecisionResets.load();
    value.postDeathDecisionResetRejected = gPostDeathDecisionResetRejected.load();
    value.postDeathRoleTaskResets = gPostDeathRoleTaskResets.load();
    value.interactionBindingsResolved = gInteractionBindingsResolved.load();
    value.interactionBindingRejected = gInteractionBindingRejected.load();
    value.lastHostAttachment = gLastHostAttachment.load();
    value.lastLocalAttachment = gLastLocalAttachment.load();
    value.lastTransitionVariant = gLastTransitionVariant.load();
    value.role = static_cast<std::uint32_t>(gRole.load());
    value.nativeGeneration = gActiveNativeGeneration.load(); value.worldGeneration = gActiveWorldGeneration.load();
    value.lastEntityA = gLastEntityA.load(); value.lastEntityB = gLastEntityB.load();
    value.lastCharacterClass = gLastCharacterClass.load();
    value.lastActionTypeRva = gLastActionTypeRva.load();
    value.lastActionPathHash = gLastActionPathHash.load();
    value.lastActionStatus = gLastActionStatus.load();
    value.lastRoleTaskTypeRva = gLastRoleTaskTypeRva.load();
    value.lastRoleTaskKeyA = gLastRoleTaskKeyA.load();
    value.lastRoleTaskKeyB = gLastRoleTaskKeyB.load();
    value.lastLifecycleFlags = gLastLifecycleFlags.load();
    value.lastWithdrawState = gLastWithdrawState.load();
    value.lastInteractionTargetA = gLastInteractionTargetA.load();
    value.lastInteractionTargetB = gLastInteractionTargetB.load();
    value.lastInteractionTargetMode = gLastInteractionTargetMode.load();
    value.lastInteractionTargetPeer = gLastInteractionTargetPeer.load();
    value.lastAppliedInteractionTargetPeer = gLastAppliedInteractionTargetPeer.load();
    value.lastAiPlayerQueryPeer = gLastAiPlayerQueryPeer.load();
    value.lastAiPlayerSlotPeer = gLastAiPlayerSlotPeer.load();
    value.lastAppliedInteractionTargetA = gLastAppliedInteractionTargetA.load();
    value.lastAppliedInteractionTargetB = gLastAppliedInteractionTargetB.load();
    value.lastInteractionTransition = gLastInteractionTransition.load();
    value.stable = gGenerations != nullptr && gGenerations->IsStableGeneration(value.nativeGeneration);
    return value;
}
void LogDiagnostics(Diagnostics& diagnostics) {
    const auto ai = DiagnosticsSnapshot();
    diagnostics.Log("info", "aisync_v2_snapshot", "AISync V2 high-level Character authority counters; animation/audio and downstream CATHODE consequences remain retail-local.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"ownsAI", Diagnostics::Boolean(OwnsAI()), true},
         {"role", Diagnostics::Unsigned(ai.role), true},
         {"nativeGeneration", Diagnostics::Unsigned(ai.nativeGeneration), true},
         {"worldGeneration", Diagnostics::Unsigned(ai.worldGeneration), true},
         {"stable", Diagnostics::Boolean(ai.stable), true},
         {"managerUpdates", Diagnostics::Unsigned(ai.managerUpdates), true},
         {"admitted", Diagnostics::Unsigned(ai.admitted), true},
         {"identityRejected", Diagnostics::Unsigned(ai.identityRejected), true},
         {"treeUnavailable", Diagnostics::Unsigned(ai.treeUnavailable), true},
         {"ownerMismatch", Diagnostics::Unsigned(ai.ownerMismatch), true},
         {"poseCaptured", Diagnostics::Unsigned(ai.poseCaptured), true},
         {"poseApplied", Diagnostics::Unsigned(ai.poseApplied), true},
         {"poseRejected", Diagnostics::Unsigned(ai.poseRejected), true},
         {"actionCaptured", Diagnostics::Unsigned(ai.actionCaptured), true},
         {"actionTransitions", Diagnostics::Unsigned(ai.actionTransitions), true},
         {"roleTaskCaptured", Diagnostics::Unsigned(ai.roleTaskCaptured), true},
         {"roleTaskTransitions", Diagnostics::Unsigned(ai.roleTaskTransitions), true},
         {"lifecycleCaptured", Diagnostics::Unsigned(ai.lifecycleCaptured), true},
         {"lifecycleApplied", Diagnostics::Unsigned(ai.lifecycleApplied), true},
         {"lifecycleSnaps", Diagnostics::Unsigned(ai.lifecycleSnaps), true},
         {"executionBarriers", Diagnostics::Unsigned(ai.executionBarriers), true},
         {"withdrawSamples", Diagnostics::Unsigned(ai.withdrawSamples), true},
         {"withdrawTransitions", Diagnostics::Unsigned(ai.withdrawTransitions), true},
         {"withdrawApplied", Diagnostics::Unsigned(ai.withdrawApplied), true},
         {"interactionCaptured", Diagnostics::Unsigned(ai.interactionCaptured), true},
         {"interactionApplied", Diagnostics::Unsigned(ai.interactionApplied), true},
         {"interactionRejected", Diagnostics::Unsigned(ai.interactionRejected), true},
         {"interactionRemapped", Diagnostics::Unsigned(ai.interactionRemapped), true},
         {"pairedTransitionCaptured", Diagnostics::Unsigned(ai.pairedTransitionCaptured), true},
         {"pairedTransitionApplied", Diagnostics::Unsigned(ai.pairedTransitionApplied), true},
         {"playerRepresentationsRegistered", Diagnostics::Unsigned(ai.playerRepresentationsRegistered), true},
         {"provisionalPlayerActorsRetired", Diagnostics::Unsigned(ai.provisionalPlayerActorsRetired), true},
         {"participantCandidates", Diagnostics::Unsigned(ai.participantCandidates), true},
         {"participantTargetRejected", Diagnostics::Unsigned(ai.participantTargetRejected), true},
         {"participantCandidatePasses", Diagnostics::Unsigned(ai.participantCandidatePasses), true},
         {"senseEventPasses", Diagnostics::Unsigned(ai.senseEventPasses), true},
         {"participantSignalLookups", Diagnostics::Unsigned(ai.participantSignalLookups), true},
         {"participantSignalRemaps", Diagnostics::Unsigned(ai.participantSignalRemaps), true},
         {"participantSignalRetailMissing", Diagnostics::Unsigned(ai.participantSignalRetailMissing), true},
         {"participantSignalRetailMismatch", Diagnostics::Unsigned(ai.participantSignalRetailMismatch), true},
         {"participantSignalAmbiguous", Diagnostics::Unsigned(ai.participantSignalAmbiguous), true},
         {"lastParticipantSignalPeer", Diagnostics::Unsigned(ai.lastParticipantSignalPeer), true},
         {"participantPairLookups", Diagnostics::Unsigned(ai.participantPairLookups), true},
         {"participantPairRemaps", Diagnostics::Unsigned(ai.participantPairRemaps), true},
         {"participantPairRetailMissing", Diagnostics::Unsigned(ai.participantPairRetailMissing), true},
         {"participantPairAmbiguous", Diagnostics::Unsigned(ai.participantPairAmbiguous), true},
         {"lastParticipantPairPeer", Diagnostics::Unsigned(ai.lastParticipantPairPeer), true},
         {"pairedInitializeSucceeded", Diagnostics::Unsigned(ai.pairedInitializeSucceeded), true},
         {"pairedInitializeFailed", Diagnostics::Unsigned(ai.pairedInitializeFailed), true},
         {"pairedCycleReleased", Diagnostics::Unsigned(ai.pairedCycleReleased), true},
         {"pairedVictimTransitions", Diagnostics::Unsigned(ai.pairedVictimTransitions), true},
         {"pairedVictimTransitionRejected", Diagnostics::Unsigned(ai.pairedVictimTransitionRejected), true},
         {"pairedVictimInitializeAborted", Diagnostics::Unsigned(ai.pairedVictimInitializeAborted), true},
         {"pairedVictimEventsRejected", Diagnostics::Unsigned(ai.pairedVictimEventsRejected), true},
         {"pairedVictimDuplicates", Diagnostics::Unsigned(ai.pairedVictimDuplicates), true},
         {"pairedVictimBusy", Diagnostics::Unsigned(ai.pairedVictimBusy), true},
         {"pairedVictimNativeDispatches", Diagnostics::Unsigned(ai.pairedVictimNativeDispatches), true},
         {"pairedVictimNativeStarted", Diagnostics::Unsigned(ai.pairedVictimNativeStarted), true},
         {"pairedVictimAvailabilityOverrides", Diagnostics::Unsigned(ai.pairedVictimAvailabilityOverrides), true},
         {"pairedVictimLocalDispatches", Diagnostics::Unsigned(ai.pairedVictimLocalDispatches), true},
         {"pairedVictimLocalStarted", Diagnostics::Unsigned(ai.pairedVictimLocalStarted), true},
         {"pairedVictimConvergenceDetached", Diagnostics::Unsigned(ai.pairedVictimConvergenceDetached), true},
         {"pairedVictimSnapshotsDeferred", Diagnostics::Unsigned(ai.pairedVictimSnapshotsDeferred), true},
         {"pairedVictimHealthContinuations", Diagnostics::Unsigned(ai.pairedVictimHealthContinuations), true},
         {"pairedVictimWorkerDispatches", Diagnostics::Unsigned(ai.pairedVictimWorkerDispatches), true},
         {"pairedVictimLocalAvailabilityOverrides", Diagnostics::Unsigned(ai.pairedVictimLocalAvailabilityOverrides), true},
         {"pairedDeadTargetRejected", Diagnostics::Unsigned(ai.pairedDeadTargetRejected), true},
         {"pairedRecoveryDeferred", Diagnostics::Unsigned(ai.pairedRecoveryDeferred), true},
         {"lastVictimAdmissionHp", Diagnostics::Unsigned(ai.lastVictimAdmissionHp), true},
         {"lastVictimAdmissionHealth", Diagnostics::Unsigned(ai.lastVictimAdmissionHealth), true},
         {"lastVictimAdmissionEnabled", Diagnostics::Unsigned(ai.lastVictimAdmissionEnabled), true},
         {"lastRejectedPairedReason", Diagnostics::Unsigned(ai.lastRejectedPairedReason), true},
         {"lastRejectedPairedSourceState", Diagnostics::Unsigned(ai.lastRejectedPairedSourceState), true},
         {"candidateEligibilityChecks", Diagnostics::Unsigned(ai.candidateEligibilityChecks), true},
         {"candidateEligibilityOverrides", Diagnostics::Unsigned(ai.candidateEligibilityOverrides), true},
         {"recordEligibilityChecks", Diagnostics::Unsigned(ai.recordEligibilityChecks), true},
         {"recordEligibilityOverrides", Diagnostics::Unsigned(ai.recordEligibilityOverrides), true},
         {"participantRecordsValidated", Diagnostics::Unsigned(ai.participantRecordsValidated), true},
         {"participantRecordsRetained", Diagnostics::Unsigned(ai.participantRecordsRetained), true},
         {"participantRecordsRejected", Diagnostics::Unsigned(ai.participantRecordsRejected), true},
         {"participantPerceptionSamples", Diagnostics::Unsigned(ai.participantPerceptionSamples), true},
         {"participantPerceptionDropped", Diagnostics::Unsigned(ai.participantPerceptionDropped), true},
         {"deadTargetRecoveries", Diagnostics::Unsigned(ai.deadTargetRecoveries), true},
         {"aiPlayerQueryRemaps", Diagnostics::Unsigned(ai.aiPlayerQueryRemaps), true},
         {"aiPlayerSlotRemaps", Diagnostics::Unsigned(ai.aiPlayerSlotRemaps), true},
         {"aiPlayerSlotRestoreFailures", Diagnostics::Unsigned(ai.aiPlayerSlotRestoreFailures), true},
         {"postDeathDecisionResets", Diagnostics::Unsigned(ai.postDeathDecisionResets), true},
         {"postDeathDecisionResetRejected", Diagnostics::Unsigned(ai.postDeathDecisionResetRejected), true},
         {"postDeathRoleTaskResets", Diagnostics::Unsigned(ai.postDeathRoleTaskResets), true},
         {"interactionBindingsResolved", Diagnostics::Unsigned(ai.interactionBindingsResolved), true},
         {"interactionBindingRejected", Diagnostics::Unsigned(ai.interactionBindingRejected), true},
         {"lastHostAttachment", Diagnostics::Unsigned(ai.lastHostAttachment), true},
         {"lastLocalAttachment", Diagnostics::Unsigned(ai.lastLocalAttachment), true},
         {"lastTransitionVariant", Diagnostics::Unsigned(ai.lastTransitionVariant), true},
         {"lastEntityA", Diagnostics::Unsigned(ai.lastEntityA), true},
         {"lastEntityB", Diagnostics::Unsigned(ai.lastEntityB), true},
         {"lastCharacterClass", Diagnostics::Unsigned(ai.lastCharacterClass), true},
         {"lastActionTypeRva", Diagnostics::Unsigned(ai.lastActionTypeRva), true},
         {"lastActionPathHash", Diagnostics::Unsigned(ai.lastActionPathHash), true},
         {"lastActionStatus", Diagnostics::Unsigned(ai.lastActionStatus), true},
         {"lastRoleTaskTypeRva", Diagnostics::Unsigned(ai.lastRoleTaskTypeRva), true},
         {"lastRoleTaskKeyA", Diagnostics::Unsigned(ai.lastRoleTaskKeyA), true},
         {"lastRoleTaskKeyB", Diagnostics::Unsigned(ai.lastRoleTaskKeyB), true},
         {"lastLifecycleFlags", Diagnostics::Unsigned(ai.lastLifecycleFlags), true},
         {"lastWithdrawState", Diagnostics::Unsigned(ai.lastWithdrawState), true},
         {"lastInteractionTargetA", Diagnostics::Unsigned(ai.lastInteractionTargetA), true},
         {"lastInteractionTargetB", Diagnostics::Unsigned(ai.lastInteractionTargetB), true},
         {"lastInteractionTargetMode", Diagnostics::Unsigned(ai.lastInteractionTargetMode), true},
         {"lastInteractionTargetPeer", Diagnostics::Unsigned(ai.lastInteractionTargetPeer), true},
         {"lastAppliedInteractionTargetPeer", Diagnostics::Unsigned(ai.lastAppliedInteractionTargetPeer), true},
         {"lastAiPlayerQueryPeer", Diagnostics::Unsigned(ai.lastAiPlayerQueryPeer), true},
         {"lastAiPlayerSlotPeer", Diagnostics::Unsigned(ai.lastAiPlayerSlotPeer), true},
         {"lastAppliedInteractionTargetA", Diagnostics::Unsigned(ai.lastAppliedInteractionTargetA), true},
         {"lastAppliedInteractionTargetB", Diagnostics::Unsigned(ai.lastAppliedInteractionTargetB), true},
         {"lastInteractionTransition", Diagnostics::Unsigned(ai.lastInteractionTransition), true},
         {"remoteQueued", Diagnostics::Unsigned(ai.remoteQueued), true},
         {"observed", Diagnostics::Unsigned(ai.observed), true},
         {"applied", Diagnostics::Unsigned(ai.applied), true},
         {"dropped", Diagnostics::Unsigned(ai.dropped), true},
         {"refused", Diagnostics::Unsigned(ai.refused), true},
         {"suspended", Diagnostics::Unsigned(ai.suspended), true},
         {"faults", Diagnostics::Unsigned(ai.faults), true},
         {"missingIdentity", Diagnostics::Unsigned(ai.missingIdentity), true}});
    // Bootstrap and IPC can both request diagnostics. Serialize consumers
    // only; the engine-thread producer never takes this mutex.
    std::lock_guard sampleLock(gParticipantProbeConsumerMutex);
    for (std::size_t i = 0U; i < kParticipantProbeRing; ++i) {
        const auto read = gParticipantProbeRead.load(std::memory_order_relaxed);
        if (read == gParticipantProbeWrite.load(std::memory_order_acquire)) break;
        const auto sample = gParticipantProbes[read % kParticipantProbeRing];
        gParticipantProbeRead.store(read + 1U, std::memory_order_release);
        diagnostics.Log("info", "aisync_participant_perception", "Sampled retail participant candidate, senses, threat and selected target; observation only.",
            {{"sessionCookie", Diagnostics::Unsigned(sample.sessionCookie), true},
             {"nativeGeneration", Diagnostics::Unsigned(sample.nativeGeneration), true},
             {"worldGeneration", Diagnostics::Unsigned(sample.worldGeneration), true},
             {"sampleTick", Diagnostics::Unsigned(sample.tick), true},
             {"entityA", Diagnostics::Unsigned(sample.entityA), true},
             {"entityB", Diagnostics::Unsigned(sample.entityB), true},
             {"characterClass", Diagnostics::Unsigned(sample.characterClass), true},
             {"peer", Diagnostics::Unsigned(sample.peer), true},
             {"living", Diagnostics::Boolean(sample.living), true},
             {"signalRegistryReadable", Diagnostics::Boolean(sample.signalRegistryReadable), true},
             {"signalRegistryFound", Diagnostics::Boolean(sample.signalRegistryFound), true},
             {"signalRegistryMatches", Diagnostics::Boolean(sample.signalRegistryMatches), true},
             {"visibilityFlagsReadable", Diagnostics::Boolean(sample.visibilityFlagsReadable), true},
             {"ownerHidden", Diagnostics::Unsigned(sample.ownerHidden), true},
             {"targetHidden", Diagnostics::Unsigned(sample.targetHidden), true},
             {"pairReadable", Diagnostics::Boolean(sample.pairReadable), true},
             {"paired", Diagnostics::Boolean(sample.paired), true},
             {"executionReadable", Diagnostics::Boolean(sample.executionReadable), true},
             {"retailEligibility", Diagnostics::Unsigned(sample.retailEligibility), true},
             {"executionEnabled", Diagnostics::Unsigned(sample.executionEnabled), true},
             {"executionStateA", Diagnostics::Unsigned(sample.executionStateA), true},
             {"executionStateB", Diagnostics::Unsigned(sample.executionStateB), true},
             {"executionPhase", Diagnostics::Unsigned(sample.executionPhase), true},
             {"ownerExecutionReadable", Diagnostics::Boolean(sample.ownerExecutionReadable), true},
             {"ownerRetailEligibility", Diagnostics::Unsigned(sample.ownerRetailEligibility), true},
             {"ownerGatesReadable", Diagnostics::Boolean(sample.ownerGatesReadable), true},
             {"ownerEnabled", Diagnostics::Unsigned(sample.ownerEnabled), true},
             {"ownerSenseBlocked", Diagnostics::Unsigned(sample.ownerSenseBlocked), true},
             {"sensesReadable", Diagnostics::Boolean(sample.sensesReadable), true},
             {"sensesDisabled", Diagnostics::Unsigned(sample.sensesDisabled), true},
             {"recordPresent", Diagnostics::Boolean(sample.recordPresent), true},
             {"recordReadable", Diagnostics::Boolean(sample.recordReadable), true},
             {"recordActive", Diagnostics::Unsigned(sample.recordActive), true},
             {"recordSelected", Diagnostics::Unsigned(sample.recordSelected), true},
             {"recordForced", Diagnostics::Unsigned(sample.recordForced), true},
             {"sightInput", sample.recordReadable ? std::to_string(sample.sightInput) : "null", true},
             {"sightTimestamp", sample.recordReadable ? std::to_string(sample.sightTimestamp) : "null", true},
             {"activeSenseMask", Diagnostics::Unsigned(sample.activeSenseMask), true},
             {"primarySenseIndex", Diagnostics::Unsigned(sample.primarySenseIndex), true},
             {"recentSenseIndex", Diagnostics::Unsigned(sample.recentSenseIndex), true},
             {"threatReadable", Diagnostics::Boolean(sample.threatReadable), true},
             {"threatPresent", Diagnostics::Boolean(sample.threatPresent), true},
             {"threatState", Diagnostics::Unsigned(sample.threatState), true},
             {"selectedReadable", Diagnostics::Boolean(sample.selectedReadable), true},
             {"selectedPresent", Diagnostics::Boolean(sample.selectedPresent), true},
             {"selectedPeer", Diagnostics::Unsigned(sample.selectedPeer), true}});
    }
}
} // namespace aimp::ai_sync
