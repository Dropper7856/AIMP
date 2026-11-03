#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aimp {

inline constexpr std::uint32_t kWorldManifestFormatVersion = 9U;
inline constexpr std::uint32_t kWorldManifestCompilerVersion = 0x00100010U;

// WorldSync keeps transport identity independent from CATHODE execution. The
// registry may discover all domains; v12.5 only exposes evidence-backed generic
// primitives and still leaves object-specific adapters to later versions.
enum class WorldDomain : std::uint8_t {
    Unknown = 0,
    World,
    AI,
    Player,
    UI,
    AIMP,
};

enum class WorldEntityKind : std::uint8_t {
    Unknown = 0,
    Function,
    Variable,
    Proxy,
    Alias,
    CompositeInstance,
    Override,
};

enum class WorldReferenceKind : std::uint8_t {
    Proxy = 0,
    Alias,
};

enum WorldEntityFlags : std::uint16_t {
    WorldEntityFlagNone = 0U,
    WorldEntityFlagEnvironmentPrimitive = 1U << 0U,
    WorldEntityFlagNondeterministicBoundary = 1U << 1U,
    WorldEntityFlagZonePrimitive = 1U << 2U,
    WorldEntityFlagCompositeInstance = 1U << 3U,
    WorldEntityFlagHasOutgoingLinks = 1U << 4U,
    WorldEntityFlagHasOverrides = 1U << 5U,
    WorldEntityFlagSourceFromPak = 1U << 6U,
    WorldEntityFlagSourceFromBin = 1U << 7U,
    WorldEntityFlagRuntimeAddressable = 1U << 8U,
    WorldEntityFlagPlayerMissionComplete = 1U << 9U,
};

struct WorldEntityId final {
    std::uint64_t high{};
    std::uint64_t low{};

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return high != 0U || low != 0U;
    }

    [[nodiscard]] constexpr bool operator==(const WorldEntityId& other) const noexcept {
        return high == other.high && low == other.low;
    }
};

struct WorldCompositeDescriptor final {
    std::uint32_t compositeShortGuid{};
    std::string name;
    WorldDomain domain{WorldDomain::Unknown};
    bool aimpNamespace{};
    bool instantiated{};
};

struct WorldInstancePathDescriptor final {
    std::uint32_t pathId{};
    std::vector<std::uint32_t> steps;
    std::uint64_t hierarchyHash{};
    std::uint32_t cathodeHierarchyId{};
};

struct WorldEntityDescriptor final {
    WorldEntityId id{};
    WorldDomain domain{WorldDomain::Unknown};
    WorldEntityKind kind{WorldEntityKind::Unknown};
    std::uint16_t flags{WorldEntityFlagNone};
    std::uint32_t sourceCompositeShortGuid{};
    std::uint32_t sourceShortGuid{};
    std::uint32_t instancePathId{};
    std::uint64_t hierarchyHash{};
    std::uint32_t cathodeEntityA{};
    std::uint32_t cathodeEntityB{};
    std::uint32_t zoneId{};
    std::uint32_t primitiveType{};
    std::string primitiveName;
    std::string entityName;
};

// Fix3.24 static bridge emitted from COMMANDS/CathodeLib only. It contains
// stable manifest IDs and ShortGuids; no runtime pointer/reference is persisted.
struct WorldCompositePublicInputDescriptor final {
    WorldEntityId ownerCompositeInstance{};
    WorldEntityId interfaceVariableEntity{};
    std::uint32_t publicInputShortGuid{};
    std::uint32_t interfaceLinkParameter{};
    WorldEntityId internalTargetEntity{};
    std::uint32_t internalTargetInputShortGuid{};
    std::uint32_t dataType{};
    bool noTypedData{};
    bool internalFallbackExclusive{};
};

// BuildFix05 manifest-only downstream canonicalization anchor. The runtime
// observer may use this to map a proven internal set_false endpoint back to a
// unique public Composite input, but the internal identity is never networked.
struct WorldCompositePublicInputCanonicalAnchor final {
    WorldEntityId ownerCompositeInstance{};
    std::uint32_t publicInputShortGuid{};
    WorldEntityId internalTargetEntity{};
    std::uint32_t internalTargetInputShortGuid{};
};

enum WorldCausalDependencyFlags : std::uint32_t {
    WorldCausalDependencyNone = 0U,
    WorldCausalDependencyWorldEffect = 1U << 0U,
    WorldCausalDependencyPlayer = 1U << 1U,
    WorldCausalDependencyAI = 1U << 2U,
    WorldCausalDependencyUI = 1U << 3U,
    WorldCausalDependencyAIMP = 1U << 4U,
    WorldCausalDependencyUnknown = 1U << 5U,
    WorldCausalDependencyTruncated = 1U << 6U,
    WorldCausalDependencyBranching = 1U << 7U,
};

