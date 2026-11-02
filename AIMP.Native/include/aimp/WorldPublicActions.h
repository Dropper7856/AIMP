#pragma once

#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldActionLineage.h"
#include <array>
#include <memory>

namespace aimp::world_actions {

// WS3 v0.11.4.63 experimental admission controls. A set bit means the
// corresponding safety gate is enforced (STRICT). Clearing a bit opens only
// that admission family for live testing; structural invariants such as valid
// memory reads, stable WorldEntityId values, RemoteApply anti-echo, wire
// validation and queue integrity remain non-bypassable.
enum class AdmissionSecurityGate : std::uint32_t {
    ForbiddenBehaviorInputs = 1U << 0U,
    ManifestGuidPrefilter = 1U << 1U,
    PayloadlessOnly = 1U << 2U,
    CertifiedCallbackShape = 1U << 3U,
    ZeroTail = 1U << 4U,
    PlayerTargetLocalOnly = 1U << 5U,
    AiTargetLocalOnly = 1U << 6U,
    UiTargetLocalOnly = 1U << 7U,
    AimpTargetLocalOnly = 1U << 8U,
    WorldContinuation = 1U << 9U,
    UnknownCausality = 1U << 10U,
    ManifestOwnerInput = 1U << 11U,
    ManifestTargetContract = 1U << 12U,
    LocalRootDescendants = 1U << 13U,
};

inline constexpr std::uint32_t kAllAdmissionSecurityMask =
    (1U << 14U) - 1U;

struct IdentityCandidateDiagnostics final {
    std::uint64_t runtimeKeyValid{};
    std::uint64_t runtimeKeyReject{};
    std::uint64_t candidateValid{};
    std::uint64_t candidateReject{};
    // Only counted when IdentifyRuntimeEntity returned a WorldEntityId.
    // Current/Self use IsCompositePublicInputAllowed, not just membership.
    // Owner uses the exact, unambiguous WS3 v2 forward port after the runtime
    // context passed; its static DataType/noTypedData does not decide eligibility.
    std::uint64_t publicInputValid{};
    std::uint64_t publicInputReject{};
};

// Last context-eligible ProbeDirect observation, for local diagnostics only.
// No pointer is retained. Runtime keys are never copied into gameplay packets.
struct ProbeIdentityObservation final {
    bool observed{};
    bool boundaryOwnerValid{};
    bool ownerMatchesCurrent{};
    CathodeRuntimeKey currentKey{};
    CathodeRuntimeKey ownerKey{};
    WorldEntityId currentId{};
    WorldEntityId ownerId{};
    WorldEntityKind currentKind{WorldEntityKind::Unknown};
    WorldEntityKind ownerKind{WorldEntityKind::Unknown};
    // Snapshot of the already-loaded manifest forward index for ProbeDirect only.
    // These fields are diagnostic and never participate in sender authorization.
    bool manifestReady{};
    bool manifestGuidKnown{};
    bool manifestExactForwardKeyPresent{};
    bool manifestExactForwardAllowed{};
    bool manifestExactOwnerSeenForGuid{};
    std::uint32_t manifestExactBridgeCount{};
    std::uint32_t manifestExactPayloadlessBridgeCount{};
    std::uint32_t manifestExactTypedBridgeCount{};
    std::uint32_t manifestGuidBridgeCount{};
    std::uint32_t manifestGuidOwnerCount{};
    WorldEntityId manifestFirstGuidOwner{};
    std::uint32_t manifestFirstGuidDataType{};
    bool manifestFirstGuidNoTypedData{};
};

inline constexpr std::size_t kRelevanceTopProducerCount = 16U;

struct RelevanceProducerDiagnostics final {
    std::uint64_t count{};
    WorldEntityId owner{};
    std::uint32_t shortGuid{};
    std::uint32_t primitiveType{};
    std::uint32_t replicationRelevance{};
    std::uint8_t rootKind{};
};

struct Diagnostics final {
    std::uint64_t contextUnsupported{};
    std::uint64_t contextReconstructed{};
    std::uint64_t contextPending{};
    std::uint64_t contextChainsCaptured{};
    std::uint64_t contextFramesCaptured{};
    std::uint64_t contextChainDepthPeak{};
    std::uint64_t contextChainOverflow{};
    std::uint64_t contextFramesReconstructed{};
    std::uint64_t actorUnresolved{};
    std::uint64_t causeTableOverflow{};
    // Candidate classification before queue/rate admission. PayloadRoot stays
    // zero until a retail payload codec is certified. Probe witnesses included.
    std::array<std::uint64_t, static_cast<std::size_t>(ActionClass::Count)> classified{};
    std::uint64_t targetOwnerDistinct{};
    std::uint64_t externalRoots{};
    std::uint64_t delayedRoots{};
    bool installed{};
    bool enabled{};
    bool emptyCallbackTest{};
    std::uint32_t admissionSecurityMask{kAllAdmissionSecurityMask};
    std::uint64_t seen{};
    std::uint64_t publicCandidates{};
    std::uint64_t rootsSent{};
    std::uint64_t compositeInputRootsSent{};
    std::uint64_t emittedOutputRootsSent{};
    std::uint64_t scheduledInputRootsSent{};
    std::uint64_t schedulerEdgeRootsSent{};
    std::uint64_t typedMessageRootsSent{};
    std::uint64_t typedMessageRootsApplied{};
    std::uint64_t typedPlayerPayloadLocalOnly{};
    std::uint64_t canonicalCompositeRootsSent{};
    std::uint64_t canonicalCompositeRootsApplied{};
    std::uint64_t doorLockStateRootsSent{};
    std::uint64_t doorLockStateRootsApplied{};
    std::uint64_t schedulerEdgeSidecarObserved{};
    std::uint64_t schedulerEdgeSidecarSubmitted{};
    std::uint64_t schedulerEdgeRemoteNaturalRemembered{};
    std::uint64_t schedulerEdgeRemoteDedupConsumed{};
    std::uint64_t scheduledEventRootsSuppressed{};
    std::uint64_t descendantsSuppressed{};
    std::uint64_t remoteSuppressed{};
    std::uint64_t identityRejected{};
    // First failing sender step. BoundaryLayout plus the eight existing reason
    // counters partition identityRejected; the latter now validate the owner.
    std::uint64_t identityBoundaryLayoutRejected{};
    std::uint64_t probeIdentityBoundaryLayoutRejected{};
    std::uint64_t identityBoundaryOwnerValid{};
    std::uint64_t probeIdentityBoundaryOwnerValid{};
    IdentityCandidateDiagnostics identityOwner{};
    IdentityCandidateDiagnostics probeIdentityOwner{};
    ProbeIdentityObservation probeIdentityLast{};
    std::uint64_t probeDirectRootsSent{};
    std::uint64_t identityRuntimeKeyRejected{};
    std::uint64_t identityRegistryLookupRejected{};
    std::uint64_t identityPublicInputRejected{};
    // Historical Current-candidate hypotheses, preserved as passive diagnostics.
    // They never select the packet owner or bypass boundary-owner validation.
    std::uint64_t identityDirectOwnerValid{};
    std::uint64_t identityInterfaceMapValid{};
    std::uint64_t identityInterfaceMapRejected{};
    std::uint64_t probeIdentityDirectOwnerValid{};
    std::uint64_t probeIdentityInterfaceMapValid{};
    std::uint64_t probeIdentityInterfaceMapRejected{};
    std::uint64_t identityDefinitionGetterRejected{};
    std::uint64_t identityDefinitionNullRejected{};
    std::uint64_t identityDefinitionDispatchRejected{};
    std::uint64_t identityDefinitionFaultRejected{};
    std::uint64_t identityDefinitionSelfRejected{};
    IdentityCandidateDiagnostics identityCurrent{};
    IdentityCandidateDiagnostics identitySelf{};
    IdentityCandidateDiagnostics probeIdentityCurrent{};
    IdentityCandidateDiagnostics probeIdentitySelf{};
    std::uint64_t probeIdentityRuntimeKeyRejected{};
    std::uint64_t probeIdentityRegistryLookupRejected{};
    std::uint64_t probeIdentityPublicInputRejected{};
    std::uint64_t probeIdentityDefinitionGetterRejected{};
    std::uint64_t probeIdentityDefinitionNullRejected{};
    std::uint64_t probeIdentityDefinitionDispatchRejected{};
    std::uint64_t probeIdentityDefinitionFaultRejected{};
    std::uint64_t probeIdentityDefinitionSelfRejected{};
    std::uint64_t contextRejected{};
    // Mutually exclusive first failures: Arg > Payload > Callback > Delay > Tail.
    // Their sum is contextRejected (apart from concurrent snapshot reads).
    std::uint64_t argRejected{};
    std::uint64_t payloadRejected{};
    std::uint64_t callbackRejected{};
    std::uint64_t delayRejected{};
    std::uint64_t tailRejected{};
    // Passive observations in the active ROOT candidate context path only.
    // Context flags may overlap; eligible means context passed, before identity/send.
    std::uint64_t probeDirectSeen{};
    std::uint64_t probeDirectArgRejected{};
    std::uint64_t probeDirectPayload{};
    std::uint64_t probeDirectCallback{};
    std::uint64_t probeDirectDelay{};
    std::uint64_t probeDirectTail{};
    std::uint64_t probeDirectEligible{};
    std::uint64_t queueRejected{};
    // Compatibility diagnostics: direct Player-local effects remain local.
    // NodesScanned/Mapped describe the bounded retained WorkItem ancestry used
    // to locate a known Player/AI/UI/AIMP cause behind internal World work.
    std::uint64_t playerCausalRejected{};
    std::uint64_t playerCausalNodesScanned{};
    std::uint64_t playerCausalNodesMapped{};
    // Same-owner and distinct-target candidates are counted separately in ROOT v6.
    // A distinct target must be a World endpoint of this exact public port.
    std::uint64_t targetOwnerRejected{};
    std::uint64_t targetOwnerMatched{};
    std::uint64_t probeTargetOwnerRejected{};
    std::uint64_t causalUnknownRejected{};
    std::uint64_t causalShapeRejected{};
    std::uint64_t causalUnmappedNodes{};
    std::uint64_t causalCycleRejected{};
    std::uint64_t causalDepthRejected{};
    std::uint64_t lineageOverflow{};
    // Causal origin inheritance/restoration, not additional network ROOTs.
    std::uint64_t lineageBuildInherit{};
    std::uint64_t lineageExecuteRestore{};
    std::uint64_t lineageCallbackRestore{};
    std::uint64_t applied{};
    std::uint64_t applyRejected{};
    std::uint64_t nativeFaults{};
    // WS3 0.11.4.70 persistent remote-ROOT flight recorder. These scalar
    // counters mirror the tiny on-disk snapshot so diagnostics can confirm
    // whether tracing was active even when AI.exe did not crash.
    std::uint64_t rootApplyTraceWrites{};
    std::uint64_t rootApplyJournalWrites{};
    std::uint64_t rootApplyTraceStarted{};
    std::uint64_t rootApplyTraceCompleted{};
    std::uint64_t rootApplyTraceLastStartedObservation{};
    std::uint64_t rootApplyTraceLastStartedSequence{};
    std::uint64_t rootApplyTraceLastCompletedObservation{};
    std::uint64_t rootApplyTraceLastCompletedSequence{};
    std::uint64_t rootApplyTraceCurrentStage{};
    std::uint64_t ws2Bypassed{};
    // Callback pointers/parents never cross IPC. Since v0.11.4.61, a
    // payloadless public Method may detach its sender-local retained parent when
    // either bounded retained ancestry or pre-ROOT source provenance proves a
    // known Player/AI/UI/AIMP cause. All-World callback chains remain
    // local/NeedsAdapter. The historical
    // EmptyCallback switch still applies only to the ProbeDirect witness.
    std::uint64_t callbackRoots{};
    std::uint64_t callbackGameplayRejected{};
    // ROOT v6: safe sender-local callback detachment is permitted only when a
    // bounded retained ancestry proves a known local-domain origin behind the
    // current replayable public World Method. These counters expose coverage
    // without serializing the retained parent/callback.
    std::uint64_t callbackBoundaryCandidates{};
    std::uint64_t callbackBoundaryRoots{};
    std::uint64_t callbackBoundaryPlayerRoots{};
    std::uint64_t callbackBoundaryAiRoots{};
    std::uint64_t callbackBoundaryUiRoots{};
    std::uint64_t callbackBoundaryAimpRoots{};
    std::uint64_t callbackBoundaryApplied{};
    std::uint64_t callbackWorldContinuationRejected{};
    std::uint64_t callbackBoundaryUnknownRejected{};
    // v0.11.4.62 live admission A/B diagnostics. These counters increment when
    // an unchecked launcher box lets an action pass a gate that would have
    // rejected it under the default strict policy.
    std::uint64_t admissionForbiddenInputBypassed{};
    std::uint64_t admissionGuidPrefilterRejected{};
    std::uint64_t admissionGuidPrefilterBypassed{};
    std::uint64_t admissionPayloadBypassed{};
    std::uint64_t admissionCallbackShapeBypassed{};
    std::uint64_t admissionTailBypassed{};
    std::uint64_t admissionPlayerTargetBypassed{};
    std::uint64_t admissionAiTargetBypassed{};
    std::uint64_t admissionUiTargetBypassed{};
    std::uint64_t admissionAimpTargetBypassed{};
    std::uint64_t admissionWorldContinuationBypassed{};
    std::uint64_t admissionUnknownCausalityBypassed{};
    std::uint64_t admissionManifestOwnerInputBypassed{};
    std::uint64_t admissionManifestTargetBypassed{};
    std::uint64_t admissionDescendantBypassed{};
    // v0.11.4.73: pre-ROOT source provenance carries a source domain plus a
    // unique process-local causal chain id. WorkBuild/WorkExecute propagate the
    // exact chain through retained ancestry and active execution frames until
    // one replayable shared-World Method commits the chain's network ROOT.
    // These tags are lifetime-bound and never serialized.
    std::uint64_t causalProvenanceSeeded{};
    std::uint64_t causalProvenanceInherited{};
    std::uint64_t causalProvenanceBoundary{};
    std::uint64_t causalProvenanceOverflow{};
    std::uint64_t causalProvenancePlayer{};
    std::uint64_t causalProvenanceAi{};
    std::uint64_t causalProvenanceUi{};
    std::uint64_t causalProvenanceAimp{};
    // 0.11.4.73 causal transaction diagnostics. A chain is process-local and
    // lifetime-bound to its WorkItems; it never crosses the network. Exactly
    // one replayable shared-World boundary may commit a ROOT for each chain.
    std::uint64_t causalChainsCreated{};
    std::uint64_t causalChainsReleased{};
    std::uint64_t causalGroupsLive{};
    std::uint64_t causalGroupsLivePeak{};
    std::uint64_t causalRootReserved{};
    std::uint64_t causalRootCommitted{};
    std::uint64_t causalRootReservationCanceled{};
    std::uint64_t causalRootAlreadyClaimed{};
    std::uint64_t causalRootMissing{};
    std::uint64_t causalRootNonWorldLocal{};
    std::uint64_t causalRootGroupOverflow{};
    std::uint64_t causalRootSuppressedSiblings{};
    std::uint64_t causalRootProbeStandalone{};
    // 0.11.4.76.3 generic causal-boundary discovery. These counters describe
    // manifest-derived graph semantics, never gameplay-specific ShortGuids.
    std::uint64_t causalBoundaryProfileMissing{};
    std::uint64_t causalBoundaryNoWorldEffect{};
    std::uint64_t causalBoundaryPlayerDependent{};
    std::uint64_t causalBoundaryWorldCandidate{};
    std::uint64_t causalBoundaryTruncated{};
    std::uint64_t causalBoundaryUnknownDependency{};
    // 0.11.4.74 parent-context propagation. These bindings are process-local
    // scheduler metadata keyed by a retained callback WorkItem + invocation.
    // They never cross the wire; they let delayed/sibling work inherit the
    // already selected causal ROOT without tagging the shared parent itself.
    std::uint64_t parentContextBound{};
    std::uint64_t parentContextBoundLocalRoot{};
    std::uint64_t parentContextBoundRemoteRoot{};
    std::uint64_t parentContextInheritedOrigin{};
    std::uint64_t parentContextInheritedCausal{};
    std::uint64_t parentContextInheritedCause{};
    std::uint64_t parentContextReleased{};
    std::uint64_t parentContextStale{};
    std::uint64_t parentContextOverflow{};
    std::uint64_t parentContextsLive{};
    std::uint64_t parentContextsPeak{};
    std::uint64_t lineageGuardSuppressed{};
    std::uint64_t probeCallbackExceptions{};
    std::uint64_t probeCausalUnknownExceptions{};
    std::uint64_t rootRateRejected{};
    // Step 3 authority/admission diagnostics. "Preserved" means the candidate
    // was not taken over by authority and retail remained executable locally.
    std::uint64_t authorityRetailPreserved{};
    std::uint64_t authorityLocalSuppressed{};
    std::uint64_t authorityAwaitSubmitted{};
    std::uint64_t authorityAutonomousSuppressed{};
    std::uint64_t causalBoundaryBranching{};
    std::uint64_t emittedOutputForwardingRejected{};
    // Patch 4.1 carrier ownership / pipeline diagnostics. These count only
    // certified StateTransition ReplicateCarrier candidates, not generic
    // relevance observations. "Suppressed" below means network ROOT fallback
    // suppression; retail execution is deliberately preserved.
    std::uint64_t carrierObserved{};
    std::uint64_t carrierNetworkEligible{};
    std::uint64_t carrierForwardingComplete{};
    std::uint64_t carrierSelectorResolved{};
    std::uint64_t carrierSelectorRejected{};
    std::uint64_t carrierContextCaptured{};
    std::uint64_t carrierManifestAdmitted{};
    std::uint64_t carrierPreBusPreserved{};
    std::uint64_t carrierSubmitAttempt{};
    std::uint64_t carrierSubmitSuccess{};
    std::uint64_t scheduledCoveredByCarrier{};
    std::uint64_t scheduledCoveredByCarrierSuppressed{};
    // Patch 5 high-level CompositeInput carrier pipeline. These are separate
    // from Patch 4's emitted-output carrier counters so diagnostics can prove
    // that network ownership actually moved to the higher CATHODE cause.
    std::uint64_t compositeCarrierObserved{};
    std::uint64_t compositeCarrierNetworkEligible{};
    std::uint64_t compositeCarrierTargetAdmitted{};
    std::uint64_t compositeCarrierManifestAdmitted{};
    std::uint64_t compositeCarrierPreBusPreserved{};
    std::uint64_t compositeCarrierSubmitAttempt{};
    std::uint64_t compositeCarrierSubmitSuccess{};
    // Patch 4.2: an exact intrinsic StateTransition without a certified
    // higher carrier is an unsafe network boundary. It remains retail-local
    // on Host but is quarantined from ROOT publication.
    std::uint64_t scheduledStateTransitionNoCarrier{};
    std::uint64_t scheduledStateTransitionQuarantinedNoCarrier{};
    std::uint64_t scheduledInputCertificationRejected{};
    std::uint64_t callbackBridgeScopes{};
    std::uint64_t callbackBridgeSubmitted{};
    std::uint64_t callbackBridgeOwnerRejected{};
    std::uint64_t callbackBridgeSameInputRejected{};
    std::uint64_t callbackBridgeAlreadyClaimedRejected{};
    std::uint64_t callbackBridgeRateRejected{};
    std::uint64_t callbackBridgeQueueRejected{};
    // WM08 replication relevance. Candidates are counted before the relevance
    // decision; disposition counters form the semantic admission partition.
    std::uint64_t relevanceCandidates{};
    std::uint64_t relevanceReplicateIntrinsic{};
    std::uint64_t relevanceReplicateCarrier{};
    std::uint64_t relevanceDefer{};
    std::uint64_t relevanceNotRelevant{};
    std::uint64_t relevanceUnknown{};
    std::uint64_t relevanceReasonRouting{};
    std::uint64_t relevanceReasonSchedulerMaintenance{};
    std::uint64_t relevanceReasonInternalState{};
    std::uint64_t relevanceReasonNoSink{};
    std::uint64_t relevanceReasonBetterDescendant{};
    std::uint64_t relevanceReasonCycleAmbiguous{};
    std::uint64_t relevanceReasonHistorySensitive{};
    std::uint64_t relevanceReasonStateTransitionCarrier{};
    std::uint64_t relevanceReasonCompositeStateTransitionCarrier{};
    std::uint64_t relevanceReasonCompositeRelevantFrontierRelaxed{};
    std::uint64_t relevanceInboundRejected{};
    std::array<RelevanceProducerDiagnostics, kRelevanceTopProducerCount> relevanceTop{};
    std::uint32_t relevanceTopCount{};
    std::uint64_t installFailures{};
};

// Bounded process-local crash correlation. Only semantic IDs enter packets;
// item/parent addresses below are diagnostics, never replay references.
struct ContextTrace final {
    std::uint64_t serial{}, cause{}, sourcePeer{}, authoritySequence{};
    std::uint64_t invocation{}, parentInvocation{}, causalChainId{};
    WorldEntityId owner{}, target{}, actor{};
    CathodeRuntimeKey contextRuntimeKey{};
    std::uintptr_t item{}, parent{};
    std::uint32_t epoch{}, input{}, parentInput{}, parentFlags{};
    std::uint16_t parentPayloadBytes{};
    std::uint8_t stage{}, actorKind{}, contextKind{}, pendingReason{}, causalSource{}, rootKind{};
    bool parentReadable{}, parentPayload{};
};
[[nodiscard]] std::array<ContextTrace, 64U> ContextTraceSnapshot() noexcept;

// Uses the existing registry, Host authority, opcode, dispatcher and apply
// scope. This module owns only the retail boundary and local lineage tags.
[[nodiscard]] bool Arm(const SignatureResolver&, HookManager&,
                       WorldEntityRegistry&, std::string& error) noexcept;
[[nodiscard]] bool Requested() noexcept;
[[nodiscard]] bool Installed() noexcept;
[[nodiscard]] bool Enabled() noexcept;

// PlayerSync bootstrap-only preparation for the certified retail CompositeInput
// boundary. This resolves the module/00965920 target without installing any
// WorldSync hook, binding a registry, or requiring WORLD_MANIFEST_LOAD.
// Arm() may later install the normal WorldSync detour over the same target.
[[nodiscard]] bool PrepareLocalCompositeInputBoundary(
    const SignatureResolver& resolver) noexcept;

// Patch 115.5: narrow local observer for the certified EmittedOutput boundary.
// It receives only sender-local pointers and is invoked before WorldSync
// admission. The observer must not retain the borrowed holder unless it uses
// the retail managed-reference primitive itself. No pointer crosses IPC.
using LocalEmittedOutputObserver = void(*)(
    void* sharedRef, void* executionObject, std::uint32_t outputShortGuid) noexcept;
[[nodiscard]] bool PrepareLocalEmittedOutputObservation(
    const SignatureResolver& resolver, HookManager& hooks,
    LocalEmittedOutputObserver observer, std::string& error) noexcept;
void ClearLocalEmittedOutputObserver(
    LocalEmittedOutputObserver observer) noexcept;

// PlayerSync local-only bridge: invoke a public input on an already-created
// retail Composite interface. Generic CompositeInstance uses base 00965920,
// while specialized retail entities may override the same virtual +0x394 slot.
// Network admission remains strict elsewhere; this helper accepts only an
// executable AI.exe virtual dispatch returned by the exact +0x70 retail getter.
// The RemoteRoot lineage scope prevents this local provisioning action (and an
// override's possible call into hooked base 00965920) from being published by
// WorldSync.
enum class LocalCompositeInputInvokeStatus : std::uint8_t {
    Invoked = 0U,
    RuntimeUnavailable = 1U,
    InvalidArguments = 2U,
    DefinitionGetterRejected = 3U,
    DefinitionNullRejected = 4U,
    DefinitionDispatchRejected = 5U,
    DefinitionFaultRejected = 6U,
    InvalidBoundary = 7U,
    NativeFault = 8U,
};
[[nodiscard]] bool MissionOutputReady() noexcept;
// MissionSync validates its own compiled frontier. This invokes only the native
// emitted-port propagator with an empty context, never a source decision/AI.
[[nodiscard]] LocalCompositeInputInvokeStatus InvokeMissionOutputNoCapture(
    const WorldEntityLease& lease, std::uint32_t port, std::uint8_t selector) noexcept;
[[nodiscard]] LocalCompositeInputInvokeStatus InvokeLocalCompositeInputNoCapture(
    void* executionObject, void* sharedRef, std::uint32_t publicInputShortGuid,
    std::uintptr_t* resolvedDispatchOut = nullptr) noexcept;
// Execute a local module's native continuation under RemoteRoot lineage.
// Descendant work keeps that origin, so it is not republished by WorldSync.
// The caller owns validation and native-reference lifetimes.
[[nodiscard]] bool InvokeLocalActionNoCapture(void(*action)(void*) noexcept, void* context) noexcept;
// Launcher IPC owns this runtime setting after connection; safe before Arm.
// Compatibility name: it enables only the ProbeDirect callback witness.
void SetEmptyCallbackTest(bool enabled) noexcept;
[[nodiscard]] bool IsLocalPeer(std::uint64_t peer) noexcept;
void SetSessionAuthority(std::uint64_t localPeer, bool host) noexcept;
void ClearSessionAuthority() noexcept;
void SetWorldBusActive(bool active) noexcept;
[[nodiscard]] bool SessionAuthorityBound() noexcept;
[[nodiscard]] bool SessionAuthorityHost() noexcept;
[[nodiscard]] std::uint64_t SessionAuthorityPeer() noexcept;
[[nodiscard]] bool WorldBusActive() noexcept;

// Patch 5.3: QueueInput/SchedulerEdge is a native WS3 ROOT boundary, not a
// legacy WS2 operation. Submission returns how the caller must treat the local
// retail QueueInput after authority admission.
enum class SchedulerEdgeSubmission : std::uint8_t {
    RetailOnly = 0U,
    ExecuteLocalRoot = 1U,
    AwaitAuthority = 2U,
    AuthoritySuppressed = 3U,
};
[[nodiscard]] SchedulerEdgeSubmission SubmitSchedulerEdgeRoot(
    void** sourceSharedRef, const WorldEntityId& source, const WorldEntityId& target,
    std::uint32_t schedulerParameterShortGuid, std::uint32_t targetInputShortGuid,
    double relativeDelaySeconds, ContextTrace& cause) noexcept;
using SchedulerEdgeRetailThunk = void (*)(void*) noexcept;
void InvokeSchedulerEdgeRetailScoped(
    Origin origin, const ContextTrace& cause, SchedulerEdgeRetailThunk thunk, void* opaque) noexcept;

// Patch 5.8: WS2's proven callback-bearing public Composite canonicalization is
// absorbed as a native WS3 ROOT. Only non-Host clients author this optimistic
// local-first root; Host ordering and authoritative broadcast remain unchanged.
enum class CanonicalCompositeSubmission : std::uint8_t {
    RetailOnly = 0U,
    Submitted = 1U,
};
[[nodiscard]] CanonicalCompositeSubmission SubmitCanonicalCompositeRoot(
    const WorldEntityId& owner, std::uint32_t publicInputShortGuid,
    ContextTrace& cause) noexcept;
// Patch 5.10 keeps the experimental 5.8 CanonicalComposite Client sender
// dormant. The function is intentionally non-constexpr because the native
// build uses /W4 /WX and the dormant call sites must remain warning-clean.
[[nodiscard]] bool CanonicalCompositeClientCaptureActive() noexcept;
using CanonicalCompositeRetailThunk = void (*)(void*) noexcept;
void InvokeCanonicalCompositeRetailScoped(
    Origin origin, const ContextTrace& cause,
    CanonicalCompositeRetailThunk thunk, void* opaque) noexcept;

// Patch 5.4: second proven WS2 generic CATHODE primitive. A typed-message ROOT
// transports only stable World source/target identity plus the bounded blob.
// Sender RuntimeKeys and callback/work-item pointers never cross IPC.
enum class TypedMessageSubmission : std::uint8_t {
    RetailOnly = 0U,
    ExecuteLocalRoot = 1U,
    AwaitAuthority = 2U,
    AuthoritySuppressed = 3U,
};
[[nodiscard]] TypedMessageSubmission SubmitTypedMessageRoot(
    const WorldEntityId& source, const WorldEntityId& target,
    std::uint32_t messageType, std::span<const std::byte> payload,
    double relativeDelaySeconds, ContextTrace& cause) noexcept;
using TypedMessageRetailThunk = void (*)(void*) noexcept;
void InvokeTypedMessageRetailScoped(
    Origin origin, const ContextTrace& cause, TypedMessageRetailThunk thunk, void* opaque) noexcept;

// Patch 5.11 compatibility surface. Patch 5.12 retires active DoorLockState
// authoring after runtime proved that the working Host path is the generic
// upstream StateTransition carrier. The codec/receiver remains available for
// diagnostics and backward inspection, but current capture does not call it.
enum class DoorLockStateSubmission : std::uint8_t {
    RetailOnly = 0U,
    Submitted = 1U,
};
[[nodiscard]] DoorLockStateSubmission SubmitDoorLockStateRoot(
    const WorldEntityId& source, const WorldEntityId& door,
    std::uint32_t semanticInput, ContextTrace& cause) noexcept;
// True only while retail is executing under an authoritative RemoteRoot.
// Unlike SuppressDescendantCapture(), LocalRoot descendants are not suppressed.
[[nodiscard]] bool RemoteRootOriginActive() noexcept;
// Patch 5.3.1: StateTransition ROOT ownership does not consume lateral Event
// edges. LocalRoot may publish a SchedulerEdge sidecar; RemoteRoot may only
// remember a naturally regenerated edge so its later authoritative sidecar can
// commit without replaying the effect twice. SchedulerEdge's own retail replay
// is excluded from both paths.
[[nodiscard]] bool SchedulerEdgeLocalSidecarCaptureActive() noexcept;
[[nodiscard]] bool SchedulerEdgeRemoteNaturalCaptureActive() noexcept;
void RememberRemoteNaturalSchedulerEdge(
    const WorldEntityId& source, const WorldEntityId& target,
    std::uint32_t schedulerParameterShortGuid, std::uint32_t targetInputShortGuid,
    double relativeDelaySeconds) noexcept;
[[nodiscard]] bool ConsumeRemoteNaturalSchedulerEdge(
    const WorldEntityId& source, const WorldEntityId& target,
    std::uint32_t schedulerParameterShortGuid, std::uint32_t targetInputShortGuid,
    double relativeDelaySeconds) noexcept;
// Shared validation for the currently supported ROOT wire contract.
[[nodiscard]] bool PublicRootManifestAdmissionAllowed(
    const WorldEntityRegistry&, const WorldEntityId&,
    const world_interactions::CompositePublicInputEvent&) noexcept;
[[nodiscard]] bool PublicRootManifestAdmissionAllowed(
    const WorldEntityRegistry& registry, const WorldEntityId& owner,
    std::uint32_t input, const WorldEntityId& target,
    double relativeDelaySeconds, std::uint8_t flags) noexcept;
[[nodiscard]] bool SuppressDescendantCapture() noexcept;
// Historical v0.11.4.55/.56 pre-Queue bridge API. Gameplay use is disabled;
// retained only for bounded diagnostics/backward source comparison.
[[nodiscard]] bool CallbackBridgeCaptureActive() noexcept;
// Diagnostic-only accessor for the active stack-local bridge context. Values
// are process-local and never serialized. Used by the bounded pre-Queue trace
// to identify the exact outer public input associated with an unknown leaf.
[[nodiscard]] bool TryGetCallbackBridgeContext(
    WorldEntityId& owner, std::uint32_t& outerInput) noexcept;
[[nodiscard]] bool SubmitCallbackBridgeRoot(
    const WorldEntityId& owner, std::uint32_t input) noexcept;
[[nodiscard]] bool DeferPublicInput(void* sharedRef, std::uint32_t input) noexcept;
void ResetForGeneration() noexcept;
void BeginLogicalShutdown() noexcept;
[[nodiscard]] Diagnostics Snapshot() noexcept;
struct PreparedPublicRoot;
// Build and pin every receiver-local dependency before the first transaction member runs.
[[nodiscard]] world_interactions::ExecuteResult Prepare(
    const world_interactions::CompositePublicInputEvent&, const WorldEntityId&,
    WorldEntityRegistry&, std::shared_ptr<PreparedPublicRoot>&) noexcept;
[[nodiscard]] world_interactions::ExecuteResult ExecutePrepared(
    PreparedPublicRoot&) noexcept;
[[nodiscard]] world_interactions::ExecuteResult Execute(
    const world_interactions::CompositePublicInputEvent&,
    const WorldEntityId&, const WorldEntityLease&, WorldEntityRegistry&) noexcept;
// The envelope binds a player actor to the originating authenticated peer.
void SetRemoteCauseEnvelope(std::uint64_t sourcePeer, std::uint64_t sequence) noexcept;
void RecordDependencyPending(const world_interactions::CompositePublicInputEvent&,
    const WorldEntityId& owner, std::uint64_t peer, std::uint64_t sequence,
    std::uint8_t reason) noexcept;

} // namespace aimp::world_actions
