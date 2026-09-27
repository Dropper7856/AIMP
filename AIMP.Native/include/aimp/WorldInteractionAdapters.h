#pragma once

#include "aimp/Common.h"
#include "aimp/WorldEntityRegistry.h"
#include "aimp/WorldReplicationTypes.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace aimp {

class HookManager;
class SignatureResolver;
class WorldEntityLease;
class WorldEntityRegistry;

namespace world_interactions {

using LocalInputTargetObserver = void(*)(
    void* targetSharedRef, std::uint32_t inputShortGuid) noexcept;

inline constexpr std::uint32_t kOperationObservedInputEdge = 0x000C0601U;
inline constexpr std::uint32_t kOperationObservedCallbackInputEdge = 0x000C0603U;
// Fix3.20: new safe wire identity for the runtime-proven Character callback
// family. The historical 0x000C0603 payload remains decode-only/fail-closed.
inline constexpr std::uint32_t kOperationObservedCharacterCallbackInputEdge = 0x000C0604U;
inline constexpr std::uint32_t kOperationObservedTypedMessage = 0x000C0801U;
inline constexpr std::uint32_t kOperationObservedLogicSwitchState = 0x000C0901U;
// Fix3.24 AIMP/WorldSync operation ID. This is NOT a retail CATHODE messageType.
// It carries only stable Composite instance identity (WorldMutation.entity) and
// the public input ShortGuid proven by the manifest bridge.
inline constexpr std::uint32_t kOperationCompositePublicInput = 0x000C0A01U;
inline constexpr std::uint8_t kPayloadVersion = 1U;
inline constexpr std::uint8_t kObservedInputEdgeKind = 1U;
inline constexpr std::uint8_t kObservedCallbackInputEdgeKind = 5U;
inline constexpr std::uint8_t kObservedCharacterCallbackInputEdgeKind = 6U;
inline constexpr std::size_t kObservedInputEdgePayloadBytes = 48U;
inline constexpr std::size_t kObservedCallbackInputEdgePayloadBytes = 56U;
inline constexpr std::size_t kObservedCharacterCallbackInputEdgePayloadBytes = 80U;
// Historical Fix3 wire constants. They are retained only so mixed/stale Fix3
// traffic can still be decoded and fail-closed. Fix3.8 does not treat them as
// a proven callback ABI and never emits or executes callback replay from them.
inline constexpr std::uint32_t kCallbackReferenceFamilyCallerRva = 0x004BE280U;
inline constexpr std::uint32_t kCallbackReferenceConstructorFlags = 0x80000000U;
// Fix3.20 runtime/static-confirmed narrow callback classifier. These constants
// describe the CATHODE::Character callback work item observed on both peers;
// they are not Door/Unlock semantics.
inline constexpr std::uint32_t kCharacterCallbackInputShortGuid = 0x6575CA9BU;
inline constexpr std::uint32_t kCharacterCallbackConstructorFlags = 0x12000000U;
inline constexpr std::uint32_t kCharacterCallbackExecutionVtableRva = 0x00DD08D8U;
inline constexpr std::uint32_t kCharacterCallbackFamilyCallerRva = 0x004BE280U;
inline constexpr std::uint8_t kObservedTypedMessageKind = 2U;
inline constexpr std::uint8_t kObservedTypedMessagePayloadVersion = 2U;
// Fix3.14 diagnostic extension. Version 3 is emitted only for the bounded
// witness family while the remote-identity probe is armed; normal typed
// traffic remains byte-for-byte v2.
inline constexpr std::uint8_t kObservedTypedMessageIdentityPayloadVersion = 3U;
inline constexpr std::uint8_t kObservedLogicSwitchStateKind = 3U;
inline constexpr std::uint8_t kCompositePublicInputKind = 7U;
// WS3 ROOT v13: v12 layout is retained byte-for-byte. Kind 6 absorbs the
// proven WS2 canonical Composite-input valve as a native WS3 ROOT: the sender
// executes its callback-bearing retail input locally, while the wire carries
// only stable Composite owner + public input. No callback pointer/RuntimeKey is
// serialized. v12 remains decodable for already-staged same-build traffic.
inline constexpr std::uint8_t kCompositePublicRootPayloadVersion = 13U;
inline constexpr std::uint8_t kCompositePublicRootPreviousPayloadVersion = 12U;
inline constexpr std::size_t kCompositePublicRootHeaderBytes = 96U;
inline constexpr std::size_t kCompositePublicRootContextFrameBytes = 32U;
inline constexpr std::size_t kCompositePublicRootMaximumContextDepth = 16U;
inline constexpr std::size_t kCompositePublicRootMaximumContextPayloadBytes =
    kWorldPayloadMaximumBytes - kCompositePublicRootHeaderBytes -
    kCompositePublicRootContextFrameBytes;
inline constexpr std::uint32_t kNativeCallbackInputShortGuid = 0x6575CA9BU;
inline constexpr std::uint8_t kPublicRootExternalInvocation =
    world_actions::kPublicRootFlagExternalInvocation;
inline constexpr std::uint8_t kPublicRootDetachedCallback =
    world_actions::kPublicRootFlagDetachedCallback;
inline constexpr std::uint8_t kPublicRootKnownFlags = world_actions::kPublicRootKnownFlags;
inline constexpr std::size_t kObservedLogicSwitchStatePayloadBytes = 24U;
inline constexpr std::size_t kCompositePublicInputPayloadBytes = 16U;
inline constexpr std::size_t kObservedTypedMessageHeaderBytes = 48U;
inline constexpr std::size_t kObservedTypedMessageIdentityHeaderBytes = 80U;
inline constexpr std::size_t kObservedTypedMessageMaximumDataBytes =
    kWorldPayloadMaximumBytes - kObservedTypedMessageHeaderBytes;

enum class SourceCategory : std::uint8_t {
    // v12.7 Fix 1: the active path is deliberately type-agnostic. Legacy
    // categories remain decodable for compatibility but are no longer used by
    // local capture.
    GenericWorld = 0,
    Interaction = 1,
    Pickup = 2,
    Rewire = 3,
};


struct ObservedTypedMessage final {
    std::uint32_t messageType{};
    std::uint64_t observationId{};
    CathodeRuntimeKey sourceRuntimeKey{};
    CathodeRuntimeKey targetRuntimeKey{};
    // Fix3.14: stable manifest identities are carried alongside sender-local
    // RuntimeKeys. They are diagnostic metadata only in Fix3.14; the existing
    // raw-key replay safety gate remains authoritative and unchanged.
    WorldEntityId sourceWorldEntity{};
    WorldEntityId targetWorldEntity{};
    bool stableWorldIdentityIncluded{};
    double relativeDelaySeconds{};
};

struct ObservedInputEdge final {
    SourceCategory sourceCategory{SourceCategory::GenericWorld};
    std::uint32_t schedulerParameterShortGuid{};
    std::uint32_t targetInputShortGuid{};
    std::uint64_t observationId{};
    WorldEntityId sourceEntity{};
    float relativeDelaySeconds{};
};

// Historical Fix3/Fix3.2 payload shape. The callbackConstructorFlags field was
// an experimental hypothesis and is NOT a proven generic callback recipe.
// Fix3.8 keeps this struct solely for compatibility decoding/no-op handling.
struct ObservedCallbackInputEdge final {
    SourceCategory sourceCategory{SourceCategory::GenericWorld};
    std::uint32_t schedulerParameterShortGuid{};
    std::uint32_t targetInputShortGuid{};
    std::uint32_t callbackConstructorFlags{kCallbackReferenceConstructorFlags};
    std::uint32_t callbackPayloadSize{};
    std::uint32_t callbackFamilyCallerRva{};
    std::uint64_t observationId{};
    WorldEntityId sourceEntity{};
    float relativeDelaySeconds{};
};

// Fix3.20 transport for one runtime-proven callback-bearing QueueInput class.
// Target identity remains the WorldMutation anchor; source and callback target
// are stable manifest identities. No pointer/refcount/work-item bytes cross IPC.
struct ObservedCharacterCallbackInputEdge final {
    SourceCategory sourceCategory{SourceCategory::GenericWorld};
    std::uint32_t schedulerParameterShortGuid{};
    std::uint32_t targetInputShortGuid{};
    std::uint32_t callbackInputShortGuid{kCharacterCallbackInputShortGuid};
    std::uint32_t callbackConstructorFlags{kCharacterCallbackConstructorFlags};
    std::uint32_t callbackExecutionVtableRva{kCharacterCallbackExecutionVtableRva};
    std::uint32_t callbackFamilyCallerRva{kCharacterCallbackFamilyCallerRva};
    std::uint64_t observationId{};
    WorldEntityId sourceEntity{};
    WorldEntityId callbackEntity{};
    float relativeDelaySeconds{};
};

struct ObservedLogicSwitchState final {
    std::uint64_t observationId{};
    CathodeRuntimeKey contextRuntimeKey{};
    bool enabled{};
};

// Canonical public-interface event. The owning Composite instance is always the
// WorldMutation anchor and is intentionally not duplicated here. WS3 v10 carries
// a complete retained WorkItem ancestry as portable frame descriptors. Frame 0
// is the public action parent (or the consumed ScheduledInput item); later frames are its
// ancestors. Reconstruction therefore runs in reverse order.
enum class PublicContextKind : std::uint8_t { Unspecified, Autonomous, NativeContextChain };
// SourcePlayer is bound to the authenticated envelope sourcePeer. It is never
// resolved through the receiver's local-player singleton.
enum class PublicActorKind : std::uint8_t { Authority, SourcePlayer, WorldEntity };

struct PublicNativeContextFrame final {
    CathodeRuntimeKey runtimeKey{};
    std::uint32_t constructorFlags{};
    std::uint32_t inputShortGuid{};
    // Sender-local invocation serial, scoped by authenticated peer/generation.
    // Diagnostic/causal identity only; never dereferenced by the receiver.
    std::uint64_t invocation{};
    std::uint16_t payloadOffset{};
    std::uint16_t payloadSize{};
    std::uint32_t descriptorDelayBits{};
};

enum class PublicRootKind : std::uint8_t {
    CompositeInput = 1U, EmittedOutput = 2U, ScheduledInput = 3U,
    SchedulerEdge = 4U, TypedMessage = 5U, CanonicalCompositeInput = 6U,
    DoorLockState = 7U
};
inline constexpr std::uint16_t kMaximumTransactionRoots = 32U;

struct CompositePublicInputEvent final {
    std::uint64_t observationId{};
    std::uint32_t publicInputShortGuid{};
    // SchedulerEdge ROOT only: the separate ShortGuid passed as the explicit
    // FUN_009271C0 scheduler parameter. publicInputShortGuid remains the
    // target descriptor input ShortGuid. For TypedMessage ROOT,
    // publicInputShortGuid stores the retail FUN_00930640 messageType. For
    // DoorLockState ROOT it stores certified RequestLock/RequestUnlock input.
    std::uint32_t schedulerParameterShortGuid{};
    bool retailRoot{};
    PublicRootKind rootKind{PublicRootKind::CompositeInput};
    std::uint8_t interfaceSelector{}; // output: 1 = Entity+70, 2 = Entity+6C
    std::uint64_t causalSerial{};
    std::uint64_t segmentSerial{};
    std::uint64_t authorityTransaction{};
    std::uint16_t rootOrdinal{};
    std::uint16_t rootCount{1U};
    WorldEntityId targetEntity{};
    double relativeDelaySeconds{};
    std::uint8_t flags{};
    PublicContextKind contextKind{PublicContextKind::Unspecified};
    PublicActorKind actorKind{PublicActorKind::Authority};
    WorldEntityId actorEntity{};
    std::uint8_t contextFrameCount{};
    std::uint16_t contextPayloadSize{};
    std::array<PublicNativeContextFrame, kCompositePublicRootMaximumContextDepth> contextFrames{};
    std::array<std::byte, kCompositePublicRootMaximumContextPayloadBytes> contextPayload{};