enum class WorldReplicationRelevanceDisposition : std::uint32_t {
    Unknown = 0U,
    NotRelevant = 1U,
    DeferToDescendant = 2U,
    ReplicateIntrinsic = 3U,
    ReplicateCarrier = 4U,
};

enum class WorldReplicationSemanticClass : std::uint32_t {
    Unknown = 0U,
    RoutingOnly = 1U,
    SchedulerMaintenanceOnly = 2U,
    InternalStateOnly = 3U,
    SharedWorldMutation = 4U,
    SharedPhysicsMutation = 5U,
    SharedLifecycleMutation = 6U,
    CompositeRouting = 7U,
};

enum class WorldReplicationContractKind : std::uint32_t {
    Unknown = 0U,
    Event = 1U,
    State = 2U,
    StateTransition = 3U,
};

enum WorldReplicationRelevanceEvidence : std::uint32_t {
    WorldReplicationEvidenceNone = 0U,
    WorldReplicationEvidenceReplicableSinkReachable = 1U << 8U,
    WorldReplicationEvidenceRoutingOnly = 1U << 9U,
    WorldReplicationEvidenceSchedulerMaintenance = 1U << 10U,
    WorldReplicationEvidenceInternalStateOnly = 1U << 11U,
    WorldReplicationEvidenceIntrinsicSharedEffect = 1U << 12U,
    WorldReplicationEvidenceEffectBeforeOutput = 1U << 13U,
    WorldReplicationEvidenceCompleteDescendantFrontier = 1U << 14U,
    WorldReplicationEvidenceReplayHistorySensitive = 1U << 15U,
    WorldReplicationEvidenceCycleAmbiguous = 1U << 16U,
    WorldReplicationEvidenceSemanticUnknown = 1U << 17U,
    WorldReplicationEvidenceNoReplicableSink = 1U << 18U,
    WorldReplicationEvidenceNativeHandler = 1U << 19U,
    WorldReplicationEvidenceStructuralGraph = 1U << 20U,
    WorldReplicationEvidenceDirectUnhookedSink = 1U << 21U,
    WorldReplicationEvidenceExactInputSemantic = 1U << 22U,
    WorldReplicationEvidenceStateTransitionCarrier = 1U << 23U,
    WorldReplicationEvidenceCompositeStateTransitionCarrier = 1U << 24U,
    WorldReplicationEvidenceCompositeRelevantFrontierRelaxed = 1U << 25U,
    WorldReplicationEvidenceEmittedEventCarrier = 1U << 26U,
};

inline constexpr std::uint32_t kWorldReplicationDispositionMask = 0x7U;
inline constexpr std::uint32_t kWorldReplicationSemanticClassShift = 3U;
inline constexpr std::uint32_t kWorldReplicationSemanticClassMask =
    0x1FU << kWorldReplicationSemanticClassShift;
inline constexpr std::uint32_t kWorldReplicationKnownEvidenceMask =
    WorldReplicationEvidenceReplicableSinkReachable |
    WorldReplicationEvidenceRoutingOnly |
    WorldReplicationEvidenceSchedulerMaintenance |
    WorldReplicationEvidenceInternalStateOnly |
    WorldReplicationEvidenceIntrinsicSharedEffect |
    WorldReplicationEvidenceEffectBeforeOutput |
    WorldReplicationEvidenceCompleteDescendantFrontier |
    WorldReplicationEvidenceReplayHistorySensitive |
    WorldReplicationEvidenceCycleAmbiguous |
    WorldReplicationEvidenceSemanticUnknown |
    WorldReplicationEvidenceNoReplicableSink |
    WorldReplicationEvidenceNativeHandler |
    WorldReplicationEvidenceStructuralGraph |
    WorldReplicationEvidenceDirectUnhookedSink |
    WorldReplicationEvidenceExactInputSemantic |
    WorldReplicationEvidenceStateTransitionCarrier |
    WorldReplicationEvidenceCompositeStateTransitionCarrier |
    WorldReplicationEvidenceCompositeRelevantFrontierRelaxed |
    WorldReplicationEvidenceEmittedEventCarrier;

