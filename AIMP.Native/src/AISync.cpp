#include "aimp/AISync.h"
#include "aimp/AISyncExecutionPolicy.h"
#include "aimp/AISyncNavigation.h"
#include "aimp/BuildProfile.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <type_traits>

namespace aimp::ai_sync {
namespace {
static_assert(sizeof(void*) == 4U);
static_assert(std::is_trivially_copyable_v<State>);
static_assert(std::is_trivially_copyable_v<ImpactEvent>);
constexpr std::size_t kActors = 32U, kRing = 256U;
constexpr ULONGLONG kSendPeriod = 50ULL, kRefreshPeriod = 250ULL, kTimeout = 1500ULL;
constexpr ULONGLONG kManagerDormancyPeriod = 500ULL;
constexpr ULONGLONG kDeadTargetGracePeriod = 1500ULL;
constexpr ULONGLONG kPostDeathArbitrationGracePeriod = 2000ULL;
constexpr ULONGLONG kPostDeathRecentSenseHold = 2000ULL;
constexpr ULONGLONG kMotionLease = 600ULL;
constexpr std::uint32_t kStateFlags = PoseValid | Moving | ActionValid | InteractionValid | RoleTaskValid |
    ExecutionValid;
constexpr std::uint32_t kExecutionFlags = ExecutionTransition | ExecutionCertified | ExecutionPaired;
constexpr std::uint32_t kInteractionFlags = InteractionActorIsSource |
    InteractionTargetIsParticipant | InteractionPairValid | InteractionCycleActive | InteractionNativeOutcomeRequired |
    InteractionSourceCycleRetained;
constexpr std::uint32_t kOutcomeFlags = OutcomePrepared | OutcomeVictimAdmitted |
    OutcomeSourceEnded | OutcomeVictimEnded | OutcomeTerminal | OutcomeNativeDeath;
constexpr std::uint32_t kLifecycleFlags = LifecycleSpatialDiscontinuity | LifecycleTraversalAction |
    LifecycleWithdrawn | LifecycleWithdrawState2 | LifecycleHostDormant | LifecycleCharacterBackstage;
constexpr float kLifecycleDiscontinuityDistance = 3.5F;
constexpr float kLifecycleVerticalDiscontinuity = 2.5F;
constexpr std::uintptr_t kPositionOffset = 0x70U, kYawOffset = 0x174U;
constexpr std::uintptr_t kInteractionManagerGlobalRva = 0x012F0C88U;
constexpr std::uintptr_t kRoleTaskCleanupRva = 0x00361A90U;
constexpr auto kCharacterRegistry = character_direct_step_runtime::NativeCharacterRegistry();
constexpr std::uintptr_t kCharacterBackstageRva = 0x0038B140U;
constexpr std::uintptr_t kCharacterFrontstageRva = 0x003C0220U;
constexpr std::uintptr_t kCharacterRelocateRva = 0x003C6330U;
constexpr std::uintptr_t kCharacterResolverRegistryRva = 0x0134EF40U;
constexpr std::uintptr_t kCharacterResolverRva = 0x002545C0U;
constexpr std::uintptr_t kResolveImpactDamageRva = 0x003A0A40U;
constexpr float kPlacementTolerance = 0.25F;
constexpr float kMotionThreshold = 0.02F, kMaximumVelocity = 20.0F;
constexpr float kIntentCatastrophicDistance = 8.0F;
constexpr float kIntentCatastrophicVerticalDistance = 4.0F;
constexpr ULONGLONG kIntentCatastrophicGrace = 2000ULL;
using ManagerUpdate = void(__thiscall*)(void*, std::uint32_t, float);
using Coordinator = void(__thiscall*)(void*, std::uint32_t);
using CharacterInteraction = std::uint32_t(__thiscall*)(void*, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint32_t, std::uint64_t, std::int32_t);
using PairedTransitionA = void(__thiscall*)(void*, std::uintptr_t, std::int32_t, std::uint32_t);
using PairedTransitionB = void(__thiscall*)(void*, std::uintptr_t, std::int32_t,
    std::uint32_t, std::uint32_t);
using CharacterImpact = void(__thiscall*)(void*, int*);
using ResolveImpactDamage = std::int16_t(__fastcall*)(int*);
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
// Certified 007CC900: ECX=Character, NINE stack words, RET 24h.
// The descriptor (word 7) and two trailing flags were lost in the decompiler.
using NativeDamage = void(__thiscall*)(void*, std::uint32_t, std::uintptr_t, float,
    std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
// 007B4AD0 is the retail health commit, ECX=Character+320, RET 8.
// It notifies the health state machine and Character callbacks (007AD730).
using NativeHealthCommit = void(__thiscall*)(void*, std::uint32_t, std::uintptr_t);
using Hostility = std::uint32_t(__thiscall*)(void*, std::uintptr_t);
using ThePlayerResolver = std::uintptr_t(__cdecl*)();
// Both certified functions take ECX only and use plain RET. The record
// validator returns AL (true means discard), not a defined full EAX.
using TargetEligibility = std::uint32_t(__thiscall*)(void*);
using RecordInvalid = std::uint8_t(__thiscall*)(void*);
using BackstageUpdate = std::uint8_t(__thiscall*)(void*);
using BackstageHoldUpdate = void(__thiscall*)(void*);
// FUN_006D85E0: ECX=Alien AI controller, no stack arguments, plain RET.
// It is the native producer for sweep+0x51; feeding a transient request here
// preserves its explicit Character restrictions instead of forcing the sweep.
using BackstageActivationRefresh = void(__thiscall*)(void*);
using NavigationGoal = void(__thiscall*)(void*, const float*);
using NavigationUpdate = void(__thiscall*)(void*);
// Owned shared path-query reference at +0/+4, ECX only, plain RET.
using NavigationRelease = void(__thiscall*)(void*);
constexpr ULONGLONG kParticipantProbePeriod = 1000ULL;
constexpr ULONGLONG kPairPlacementReadyTimeout = 250ULL;
constexpr std::size_t kParticipantProbeRing = 256U;
struct ParticipantProbe final {
    std::uint64_t sessionCookie{}, peer{}, selectedPeer{}, pendingPeer{}, tick{};
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
    bool selectedTerminalDead{}, pendingReadable{}, pendingPresent{}, pendingLiving{}, targetSignalsReadable{};
    std::uint8_t haveTargetSignal{}, haveNextTargetSignal{};
    float sightInput{}, sightTimestamp{};
    bool signalRegistryReadable{}, signalRegistryFound{}, signalRegistryMatches{};
    bool visibilityFlagsReadable{};
    std::uint8_t ownerHidden{}, targetHidden{};
    bool playerComponentsReadable{}, playerControlPresent{}, playerNavigationPresent{};
    std::uint8_t playerControlLocal{};
    bool backstageReadable{};
    std::uint32_t backstageState{}, backstageKeyA{}, backstageKeyB{};
    std::uint8_t backstageEnabled{}, backstagePolicy{};
    bool navigationCached{}, navigationGoalKnown{};
    std::uint32_t navigationRouteCount{};
};
// Engine-thread producer, serialized worker consumers. Samples hold values,
// never pointers to Characters/records that a later worker might dereference.
std::array<ParticipantProbe, kParticipantProbeRing> gParticipantProbes{};
std::atomic<std::uint32_t> gParticipantProbeWrite{}, gParticipantProbeRead{};

enum class TargetSuccessionPhase : std::uint32_t {
    UpdateEntry = 1U,
    PostRecovery = 2U,
    PostRetailUpdate = 3U,
    RecoveryEvaluation = 4U,
    PairRetired = 5U,
    SuccessorPreserved = 6U,
    FallbackReset = 7U,
    FallbackResetRejected = 8U,
    InteractionAdvanced = 9U,
    PostPrepass = 10U,
    PostCoordinator = 11U
};
struct TargetSelectionSnapshot final {
    std::uintptr_t selectedRecord{}, selectedTarget{}, pendingRecord{}, pendingTarget{};
    std::uint64_t selectedPeer{}, pendingPeer{};
    std::uint32_t selectedBirth{}, pendingBirth{};
    std::uint8_t haveTargetSignal{}, haveNextTargetSignal{};
    bool selectionReadable{}, selectedReadable{}, selectedPresent{}, selectedLiving{}, selectedTerminalDead{};
    bool pendingReadable{}, pendingPresent{}, pendingLiving{}, pendingTerminalDead{}, targetSignalsReadable{};
};
struct TargetSuccessionProbe final {
    std::uint64_t sessionCookie{}, tick{}, retiredPeer{}, selectedPeer{}, pendingPeer{};
    std::uint32_t nativeGeneration{}, worldGeneration{}, entityA{}, entityB{}, birth{};
    std::uint32_t interactionRevision{}, pairRevision{}, phase{};
    std::uint32_t selectedBirth{}, pendingBirth{};
    std::uint8_t haveTargetSignal{}, haveNextTargetSignal{};
    bool selectionReadable{}, selectedReadable{}, selectedPresent{}, selectedLiving{}, selectedTerminalDead{};
    bool pendingReadable{}, pendingPresent{}, pendingLiving{}, pendingTerminalDead{}, targetSignalsReadable{};
};
std::array<TargetSuccessionProbe, kParticipantProbeRing> gTargetSuccessionProbes{};
std::atomic<std::uint32_t> gTargetSuccessionProbeWrite{}, gTargetSuccessionProbeRead{};
std::mutex gParticipantProbeConsumerMutex;
constexpr std::size_t kPoseHistory = 8U;
struct PoseSample final {
    ULONGLONG tick{};
    float position[3]{}, yaw{}, velocityX{}, velocityZ{};
};
struct Actor {
    std::uintptr_t character{}, tree{}, lastRoleTask{};
    std::uint32_t runtimeId{}, entityA{}, entityB{}, characterClass{}, birth{};
    State desired{}, previous{};
    ULONGLONG lastSend{}, lastReceive{}, lastManagerUpdate{};
    ULONGLONG lastPoseSample{}, lastProxyRefresh{}, lastInteractionAttempt{}, lastInteractionCapture{};
    ULONGLONG deadTargetSince{}, stationarySince{}, lastParticipantProbe{}, lastPresentationProbe{};
    ULONGLONG intentDivergenceSince{}, postDeathArbitrationSince{}, lastLivingParticipantSense{};
    std::uint64_t lastLivingParticipantSensePeer{};
    std::uint32_t postDeathArbitrationRevision{};
    std::uint64_t postDeathRetiredPeer{};
    std::uintptr_t observedSelectedRecord{}, observedPendingRecord{};
    std::uint64_t observedSelectedPeer{}, observedPendingPeer{};
    std::uint32_t observedSelectedBirth{}, observedPendingBirth{};
    std::uint8_t observedHaveTargetSignal{}, observedHaveNextTargetSignal{};
    bool observedSelectedPresent{}, observedSelectedLiving{}, observedSelectedTerminalDead{};
    bool observedPendingPresent{}, observedPendingLiving{}, observedPendingTerminalDead{};
    bool observedSelectionReadable{}, observedSelectedReadable{}, observedPendingReadable{}, observedTargetSignalsReadable{};
    bool targetSelectionObserved{};
    std::uint32_t participantProbeCursor{};
    std::uint32_t completedRevision{}, completedInteractionRevision{};
    std::uint32_t completedActionRevision{}, completedRoleTaskRevision{}, completedLifecycleRevision{};
    std::uint32_t completedExecutionRevision{};
    std::uint32_t replayedExecutionRevision{};
    ULONGLONG lastExecutionAttempt{};
    std::uintptr_t capturedNavigationRequest{};
    std::uint32_t capturedNavigationEpoch{};
    MovementOwner movementOwner{MovementOwner::None};
    std::uint32_t interactionActionRevision{}, interactionActionPathHash{};
    std::uint32_t completedPairRevision{}, completedTransitionRevision{}, recoveredInteractionRevision{};
    std::uint32_t resolvedPairRevision{}, localPairFlagsLow{}, localPairFlagsHigh{};
    std::int32_t localPairAttachment{-1};
    std::uintptr_t resolvedPairTarget{};
    std::int32_t resolvedSourceVariant{};
    // Engine-thread-only placement preparation, scoped to the exact live pair.
    // No victim/health/animation state is held or authored by this record.
    ULONGLONG pairPlacementSince{};
    std::uintptr_t pairPlacementTarget{};
    std::uint32_t pairPlacementBirth{}, pairPlacementPair{}, pairPlacementTransition{}, pairPlacementTargetBirth{};
    bool pairPlacementPending{};
    // Client-observer only: remembers a remote fatal pair that has completed
    // locally so delayed Host outro snapshots cannot reopen its 4E presentation.
    // Revisions are value-only authority epochs; no native pointer crosses this
    // boundary or survives a birth/world/session reset.
    std::uint32_t observerReleasedBirth{}, observerReleasedPairRevision{};
    std::uint32_t observerReleasedInteractionRevision{}, observerReleasedActionRevision{};
    std::uint32_t observerReleasedExecutionRevision{}, observerReleasedRoleTaskRevision{};
    std::uint32_t observerReleasedLifecycleRevision{}, observerReleasedLocomotionRevision{};
    std::uint32_t observerReleasedNavigationRevision{}, observerReleasedNavigationRequestRevision{};
    std::uint64_t observerReleasedTargetPeer{};
    bool observerHandoffNeutral{};
    std::uint32_t remoteBirth{}, remoteRevision{};
    float lastPosition[3]{};
    bool blocked{}, duplicate{}, faulted{}, hasLastPosition{}, interactionOwnsCycle{};
    bool traversalAction{}, hostDormant{}, withdrawKnown{};
    bool backstageKnown{}, backstage{}, placementPending{}, roleTaskCleanupPending{};
    std::uint32_t withdrawState{};
    // Client presentation only; bounded, engine-thread owned, no native pointers.
    std::array<PoseSample, kPoseHistory> poses{};
    std::size_t poseCount{};
    std::uint32_t poseRevision{};
    bool poseInterpolating{};
};
std::array<Actor, kActors> gActors{}; // engine thread only
// Value-only observations distinguish transport liveness, physical drift and
// hidden presentation. One sample/actor/second plus pair admission boundaries.
struct PresentationProbe final {
    std::uint64_t sessionCookie{}, peer{}, tick{};
    std::uint32_t nativeGeneration{}, worldGeneration{}, role{}, phase{};
    std::uint32_t entityA{}, entityB{}, birth{}, revision{}, pairRevision{}, lifecycleFlags{}, interactionFlags{};
    std::uint32_t observerReleasedPairRevision{};
    NavigationGoalState navigation{};
    std::uint32_t movementOwner{};
    std::uint32_t cycleType{}, stage{0xFFFFFFFFU}, withdrawState{0xFFFFFFFFU};
    float local[3]{}, authority[3]{}, victim[3]{}, authorityVictim[3]{};
    std::uint8_t cycleState{}, hidden{}, enabled{};
    std::uint16_t victimHp{};
    std::uint32_t victimHealthState{};
    bool victimHealthReadable{};
    std::uintptr_t sourceCharacter{}, victimCharacter{}, localCharacter{};
    std::uint32_t sourceRuntimeId{};
    std::uint32_t victimRuntimeId{}, victimEntityA{}, victimEntityB{}, victimCycleType{};
    std::uint32_t localRuntimeId{}, localHp{}, localHealthState{};
    NativeOutcome outcome{};
    std::uint8_t victimCycleState{}, victimAcquired{}, victimEnded{}, victimInteractive{};
    bool victimLocal{}, victimRepresentation{}, localHealthReadable{};
    bool positionReadable{}, victimReadable{}, visibilityReadable{}, cycleReadable{}, ownsPair{}, placementPending{}, pairPlacementPending{};
    bool observerHandoffNeutral{};
};
std::array<PresentationProbe, kParticipantProbeRing> gPresentationProbes{};
std::atomic<std::uint32_t> gPresentationProbeWrite{}, gPresentationProbeRead{};
std::atomic<std::uint64_t> gPresentationSamples{}, gPresentationDropped{};
std::atomic<std::uint64_t> gPairedSourcePlacements{}, gPairedSourcePlacementRejected{};
std::atomic<std::uint64_t> gPairedSourcePlacementDeferred{}, gPairedSourcePlacementResumed{};
std::atomic<std::uint64_t> gPairedSourcePlacementCancelled{}, gPairedSourcePlacementExpired{};
std::atomic<std::uint64_t> gPairedSourcePoseSuppressed{};
std::atomic<std::uint64_t> gPairedObserverSnapshotsDeferred{};
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
// Workers never read Actor/gCurrent. A short value-only exchange carries the
// exact pair and native result. Periodic engine capture only tries this mutex;
// no native calls, logging or I/O run while an exchange holds it.
struct OutcomeBinding final {
    PairIdentity source{}, victim{};
    State state{};
    std::uint8_t serial{};
    bool claimed{}, lateFinishClaimed{};
};
std::array<OutcomeBinding, kActors> gOutcomes{};
std::mutex gOutcomeMutex;
std::atomic<std::uint64_t> gOutcomeCaptured{}, gOutcomeApplied{}, gOutcomeRejected{}, gOutcomeDuplicates{};
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
    NativeOutcome outcome{};
    std::uint32_t pairRevision{};
    std::size_t slot{kActors};
    std::uint8_t sourceSerial{};
    bool livingBefore{}, captured{}, playerPreparation{};
    bool authoritativeClient{}, victimDispatch{}, retailDispatched{}, duplicateOutcome{}, nativeFinish{};
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
// Motion backpressure cannot consume the capacity reserved for native results.
std::array<State, kRing> gOutgoingOutcomes{};
std::atomic<std::uint32_t> gOutcomeWrite{}, gOutcomeRead{};
// Client weapon impacts travel in the reverse direction from ordinary AISync
// state. Fixed-size rings avoid allocation inside Character impact detours.
std::mutex gImpactMailboxMutex;
std::array<ImpactEvent, kRing> gOutgoingImpacts{}, gIncomingImpacts{};
std::size_t gOutgoingImpactRead{}, gOutgoingImpactWrite{};
std::size_t gIncomingImpactRead{}, gIncomingImpactWrite{};
std::uint32_t gImpactSequence{};
struct ImpactSequence final { std::uint64_t peer{}; std::uint32_t sequence{}; };
std::array<ImpactSequence, kActors> gLastImpactSequence{};
std::mutex gMailboxMutex;
Configuration gRequested{};
bool gConfigurationPending{};
std::array<State, kActors> gIncoming{};
std::array<std::deque<State>, kActors> gIncomingOutcomes{}; // bounded by kRing globally
std::array<State, kActors> gLastQueuedOutcome{};
std::size_t gIncomingOutcomeCount{};
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
// Only complete MOV instructions are displaced; each untouched path uses its
// own relocation-free trampoline. Adapted paths resume at retail player logic.
void* gPlayerPreparationTrampoline{}, *gPlayerPreparationContinue{};
void* gOutcomePreparationContinue{}, *gOutcomePreparationAbort{};
void* gPlayerCategoryTrampoline{}, *gPlayerCategoryContinue{};
std::atomic<void*> gPlayerPreparationPublished{}, gPlayerCategoryPublished{};
void* gVictimUpdateTrampoline{};
std::atomic<void*> gVictimUpdateOriginal{};
void* gHealthCommitTrampoline{};
std::atomic<void*> gHealthCommitOriginal{};
void* gVictimFinishTrampoline{}, *gVictimFinishSkip{}, *gVictimFinishKill{};
std::atomic<void*> gVictimFinishPublished{};
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
std::atomic<void*> gBackstageUpdateOriginal{}, gBackstageHoldOriginal{};
void* gBackstageUpdateTrampoline{}, *gBackstageHoldTrampoline{};
void* gBackstageEligibilityTrampoline{}, *gBackstageEligibilityContinue{};
std::atomic<void*> gBackstageEligibilityPublished{};
void* gBackstageNavigationTrampoline{}, *gBackstageNavigationContinue{};
std::atomic<void*> gBackstageNavigationPublished{};
std::atomic<std::uint64_t> gBackstageEligibilityOverrides{}, gBackstageNavigationPlans{};
std::atomic<std::uint64_t> gBackstageNavigationUpdates{}, gBackstageNavigationRejected{};
std::atomic<std::uint64_t> gBackstageDeadTargetsRejected{}, gBackstageHoldContexts{};
std::atomic<std::uint64_t> gBackstageSurvivorRearmAttempts{}, gBackstageSurvivorRearms{};
std::atomic<std::uint64_t> gBackstageSurvivorRearmRestricted{}, gBackstageSurvivorRearmFailures{};
std::atomic<std::uint64_t> gLastBackstageSurvivorPeer{};
// Same data layout as the optional player route cache made by 006EE5B0.
// Only AISync owns these buffers and their +0/+4 shared query references.
// No player controller is fabricated or installed on a PlayerSync Character.
struct ParticipantNavigation final {
    alignas(16) std::array<std::byte, 0x458U> native{};
    std::uint64_t peer{}, sessionCookie{};
    std::uintptr_t character{};
    std::uint32_t runtimeId{}, nativeGeneration{}, worldGeneration{};
    ULONGLONG lastUpdate{};
    bool initialized{}, faulted{};
};
std::array<ParticipantNavigation, kActors> gParticipantNavigation{};
bool gNavigationHostActive{};
std::uint64_t gNavigationSession{};
std::uint64_t gNavigationGeneration{};
std::uint32_t gNavigationWorld{};
ULONGLONG gNavigationMaintenance{};
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
std::atomic<std::uint64_t> gIntentCaptured{}, gIntentApplied{}, gIntentRejected{}, gIntentRecoveries{};
std::atomic<std::uint64_t> gActionCaptured{}, gActionTransitions{};
std::atomic<std::uint64_t> gExecutionCaptured{}, gExecutionTransitions{}, gExecutionReplayed{}, gExecutionReplayRejected{};
std::atomic<std::uint64_t> gRoleTaskCaptured{}, gRoleTaskTransitions{};
std::atomic<std::uint64_t> gLifecycleCaptured{}, gLifecycleApplied{}, gLifecycleSnaps{}, gExecutionBarriers{};
std::atomic<std::uint64_t> gImpactCaptured{}, gImpactQueued{}, gImpactApplied{}, gImpactRejected{};
std::atomic<std::uint64_t> gWithdrawSamples{}, gWithdrawTransitions{}, gWithdrawApplied{};
std::atomic<std::uint64_t> gBackstageCaptured{}, gBackstageTransitions{}, gBackstageApplied{}, gBackstageRejected{};
std::atomic<std::uint64_t> gPlacementVerified{}, gPlacementRejected{}, gLifecycleRetries{};
std::atomic<std::uint64_t> gPoseInterpolated{}, gPairedHostCycleWaits{}, gOutcomeSnapshotsCoalesced{};
std::atomic<std::uint64_t> gInteractionCaptured{}, gInteractionApplied{}, gInteractionRejected{};
std::atomic<std::uint64_t> gInteractionRemapped{};
std::atomic<std::uint64_t> gPairedTransitionCaptured{}, gPairedTransitionApplied{};
std::atomic<std::uint64_t> gPlayerRepresentationsRegistered{};
std::atomic<std::uint64_t> gProvisionalPlayerActorsRetired{};
std::atomic<std::uint64_t> gParticipantCandidates{}, gParticipantTargetRejected{}, gDeadTargetRecoveries{};
std::atomic<std::uint64_t> gPostDeathMeleeSelectionChecks{}, gPostDeathMeleeSelectionRejects{};
std::atomic<std::uint64_t> gPostDeathMeleeSelectionWriteFailures{}, gLastPostDeathRetiredPeer{};
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
std::atomic<std::uint64_t> gPairedObserverCompletions{}, gPairedObserverReleaseAttempts{};
std::atomic<std::uint64_t> gPairedObserverReleases{}, gPairedObserverReleaseRejected{};
std::atomic<std::uint64_t> gPairedObserverPairReopenSuppressed{}, gPairedObserverNeutralHolds{};
std::atomic<std::uint64_t> gPairedObserverHandoffResumed{};
std::atomic<std::uint64_t> gPairedVictimTransitions{}, gPairedVictimTransitionRejected{};
std::atomic<std::uint64_t> gPairedVictimInitializeAborted{};
std::atomic<std::uint64_t> gPairedVictimEventsRejected{}, gPairedVictimDuplicates{}, gPairedVictimBusy{};
std::atomic<std::uint64_t> gPairedVictimNativeDispatches{}, gPairedVictimNativeStarted{};
std::atomic<std::uint64_t> gPairedVictimAvailabilityOverrides{};
std::atomic<std::uint64_t> gPairedPlayerPreparationRemaps{}, gPairedPlayerCategoryRemaps{};
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
std::atomic<std::uint64_t> gTargetSuccessionSamples{}, gTargetSuccessionDropped{};
std::atomic<std::uint64_t> gPostDeathSuccessionsPreserved{}, gPostDeathArbitrationDeferrals{}, gPostDeathDecisionUncertain{};
std::atomic<std::uint64_t> gAiPlayerQueryRemaps{}, gAiPlayerSlotRemaps{}, gAiPlayerSlotRestoreFailures{};
std::atomic<std::uint64_t> gPostDeathDecisionResets{}, gPostDeathDecisionResetRejected{}, gPostDeathRoleTaskResets{};
std::atomic<std::uint64_t> gInteractionBindingsResolved{}, gInteractionBindingRejected{};
std::atomic<std::uint32_t> gLastHostAttachment{}, gLastLocalAttachment{}, gLastTransitionVariant{};
std::atomic<std::uint32_t> gActiveNativeGeneration{}, gActiveWorldGeneration{};
// Worker-safe value-only callback trace. Never read Actor or gCurrent here.
struct PairedGraphProbe final {
    std::uint64_t tick{}, peer{};
    std::uint32_t role{}, generation{}, event{}, side{}, phase{}, pairRevision{}, birth{};
    std::uint32_t sourceA{}, sourceB{}, victimA{}, victimB{}, sourceStage{0xFFFFFFFFU};
    std::uint8_t sourceState{}, victimState{}, sourceFinished{}, victimEnded{}, victimAcquired{};
    bool readable{};
};
std::array<PairedGraphProbe, kParticipantProbeRing> gPairedGraphProbes{};
std::mutex gPairedGraphMutex;
std::uint32_t gPairedGraphWrite{}, gPairedGraphRead{};
std::atomic<std::uint64_t> gPairedGraphDropped{};
void* gSourceGraphEventTrampoline{}, *gVictimGraphEventTrampoline{}, *gSourceGraphFinishTrampoline{};
std::atomic<void*> gSourceGraphEventOriginal{}, gVictimGraphEventOriginal{}, gSourceGraphFinishOriginal{};
void* gNavigationSubmitTrampoline{};
std::atomic<void*> gNavigationSubmitOriginal{};
std::atomic<std::uint32_t> gLastEntityA{}, gLastEntityB{}, gLastCharacterClass{};
std::atomic<std::uint32_t> gLastActionTypeRva{}, gLastActionPathHash{}, gLastActionStatus{};
std::atomic<std::uint32_t> gLastExecutionTypeRva{}, gLastExecutionState{}, gLastExecutionFlags{};
std::atomic<std::uint32_t> gLastRejectedExecutionTypeRva{}, gLastRejectedExecutionState{};
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
bool CertifiedExecutionCycle(std::uint32_t typeRva, std::uint32_t state) noexcept;
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
    if (!navigation::ValidGoal(s.navigation)) return false;
    if (s.version != 11U || s.nativeGeneration == 0U || s.sessionCookie == 0U ||
        s.worldGeneration == 0U || (s.entityA == 0U && s.entityB == 0U) ||
        s.birth == 0U || s.revision == 0U || !AICharacterClass(s.characterClass) ||
        (s.stateFlags & ~kStateFlags) != 0U || s.moveType > 3U ||
        s.actionStatus > 10U || s.actionDepth > 64U ||
        (s.interactionFlags & ~kInteractionFlags) != 0U ||
        s.interactionTransition > 2U || (s.interactionTransitionFlags & ~0xFFFFU) != 0U ||
        s.interactionVariant < -1 || s.interactionVariant > 0xFFFF ||
        s.interactionTransitionVariant < -1 || s.interactionTransitionVariant > 0xFFFF ||
        (s.lifecycleFlags & ~kLifecycleFlags) != 0U ||
        (s.executionFlags & ~kExecutionFlags) != 0U || s.executionCycleState > 0xFFU) return false;
    for (const auto coordinate : s.position)
        if (!FiniteCoordinate(coordinate)) return false;
    for (const auto coordinate : s.interactionTargetPosition)
        if (!FiniteCoordinate(coordinate)) return false;
    if (!std::isfinite(s.yaw) || std::abs(s.yaw) > 3.3F ||
        !std::isfinite(s.velocityX) || !std::isfinite(s.velocityZ) ||
        std::abs(s.velocityX) > kMaximumVelocity || std::abs(s.velocityZ) > kMaximumVelocity ||
        !std::isfinite(s.locomotionIntensity) || s.locomotionIntensity < 0.0F ||
        s.locomotionIntensity > 64.0F)
        return false;
    if ((s.stateFlags & Moving) != 0U && s.locomotionRevision == 0U) return false;
    if ((s.interactionFlags & InteractionSourceCycleRetained) != 0U &&
        ((s.stateFlags & InteractionValid) == 0U || s.interactionTransition == 0U ||
         (s.interactionFlags & InteractionPairValid) == 0U)) return false;
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
    if (s.executionFlags != 0U && (s.stateFlags & ExecutionValid) == 0U) return false;
    if ((s.stateFlags & ExecutionValid) != 0U &&
        (s.executionRevision == 0U || s.executionCycleTypeRva == 0U)) return false;
    if ((s.executionFlags & ExecutionCertified) != 0U &&
        ((s.executionFlags & ExecutionTransition) == 0U ||
         !CertifiedExecutionCycle(s.executionCycleTypeRva, s.executionCycleState))) return false;
    if (s.lifecycleFlags != 0U && s.lifecycleRevision == 0U) return false;
    const auto& o = s.outcome;
    if ((o.flags & ~kOutcomeFlags) != 0U || o.definition > 0xFFFFU ||
        (o.kind != 0U && o.kind != 1U && o.kind != 3U) || o.hpBefore > 0xFFFFU ||
        o.hpPrepared > 0xFFFFU || o.hpReaction > 0xFFFFU || o.healthReaction > 4U ||
        !std::isfinite(o.sourceYaw) || std::abs(o.sourceYaw) > 100000.0F) return false;
    for (const auto coordinate : o.targetOffset) if (!FiniteCoordinate(coordinate)) return false;
    if (o.revision != 0U && ((s.stateFlags & InteractionValid) == 0U ||
        s.interactionTargetPeer == 0U || s.interactionTransition == 0U ||
        (s.interactionFlags & InteractionNativeOutcomeRequired) == 0U ||
        (o.flags & OutcomePrepared) == 0U || o.hpBefore == 0U || o.hpPrepared > o.hpBefore)) return false;
    if ((o.revision == 0U) != (o.flags == 0U) ||
        ((o.flags & OutcomeNativeDeath) != 0U && (o.flags & OutcomeVictimAdmitted) == 0U) ||
        ((o.flags & OutcomeTerminal) != 0U) != (o.revision != 0U && o.hpReaction == 0U && o.healthReaction == 4U)) return false;
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
bool ReleaseParticipantNavigation(ParticipantNavigation& navigation) noexcept {
    if (navigation.faulted) return false;
    if (navigation.initialized) {
        // The buffer owns a shared reference to the path request, not to an
        // old Character. Release remains valid across a level invalidation;
        // it never reads navigation.character or the old registry.
        __try {
            reinterpret_cast<NavigationRelease>(gBase + 0x002F58D0U)(navigation.native.data());
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            navigation.faulted = true;
            gBackstageNavigationRejected.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
    }
    navigation = {};
    return true;
}
void PumpHostNavigationContext() noexcept {
    const auto generation = gGenerations->CurrentGeneration();
    const bool host = gRole.load(std::memory_order_acquire) == Role::Host;
    if (!host && !gNavigationHostActive) return;
    gNavigationHostActive = host;
    const bool changed = generation != gNavigationGeneration ||
        gCurrent.sessionCookie != gNavigationSession || gCurrent.worldGeneration != gNavigationWorld;
    if (!host || changed) {
        for (auto& navigation : gParticipantNavigation)
            if (navigation.initialized) static_cast<void>(ReleaseParticipantNavigation(navigation));
        gNavigationGeneration = generation;
        gNavigationSession = gCurrent.sessionCookie;
        gNavigationWorld = gCurrent.worldGeneration;
        gNavigationMaintenance = 0U;
    }
    if (!host || !Stable()) return;
    const auto now = GetTickCount64();
    if (!changed && now - gNavigationMaintenance < kRefreshPeriod) return;
    gNavigationMaintenance = now;
    for (auto& navigation : gParticipantNavigation) {
        if (!navigation.initialized || navigation.faulted) continue;
        std::uint32_t birth{};
        const auto participant = ParticipantForPeer(navigation.peer);
        if (participant != navigation.character || !LivingCharacter(participant) ||
            !Read(participant + 0x50U, birth) || birth != navigation.runtimeId)
            static_cast<void>(ReleaseParticipantNavigation(navigation));
    }
}
std::uintptr_t HostBackstageOwner(const std::uintptr_t sweep) noexcept {
    std::uintptr_t owner{}, adapter{}, ownedSweep{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() ||
        !Read(sweep, owner) || !Eligible(owner, type) ||
        !Read(owner + 0x34F0U, adapter) || adapter == 0U ||
        !Read(adapter + 0x1CU, ownedSweep) || ownedSweep != sweep) return 0U;
    return owner;
}
bool HostBackstageParticipant(const std::uintptr_t sweep, const std::uintptr_t target) noexcept {
    const auto owner = HostBackstageOwner(sweep);
    return owner != 0U && gAiPlayerQueryOwner == owner &&
        RegisteredPlayerRepresentation(target) && PeerForParticipant(target) != 0U && LivingCharacter(target);
}
std::uint32_t __cdecl BackstageParticipantEligibility(
    const std::uintptr_t sweep, const std::uintptr_t target) noexcept {
    if (!HostBackstageParticipant(sweep, target)) return 0U;
    std::uintptr_t control{};
    std::uint8_t local{};
    if (!Read(target + 0x31CU, control) ||
        (control != 0U && !Read(control + 0x48U, local))) return 0U;
    if (control != 0U && local != 0U) return 0U; // Already admitted by retail.
    gBackstageEligibilityOverrides.fetch_add(1U, std::memory_order_relaxed);
    return 1U;
}
bool UpdateParticipantNavigation(ParticipantNavigation& navigation, const ULONGLONG now) noexcept {
    if (navigation.faulted) return false;
    if (navigation.sessionCookie != gNavigationSession ||
        navigation.nativeGeneration != gNavigationGeneration || navigation.worldGeneration != gNavigationWorld ||
        (navigation.lastUpdate != 0U && now - navigation.lastUpdate < kSendPeriod)) return true;

    // The retail game only ever needed a route toward THE_PLAYER. In AIMP the
    // Host's real player can be terminal-dead while the backstage sweep is
    // evaluating a living PlayerRepresentation. Using the dead Host's old
    // Character+3500 objective leaves this synthetic cache enabled but with no
    // destination. Give each participant cache the position of the exact
    // participant for which retail requested navigation instead. This does NOT
    // choose an AI target: the native sweep still owns candidate selection,
    // eligibility and route consumption.
    const auto target = navigation.character;
    std::uint32_t birth{};
    float goal[3]{};
    if (target == 0U || ParticipantForPeer(navigation.peer) != target || !LivingCharacter(target) ||
        !Read(target + 0x50U, birth) || birth != navigation.runtimeId ||
        !Read(target + 0x70U, goal[0]) || !Read(target + 0x74U, goal[1]) ||
        !Read(target + 0x78U, goal[2]) || !FiniteCoordinate(goal[0]) ||
        !FiniteCoordinate(goal[1]) || !FiniteCoordinate(goal[2])) return false;

    __try {
        // 006C5D70: ECX=route cache, goal pointer on stack, RET 4.
        // 00721270: ECX=route cache, plain RET. Only updates/consumes the
        // native path request; does not run the participant's suppressed AI.
        reinterpret_cast<NavigationGoal>(gBase + 0x002C5D70U)(navigation.native.data(), goal);
        reinterpret_cast<NavigationUpdate>(gBase + 0x00321270U)(navigation.native.data());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        navigation.faulted = true;
        gBackstageNavigationRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    navigation.lastUpdate = now;
    gBackstageNavigationUpdates.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
std::uintptr_t __cdecl BackstageParticipantNavigation(
    const std::uintptr_t sweep, const std::uintptr_t target) noexcept {
    std::uintptr_t retail{};
    if (!Read(target + 0x3500U, retail)) return 0U;
    if (retail != 0U || !HostBackstageParticipant(sweep, target)) return retail;
    const auto peer = PeerForParticipant(target);
    std::uint32_t birth{};
    if (!Read(target + 0x50U, birth)) return 0U;
    ParticipantNavigation* found{};
    ParticipantNavigation* free{};
    for (auto& navigation : gParticipantNavigation) {
        if (navigation.faulted) continue;
        if (!navigation.initialized) { if (free == nullptr) free = &navigation; continue; }
        if (navigation.peer != peer) continue;
        if (navigation.character != target || navigation.runtimeId != birth ||
            navigation.nativeGeneration != gCurrent.nativeGeneration ||
            navigation.worldGeneration != gCurrent.worldGeneration ||
            navigation.sessionCookie != gCurrent.sessionCookie) {
            if (!ReleaseParticipantNavigation(navigation)) return 0U;
        }
        found = &navigation;
        break;
    }
    if (found == nullptr) found = free;
    if (found == nullptr) return 0U;
    if (!found->initialized) {
        found->peer = peer; found->character = target; found->runtimeId = birth;
        found->nativeGeneration = gCurrent.nativeGeneration;
        found->worldGeneration = gCurrent.worldGeneration; found->sessionCookie = gCurrent.sessionCookie;
        const auto address = reinterpret_cast<std::uintptr_t>(found->native.data());
        // Constructor 006EE5B0: null query, Character +8, invalid nav mesh
        // revision +44, empty route/count +450 and disabled goal +454/+455.
        // An empty route still lets 0040BBF0 take its native scripted-vent
        // fallback (0040AD00); a missing cache returned before that fallback.
        if (!Write(address + 8U, target) || !Write(address + 0x44U, 0xFFFFFFFFU)) return 0U;
        found->initialized = true;
    }
    if (!UpdateParticipantNavigation(*found, GetTickCount64())) return 0U;
    gBackstageNavigationPlans.fetch_add(1U, std::memory_order_relaxed);
    // Used only as native EDI in 0040BBF0. Never installed at Character+3500,
    // never borrowed from the dead Host, and never transmitted over the wire.
    return reinterpret_cast<std::uintptr_t>(found->native.data());
}
bool NativeVictimFlowRunning(std::uintptr_t source, std::uintptr_t victim) noexcept;
bool HiddenBackstageCycle(const std::uintptr_t owner) noexcept {
    std::uintptr_t cycle{}, cycleOwner{};
    std::uint8_t state{};
    return owner != 0U && Read(owner + 0x36CU, cycle) && cycle != 0U &&
        Read(cycle + 4U, cycleOwner) && cycleOwner == owner &&
        Read(cycle + 0x1AU, state) && state == 0x4DU;
}
bool RearmBackstageSearchForSurvivor(const std::uintptr_t owner,
    const std::uintptr_t sweep) noexcept {
    // Retail has only one human player. Once that Character is terminal-dead,
    // its ordinary producers can leave controller +0C/+0D/+0E all clear after
    // the Alien withdraws. The physical 4D cycle then keeps running, but
    // FUN_006D85E0 continually leaves sweep+51 disabled and 0040C1C0 returns
    // before AIMP's participant eligibility/navigation adaptations are reached.
    //
    // Adapt only that impossible-in-retail multiplayer state: an authoritative
    // Host whose real local player is terminal-dead, an Alien already in its
    // hidden 4D cycle, and at least one living registered participant. A
    // transient +0D request is supplied only while the native producer runs;
    // the controller field is restored immediately. The producer itself still
    // owns sweep+51, so Character flag 0x1D and the native 0x18 policy remain
    // authoritative and can refuse the re-arm.
    if (owner == 0U || sweep == 0U || !HiddenBackstageCycle(owner)) return false;

    const auto realLocal = LocalPlayer();
    if (!TerminalDeadCharacter(realLocal) || NativeVictimFlowRunning(owner, realLocal)) return false;

    std::uint64_t peer{};
    if (LivingAiParticipant(owner, peer) == 0U || peer == 0U) return false;

    std::uintptr_t controller{}, ownedSweep{};
    std::uint8_t enabled{}, requestC{}, requestD{}, requestE{};
    if (!Read(owner + 0x34F0U, controller) || controller == 0U ||
        !Read(controller + 0x1CU, ownedSweep) || ownedSweep != sweep ||
        !Read(sweep + 0x51U, enabled) ||
        !Read(controller + 0x0CU, requestC) || !Read(controller + 0x0DU, requestD) ||
        !Read(controller + 0x0EU, requestE) || enabled != 0U) return false;

    // If retail already has a request but still disabled the sweep, an
    // independent native restriction is active. Never override that state.
    if (requestC != 0U || requestD != 0U || requestE != 0U) return false;

    gBackstageSurvivorRearmAttempts.fetch_add(1U, std::memory_order_relaxed);
    constexpr std::uint8_t kSurvivorRequest = 1U;
    if (!Write(controller + 0x0DU, kSurvivorRequest)) {
        gBackstageSurvivorRearmFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    bool refreshed{};
    __try {
        reinterpret_cast<BackstageActivationRefresh>(gBase + 0x002D85E0U)(
            reinterpret_cast<void*>(controller));
        refreshed = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        refreshed = false;
    }

    const bool restored = Write(controller + 0x0DU, requestD);
    if (!refreshed || !restored) {
        gBackstageSurvivorRearmFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    std::uint8_t after{};
    if (!Read(sweep + 0x51U, after)) {
        gBackstageSurvivorRearmFailures.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    if (after == 0U) {
        // Most importantly, this covers Character flag 0x1D: the native
        // producer saw our request and still rejected the search.
        gBackstageSurvivorRearmRestricted.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    gBackstageSurvivorRearms.fetch_add(1U, std::memory_order_relaxed);
    gLastBackstageSurvivorPeer.store(peer, std::memory_order_relaxed);
    return true;
}
std::uintptr_t __fastcall BackstageSelectedTargetDetour(void* selection, void*) noexcept {
    const auto target = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(gBase + 0x0000A3D0U)(selection);
    const auto owner = gAiPlayerQueryOwner;
    std::uintptr_t ownedSelection{};
    if (owner != 0U && gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable() &&
        Read(owner + 0x34A8U, ownedSelection) && ownedSelection == reinterpret_cast<std::uintptr_t>(selection) &&
        PeerForParticipant(target) != 0U && TerminalDeadCharacter(target) && !NativeVictimFlowRunning(owner, target)) {
        gBackstageDeadTargetsRejected.fetch_add(1U, std::memory_order_relaxed);
        return 0U; // Let the original native THE_PLAYER fallback run.
    }
    return target;
}
std::uint8_t __fastcall BackstageUpdateDetour(void* sweep, void*) noexcept {
    const auto original = reinterpret_cast<BackstageUpdate>(gBackstageUpdateOriginal.load());
    if (original == nullptr) return 0U;
    const auto owner = HostBackstageOwner(reinterpret_cast<std::uintptr_t>(sweep));
    const auto previous = gAiPlayerQueryOwner;
    if (owner != 0U) {
        gAiPlayerQueryOwner = owner;
        static_cast<void>(RearmBackstageSearchForSurvivor(
            owner, reinterpret_cast<std::uintptr_t>(sweep)));
    }
    std::uint8_t result{};
    { AiPlayerSlotScope context(owner); result = original(sweep); }
    gAiPlayerQueryOwner = previous;
    return result;
}
void __fastcall BackstageHoldDetour(void* cycle, void*) noexcept {
    const auto original = reinterpret_cast<BackstageHoldUpdate>(gBackstageHoldOriginal.load());
    if (original == nullptr) return;
    std::uintptr_t owner{}, currentCycle{};
    std::uint32_t type{};
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() ||
        !Read(reinterpret_cast<std::uintptr_t>(cycle) + 4U, owner) || !Eligible(owner, type) ||
        !Read(owner + 0x36CU, currentCycle) || currentCycle != reinterpret_cast<std::uintptr_t>(cycle)) owner = 0U;
    const auto previous = gAiPlayerQueryOwner;
    if (owner != 0U) { gAiPlayerQueryOwner = owner; gBackstageHoldContexts.fetch_add(1U, std::memory_order_relaxed); }
    // The 4D physical update runs outside BehaviourManager. Its closest-player
    // query must see the same surviving player context as native AI decisions.
    { AiPlayerSlotScope context(owner); original(cycle); }
    gAiPlayerQueryOwner = previous;
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
void Publish(Actor& actor) noexcept;
bool PublishOutcomeBinding(Actor& actor, const std::uintptr_t victim) noexcept {
    PairIdentity sourceIdentity{}, victimIdentity{};
    const auto slot = PublishedPairSource(actor.character, sourceIdentity);
    if (slot == kActors || !PublishedPairParticipant(victim, victimIdentity) ||
        victimIdentity.peer != actor.desired.interactionTargetPeer) return false;
    std::unique_lock lock(gOutcomeMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    auto& binding = gOutcomes[slot];
    const bool same = binding.source.character == sourceIdentity.character &&
        binding.source.runtimeId == sourceIdentity.runtimeId && binding.source.epoch == sourceIdentity.epoch &&
        binding.state.birth == actor.desired.birth &&
        binding.state.interactionPairRevision == actor.desired.interactionPairRevision &&
        binding.state.interactionTargetPeer == actor.desired.interactionTargetPeer;
    // Consumption belongs to the network attack, not to a disposable proxy
    // address. Rebinding the same peer cannot replay its damage or late finish.
    if (!same) binding = {};
    binding.source = sourceIdentity; binding.victim = victimIdentity;
    // Host workers own the result; refreshing the pair header must not erase it.
    const auto result = binding.state.outcome;
    binding.state = actor.desired;
    if (!Client()) binding.state.outcome = result;
    return true;
}
bool LoadOutcome(const std::uintptr_t source, const std::uintptr_t victim,
    OutcomeBinding& value) noexcept {
    PairIdentity identity{};
    const auto slot = PublishedPairSource(source, identity);
    if (slot == kActors) return false;
    std::unique_lock lock(gOutcomeMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    value = gOutcomes[slot];
    return value.source.character == source && value.source.runtimeId == identity.runtimeId &&
        value.source.epoch == identity.epoch && value.victim.character == victim &&
        value.state.interactionPairRevision != 0U && PairIdentityLive(value.victim);
}
bool ClaimOutcome(NativeVictimSubmission& submission, const std::size_t slot) noexcept {
    std::lock_guard lock(gOutcomeMutex);
    auto& binding = gOutcomes[slot];
    if (binding.source.character != submission.source || binding.victim.character != submission.victimBefore.character ||
        binding.source.epoch != submission.victimBefore.epoch ||
        binding.victim.runtimeId != submission.victimBefore.runtimeId ||
        binding.state.interactionPairRevision == 0U) return false;
    if (binding.claimed) {
        submission.duplicateOutcome = true;
        gOutcomeDuplicates.fetch_add(1U, std::memory_order_relaxed); return false;
    }
    const bool client = gRole.load(std::memory_order_acquire) == Role::Client;
    if (client && (binding.state.outcome.flags & OutcomePrepared) == 0U) return false;
    binding.claimed = true; binding.serial = submission.sourceSerial;
    submission.slot = slot; submission.pairRevision = binding.state.interactionPairRevision;
    submission.authoritativeClient = client;
    if (client) submission.outcome = binding.state.outcome;
    return true;
}
void RecordOutcome(NativeVictimSubmission& submission) noexcept {
    if (submission.authoritativeClient || submission.slot == kActors ||
        (submission.outcome.flags & OutcomePrepared) == 0U) return;
    float sourcePosition[3]{};
    float sourceYaw{};
    bool pose = submission.source != 0U && Read(submission.source + kYawOffset, sourceYaw);
    for (std::size_t i = 0U; i < 3U; ++i)
        pose = Read(submission.source + kPositionOffset + i * 4U, sourcePosition[i]) &&
            FiniteCoordinate(sourcePosition[i]) && pose;
    std::lock_guard lock(gOutcomeMutex);
    auto& binding = gOutcomes[submission.slot];
    if (binding.state.interactionPairRevision != submission.pairRevision ||
        binding.serial != submission.sourceSerial || binding.source.epoch != submission.victimBefore.epoch ||
        binding.victim.character != submission.victimBefore.character ||
        binding.victim.runtimeId != submission.victimBefore.runtimeId ||
        binding.victim.peer != submission.victimBefore.peer) return;
    if (binding.state.outcome.revision == 0U && pose && std::isfinite(sourceYaw) && std::abs(sourceYaw) <= 3.3F) {
        // Retain the admission pose even if the first engine publication comes
        // after a withdrawal. Later world/vent snapshots must follow the result.
        for (std::size_t i = 0U; i < 3U; ++i) binding.state.position[i] = sourcePosition[i];
        binding.state.yaw = sourceYaw; binding.state.stateFlags |= PoseValid;
    }
    submission.outcome.flags |= binding.state.outcome.flags & OutcomeNativeDeath;
    submission.outcome.revision = binding.state.outcome.revision + 1U;
    binding.state.outcome = submission.outcome;
    gOutcomeCaptured.fetch_add(1U, std::memory_order_relaxed);
}
void CaptureOutcome(Actor& actor) noexcept {
    const auto slot = static_cast<std::size_t>(&actor - gActors.data());
    std::unique_lock lock(gOutcomeMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto& binding = gOutcomes[slot];
    if (binding.source.character == actor.character && binding.source.runtimeId == actor.runtimeId &&
        binding.source.epoch == gPairBindingEpoch.load(std::memory_order_acquire) &&
        binding.state.birth == actor.birth &&
        binding.state.interactionPairRevision == actor.desired.interactionPairRevision &&
        binding.state.outcome.revision > actor.desired.outcome.revision)
        actor.desired.outcome = binding.state.outcome;
}
void ObserveVictimOutcome(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uint8_t serial, NativeOutcome& outcome) noexcept {
    std::uint16_t hp{};
    if (!Read(victim + 0x33CU, hp) || !Read(victim + 0x32CU, outcome.healthReaction)) return;
    outcome.hpReaction = hp;
    outcome.flags &= ~OutcomeTerminal;
    if (hp == 0U && outcome.healthReaction == 4U) outcome.flags |= OutcomeTerminal;
    std::uintptr_t cycle{}, table{}, linkedSource{}, linkedCycle{}, current{};
    std::uint8_t state{}, acquired{}, ended{}, flags{}, linkedSerial{}, interactive{};
    if (Read(victim + 0x36CU, cycle) && Read(cycle, table) && table == gBase + 0x00EAE690U &&
        Read(cycle + 0x1AU, state) && state == 0x4FU && Read(cycle + 0xD9U, acquired) && acquired != 0U &&
        Read(cycle + 0x68U, linkedSource) && linkedSource == source &&
        Read(cycle + 0xB0U, linkedCycle) && Read(source + 0x36CU, current) && current == linkedCycle &&
        Read(linkedCycle + 0x19U, linkedSerial) && linkedSerial == serial) {
        outcome.flags |= OutcomeVictimAdmitted;
        if (outcome.kind != 0U && Read(cycle + 0xDAU, interactive) && interactive == 0U)
            outcome.flags |= OutcomeNativeDeath;
        if (Read(cycle + 0xD8U, ended) && Read(cycle + 8U, flags) && (ended != 0U || (flags & 6U) != 0U))
            outcome.flags |= OutcomeVictimEnded;
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
    if (actor.backstageKnown && actor.backstage) flags |= LifecycleCharacterBackstage;
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

void CaptureCharacterBackstage(Actor& actor) noexcept {
    std::uint8_t hidden{};
    if (!Read(actor.character + 0x2DAU, hidden)) return;
    const bool backstage = hidden != 0U;
    gBackstageCaptured.fetch_add(1U, std::memory_order_relaxed);
    const bool changed = actor.backstageKnown && actor.backstage != backstage;
    const bool initialHidden = !actor.backstageKnown && backstage;
    actor.backstageKnown = true; actor.backstage = backstage;
    if (changed || initialHidden) {
        gBackstageTransitions.fetch_add(1U, std::memory_order_relaxed);
        MarkLifecycle(actor, LifecycleSpatialDiscontinuity);
    } else RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
}

// Match retail's backstage restore: ECX Character, one byte stack argument,
// RET 4, followed on re-entry by animated Character vfunc+1C / frontstage reset.
// The native entries update presentation/collision ownership themselves.
bool SetCharacterBackstage(const std::uintptr_t character, const bool backstage) noexcept {
    if (!EngineThread() || !Stable() || !Client() || character == 0U) return false;
    __try {
        using Set = void(__thiscall*)(void*, std::uint8_t);
        reinterpret_cast<Set>(gBase + kCharacterBackstageRva)(
            reinterpret_cast<void*>(character), backstage ? 1U : 0U);
        if (!backstage) {
            std::uintptr_t table{}, getter{};
            if (!Read(character, table) || !Read(table + 0x1CU, getter) ||
                getter < gBase + 0x1000U || getter >= gBase + 0x00D9E13BU) return false;
            const auto animated = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(
                reinterpret_cast<void*>(character));
            if (animated == 0U) return false;
            reinterpret_cast<void(__thiscall*)(void*)>(gBase + kCharacterFrontstageRva)(
                reinterpret_cast<void*>(animated));
        }
        std::uint8_t actual{};
        return Read(character + 0x2DAU, actual) && (actual != 0U) == backstage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

bool ApplyAuthoritativeBackstage(Actor& actor, const State& state) noexcept {
    const bool backstage = (state.lifecycleFlags & LifecycleCharacterBackstage) != 0U;
    // Preserve the withdraw controller's own presentation while Host dormancy
    // has no live Character sample. A live frontstage snapshot repairs it.
    if (!backstage && (state.lifecycleFlags & (LifecycleWithdrawn | LifecycleHostDormant)) != 0U)
        return true;
    std::uint8_t actual{};
    if (!Read(actor.character + 0x2DAU, actual)) return false;
    if ((actual != 0U) == backstage) return true;
    if (!SetCharacterBackstage(actor.character, backstage)) {
        gBackstageRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    actor.completedRevision = 0U; actor.lastProxyRefresh = 0U;
    actor.placementPending = !backstage;
    gBackstageApplied.fetch_add(1U, std::memory_order_relaxed);
    return true;
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
    return (state.lifecycleFlags & (LifecycleWithdrawn | LifecycleHostDormant | LifecycleCharacterBackstage)) != 0U;
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
    navigation::Cancel(actor.character);
    actor.poseCount = 0U; actor.poseRevision = 0U; actor.poseInterpolating = false;
    static_cast<void>(character_direct_step_runtime::Stop(actor.character));
    const bool tree = actor.tree == 0U || SuspendExistingTree(actor);
    const bool task = ClearRoleTaskExecutor(actor);
    if (task) actor.roleTaskCleanupPending = false;
    actor.completedRevision = 0U;
    actor.lastProxyRefresh = 0U;
    gExecutionBarriers.fetch_add(1U, std::memory_order_relaxed);
    return tree && task;
}

bool CharacterAtPlacement(const std::uintptr_t character, const float* coordinates) noexcept {
    float distanceSquared{};
    for (std::size_t i = 0U; i < 3U; ++i) {
        float actual{};
        if (!Read(character + kPositionOffset + i * sizeof(float), actual) ||
            !FiniteCoordinate(actual)) return false;
        const auto delta = actual - coordinates[i];
        distanceSquared += delta * delta;
    }
    return std::isfinite(distanceSquared) && distanceSquared <= kPlacementTolerance * kPlacementTolerance;
}

bool RelocateCharacterEntry(const std::uintptr_t character, const float* position, const float yaw) noexcept {
    __try {
        // FUN_007C6330 is the Character-level retail placement carrier used by
        // vent exits and scripted relocation. ECX Character, position/yaw/mode
        // on the stack, RET 0C. It sets the pose and requests retail physical
        // reconciliation; it works before a hidden body's registry is ready.
        // Registry FUN_007ECCA0 refreshes the current physical transform and
        // cannot certify that a requested pose actually reached the Character.
        using Relocate = void(__thiscall*)(void*, const float*, float, std::uint32_t);
        reinterpret_cast<Relocate>(gBase + kCharacterRelocateRva)(
            reinterpret_cast<void*>(character), position, yaw, 1U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

bool PlaceCharacterPose(const std::uintptr_t character, const float* coordinates, const float yaw) noexcept {
    if (!EngineThread() || !Stable() || !Client() || character == 0U || coordinates == nullptr ||
        !std::isfinite(yaw) || std::abs(yaw) > 3.3F) return false;
    for (std::size_t i = 0U; i < 3U; ++i)
        if (!FiniteCoordinate(coordinates[i])) return false;
    // Do not re-enter native relocation every pair retry: it deliberately
    // disables execution for one retail frame while the physical pose settles.
    if (CharacterAtPlacement(character, coordinates)) {
        std::string error;
        if (character_direct_step_runtime::ApplyReplicatedYaw(character, yaw, error)) {
            gPlacementVerified.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
    } else {
        alignas(16) const std::array<float, 4> position{
            coordinates[0], coordinates[1], coordinates[2], 0.0F};
        if (RelocateCharacterEntry(character, position.data(), yaw) &&
            CharacterAtPlacement(character, coordinates)) {
            gPlacementVerified.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
    }
    gPlacementRejected.fetch_add(1U, std::memory_order_relaxed);
    return false;
}

bool SnapAuthoritativePose(Actor& actor, const State& state) noexcept {
    if ((state.stateFlags & PoseValid) == 0U ||
        !PlaceCharacterPose(actor.character, state.position, state.yaw)) return false;
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

bool CertifiedExecutionCycle(const std::uint32_t typeRva,
    const std::uint32_t state) noexcept {
    return FindExecutionTransition(typeRva, state) != nullptr;
}

void CaptureExecutionCycle(Actor& actor) noexcept {
    std::uintptr_t cycle{}, owner{}, vtable{};
    std::uint8_t state{};
    const bool valid = Read(actor.character + 0x36CU, cycle) && cycle != 0U &&
        Read(cycle + 4U, owner) && owner == actor.character && Read(cycle, vtable) &&
        vtable > gBase && vtable - gBase <= 0x02000000U && Read(cycle + 0x1AU, state);
    const auto typeRva = valid ? static_cast<std::uint32_t>(vtable - gBase) : 0U;
    std::uint32_t flags{};
    if (valid) {
        const bool paired = state == 0x42U || state == 0x47U || state == 0x4EU || state == 0x4FU;
        const bool transition = state != 0x01U && state != 0x38U && !paired;
        if (paired) flags |= ExecutionPaired;
        if (transition) flags |= ExecutionTransition;
        if (transition && CertifiedExecutionCycle(typeRva, state)) flags |= ExecutionCertified;
    }
    const bool wasValid = (actor.desired.stateFlags & ExecutionValid) != 0U;
    const bool changed = valid != wasValid || actor.desired.executionCycleTypeRva != typeRva ||
        actor.desired.executionCycleState != static_cast<std::uint32_t>(state) ||
        actor.desired.executionFlags != flags;
    if (changed) {
        ++actor.desired.executionRevision;
        if (actor.desired.executionRevision == 0U) actor.desired.executionRevision = 1U;
        actor.lastSend = 0U;
        gExecutionTransitions.fetch_add(1U, std::memory_order_relaxed);
    }
    actor.desired.executionCycleTypeRva = typeRva;
    actor.desired.executionCycleState = valid ? static_cast<std::uint32_t>(state) : 0U;
    actor.desired.executionFlags = flags;
    if (valid) {
        actor.desired.stateFlags |= ExecutionValid;
        gExecutionCaptured.fetch_add(1U, std::memory_order_relaxed);
        gLastExecutionTypeRva.store(typeRva, std::memory_order_relaxed);
        gLastExecutionState.store(static_cast<std::uint32_t>(state), std::memory_order_relaxed);
        gLastExecutionFlags.store(flags, std::memory_order_relaxed);
    } else {
        actor.desired.stateFlags &= ~ExecutionValid;
    }
}


void CapturePose(Actor& actor, const ULONGLONG now) noexcept {
    // Pose is recovery/lifecycle evidence only. Continuous AI movement is
    // captured independently from the native locomotion-intent tap below.
    if (actor.hasLastPosition && now - actor.lastPoseSample < kSendPeriod) return;
    float position[3]{}, yaw{};
    if (!Read(actor.character + kPositionOffset, position[0]) ||
        !Read(actor.character + kPositionOffset + 4U, position[1]) ||
        !Read(actor.character + kPositionOffset + 8U, position[2]) ||
        !Read(actor.character + kYawOffset, yaw) ||
        !FiniteCoordinate(position[0]) || !FiniteCoordinate(position[1]) ||
        !FiniteCoordinate(position[2]) || !std::isfinite(yaw) || std::abs(yaw) > 3.3F) {
        actor.desired.stateFlags &= ~PoseValid;
        gPoseRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    if (actor.hasLastPosition && now > actor.lastPoseSample) {
        const auto elapsed = now - actor.lastPoseSample;
        const auto dx = position[0] - actor.lastPosition[0];
        const auto dy = position[1] - actor.lastPosition[1];
        const auto dz = position[2] - actor.lastPosition[2];
        const auto spatialDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (elapsed <= 500ULL && std::isfinite(spatialDistance) &&
            (spatialDistance >= kLifecycleDiscontinuityDistance ||
             std::abs(dy) >= kLifecycleVerticalDiscontinuity)) {
            MarkLifecycle(actor, LifecycleSpatialDiscontinuity |
                (actor.traversalAction ? LifecycleTraversalAction : 0U));
        }
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        actor.lastPosition[i] = position[i]; actor.desired.position[i] = position[i];
    }
    actor.hasLastPosition = true; actor.lastPoseSample = now;

    if ((actor.desired.interactionFlags & InteractionCycleActive) != 0U) {
        const auto target = (actor.desired.interactionFlags & InteractionTargetIsParticipant) != 0U
            ? ParticipantForPeer(actor.desired.interactionTargetPeer)
            : FindCharacterByIdentity(actor.desired.interactionTargetA, actor.desired.interactionTargetB);
        float targetPosition[3]{};
        bool readable = target != 0U;
        for (std::size_t i = 0U; i < 3U; ++i) {
            const bool coordinate = target != 0U &&
                Read(target + kPositionOffset + i * sizeof(float), targetPosition[i]) &&
                FiniteCoordinate(targetPosition[i]);
            readable = readable && coordinate;
        }
        if (readable)
            for (std::size_t i = 0U; i < 3U; ++i) actor.desired.interactionTargetPosition[i] = targetPosition[i];
    }
    actor.desired.yaw = yaw;
    actor.desired.stateFlags |= PoseValid;

    // The pre-transform intent tap only runs for the retail Move cycle. A Host
    // transition to Idle therefore closes the current intention here rather
    // than relying solely on the Client lease timeout.
    std::uintptr_t cycle{};
    std::uint8_t cycleState{};
    if (Read(actor.character + 0x36CU, cycle) && cycle != 0U &&
        Read(cycle + 0x1AU, cycleState) && cycleState != 0x38U &&
        (actor.desired.stateFlags & Moving) != 0U) {
        actor.desired.stateFlags &= ~Moving;
        actor.desired.velocityX = actor.desired.velocityZ = 0.0F;
        actor.desired.locomotionIntensity = 0.0F;
        ++actor.desired.locomotionRevision;
        if (actor.desired.locomotionRevision == 0U) actor.desired.locomotionRevision = 1U;
    }
    gPoseCaptured.fetch_add(1U, std::memory_order_relaxed);
}

Actor* Find(const std::uintptr_t c) noexcept {
    for (auto& actor : gActors) if (actor.character == c) return &actor;
    return nullptr;
}

void ObserveNativeAIIntent(const std::uintptr_t character,
    const character_direct_step_runtime::NativeAIIntent& intent) noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable() || character == 0U ||
        PairSourceBusy(character) || PlayerRepresentation(character)) return;
    auto* actor = Find(character);
    if (actor == nullptr || actor->duplicate || actor->faulted || actor->blocked) return;

    auto x = intent.moving ? intent.directionX : 0.0F;
    auto z = intent.moving ? intent.directionZ : 0.0F;
    auto intensity = intent.moving ? intent.intensity : 0.0F;
    const auto magnitude = std::hypot(x, z);
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(intensity) ||
        !std::isfinite(magnitude) || magnitude > kMaximumVelocity ||
        intensity < 0.0F || intensity > 64.0F ||
        (intent.moveType > 3U && intent.moveType != 0xFFFFFFFFU)) {
        gIntentRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const bool wasMoving = (actor->desired.stateFlags & Moving) != 0U;
    const bool moving = intent.moving && magnitude >= kMotionThreshold;
    const auto moveType = intent.moveType <= 3U ? intent.moveType : actor->desired.moveType;
    const bool changed = wasMoving != moving || actor->desired.moveType != moveType ||
        std::abs(actor->desired.velocityX - x) > 0.002F ||
        std::abs(actor->desired.velocityZ - z) > 0.002F ||
        std::abs(actor->desired.locomotionIntensity - intensity) > 0.002F;
    actor->desired.velocityX = moving ? x : 0.0F;
    actor->desired.velocityZ = moving ? z : 0.0F;
    actor->desired.locomotionIntensity = moving ? intensity : 0.0F;
    actor->desired.moveType = moveType;
    if (moving) actor->desired.stateFlags |= Moving;
    else actor->desired.stateFlags &= ~Moving;
    if (changed) {
        ++actor->desired.locomotionRevision;
        if (actor->desired.locomotionRevision == 0U) actor->desired.locomotionRevision = 1U;
        actor->lastSend = 0U;
    }
    gIntentCaptured.fetch_add(1U, std::memory_order_relaxed);
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
bool ClientNativeVictimOwnsPair(const std::uintptr_t source) noexcept {
    PairIdentity participant{};
    std::uintptr_t sourceCycle{}, target{}, victimCycle{};
    std::uint8_t state{}, interactive{};
    // An observer's NPC-backed player victim owns a genuine fatal graph too.
    // Protect the exact current pair for every human participant, not only the
    // first-person local player. A live interactive struggle remains interruptible.
    return gRole.load(std::memory_order_acquire) == Role::Client &&
        Read(source + 0x36CU, sourceCycle) && Read(sourceCycle + 0x1AU, state) && state == 0x4EU &&
        Read(sourceCycle + 0x44U, target) && PublishedPairParticipant(target, participant) &&
        NativeVictimFlowRunning(source, target) &&
        Read(target + 0x36CU, victimCycle) && Read(victimCycle + 0xDAU, interactive) &&
        (interactive == 0U || TerminalDeadCharacter(target));
}

enum class RemoteObserverVictimState : std::uint8_t {
    Unknown = 0U,
    Active = 1U,
    Complete = 2U
};

RemoteObserverVictimState ReadRemoteObserverVictimState(const Actor& actor,
    const std::uintptr_t target, const OutcomeBinding& binding) noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Client || target == 0U ||
        LocalClientVictim(target) || binding.victim.character != target ||
        binding.state.birth != actor.desired.birth ||
        binding.state.interactionPairRevision != actor.desired.interactionPairRevision ||
        binding.state.interactionTargetPeer != actor.desired.interactionTargetPeer ||
        !PairIdentityLive(binding.victim)) return RemoteObserverVictimState::Unknown;

    std::uintptr_t sourceCycle{}, victimCycle{}, owner{}, table{}, linkedSource{}, linkedCycle{}, current{};
    std::uint32_t cachedBirth{};
    std::uint8_t sourceState{}, victimState{}, acquired{}, ended{}, flags{}, interactive{};
    if (!Read(actor.character + 0x36CU, sourceCycle) || sourceCycle == 0U ||
        !Read(sourceCycle + 0x1AU, sourceState) || sourceState != 0x4EU ||
        !Read(target + 0x36CU, victimCycle) || victimCycle == 0U ||
        !Read(victimCycle + 4U, owner) || owner != target ||
        !Read(victimCycle, table) || table != gBase + 0x00EAE690U ||
        !Read(victimCycle + 0x1AU, victimState) || victimState != 0x4FU ||
        !Read(victimCycle + 0x68U, linkedSource) || linkedSource != actor.character ||
        !Read(victimCycle + 0x6CU, cachedBirth) || cachedBirth != binding.source.runtimeId ||
        !Read(victimCycle + 0xB0U, linkedCycle) || linkedCycle != sourceCycle ||
        !Read(actor.character + 0x36CU, current) || current != sourceCycle ||
        !Read(victimCycle + 0xD9U, acquired) ||
        !Read(victimCycle + 0xD8U, ended) ||
        !Read(victimCycle + 8U, flags) ||
        !Read(victimCycle + 0xDAU, interactive))
        return RemoteObserverVictimState::Unknown;

    if (acquired == 0U) return RemoteObserverVictimState::Active;
    if (interactive != 0U) return RemoteObserverVictimState::Active;
    if (ended != 0U || (flags & 6U) != 0U) return RemoteObserverVictimState::Complete;
    return RemoteObserverVictimState::Active;
}

bool ObserverReleasedPairMatches(const Actor& actor, const State& state) noexcept {
    return actor.observerReleasedBirth != 0U &&
        actor.observerReleasedBirth == state.birth &&
        actor.observerReleasedPairRevision == state.interactionPairRevision &&
        actor.observerReleasedTargetPeer == state.interactionTargetPeer;
}

void ClearObserverReleaseMemory(Actor& actor) noexcept {
    actor.observerReleasedBirth = 0U;
    actor.observerReleasedPairRevision = 0U;
    actor.observerReleasedInteractionRevision = 0U;
    actor.observerReleasedActionRevision = 0U;
    actor.observerReleasedExecutionRevision = 0U;
    actor.observerReleasedRoleTaskRevision = 0U;
    actor.observerReleasedLifecycleRevision = 0U;
    actor.observerReleasedLocomotionRevision = 0U;
    actor.observerReleasedNavigationRevision = 0U;
    actor.observerReleasedNavigationRequestRevision = 0U;
    actor.observerReleasedTargetPeer = 0U;
    actor.observerHandoffNeutral = false;
}

bool ObserverHandoffHasNewAuthority(const Actor& actor, const State& state) noexcept {
    if (!actor.observerHandoffNeutral) return true;
    if (!ObserverReleasedPairMatches(actor, state)) return true;
    // A newer compatible Host state can have arrived BEFORE local completion.
    // Do not record that state as the release baseline and then wait forever
    // for a second change (e.g. a stationary goal with unchanged steering).
    if (HostLeftPairedExecution((state.stateFlags & ExecutionValid) != 0U,
        (state.executionFlags & ExecutionPaired) != 0U,
        (state.interactionFlags & InteractionCycleActive) != 0U,
        (state.interactionFlags & InteractionSourceCycleRetained) != 0U)) return true;
    return state.actionRevision > actor.observerReleasedActionRevision ||
        state.executionRevision > actor.observerReleasedExecutionRevision ||
        state.roleTaskRevision > actor.observerReleasedRoleTaskRevision ||
        state.lifecycleRevision > actor.observerReleasedLifecycleRevision ||
        state.locomotionRevision > actor.observerReleasedLocomotionRevision ||
        NavigationAuthorityAdvanced(state.navigation.flags, state.navigation.revision,
            state.navigation.requestRevision, actor.observerReleasedNavigationRevision,
            actor.observerReleasedNavigationRequestRevision) ||
        (state.interactionRevision > actor.observerReleasedInteractionRevision &&
            (state.interactionFlags & (InteractionCycleActive | InteractionSourceCycleRetained)) == 0U);
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
// A PlayerSync representation has an NPC controller. Retail 00662670
// consequently skips the player damage/nonfatal choice, and 006E3960 filters
// its animation definitions as an NPC. The actual Client still takes both
// player branches. Recognize the same remote participant at these two queries,
// only inside its already leased Host preparation. Keep every subsequent
// retail difficulty, health, geometry, definition and fatal-fallback decision.
bool RemotePlayerPreparation(const std::uintptr_t victim,
    const std::uintptr_t sourceCycle) noexcept {
    const auto* submission = gNativeVictimSubmission;
    if (gRole.load(std::memory_order_acquire) != Role::Host ||
        !PairGenerationStable() || submission == nullptr || !submission->livingBefore ||
        submission->captured || submission->sourceCycle != sourceCycle ||
        submission->victimBefore.character != victim || submission->victimBefore.peer == 0U ||
        !PairIdentityLive(submission->victimBefore) || !RegisteredPlayerRepresentation(victim) ||
        !VictimAdmissionMatches(victim, submission->source, sourceCycle)) return false;
    std::uintptr_t current{}, source{}, target{};
    std::uint32_t birth{};
    std::uint8_t serial{}, state{}, flags{};
    return Read(submission->source + 0x36CU, current) && current == sourceCycle &&
        Read(sourceCycle + 4U, source) && source == submission->source &&
        Read(sourceCycle + 0x44U, target) && target == victim &&
        Read(sourceCycle + 0x48U, birth) && birth == submission->victimBefore.runtimeId &&
        Read(sourceCycle + 0x19U, serial) && serial == submission->sourceSerial &&
        Read(sourceCycle + 0x1AU, state) && state == 0x4EU &&
        Read(sourceCycle + 8U, flags) && (flags & 6U) == 0U;
}
void CommitOutcomeHealth(const std::uintptr_t victim, const std::uintptr_t source,
    const std::uint32_t hp) noexcept {
    const auto original = reinterpret_cast<NativeHealthCommit>(gHealthCommitOriginal.load(std::memory_order_acquire));
    if (original != nullptr) original(reinterpret_cast<void*>(victim + 0x320U), hp, source);
}
void __fastcall OutcomeHealthCommitDetour(void* context, void*, const std::uint32_t hp,
    const std::uintptr_t source) noexcept {
    const auto original = reinterpret_cast<NativeHealthCommit>(gHealthCommitOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto* submission = gNativeVictimSubmission;
    if (submission != nullptr && submission->authoritativeClient && submission->source == source &&
        reinterpret_cast<std::uintptr_t>(context) == submission->victimBefore.character + 0x320U &&
        PairIdentityLive(submission->victimBefore)) {
        original(context, submission->victimDispatch ? submission->outcome.hpReaction : submission->outcome.hpPrepared, source);
        return;
    }
    original(context, hp, source);
}
std::uintptr_t ResolvedClientPreparation(const std::uintptr_t victim,
    const std::uintptr_t sourceCycle) noexcept {
    auto* submission = gNativeVictimSubmission;
    constexpr std::uintptr_t rejected = 2U;
    if (submission == nullptr || !submission->authoritativeClient ||
        submission->sourceCycle != sourceCycle || submission->victimBefore.character != victim ||
        !VictimAdmissionMatches(victim, submission->source, sourceCycle)) return rejected;
    // A rejected result aborts this producer at its existing retail epilogue;
    // it must never fall back to an independent local difficulty/geometry roll.
    PairIdentity source{};
    std::uintptr_t manager{}, table{}, getter{}, definition{};
    std::uint32_t kind{};
    const auto& result = submission->outcome;
    if ((result.flags & OutcomePrepared) == 0U ||
        PublishedPairSource(submission->source, source) == kActors || source.animated == 0U ||
        !Read(source.animated + 0x34B4U, manager) || manager == 0U ||
        !Read(manager, table) || !Read(table + 0x34U, getter) ||
        getter != gBase + 0x002D60F0U) {
        gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed); return rejected;
    }
    __try {
        if ((result.flags & OutcomeVictimAdmitted) == 0U) {
            // A native interruption may keep preparation damage without ever
            // acquiring 4F. Replay that nonterminal result, then use the retail
            // failed-source epilogue; do not invent an acquired victim graph.
            if ((result.flags & OutcomeTerminal) != 0U) {
                gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed); return rejected;
            }
            const auto damage = result.hpBefore - result.hpPrepared;
            if (damage != 0U)
                reinterpret_cast<NativeDamage>(gBase + 0x003CC900U)(reinterpret_cast<void*>(victim),
                    damage, submission->source, 0.0F, 0U, 1U, 0U, 0x16U, 0U, 0U);
            CommitOutcomeHealth(victim, submission->source, result.hpReaction);
            gOutcomeApplied.fetch_add(1U, std::memory_order_relaxed);
            return rejected;
        }
        definition = reinterpret_cast<std::uintptr_t(__thiscall*)(void*, std::uint32_t)>(getter)(
            reinterpret_cast<void*>(manager), result.definition);
        if (definition == 0U || !Read(definition + 8U, kind) || kind != result.kind) {
            gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed); return rejected;
        }
        float position[3]{};
        for (std::size_t i = 0U; i < 3U; ++i)
            if (!Read(victim + kPositionOffset + i * 4U, position[i]) ||
                !FiniteCoordinate(position[i] + result.targetOffset[i])) return rejected;
        if (!Write(sourceCycle + 0xB8U, result.sourceYaw)) return rejected;
        for (std::size_t i = 0U; i < 3U; ++i)
            if (!Write(sourceCycle + 0xD0U + i * 4U, position[i] + result.targetOffset[i])) return rejected;
        const auto damage = result.hpBefore - result.hpPrepared;
        if (damage != 0U)
            reinterpret_cast<NativeDamage>(gBase + 0x003CC900U)(reinterpret_cast<void*>(victim),
                damage, submission->source, 0.0F, 0U, 1U, 0U, 0x16U, 0U, 0U);
        // Native gates can differ on a presentation Character. Commit the
        // witnessed Host value through the SAME health state machine, once.
        CommitOutcomeHealth(victim, submission->source, result.hpPrepared);
        gOutcomeApplied.fetch_add(1U, std::memory_order_relaxed);
        return definition;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed); return rejected;
    }
}
std::uintptr_t __cdecl PlayerPreparationBoundary(const std::uintptr_t victim,
    const std::uintptr_t sourceCycle) noexcept {
    if (gNativeVictimSubmission != nullptr && gNativeVictimSubmission->authoritativeClient)
        return ResolvedClientPreparation(victim, sourceCycle);
    if (!RemotePlayerPreparation(victim, sourceCycle)) return 0;
    gNativeVictimSubmission->playerPreparation = true;
    gPairedPlayerPreparationRemaps.fetch_add(1U, std::memory_order_relaxed);
    return 1;
}
int __cdecl PlayerCategoryBoundary(const std::uintptr_t victim) noexcept {
    const auto* submission = gNativeVictimSubmission;
    if (submission == nullptr || !submission->playerPreparation ||
        !RemotePlayerPreparation(victim, submission->sourceCycle)) return 0;
    gPairedPlayerCategoryRemaps.fetch_add(1U, std::memory_order_relaxed);
    return 1;
}
// 006626F7: preserve GPRs/EFLAGS and aligned FXSAVE state (x87/MMX/XMM/MXCSR).
// EDI holds the saved PUSHAD frame; saved ECX=+18h, ESI=+4, EAX=+1Ch.
// 1 takes the Host player branch;
// a definition pointer resumes at 00662837 with ONLY saved EAX replaced;
// 2 aborts at the retail epilogue after marking this exact cycle failed.
__declspec(naked) void PlayerPreparationShim() {
    __asm {
        pushfd
        pushad
        mov edi, esp
        sub esp, 210h
        and esp, 0FFFFFFF0h
        fxsave [esp]
        cld
        push dword ptr [edi + 4]
        push dword ptr [edi + 18h]
        call PlayerPreparationBoundary
        add esp, 8
        cmp eax, 1
        je player
        cmp eax, 2
        je abortPreparation
        test eax, eax
        jnz resolved
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gPlayerPreparationTrampoline]
    player:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gPlayerPreparationContinue]
    resolved:
        mov dword ptr [edi + 1Ch], eax
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gOutcomePreparationContinue]
    abortPreparation:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        mov dword ptr [esi + 44h], ebx
        or byte ptr [esi + 8], 6
        jmp dword ptr [gOutcomePreparationAbort]
    }
}
// Interior query: all GPRs, EFLAGS and eight SSE registers survive.
// No controller, health, source parameters, animation or target is written.
__declspec(naked) void PlayerCategoryShim() {
    __asm {
        pushfd
        pushad
        sub esp, 80h
        movdqu xmmword ptr [esp + 0h], xmm0
        movdqu xmmword ptr [esp + 10h], xmm1
        movdqu xmmword ptr [esp + 20h], xmm2
        movdqu xmmword ptr [esp + 30h], xmm3
        movdqu xmmword ptr [esp + 40h], xmm4
        movdqu xmmword ptr [esp + 50h], xmm5
        movdqu xmmword ptr [esp + 60h], xmm6
        movdqu xmmword ptr [esp + 70h], xmm7
        push esi
        call PlayerCategoryBoundary
        add esp, 4
        test eax, eax
        jnz player
        movdqu xmm0, xmmword ptr [esp + 0h]
        movdqu xmm1, xmmword ptr [esp + 10h]
        movdqu xmm2, xmmword ptr [esp + 20h]
        movdqu xmm3, xmmword ptr [esp + 30h]
        movdqu xmm4, xmmword ptr [esp + 40h]
        movdqu xmm5, xmmword ptr [esp + 50h]
        movdqu xmm6, xmmword ptr [esp + 60h]
        movdqu xmm7, xmmword ptr [esp + 70h]
        add esp, 80h
        popad
        popfd
        jmp dword ptr [gPlayerCategoryTrampoline]
    player:
        movdqu xmm0, xmmword ptr [esp + 0h]
        movdqu xmm1, xmmword ptr [esp + 10h]
        movdqu xmm2, xmmword ptr [esp + 20h]
        movdqu xmm3, xmmword ptr [esp + 30h]
        movdqu xmm4, xmmword ptr [esp + 40h]
        movdqu xmm5, xmmword ptr [esp + 50h]
        movdqu xmm6, xmmword ptr [esp + 60h]
        movdqu xmm7, xmmword ptr [esp + 70h]
        add esp, 80h
        popad
        popfd
        jmp dword ptr [gPlayerCategoryContinue]
    }
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
        if (gNativeVictimSubmission != nullptr && !gNativeVictimSubmission->authoritativeClient &&
            gNativeVictimSubmission->slot != kActors) {
            auto& result = gNativeVictimSubmission->outcome;
            result.flags |= OutcomeVictimAdmitted;
            std::uint8_t interactive{};
            if (result.kind != 0U && Read(cycle + 0xDAU, interactive) && interactive == 0U)
                result.flags |= OutcomeNativeDeath;
        }
        if (gNativeVictimSubmission != nullptr && gNativeVictimSubmission->authoritativeClient &&
            PairIdentityLive(gNativeVictimSubmission->victimBefore) &&
            (gNativeVictimSubmission->outcome.kind != 0U ||
             (gNativeVictimSubmission->outcome.flags & OutcomeTerminal) == 0U))
            CommitOutcomeHealth(victim, source, gNativeVictimSubmission->outcome.hpReaction);
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
        gNativeVictimSubmission->slot != kActors &&
        ManagedPairedVictim(victim, source) && ValidPairedVictimSource(victim, source, sourceCycle, state) &&
        crash_guard::TryCopy(gNativeVictimSubmission->packet.data(), packet,
            gNativeVictimSubmission->packet.size())) {
        gNativeVictimSubmission->victim = victim;
        gNativeVictimSubmission->captured = true;
        if (!gNativeVictimSubmission->authoritativeClient) {
            auto& outcome = gNativeVictimSubmission->outcome;
            std::uintptr_t definition{};
            std::uint16_t index{}, hp{};
            bool complete = Read(sourceCycle + 0x4CU, definition) && definition != 0U &&
                Read(definition + 8U, outcome.kind) && Read(sourceCycle + 0x108U, index) &&
                Read(sourceCycle + 0xB8U, outcome.sourceYaw) && Read(victim + 0x33CU, hp);
            outcome.definition = index; outcome.hpPrepared = hp;
            for (std::size_t i = 0U; i < 3U; ++i) {
                float selected{}, targetPosition{};
                complete = Read(sourceCycle + 0xD0U + i * 4U, selected) &&
                    Read(victim + kPositionOffset + i * 4U, targetPosition) && complete;
                outcome.targetOffset[i] = selected - targetPosition;
                complete = FiniteCoordinate(outcome.targetOffset[i]) && complete;
            }
            if (complete && outcome.hpBefore != 0U && outcome.hpPrepared <= outcome.hpBefore &&
                std::isfinite(outcome.sourceYaw) && std::abs(outcome.sourceYaw) <= 100000.0F)
                outcome.flags = OutcomePrepared;
        }
        // Preserve the already working real Host player's inline retail flow.
        // Only remote/Client victims need the existing delayed dispatch guard.
        if (gRole.load(std::memory_order_acquire) == Role::Host &&
            !RegisteredPlayerRepresentation(victim) && dispatch != nullptr) {
            gNativeVictimSubmission->victimDispatch = true;
            gNativeVictimSubmission->retailDispatched = true;
            reinterpret_cast<CharacterMessage>(dispatch)(character, packet);
        }
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
        !ManagedPairedVictim(victim, submission.source)) { original(context); return; }
    if (
        !PublishedPairParticipant(victim, submission.victimBefore) ||
        !Read(submission.sourceCycle + 0x19U, submission.sourceSerial) ||
        !VictimHealthAllowsAdmission(victim) ||
        !BeginVictimAdmission(victim, submission.source, submission.sourceCycle, slot)) {
        // A duplicate/busy/dead admission must not re-run preparation damage.
        gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const bool hasCarrier = ClaimOutcome(submission, slot);
    if (!hasCarrier && (submission.duplicateOutcome ||
        RegisteredPlayerRepresentation(victim) || LocalClientVictim(victim))) {
        gPairSourceBusy[slot].store(false, std::memory_order_release);
        gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed); return;
    }
    std::uint16_t hpBefore{};
    if (!submission.authoritativeClient && Read(victim + 0x33CU, hpBefore))
        submission.outcome.hpBefore = hpBefore;
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
            submission.victimDispatch = true;
            if (!submission.retailDispatched) {
                gPairedVictimNativeDispatches.fetch_add(1U, std::memory_order_relaxed);
                if (!EngineThread()) gPairedVictimWorkerDispatches.fetch_add(1U, std::memory_order_relaxed);
                if (LocalClientVictim(submission.victim))
                    gPairedVictimLocalDispatches.fetch_add(1U, std::memory_order_relaxed);
                PairedVictimEventDetour(submission.packet.data(), nullptr, reinterpret_cast<void*>(submission.victim));
            }
            if (!submission.authoritativeClient) {
                ObserveVictimOutcome(submission.victim, submission.source, submission.sourceSerial, submission.outcome);
                if (submission.nativeFinish) submission.outcome.flags |= OutcomeNativeDeath;
                RecordOutcome(submission);
            }
        }
    } __finally {
        gNativeVictimSubmission = previous;
        gVictimAdmissionSource = previousSource; gVictimAdmissionTarget = previousTarget;
        gVictimAdmissionCycle = previousCycle; gVictimAdmissionDepth = previousDepth;
        gPairSourceBusy[slot].store(false, std::memory_order_release);
    }
}
int __cdecl VictimFinishBoundary(const std::uintptr_t cycle) noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    auto* submission = gNativeVictimSubmission;
    if (role == Role::Host) return 0; // all Host predicates/effects stay retail
    if (role != Role::Client) return 0;
    std::uintptr_t victim{}, source{};
    if (!Read(cycle + 4U, victim) || !Read(cycle + 0x68U, source) ||
        !ManagedPairedVictim(victim, source)) return 0;
    // Skip the WHOLE late-kill block, including control/state transitions.
    // Missing authority cannot authorize a local Character+29B conversion.
    if (submission == nullptr || !submission->authoritativeClient || submission->source != source ||
        submission->victimBefore.character != victim || !PairIdentityLive(submission->victimBefore)) return 1;
    if (submission->outcome.kind != 0U) return 0;
    if ((submission->outcome.flags & (OutcomeNativeDeath | OutcomeTerminal)) !=
        (OutcomeNativeDeath | OutcomeTerminal)) return 1;
    PairIdentity identity{};
    const auto slot = PublishedPairSource(source, identity);
    if (slot == kActors) return 1;
    std::lock_guard lock(gOutcomeMutex);
    auto& binding = gOutcomes[slot];
    if (binding.source.runtimeId != identity.runtimeId || binding.source.epoch != identity.epoch ||
        binding.victim.runtimeId != submission->victimBefore.runtimeId ||
        binding.state.interactionPairRevision != submission->pairRevision || binding.lateFinishClaimed) return 1;
    // HP may already be terminal; the native control/state/finish block is
    // nevertheless required. Claim the transition, not merely an HP change.
    binding.lateFinishClaimed = true;
    return 2;
}
// CALL 00655B88 is reached only AFTER this block's kill and state=2 calls.
// 00649FA0: ECX=4F cycle, no stack arguments, plain RET (0064A078).
void __fastcall VictimFinishExecutedDetour(void* context, void*) noexcept {
    reinterpret_cast<PairedVictimInitialize>(gBase + 0x00249FA0U)(context);
    auto* submission = gNativeVictimSubmission;
    std::uintptr_t victim{};
    if (submission != nullptr && Read(reinterpret_cast<std::uintptr_t>(context) + 4U, victim) &&
        victim == submission->victimBefore.character && PairIdentityLive(submission->victimBefore))
        submission->nativeFinish = true;
}
__declspec(naked) void VictimFinishShim() {
    __asm {
        pushfd
        pushad
        mov edi, esp
        sub esp, 210h
        and esp, 0FFFFFFF0h
        fxsave [esp]
        cld
        push dword ptr [edi + 4]
        call VictimFinishBoundary
        add esp, 4
        cmp eax, 1
        je skipKill
        cmp eax, 2
        je kill
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gVictimFinishTrampoline]
    skipKill:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gVictimFinishSkip]
    kill:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        mov eax, dword ptr [esi + 4]
        jmp dword ptr [gVictimFinishKill]
    }
}
void __fastcall VictimOutcomeUpdateDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<PairedVictimInitialize>(gVictimUpdateOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    std::uintptr_t victim{}, source{}, owned{};
    std::uintptr_t linked{}, current{}, table{};
    std::uint32_t cachedBirth{};
    std::uint8_t serial{};
    OutcomeBinding binding{};
    const bool managed = Read(cycle + 4U, victim) && Read(cycle + 0x68U, source) &&
        Read(victim + 0x36CU, owned) && owned == cycle && LoadOutcome(source, victim, binding) &&
        Read(cycle, table) && table == gBase + 0x00EAE690U &&
        Read(cycle + 0x6CU, cachedBirth) && cachedBirth == binding.source.runtimeId &&
        Read(cycle + 0xB0U, linked) && Read(source + 0x36CU, current) && linked == current &&
        Read(linked + 0x19U, serial) && serial == binding.serial &&
        (binding.state.outcome.flags & OutcomePrepared) != 0U;
    NativeVictimSubmission submission{};
    submission.source = source; submission.victimBefore = binding.victim;
    submission.outcome = binding.state.outcome;
    submission.pairRevision = binding.state.interactionPairRevision;
    submission.authoritativeClient = managed && gRole.load(std::memory_order_acquire) == Role::Client;
    submission.victimDispatch = true;
    auto* previous = gNativeVictimSubmission;
    if (managed) gNativeVictimSubmission = &submission;
    __try { original(context); }
    __finally { gNativeVictimSubmission = previous; }
    if (submission.authoritativeClient && submission.nativeFinish && PairIdentityLive(submission.victimBefore))
        CommitOutcomeHealth(victim, source, submission.outcome.hpReaction);
    if (!managed || gRole.load(std::memory_order_acquire) != Role::Host ||
        !PairIdentityLive(binding.source) || !PairIdentityLive(binding.victim)) return;
    auto result = binding.state.outcome;
    ObserveVictimOutcome(victim, source, binding.serial, result);
    if (submission.nativeFinish) result.flags |= OutcomeNativeDeath;
    if (Read(victim + 0x36CU, owned) && owned != cycle) result.flags |= OutcomeVictimEnded;
    if (std::memcmp(&result, &binding.state.outcome, sizeof(result)) == 0) return;
    NativeVictimSubmission observation{};
    PairIdentity sourceIdentity{};
    observation.slot = PublishedPairSource(source, sourceIdentity);
    observation.pairRevision = binding.state.interactionPairRevision;
    observation.sourceSerial = binding.serial; observation.victimBefore = binding.victim;
    observation.outcome = result;
    RecordOutcome(observation);
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
void TracePairedGraph(const std::uintptr_t source, const std::uintptr_t victim,
    const std::uint32_t event, const std::uint32_t side, const std::uint32_t phase) noexcept {
    PairIdentity identity{};
    if (PublishedPairSource(source, identity) == kActors || !PairIdentityLive(identity)) return;
    PairedGraphProbe sample{};
    sample.tick = GetTickCount64(); sample.event = event; sample.side = side; sample.phase = phase;
    sample.role = static_cast<std::uint32_t>(gRole.load(std::memory_order_acquire));
    sample.generation = gActiveWorldGeneration.load(std::memory_order_acquire);
    sample.sourceA = identity.entityA; sample.sourceB = identity.entityB;
    std::uint32_t victimType{};
    static_cast<void>(CharacterIdentity(victim, sample.victimA, sample.victimB, victimType));
    OutcomeBinding binding{};
    if (LoadOutcome(source, victim, binding)) {
        sample.peer = binding.state.interactionTargetPeer;
        sample.pairRevision = binding.state.interactionPairRevision; sample.birth = binding.state.birth;
    }
    std::uintptr_t sourceCycle{}, victimCycle{}, owner{};
    sample.readable = Read(source + 0x36CU, sourceCycle) && sourceCycle != 0U &&
        Read(sourceCycle + 4U, owner) && owner == source &&
        Read(sourceCycle + 0x1AU, sample.sourceState);
    if (sample.readable && sample.sourceState == 0x4EU) {
        sample.readable = Read(sourceCycle + 0xE0U, sample.sourceStage) &&
            Read(sourceCycle + 0xE6U, sample.sourceFinished);
    }
    if (victim != 0U && Read(victim + 0x36CU, victimCycle) && victimCycle != 0U &&
        Read(victimCycle + 4U, owner) && owner == victim &&
        Read(victimCycle + 0x1AU, sample.victimState) && sample.victimState == 0x4FU) {
        sample.readable = sample.readable && Read(victimCycle + 0xD8U, sample.victimEnded) &&
            Read(victimCycle + 0xD9U, sample.victimAcquired);
    }
    std::unique_lock lock(gPairedGraphMutex, std::try_to_lock);
    if (!lock.owns_lock() || gPairedGraphWrite - gPairedGraphRead >= kParticipantProbeRing) {
        gPairedGraphDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    gPairedGraphProbes[gPairedGraphWrite++ % kParticipantProbeRing] = sample;
}
void __fastcall SourceGraphEventDetour(void* context, void*, const std::uint32_t event) noexcept {
    using Event = void(__thiscall*)(void*, std::uint32_t);
    const auto original = reinterpret_cast<Event>(gSourceGraphEventOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{}, victim{};
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    if (Read(cycle + 4U, source) && Read(cycle + 0x44U, victim))
        TracePairedGraph(source, victim, event, 1U, 0U);
    original(context, event);
    TracePairedGraph(source, victim, event, 1U, 1U);
}
void __fastcall VictimGraphEventDetour(void* context, void*, const std::uint32_t event) noexcept {
    using Event = void(__thiscall*)(void*, std::uint32_t);
    const auto original = reinterpret_cast<Event>(gVictimGraphEventOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{}, victim{};
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    if (Read(cycle + 4U, victim) && Read(cycle + 0x68U, source))
        TracePairedGraph(source, victim, event, 2U, 0U);
    original(context, event);
    TracePairedGraph(source, victim, event, 2U, 1U);
}
void __fastcall SourceGraphFinishDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(gSourceGraphFinishOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t source{}, victim{};
    const auto cycle = reinterpret_cast<std::uintptr_t>(context);
    if (Read(cycle + 4U, source) && Read(cycle + 0x44U, victim))
        TracePairedGraph(source, victim, 0U, 3U, 0U);
    original(context);
    TracePairedGraph(source, victim, 0U, 3U, 1U);
}
void SamplePresentation(Actor& actor, const ULONGLONG now, const std::uint32_t phase = 0U) noexcept {
    if (!EngineThread() || !Stable() || actor.character == 0U || actor.duplicate) return;
    if (phase == 0U && now - actor.lastPresentationProbe < kParticipantProbePeriod) return;
    actor.lastPresentationProbe = now;
    PresentationProbe sample{};
    const auto& state = actor.desired;
    sample.sessionCookie = gCurrent.sessionCookie; sample.tick = now;
    sample.nativeGeneration = gCurrent.nativeGeneration; sample.worldGeneration = gCurrent.worldGeneration;
    sample.role = static_cast<std::uint32_t>(gRole.load(std::memory_order_acquire)); sample.phase = phase;
    sample.entityA = actor.entityA; sample.entityB = actor.entityB;
    sample.sourceCharacter = actor.character; sample.sourceRuntimeId = actor.runtimeId;
    sample.birth = Client() ? actor.remoteBirth : actor.birth; sample.revision = state.revision;
    sample.pairRevision = state.interactionPairRevision; sample.peer = state.interactionTargetPeer;
    sample.outcome = state.outcome;
    sample.interactionFlags = state.interactionFlags;
    sample.lifecycleFlags = state.lifecycleFlags; sample.ownsPair = actor.interactionOwnsCycle;
    sample.observerReleasedPairRevision = actor.observerReleasedPairRevision;
    sample.navigation = state.navigation;
    sample.movementOwner = static_cast<std::uint32_t>(actor.movementOwner);
    sample.observerHandoffNeutral = actor.observerHandoffNeutral;
    sample.placementPending = actor.placementPending;
    sample.pairPlacementPending = actor.pairPlacementPending;
    sample.positionReadable = true;
    for (std::size_t i = 0U; i < 3U; ++i) {
        const bool readable = Read(actor.character + kPositionOffset + i * sizeof(float), sample.local[i]) &&
            FiniteCoordinate(sample.local[i]);
        sample.positionReadable = sample.positionReadable && readable;
        sample.authority[i] = state.position[i]; sample.authorityVictim[i] = state.interactionTargetPosition[i];
    }
    sample.visibilityReadable = Read(actor.character + 0x2DAU, sample.hidden) &&
        Read(actor.character + 0x2FAU, sample.enabled);
    std::uintptr_t cycle{}, table{};
    sample.cycleReadable = Read(actor.character + 0x36CU, cycle) && cycle != 0U &&
        Read(cycle + 0x1AU, sample.cycleState) && Read(cycle, table) && table >= gBase;
    if (sample.cycleReadable) {
        sample.cycleType = static_cast<std::uint32_t>(table - gBase);
        if (sample.cycleState == 0x4EU && table == gBase + 0x00EAE5D0U)
            static_cast<void>(Read(cycle + 0xE0U, sample.stage));
    }
    std::uintptr_t withdraw{};
    static_cast<void>(ResolveAlienWithdrawContext(actor.character, withdraw, sample.withdrawState));
    const auto target = (state.interactionFlags & InteractionTargetIsParticipant) != 0U
        ? ParticipantForPeer(state.interactionTargetPeer)
        : FindCharacterByIdentity(state.interactionTargetA, state.interactionTargetB);
    sample.victimReadable = target != 0U;
    sample.victimCharacter = target; sample.localCharacter = LocalPlayer();
    sample.victimLocal = target != 0U && target == sample.localCharacter;
    sample.victimRepresentation = RegisteredPlayerRepresentation(target);
    static_cast<void>(Read(target + 0x50U, sample.victimRuntimeId));
    static_cast<void>(Read(target + 0x54U, sample.victimEntityA));
    static_cast<void>(Read(target + 0x58U, sample.victimEntityB));
    static_cast<void>(Read(sample.localCharacter + 0x50U, sample.localRuntimeId));
    std::uint16_t localHp{};
    sample.localHealthReadable = sample.localCharacter != 0U &&
        Read(sample.localCharacter + 0x33CU, localHp) &&
        Read(sample.localCharacter + 0x32CU, sample.localHealthState);
    sample.localHp = localHp;
    std::uintptr_t victimCycle{}, victimTable{};
    if (target != 0U && Read(target + 0x36CU, victimCycle) && victimCycle != 0U &&
        Read(victimCycle, victimTable) && victimTable >= gBase &&
        Read(victimCycle + 0x1AU, sample.victimCycleState)) {
        sample.victimCycleType = static_cast<std::uint32_t>(victimTable - gBase);
        if (victimTable == gBase + 0x00EAE690U && sample.victimCycleState == 0x4FU) {
            static_cast<void>(Read(victimCycle + 0xD8U, sample.victimEnded));
            static_cast<void>(Read(victimCycle + 0xD9U, sample.victimAcquired));
            static_cast<void>(Read(victimCycle + 0xDAU, sample.victimInteractive));
        }
    }
    sample.victimHealthReadable = target != 0U && Read(target + 0x33CU, sample.victimHp) &&
        Read(target + 0x32CU, sample.victimHealthState);
    for (std::size_t i = 0U; i < 3U; ++i) {
        const bool readable = target != 0U && Read(target + kPositionOffset + i * sizeof(float), sample.victim[i]) &&
            FiniteCoordinate(sample.victim[i]);
        sample.victimReadable = sample.victimReadable && readable;
    }
    const auto write = gPresentationProbeWrite.load(std::memory_order_relaxed);
    if (write - gPresentationProbeRead.load(std::memory_order_acquire) >= kParticipantProbeRing) {
        gPresentationDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    gPresentationProbes[write % kParticipantProbeRing] = sample;
    gPresentationProbeWrite.store(write + 1U, std::memory_order_release);
    gPresentationSamples.fetch_add(1U, std::memory_order_relaxed);
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
    // Flush the prior pair's immutable result before replacing its identity.
    // Preparation callbacks have released their source lease by this boundary.
    CaptureOutcome(*actor);
    if (actor->desired.outcome.revision != 0U) {
        std::unique_lock lock(gOutcomeMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            auto& binding = gOutcomes[static_cast<std::size_t>(actor - gActors.data())];
            if ((binding.state.outcome.flags & OutcomeSourceEnded) == 0U) {
                binding.state.outcome.flags |= OutcomeSourceEnded;
                ++binding.state.outcome.revision;
            }
            actor->desired.outcome = binding.state.outcome;
            actor->desired.interactionFlags &= ~(InteractionCycleActive | InteractionSourceCycleRetained);
        }
        actor->lastSend = 0U;
    }
    Publish(*actor);
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
    state.outcome = {};
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
    // Publish the pair before a transition can synchronously prepare it.
    if (participant) static_cast<void>(PublishOutcomeBinding(*actor, target));
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
    state.interactionFlags |= InteractionCycleActive | InteractionSourceCycleRetained;
    std::uint8_t nativeCycleState{};
    std::uintptr_t nativeCycleTable{};
    if (state.interactionTargetPeer != 0U && Read(manager + 0x1AU, nativeCycleState) && nativeCycleState == 0x4EU &&
        Read(manager, nativeCycleTable) && nativeCycleTable == gBase + 0x00EAE5D0U)
        state.interactionFlags |= InteractionNativeOutcomeRequired;
    state.interactionTransitionFlags = (firstFlag & 0xFFU) | ((secondFlag & 0xFFU) << 8U);
    actor->lastInteractionCapture = now;
    static_cast<void>(PublishOutcomeBinding(*actor, target));
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
    navigation::Cancel(actor.character);
    actor.movementOwner = MovementOwner::None;
    actor.replayedExecutionRevision = 0U;
    actor.lastExecutionAttempt = 0U;
    actor.poseCount = 0U; actor.poseRevision = 0U; actor.poseInterpolating = false;
    if (actor.character != 0U && Client()) {
        static_cast<void>(character_direct_step_runtime::Stop(actor.character));
        // A Client role/task can outlive the BT leaf that created it. Always
        // retire that executor through the retail cleanup before authority is
        // released, otherwise an old task can keep fighting future PoseSync.
        if (ClearRoleTaskExecutor(actor)) actor.roleTaskCleanupPending = false;
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
void ClearPairPlacement(Actor& actor, const bool cancelled = true) noexcept {
    if (actor.pairPlacementPending && cancelled)
        gPairedSourcePlacementCancelled.fetch_add(1U, std::memory_order_relaxed);
    actor.pairPlacementPending = false;
    actor.pairPlacementSince = 0U; actor.pairPlacementTarget = 0U;
    actor.pairPlacementBirth = 0U; actor.pairPlacementPair = 0U;
    actor.pairPlacementTransition = 0U; actor.pairPlacementTargetBirth = 0U;
}
bool PairPlacementMatchesState(const Actor& actor, const State& state) noexcept {
    return actor.pairPlacementPending && actor.pairPlacementBirth == state.birth &&
        actor.pairPlacementPair == state.interactionPairRevision &&
        actor.pairPlacementTransition == state.interactionTransitionRevision &&
        (state.stateFlags & (PoseValid | InteractionValid)) == (PoseValid | InteractionValid) &&
        state.interactionTransition != 0U &&
        (state.interactionFlags & InteractionCycleActive) != 0U && !ProxyWithdrawn(state);
}
bool PlaceReplicatedPairSource(Actor& actor, const std::uintptr_t target, const ULONGLONG now) noexcept {
    const auto& state = actor.desired;
    float placement[3]{};
    // Translate the authoritative source/victim offset into this process's
    // exact victim frame. A delayed follower or a remote-player correction
    // must not start a native kill metres away. Never move the real player or
    // choose another target. Retail owns all following paired root motion.
    std::uint32_t targetBirth{};
    if ((state.stateFlags & PoseValid) == 0U ||
        !Read(target + 0x50U, targetBirth) || targetBirth == 0U) {
        ClearPairPlacement(actor); return false;
    }
    if (actor.pairPlacementPending &&
        (!PairPlacementMatchesState(actor, state) || actor.pairPlacementTarget != target ||
         actor.pairPlacementTargetBirth != targetBirth)) ClearPairPlacement(actor);
    if (actor.pairPlacementPending && now - actor.pairPlacementSince > kPairPlacementReadyTimeout) {
        gPairedSourcePlacementExpired.fetch_add(1U, std::memory_order_relaxed);
        ClearPairPlacement(actor); return false;
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        float localVictim{};
        if (!Read(target + kPositionOffset + i * sizeof(float), localVictim) ||
            !FiniteCoordinate(localVictim)) {
            ClearPairPlacement(actor); return false;
        }
        placement[i] = state.position[i] + localVictim - state.interactionTargetPosition[i];
        if (!FiniteCoordinate(placement[i])) {
            ClearPairPlacement(actor); return false;
        }
    }
    if (!PlaceCharacterPose(actor.character, placement, state.yaw)) {
        ClearPairPlacement(actor); return false;
    }
    // FUN_007C6330 deliberately clears Character+2FA during relocation. This
    // is preparation, not rejection. Keep the follower OFF, then revisit this
    // exact pair on the next engine update, without the ordinary 250ms backoff.
    // Re-read the actual victim frame above; never move/freeze the human player.
    if (!CharacterReady(actor)) {
        if (!actor.pairPlacementPending) actor.pairPlacementSince = now;
        actor.pairPlacementPending = true;
        actor.pairPlacementTarget = target; actor.pairPlacementTargetBirth = targetBirth;
        actor.pairPlacementBirth = state.birth; actor.pairPlacementPair = state.interactionPairRevision;
        actor.pairPlacementTransition = state.interactionTransitionRevision;
        return false;
    }
    if (actor.pairPlacementPending)
        gPairedSourcePlacementResumed.fetch_add(1U, std::memory_order_relaxed);
    ClearPairPlacement(actor, false);
    actor.lastProxyRefresh = 0U; actor.completedRevision = 0U;
    gPairedSourcePlacements.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
bool ApplyAuthoritativeInteraction(Actor& actor, const ULONGLONG now) noexcept {
    if (PairSourceBusy(actor.character)) return true;
    const auto& state = actor.desired;
    if (actor.observerReleasedBirth != 0U && !ObserverReleasedPairMatches(actor, state)) {
        const bool wasNeutral = actor.observerHandoffNeutral;
        ClearObserverReleaseMemory(actor);
        if (wasNeutral) {
            gPairedObserverHandoffResumed.fetch_add(1U, std::memory_order_relaxed);
            SamplePresentation(actor, now, 6U);
        }
    }
    if (ObserverReleasedPairMatches(actor, state)) {
        // Same-pair progress/outro packets are still authoritative metadata, but
        // this observer has already completed the remote 4F and retired its 4E.
        // Never reopen the old presentation because a delayed snapshot carries
        // a newer interaction revision or repeats SourceCycleRetained.
        if (state.interactionRevision > actor.completedInteractionRevision)
            actor.completedInteractionRevision = state.interactionRevision;
        if (state.interactionTransitionRevision > actor.completedTransitionRevision)
            actor.completedTransitionRevision = state.interactionTransitionRevision;
        gPairedObserverPairReopenSuppressed.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    if (actor.pairPlacementPending && !PairPlacementMatchesState(actor, state))
        ClearPairPlacement(actor);
    if (actor.pairPlacementPending && now - actor.pairPlacementSince > kPairPlacementReadyTimeout) {
        gPairedSourcePlacementExpired.fetch_add(1U, std::memory_order_relaxed);
        ClearPairPlacement(actor); actor.lastInteractionAttempt = now;
        return true; // bounded wait even if target binding becomes unavailable
    }
    if ((state.stateFlags & InteractionValid) == 0U ||
        actor.completedInteractionRevision == state.interactionRevision) {
        ClearPairPlacement(actor); return true;
    }
    // A pair-only packet lacks the source definition needed to bind its bone.
    // Retain it until the following resolved transition arrives; no fabrication.
    if (state.interactionTransition == 0U) return true;
    const bool participant = (state.interactionFlags & InteractionTargetIsParticipant) != 0U;
    if ((state.interactionFlags & InteractionNativeOutcomeRequired) != 0U &&
        (state.outcome.flags & OutcomePrepared) == 0U) return true;
    // A completed Host attack is an acknowledgement, not an instruction to
    // start a stale kill when its last snapshot finally reaches this process.
    if (state.interactionTransition != 0U &&
        (state.interactionFlags & InteractionCycleActive) == 0U && state.outcome.revision == 0U) {
        actor.completedInteractionRevision = state.interactionRevision;
        return true;
    }
    // Prepared placement needs the next retail engine frame, not another
    // quarter-second relocation/follower loop. Other failures keep their backoff.
    if (!actor.pairPlacementPending && actor.lastInteractionAttempt != 0U &&
        now - actor.lastInteractionAttempt < 250ULL) return true;
    actor.lastInteractionAttempt = now;
    const auto target = participant ? ParticipantForPeer(state.interactionTargetPeer)
        : FindCharacterByIdentity(state.interactionTargetA, state.interactionTargetB);
    std::uint32_t targetA{}, targetB{}, targetType{};
    if (target == 0U || target == actor.character ||
        !CharacterIdentity(target, targetA, targetB, targetType)) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return true; // keep the event until the exact participant is bound
    }
    if (participant && !PublishOutcomeBinding(actor, target)) return true;
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
            navigation::Cancel(actor.character);
            actor.movementOwner = MovementOwner::Paired;
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
            SamplePresentation(actor, now, 1U);
            actor.poseCount = 0U; actor.poseRevision = 0U; actor.poseInterpolating = false;
            // Release only locomotion before pair admission. A native paired
            // message can immediately acquire the victim's physical cycle.
            if (!character_direct_step_runtime::Stop(actor.character) ||
                (RegisteredPlayerRepresentation(target) && !character_direct_step_runtime::Stop(target))) {
                ClearPairPlacement(actor);
                gPairedSourcePlacementRejected.fetch_add(1U, std::memory_order_relaxed);
                return true;
            }
            if (!PlaceReplicatedPairSource(actor, target, now)) {
                if (actor.pairPlacementPending)
                    gPairedSourcePlacementDeferred.fetch_add(1U, std::memory_order_relaxed);
                else gPairedSourcePlacementRejected.fetch_add(1U, std::memory_order_relaxed);
                return true; // exact event retained; no native pair acknowledged yet
            }
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
    SamplePresentation(actor, now, 2U);
    gInteractionApplied.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
bool PairedTransitionOwnsCycle(Actor& actor, const ULONGLONG now) noexcept {
    // Do not reuse the source layout while a retail worker admits its victim.
    if (PairSourceBusy(actor.character)) return true;
    if (!actor.interactionOwnsCycle) return false;
    // A replacement event is admitted below after the old local victim lease
    // has ended. Its new Host cycle cannot extend ownership of our old pair.
    if (actor.completedPairRevision != actor.desired.interactionPairRevision) return false;

    bool remoteObserverFatalComplete{};
    const auto target = ParticipantForPeer(actor.desired.interactionTargetPeer);
    if (actor.desired.outcome.revision != 0U) {
        OutcomeBinding binding{};
        if (!LoadOutcome(actor.character, target, binding) || !binding.claimed) return true;
        const auto fatalFlags = OutcomeVictimAdmitted | OutcomeNativeDeath | OutcomeTerminal;
        const bool fatalKind0 = binding.state.outcome.kind == 0U &&
            (binding.state.outcome.flags & (OutcomeNativeDeath | OutcomeTerminal)) ==
                (OutcomeNativeDeath | OutcomeTerminal);
        if (fatalKind0 && !binding.lateFinishClaimed) return true;

        // kind=0 has its certified late control/finish block and therefore keeps
        // the existing lateFinishClaimed contract. kind=1 follows a different
        // native path: for a remote observer, completion is proven by the exact
        // local 4F graph ending plus terminal authoritative consequences.
        if (fatalKind0) {
            remoteObserverFatalComplete = binding.lateFinishClaimed && target != 0U &&
                !LocalClientVictim(target) && TerminalDeadCharacter(target);
        } else if (binding.state.outcome.kind == 1U &&
            (binding.state.outcome.flags & fatalFlags) == fatalFlags && target != 0U &&
            !LocalClientVictim(target) && TerminalDeadCharacter(target)) {
            const auto victimState = ReadRemoteObserverVictimState(actor, target, binding);
            if (victimState == RemoteObserverVictimState::Complete) {
                std::uintptr_t cycle{};
                std::uint8_t nativeState{};
                bool localActive{};
                if (ReadPairedCycleActivity(actor.character, cycle, nativeState, localActive) &&
                    nativeState == 0x4EU && !localActive) {
                    remoteObserverFatalComplete = true;
                    gPairedObserverCompletions.fetch_add(1U, std::memory_order_relaxed);
                    SamplePresentation(actor, now, 4U);
                }
            }
        }
    }
    if (ClientNativeVictimOwnsPair(actor.character)) return true;
    const bool withdrawn = ProxyWithdrawn(actor.desired);
    if (!withdrawn && (actor.desired.interactionFlags & InteractionSourceCycleRetained) != 0U &&
        !remoteObserverFatalComplete) {
        // While the local victim graph still matters, keep the attacker through
        // the Host's real 4E outro. A completed remote observer graph is allowed
        // to release so the next authoritative execution can take over.
        gPairedHostCycleWaits.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    // BT revisions and a paired type alone do not prove an animation is still
    // running. A completed/failed local cycle must not hold presentation forever
    // while waiting for the next authoritative completion snapshot.
    std::uintptr_t cycle{};
    std::uint8_t state{};
    bool localActive{};
    if (!withdrawn &&
        (actor.desired.interactionFlags & InteractionCycleActive) != 0U &&
        (!ReadPairedCycleActivity(actor.character, cycle, state, localActive) || localActive)) return true;

    const bool observerRelease = remoteObserverFatalComplete;
    if (observerRelease) {
        gPairedObserverReleaseAttempts.fetch_add(1U, std::memory_order_relaxed);
    }
    if (!character_direct_step_runtime::FinishReplicatedInteraction(actor.character)) {
        if (observerRelease)
            gPairedObserverReleaseRejected.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    actor.interactionOwnsCycle = false;
    gPairedCycleReleased.fetch_add(1U, std::memory_order_relaxed);
    if (observerRelease) {
        // Remember the exact released pair so repeated Host outro snapshots can
        // update metadata without reacquiring the old local 4E. Until a newer
        // decision epoch arrives, stay in the retail Idle reached above instead
        // of replaying a locomotion request captured before/during the kill.
        actor.observerReleasedBirth = actor.desired.birth;
        actor.observerReleasedPairRevision = actor.desired.interactionPairRevision;
        actor.observerReleasedInteractionRevision = actor.desired.interactionRevision;
        actor.observerReleasedActionRevision = actor.desired.actionRevision;
        actor.observerReleasedExecutionRevision = actor.desired.executionRevision;
        actor.observerReleasedRoleTaskRevision = actor.desired.roleTaskRevision;
        actor.observerReleasedLifecycleRevision = actor.desired.lifecycleRevision;
        actor.observerReleasedLocomotionRevision = actor.desired.locomotionRevision;
        actor.observerReleasedNavigationRevision = actor.desired.navigation.revision;
        actor.observerReleasedNavigationRequestRevision = actor.desired.navigation.requestRevision;
        actor.observerReleasedTargetPeer = actor.desired.interactionTargetPeer;
        actor.observerHandoffNeutral = true;
        actor.movementOwner = MovementOwner::Neutral;
        gPairedObserverReleases.fetch_add(1U, std::memory_order_relaxed);
        SamplePresentation(actor, now, 5U);
    }
    actor.lastProxyRefresh = 0U;
    actor.completedRevision = 0U;
    actor.poseCount = 0U; actor.poseRevision = 0U; actor.poseInterpolating = false;
    return false;
}

void CaptureInteractionCycle(Actor& actor) noexcept {
    auto& state = actor.desired;
    if ((state.stateFlags & InteractionValid) == 0U || state.interactionTransition == 0U) return;
    std::uintptr_t cycle{};
    std::uint8_t nativeState{};
    bool running{};
    const bool matching = PairedCycleMatches(actor.character, state) &&
        ReadPairedCycleActivity(actor.character, cycle, nativeState, running);
    const bool active = matching && running;
    std::uintptr_t target{};
    std::uint32_t targetBirth{}, currentBirth{};
    std::uint8_t flags{};
    std::uint32_t stage{};
    // 00663E00 can clear +44 in stage 6 without setting completion flags.
    // Preserve that proven admitted pair's source outro too. A failed typed
    // initializer has no admitted result and must not create such a lease.
    const bool admittedOutro = matching && nativeState == 0x4EU &&
        (state.outcome.flags & OutcomeVictimAdmitted) != 0U && Read(cycle + 0xE0U, stage) && stage == 6U;
    const bool retained = active || (matching && Read(cycle + 8U, flags) && (flags & 6U) == 0U &&
        (admittedOutro || (Read(cycle + 0x44U, target) && target != 0U && Read(cycle + 0x48U, targetBirth) &&
         Read(target + 0x50U, currentBirth) && targetBirth == currentBirth)));
    if (retained) state.interactionFlags |= InteractionSourceCycleRetained;
    else state.interactionFlags &= ~InteractionSourceCycleRetained;
    if (active) state.interactionFlags |= InteractionCycleActive;
    else state.interactionFlags &= ~InteractionCycleActive;
    if (!active && state.outcome.revision != 0U && (state.outcome.flags & OutcomeSourceEnded) == 0U) {
        std::unique_lock lock(gOutcomeMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        auto& binding = gOutcomes[static_cast<std::size_t>(&actor - gActors.data())];
        if (binding.state.interactionPairRevision != state.interactionPairRevision) return;
        binding.state.outcome.flags |= OutcomeSourceEnded;
        ++binding.state.outcome.revision;
        state.outcome = binding.state.outcome;
    }
}
bool ReadTargetSelectionSnapshot(const Actor& actor, TargetSelectionSnapshot& snapshot) noexcept {
    snapshot = {};
    std::uintptr_t selection{};
    if (!Read(actor.character + 0x34A8U, selection)) return false;
    snapshot.selectionReadable = true;
    if (selection == 0U) {
        snapshot.selectedReadable = true;
        snapshot.pendingReadable = true;
    } else {
        if (Read(selection + 0x10U, snapshot.selectedRecord)) {
            snapshot.selectedReadable = snapshot.selectedRecord == 0U;
            if (snapshot.selectedRecord != 0U) {
                std::uintptr_t recordOwner{}, target{};
                std::uint32_t recordBirth{}, currentBirth{};
                if (Read(snapshot.selectedRecord + 8U, recordOwner) && recordOwner == actor.character &&
                    Read(snapshot.selectedRecord + 0x42CU, target) && target != 0U &&
                    Read(snapshot.selectedRecord + 0x430U, recordBirth) &&
                    Read(target + 0x50U, currentBirth) && recordBirth == currentBirth) {
                    snapshot.selectedReadable = true;
                    snapshot.selectedPresent = true;
                    snapshot.selectedTarget = target;
                    snapshot.selectedBirth = currentBirth;
                    snapshot.selectedPeer = PeerForParticipant(target);
                    snapshot.selectedLiving = LivingCharacter(target);
                    snapshot.selectedTerminalDead = TerminalDeadCharacter(target);
                }
            }
        }
        if (Read(selection + 0x14U, snapshot.pendingRecord)) {
            snapshot.pendingReadable = snapshot.pendingRecord == 0U;
            if (snapshot.pendingRecord != 0U) {
                std::uintptr_t recordOwner{}, target{};
                std::uint32_t recordBirth{}, currentBirth{};
                if (Read(snapshot.pendingRecord + 8U, recordOwner) && recordOwner == actor.character &&
                    Read(snapshot.pendingRecord + 0x42CU, target) && target != 0U &&
                    Read(snapshot.pendingRecord + 0x430U, recordBirth) &&
                    Read(target + 0x50U, currentBirth) && recordBirth == currentBirth) {
                    snapshot.pendingReadable = true;
                    snapshot.pendingPresent = true;
                    snapshot.pendingTarget = target;
                    snapshot.pendingBirth = currentBirth;
                    snapshot.pendingPeer = PeerForParticipant(target);
                    snapshot.pendingLiving = LivingCharacter(target);
                    snapshot.pendingTerminalDead = TerminalDeadCharacter(target);
                }
            }
        }
    }
    std::uintptr_t haveTarget{}, haveNextTarget{};
    snapshot.targetSignalsReadable = Read(actor.character + 0x3154U, haveTarget) &&
        Read(actor.character + 0x31E8U, haveNextTarget) &&
        (haveTarget == 0U || Read(haveTarget + 0x10U, snapshot.haveTargetSignal)) &&
        (haveNextTarget == 0U || Read(haveNextTarget + 0x10U, snapshot.haveNextTargetSignal));
    return snapshot.selectionReadable;
}
bool TargetSelectionCertain(const TargetSelectionSnapshot& snapshot) noexcept {
    return snapshot.selectionReadable && snapshot.selectedReadable && snapshot.pendingReadable;
}
bool HasLivingSuccessor(const TargetSelectionSnapshot& snapshot) noexcept {
    return (snapshot.selectedPresent && snapshot.selectedLiving) ||
        (snapshot.pendingPresent && snapshot.pendingLiving);
}
void QueueTargetSuccessionProbe(const Actor& actor, const TargetSelectionSnapshot& snapshot,
    const TargetSuccessionPhase phase, const ULONGLONG now) noexcept {
    TargetSuccessionProbe probe{};
    probe.sessionCookie = gCurrent.sessionCookie;
    probe.tick = now;
    probe.retiredPeer = actor.postDeathRetiredPeer;
    probe.selectedPeer = snapshot.selectedPeer;
    probe.pendingPeer = snapshot.pendingPeer;
    probe.nativeGeneration = gCurrent.nativeGeneration;
    probe.worldGeneration = gCurrent.worldGeneration;
    probe.entityA = actor.entityA;
    probe.entityB = actor.entityB;
    probe.birth = actor.birth;
    probe.interactionRevision = actor.desired.interactionRevision;
    probe.pairRevision = actor.desired.interactionPairRevision;
    probe.phase = static_cast<std::uint32_t>(phase);
    probe.selectedBirth = snapshot.selectedBirth;
    probe.pendingBirth = snapshot.pendingBirth;
    probe.haveTargetSignal = snapshot.haveTargetSignal;
    probe.haveNextTargetSignal = snapshot.haveNextTargetSignal;
    probe.selectionReadable = snapshot.selectionReadable;
    probe.selectedReadable = snapshot.selectedReadable;
    probe.selectedPresent = snapshot.selectedPresent;
    probe.selectedLiving = snapshot.selectedLiving;
    probe.selectedTerminalDead = snapshot.selectedTerminalDead;
    probe.pendingReadable = snapshot.pendingReadable;
    probe.pendingPresent = snapshot.pendingPresent;
    probe.pendingLiving = snapshot.pendingLiving;
    probe.pendingTerminalDead = snapshot.pendingTerminalDead;
    probe.targetSignalsReadable = snapshot.targetSignalsReadable;
    const auto write = gTargetSuccessionProbeWrite.load(std::memory_order_relaxed);
    if (write - gTargetSuccessionProbeRead.load(std::memory_order_acquire) >= kParticipantProbeRing) {
        gTargetSuccessionDropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    gTargetSuccessionProbes[write % kParticipantProbeRing] = probe;
    gTargetSuccessionProbeWrite.store(write + 1U, std::memory_order_release);
    gTargetSuccessionSamples.fetch_add(1U, std::memory_order_relaxed);
}
void TraceTargetSelection(Actor& actor, const TargetSuccessionPhase phase,
    const ULONGLONG now, const bool force) noexcept {
    TargetSelectionSnapshot snapshot{};
    static_cast<void>(ReadTargetSelectionSnapshot(actor, snapshot));
    const bool changed = !actor.targetSelectionObserved ||
        actor.observedSelectedRecord != snapshot.selectedRecord ||
        actor.observedPendingRecord != snapshot.pendingRecord ||
        actor.observedSelectedPeer != snapshot.selectedPeer ||
        actor.observedPendingPeer != snapshot.pendingPeer ||
        actor.observedSelectedBirth != snapshot.selectedBirth ||
        actor.observedPendingBirth != snapshot.pendingBirth ||
        actor.observedHaveTargetSignal != snapshot.haveTargetSignal ||
        actor.observedHaveNextTargetSignal != snapshot.haveNextTargetSignal ||
        actor.observedSelectedPresent != snapshot.selectedPresent ||
        actor.observedSelectedLiving != snapshot.selectedLiving ||
        actor.observedSelectedTerminalDead != snapshot.selectedTerminalDead ||
        actor.observedPendingPresent != snapshot.pendingPresent ||
        actor.observedPendingLiving != snapshot.pendingLiving ||
        actor.observedPendingTerminalDead != snapshot.pendingTerminalDead ||
        actor.observedSelectionReadable != snapshot.selectionReadable ||
        actor.observedSelectedReadable != snapshot.selectedReadable ||
        actor.observedPendingReadable != snapshot.pendingReadable ||
        actor.observedTargetSignalsReadable != snapshot.targetSignalsReadable;
    actor.targetSelectionObserved = true;
    actor.observedSelectedRecord = snapshot.selectedRecord;
    actor.observedPendingRecord = snapshot.pendingRecord;
    actor.observedSelectedPeer = snapshot.selectedPeer;
    actor.observedPendingPeer = snapshot.pendingPeer;
    actor.observedSelectedBirth = snapshot.selectedBirth;
    actor.observedPendingBirth = snapshot.pendingBirth;
    actor.observedHaveTargetSignal = snapshot.haveTargetSignal;
    actor.observedHaveNextTargetSignal = snapshot.haveNextTargetSignal;
    actor.observedSelectedPresent = snapshot.selectedPresent;
    actor.observedSelectedLiving = snapshot.selectedLiving;
    actor.observedSelectedTerminalDead = snapshot.selectedTerminalDead;
    actor.observedPendingPresent = snapshot.pendingPresent;
    actor.observedPendingLiving = snapshot.pendingLiving;
    actor.observedPendingTerminalDead = snapshot.pendingTerminalDead;
    actor.observedSelectionReadable = snapshot.selectionReadable;
    actor.observedSelectedReadable = snapshot.selectedReadable;
    actor.observedPendingReadable = snapshot.pendingReadable;
    actor.observedTargetSignalsReadable = snapshot.targetSignalsReadable;
    if (force || changed) QueueTargetSuccessionProbe(actor, snapshot, phase, now);
}
bool PostKillTargetTraceActive(const Actor& actor) noexcept {
    if (actor.postDeathArbitrationRevision != 0U) return true;
    const auto& state = actor.desired;
    if (state.interactionTargetPeer == 0U || state.actionTypeRva != 0x00ECB770U) return false;
    const auto target = ParticipantForPeer(state.interactionTargetPeer);
    return target != 0U && TerminalDeadCharacter(target);
}
void ClearPostDeathArbitration(Actor& actor) noexcept {
    actor.postDeathArbitrationRevision = 0U;
    actor.postDeathArbitrationSince = 0U;
    actor.postDeathRetiredPeer = 0U;
}
void RecoverDeadParticipantTarget(Actor& actor, const ULONGLONG now) noexcept {
    if (PairSourceBusy(actor.character)) return;
    const auto& state = actor.desired;

    // Once the old pair has been physically retired, give retail arbitration a
    // bounded opportunity to promote/reacquire a valid successor before using
    // the legacy global cleanup as a last-resort recovery. The dead-melee guard
    // is armed by recoveredInteractionRevision independently of this reset.
    if (actor.postDeathArbitrationRevision != 0U) {
        if (state.interactionRevision != actor.postDeathArbitrationRevision) {
            TraceTargetSelection(actor, TargetSuccessionPhase::InteractionAdvanced, now, true);
            ClearPostDeathArbitration(actor);
            actor.deadTargetSince = 0U;
            return;
        }
        TargetSelectionSnapshot selection{};
        static_cast<void>(ReadTargetSelectionSnapshot(actor, selection));
        TraceTargetSelection(actor, TargetSuccessionPhase::RecoveryEvaluation, now, false);
        if (!TargetSelectionCertain(selection)) {
            gPostDeathDecisionUncertain.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        if (HasLivingSuccessor(selection)) {
            gPostDeathSuccessionsPreserved.fetch_add(1U, std::memory_order_relaxed);
            TraceTargetSelection(actor, TargetSuccessionPhase::SuccessorPreserved, now, true);
            ClearPostDeathArbitration(actor);
            actor.deadTargetSince = 0U;
            return;
        }
        // A live participant can become visible just after the old paired kill
        // retires. In .186 the one-second fallback reset landed in exactly that
        // window and erased retail arbitration before pending/selected could be
        // rebuilt. Keep the BT intact while a real participant sense is recent;
        // this never invents sight, threat or a target.
        const auto sensedParticipant = ParticipantForPeer(actor.lastLivingParticipantSensePeer);
        if (actor.lastLivingParticipantSense != 0U && sensedParticipant != 0U &&
            LivingCharacter(sensedParticipant) &&
            now - actor.lastLivingParticipantSense < kPostDeathRecentSenseHold) {
            gPostDeathArbitrationDeferrals.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        if (now - actor.postDeathArbitrationSince < kPostDeathArbitrationGracePeriod) {
            gPostDeathArbitrationDeferrals.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        bool survivor = LivingCharacter(LocalPlayer());
        for (std::uint32_t i = 0U; !survivor && i < gCurrent.participantCount; ++i)
            survivor = LivingCharacter(ParticipantForPeer(gCurrent.participants[i].peer));
        if (!survivor) return;

        const bool hadRoleTask = actor.lastRoleTask != 0U;
        const bool treeReset = SuspendExistingTree(actor);
        const bool roleReset = ClearRoleTaskExecutor(actor);
        if (roleReset) {
            if (hadRoleTask) gPostDeathRoleTaskResets.fetch_add(1U, std::memory_order_relaxed);
            actor.lastRoleTask = 0U;
        }
        if (treeReset && roleReset) {
            gPostDeathDecisionResets.fetch_add(1U, std::memory_order_relaxed);
            TraceTargetSelection(actor, TargetSuccessionPhase::FallbackReset, now, true);
        } else {
            gPostDeathDecisionResetRejected.fetch_add(1U, std::memory_order_relaxed);
            TraceTargetSelection(actor, TargetSuccessionPhase::FallbackResetRejected, now, true);
        }
        ClearPostDeathArbitration(actor);
        actor.deadTargetSince = 0U;
        return;
    }

    // EC B770 is the retail ActionMeleeAttack vtable (factory 006F6640).
    // Retire only this exact completed paired attack; never cancel traversal or
    // a live victim. A previously retired revision is handled by the bounded
    // arbitration phase above, not by another destructive cleanup.
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
        actor.deadTargetSince = 0U;
        gPairedRecoveryDeferred.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (actor.deadTargetSince == 0U) actor.deadTargetSince = now;
    if (now - actor.deadTargetSince < kDeadTargetGracePeriod) return;

    TargetSelectionSnapshot before{};
    static_cast<void>(ReadTargetSelectionSnapshot(actor, before));
    TraceTargetSelection(actor, TargetSuccessionPhase::RecoveryEvaluation, now, true);
    // Unknown selector state is not equivalent to "no successor". Do not run a
    // destructive recovery until selected/pending can both be revalidated.
    if (!TargetSelectionCertain(before)) {
        gPostDeathDecisionUncertain.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    bool survivor = LivingCharacter(LocalPlayer());
    for (std::uint32_t i = 0U; !survivor && i < gCurrent.participantCount; ++i)
        survivor = LivingCharacter(ParticipantForPeer(gCurrent.participants[i].peer));
    if (!survivor || !character_direct_step_runtime::FinishReplicatedInteraction(actor.character)) return;

    // The old pair is now retired. Arm the dead-melee guard immediately, even
    // though no global BT/task reset has happened. This keeps a new melee leaf
    // from being admitted against the terminal incarnation while retail gets a
    // chance to preserve/promote another target naturally.
    actor.recoveredInteractionRevision = state.interactionRevision;
    actor.postDeathArbitrationRevision = state.interactionRevision;
    actor.postDeathArbitrationSince = now;
    actor.postDeathRetiredPeer = state.interactionTargetPeer;
    actor.deadTargetSince = 0U;
    gDeadTargetRecoveries.fetch_add(1U, std::memory_order_relaxed);
    TraceTargetSelection(actor, TargetSuccessionPhase::PairRetired, now, true);

    TargetSelectionSnapshot after{};
    static_cast<void>(ReadTargetSelectionSnapshot(actor, after));
    if (!TargetSelectionCertain(after)) {
        gPostDeathDecisionUncertain.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (HasLivingSuccessor(after)) {
        gPostDeathSuccessionsPreserved.fetch_add(1U, std::memory_order_relaxed);
        TraceTargetSelection(actor, TargetSuccessionPhase::SuccessorPreserved, now, true);
        ClearPostDeathArbitration(actor);
        return;
    }
    // Deliberately return to the retail manager for at least a bounded window.
    // The fallback reset is evaluated on subsequent manager frames only.
    gPostDeathArbitrationDeferrals.fetch_add(1U, std::memory_order_relaxed);
}

// .174 observed 17,523 new pair attempts against the terminally dead Host,
// after a successful BT reset and an Idle source. ActionMeleeAttack's start
// checks record lifetime, not health. Reject its NEW candidate during retail
// root selection, before promotion/execution; failure 8 is propagated by
// FUN_00726190 to the parent, which owns fallback and target switching.
// This does not edit target records/signals or call SetPending/Commit.
bool RecordMatches(std::uintptr_t record, std::uintptr_t owner, std::uintptr_t target) noexcept;
bool RetiredMeleeSelection(void* node, const std::uintptr_t owner,
    const std::uintptr_t execution, const std::uintptr_t tree, std::uint64_t& peer) noexcept {
    peer = 0U;
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() ||
        !Stable() || owner == 0U || gAiPlayerQueryOwner != owner) return false;
    std::uintptr_t table{}, frameNode{}, frameTree{}, treeOwner{}, waiter{}, waiterControl{};
    std::uint32_t mode{}, status{};
    std::uint8_t deferred{}, closed{};
    // Factory 006F0A10 initializes mode 3 for selection, an unstarted frame,
    // and empty waiter handles. Mode 1 belongs to execution and is untouched.
    if (!Read(reinterpret_cast<std::uintptr_t>(node), table) || table != gBase + 0x00ECB770U ||
        !Read(execution + 0x10U, frameNode) || frameNode != reinterpret_cast<std::uintptr_t>(node) ||
        !Read(execution + 0x14U, frameTree) || frameTree != tree ||
        !Read(tree + 0xCU, treeOwner) || treeOwner != owner ||
        !Read(execution + 8U, mode) || mode != 3U ||
        !Read(execution + 4U, status) || status != 0U ||
        !Read(execution + 0x1FU, deferred) || deferred == 0U ||
        !Read(execution + 0x1EU, closed) || closed != 0U ||
        !Read(execution + 0x2CU, waiter) || waiter != 0U ||
        !Read(execution + 0x30U, waiterControl) || waiterControl != 0U) return false;
    auto* actor = Find(owner);
    std::uint32_t runtimeId{};
    if (actor == nullptr || actor->tree != tree || actor->blocked || actor->faulted || actor->duplicate ||
        !Read(owner + 0x50U, runtimeId) || runtimeId != actor->runtimeId || PairSourceBusy(owner)) return false;
    gPostDeathMeleeSelectionChecks.fetch_add(1U, std::memory_order_relaxed);
    const auto& state = actor->desired;
    if ((state.stateFlags & InteractionValid) == 0U || state.interactionTargetPeer == 0U ||
        state.interactionRevision == 0U || actor->recoveredInteractionRevision != state.interactionRevision)
        return false;
    const auto target = ParticipantForPeer(state.interactionTargetPeer);
    if (target == 0U || !TerminalDeadCharacter(target) || NativeVictimFlowRunning(owner, target)) return false;
    std::uintptr_t cycle{};
    std::uint8_t cycleState{};
    bool active{};
    if (!ReadPairedCycleActivity(owner, cycle, cycleState, active) || cycleState != 1U || active) return false;
    std::uintptr_t selection{}, manager{}, selectedOwner{}, record{}, selectedTarget{};
    if (!Read(owner + 0x34A8U, selection) || selection == 0U ||
        !Read(selection + 4U, manager) || manager == 0U ||
        !Read(manager + 0x128U, selectedOwner) || selectedOwner != owner ||
        !Read(selection + 0x10U, record) || record == 0U ||
        !Read(record + 0x42CU, selectedTarget) || selectedTarget != target ||
        !RecordMatches(record, owner, target)) return false;
    peer = state.interactionTargetPeer;
    return true;
}
void __fastcall MeleeSelectionDetour(void* node, void*, const std::uintptr_t owner,
    const std::uintptr_t execution, const std::uintptr_t tree,
    const std::uint32_t previousFailure, const std::uint32_t previousSuccess) noexcept {
    // The redirected CALL is ONLY in root selection (007261EB). The distinct
    // execution CALL in 00712E20, active animation updates, and victim workers
    // remain retail. ECX=node, FIVE stack arguments, RET 14h (20 bytes).
    using Dispatch = void(__thiscall*)(void*, std::uintptr_t, std::uintptr_t,
        std::uintptr_t, std::uint32_t, std::uint32_t);
    const auto original = reinterpret_cast<Dispatch>(gBase + 0x002D0260U);
    std::uint64_t peer{};
    const bool reject = RetiredMeleeSelection(node, owner, execution, tree, peer);
    original(node, owner, execution, tree, previousFailure, previousSuccess);
    if (!reject) return;
    std::uint32_t status{};
    // For this certified deferred leaf, the original selection callback is
    // empty (00AEF7B0/RET 14h) and leaves status 1. No cycle/waiter has begun.
    // Retail unwinding then releases this native frame and tries alternatives.
    if (!Read(execution + 4U, status) || status != 1U || !Write(execution + 4U, std::uint32_t{8U})) {
        gPostDeathMeleeSelectionWriteFailures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    gPostDeathMeleeSelectionRejects.fetch_add(1U, std::memory_order_relaxed);
    gLastPostDeathRetiredPeer.store(peer, std::memory_order_relaxed);
}

void RememberPresentationPose(Actor& actor, const State& state, const ULONGLONG now) noexcept {
    if ((state.stateFlags & PoseValid) == 0U || actor.interactionOwnsCycle || ProxyWithdrawn(state)) {
        actor.poseCount = 0U; actor.poseRevision = 0U; actor.poseInterpolating = false;
        return;
    }
    if (actor.poseCount != 0U && actor.poseRevision == state.revision) return;
    if (actor.poseCount != 0U && now - actor.poses[actor.poseCount - 1U].tick > kRefreshPeriod)
        actor.poseCount = 0U; // no synthetic path across a transport stall
    if (actor.poseCount == kPoseHistory) {
        for (std::size_t i = 1U; i < kPoseHistory; ++i) actor.poses[i - 1U] = actor.poses[i];
        --actor.poseCount;
    }
    auto& sample = actor.poses[actor.poseCount++];
    sample.tick = now;
    for (std::size_t i = 0U; i < 3U; ++i) sample.position[i] = state.position[i];
    sample.yaw = state.yaw; sample.velocityX = state.velocityX; sample.velocityZ = state.velocityZ;
    actor.poseRevision = state.revision;
}
bool RecoverCatastrophicIntentDivergence(Actor& actor, const State& state,
    const ULONGLONG now) noexcept {
    const bool hostTransitionOwnsMotion = actor.replayedExecutionRevision != 0U &&
        actor.replayedExecutionRevision == state.executionRevision;
    if ((state.stateFlags & PoseValid) == 0U || ProxyWithdrawn(state) ||
        (state.lifecycleFlags & (LifecycleSpatialDiscontinuity | LifecycleTraversalAction)) != 0U ||
        PairSourceBusy(actor.character) || actor.interactionOwnsCycle || hostTransitionOwnsMotion ||
        state.navigation.flags != 0U) {
        actor.intentDivergenceSince = 0U;
        return true;
    }
    float current[3]{};
    for (std::size_t i = 0U; i < 3U; ++i)
        if (!Read(actor.character + kPositionOffset + i * sizeof(float), current[i]) ||
            !FiniteCoordinate(current[i])) return true;
    const auto dx = state.position[0] - current[0];
    const auto dy = state.position[1] - current[1];
    const auto dz = state.position[2] - current[2];
    const auto planar = std::hypot(dx, dz);
    if (!std::isfinite(planar) ||
        (planar < kIntentCatastrophicDistance &&
         std::abs(dy) < kIntentCatastrophicVerticalDistance)) {
        actor.intentDivergenceSince = 0U;
        return true;
    }
    if (actor.intentDivergenceSince == 0U) {
        actor.intentDivergenceSince = now;
        return true;
    }
    if (now - actor.intentDivergenceSince < kIntentCatastrophicGrace) return true;

    // This is intentionally not part of normal movement. It is a last-resort
    // recovery after seconds of multi-metre divergence, using the retail
    // Character relocation carrier. Ordinary corners/obstacles remain entirely
    // under local CATHODE locomotion/collision and can never trigger the old
    // 3.5m/500ms follower teleport.
    if (!PlaceCharacterPose(actor.character, state.position, state.yaw)) {
        gIntentRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    actor.intentDivergenceSince = 0U;
    actor.completedRevision = 0U;
    gIntentRecoveries.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool ApplyAuthoritativeExecution(Actor& actor, const ULONGLONG now) noexcept {
    const auto& state = actor.desired;
    if ((state.stateFlags & ExecutionValid) == 0U || state.executionRevision == 0U ||
        (state.executionFlags & ExecutionTransition) == 0U) {
        actor.replayedExecutionRevision = 0U;
        actor.completedExecutionRevision = state.executionRevision;
        return true;
    }
    if (state.executionRevision == actor.completedExecutionRevision) return true;
    actor.replayedExecutionRevision = 0U;
    // Unknown metadata is acknowledged once, but does not acquire movement.
    if (FindExecutionTransition(state.executionCycleTypeRva, state.executionCycleState) == nullptr) {
        actor.completedExecutionRevision = state.executionRevision;
        gExecutionReplayRejected.fetch_add(1U, std::memory_order_relaxed);
        gLastRejectedExecutionTypeRva.store(state.executionCycleTypeRva, std::memory_order_relaxed);
        gLastRejectedExecutionState.store(state.executionCycleState, std::memory_order_relaxed);
        return true;
    }
    if (actor.lastExecutionAttempt != 0U && now - actor.lastExecutionAttempt < kRefreshPeriod) return true;
    actor.lastExecutionAttempt = now;
    navigation::Cancel(actor.character);
    std::string error;
    const auto result = character_direct_step_runtime::ReplayAIExecutionTransition(
        actor.character, state.executionCycleTypeRva, state.executionCycleState, error);
    if (result == character_direct_step_runtime::AIExecutionReplayResult::Applied) {
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(actor.character));
        actor.completedExecutionRevision = state.executionRevision;
        actor.replayedExecutionRevision = state.executionRevision;
        actor.intentDivergenceSince = 0U;
        actor.lastExecutionAttempt = 0U;
        gExecutionReplayed.fetch_add(1U, std::memory_order_relaxed);
    } else {
        // A failed replay remains pending and can be retried; it cannot create
        // the ownership vacuum of the old reject-and-complete path.
        gExecutionReplayRejected.fetch_add(1U, std::memory_order_relaxed);
        gLastRejectedExecutionTypeRva.store(state.executionCycleTypeRva, std::memory_order_relaxed);
        gLastRejectedExecutionState.store(state.executionCycleState, std::memory_order_relaxed);
    }
    return true;
}

bool ApplyAuthoritativeIntent(Actor& actor, const ULONGLONG now) noexcept {
    const auto& state = actor.desired;
    if (PairSourceBusy(actor.character)) { actor.movementOwner = MovementOwner::Paired; return true; }
    if (actor.observerHandoffNeutral) {
        if (ObserverHandoffHasNewAuthority(actor, state)) {
            // Resume motion/execution, but retain the released-pair identity until
            // a new pair/birth arrives so delayed same-pair outro snapshots can
            // never reconstruct the old 4E.
            actor.observerHandoffNeutral = false;
            gPairedObserverHandoffResumed.fetch_add(1U, std::memory_order_relaxed);
            SamplePresentation(actor, now, 6U);
        } else {
            // FinishReplicatedInteraction has already returned the source to a
            // retail Idle. Do not overwrite it with stale steering from the
            // released kill snapshot while Host is still finishing its outro.
            actor.completedRevision = state.revision;
            actor.movementOwner = MovementOwner::Neutral;
            actor.lastProxyRefresh = now;
            gPairedObserverNeutralHolds.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
    }
    if (actor.pairPlacementPending) {
        gPairedSourcePoseSuppressed.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    if (actor.lastProxyRefresh == now) return true;
    if (now - actor.lastReceive >= kMotionLease) {
        navigation::Cancel(actor.character);
        actor.movementOwner = MovementOwner::Neutral;
        std::string idleError;
        return character_direct_step_runtime::HoldAIIntentIdle(actor.character, state.moveType, idleError);
    }
    if (!RecoverCatastrophicIntentDivergence(actor, state, now)) return false;
    if (!ApplyAuthoritativeExecution(actor, now)) return false;

    const bool executionApplied = actor.replayedExecutionRevision != 0U &&
        actor.replayedExecutionRevision == state.executionRevision;
    actor.movementOwner = ChooseMovementOwner(false, false, executionApplied, state.navigation.flags == 1U);
    if (actor.movementOwner == MovementOwner::Execution) {
        actor.completedRevision = state.revision;
        actor.lastProxyRefresh = now;
        return true;
    }
    if (actor.movementOwner == MovementOwner::Navigation) {
        std::string navigationError;
        if (!navigation::Apply(actor.character, state.navigation, actor.lastReceive + kMotionLease, navigationError))
            return false;
        actor.intentDivergenceSince = 0U;
        actor.completedRevision = state.revision;
        actor.lastProxyRefresh = now;
        gIntentApplied.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    navigation::Cancel(actor.character);
    // Unrecognized move requests retain the bounded steering carrier. Unknown
    // execution metadata does not suppress it and never runs a local brain.
    std::string error;
    const bool moving = (state.stateFlags & Moving) != 0U;
    const bool applied = moving
        ? character_direct_step_runtime::ArmAIIntentReplication(
            actor.character, state.velocityX, state.velocityZ,
            state.locomotionIntensity, state.moveType, error)
        : character_direct_step_runtime::HoldAIIntentIdle(
            actor.character, state.moveType, error);
    if (!applied) {
        gIntentRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    actor.completedRevision = state.revision;
    actor.lastProxyRefresh = now;
    gIntentApplied.fetch_add(1U, std::memory_order_relaxed);
    gPoseApplied.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool SameState(const State& a, const State& b) noexcept {
    if (std::memcmp(&a.outcome, &b.outcome, sizeof(a.outcome)) != 0) return false;
    if (std::memcmp(&a.navigation, &b.navigation, sizeof(NavigationGoalState)) != 0) return false;
    if (a.stateFlags != b.stateFlags || a.moveType != b.moveType ||
        a.locomotionRevision != b.locomotionRevision ||
        std::abs(a.locomotionIntensity - b.locomotionIntensity) > 0.002F ||
        a.actionTypeRva != b.actionTypeRva || a.actionPathHash != b.actionPathHash ||
        a.actionRevision != b.actionRevision || a.actionStatus != b.actionStatus ||
        a.actionDepth != b.actionDepth ||
        a.executionRevision != b.executionRevision ||
        a.executionCycleTypeRva != b.executionCycleTypeRva ||
        a.executionCycleState != b.executionCycleState ||
        a.executionFlags != b.executionFlags ||
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
        if (std::abs(a.interactionTargetPosition[i] - b.interactionTargetPosition[i]) > 0.01F)
            return false;
    // Ordinary world pose is recovery evidence, not a movement driver. Do not
    // wake the network for every centimetre of Host travel; current pose still
    // rides on intent/heartbeat packets and lifecycle/pair transitions force
    // their own revisions. During a live pair, source placement remains exact.
    const bool pairSensitive =
        ((a.stateFlags | b.stateFlags) & InteractionValid) != 0U ||
        ((a.lifecycleFlags | b.lifecycleFlags) &
            (LifecycleSpatialDiscontinuity | LifecycleTraversalAction)) != 0U;
    if (pairSensitive) {
        for (std::size_t i = 0U; i < 3U; ++i)
            if (std::abs(a.position[i] - b.position[i]) > 0.01F) return false;
        if (std::abs(a.yaw - b.yaw) > 0.01F) return false;
    }
    return std::abs(a.velocityX - b.velocityX) <= 0.02F &&
        std::abs(a.velocityZ - b.velocityZ) <= 0.02F;
}
void CaptureNavigationGoal(Actor& actor) noexcept {
    navigation::Sample sample{};
    if (!ProxyWithdrawn(actor.desired)) sample = navigation::Capture(actor.character);
    if (actor.interactionOwnsCycle ||
        (actor.desired.executionFlags & ExecutionPaired) != 0U ||
        (actor.desired.lifecycleFlags & LifecycleTraversalAction) != 0U) sample = {};
    const auto previous = actor.desired.navigation;
    if (sample.request != actor.capturedNavigationRequest) {
        actor.capturedNavigationRequest = sample.request;
        ++actor.capturedNavigationEpoch;
        if (actor.capturedNavigationEpoch == 0U) actor.capturedNavigationEpoch = 1U;
    }
    sample.goal.requestRevision = actor.capturedNavigationEpoch;
    sample.goal.revision = previous.revision;
    if (std::memcmp(&sample.goal, &previous, sizeof(NavigationGoalState)) != 0) {
        ++sample.goal.revision;
        if (sample.goal.revision == 0U) sample.goal.revision = 1U;
    }
    actor.desired.navigation = sample.goal;
}
void Publish(Actor& actor) noexcept {
    if (PairSourceBusy(actor.character)) return;
    CaptureOutcome(actor);
    // Execution cycles can be shorter than the 20 Hz network cadence. Observe
    // them on every certified BehaviourManager update; a cycle change clears
    // lastSend so the exact transition gets an immediate authority snapshot.
    CaptureExecutionCycle(actor);
    const auto now = GetTickCount64();
    bool newOutcome = actor.desired.outcome.revision != 0U &&
        (actor.desired.interactionPairRevision != actor.previous.interactionPairRevision ||
         actor.desired.outcome.revision != actor.previous.outcome.revision);
    if (!newOutcome && now - actor.lastSend < kSendPeriod) return;
    CaptureInteractionCycle(actor);
    newOutcome = actor.desired.outcome.revision != 0U &&
        (actor.desired.interactionPairRevision != actor.previous.interactionPairRevision ||
         actor.desired.outcome.revision != actor.previous.outcome.revision);
    if ((actor.desired.stateFlags & InteractionValid) != 0U &&
        (actor.desired.interactionFlags & InteractionCycleActive) == 0U &&
        actor.desired.outcome.revision == 0U &&
        actor.lastInteractionCapture != 0U && now - actor.lastInteractionCapture > kTimeout) {
        actor.desired.stateFlags &= ~InteractionValid;
        actor.desired.interactionFlags = 0U;
        actor.desired.interactionTransition = 0U;
        actor.desired.interactionTransitionFlags = 0U;
    }
    CaptureWithdrawState(actor);
    CaptureCharacterBackstage(actor);
    CapturePose(actor, now);
    CaptureActiveAction(actor);
    CaptureRoleTask(actor);
    CaptureNavigationGoal(actor);
    SamplePresentation(actor, now, newOutcome ? 3U : 0U);
    const bool changed = !SameState(actor.desired, actor.previous);
    if (!changed && now - actor.lastSend < kRefreshPeriod) return;
    auto state = actor.desired;
    if (changed) ++state.revision;
    if (newOutcome && (actor.previous.interactionPairRevision != state.interactionPairRevision ||
        actor.previous.outcome.revision == 0U)) {
        OutcomeBinding binding{};
        if (!LoadOutcome(actor.character, ParticipantForPeer(state.interactionTargetPeer), binding)) return;
        // This event presents the prepared pair first, before a possibly newer
        // hidden/vent pose. Ordinary convergence follows on the next snapshot.
        for (std::size_t i = 0U; i < 3U; ++i) state.position[i] = binding.state.position[i];
        state.yaw = binding.state.yaw;
        state.lifecycleFlags = binding.state.lifecycleFlags &
            ~(LifecycleWithdrawn | LifecycleWithdrawState2 | LifecycleHostDormant | LifecycleCharacterBackstage);
        state.stateFlags |= PoseValid;
    }
    auto& writeCursor = newOutcome ? gOutcomeWrite : gWrite;
    auto& readCursor = newOutcome ? gOutcomeRead : gRead;
    auto& queue = newOutcome ? gOutgoingOutcomes : gOutgoing;
    const auto write = writeCursor.load(std::memory_order_relaxed);
    if (write - readCursor.load(std::memory_order_acquire) >= kRing) {
        if (newOutcome) gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed);
        gDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    actor.desired.revision = state.revision;
    actor.previous = state; actor.lastSend = now;
    queue[write % kRing] = state;
    writeCursor.store(write + 1U, std::memory_order_release);
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
    CaptureOutcome(actor);
    // A Character can leave its paired layout while its brain goes dormant
    // (backstage/vent). Retention must stay fresh on the independent pump too.
    if (characterPresent) CaptureInteractionCycle(actor);
    if (actor.desired.outcome.revision != 0U &&
        (actor.desired.interactionPairRevision != actor.previous.interactionPairRevision ||
         actor.desired.outcome.revision != actor.previous.outcome.revision)) {
        actor.lastSend = 0U;
        Publish(actor);
        return;
    }

    if (characterPresent) {
        CaptureWithdrawState(actor);
        CaptureCharacterBackstage(actor);
        CaptureExecutionCycle(actor);
    }
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
    if (actor.desired.navigation.flags != 0U) {
        actor.desired.navigation.flags = 0U;
        ++actor.desired.navigation.revision;
        if (actor.desired.navigation.revision == 0U) actor.desired.navigation.revision = 1U;
        actor.capturedNavigationRequest = 0U;
    }
    actor.desired.velocityX = 0.0F;
    actor.desired.velocityZ = 0.0F;
    SamplePresentation(actor, now);

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
    std::uintptr_t control{}, navigation{}, backstageAdapter{}, sweep{}, sweepOwner{};
    sample.playerComponentsReadable = Read(target + 0x31CU, control) &&
        Read(target + 0x3500U, navigation) &&
        (control == 0U || Read(control + 0x48U, sample.playerControlLocal));
    sample.playerControlPresent = control != 0U;
    sample.playerNavigationPresent = navigation != 0U;
    sample.backstageReadable = Read(actor.character + 0x34F0U, backstageAdapter) && backstageAdapter != 0U &&
        Read(backstageAdapter + 0x1CU, sweep) && sweep != 0U && Read(sweep, sweepOwner) &&
        sweepOwner == actor.character && Read(sweep + 0x28U, sample.backstageState) &&
        Read(sweep + 0x2CU, sample.backstageKeyA) && Read(sweep + 0x30U, sample.backstageKeyB) &&
        Read(sweep + 0x51U, sample.backstageEnabled) && Read(sweep + 0x53U, sample.backstagePolicy);
    for (const auto& cached : gParticipantNavigation) {
        if (!cached.initialized || cached.faulted || cached.peer != peer || cached.character != target ||
            cached.nativeGeneration != gCurrent.nativeGeneration || cached.worldGeneration != gCurrent.worldGeneration ||
            cached.sessionCookie != gCurrent.sessionCookie) continue;
        sample.navigationCached = true;
        const auto address = reinterpret_cast<std::uintptr_t>(cached.native.data());
        std::uint8_t goalEnabled{};
        sample.navigationGoalKnown = Read(address + 0x455U, goalEnabled) && goalEnabled != 0U;
        static_cast<void>(Read(address + 0x450U, sample.navigationRouteCount));
        break;
    }
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
            sample.selectedTerminalDead = TerminalDeadCharacter(selectedTarget);
        }
    }
    // Pending remains the retail choice. Record it separately so the next
    // trace distinguishes fallback from an absent eligible replacement.
    std::uintptr_t pendingRecord{}, pendingTarget{}, haveTarget{}, haveNextTarget{};
    if (selection != 0U && Read(selection + 0x14U, pendingRecord)) {
        sample.pendingReadable = pendingRecord == 0U;
        if (pendingRecord != 0U && Read(pendingRecord + 0x42CU, pendingTarget) &&
            pendingTarget != 0U && RecordMatches(pendingRecord, actor.character, pendingTarget)) {
            sample.pendingReadable = true;
            sample.pendingPresent = true;
            sample.pendingPeer = PeerForParticipant(pendingTarget);
            sample.pendingLiving = LivingCharacter(pendingTarget);
        }
    }
    sample.targetSignalsReadable = Read(actor.character + 0x3154U, haveTarget) &&
        Read(actor.character + 0x31E8U, haveNextTarget) &&
        (haveTarget == 0U || Read(haveTarget + 0x10U, sample.haveTargetSignal)) &&
        (haveNextTarget == 0U || Read(haveNextTarget + 0x10U, sample.haveNextTargetSignal));
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
    if (sample.living && sample.recordPresent && sample.recordReadable && sample.recordActive != 0U &&
        (sample.sightInput > 0.0F || sample.activeSenseMask != 0U)) {
        actor.lastLivingParticipantSense = now;
        actor.lastLivingParticipantSensePeer = peer;
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
    bool traceTargetSuccession{};
    gUpdating = actor;
    if (actor != nullptr) {
        actor->blocked = Client();
        if (gRole.load(std::memory_order_acquire) == Role::Host) {
            gAiPlayerQueryOwner = actor->character;
            // Preserve the retail player's pre-death alliance before a kill can
            // move THE_PLAYER to the dead alliance used by the solo game.
            CacheLivingPlayerAlliance();
            traceTargetSuccession = PostKillTargetTraceActive(*actor);
            if (traceTargetSuccession)
                TraceTargetSelection(*actor, TargetSuccessionPhase::UpdateEntry, GetTickCount64(), false);
            else
                actor->targetSelectionObserved = false;
        }
        if (actor->blocked) gSuspended.fetch_add(1U, std::memory_order_relaxed);
        else {
            RecoverDeadParticipantTarget(*actor, GetTickCount64());
            if (gRole.load(std::memory_order_acquire) == Role::Host &&
                (traceTargetSuccession || actor->targetSelectionObserved || PostKillTargetTraceActive(*actor)))
                TraceTargetSelection(*actor, TargetSuccessionPhase::PostRecovery, GetTickCount64(), false);
        }
    }
    {
        AiPlayerSlotScope playerContext(actor != nullptr && !actor->blocked ? actor->character : 0U);
        original(manager, previousTime, delta);
    }
    if (actor != nullptr && actor != &refused && actor->character == c &&
        gRole.load(std::memory_order_acquire) == Role::Host &&
        (traceTargetSuccession || actor->targetSelectionObserved || PostKillTargetTraceActive(*actor)))
        TraceTargetSelection(*actor, TargetSuccessionPhase::PostRetailUpdate, GetTickCount64(), false);
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
        const auto now = GetTickCount64();
        SamplePresentation(*actor, now);
        // Both first-person and observer victim graphs must keep their actual
        // attacker until native fatal completion. Housekeeping still ran above.
        if (ClientNativeVictimOwnsPair(actor->character)) return;
        if (gRole.load(std::memory_order_acquire) == Role::Client && Stable() &&
            !actor->faulted && now - actor->lastReceive <= kTimeout &&
            actor->interactionOwnsCycle && PairedTransitionOwnsCycle(*actor, now)) return;
        const bool withdrawn = ProxyWithdrawn(actor->desired);
        if (!Stable() || gRole.load(std::memory_order_acquire) == Role::FrozenClient ||
            actor->faulted || now - actor->lastReceive > kTimeout ||
            (!withdrawn && (actor->desired.stateFlags & PoseValid) == 0U)) {
            ClearPairPlacement(*actor);
            static_cast<void>(Release(*actor));
        } else if (!withdrawn && !CharacterReady(*actor)) {
            if (actor->pairPlacementPending &&
                now - actor->pairPlacementSince <= kPairPlacementReadyTimeout) {
                // A native placement temporarily disables execution. Let retail
                // reconcile its body; keep the pair token and follower stopped.
                static_cast<void>(character_direct_step_runtime::Stop(actor->character));
            } else {
                if (actor->pairPlacementPending)
                    gPairedSourcePlacementExpired.fetch_add(1U, std::memory_order_relaxed);
                ClearPairPlacement(*actor);
                static_cast<void>(Release(*actor));
            }
        } else if (!ApplyAuthoritativeWithdraw(*actor, actor->desired) ||
            !ApplyAuthoritativeBackstage(*actor, actor->desired)) {
            actor->placementPending = true;
            static_cast<void>(Release(*actor));
        } else if (withdrawn) {
            navigation::Cancel(actor->character);
            ClearPairPlacement(*actor);
            // Host is backstage/withdrawn/dormant. Keep the follower quiesced
            // until the Host's explicit live frontstage state returns.
            static_cast<void>(character_direct_step_runtime::Stop(actor->character));
            actor->lastProxyRefresh = now;
        } else if (actor->placementPending) {
            // The CharacterManager snapshot pump retries placement independently
            // of a sleeping BehaviourManager. Do not arm a stale follower/pair.
            static_cast<void>(character_direct_step_runtime::Stop(actor->character));
        } else if (!ApplyAuthoritativeInteraction(*actor, now) ||
            (!PairedTransitionOwnsCycle(*actor, now) &&
                !ApplyAuthoritativeIntent(*actor, now))) {
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
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable()) {
        if (auto* actor = Find(owner); actor != nullptr &&
            (actor->targetSelectionObserved || PostKillTargetTraceActive(*actor)))
            TraceTargetSelection(*actor, TargetSuccessionPhase::PostPrepass, GetTickCount64(), false);
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
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable()) {
        if (auto* actor = Find(owner); actor != nullptr &&
            (actor->targetSelectionObserved || PostKillTargetTraceActive(*actor)))
            TraceTargetSelection(*actor, TargetSuccessionPhase::PostCoordinator, GetTickCount64(), false);
    }
    gAiPlayerQueryOwner = previousQueryOwner;
}

bool ClientAIImpactVictim(void* receiver, std::uintptr_t& victim) noexcept {
    victim = 0U;
    if (!Client() || receiver == nullptr) return false;
    std::uint32_t type{};
    // Both certified Character impact receivers keep their owning Character at
    // +0x24. On a Client, AISync-owned NPC consequences are Host-authoritative:
    // local/remote weapon presentation may run, but it must not independently
    // damage or retarget the replicated AI.
    return Read(reinterpret_cast<std::uintptr_t>(receiver) + 0x24U, victim) &&
        victim != 0U && Eligible(victim, type);
}

std::uintptr_t ResolveImpactSource(int* hit) noexcept {
    if (hit == nullptr || gBase == 0U) return 0U;
    std::uintptr_t resources{};
    if (!Read(gBase + kCharacterResolverRegistryRva, resources) || resources == 0U) return 0U;
    __try {
        return reinterpret_cast<SenseCharacterResolver>(gBase + kCharacterResolverRva)(
            reinterpret_cast<void*>(resources), reinterpret_cast<const std::uint32_t*>(hit));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0U; }
}

// Keep SEH in leaf functions that do not require C++ object unwinding. MSVC
// rejects __try in callers that also own std::lock_guard or other unwindable
// objects (C2712). These helpers isolate the retail calls without changing
// the authority semantics of the impact pipeline.
bool ResolveImpactDamageSafe(int* hit, std::int32_t& damage) noexcept {
    damage = 0;
    if (hit == nullptr || gBase == 0U) return false;
    __try {
        damage = static_cast<std::int32_t>(
            reinterpret_cast<ResolveImpactDamage>(gBase + kResolveImpactDamageRva)(hit));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        damage = 0;
        return false;
    }
}

bool ApplyNativeImpactDamageSafe(const std::uintptr_t target, const std::uint32_t damage,
    const std::uintptr_t source, const std::uint32_t descriptor) noexcept {
    if (target == 0U || source == 0U || damage == 0U || gBase == 0U) return false;
    __try {
        reinterpret_cast<NativeDamage>(gBase + 0x003CC900U)(
            reinterpret_cast<void*>(target), damage, source, 0.0F,
            0U, 1U, 0U, descriptor, 0U, 0U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PublishClientImpact(const std::uintptr_t victim, int* hit,
    const std::uint32_t channel) noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Client || !EngineThread() || !Stable() || victim == 0U ||
        hit == nullptr || gCurrent.localPeer == 0U || (channel != 1U && channel != 2U)) return false;
    if (ResolveImpactSource(hit) != LocalPlayer()) return false;

    std::int32_t damage{};
    if (channel == 1U) {
        if (!ResolveImpactDamageSafe(hit, damage)) return false;
    } else {
        std::uint16_t secondary{};
        if (!Read(reinterpret_cast<std::uintptr_t>(hit) + 0x38U, secondary)) return false;
        damage = static_cast<std::int32_t>(secondary);
    }
    if (damage <= 0 || damage > 0xFFFF) return false;

    std::uint32_t entityA{}, entityB{}, type{}, birth{}, descriptor{};
    if (!CharacterIdentity(victim, entityA, entityB, type) || !AICharacterClass(type) ||
        !Read(victim + 0x50U, birth) || birth == 0U ||
        !Read(reinterpret_cast<std::uintptr_t>(hit) + 0x18U, descriptor)) return false;

    ImpactEvent event{};
    event.nativeGeneration = gCurrent.nativeGeneration;
    event.sessionCookie = gCurrent.sessionCookie;
    event.worldGeneration = gCurrent.worldGeneration;
    event.sourcePeer = gCurrent.localPeer;
    event.targetA = entityA; event.targetB = entityB; event.targetClass = type; event.targetBirth = birth;
    event.damage = static_cast<std::uint32_t>(damage);
    event.descriptor = descriptor;
    event.channel = channel;

    std::lock_guard lock(gImpactMailboxMutex);
    const auto pending = gOutgoingImpactWrite - gOutgoingImpactRead;
    if (pending >= kRing) {
        gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    event.sequence = ++gImpactSequence;
    if (event.sequence == 0U) event.sequence = ++gImpactSequence;
    gOutgoingImpacts[gOutgoingImpactWrite % kRing] = event;
    ++gOutgoingImpactWrite;
    gImpactCaptured.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
void __fastcall CharacterImpactDetour(void* receiver, void*, int* hit) noexcept {
    const auto original = reinterpret_cast<CharacterImpact>(
        gCharacterImpactOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t victim{};
    if (ClientAIImpactVictim(receiver, victim)) {
        static_cast<void>(PublishClientImpact(victim, hit, 1U));
        return;
    }
    original(receiver, hit);
}
void __fastcall CharacterImpactSecondaryDetour(void* receiver, void*, int* hit) noexcept {
    const auto original = reinterpret_cast<CharacterImpact>(
        gCharacterImpactSecondaryOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    std::uintptr_t victim{};
    if (ClientAIImpactVictim(receiver, victim)) {
        static_cast<void>(PublishClientImpact(victim, hit, 2U));
        return;
    }
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
std::uint8_t __fastcall NavigationSubmitDetour(void* request, void*) noexcept {
    const auto original = reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(
        gNavigationSubmitOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    const auto accepted = original(request);
    if (accepted == 0U || gRole.load(std::memory_order_acquire) != Role::Host ||
        !EngineThread() || !Stable()) return accepted;
    const auto address = reinterpret_cast<std::uintptr_t>(request);
    std::uintptr_t type{}, scheduler{}, character{};
    if (!Read(address, type) || type != gBase + 0x00EB1A70U ||
        !Read(address + 0x14U, scheduler) || !Read(scheduler, character) ||
        scheduler != character + 0x2CD0U) return accepted;
    if (auto* actor = Find(character); actor != nullptr && !actor->duplicate && !actor->faulted) {
        // An allocator can reuse the SAME request address and goal between two
        // network samples. Native admission, rather than pointer equality,
        // creates the portable request epoch so completed Clients resume it.
        actor->capturedNavigationRequest = address;
        ++actor->capturedNavigationEpoch;
        if (actor->capturedNavigationEpoch == 0U) actor->capturedNavigationEpoch = 1U;
        actor->lastSend = 0U;
    }
    return accepted;
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
__declspec(naked) void BackstageEligibilityShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push edi
        push esi
        call BackstageParticipantEligibility
        add esp, 8
        mov [ebx + 28], eax
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        test eax, eax
        jnz accepted
        jmp dword ptr [gBackstageEligibilityTrampoline]
    accepted:
        jmp dword ptr [gBackstageEligibilityContinue]
    }
}
__declspec(naked) void BackstageNavigationShim() noexcept {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0FFFFFFF0h
        fxsave [esp]
        push ebp
        push ecx
        call BackstageParticipantNavigation
        add esp, 8
        mov [ebx], eax
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        // Replaces only MOV EDI,[EBP+3500]; all other registers and flags
        // retain their retail values. EDI was already saved by 0040BBF0.
        jmp dword ptr [gBackstageNavigationContinue]
    }
}
bool HookBackstageParticipantContext(HookManager& hooks, std::string& error) {
    // 0040C1C0: ECX=sweep context, no stack arguments, AL result, plain RET.
    // 0040C29F: ESI=sweep, EDI=chosen Character, skip only the solo FPS gate.
    // 0040BBFA: EBP=chosen Character, ECX=sweep; EDI is callee-saved already.
    // 004015E0: ECX=4D cycle; aligned native frame, ESI saved, plain RET.
    if (!MatchRetailBytes(0x0000C29FU,
            {0x8B,0x87,0x1C,0x03,0x00,0x00,0x85,0xC0,0x74,0x37,0x80,0x78,0x48,0x00,0x74,0x31}) ||
        !MatchRetailBytes(0x0000C2AFU, {0x83,0x7E,0x2C,0x00}) ||
        !MatchRetailBytes(0x0000C2C7U, {0x53,0x57,0x8B,0xCE,0xE8,0x20,0xF9,0xFF,0xFF}) ||
        !MatchRetailBytes(0x0000C2DAU, {0x8A,0xC3,0x5B,0x5F,0x5E,0xC3}) ||
        !MatchRetailBytes(0x0000BBF0U, {0x83,0xEC,0x10,0x55,0x8B,0x6C,0x24,0x18,0x56,0x57}) ||
        !MatchRetailBytes(0x0000BBFAU, {0x8B,0xBD,0x00,0x35,0x00,0x00,0x8B,0xF1,0x85,0xFF}) ||
        !MatchRetailBytes(0x0000BC0BU, {0x83,0xC4,0x10,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00321270U, {0x83,0xEC,0x7C,0x53,0x56,0x8B,0xF1}) ||
        !MatchRetailBytes(0x00321375U, {0x5E,0x5B,0x83,0xC4,0x7C,0xC3}) ||
        !MatchRetailBytes(0x003214E5U, {0x5F,0x8B,0xCE,0x5E,0x5B,0x83,0xC4,0x7C,0xE9,0x8E,0x6D,0xFB,0xFF}) ||
        !MatchRetailBytes(0x002D8280U, {0x81,0xEC,0x0C,0x0C,0x00,0x00,0x53,0x56,0x8B,0xF1,0x33,0xDB}) ||
        !MatchRetailBytes(0x002D851DU, {0x5E,0x5B,0x81,0xC4,0x0C,0x0C,0x00,0x00,0xC3}) ||
        !MatchRetailBytes(0x002EE5CBU, {0x6A,0x04,0x68,0x58,0x04,0x00,0x00,0xFF,0xD2}) ||
        !MatchRetailBytes(0x002EE61FU, {0x89,0x58,0x48,0x89,0x98,0x50,0x04,0x00,0x00,0x66,0x89,0x98,0x54,0x04,0x00,0x00}) ||
        !MatchRetailBytes(0x002EE639U, {0xC7,0x40,0x44,0xFF,0xFF,0xFF,0xFF,0x5B,0xC3}) ||
        !MatchRetailBytes(0x002C5DBAU, {0x66,0xC7,0x81,0x54,0x04,0x00,0x00,0x01,0x01,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x002C5DC6U, {0xC6,0x81,0x55,0x04,0x00,0x00,0x01,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x002F58D0U, {0x56,0x8B,0x71,0x04,0xC7,0x41,0x04,0x00,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x002F5919U, {0x5E,0xC3}) ||
        !MatchRetailBytes(0x00001628U, {0x8B,0x48,0x08,0x52,0xE8,0x9F,0x82,0x37,0x00}) ||
        !MatchRetailBytes(0x00001656U, {0x5E,0x8B,0xE5,0x5D,0xC3})) {
        error = "aisync backstage context: certified registers/stack/route-cache ABI mismatch";
        return false;
    }
    gBackstageEligibilityContinue = reinterpret_cast<void*>(gBase + 0x0000C2AFU);
    gBackstageNavigationContinue = reinterpret_cast<void*>(gBase + 0x0000BC00U);
    if (!Hook(hooks, "aisync_backstage_participant_update", 0x0000C1C0U,
            reinterpret_cast<void*>(&BackstageUpdateDetour), {0x56,0x8B,0xF1,0x8B,0x4E,0x10},
            &gBackstageUpdateTrampoline, &gBackstageUpdateOriginal, error) ||
        !Hook(hooks, "aisync_backstage_participant_eligibility", 0x0000C29FU,
            reinterpret_cast<void*>(&BackstageEligibilityShim), {0x8B,0x87,0x1C,0x03,0x00,0x00},
            &gBackstageEligibilityTrampoline, &gBackstageEligibilityPublished, error) ||
        !Hook(hooks, "aisync_backstage_participant_navigation", 0x0000BBFAU,
            reinterpret_cast<void*>(&BackstageNavigationShim), {0x8B,0xBD,0x00,0x35,0x00,0x00},
            &gBackstageNavigationTrampoline, &gBackstageNavigationPublished, error) ||
        !Hook(hooks, "aisync_backstage_hold_player_context", 0x000015E0U,
            reinterpret_cast<void*>(&BackstageHoldDetour), {0x55,0x8B,0xEC,0x83,0xE4,0xF0},
            &gBackstageHoldTrampoline, &gBackstageHoldOriginal, error)) return false;
    CallRedirectRequest selected;
    selected.name = "aisync_backstage_living_selected_player";
    selected.callSite = reinterpret_cast<void*>(gBase + 0x0000C285U);
    selected.expectedTarget = reinterpret_cast<void*>(gBase + 0x0000A3D0U);
    selected.detour = reinterpret_cast<void*>(&BackstageSelectedTargetDetour);
    return hooks.InstallCallRedirect(selected, error);
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
bool HookPairedPlayerClassification(HookManager& hooks, std::string& error) {
    // 006626F7: ECX=victim, ESI=source 4E cycle, EBX=0. Skip only the
    // NPC-controller test; 0066270E retains 29B/difficulty/health/fallback.
    // 006E3A4D: ESI=victim, EBX=filter. Resume at MOV EAX,1 (player category)
    // then the original three-argument filter call, with ECX/EDX unchanged.
    if (!MatchRetailBytes(0x002626F4U,
            {0x8B,0x4E,0x44,0x8B,0x81,0x1C,0x03,0x00,0x00,0x3B,0xC3,
             0x0F,0x84,0xFE,0x00,0x00,0x00,0x38,0x58,0x48,0x0F,0x84,0xF5,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x0026270EU,
            {0x38,0x99,0x9B,0x02,0x00,0x00,0x0F,0x85,0xE9,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x00262730U,
            {0x8B,0x4E,0x04,0x8B,0x01,0x8B,0x50,0x1C,0xFF,0xD2,
             0x8B,0x80,0x34,0x42,0x00,0x00,0x83,0x78,0x74,0x02,0x0F,0x8C,0xB9,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x002627FAU, {0xE8,0xD1,0x8B,0x09,0x00,0x3B,0xC3,0x75,0x34}) ||
        !MatchRetailBytes(0x002FB437U, {0xC2,0x10,0x00}) ||
        !MatchRetailBytes(0x002FB446U, {0xC2,0x10,0x00}) ||
        !MatchRetailBytes(0x002E3A4DU,
            {0x8B,0x86,0x1C,0x03,0x00,0x00,0x85,0xC0,0x74,0x0D,0x80,0x78,0x48,0x00,0x74,0x07,
             0xB8,0x01,0x00,0x00,0x00,0xEB,0x11}) ||
        !MatchRetailBytes(0x002E3A75U,
            {0x50,0x51,0x52,0x8B,0xCB,0xE8,0x41,0x23,0xFF,0xFF}) ||
        !MatchRetailBytes(0x002E3B06U,
            {0x5F,0x5E,0xB0,0x01,0x5B,0x8B,0xE5,0x5D,0xC2,0x14,0x00}) ||
        !MatchRetailBytes(0x002D5E25U, {0x5F,0x5B,0x5E,0x5D,0xC2,0x0C,0x00})) {
        error = "aisync paired player classification: certified victim/register/branch/stack bytes mismatch";
        return false;
    }
    gPlayerPreparationContinue = reinterpret_cast<void*>(gBase + 0x0026270EU);
    gPlayerCategoryContinue = reinterpret_cast<void*>(gBase + 0x002E3A5DU);
    gOutcomePreparationContinue = reinterpret_cast<void*>(gBase + 0x00262837U);
    gOutcomePreparationAbort = reinterpret_cast<void*>(gBase + 0x002626CCU);
    return Hook(hooks, "aisync_paired_player_preparation", 0x002626F7U,
        reinterpret_cast<void*>(&PlayerPreparationShim), {0x8B,0x81,0x1C,0x03,0x00,0x00},
        &gPlayerPreparationTrampoline, &gPlayerPreparationPublished, error) &&
        Hook(hooks, "aisync_paired_player_category", 0x002E3A4DU,
        reinterpret_cast<void*>(&PlayerCategoryShim), {0x8B,0x86,0x1C,0x03,0x00,0x00},
        &gPlayerCategoryTrampoline, &gPlayerCategoryPublished, error);
}

bool HookNativeOutcomes(HookManager& hooks, std::string& error) {
    // Every continuation starts at an instruction boundary. No relocated
    // operand or relative branch is copied into these three trampolines.
    std::uintptr_t definitionGetter{};
    if (!Read(gBase + 0x00ECC330U + 0x34U, definitionGetter) || definitionGetter != gBase + 0x002D60F0U ||
        !MatchRetailBytes(0x002D60F0U,
            {0x8B,0x44,0x24,0x04,0x83,0xF8,0x19,0x73,0x17,0x83,0xF8,0xFF,0x74,0x12,
             0x8B,0x89,0xC0,0x01,0x00,0x00,0x69,0xC0,0x84,0x00,0x00,0x00,
             0x03,0x41,0x04,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00262837U,
            {0x89,0x46,0x4C,0x39,0x58,0x08,0x0F,0x94,0xC0,0x88,0x86,0xE5,0x00,0x00,0x00}) ||
        !MatchRetailBytes(0x002626CCU,
            {0x8B,0x8C,0x24,0xE4,0x00,0x00,0x00,0x64,0x89,0x0D,0x00,0x00,0x00,0x00,
             0x5F,0x5E,0x5B,0x8B,0xE5,0x5D,0xC3}) ||
        !MatchRetailBytes(0x00255B30U, {0x81,0xEC,0x84,0x00,0x00,0x00,0x53,0x55,0x56,0x8B,0xF1}) ||
        !MatchRetailBytes(0x00255B4AU,
            {0x80,0xBE,0xDA,0x00,0x00,0x00,0x00,0x74,0x3A,0x8B,0x46,0x04,
             0x80,0xB8,0x9B,0x02,0x00,0x00,0x00,0x74,0x2E}) ||
        !MatchRetailBytes(0x00255B5FU,
            {0xC6,0x80,0xE8,0x02,0x00,0x00,0x00,0x8B,0x46,0x04,
             0xC6,0x80,0xEA,0x02,0x00,0x00,0x00,0x8B,0x4E,0x68,0x51,0x8B,0x4E,0x04,
             0xE8,0x94,0xB0,0x17,0x00,0x8B,0x4E,0x04,0x6A,0x02,
             0xE8,0x3A,0x48,0x14,0x00,0x8B,0xCE,0xE8,0x13,0x44,0xFF,0xFF}) ||
        !MatchRetailBytes(0x003B4AD0U,
            {0x56,0x8B,0xF1,0x0F,0xB7,0x46,0x1C,0x66,0x85,0xC0,0x74,0x62,
             0x66,0x8B,0x4C,0x24,0x08}) ||
        !MatchRetailBytes(0x003B4B3EU, {0x5E,0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x003CC952U, {0xC2,0x24,0x00}) ||
        !MatchRetailBytes(0x003D0C63U, {0x5F,0x5E,0x59,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x0026BF29U,
            {0x8B,0x4E,0x04,0x8B,0x11,0x8B,0x42,0x1C,0xFF,0xD0,
             0x8B,0x88,0xB4,0x34,0x00,0x00,0x0F,0xB7,0x86,0x0A,0x01,0x00,0x00,
             0x8B,0x11,0x8B,0x52,0x34,0x50,0xFF,0xD2,0x89,0x46,0x4C}) ||
        !MatchRetailBytes(0x00255DFAU, {0x5F,0x5E,0x5D,0x5B,0x81,0xC4,0x84,0x00,0x00,0x00,0xC3})) {
        error = "aisync native outcome: certified ECX/ESI/EAX/RET/continuation bytes mismatch";
        return false;
    }
    gVictimFinishSkip = reinterpret_cast<void*>(gBase + 0x00255B8DU);
    gVictimFinishKill = reinterpret_cast<void*>(gBase + 0x00255B5FU);
    if (!MatchRetailBytes(0x00249FA0U, {0x51,0x56,0x8B,0xF1,0x8B,0x46,0x04}) ||
        !MatchRetailBytes(0x0024A076U, {0x5E,0x59,0xC3})) {
        error = "aisync native outcome: victim finish ECX-only/plain RET mismatch"; return false;
    }
    if (!(Hook(hooks, "aisync_native_outcome_health_commit", 0x003B4AD0U,
            reinterpret_cast<void*>(&OutcomeHealthCommitDetour), {0x56,0x8B,0xF1,0x0F,0xB7,0x46,0x1C},
            &gHealthCommitTrampoline, &gHealthCommitOriginal, error) &&
        Hook(hooks, "aisync_native_outcome_victim_update", 0x00255B30U,
            reinterpret_cast<void*>(&VictimOutcomeUpdateDetour), {0x81,0xEC,0x84,0x00,0x00,0x00,0x53},
            &gVictimUpdateTrampoline, &gVictimUpdateOriginal, error) &&
        Hook(hooks, "aisync_native_outcome_victim_finish", 0x00255B4AU,
            reinterpret_cast<void*>(&VictimFinishShim), {0x80,0xBE,0xDA,0x00,0x00,0x00,0x00},
            &gVictimFinishTrampoline, &gVictimFinishPublished, error))) return false;
    CallRedirectRequest finished;
    finished.name = "aisync_native_outcome_victim_finish_executed";
    finished.callSite = reinterpret_cast<void*>(gBase + 0x00255B88U);
    finished.expectedTarget = reinterpret_cast<void*>(gBase + 0x00249FA0U);
    finished.detour = reinterpret_cast<void*>(&VictimFinishExecutedDetour);
    return hooks.InstallCallRedirect(finished, error);
}

bool HookPostDeathMeleeSelection(HookManager& hooks, std::string& error) {
    std::uintptr_t callback{};
    // These are ActionMeleeAttack's callbacks, not an Alien class test.
    // Compare relocated pointers by RVA; all byte ranges below contain no
    // HIGHLOW operands. Installation is refused on an unrecognized contract.
    if (!Read(gBase + 0x00ECB770U + 0x30U, callback) || callback != gBase + 0x006EF7B0U ||
        !Read(gBase + 0x00ECB770U + 0x34U, callback) || callback != gBase + 0x00317D90U ||
        !Read(gBase + 0x00ECB770U + 0x48U, callback) || callback != gBase + 0x002F0A10U ||
        !Read(gBase + 0x00ECB770U + 0x3D8U, callback) || callback != gBase + 0x00AA70F0U ||
        !MatchRetailBytes(0x006EF7B0U, {0xC2,0x14,0x00}) ||
        !MatchRetailBytes(0x00AA70F0U, {0xB0,0x01,0xC3}) ||
        !MatchRetailBytes(0x003261DDU,
            {0x52,0x0F,0xB6,0x11,0x8B,0x4E,0x0C,0x52,0x56,0x50,0x51,0x8B,0x48,0x10,
             0xE8,0x70,0xA0,0xFA,0xFF}) ||
        !MatchRetailBytes(0x002D0260U,
            {0x56,0x8B,0x74,0x24,0x0C,0x80,0x7E,0x1F,0x00,0x57,0x8B,0xF9,0x74,0x4C,
             0x8B,0x07,0x8B,0x90,0xD8,0x03,0x00,0x00,0xFF,0xD2,0x84,0xC0,0x74,0x3E,
             0xB8,0x01,0x00,0x00,0x00,0x39,0x46,0x08,0x74,0x0A,0x89,0x46,0x04,
             0x8B,0x07,0x8B,0x40,0x30,0xEB,0x36}) ||
        !MatchRetailBytes(0x002D02B5U, {0x5F,0x5E,0xC2,0x14,0x00}) ||
        !MatchRetailBytes(0x002D02DFU, {0x5F,0x5E,0xC2,0x14,0x00}) ||
        !MatchRetailBytes(0x002F0A33U,
            {0x8B,0x74,0x24,0x08,0x89,0x48,0x10,0x8B,0x4C,0x24,0x0C,0x89,0x70,0x08,
             0x89,0x50,0x04,0x89,0x50,0x0C,0x89,0x48,0x14,0x89,0x50,0x18,0x89,0x50,0x20,
             0x89,0x50,0x24,0x88,0x50,0x28,0xC7,0x40,0x1C,0x00,0x00,0x00,0x01}) ||
        !MatchRetailBytes(0x002F0A63U,
            {0x89,0x50,0x2C,0x89,0x50,0x30,0x5E,0xC2,0x08,0x00}) ||
        !MatchRetailBytes(0x0032630BU,
            {0x8B,0x4F,0x10,0x8B,0x11,0x8B,0x46,0x0C,0x8B,0x52,0x44,0x56,
             0x6A,0x08,0x57,0x50,0xFF,0xD2})) {
        error = "aisync post-death melee selection: certified mode/frame/ECX/five arguments/RET 14h mismatch";
        return false;
    }
    CallRedirectRequest selection;
    selection.name = "aisync_post_death_melee_selection";
    selection.callSite = reinterpret_cast<void*>(gBase + 0x003261EBU);
    selection.expectedTarget = reinterpret_cast<void*>(gBase + 0x002D0260U);
    selection.detour = reinterpret_cast<void*>(&MeleeSelectionDetour);
    return hooks.InstallCallRedirect(selection, error);
}
bool ValidateCharacterLifecycleAbi(std::string& error) {
    if (!MatchRetailBytes(0x0038B140U,
            {0x53,0x8B,0x5C,0x24,0x08,0x56,0x8B,0xF1,0x8D,0x8E,0xF0,0x03,0x00,0x00,
             0x88,0x9E,0xDA,0x02,0x00,0x00,0x88,0x9E,0xD9,0x02,0x00,0x00}) ||
        !MatchRetailBytes(0x0038B15EU, {0x53,0xE8,0x9C,0x69,0xF0,0xFF}) ||
        !MatchRetailBytes(0x0038B177U, {0x53,0xE8,0xF3,0x76,0xFF,0xFF}) ||
        !MatchRetailBytes(0x0038B17DU, {0x5E,0x5B,0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x003C2A65U,
            {0x53,0x8B,0xCE,0xE8,0xD3,0x86,0xFC,0xFF,0x8B,0x06,0x8B,0x50,0x1C,
             0x8B,0xCE,0xFF,0xD2,0x8B,0xC8,0xE8,0xA3,0xD7,0xFF,0xFF}) ||
        !MatchRetailBytes(0x003C0238U, {0x53,0x56,0x8B,0xF1,0x33,0xDB,0x38,0x9E,0xF8,0x02,0x00,0x00}) ||
        !MatchRetailBytes(0x003C03A7U, {0x83,0xC4,0x28,0xC3}) ||
        !MatchRetailBytes(0x003C6330U,
            {0x53,0x8B,0x5C,0x24,0x08,0x55,0x8B,0x6C,0x24,0x14,0x57,0x8B,0xF9}) ||
        !MatchRetailBytes(0x003C6389U,
            {0xF3,0x0F,0x10,0x44,0x24,0x14,0x6A,0x00,0x55,0x51,0xF3,0x0F,0x11,0x04,0x24,
             0x53,0x8B,0xCF,0xE8,0x70,0xC3,0xFF,0xFF,0x5F,0x5D,0x5B,0xC2,0x0C,0x00})) {
        error = "aisync Character lifecycle: certified backstage/frontstage/placement ABI mismatch";
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

bool HookPairedGraphTrace(HookManager& hooks, std::string& error) {
    if (!MatchRetailBytes(0x00264AC8U, {0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00265A25U, {0xC2,0x04,0x00}) ||
        !MatchRetailBytes(0x00255A5BU, {0xC3})) {
        error = "AISync paired graph callback ABI mismatch";
        return false;
    }
    // 00664A10: PUSH ESI; PUSH EDI; PUSH string (PE HIGHLOW). Complete
    // prefix with its loaded ASLR operand. Both event handlers have RET 4.
    HookRequest source;
    source.name = "aisync_paired_source_graph_events";
    source.target = reinterpret_cast<void*>(gBase + 0x00264A10U);
    source.detour = reinterpret_cast<void*>(&SourceGraphEventDetour);
    source.patchLength = 7U;
    source.expected.bytes = {0x56,0x57,0x68,0,0,0,0};
    const auto operand = static_cast<std::uint32_t>(gBase + 0x00EAE984U);
    std::memcpy(source.expected.bytes.data() + 3U, &operand, sizeof(operand));
    source.expected.mask = "xxxxxxx";
    source.relocationSafePrefix = true;
    source.trampolineOut = &gSourceGraphEventTrampoline;
    source.publishedTrampolineOut = &gSourceGraphEventOriginal;
    return hooks.Install(source, error) &&
        Hook(hooks, "aisync_paired_victim_graph_events", 0x002658E0U,
            reinterpret_cast<void*>(&VictimGraphEventDetour), {0x83,0xEC,0x1C,0x56,0x57},
            &gVictimGraphEventTrampoline, &gVictimGraphEventOriginal, error) &&
        Hook(hooks, "aisync_paired_source_native_finish", 0x002559A0U,
            reinterpret_cast<void*>(&SourceGraphFinishDetour), {0x83,0xEC,0x10,0x56,0x8B,0xF1},
            &gSourceGraphFinishTrampoline, &gSourceGraphFinishOriginal, error);
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
    if (!ValidateRoleTaskCleanupAbi(error) || !ValidateCharacterLifecycleAbi(error) ||
        !navigation::Initialize(base, error)) return false;
    gBranchExit = reinterpret_cast<void*>(base + 0x003344D4U);
    const auto first = hooks.InstalledCount();
    // Complete instructions; no relative CALL/Jcc in a prefix. The PUSH
    // handler operands below are certified PE HIGHLOW relocations; their loaded
    // absolute values remain valid when copied into the trampoline.
    const bool installed =
        HookNativeOutcomes(hooks, error) &&
        HookParticipantSenseSignals(hooks, error) &&
        HookParticipantPairedIdentity(hooks, error) &&
        HookPairedVictimGuards(hooks, error) &&
        HookPairedGraphTrace(hooks, error) &&
        Hook(hooks, "aisync_native_navigation_submission", 0x00283CE0U,
            reinterpret_cast<void*>(&NavigationSubmitDetour), {0x64,0xA1,0,0,0,0,0x6A,0xFF},
            &gNavigationSubmitTrampoline, &gNavigationSubmitOriginal, error) &&
        HookParticipantEligibility(hooks, error) &&
        HookPostDeathMeleeSelection(hooks, error) &&
        HookPairedPlayerClassification(hooks, error) &&
        HookBackstageParticipantContext(hooks, error) &&
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
    character_direct_step_runtime::SetNativeAIIntentObserver(&ObserveNativeAIIntent);
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
    if (contextChanged) {
        gIncomingUsed.fill(false);
        for (auto& queue : gIncomingOutcomes) queue.clear();
        gLastQueuedOutcome.fill({}); gIncomingOutcomeCount = 0U;
        std::lock_guard impactLock(gImpactMailboxMutex);
        gOutgoingImpactRead = gOutgoingImpactWrite = 0U;
        gIncomingImpactRead = gIncomingImpactWrite = 0U;
        gImpactSequence = 0U;
        gLastImpactSequence.fill({});
    }
    return true;
}

bool ValidImpactEvent(const ImpactEvent& value) noexcept {
    return value.version == 1U && value.nativeGeneration != 0U && value.sessionCookie != 0U &&
        value.worldGeneration != 0U && value.sequence != 0U && value.sourcePeer != 0U &&
        (value.targetA != 0U || value.targetB != 0U) && AICharacterClass(value.targetClass) &&
        value.targetBirth != 0U && value.damage != 0U && value.damage <= 0xFFFFU &&
        (value.channel == 1U || value.channel == 2U) && value.reserved == 0U;
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
    std::size_t selected = kActors;
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (!gIncomingUsed[i]) { if (free == kActors) free = i; continue; }
        const auto& old = gIncoming[i];
        if (old.entityA != value.entityA || old.entityB != value.entityB) continue;
        if (value.birth < old.birth || (value.birth == old.birth && value.revision < old.revision)) return true;
        selected = i; break;
    }
    if (selected == kActors) selected = free;
    if (selected == kActors) { gDropped.fetch_add(1U, std::memory_order_relaxed); return false; }
    auto& last = gLastQueuedOutcome[selected];
    const bool differentActor = last.entityA != value.entityA || last.entityB != value.entityB;
    const bool newOutcome = value.outcome.revision != 0U && (differentActor || value.birth > last.birth ||
        (value.birth == last.birth && (value.interactionPairRevision > last.interactionPairRevision ||
         (value.interactionPairRevision == last.interactionPairRevision && value.outcome.revision > last.outcome.revision))));
    if (newOutcome) {
        if (gIncomingOutcomeCount >= kRing) {
            gOutcomeRejected.fetch_add(1U, std::memory_order_relaxed); return false;
        }
        gIncomingOutcomes[selected].push_back(value); ++gIncomingOutcomeCount; last = value;
    }
    gIncoming[selected] = value; gIncomingUsed[selected] = true; gIncomingAt[selected] = GetTickCount64();
    if (selected == free) gNextIdentityProbe[selected] = 0ULL;
    return true;
}

bool QueueRemoteImpact(const std::span<const std::byte> payload) noexcept {
    ImpactEvent value{};
    if (payload.size() != sizeof(value) || !Installed()) return false;
    std::memcpy(&value, payload.data(), sizeof(value));
    if (!ValidImpactEvent(value)) return false;
    std::lock_guard mailboxLock(gMailboxMutex);
    std::lock_guard lock(gImpactMailboxMutex);
    if (gRequested.role != Role::Host || value.sessionCookie != gRequested.sessionCookie ||
        value.worldGeneration != gRequested.worldGeneration || value.sourcePeer == gRequested.localPeer) {
        gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    if (gIncomingImpactWrite - gIncomingImpactRead >= kRing) {
        gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    gIncomingImpacts[gIncomingImpactWrite % kRing] = value;
    ++gIncomingImpactWrite;
    gImpactQueued.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool TryPopObserved(State& state) noexcept {
    const auto resultRead = gOutcomeRead.load(std::memory_order_relaxed);
    if (resultRead != gOutcomeWrite.load(std::memory_order_acquire)) {
        state = gOutgoingOutcomes[resultRead % kRing];
        gOutcomeRead.store(resultRead + 1U, std::memory_order_release);
        return true;
    }
    const auto read = gRead.load(std::memory_order_relaxed);
    if (read == gWrite.load(std::memory_order_acquire)) return false;
    state = gOutgoing[read % kRing];
    gRead.store(read + 1U, std::memory_order_release);
    return true;
}

bool TryPopObservedImpact(ImpactEvent& impact) noexcept {
    std::lock_guard lock(gImpactMailboxMutex);
    if (gOutgoingImpactRead == gOutgoingImpactWrite) return false;
    impact = gOutgoingImpacts[gOutgoingImpactRead % kRing];
    ++gOutgoingImpactRead;
    return true;
}

bool AcceptImpactSequence(const std::uint64_t peer, const std::uint32_t sequence) noexcept {
    ImpactSequence* free{};
    for (auto& entry : gLastImpactSequence) {
        if (entry.peer == peer) {
            if (sequence <= entry.sequence) return false;
            entry.sequence = sequence;
            return true;
        }
        if (entry.peer == 0U && free == nullptr) free = &entry;
    }
    if (free == nullptr) return false;
    free->peer = peer; free->sequence = sequence;
    return true;
}

void PumpHostImpacts() noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Host || !Stable()) return;
    std::array<ImpactEvent, 32U> batch{};
    std::size_t count{};
    {
        std::lock_guard lock(gImpactMailboxMutex);
        while (count < batch.size() && gIncomingImpactRead != gIncomingImpactWrite) {
            batch[count++] = gIncomingImpacts[gIncomingImpactRead % kRing];
            ++gIncomingImpactRead;
        }
    }
    for (std::size_t i = 0U; i < count; ++i) {
        const auto& impact = batch[i];
        if (!ValidImpactEvent(impact) || impact.sessionCookie != gCurrent.sessionCookie ||
            impact.worldGeneration != gCurrent.worldGeneration || impact.sourcePeer == gCurrent.localPeer ||
            !AcceptImpactSequence(impact.sourcePeer, impact.sequence)) {
            gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        const auto source = ParticipantForPeer(impact.sourcePeer);
        const auto target = FindCharacterByIdentity(impact.targetA, impact.targetB);
        std::uint32_t entityA{}, entityB{}, type{}, birth{};
        if (source == 0U || !RegisteredPlayerRepresentation(source) || target == 0U ||
            !CharacterIdentity(target, entityA, entityB, type) || entityA != impact.targetA ||
            entityB != impact.targetB || type != impact.targetClass || !Read(target + 0x50U, birth) ||
            birth != impact.targetBirth || TerminalDeadCharacter(target)) {
            gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        std::uint32_t eligibleType{};
        if (!Eligible(target, eligibleType) || eligibleType != type) {
            gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (!ApplyNativeImpactDamageSafe(target, impact.damage, source, impact.descriptor)) {
            gImpactRejected.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (auto* actor = Find(target); actor != nullptr) actor->lastSend = 0U;
        gImpactApplied.fetch_add(1U, std::memory_order_relaxed);
    }
}

void PumpOnEngineThread() noexcept {
    if (!Installed() || !EngineThread()) return;
    // Generation cleanup must precede dereferencing any navigation owner.
    if (gGeneration != gGenerations->CurrentGeneration()) navigation::Reset(false);
    navigation::Pump(GetTickCount64());
    // Own native path-query references independently of Characters. Maintain
    // them before taking the IPC mailbox mutex; cleanup can invoke native code.
    PumpHostNavigationContext();
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
        for (auto& queue : gIncomingOutcomes) queue.clear();
        gLastQueuedOutcome.fill({}); gIncomingOutcomeCount = 0U;
        {
            std::lock_guard impactLock(gImpactMailboxMutex);
            gOutgoingImpactRead = gOutgoingImpactWrite = 0U;
            gIncomingImpactRead = gIncomingImpactWrite = 0U;
            gImpactSequence = 0U;
            gLastImpactSequence.fill({});
        }
    }
    if (Stable()) RefreshPairBindings();
    PumpHostImpacts();
    // Host publication must survive AI-manager dormancy (vent/backstage).
    // CharacterManager invokes this pump independently of each NPC brain.
    PumpHostAuthority();

    if (!Stable() || !Client()) return;
    const auto now = GetTickCount64();
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (!gIncomingUsed[i]) continue;
        auto& outcomes = gIncomingOutcomes[i];
        if (outcomes.empty() && now - gIncomingAt[i] > kTimeout) {
            gIncomingUsed[i] = false; gMissing.fetch_add(1U, std::memory_order_relaxed); continue;
        }
        auto value = outcomes.empty() ? gIncoming[i] : outcomes.front();
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
        // A later event cannot replace a pair whose irreversible native result
        // has not yet been admitted (placement/binding may need another frame).
        if (actor.remoteBirth == value.birth && actor.desired.outcome.revision != 0U) {
            OutcomeBinding binding{};
            if (!LoadOutcome(actor.character, ParticipantForPeer(actor.desired.interactionTargetPeer), binding) ||
                !binding.claimed) {
                // Keep the admission pose/frontstage until its worker consumes
                // the result. A same-pair health update may refine authority,
                // but cannot withdraw/rebuild the source before admission.
                if (value.interactionPairRevision == actor.desired.interactionPairRevision &&
                    value.outcome.revision > actor.desired.outcome.revision) {
                    actor.desired.outcome = value.outcome;
                    static_cast<void>(PublishOutcomeBinding(actor, ParticipantForPeer(value.interactionTargetPeer)));
                }
                actor.lastReceive = now;
                continue;
            }
            const bool samePair = value.interactionPairRevision == actor.desired.interactionPairRevision;
            const auto& result = samePair && value.outcome.revision > binding.state.outcome.revision
                ? value.outcome : binding.state.outcome;
            if (actor.interactionOwnsCycle && binding.claimed && !binding.lateFinishClaimed && result.kind == 0U &&
                (result.flags & (OutcomeNativeDeath | OutcomeTerminal)) == (OutcomeNativeDeath | OutcomeTerminal)) {
                // A late native death must consume its control/finish block
                // before a following vent/task snapshot destroys the source.
                actor.desired.outcome = result;
                static_cast<void>(PublishOutcomeBinding(actor, ParticipantForPeer(actor.desired.interactionTargetPeer)));
                actor.lastReceive = now;
                continue;
            }
        }
        // Retain this latest snapshot until the worker admission has returned;
        // birth/withdraw/task cleanup must not reconstruct its source cycle.
        if (PairSourceBusy(actor.character)) continue;
        if (value.birth == actor.remoteBirth && ClientNativeVictimOwnsPair(actor.character)) {
            // Retain the latest Host snapshot until this exact fatal victim
            // finishes, including an observer's remote human representation.
            // No task/lifecycle cleanup may reuse the paired source meanwhile.
            actor.lastReceive = now;
            gPairedVictimSnapshotsDeferred.fetch_add(1U, std::memory_order_relaxed);
            if (!LocalClientVictim(ParticipantForPeer(actor.desired.interactionTargetPeer)))
                gPairedObserverSnapshotsDeferred.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (!outcomes.empty()) {
            outcomes.pop_front(); --gIncomingOutcomeCount;
            OutcomeBinding binding{};
            const bool consumedPair = actor.remoteBirth == value.birth &&
                actor.desired.interactionPairRevision == value.interactionPairRevision &&
                actor.desired.interactionTargetPeer == value.interactionTargetPeer &&
                LoadOutcome(actor.character, ParticipantForPeer(value.interactionTargetPeer), binding) && binding.claimed;
            if (consumedPair) {
                // Health admission remains ordered and exactly once. Progress
                // for that already consumed pair need not replay old world
                // poses after a local fatal graph deferred several snapshots.
                while (!outcomes.empty() && outcomes.front().birth == value.birth &&
                    outcomes.front().interactionPairRevision == value.interactionPairRevision &&
                    outcomes.front().interactionTargetPeer == value.interactionTargetPeer) {
                    value = outcomes.front(); outcomes.pop_front(); --gIncomingOutcomeCount;
                    gOutcomeSnapshotsCoalesced.fetch_add(1U, std::memory_order_relaxed);
                }
                const auto& latest = gIncoming[i];
                if (latest.birth == value.birth && latest.interactionPairRevision == value.interactionPairRevision &&
                    latest.interactionTargetPeer == value.interactionTargetPeer && latest.revision > value.revision &&
                    latest.outcome.revision >= value.outcome.revision) {
                    value = latest;
                    gOutcomeSnapshotsCoalesced.fetch_add(1U, std::memory_order_relaxed);
                }
                if (actor.interactionOwnsCycle && !binding.lateFinishClaimed && value.outcome.kind == 0U &&
                    (value.outcome.flags & (OutcomeNativeDeath | OutcomeTerminal)) == (OutcomeNativeDeath | OutcomeTerminal)) {
                    // Coalescing can discover a late kill beyond the front
                    // event. Admit that result without its newer vent/pose;
                    // the same native finish lease checked above still wins.
                    actor.desired.outcome = value.outcome;
                    static_cast<void>(PublishOutcomeBinding(actor, ParticipantForPeer(value.interactionTargetPeer)));
                    actor.lastReceive = now;
                    continue;
                }
            }
        }
        gIncomingUsed[i] = !outcomes.empty() || gIncoming[i].birth > value.birth ||
            (gIncoming[i].birth == value.birth && gIncoming[i].revision > value.revision);
        if (value.birth < actor.remoteBirth ||
            (value.birth == actor.remoteBirth && value.revision < actor.remoteRevision)) continue;

        if (actor.pairPlacementPending && !PairPlacementMatchesState(actor, value))
            ClearPairPlacement(actor);
        const bool hadRemote = actor.remoteBirth != 0U;
        const bool birthChanged = value.birth != actor.remoteBirth;
        const bool taskChanged = hadRemote && value.roleTaskRevision != actor.desired.roleTaskRevision;
        const bool actionChanged = hadRemote && value.actionRevision != actor.desired.actionRevision;
        const bool lifecycleChanged = hadRemote && value.lifecycleRevision != actor.desired.lifecycleRevision;
        const bool poseMissing = (value.stateFlags & PoseValid) == 0U;
        const auto persistent = LifecycleWithdrawn | LifecycleWithdrawState2 | LifecycleHostDormant |
            LifecycleCharacterBackstage;
        std::uint8_t localHidden{};
        const bool visibilityMismatch = Read(actor.character + 0x2DAU, localHidden) &&
            (((value.lifecycleFlags & LifecycleCharacterBackstage) != 0U && localHidden == 0U) ||
             (!ProxyWithdrawn(value) && localHidden != 0U));
        // Compare persistent values even if a coalesced packet has no new
        // event revision. A local vent callback can also hide the proxy later.
        const bool pairedPlacement = !birthChanged && !ProxyWithdrawn(value) && actor.interactionOwnsCycle &&
            actor.completedPairRevision == value.interactionPairRevision &&
            (value.interactionFlags & (InteractionCycleActive | InteractionSourceCycleRetained)) != 0U;
        // A paired outro can temporarily place/hide a body. While Host still
        // retains this source, update metadata only; local native presentation
        // owns it. Persistent visibility is reconciled on actual source release.
        const bool persistentChanged = ((value.lifecycleFlags ^ actor.desired.lifecycleFlags) & persistent) != 0U;
        if (pairedPlacement && (persistentChanged || visibilityMismatch)) actor.placementPending = true;
        if (pairedPlacement && (taskChanged || actionChanged)) actor.roleTaskCleanupPending = true;
        const bool placementChanged = !pairedPlacement && (birthChanged || actor.placementPending || visibilityMismatch ||
            ((value.lifecycleFlags ^ actor.desired.lifecycleFlags) & persistent) != 0U ||
            (lifecycleChanged && (value.lifecycleFlags & LifecycleSpatialDiscontinuity) != 0U));
        bool lifecycleApplied = false;

        if (birthChanged) {
            if (actor.interactionOwnsCycle)
                static_cast<void>(character_direct_step_runtime::FinishReplicatedInteraction(actor.character));
            actor.interactionOwnsCycle = false;
            ClearObserverReleaseMemory(actor);
            actor.completedInteractionRevision = 0U;
            actor.completedPairRevision = 0U;
            actor.completedTransitionRevision = 0U;
            actor.resolvedPairRevision = 0U;
            actor.completedExecutionRevision = 0U;
        actor.replayedExecutionRevision = 0U;
        actor.lastExecutionAttempt = 0U;
        }

        // BT and RoleTask remain Host-authoritative decision epochs. Clients do
        // not run an autonomous alternative tree/task: on a Host transition,
        // quiesce any stale local execution before replaying the new resolved
        // native locomotion intention. This is an execution barrier, not C++
        // task-object deserialization.
        if (placementChanged) {
            if (!QuiesceProxyExecution(actor)) {
                actor.faulted = true;
                gFaults.fetch_add(1U, std::memory_order_relaxed);
            }
        } else if (!pairedPlacement && (taskChanged || actionChanged || actor.roleTaskCleanupPending)) {
            const bool tree = actor.tree == 0U || SuspendExistingTree(actor);
            const bool task = ClearRoleTaskExecutor(actor);
            if (!tree || !task) actor.faulted = true;
            else {
                actor.roleTaskCleanupPending = false;
                gExecutionBarriers.fetch_add(1U, std::memory_order_relaxed);
            }
        } else if (!pairedPlacement && poseMissing) {
            static_cast<void>(Release(actor));
        }

        // Withdraw state remains separate from Character backstage: in the
        // failing .170 run the Alien's withdraw state stayed zero in the vents.
        // Return through native frontstage before placement or pair admission.
        if (!actor.faulted && placementChanged) {
            actor.placementPending = true;
            if (!ApplyAuthoritativeWithdraw(actor, value) ||
                !ApplyAuthoritativeBackstage(actor, value)) {
                gLifecycleRetries.fetch_add(1U, std::memory_order_relaxed);
            } else if (ProxyWithdrawn(value)) {
                actor.placementPending = false;
                lifecycleApplied = true;
            } else if ((value.stateFlags & PoseValid) != 0U) {
                if (SnapAuthoritativePose(actor, value)) {
                    actor.placementPending = false;
                    lifecycleApplied = true;
                } else {
                    // Keep authority/transport live and retry against the
                    // newest pose. An unready body is not a permanent AI fault.
                    gLifecycleRetries.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        }
        if (lifecycleApplied) gLifecycleApplied.fetch_add(1U, std::memory_order_relaxed);

        const bool newOutcome = value.outcome.revision != 0U &&
            (value.interactionPairRevision != actor.desired.interactionPairRevision ||
             value.outcome.revision != actor.desired.outcome.revision);
        actor.remoteBirth = value.birth; actor.remoteRevision = value.revision;
        actor.desired = value; actor.lastReceive = now;
        RememberPresentationPose(actor, value, now);
        if (value.outcome.revision != 0U)
            static_cast<void>(PublishOutcomeBinding(actor, ParticipantForPeer(value.interactionTargetPeer)));
        if (newOutcome) SamplePresentation(actor, now, 3U);
        actor.completedActionRevision = value.actionRevision;
        actor.completedRoleTaskRevision = value.roleTaskRevision;
        if (lifecycleApplied) actor.completedLifecycleRevision = value.lifecycleRevision;
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
        navigation::Cancel(c);
        *actor = {};
        gProvisionalPlayerActorsRetired.fetch_add(1U, std::memory_order_relaxed);
    }
}
void ForgetCharacter(const std::uintptr_t c) noexcept {
    InvalidatePairCharacter(c);
    if (EngineThread()) navigation::Cancel(c);
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
    for (auto& navigation : gParticipantNavigation)
        if (navigation.initialized && navigation.character == c)
            static_cast<void>(ReleaseParticipantNavigation(navigation));
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
    for (auto& navigation : gParticipantNavigation)
        if (navigation.initialized && navigation.runtimeId == id)
            static_cast<void>(ReleaseParticipantNavigation(navigation));
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
    for (auto& queue : gIncomingOutcomes) queue.clear();
    gLastQueuedOutcome.fill({}); gIncomingOutcomeCount = 0U;
    {
        std::lock_guard impactLock(gImpactMailboxMutex);
        gOutgoingImpactRead = gOutgoingImpactWrite = 0U;
        gIncomingImpactRead = gIncomingImpactWrite = 0U;
        gImpactSequence = 0U;
        gLastImpactSequence.fill({});
    }
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
    value.poseRejected = gPoseRejected.load();
    value.intentCaptured = gIntentCaptured.load(); value.intentApplied = gIntentApplied.load();
    value.intentRejected = gIntentRejected.load(); value.intentRecoveries = gIntentRecoveries.load();
    value.actionCaptured = gActionCaptured.load();
    value.actionTransitions = gActionTransitions.load();
    value.executionCaptured = gExecutionCaptured.load();
    value.executionTransitions = gExecutionTransitions.load();
    value.executionReplayed = gExecutionReplayed.load();
    value.executionReplayRejected = gExecutionReplayRejected.load();
    value.roleTaskCaptured = gRoleTaskCaptured.load();
    value.roleTaskTransitions = gRoleTaskTransitions.load();
    value.lifecycleCaptured = gLifecycleCaptured.load();
    value.lifecycleApplied = gLifecycleApplied.load();
    value.lifecycleSnaps = gLifecycleSnaps.load();
    value.backstageCaptured = gBackstageCaptured.load();
    value.backstageTransitions = gBackstageTransitions.load();
    value.backstageApplied = gBackstageApplied.load();
    value.backstageRejected = gBackstageRejected.load();
    value.placementVerified = gPlacementVerified.load();
    value.placementRejected = gPlacementRejected.load();
    value.lifecycleRetries = gLifecycleRetries.load();
    value.executionBarriers = gExecutionBarriers.load();
    value.impactCaptured = gImpactCaptured.load();
    value.impactQueued = gImpactQueued.load();
    value.impactApplied = gImpactApplied.load();
    value.impactRejected = gImpactRejected.load();
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
    value.pairedObserverCompletions = gPairedObserverCompletions.load();
    value.pairedObserverReleaseAttempts = gPairedObserverReleaseAttempts.load();
    value.pairedObserverReleases = gPairedObserverReleases.load();
    value.pairedObserverReleaseRejected = gPairedObserverReleaseRejected.load();
    value.pairedObserverPairReopenSuppressed = gPairedObserverPairReopenSuppressed.load();
    value.pairedObserverNeutralHolds = gPairedObserverNeutralHolds.load();
    value.pairedObserverHandoffResumed = gPairedObserverHandoffResumed.load();
    value.pairedVictimTransitions = gPairedVictimTransitions.load();
    value.pairedVictimTransitionRejected = gPairedVictimTransitionRejected.load();
    value.pairedVictimInitializeAborted = gPairedVictimInitializeAborted.load();
    value.pairedVictimEventsRejected = gPairedVictimEventsRejected.load();
    value.pairedVictimDuplicates = gPairedVictimDuplicates.load();
    value.pairedVictimBusy = gPairedVictimBusy.load();
    value.pairedVictimNativeDispatches = gPairedVictimNativeDispatches.load();
    value.pairedVictimNativeStarted = gPairedVictimNativeStarted.load();
    value.pairedVictimAvailabilityOverrides = gPairedVictimAvailabilityOverrides.load();
    value.pairedPlayerPreparationRemaps = gPairedPlayerPreparationRemaps.load();
    value.pairedPlayerCategoryRemaps = gPairedPlayerCategoryRemaps.load();
    value.pairedVictimLocalDispatches = gPairedVictimLocalDispatches.load();
    value.pairedVictimLocalStarted = gPairedVictimLocalStarted.load();
    value.pairedVictimConvergenceDetached = gPairedVictimConvergenceDetached.load();
    value.pairedVictimSnapshotsDeferred = gPairedVictimSnapshotsDeferred.load();
    value.pairedVictimHealthContinuations = gPairedVictimHealthContinuations.load();
    value.pairedVictimWorkerDispatches = gPairedVictimWorkerDispatches.load();
    value.pairedVictimLocalAvailabilityOverrides = gPairedVictimLocalAvailabilityOverrides.load();
    value.pairedDeadTargetRejected = gPairedDeadTargetRejected.load();
    value.pairedRecoveryDeferred = gPairedRecoveryDeferred.load();
    value.pairedSourcePlacements = gPairedSourcePlacements.load();
    value.pairedSourcePlacementRejected = gPairedSourcePlacementRejected.load();
    value.pairedSourcePlacementDeferred = gPairedSourcePlacementDeferred.load();
    value.pairedSourcePlacementResumed = gPairedSourcePlacementResumed.load();
    value.pairedSourcePlacementCancelled = gPairedSourcePlacementCancelled.load();
    value.pairedSourcePlacementExpired = gPairedSourcePlacementExpired.load();
    value.pairedSourcePoseSuppressed = gPairedSourcePoseSuppressed.load();
    value.pairedObserverSnapshotsDeferred = gPairedObserverSnapshotsDeferred.load();
    value.presentationSamples = gPresentationSamples.load();
    value.presentationDropped = gPresentationDropped.load();
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
    value.targetSuccessionSamples = gTargetSuccessionSamples.load();
    value.targetSuccessionDropped = gTargetSuccessionDropped.load();
    value.postDeathSuccessionsPreserved = gPostDeathSuccessionsPreserved.load();
    value.postDeathArbitrationDeferrals = gPostDeathArbitrationDeferrals.load();
    value.postDeathDecisionUncertain = gPostDeathDecisionUncertain.load();
    value.deadTargetRecoveries = gDeadTargetRecoveries.load();
    value.aiPlayerQueryRemaps = gAiPlayerQueryRemaps.load();
    value.aiPlayerSlotRemaps = gAiPlayerSlotRemaps.load();
    value.aiPlayerSlotRestoreFailures = gAiPlayerSlotRestoreFailures.load();
    value.postDeathMeleeSelectionChecks = gPostDeathMeleeSelectionChecks.load();
    value.postDeathMeleeSelectionRejects = gPostDeathMeleeSelectionRejects.load();
    value.postDeathMeleeSelectionWriteFailures = gPostDeathMeleeSelectionWriteFailures.load();
    value.lastPostDeathRetiredPeer = gLastPostDeathRetiredPeer.load();
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
    value.lastExecutionTypeRva = gLastExecutionTypeRva.load();
    value.lastExecutionState = gLastExecutionState.load();
    value.lastExecutionFlags = gLastExecutionFlags.load();
    value.lastRejectedExecutionTypeRva = gLastRejectedExecutionTypeRva.load();
    value.lastRejectedExecutionState = gLastRejectedExecutionState.load();
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
    const auto nav = navigation::Snapshot();
    diagnostics.Log("info", "aisync_navigation", "Host resolved goals executed by local retail navigation; v11 wire.",
        {{"captured", Diagnostics::Unsigned(nav.captured), true},
         {"submitted", Diagnostics::Unsigned(nav.submitted), true},
         {"updated", Diagnostics::Unsigned(nav.updated), true},
         {"completed", Diagnostics::Unsigned(nav.completed), true},
         {"failed", Diagnostics::Unsigned(nav.failed), true},
         {"cancelled", Diagnostics::Unsigned(nav.cancelled), true},
         {"expired", Diagnostics::Unsigned(nav.expired), true},
         {"pairedGraphDropped", Diagnostics::Unsigned(gPairedGraphDropped.load()), true}});
    for (;;) {
        PairedGraphProbe sample{};
        {
            std::unique_lock lock(gPairedGraphMutex, std::try_to_lock);
            if (!lock.owns_lock() || gPairedGraphRead == gPairedGraphWrite) break;
            sample = gPairedGraphProbes[gPairedGraphRead++ % kParticipantProbeRing];
        }
        diagnostics.Log("info", "aisync_paired_graph", "Retail callback trace: side 1=source event, 2=victim event, 3=source finish; phase 0=entry, 1=return. Observation only.",
            {{"sampleTick", Diagnostics::Unsigned(sample.tick), true},
             {"role", Diagnostics::Unsigned(sample.role), true},
             {"worldGeneration", Diagnostics::Unsigned(sample.generation), true},
             {"nativeEventId", Diagnostics::Unsigned(sample.event), true},
             {"side", Diagnostics::Unsigned(sample.side), true},
             {"phase", Diagnostics::Unsigned(sample.phase), true},
             {"sourceA", Diagnostics::Unsigned(sample.sourceA), true},
             {"sourceB", Diagnostics::Unsigned(sample.sourceB), true},
             {"victimA", Diagnostics::Unsigned(sample.victimA), true},
             {"victimB", Diagnostics::Unsigned(sample.victimB), true},
             {"peer", Diagnostics::Unsigned(sample.peer), true},
             {"birth", Diagnostics::Unsigned(sample.birth), true},
             {"pairRevision", Diagnostics::Unsigned(sample.pairRevision), true},
             {"sourceState", Diagnostics::Unsigned(sample.sourceState), true},
             {"victimState", Diagnostics::Unsigned(sample.victimState), true},
             {"sourceStage", Diagnostics::Unsigned(sample.sourceStage), true},
             {"sourceFinished", Diagnostics::Unsigned(sample.sourceFinished), true},
             {"victimEnded", Diagnostics::Unsigned(sample.victimEnded), true},
             {"victimAcquired", Diagnostics::Unsigned(sample.victimAcquired), true},
             {"readable", Diagnostics::Boolean(sample.readable), true}});
    }
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
         {"intentCaptured", Diagnostics::Unsigned(ai.intentCaptured), true},
         {"intentApplied", Diagnostics::Unsigned(ai.intentApplied), true},
         {"intentRejected", Diagnostics::Unsigned(ai.intentRejected), true},
         {"intentRecoveries", Diagnostics::Unsigned(ai.intentRecoveries), true},
         {"poseInterpolated", Diagnostics::Unsigned(gPoseInterpolated.load(std::memory_order_relaxed)), true},
         {"pairedHostCycleWaits", Diagnostics::Unsigned(gPairedHostCycleWaits.load(std::memory_order_relaxed)), true},
         {"outcomeSnapshotsCoalesced", Diagnostics::Unsigned(gOutcomeSnapshotsCoalesced.load(std::memory_order_relaxed)), true},
         {"backstageEligibilityOverrides", Diagnostics::Unsigned(gBackstageEligibilityOverrides.load(std::memory_order_relaxed)), true},
         {"backstageNavigationPlans", Diagnostics::Unsigned(gBackstageNavigationPlans.load(std::memory_order_relaxed)), true},
         {"backstageNavigationUpdates", Diagnostics::Unsigned(gBackstageNavigationUpdates.load(std::memory_order_relaxed)), true},
         {"backstageNavigationRejected", Diagnostics::Unsigned(gBackstageNavigationRejected.load(std::memory_order_relaxed)), true},
         {"backstageDeadTargetsRejected", Diagnostics::Unsigned(gBackstageDeadTargetsRejected.load(std::memory_order_relaxed)), true},
         {"backstageHoldContexts", Diagnostics::Unsigned(gBackstageHoldContexts.load(std::memory_order_relaxed)), true},
         {"backstageSurvivorRearmAttempts", Diagnostics::Unsigned(gBackstageSurvivorRearmAttempts.load(std::memory_order_relaxed)), true},
         {"backstageSurvivorRearms", Diagnostics::Unsigned(gBackstageSurvivorRearms.load(std::memory_order_relaxed)), true},
         {"backstageSurvivorRearmRestricted", Diagnostics::Unsigned(gBackstageSurvivorRearmRestricted.load(std::memory_order_relaxed)), true},
         {"backstageSurvivorRearmFailures", Diagnostics::Unsigned(gBackstageSurvivorRearmFailures.load(std::memory_order_relaxed)), true},
         {"lastBackstageSurvivorPeer", Diagnostics::Unsigned(gLastBackstageSurvivorPeer.load(std::memory_order_relaxed)), true},
         {"poseRejected", Diagnostics::Unsigned(ai.poseRejected), true},
         {"actionCaptured", Diagnostics::Unsigned(ai.actionCaptured), true},
         {"actionTransitions", Diagnostics::Unsigned(ai.actionTransitions), true},
         {"executionCaptured", Diagnostics::Unsigned(ai.executionCaptured), true},
         {"executionTransitions", Diagnostics::Unsigned(ai.executionTransitions), true},
         {"executionReplayed", Diagnostics::Unsigned(ai.executionReplayed), true},
         {"executionReplayRejected", Diagnostics::Unsigned(ai.executionReplayRejected), true},
         {"lastExecutionTypeRva", Diagnostics::Unsigned(ai.lastExecutionTypeRva), true},
         {"lastExecutionState", Diagnostics::Unsigned(ai.lastExecutionState), true},
         {"lastExecutionFlags", Diagnostics::Unsigned(ai.lastExecutionFlags), true},
         {"lastRejectedExecutionTypeRva", Diagnostics::Unsigned(ai.lastRejectedExecutionTypeRva), true},
         {"lastRejectedExecutionState", Diagnostics::Unsigned(ai.lastRejectedExecutionState), true},
         {"roleTaskCaptured", Diagnostics::Unsigned(ai.roleTaskCaptured), true},
         {"roleTaskTransitions", Diagnostics::Unsigned(ai.roleTaskTransitions), true},
         {"lifecycleCaptured", Diagnostics::Unsigned(ai.lifecycleCaptured), true},
         {"lifecycleApplied", Diagnostics::Unsigned(ai.lifecycleApplied), true},
         {"lifecycleSnaps", Diagnostics::Unsigned(ai.lifecycleSnaps), true},
         {"backstageCaptured", Diagnostics::Unsigned(ai.backstageCaptured), true},
         {"backstageTransitions", Diagnostics::Unsigned(ai.backstageTransitions), true},
         {"backstageApplied", Diagnostics::Unsigned(ai.backstageApplied), true},
         {"backstageRejected", Diagnostics::Unsigned(ai.backstageRejected), true},
         {"placementVerified", Diagnostics::Unsigned(ai.placementVerified), true},
         {"placementRejected", Diagnostics::Unsigned(ai.placementRejected), true},
         {"lifecycleRetries", Diagnostics::Unsigned(ai.lifecycleRetries), true},
         {"executionBarriers", Diagnostics::Unsigned(ai.executionBarriers), true},
         {"impactCaptured", Diagnostics::Unsigned(ai.impactCaptured), true},
         {"impactQueued", Diagnostics::Unsigned(ai.impactQueued), true},
         {"impactApplied", Diagnostics::Unsigned(ai.impactApplied), true},
         {"impactRejected", Diagnostics::Unsigned(ai.impactRejected), true},
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
         {"pairedObserverCompletions", Diagnostics::Unsigned(ai.pairedObserverCompletions), true},
         {"pairedObserverReleaseAttempts", Diagnostics::Unsigned(ai.pairedObserverReleaseAttempts), true},
         {"pairedObserverReleases", Diagnostics::Unsigned(ai.pairedObserverReleases), true},
         {"pairedObserverReleaseRejected", Diagnostics::Unsigned(ai.pairedObserverReleaseRejected), true},
         {"pairedObserverPairReopenSuppressed", Diagnostics::Unsigned(ai.pairedObserverPairReopenSuppressed), true},
         {"pairedObserverNeutralHolds", Diagnostics::Unsigned(ai.pairedObserverNeutralHolds), true},
         {"pairedObserverHandoffResumed", Diagnostics::Unsigned(ai.pairedObserverHandoffResumed), true},
         {"pairedVictimTransitions", Diagnostics::Unsigned(ai.pairedVictimTransitions), true},
         {"pairedVictimTransitionRejected", Diagnostics::Unsigned(ai.pairedVictimTransitionRejected), true},
         {"pairedVictimInitializeAborted", Diagnostics::Unsigned(ai.pairedVictimInitializeAborted), true},
         {"pairedVictimEventsRejected", Diagnostics::Unsigned(ai.pairedVictimEventsRejected), true},
         {"pairedVictimDuplicates", Diagnostics::Unsigned(ai.pairedVictimDuplicates), true},
         {"pairedVictimBusy", Diagnostics::Unsigned(ai.pairedVictimBusy), true},
         {"pairedVictimNativeDispatches", Diagnostics::Unsigned(ai.pairedVictimNativeDispatches), true},
         {"pairedVictimNativeStarted", Diagnostics::Unsigned(ai.pairedVictimNativeStarted), true},
         {"nativeOutcomeCaptured", Diagnostics::Unsigned(gOutcomeCaptured.load()), true},
         {"nativeOutcomeApplied", Diagnostics::Unsigned(gOutcomeApplied.load()), true},
         {"nativeOutcomeRejected", Diagnostics::Unsigned(gOutcomeRejected.load()), true},
         {"nativeOutcomeDuplicates", Diagnostics::Unsigned(gOutcomeDuplicates.load()), true},
         {"pairedVictimAvailabilityOverrides", Diagnostics::Unsigned(ai.pairedVictimAvailabilityOverrides), true},
         {"pairedPlayerPreparationRemaps", Diagnostics::Unsigned(ai.pairedPlayerPreparationRemaps), true},
         {"pairedPlayerCategoryRemaps", Diagnostics::Unsigned(ai.pairedPlayerCategoryRemaps), true},
         {"pairedVictimLocalDispatches", Diagnostics::Unsigned(ai.pairedVictimLocalDispatches), true},
         {"pairedVictimLocalStarted", Diagnostics::Unsigned(ai.pairedVictimLocalStarted), true},
         {"pairedVictimConvergenceDetached", Diagnostics::Unsigned(ai.pairedVictimConvergenceDetached), true},
         {"pairedVictimSnapshotsDeferred", Diagnostics::Unsigned(ai.pairedVictimSnapshotsDeferred), true},
         {"pairedVictimHealthContinuations", Diagnostics::Unsigned(ai.pairedVictimHealthContinuations), true},
         {"pairedVictimWorkerDispatches", Diagnostics::Unsigned(ai.pairedVictimWorkerDispatches), true},
         {"pairedVictimLocalAvailabilityOverrides", Diagnostics::Unsigned(ai.pairedVictimLocalAvailabilityOverrides), true},
         {"pairedDeadTargetRejected", Diagnostics::Unsigned(ai.pairedDeadTargetRejected), true},
         {"pairedRecoveryDeferred", Diagnostics::Unsigned(ai.pairedRecoveryDeferred), true},
         {"pairedSourcePlacements", Diagnostics::Unsigned(ai.pairedSourcePlacements), true},
         {"pairedSourcePlacementRejected", Diagnostics::Unsigned(ai.pairedSourcePlacementRejected), true},
         {"pairedSourcePlacementDeferred", Diagnostics::Unsigned(ai.pairedSourcePlacementDeferred), true},
         {"pairedSourcePlacementResumed", Diagnostics::Unsigned(ai.pairedSourcePlacementResumed), true},
         {"pairedSourcePlacementCancelled", Diagnostics::Unsigned(ai.pairedSourcePlacementCancelled), true},
         {"pairedSourcePlacementExpired", Diagnostics::Unsigned(ai.pairedSourcePlacementExpired), true},
         {"pairedSourcePoseSuppressed", Diagnostics::Unsigned(ai.pairedSourcePoseSuppressed), true},
         {"pairedObserverSnapshotsDeferred", Diagnostics::Unsigned(ai.pairedObserverSnapshotsDeferred), true},
         {"presentationSamples", Diagnostics::Unsigned(ai.presentationSamples), true},
         {"presentationDropped", Diagnostics::Unsigned(ai.presentationDropped), true},
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
         {"targetSuccessionSamples", Diagnostics::Unsigned(ai.targetSuccessionSamples), true},
         {"targetSuccessionDropped", Diagnostics::Unsigned(ai.targetSuccessionDropped), true},
         {"postDeathSuccessionsPreserved", Diagnostics::Unsigned(ai.postDeathSuccessionsPreserved), true},
         {"postDeathArbitrationDeferrals", Diagnostics::Unsigned(ai.postDeathArbitrationDeferrals), true},
         {"postDeathDecisionUncertain", Diagnostics::Unsigned(ai.postDeathDecisionUncertain), true},
         {"deadTargetRecoveries", Diagnostics::Unsigned(ai.deadTargetRecoveries), true},
         {"aiPlayerQueryRemaps", Diagnostics::Unsigned(ai.aiPlayerQueryRemaps), true},
         {"aiPlayerSlotRemaps", Diagnostics::Unsigned(ai.aiPlayerSlotRemaps), true},
         {"aiPlayerSlotRestoreFailures", Diagnostics::Unsigned(ai.aiPlayerSlotRestoreFailures), true},
         {"postDeathMeleeSelectionChecks", Diagnostics::Unsigned(ai.postDeathMeleeSelectionChecks), true},
         {"postDeathMeleeSelectionRejects", Diagnostics::Unsigned(ai.postDeathMeleeSelectionRejects), true},
         {"postDeathMeleeSelectionWriteFailures", Diagnostics::Unsigned(ai.postDeathMeleeSelectionWriteFailures), true},
         {"lastPostDeathRetiredPeer", Diagnostics::Unsigned(ai.lastPostDeathRetiredPeer), true},
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
        const auto read = gPresentationProbeRead.load(std::memory_order_relaxed);
        if (read == gPresentationProbeWrite.load(std::memory_order_acquire)) break;
        const auto sample = gPresentationProbes[read % kParticipantProbeRing];
        gPresentationProbeRead.store(read + 1U, std::memory_order_release);
        diagnostics.Log("info", "aisync_presentation", "Exact source/victim/local identities, health and paired result; phase 0=periodic, 1=before admission, 2=after transition, 3=resolved outcome, 4=remote observer completion, 5=remote observer release, 6=observer handoff resume. Terminal health alone never completes a paired graph.",
            {{"sessionCookie", Diagnostics::Unsigned(sample.sessionCookie), true},
             {"nativeGeneration", Diagnostics::Unsigned(sample.nativeGeneration), true},
             {"worldGeneration", Diagnostics::Unsigned(sample.worldGeneration), true},
             {"sampleTick", Diagnostics::Unsigned(sample.tick), true},
             {"role", Diagnostics::Unsigned(sample.role), true},
             {"phase", Diagnostics::Unsigned(sample.phase), true},
             {"entityA", Diagnostics::Unsigned(sample.entityA), true},
             {"entityB", Diagnostics::Unsigned(sample.entityB), true},
             {"sourceCharacter", Diagnostics::Pointer(reinterpret_cast<const void*>(sample.sourceCharacter))},
             {"sourceRuntimeId", Diagnostics::Unsigned(sample.sourceRuntimeId), true},
             {"birth", Diagnostics::Unsigned(sample.birth), true},
             {"revision", Diagnostics::Unsigned(sample.revision), true},
             {"pairRevision", Diagnostics::Unsigned(sample.pairRevision), true},
             {"movementOwner", Diagnostics::Unsigned(sample.movementOwner), true},
             {"navigationRevision", Diagnostics::Unsigned(sample.navigation.revision), true},
             {"navigationRequestRevision", Diagnostics::Unsigned(sample.navigation.requestRevision), true},
             {"navigationFlags", Diagnostics::Unsigned(sample.navigation.flags), true},
             {"navigationGoalX", std::to_string(sample.navigation.position[0]), true},
             {"navigationGoalY", std::to_string(sample.navigation.position[1]), true},
             {"navigationGoalZ", std::to_string(sample.navigation.position[2]), true},
             {"peer", Diagnostics::Unsigned(sample.peer), true},
             {"victimCharacter", Diagnostics::Pointer(reinterpret_cast<const void*>(sample.victimCharacter))},
             {"victimRuntimeId", Diagnostics::Unsigned(sample.victimRuntimeId), true},
             {"victimEntityA", Diagnostics::Unsigned(sample.victimEntityA), true},
             {"victimEntityB", Diagnostics::Unsigned(sample.victimEntityB), true},
             {"victimLocal", Diagnostics::Boolean(sample.victimLocal), true},
             {"victimRepresentation", Diagnostics::Boolean(sample.victimRepresentation), true},
             {"localCharacter", Diagnostics::Pointer(reinterpret_cast<const void*>(sample.localCharacter))},
             {"localRuntimeId", Diagnostics::Unsigned(sample.localRuntimeId), true},
             {"localHealthReadable", Diagnostics::Boolean(sample.localHealthReadable), true},
             {"localHp", sample.localHealthReadable ? Diagnostics::Unsigned(sample.localHp) : "null", true},
             {"localHealthState", sample.localHealthReadable ? Diagnostics::Unsigned(sample.localHealthState) : "null", true},
             {"victimCycleType", Diagnostics::Unsigned(sample.victimCycleType), true},
             {"victimCycleState", Diagnostics::Unsigned(sample.victimCycleState), true},
             {"victimAcquired", Diagnostics::Unsigned(sample.victimAcquired), true},
             {"victimEnded", Diagnostics::Unsigned(sample.victimEnded), true},
             {"victimInteractive", Diagnostics::Unsigned(sample.victimInteractive), true},
             {"outcomeRevision", Diagnostics::Unsigned(sample.outcome.revision), true},
             {"outcomeFlags", Diagnostics::Unsigned(sample.outcome.flags), true},
             {"outcomeDefinition", Diagnostics::Unsigned(sample.outcome.definition), true},
             {"outcomeKind", Diagnostics::Unsigned(sample.outcome.kind), true},
             {"outcomeHpBefore", Diagnostics::Unsigned(sample.outcome.hpBefore), true},
             {"outcomeHpPrepared", Diagnostics::Unsigned(sample.outcome.hpPrepared), true},
             {"outcomeHpReaction", Diagnostics::Unsigned(sample.outcome.hpReaction), true},
             {"outcomeHealthReaction", Diagnostics::Unsigned(sample.outcome.healthReaction), true},
             {"lifecycleFlags", Diagnostics::Unsigned(sample.lifecycleFlags), true},
             {"interactionFlags", Diagnostics::Unsigned(sample.interactionFlags), true},
             {"cycleType", Diagnostics::Unsigned(sample.cycleType), true},
             {"cycleState", Diagnostics::Unsigned(sample.cycleState), true},
             {"stage", Diagnostics::Unsigned(sample.stage), true},
             {"withdrawState", Diagnostics::Unsigned(sample.withdrawState), true},
             {"hidden", Diagnostics::Unsigned(sample.hidden), true},
             {"enabled", Diagnostics::Unsigned(sample.enabled), true},
             {"positionReadable", Diagnostics::Boolean(sample.positionReadable), true},
             {"victimReadable", Diagnostics::Boolean(sample.victimReadable), true},
             {"victimHealthReadable", Diagnostics::Boolean(sample.victimHealthReadable), true},
             {"victimHp", sample.victimHealthReadable ? Diagnostics::Unsigned(sample.victimHp) : "null", true},
             {"victimHealthState", sample.victimHealthReadable ? Diagnostics::Unsigned(sample.victimHealthState) : "null", true},
             {"visibilityReadable", Diagnostics::Boolean(sample.visibilityReadable), true},
             {"cycleReadable", Diagnostics::Boolean(sample.cycleReadable), true},
             {"ownsPair", Diagnostics::Boolean(sample.ownsPair), true},
             {"observerReleasedPairRevision", Diagnostics::Unsigned(sample.observerReleasedPairRevision), true},
             {"observerHandoffNeutral", Diagnostics::Boolean(sample.observerHandoffNeutral), true},
             {"placementPending", Diagnostics::Boolean(sample.placementPending), true},
             {"pairPlacementPending", Diagnostics::Boolean(sample.pairPlacementPending), true},
             {"localX", sample.positionReadable ? std::to_string(sample.local[0]) : "null", true},
             {"localY", sample.positionReadable ? std::to_string(sample.local[1]) : "null", true},
             {"localZ", sample.positionReadable ? std::to_string(sample.local[2]) : "null", true},
             {"authorityX", std::to_string(sample.authority[0]), true},
             {"authorityY", std::to_string(sample.authority[1]), true},
             {"authorityZ", std::to_string(sample.authority[2]), true},
             {"victimX", sample.victimReadable ? std::to_string(sample.victim[0]) : "null", true},
             {"victimY", sample.victimReadable ? std::to_string(sample.victim[1]) : "null", true},
             {"victimZ", sample.victimReadable ? std::to_string(sample.victim[2]) : "null", true},
             {"authorityVictimX", std::to_string(sample.authorityVictim[0]), true},
             {"authorityVictimY", std::to_string(sample.authorityVictim[1]), true},
             {"authorityVictimZ", std::to_string(sample.authorityVictim[2]), true}});
    }
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
             {"playerComponentsReadable", Diagnostics::Boolean(sample.playerComponentsReadable), true},
             {"playerControlPresent", Diagnostics::Boolean(sample.playerControlPresent), true},
             {"playerControlLocal", Diagnostics::Unsigned(sample.playerControlLocal), true},
             {"playerNavigationPresent", Diagnostics::Boolean(sample.playerNavigationPresent), true},
             {"backstageReadable", Diagnostics::Boolean(sample.backstageReadable), true},
             {"backstageState", Diagnostics::Unsigned(sample.backstageState), true},
             {"backstageKeyA", Diagnostics::Unsigned(sample.backstageKeyA), true},
             {"backstageKeyB", Diagnostics::Unsigned(sample.backstageKeyB), true},
             {"backstageEnabled", Diagnostics::Unsigned(sample.backstageEnabled), true},
             {"backstagePolicy", Diagnostics::Unsigned(sample.backstagePolicy), true},
             {"navigationCached", Diagnostics::Boolean(sample.navigationCached), true},
             {"navigationGoalKnown", Diagnostics::Boolean(sample.navigationGoalKnown), true},
             {"navigationRouteCount", Diagnostics::Unsigned(sample.navigationRouteCount), true},
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
             {"selectedPeer", Diagnostics::Unsigned(sample.selectedPeer), true},
             {"selectedTerminalDead", Diagnostics::Boolean(sample.selectedTerminalDead), true},
             {"pendingReadable", Diagnostics::Boolean(sample.pendingReadable), true},
             {"pendingPresent", Diagnostics::Boolean(sample.pendingPresent), true},
             {"pendingPeer", Diagnostics::Unsigned(sample.pendingPeer), true},
             {"pendingLiving", Diagnostics::Boolean(sample.pendingLiving), true},
             {"targetSignalsReadable", Diagnostics::Boolean(sample.targetSignalsReadable), true},
             {"haveTargetSignal", Diagnostics::Unsigned(sample.haveTargetSignal), true},
             {"haveNextTargetSignal", Diagnostics::Unsigned(sample.haveNextTargetSignal), true}});
    }
    for (std::size_t i = 0U; i < kParticipantProbeRing; ++i) {
        const auto read = gTargetSuccessionProbeRead.load(std::memory_order_relaxed);
        if (read == gTargetSuccessionProbeWrite.load(std::memory_order_acquire)) break;
        const auto sample = gTargetSuccessionProbes[read % kParticipantProbeRing];
        gTargetSuccessionProbeRead.store(read + 1U, std::memory_order_release);
        diagnostics.Log("info", "aisync_target_succession", "Observed post-kill target-selector mutation/recovery boundary; diagnostics only, no target is injected.",
            {{"sessionCookie", Diagnostics::Unsigned(sample.sessionCookie), true},
             {"nativeGeneration", Diagnostics::Unsigned(sample.nativeGeneration), true},
             {"worldGeneration", Diagnostics::Unsigned(sample.worldGeneration), true},
             {"sampleTick", Diagnostics::Unsigned(sample.tick), true},
             {"entityA", Diagnostics::Unsigned(sample.entityA), true},
             {"entityB", Diagnostics::Unsigned(sample.entityB), true},
             {"birth", Diagnostics::Unsigned(sample.birth), true},
             {"interactionRevision", Diagnostics::Unsigned(sample.interactionRevision), true},
             {"pairRevision", Diagnostics::Unsigned(sample.pairRevision), true},
             {"phase", Diagnostics::Unsigned(sample.phase), true},
             {"retiredPeer", Diagnostics::Unsigned(sample.retiredPeer), true},
             {"selectionReadable", Diagnostics::Boolean(sample.selectionReadable), true},
             {"selectedReadable", Diagnostics::Boolean(sample.selectedReadable), true},
             {"selectedPresent", Diagnostics::Boolean(sample.selectedPresent), true},
             {"selectedPeer", Diagnostics::Unsigned(sample.selectedPeer), true},
             {"selectedBirth", Diagnostics::Unsigned(sample.selectedBirth), true},
             {"selectedLiving", Diagnostics::Boolean(sample.selectedLiving), true},
             {"selectedTerminalDead", Diagnostics::Boolean(sample.selectedTerminalDead), true},
             {"pendingReadable", Diagnostics::Boolean(sample.pendingReadable), true},
             {"pendingPresent", Diagnostics::Boolean(sample.pendingPresent), true},
             {"pendingPeer", Diagnostics::Unsigned(sample.pendingPeer), true},
             {"pendingBirth", Diagnostics::Unsigned(sample.pendingBirth), true},
             {"pendingLiving", Diagnostics::Boolean(sample.pendingLiving), true},
             {"pendingTerminalDead", Diagnostics::Boolean(sample.pendingTerminalDead), true},
             {"targetSignalsReadable", Diagnostics::Boolean(sample.targetSignalsReadable), true},
             {"haveTargetSignal", Diagnostics::Unsigned(sample.haveTargetSignal), true},
             {"haveNextTargetSignal", Diagnostics::Unsigned(sample.haveNextTargetSignal), true}});
    }
}
} // namespace aimp::ai_sync