    // Local WS3 execution metadata. Not part of the serialized public-root payload.
    // Used by WorldPublicActions to preserve the Client state-carrier ordering
    // while normalizing the event before local publication.
    bool normalizeClientStateCarrierOrder{};
};

[[nodiscard]] bool ValidPublicRootContext(const CompositePublicInputEvent&) noexcept;
[[nodiscard]] bool SubmitPublicRoot(const WorldEntityId& owner,
    CompositePublicInputEvent& event) noexcept;

// WS3 uses the existing observation/echo IDs and outbound intent transport.
// No semantic time-window dedupe: two distinct ROOT invocations remain two.
[[nodiscard]] bool SubmitPublicRoot(
    const WorldEntityId& owner,
    std::uint32_t input) noexcept;
[[nodiscard]] bool SubmitPublicRoot(
    const WorldEntityId& owner, const WorldEntityId& target,
    std::uint32_t input, double relativeDelaySeconds,
    std::uint8_t flags = 0U) noexcept;
// Release a queued ROOT only when transport/authority confirms it was not accepted.
// The original stable intent is returned unchanged; no retail action is run.
[[nodiscard]] bool DiscardPublicRootIntent(const WorldIntent& intent) noexcept;

enum class ExecuteStatus : std::uint8_t {
    Queued = 0,
    InvalidPayload,
    RuntimeUnavailable,
    NativeFault,
};

struct ExecuteResult final {
    ExecuteStatus status{ExecuteStatus::RuntimeUnavailable};
    bool queued{};
};

// Fix3.19 diagnostic-only causal flight recorder. Entries are normalized to
// scalar CATHODE identity/provenance; no raw pointer, refcount or callback
// work-item address is retained. The trace is never consumed by replay.
// Fix3.19 BuildFix01: the first differential capture saturated at 256
// unique signatures on both peers before all three witness anchors had been
// represented. 1024 remains bounded while keeping the diagnostics snapshot
// small enough for the engine-thread stack and materially reducing false
// Host-only/Client-only conclusions caused by recorder saturation.
inline constexpr std::size_t kFix319CausalTraceMaximumEntries = 1024U;

// Fix3.21 diagnostic-only generic composite/state-value differential recorder.
// It observes only the statically bounded CATHODE helper call family reached
// from the confirmed 0x005F3AE0/0x005F4200/0x005F4310/0x005F4580 retail
// callers. No entry is transported or replayed.
inline constexpr std::size_t kFix321CompositeStateTraceMaximumEntries = 1024U;

// Fix3.22 diagnostic-only generic Composite execution snapshot recorder. It
// observes the single statically confirmed direct call from FUN_00965A90 to
// FUN_009657F0. The retail function is invoked unchanged; only bounded raw
// before/after words and a module-relative first-pointer classifier are kept.
inline constexpr std::size_t kFix322CompositeDispatchTraceMaximumEntries = 256U;

enum class CausalTraceChannel : std::uint32_t {
    TypedMessage = 1U,
    InputEdge = 2U,
    LogicSwitch = 3U,
    CallbackBuild = 4U,
};

struct CausalTraceEntry final {
    std::uint64_t signatureHash{};
    std::uint64_t count{};
    CausalTraceChannel channel{CausalTraceChannel::TypedMessage};
    std::uint32_t worldMask{};
    CathodeRuntimeKey sourceRuntimeKey{};
    CathodeRuntimeKey targetRuntimeKey{};
    WorldEntityId sourceWorldEntity{};
    WorldEntityId targetWorldEntity{};
    std::uint32_t primary{};
    std::uint32_t secondary{};
    std::uint32_t payloadBytes{};
    std::uint64_t payloadHash{};
    std::uint64_t delayBits{};
    std::uint32_t tailArgument{};
    std::uint32_t callbackInput{};
    std::uint32_t callbackFlags{};
    CathodeRuntimeKey callbackRuntimeKey{};
    std::uint32_t callbackExecutionVtableRva{};
    std::uint32_t semanticFlags{};
    std::uint32_t executionMask{};
    std::uint32_t firstDeltaMs{};
    std::uint32_t lastDeltaMs{};
    std::uint32_t firstCallSiteRva{};
    std::uint32_t lastCallSiteRva{};
    bool callSiteVaried{};
};

struct CausalTraceSnapshot final {
    bool armed{};
    bool active{};
    std::uint32_t anchorRole{};
    std::uint32_t windowMs{};
    std::uint64_t anchors{};
    std::uint64_t inspected{};
    std::uint64_t accepted{};
    std::uint64_t uniqueEntries{};
    std::uint64_t capacityDrops{};
    std::uint64_t readFailures{};
    std::uint64_t originTickMs{};
    std::size_t entryCount{};
    std::array<CausalTraceEntry, kFix319CausalTraceMaximumEntries> entries{};
};

struct CompositeStateTraceEntry final {
    std::uint64_t signatureHash{};
    std::uint64_t count{};
    std::uint32_t helperRva{};
    std::uint32_t callerRva{};
    CathodeRuntimeKey runtimeKey{};
    WorldEntityId worldEntity{};
    std::uint32_t executionVtableRva{};
    std::uint32_t identityMask{};
    std::uint32_t valueReadMask{};
    std::uint32_t valueWord0{};
    std::uint32_t contextReadMask{};
    std::uint32_t contextWord0{};
    std::uint32_t contextWord1{};
    std::uint32_t objectReadMask{};
    std::uint32_t objectWord0{};
    std::uint32_t objectWord1{};
    std::uint32_t executionMask{};
    std::uint32_t firstDeltaMs{};
    std::uint32_t lastDeltaMs{};
};

struct CompositeStateTraceSnapshot final {
    bool hookInstalled{};
    bool armed{};
    bool active{};
    std::uint32_t windowMs{};
    std::uint64_t redirectCallSites{};
    std::uint64_t installFailures{};
    std::uint64_t inspected{};
    std::uint64_t accepted{};
    std::uint64_t uniqueEntries{};
    std::uint64_t capacityDrops{};
    std::uint64_t readFailures{};
    std::uint64_t limitDrops{};
    std::uint64_t originTickMs{};
    std::size_t entryCount{};
    std::array<CompositeStateTraceEntry, kFix321CompositeStateTraceMaximumEntries> entries{};
};

struct CompositeDispatchTraceEntry final {
    std::uint64_t signatureHash{};
    std::uint64_t count{};
    std::uint32_t targetRva{};
    std::uint32_t callerRva{};
    std::uint32_t firstPointerRva{};
    std::uint32_t firstPointerReadable{};
    std::uint64_t beforeHash{};
    std::uint64_t afterHash{};
    std::uint32_t changedWordMask{};
    std::uint32_t changedWordCount{};
    std::array<std::uint32_t, 4> changedOffsets{};
    std::array<std::uint32_t, 4> beforeWords{};
    std::array<std::uint32_t, 4> afterWords{};
    std::uint32_t executionMask{};
    std::uint32_t firstDeltaMs{};
    std::uint32_t lastDeltaMs{};
};

struct CompositeDispatchTraceSnapshot final {
    bool hookInstalled{};
    bool armed{};
    bool active{};
    std::uint32_t windowMs{};
    std::uint64_t redirectCallSites{};
    std::uint64_t installFailures{};
    std::uint64_t inspected{};
    std::uint64_t accepted{};
    std::uint64_t uniqueEntries{};
    std::uint64_t changedCalls{};
    std::uint64_t capacityDrops{};
    std::uint64_t readFailures{};
    std::uint64_t limitDrops{};
    std::uint64_t originTickMs{};
    std::size_t entryCount{};
    std::array<CompositeDispatchTraceEntry, kFix322CompositeDispatchTraceMaximumEntries> entries{};
};


inline constexpr std::size_t kPreQueueUnknownTraceCapacity = 4U;

struct PreQueueUnknownTraceEntry final {
    bool valid{};
    std::uint32_t inputShortGuid{};
    std::uint32_t callerRva{};
    CathodeRuntimeKey targetRuntimeKey{};
    std::uintptr_t targetExecution{};
    WorldEntityId targetId{};
    WorldEntityId bridgeOwner{};
    std::uint32_t outerPublicInput{};
};

struct CompositePublicInputDiagnostics final {
    bool initialized{};
    bool schedulerHookInstalled{};
    bool preQueueBuilderHookInstalled{};
    std::uint64_t version{};
    std::uint64_t mapped{};
    std::uint64_t runtimeEdgeMapped{};
    std::uint64_t ownerScopeRuntimeEdgeMapped{};
    // Fix3.27: scalar-only counters for the retail input-target constructor.
    // They distinguish whether the public ShortGuid still exists before
    // QueueInput, without arming any historical recorder or serializing runtime
    // identities.
    std::uint64_t preQueueBuilderSeen{};
    std::uint64_t preQueueBuilderInputReadable{};
    std::uint64_t preQueueBuilderPublic{};
    std::uint64_t preQueueBuilderInternal{};
    std::uint64_t preQueueBuilderInternalExactMapped{};
    std::uint64_t preQueueBuilderInternalRuntimeMapped{};
    std::uint64_t preQueueBuilderInternalOwnerScopeMapped{};
    std::uint64_t preQueueBuilderUnknownGuid{};
    std::uint64_t preQueueBuilderUnknownTraceCount{};
    std::array<PreQueueUnknownTraceEntry, kPreQueueUnknownTraceCapacity> preQueueBuilderUnknownTrace{};
    std::uint64_t preQueueBuilderMapped{};
    std::uint64_t preQueueBuilderOwnerMapped{};
    std::uint64_t preQueueBuilderInterfaceMapped{};
    std::uint64_t preQueueBuilderSubmitted{};
    std::uint64_t preQueueBuilderRemoteSkipped{};
    std::uint64_t preQueueBuilderDelayRejected{};
    std::uint64_t preQueueBuilderIdentityRejected{};
    std::uint64_t preQueueBuilderTypedRejected{};
    std::uint64_t preQueueBuilderRejected{};
    std::uint64_t submitted{};
    std::uint64_t remoteAttempts{};
    std::uint64_t bridgeQueued{};
    std::uint64_t sourceFallbacks{};
    std::uint64_t runtimeUnavailable{};
    std::uint64_t rejected{};
};

struct RuntimeDiagnostics final {
    bool initialized{};
    bool hookInstalled{};
    std::uint64_t version{};
    std::uint64_t initializationCount{};
    std::uint64_t shutdownCount{};
    std::uint64_t observedSchedulerCalls{};
    std::uint64_t relevantSourceCalls{};
    std::uint64_t submittedIntents{};
    std::uint64_t unresolvedSource{};
    std::uint64_t unresolvedTarget{};
    std::uint64_t callbackEdgesSkipped{};
    std::uint64_t invalidEdgesSkipped{};
    std::uint64_t remoteApplySuppressed{};
    std::uint64_t originEchoSuppressed{};
    std::uint64_t discardedPublicRootObservations{};
    std::uint64_t echoTableFull{};
    std::uint64_t intentQueueRejected{};
    std::uint64_t sourceClassMismatch{};
    std::uint64_t executeAttempts{};
    std::uint64_t queuedRemoteEdges{};
    // Patch 5.3.2 diagnostic-only WS3 Event-boundary gates. These counters
    // observe the three retail boundaries retained from the old investigation
    // (pre-QueueInput builder, QueueInput scheduler, QueueTypedMessage) without
    // enabling any legacy WS2 network operation.
    std::uint64_t ws3PreQueueSeen{};
    std::uint64_t ws3PreQueueInputReadable{};
    std::uint64_t ws3PreQueueTargetWorldResolved{};
    std::uint64_t ws3PreQueueTargetWorldUnresolved{};
    std::uint32_t ws3PreQueueLastInput{};
    std::uint32_t ws3PreQueueLastCallerRva{};
    // Patch 5.8 process-local pre-Queue canonical identity handoff. The
    // descriptor pointer used for correlation is intentionally not exposed.
    std::uint64_t ws3CanonicalPreQueueHintStored{};
    std::uint64_t ws3CanonicalPreQueueHintConsumed{};
    std::uint64_t ws3CanonicalPreQueueHintMissed{};
    std::uint64_t ws3CanonicalPreQueueSubmitAttempt{};
    std::uint64_t ws3CanonicalPreQueueSubmitSuccess{};
    std::uint64_t ws3CanonicalPreQueueAdmissionSuppressed{};
    std::uint64_t ws3CanonicalPreQueueSubmitFailed{};
    std::uint64_t ws3CanonicalQueueCandidateSeen{};
    std::uint64_t ws3CanonicalQueueMapped{};
    std::uint64_t ws3CanonicalQueueLocallyExecuted{};
    std::uint64_t ws3CanonicalQueueAdmissionSuppressed{};
    std::uint64_t ws3QueueSeen{};
    std::uint64_t ws3QueueDescendantSuppressed{};
    std::uint64_t ws3QueueCallbackRejected{};
    std::uint64_t ws3QueueDescribeRejected{};
    std::uint64_t ws3QueueRemoteNatural{};
    std::uint64_t ws3QueueRemoteApplySuppressed{};
    std::uint64_t ws3QueueDeferredComposite{};
    std::uint64_t ws3QueueStateOwned{};
    std::uint64_t ws3QueueEventEligible{};
    std::uint64_t ws3QueueUnknownEligible{};
    std::uint64_t ws3QueueSubmitRetailOnly{};
    std::uint64_t ws3QueueSubmitAwaitAuthority{};
    std::uint64_t ws3QueueSubmitAuthoritySuppressed{};
    std::uint64_t ws3QueueSubmitLocalRoot{};
    std::uint32_t ws3QueueLastSchedulerParameter{};
    std::uint32_t ws3QueueLastTargetInput{};
    std::uint32_t ws3QueueLastCallerRva{};
    std::uint32_t ws3QueueLastGate{};
    std::uint64_t ws3TypedSeen{};
    std::uint64_t ws3TypedDescendantSuppressed{};
    std::uint64_t ws3TypedWorldPairResolved{};
    std::uint64_t ws3TypedWorldPairUnresolved{};
    std::uint64_t ws3TypedRootRetailOnly{};
    std::uint64_t ws3TypedRootAwaitAuthority{};
    std::uint64_t ws3TypedRootAuthoritySuppressed{};
    std::uint64_t ws3TypedRootLocal{};
    // Patch 5.5: receiver-local interaction/task control messages are kept on
    // the originating process instead of becoming public TypedMessage ROOTs.
    std::uint64_t ws3TypedReceiverLocalSuppressed{};
    std::uint32_t ws3TypedLastReceiverLocalSuppressedType{};
    std::uint32_t ws3TypedLastMessageType{};
    std::uint32_t ws3TypedLastPayloadBytes{};
    std::uint32_t ws3TypedLastWorldMask{};
    std::uint32_t ws3TypedLastCallerRva{};
    // Patch 5.6: passive terminal-interaction pair probe. No additional filter
    // is applied to 0x43/0x44 yet; these fields prove caller/domain/primitive
    // provenance before deciding whether that valve is receiver-local.
    std::uint64_t ws3TypedInteractionPairSeen{};
    std::uint64_t ws3TypedInteractionPair43{};
    std::uint64_t ws3TypedInteractionPair44{};
    std::uint32_t ws3TypedInteractionPair43LastCallerRva{};
    std::uint32_t ws3TypedInteractionPair44LastCallerRva{};
    std::uint32_t ws3TypedInteractionPair43LastCausalSource{};
    std::uint32_t ws3TypedInteractionPair44LastCausalSource{};
    std::uint32_t ws3TypedInteractionPairLastType{};
    std::uint32_t ws3TypedInteractionPairLastCallerRva{};
    std::uint32_t ws3TypedInteractionPairLastSourcePrimitive{};
    std::uint32_t ws3TypedInteractionPairLastTargetPrimitive{};
    std::uint32_t ws3TypedInteractionPairLastSourceDomain{};
    std::uint32_t ws3TypedInteractionPairLastTargetDomain{};
    std::uint32_t ws3TypedInteractionPairLastCausalSource{};
    std::uint64_t ws3TypedInteractionPairLastSourceHigh{};
    std::uint64_t ws3TypedInteractionPairLastSourceLow{};
    std::uint64_t ws3TypedInteractionPairLastTargetHigh{};
    std::uint64_t ws3TypedInteractionPairLastTargetLow{};
    // Fix3.24 BuildFix01 permanent health counters for the functional public
    // Composite-input path. These are not an operator-armed probe.
    std::uint64_t compositePublicInputMapped{};
    std::uint64_t compositePublicInputSubmitted{};
    std::uint64_t compositePublicInputRemoteAttempts{};
    std::uint64_t compositePublicInputBridgeQueued{};
    std::uint64_t compositePublicInputSourceFallbacks{};
    std::uint64_t compositePublicInputRuntimeUnavailable{};
    std::uint64_t compositePublicInputRejected{};
    std::uint64_t nativeFaults{};
    std::uint64_t activeDetours{};
    bool typedMessageHookInstalled{};
    std::uint64_t observedTypedMessages{};
    std::uint64_t relevantTypedMessages{};
    std::uint64_t submittedTypedMessages{};
    std::uint64_t typedPayloadBytes{};
    std::uint64_t oversizedTypedMessagesSkipped{};
    std::uint64_t invalidTypedMessagesSkipped{};
    std::uint64_t unresolvedTypedSource{};
    std::uint64_t unresolvedTypedTarget{};
    std::uint64_t typedRuntimeKeyAnchoredByTarget{};
    std::uint64_t typedRuntimeKeyAnchoredBySource{};
    std::uint64_t typedRuntimeKeyNoAnchorSkipped{};
    std::uint64_t typedRuntimeKeySafetyBlocked{};
    std::uint32_t typedBlockedMessageType{};
    std::uint32_t typedBlockedPayloadBytes{};
    CathodeRuntimeKey typedBlockedSourceKey{};
    CathodeRuntimeKey typedBlockedTargetKey{};
    std::uint32_t typedBlockedMissingMask{};
    std::uint64_t typedRemoteApplySuppressed{};
    std::uint64_t typedOriginEchoSuppressed{};
    std::uint64_t typedContentMatchSuppressed{};
    std::uint64_t typedIntentQueueRejected{};
    std::uint64_t typedExecuteAttempts{};
    std::uint64_t queuedRemoteTypedMessages{};
    std::uint64_t typedNativeFaults{};
    // Fix3.13 diagnostic-only origin probe for the exact FUN_00930640 typed
    // message family that Fix3.12 proved constructs the causal Character
    // callback. These scalars are observation-only and never gate networking.
    bool typedMessageOriginProbeActive{};
    std::uint64_t typedMessageOriginCandidates{};
    std::uint64_t typedMessageOriginExactMatches{};
    std::uint64_t typedMessageOriginCallVerified{};
    std::uint64_t typedMessageOriginReadFailures{};
    std::uint32_t typedMessageOriginMessageType{};
    std::uint32_t typedMessageOriginPayloadBytes{};
    CathodeRuntimeKey typedMessageOriginSourceKey{};
    CathodeRuntimeKey typedMessageOriginTargetKey{};
    std::uint32_t typedMessageOriginWorldMask{};
    double typedMessageOriginDelaySeconds{};
    std::uint32_t typedMessageOriginFirstCallSiteRva{};
    std::uint32_t typedMessageOriginLastCallSiteRva{};
    bool typedMessageOriginCallSiteVaried{};
    std::uint32_t typedMessageOriginThreadId{};
    std::uint64_t typedMessageOriginTickMs{};
    // Fix3.14 bounded remote identity probe. The v3 typed payload transports
    // both stable WorldEntityIds in addition to the legacy sender RuntimeKeys.
    // These fields compare sender-local keys against the receiver manifest
    // mapping without changing the current replay acceptance policy.
    bool typedRemoteIdentityProbeActive{};
    std::uint64_t typedRemoteIdentityCandidates{};
    std::uint64_t typedRemoteIdentityExactMatches{};
    std::uint32_t typedRemoteIdentityMessageType{};
    std::uint32_t typedRemoteIdentityPayloadBytes{};
    WorldEntityId typedRemoteIdentitySourceWorld{};
    WorldEntityId typedRemoteIdentityTargetWorld{};
    CathodeRuntimeKey typedRemoteIdentitySenderSourceKey{};
    CathodeRuntimeKey typedRemoteIdentitySenderTargetKey{};
    CathodeRuntimeKey typedRemoteIdentityLocalSourceKey{};
    CathodeRuntimeKey typedRemoteIdentityLocalTargetKey{};
    std::uint32_t typedRemoteIdentityMask{};
    std::uint64_t typedRemoteIdentityExecuteReached{};
    std::uint64_t typedRemoteIdentityQueued{};
    std::uint64_t typedRemoteIdentityRuntimeUnavailable{};
    std::uint64_t typedRemoteIdentityNativeFaults{};
    std::uint64_t typedRemoteIdentityInvalidPayloads{};
    // Fix3.15 diagnostic-only synchronous downstream fingerprint for the exact
    // LogicSwitch CLEAR witness. It records normalized CATHODE QueueInput edges
    // traversed while the retail CLEAR call is on-stack. No replay decision
    // consumes these fields.
    bool logicDownstreamProbeActive{};
    std::uint64_t logicDownstreamScopes{};
    std::uint64_t logicDownstreamEdges{};
    std::uint64_t logicDownstreamValidEdges{};
    std::uint64_t logicDownstreamCallbackEdges{};
    std::uint64_t logicDownstreamReadFailures{};
    std::uint32_t logicDownstreamWorldMask{};
    CathodeRuntimeKey logicDownstreamSourceKey{};
    CathodeRuntimeKey logicDownstreamTargetKey{};
    std::uint32_t logicDownstreamSchedulerParameter{};
    std::uint32_t logicDownstreamTargetInput{};
    std::uint32_t logicDownstreamCallbackFlags{};
    std::uint32_t logicDownstreamFirstCallSiteRva{};
    std::uint32_t logicDownstreamLastCallSiteRva{};
    bool logicDownstreamCallSiteVaried{};
    std::uint64_t logicDownstreamSequenceHash{};
    std::uint32_t logicDownstreamThreadId{};
    std::uint64_t logicDownstreamTickMs{};
    // Fix3.16 diagnostic-only post-CLEAR anchored scheduler probe. Fix3.15
    // proved no synchronous QueueInput occurs inside the witness CLEAR. These
    // fields describe only QueueInput edges observed in a 250 ms bounded
    // post-CLEAR window whose source or target is that exact LogicSwitch key.
    bool postClearWindowActive{};
    std::uint64_t postClearArms{};
    std::uint64_t postClearSeen{};
    std::uint64_t postClearAnchorMatches{};
    std::uint64_t postClearValidEdges{};
    std::uint64_t postClearCallbackEdges{};
    std::uint64_t postClearReadFailures{};
    std::uint64_t postClearInspectLimitDrops{};
    std::uint32_t postClearAnchorRole{};
    std::uint32_t postClearWorldMask{};
    CathodeRuntimeKey postClearSourceKey{};
    CathodeRuntimeKey postClearTargetKey{};
    std::uint32_t postClearSchedulerParameter{};
    std::uint32_t postClearTargetInput{};
    std::uint32_t postClearCallbackFlags{};
    std::uint32_t postClearFirstCallSiteRva{};
    std::uint32_t postClearLastCallSiteRva{};
    bool postClearCallSiteVaried{};
    std::uint64_t postClearSequenceHash{};
    std::uint32_t postClearFirstDeltaMs{};
    std::uint32_t postClearLastDeltaMs{};
    std::uint32_t postClearThreadId{};
    std::uint64_t postClearTickMs{};
    // Fix3.18 diagnostic-only code/thunk provenance for the exact virtual
    // target captured by Fix3.17. The 32-byte sample is reduced to two leading
    // words plus a portable hash; only exact x86 JMP/CALL entry encodings are
    // decoded. No code pointer is invoked or serialized.
    bool clearCodeProbeActive{};
    std::uint64_t clearCodeCandidates{};
    std::uint64_t clearCodeExactMatches{};
    std::uint64_t clearCodeReadFailures{};
    std::uint32_t clearCodeTargetRva{};
    std::uint32_t clearCodeWord0{};
    std::uint32_t clearCodeWord1{};
    std::uint32_t clearCodeBranchKind{};
    std::uint32_t clearCodeBranchTargetRva{};
    std::uint32_t clearCodeFinalTargetRva{};
    std::uint32_t clearCodeJumpDepth{};
    std::uint64_t clearCodeHash{};
    std::uint32_t clearCodeThreadId{};
    std::uint64_t clearCodeTickMs{};
    // Fix3.17 diagnostic-only retail dispatch provenance. Fix3.16 proved that
    // the witness CLEAR does not return through QueueInput with the LogicSwitch
    // as an endpoint. These fields record the actual virtual target stored at
    // LogicSwitch vtable + 0x44 for the exact witnessed transition. The probe
    // is read-only and does not invoke, replace, or network this target.
    bool clearDispatchProbeActive{};
    std::uint64_t clearDispatchCandidates{};
    std::uint64_t clearDispatchExactMatches{};
    std::uint64_t clearDispatchReadFailures{};
    std::uint32_t clearDispatchVtableRva{};
    std::uint32_t clearDispatchSlot{};
    std::uint32_t clearDispatchTargetRva{};
    std::uint32_t clearDispatchFirstTargetRva{};
    std::uint32_t clearDispatchLastTargetRva{};
    bool clearDispatchTargetVaried{};
    std::uint32_t clearDispatchBeforeState{};
    std::uint32_t clearDispatchAfterState{};
    std::uint32_t clearDispatchThreadId{};
    std::uint64_t clearDispatchTickMs{};
    bool logicSwitchHookInstalled{};
    std::uint64_t logicSwitchObserved{};
    std::uint64_t logicSwitchWorldStates{};
    std::uint64_t logicSwitchSubmitted{};
    std::uint64_t logicSwitchUnresolved{};
    std::uint64_t logicSwitchRemoteSuppressed{};
    std::uint64_t logicSwitchIntentRejected{};
    std::uint64_t logicSwitchExecuteAttempts{};
    std::uint64_t logicSwitchAppliedRemote{};
    std::uint64_t logicSwitchRuntimeUnavailable{};
    std::uint64_t logicSwitchNativeFaults{};
    std::uint64_t logicSwitchBindingRegistrations{};
    std::uint64_t logicSwitchBindingHits{};
    std::uint64_t logicSwitchBindingMisses{};
    std::uint64_t logicSwitchBindingDrops{};
    std::uint64_t logicSwitchContextReplays{};
    std::uint64_t logicSwitchSyntheticContextReplays{};
    std::uint64_t logicSwitchSafetyBlocked{};
    std::uint64_t logicSwitchContextWorld{};
    std::uint64_t logicSwitchContextNonWorld{};
    CathodeRuntimeKey logicSwitchLastContextKey{};
    CathodeRuntimeKey logicSwitchLastStateKey{};
    bool logicSwitchLastEnabled{};
    // Fix3.11 diagnostic-only exact callback capture for the runtime key
    // previously correlated with the ChallengeMap16 unlock LogicSwitch.
    std::uint64_t logicExactTargetMatches{};
    std::uint64_t logicExactHolderReadable{};
    std::uint64_t logicExactCallbackPresent{};
    std::uint64_t logicExactRawReadable{};
    std::uint64_t logicExactRawReadFailures{};
    std::uint64_t logicExactCallbackObjectPresent{};
    std::uint64_t logicExactCallbackKeyResolved{};
    std::uint64_t logicExactCallbackExecutionReadable{};
    std::uint32_t logicExactInput{};
    std::uint32_t logicExactFlags{};
    std::uint32_t logicExactRefCount{};
    bool logicExactNestedPresent{};
    bool logicExactPayloadPresent{};
    CathodeRuntimeKey logicExactCallbackKey{};
    std::uint32_t logicExactCallbackExecutionVtableRva{};
    bool logicExactContextSameAsState{};
    bool logicExactEnabled{};
    // Fix3.12 diagnostic-only constructor-origin probe for the exact Character
    // callback family captured by Fix3.11. No replay/network branch consumes it.
    bool characterCallbackOriginHookInstalled{};
    bool characterCallbackOriginProbeActive{};
    std::uint64_t characterCallbackOriginCandidates{};
    std::uint64_t characterCallbackOriginExactKeyMatches{};
    std::uint64_t characterCallbackOriginCallVerified{};
    std::uint64_t characterCallbackOriginReadFailures{};
    CathodeRuntimeKey characterCallbackOriginSourceKey{};
    std::uint32_t characterCallbackOriginExecutionVtableRva{};
    std::uint32_t characterCallbackOriginFlags{};
    std::uint32_t characterCallbackOriginPayloadBytes{};
    std::uint32_t characterCallbackOriginFirstCallSiteRva{};
    std::uint32_t characterCallbackOriginLastCallSiteRva{};
    bool characterCallbackOriginCallSiteVaried{};
    std::uint32_t characterCallbackOriginThreadId{};
    std::uint64_t characterCallbackOriginTickMs{};
    std::uint64_t schedulerCausalEdgesRecorded{};
    std::uint64_t logicSwitchCausalNestedMatches{};
    std::uint64_t logicSwitchCausalEndpointMatches{};
    std::uint64_t logicSwitchCausalRecentMatches{};
    std::uint64_t logicSwitchCausalCallbackMatches{};
    std::uint64_t logicSwitchCausalNoMatches{};
    std::uint64_t logicSwitchCausalLastAgeMs{};
    WorldEntityId logicSwitchCausalLastSource{};
    WorldEntityId logicSwitchCausalLastTarget{};
    WorldEntityId logicSwitchCausalLastState{};
    std::uint32_t logicSwitchCausalLastSchedulerParameter{};
    std::uint32_t logicSwitchCausalLastTargetInput{};
    bool logicSwitchCausalLastCallback{};
    bool logicSwitchCausalLastEnabled{};
    WorldEntityId logicSwitchCausalLastCallbackEntity{};
    std::uint32_t logicSwitchCausalLastCallbackExecutionVtableRva{};
    std::uint32_t logicSwitchCausalLastQueueCallerRva{};
    bool logicSwitchCausalLastCallbackExecutionReadable{};
    bool logicSwitchCausalLastCallbackExecutionPresent{};
    bool logicSwitchCausalLastSourceRuntimeKeyValid{};
    bool logicSwitchCausalLastTargetRuntimeKeyValid{};
    bool logicSwitchCausalLastCallbackRuntimeKeyValid{};
    std::uint32_t logicSwitchCausalLastSourceRuntimeKeyA{};
    std::uint32_t logicSwitchCausalLastSourceRuntimeKeyB{};
    std::uint32_t logicSwitchCausalLastTargetRuntimeKeyA{};
    std::uint32_t logicSwitchCausalLastTargetRuntimeKeyB{};
    std::uint32_t logicSwitchCausalLastCallbackRuntimeKeyA{};
    std::uint32_t logicSwitchCausalLastCallbackRuntimeKeyB{};
    std::uint32_t logicSwitchCausalLastStateRuntimeKeyA{};
    std::uint32_t logicSwitchCausalLastStateRuntimeKeyB{};
    bool logicSwitchCausalLastCallbackWorldEntity{};
    bool logicSwitchCausalLastCallbackSameAsSource{};
    bool logicSwitchCausalLastCallbackSameAsTarget{};
    bool logicSwitchCausalLastCallbackRawReadable{};
    std::array<std::uint32_t, 10> logicSwitchCausalLastCallbackRawWords{};
    std::uint64_t callbackRecordsObserved{};
    std::uint64_t callbackExecutionReadable{};
    std::uint64_t callbackWorldEntityResolved{};
    std::uint64_t callbackSameAsSource{};
    std::uint64_t callbackSameAsTarget{};
    std::uint64_t callbackRawReadable{};
    // Historical Fix3.20 replay is disabled while WS3 strict is installed.
    bool characterCallbackReplayActive{};
    std::uint64_t characterCallbackReplayObserved{};
    std::uint64_t characterCallbackReplayPortable{};
    std::uint64_t characterCallbackReplayParentSuppressed{};
    std::uint64_t characterCallbackReplayNestedSuppressed{};
    std::uint64_t characterCallbackReplaySubmitted{};
    std::uint64_t characterCallbackReplayContentSuppressed{};
    std::uint64_t characterCallbackReplayExecuteAttempts{};
    std::uint64_t characterCallbackReplayQueued{};
    std::uint64_t characterCallbackReplayRuntimeUnavailable{};
    std::uint64_t characterCallbackReplayNativeFaults{};
    std::uint64_t characterCallbackReplayInvalid{};
    std::uint64_t characterCallbackReplayLimitDrops{};
    std::uint64_t callbackReferenceFamilyObserved{};
    std::uint64_t callbackReferencePortable{};
    std::uint64_t callbackReferenceSubmitted{};
    std::uint64_t callbackReferenceUnresolved{};
    std::uint64_t callbackReferenceExecuteAttempts{};
    std::uint64_t callbackReferenceAppliedRemote{};
    std::uint64_t callbackReferenceRuntimeUnavailable{};
    std::uint64_t callbackReferenceNativeFaults{};
    std::uint64_t callbackReferenceSettlingSkipped{};
    std::uint64_t callbackReferenceThrottled{};
    std::uint64_t callbackAbiProbeCallerMatches{};
    std::uint64_t callbackAbiProbePairMatches{};
    std::uint64_t callbackAbiProbeInputMatches{};
    std::uint64_t callbackAbiProbeSamplesCaptured{};
    std::uint64_t callbackAbiProbeSampleLimitDrops{};
    std::uint64_t callbackAbiProbeRawReadFailures{};
    std::uint64_t callbackAbiProbeIdentityAttempts{};
    std::uint64_t callbackAbiProbeIdentityLimitDrops{};
    std::uint64_t callbackAbiProbeRuntimeKeyAttempts{};
    std::uint64_t callbackAbiProbeSourceKeyResolved{};
    std::uint64_t callbackAbiProbeTargetKeyResolved{};
    std::uint64_t callbackAbiProbeCallbackKeyResolved{};
    std::uint64_t callbackAbiProbeCallbackObjectPresent{};
    std::uint64_t callbackAbiProbeTargetResolved{};
    std::uint32_t callbackAbiProbeLastInputShortGuid{};
    std::uint32_t callbackAbiProbeFirstFlags{};
    std::uint32_t callbackAbiProbeLastFlags{};
    std::uint32_t callbackAbiProbeLastRefCount{};
    std::uint32_t callbackAbiProbeLastMatchKind{};
    bool callbackAbiProbeFlagsVaried{};
    bool callbackAbiProbeLastInputIsCallback{};
    bool callbackAbiProbeLastNestedPresent{};
    bool callbackAbiProbeLastPayloadPresent{};
    bool callbackAbiProbeLastTargetResolved{};
    bool callbackAbiProbeLastTargetSameAsSource{};
    bool callbackAbiProbeLastTargetSameAsTarget{};
    bool callbackAbiProbeLastTargetSameAsState{};
    WorldEntityId callbackAbiProbeLastTargetEntity{};
};

[[nodiscard]] constexpr ModuleReport Report() noexcept {
    return {
        "WorldSync2.GenericCathodeEdges",
        ModuleStatus::Implemented,
        "v13.3 Fix3.27 BuildFix01 preserves CacheFix02, AIMPWM05 and the public Composite-input receiver/wire. The pre-QueueInput hook at FUN_00921F70 now accepts only two manifest-stable target identities: the owner CompositeInstance or the exact public InterfaceVariableEntity. Both paths remain payloadless/zero-delay/fail-closed; RuntimeKeys/callbacks remain process-local and historical broad replay stays blocked."};
}

[[nodiscard]] bool EncodeObservedInputEdgePayload(
    const ObservedInputEdge& value,
    WorldPayload& output) noexcept;
[[nodiscard]] bool DecodeObservedInputEdgePayload(
    std::span<const std::byte> payload,
    ObservedInputEdge& output) noexcept;
[[nodiscard]] bool EncodeObservedCallbackInputEdgePayload(
    const ObservedCallbackInputEdge& value,
    WorldPayload& output) noexcept;
[[nodiscard]] bool DecodeObservedCallbackInputEdgePayload(
    std::span<const std::byte> payload,
    ObservedCallbackInputEdge& output) noexcept;
[[nodiscard]] bool EncodeObservedCharacterCallbackInputEdgePayload(
    const ObservedCharacterCallbackInputEdge& value,
    WorldPayload& output) noexcept;
[[nodiscard]] bool DecodeObservedCharacterCallbackInputEdgePayload(
    std::span<const std::byte> payload,
    ObservedCharacterCallbackInputEdge& output) noexcept;
[[nodiscard]] bool PrepareObservedTypedMessagePayload(
    const ObservedTypedMessage& value,
    std::size_t dataLength,
    WorldPayload& output) noexcept;
[[nodiscard]] bool EncodeObservedTypedMessagePayload(
    const ObservedTypedMessage& value,
    std::span<const std::byte> data,
    WorldPayload& output) noexcept;
[[nodiscard]] bool DecodeObservedTypedMessagePayload(
    std::span<const std::byte> payload,
    ObservedTypedMessage& output,
    std::span<const std::byte>& data) noexcept;
[[nodiscard]] bool EncodeObservedLogicSwitchStatePayload(
    const ObservedLogicSwitchState& value,
    WorldPayload& output) noexcept;
[[nodiscard]] bool DecodeObservedLogicSwitchStatePayload(
    std::span<const std::byte> payload,
    ObservedLogicSwitchState& output) noexcept;
[[nodiscard]] bool EncodeCompositePublicInputPayload(
    const CompositePublicInputEvent& value,
    WorldPayload& output) noexcept;
[[nodiscard]] bool DecodeCompositePublicInputPayload(
    std::span<const std::byte> payload,
    CompositePublicInputEvent& output) noexcept;

bool Initialize(
    const SignatureResolver& resolver,
    HookManager& hooks,
    WorldEntityRegistry& registry,
    std::string& error) noexcept;
// PlayerSync-local observation lane. The callback receives only process-local
// values already present at retail FUN_00921F70 and must remain noexcept. It is
// never serialized or used by WorldSync identity logic.
void SetLocalInputTargetObserver(LocalInputTargetObserver observer) noexcept;
[[nodiscard]] bool LocalInputTargetObservationReady() noexcept;
// Installs generic event and LogicSwitch SET/CLEAR observers only after a
// compatible runtime manifest has been loaded. Fix3.21 may additionally install
// 19 fail-closed direct-CALL redirects for its observation-only state/value
// recorder; failure of that optional recorder does not disable stable WorldSync.
// No new native replay path is added here.
[[nodiscard]] bool ArmCapture(std::string& error) noexcept;
[[nodiscard]] bool Initialized() noexcept;
// Historical Fix3.4-Fix3.8 read-only callback classifier. Fix3.9 leaves this
// available for targeted research but WORLD_INTERACTION_STATS no longer arms
// it during normal runtime-key transport validation.
void ArmCallbackClassificationProbe() noexcept;
// Fix3.12: arms a 30-second read-only origin window. The FUN_00926FF0 detour
// remains a strict retail pass-through and records only the already-proven
// Character callback family (flags 0x12000000, no payload, Character vtable).
void ArmCharacterCallbackOriginProbe() noexcept;
// Fix3.13: arms a 30-second read-only origin window inside the already-installed
// FUN_00930640 typed-message detour. It records only scalar provenance for the
// exact family identified by Fix3.12 and never changes transport/replay policy.
void ArmTypedMessageOriginProbe() noexcept;
// Fix3.14: arms a 30-second receiver-side identity window. The v3 typed payload
// carries stable source/target WorldEntityIds, but this probe only compares
// sender RuntimeKeys with receiver manifest RuntimeKeys. No remap is executed.
void ArmRemoteTypedIdentityProbe() noexcept;
// Fix3.15/Fix3.16: arms a 30-second read-only downstream research window.
// Fix3.15 fingerprints QueueInput calls synchronous with the exact witness
// CLEAR; Fix3.16 additionally opens a 250 ms bounded post-CLEAR window and
// retains only edges anchored to that LogicSwitch RuntimeKey. Retail execution
// and all replay/transport policies remain unchanged.
void ArmLogicDownstreamProbe() noexcept;
// Fix3.19: arms a 30-second wait for the already-proven typed witness. The
// first local exact witness (Host) or remote exact witness (Client) starts a
// one-second bounded causal flight recorder over the existing typed-message
// and QueueInput hooks. Observation only; all replay safety gates are unchanged.
void ArmCausalDifferentialTrace() noexcept;
// Fix3.21: arms one immediate 60-second, observation-only differential window
// over the certified generic CATHODE state/value helper call family.
void ArmCompositeStateDifferentialTrace() noexcept;
[[nodiscard]] CompositeStateTraceSnapshot CompositeStateTraceDiagnosticsSnapshot() noexcept;
// Fix3.22: arms one immediate 60-second, observation-only window around the
// statically confirmed FUN_00965A90 -> FUN_009657F0 Composite execution choke
// point. No observed bytes are transported or replayed.
void ArmCompositeDispatchDifferentialTrace() noexcept;
[[nodiscard]] CompositeDispatchTraceSnapshot CompositeDispatchTraceDiagnosticsSnapshot() noexcept;
// Fix3.20: 30-second bounded experimental replay window; WS3 strict refuses to arm it.
void ArmCharacterCallbackReplayExperiment() noexcept;
[[nodiscard]] CausalTraceSnapshot CausalTraceDiagnosticsSnapshot() noexcept;
void ObserveRemoteTypedIdentity(
    const ObservedTypedMessage& message,
    std::size_t dataBytes,
    const WorldEntityId& mutationEntity,
    const CathodeRuntimeKey& resolvedTargetKey,
    bool rawGateAccepted) noexcept;
void ObserveRemoteTypedReplayResult(
    const ObservedTypedMessage& message,
    std::size_t dataBytes,
    ExecuteStatus status,
    bool queued) noexcept;
[[nodiscard]] CompositePublicInputDiagnostics CompositePublicInputDiagnosticsSnapshot() noexcept;
[[nodiscard]] RuntimeDiagnostics DiagnosticsSnapshot() noexcept;

// Returns true only while this process still owns the observation slot for a
// locally executed retail edge. The non-destructive form is needed across a
// network reconnect because the authoritative mutation retains the original
// SourcePeer while the reconnecting client receives a new PeerId.
[[nodiscard]] bool HasLocalObservation(std::uint64_t observationId) noexcept;
// Consumes the exact local observation at the transaction execution boundary.
[[nodiscard]] bool ConsumeLocalObservation(std::uint64_t observationId) noexcept;
[[nodiscard]] bool ConsumeLocalTypedMessage(
    const ObservedTypedMessage& message,
    std::span<const std::byte> data) noexcept;
[[nodiscard]] bool ConsumeLocalLogicSwitchState(std::uint64_t observationId) noexcept;

[[nodiscard]] ExecuteResult Execute(
    const ObservedInputEdge& edge,
    const WorldEntityLease& target,
    const WorldEntityLease& source) noexcept;
[[nodiscard]] ExecuteResult ExecuteCallbackInputEdge(
    const ObservedCallbackInputEdge& edge,
    const WorldEntityLease& target,
    const WorldEntityLease& source) noexcept;
[[nodiscard]] ExecuteResult ExecuteCharacterCallbackInputEdge(
    const ObservedCharacterCallbackInputEdge& edge,
    const WorldEntityLease& target,
    const WorldEntityLease& source,
    const WorldEntityLease& callback) noexcept;
[[nodiscard]] bool ConsumeLocalCharacterCallbackMatch(
    const ObservedCharacterCallbackInputEdge& edge,
    const WorldEntityId& targetEntity) noexcept;
[[nodiscard]] ExecuteResult ExecuteTypedMessage(
    const ObservedTypedMessage& message,
    std::span<const std::byte> data) noexcept;
[[nodiscard]] ExecuteResult ExecuteLogicSwitchState(
    const ObservedLogicSwitchState& state,
    const CathodeRuntimeKey& targetKey,
    const WorldEntityLease& target) noexcept;
[[nodiscard]] ExecuteResult ExecuteCanonicalCompositePublicInput(
    const CompositePublicInputEvent& event,
    const WorldEntityId& ownerCompositeInstance,
    const WorldEntityLease& target,
    WorldEntityRegistry& registry) noexcept;
[[nodiscard]] ExecuteResult ExecuteCompositePublicInput(
    const CompositePublicInputEvent& event,
    const WorldEntityId& ownerCompositeInstance,
    const WorldEntityLease& target,
    WorldEntityRegistry& registry) noexcept;

void ResetForGeneration() noexcept;
void BeginLogicalShutdown() noexcept;

} // namespace world_interactions
} // namespace aimp