struct WorldCompositePublicInputCausalProfile final {
    WorldEntityId ownerCompositeInstance{};
    std::uint32_t publicInputShortGuid{};
    std::uint32_t dependencyFlags{};
    std::uint32_t endpointKind{};
    std::uint32_t replicationRelevance{};
    WorldReplicationContractKind replicationContractKind{WorldReplicationContractKind::Unknown};
    WorldEntityId semanticTargetEntity{};
    std::uint32_t semanticOperation{};

    [[nodiscard]] constexpr bool Has(std::uint32_t flag) const noexcept {
        return (dependencyFlags & flag) != 0U;
    }
    [[nodiscard]] constexpr WorldReplicationRelevanceDisposition RelevanceDisposition() const noexcept {
        return static_cast<WorldReplicationRelevanceDisposition>(
            replicationRelevance & kWorldReplicationDispositionMask);
    }
    [[nodiscard]] constexpr WorldReplicationSemanticClass SemanticClass() const noexcept {
        return static_cast<WorldReplicationSemanticClass>(
            (replicationRelevance & kWorldReplicationSemanticClassMask) >>
            kWorldReplicationSemanticClassShift);
    }
    [[nodiscard]] constexpr std::uint32_t RelevanceEvidence() const noexcept {
        return replicationRelevance & kWorldReplicationKnownEvidenceMask;
    }
    [[nodiscard]] constexpr bool HasRelevanceEvidence(std::uint32_t flag) const noexcept {
        return (RelevanceEvidence() & flag) != 0U;
    }
    [[nodiscard]] constexpr bool HasSemanticContract() const noexcept {
        return replicationContractKind != WorldReplicationContractKind::Unknown &&
            semanticTargetEntity.Valid() && semanticOperation != 0U;
    }
    [[nodiscard]] constexpr bool ActorIndependentWorldDomain() const noexcept {
        return Has(WorldCausalDependencyWorldEffect) &&
            !Has(WorldCausalDependencyPlayer | WorldCausalDependencyAI |
                WorldCausalDependencyUI | WorldCausalDependencyAIMP |
                WorldCausalDependencyUnknown | WorldCausalDependencyTruncated);
    }
    // Historical helper retained for diagnostics that specifically need the
    // pre-WM08 non-branching condition. Network admission uses
    // NetworkReplicableBoundary() below.
    [[nodiscard]] constexpr bool ActorIndependentWorldBoundary() const noexcept {
        return ActorIndependentWorldDomain() && !Has(WorldCausalDependencyBranching);
    }
    [[nodiscard]] constexpr bool ReplicationAllowed() const noexcept {
        const auto disposition = RelevanceDisposition();
        return disposition == WorldReplicationRelevanceDisposition::ReplicateIntrinsic ||
            disposition == WorldReplicationRelevanceDisposition::ReplicateCarrier;
    }
    [[nodiscard]] constexpr bool CertifiedExactIntrinsicBoundary() const noexcept {
        return RelevanceDisposition() == WorldReplicationRelevanceDisposition::ReplicateIntrinsic &&
            Has(WorldCausalDependencyWorldEffect) &&
            HasRelevanceEvidence(WorldReplicationEvidenceIntrinsicSharedEffect) &&
            HasRelevanceEvidence(WorldReplicationEvidenceNativeHandler) &&
            HasRelevanceEvidence(WorldReplicationEvidenceExactInputSemantic);
    }
    [[nodiscard]] constexpr bool CertifiedEmittedStateTransitionCarrierBoundary() const noexcept {
        return endpointKind == 2U &&
            RelevanceDisposition() == WorldReplicationRelevanceDisposition::ReplicateCarrier &&
            replicationContractKind == WorldReplicationContractKind::StateTransition &&
            HasSemanticContract() && Has(WorldCausalDependencyWorldEffect) &&
            !Has(WorldCausalDependencyUnknown | WorldCausalDependencyTruncated) &&
            HasRelevanceEvidence(WorldReplicationEvidenceStateTransitionCarrier);
    }
    [[nodiscard]] constexpr bool CertifiedEmittedEventCarrierBoundary() const noexcept {
        return endpointKind == 2U &&
            RelevanceDisposition() == WorldReplicationRelevanceDisposition::ReplicateCarrier &&
            replicationContractKind == WorldReplicationContractKind::Event &&
            HasSemanticContract() && Has(WorldCausalDependencyWorldEffect) &&
            !Has(WorldCausalDependencyUnknown | WorldCausalDependencyTruncated) &&
            HasRelevanceEvidence(WorldReplicationEvidenceEmittedEventCarrier);
    }
    [[nodiscard]] constexpr bool CertifiedCompositeStateTransitionCarrierBoundary() const noexcept {
        const bool relaxedFrontier =
            HasRelevanceEvidence(WorldReplicationEvidenceCompositeRelevantFrontierRelaxed);
        return endpointKind == 1U &&
            RelevanceDisposition() == WorldReplicationRelevanceDisposition::ReplicateCarrier &&
            replicationContractKind == WorldReplicationContractKind::StateTransition &&
            HasSemanticContract() && Has(WorldCausalDependencyWorldEffect) &&
            !Has(WorldCausalDependencyTruncated) &&
            (!Has(WorldCausalDependencyUnknown) || relaxedFrontier) &&
            HasRelevanceEvidence(WorldReplicationEvidenceCompositeStateTransitionCarrier) &&
            HasRelevanceEvidence(WorldReplicationEvidenceCompleteDescendantFrontier) &&
            (!HasRelevanceEvidence(WorldReplicationEvidenceCycleAmbiguous) || relaxedFrontier) &&
            !HasRelevanceEvidence(WorldReplicationEvidenceSemanticUnknown);
    }
    [[nodiscard]] constexpr bool CertifiedStateTransitionCarrierBoundary() const noexcept {
        return CertifiedEmittedStateTransitionCarrierBoundary() ||
            CertifiedCompositeStateTransitionCarrierBoundary();
    }
    [[nodiscard]] constexpr bool NetworkReplicableBoundary() const noexcept {
        if (!ReplicationAllowed()) return false;
        // Exact intrinsic mutations keep the existing WM08 admission. Patch 4
        // adds the immediately captured EmittedOutput before a unique exact
        // StateTransition ScheduledInput. Patch 5.1 additionally permits a
        // public CompositeInput when all known relevant descendants converge on
        // one exact StateTransition semantic target/operation. Non-truncated
        // UnknownDependency side paths and cycle ambiguity are allowed only when
        // the compiler marks the explicit relaxed-frontier evidence bit. These compiler-proven
        // carrier classes may bypass broad actor-domain reachability because
        // semantic convergence, not an entity name/GUID heuristic, selects the
        // replay boundary. All other carriers
        // retain the conservative actor-independent-domain requirement.
        return CertifiedExactIntrinsicBoundary() ||
            CertifiedEmittedEventCarrierBoundary() ||
            CertifiedStateTransitionCarrierBoundary() ||
            ActorIndependentWorldDomain();
    }
};

