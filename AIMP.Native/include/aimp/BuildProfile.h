#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aimp {

enum class SymbolId : std::uint16_t {
    GlobalStateRoot,
    DescriptorListHead,
    NetzVtable,
    NetzConstructor,
    NetzDestructor,
    NetzDispatch,
    AllocateChannel,
    ReactivateChannel,
    ReleaseChannel,
    BuildDescriptorRegistry,
    ParticipantBind,
    ParticipantFindIdentity,
    ParticipantRecordConstruct,
    ParticipantRecordCopy,
    ParticipantAppend,
    ParticipantDetach,
    CharacterCreate,
    CharacterDestroy,
    CharacterDestroyFull,
    MainThreadCharacterProcessing,
    CharacterManagerUpdate,
    CharacterReadinessProducer,
    CharacterReadinessPublisher,
    CharacterReadinessBarrier,
    CharacterOrchestrator,
    ReplicationContextConstruct,
    ReplicationContextAllocate,
    ReplicationContextRelease,
    EmptyRangeProvider,
    NeutralizedPublisher,
    ReceiveSpawnRequestF0,
    ReceiveSpawnRequest130,
    CharacterRuntimeIdCounter,
    UiRegistryRoot,
    UiFindNamedElement,
    PopupMessageEnqueue,
    CharacterDirectMove,
    CathodeEntityManagerRoot,
    CathodeResolveEntity,
    CathodeExtractHierarchyContext,
    CathodeExtractRuntimeKey,
    CharacterWeaponPayloadToObjectRef,
    CathodeAssignEntityRef,
    CathodeBuildCallbackValue,
    CathodeBuildRetainedWorkItem,
    CathodeEnterWorkItemCriticalSection,
    CathodeLeaveWorkItemCriticalSection,
    CathodeRetainCallbackValue,
    CathodeReleaseCallbackValue,
    CathodeProviderRegistryRoot,
    CathodeDispatchInput,
    CathodeBuildInputTarget,
    CathodeQueueInput,
    CathodeQueueTypedMessage,
    CathodePublicCompositeAction,
    CathodeOutputPropagation,
    CathodeSchedulerDrain,
    CathodeWorkItemBuild,
    CathodeWorkItemExecute,
    CathodeWorkItemDestroy,
    CathodeLogicSwitchConstructor,
    CathodeLogicSwitchVtable,
    CathodeLogicSwitchSet,
    CathodeLogicSwitchClear,
    CathodeReleaseInputTarget,
    CathodeInteractionVtable,
    CathodeWorldPickupVtable,
    CathodeRewireSystemVtable,
    CathodeRewireLocationVtable,
    CathodeRewireAccessPointVtable,
    CathodeRewireTotalPowerVtable,
    CathodeRewireVtable,
    CathodeSplinePathVtable,
    CathodeEnvironmentModelReferenceVtable,
    CathodeAnimatedModelAttachmentNodeVtable,
    CathodePlayEnvironmentAnimationVtable,
    CathodeDoorStatusVtable,
    CathodeDoorNativeVtable,
    CathodeDoorStartedOpening,
    CathodeDoorStartedClosing,
    CathodeDoorRequestLock,
    CathodeDoorRequestUnlock,
    CathodeDoorForceOpen,
    CathodeDoorForceClose,
    CathodeFindNamedComposite,
    CathodeCreateEntity,
    CathodeFinalizeEntity,
    CathodeSetEntityKind,
    CharacterEquipmentSystemRoot,
    CharacterGiveWeaponToCharacter,
    CharacterResolveEquipmentInstance,
    CharacterAssignEquipmentInstance,
    CathodeNpcEquipmentCommand,
    CharacterPrimaryActionStart,
    CharacterPrimaryActionStop,
    CharacterAimStart,
    CharacterAimStop,
    CharacterShootingRequestCreate,
    CharacterShootingRequestActivate,
    CharacterShootingRequestRelease,
    CharacterShootingRequestCancel,
    CharacterEquipmentControllerUpdate,
    CharacterCurrentEquipmentItem,
    CharacterActionFrameDelta,
    CharacterItemInitializeNpcResources,
    CharacterDeathSoloGameOver,
    CharacterDeathSoloGameOverEpilogue,
    GameplayReturnToFrontend,
};