struct WorldConnectionDescriptor final {
    WorldEntityId sourceEntity{};
    std::uint32_t sourceParameter{};
    WorldEntityId targetEntity{};
    std::uint32_t targetParameter{};
    bool potentialCausalEdge{};
};

struct WorldOverrideDescriptor final {
    WorldEntityId ownerCompositeInstance{};
    std::uint32_t parameterName{};
    std::uint32_t dataType{};
    std::uint32_t parameterVariant{};
};

struct WorldReferenceDescriptor final {
    WorldEntityId ownerEntity{};
    WorldReferenceKind kind{WorldReferenceKind::Proxy};
    std::uint32_t functionType{};
    std::vector<std::uint32_t> targetPathSteps;
};

struct WorldZoneDescriptor final {
    WorldEntityId zoneEntity{};
    std::uint32_t zoneId{};
    std::uint32_t instancePathId{};
    std::string entityName;
};

struct WorldManifestSourceInfo final {
    std::string commandsPakSha256;
    std::string commandsBinSha256;
    bool commandsPakPresent{};
    bool commandsBinPresent{};
    bool sourcesStructurallyAgree{};
    std::string structuralFingerprintPak;
    std::string structuralFingerprintBin;
};

struct WorldManifest final {
    std::uint32_t formatVersion{kWorldManifestFormatVersion};
    std::uint32_t compilerVersion{kWorldManifestCompilerVersion};
    std::uint64_t mapContentIdHigh{};
    std::uint64_t mapContentIdLow{};
    std::uint32_t rootCompositeShortGuid{};
    std::uint32_t globalCompositeShortGuid{};
    std::uint32_t pauseMenuCompositeShortGuid{};
    WorldManifestSourceInfo source;
    std::vector<WorldCompositeDescriptor> composites;
    std::vector<WorldInstancePathDescriptor> instancePaths;
    std::vector<WorldEntityDescriptor> entities;
    std::vector<WorldCompositePublicInputDescriptor> compositePublicInputs;
    std::vector<WorldCompositePublicInputCanonicalAnchor> compositePublicInputCanonicalAnchors;
    std::vector<WorldCompositePublicInputCausalProfile> compositePublicInputCausalProfiles;
    std::vector<WorldConnectionDescriptor> connections;
    std::vector<WorldOverrideDescriptor> overrides;
    std::vector<WorldReferenceDescriptor> references;
    std::vector<WorldZoneDescriptor> zones;
    std::uint32_t excludedAimpCompositeCount{};
    std::uint32_t excludedAimpEntityCount{};
    std::vector<std::string> warnings;
};

inline constexpr std::uint16_t kWorldReplicationSchemaVersion = 1U;
inline constexpr std::size_t kWorldPayloadMaximumBytes = 4096U;
inline constexpr std::uint32_t kWorldNetworkProbeOperation = 0xFFFF0001U;

enum class WorldMutationKind : std::uint8_t {
    Intent = 0,
    State,
    Event,
    Timeline,
};

enum WorldMutationFlags : std::uint8_t {
    WorldMutationFlagNone = 0U,
    WorldMutationFlagReliable = 1U << 0U,
    WorldMutationFlagReplaceable = 1U << 1U,
    WorldMutationFlagReplayable = 1U << 2U,
    WorldMutationFlagSyntheticProbe = 1U << 3U,
};

struct WorldManifestIdentity final {
    std::uint32_t formatVersion{kWorldManifestFormatVersion};
    std::uint32_t compilerVersion{kWorldManifestCompilerVersion};
    WorldEntityId mapContentId{};

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return formatVersion == kWorldManifestFormatVersion &&
            compilerVersion == kWorldManifestCompilerVersion &&
            mapContentId.Valid();
    }

    [[nodiscard]] constexpr bool operator==(
        const WorldManifestIdentity& other) const noexcept = default;
};

struct WorldPayload final {
    std::array<std::byte, kWorldPayloadMaximumBytes> bytes{};
    std::uint16_t size{};

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return size <= kWorldPayloadMaximumBytes;
    }
};

struct WorldIntent final {
    WorldManifestIdentity manifest{};
    WorldEntityId entity{};
    std::uint32_t levelGeneration{};
    std::uint64_t sourcePeer{};
    std::uint64_t clientSequence{};
    WorldMutationKind requestedKind{WorldMutationKind::Event};
    std::uint32_t operation{};
    WorldPayload payload{};
};

struct WorldMutation final {
    WorldManifestIdentity manifest{};
    WorldMutationKind kind{WorldMutationKind::Event};
    std::uint8_t flags{WorldMutationFlagNone};
    WorldEntityId entity{};
    std::uint32_t levelGeneration{};
    std::uint64_t sequence{};
    std::uint64_t revision{};
    std::uint64_t timelineTick{};
    std::uint64_t sourcePeer{};
    std::uint32_t operation{};
    WorldPayload payload{};
    // Local Launcher <-> DLL correlation only. This value rides in the outer
    // IPC Frame::sequence and is never encoded in the multiplayer mutation payload.
    std::uint64_t nativeDispatchId{};
};

struct WorldState final {
    WorldManifestIdentity manifest{};
    WorldEntityId entity{};
    std::uint32_t levelGeneration{};
    std::uint64_t revision{};
    std::uint64_t timelineTick{};
    std::uint32_t stateType{};
    WorldPayload payload{};
};

struct WorldEvent final {
    WorldManifestIdentity manifest{};
    WorldEntityId entity{};
    std::uint32_t levelGeneration{};
    std::uint64_t sequence{};
    std::uint64_t timelineTick{};
    std::uint64_t sourcePeer{};
    std::uint32_t eventType{};
    WorldPayload payload{};
};

struct WorldTimeline final {
    WorldManifestIdentity manifest{};
    WorldEntityId entity{};
    std::uint32_t levelGeneration{};
    std::uint64_t revision{};
    std::uint64_t timelineTick{};
    std::uint32_t timelineType{};
    WorldPayload payload{};
};

enum class WorldMutationResultCode : std::uint8_t {
    Accepted = 0,
    DuplicateRejected,
    StaleRejected,
    GenerationRejected,
    ManifestRejected,
    EntityRejected,
    QueueRejected,
    RuntimePending,
    NativeFault,
};

struct WorldMutationResult final {
    std::uint64_t sequence{};
    WorldMutationResultCode code{WorldMutationResultCode::Accepted};
    bool entityResolved{};
    bool dispatched{};
    // Echo of WorldMutation::nativeDispatchId. The AWR1 result payload omits it;
    // LauncherIPC returns it in the local IPC Frame::sequence.
    std::uint64_t nativeDispatchId{};
};

} // namespace aimp