struct MaskedBytes {
    std::vector<std::uint8_t> bytes;
    std::string mask;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] bool Matches(std::span<const std::byte> candidate) const noexcept;
};

struct SymbolSpec {
    SymbolId id{};
    std::string_view name;
    std::uint32_t rva{};
    MaskedBytes expected;
    std::size_t approvedPatchLength{};
    bool trampolineRelocationSafe{};
};

enum class ProfileMutableRangeKind : std::uint8_t {
    ProductionPath,
};

struct ProfileMutableRange {
    std::uint32_t offset{};
    std::uint32_t size{};
    ProfileMutableRangeKind kind{ProfileMutableRangeKind::ProductionPath};
};

struct ProfileSectionIdentity {
    std::string_view name;
    std::uint32_t virtualAddress{};
    std::uint32_t virtualSize{};
    std::uint32_t rawOffset{};
    std::uint32_t rawSize{};
    std::uint32_t characteristics{};
    std::array<std::uint8_t, 32> sha256{};
    std::vector<ProfileMutableRange> normalizedRanges;
};

struct BuildProfile {
    std::string_view id;
    std::uint32_t version{};
    std::array<std::uint8_t, 32> sha256{};
    std::array<std::uint8_t, 32> normalizedSha256{};
    std::uint16_t machine{};
    std::uint16_t fileCharacteristics{};
    std::uint16_t sectionCount{};
    std::uint32_t timeDateStamp{};
    std::uint32_t sizeOfImage{};
    std::uint32_t preferredImageBase{};
    std::uint32_t entryPoint{};
    std::uint32_t textRva{};
    std::uint32_t textSize{};
    std::uint64_t fileSize{};
    std::vector<ProfileSectionIdentity> sections;
    std::vector<SymbolSpec> symbols;

    [[nodiscard]] const SymbolSpec* Find(SymbolId idToFind) const noexcept;
};

struct PeSectionIdentity {
    std::string name;
    std::uint32_t virtualAddress{};
    std::uint32_t virtualSize{};
    std::uint32_t rawOffset{};
    std::uint32_t rawSize{};
    std::uint32_t characteristics{};
    std::array<std::uint8_t, 32> sha256{};
};

struct PeIdentity {
    std::wstring canonicalPath;
    std::array<std::uint8_t, 32> sha256{};
    std::uint64_t fileSize{};
    std::uint16_t machine{};
    std::uint16_t fileCharacteristics{};
    std::uint32_t timeDateStamp{};
    std::uint32_t sizeOfImage{};
    std::uint16_t optionalHeaderMagic{};
    std::uint32_t preferredImageBase{};
    std::uint32_t entryPoint{};
    std::uint16_t sectionCount{};
    bool hasRelocations{};
    bool hasDebugDirectory{};
    bool hasCodeView{};
    std::string fileVersion;
    std::vector<std::string> imports;
    std::vector<PeSectionIdentity> sections;
};

struct BuildValidation {
    bool fileRead{};
    bool peValid{};
    bool hashAvailable{};
    bool hashMatch{};
    bool metadataMatch{};
    bool sectionLayoutMatch{};
    bool protectedSectionsMatch{};
    bool normalizedHashMatch{};
    bool profileCompatible{};
    bool recognized{};
    bool validCandidate{};
    PeIdentity actual{};
    std::string reason;
};

const BuildProfile& Retail2015Profile();

BuildValidation ValidateExecutable(
    const std::wstring& path,
    const BuildProfile& profile = Retail2015Profile());

// Hash availability and digest matches are diagnostic only. Fixed RVAs are
// eligible when the target identity and PE layout still match the profile.
bool IsFixedRvaEligible(const BuildValidation& validation) noexcept;

bool IsProfileCompatible(
    std::span<const std::byte> fileBytes,
    const PeIdentity& identity,
    const BuildProfile& profile) noexcept;

std::wstring CurrentExecutablePath();
std::string Sha256Hex(const std::array<std::uint8_t, 32>& bytes);

} // namespace aimp
