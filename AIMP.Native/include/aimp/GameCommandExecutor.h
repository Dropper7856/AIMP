#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aimp {

class CaptureCatalog;
class Diagnostics;
class LevelGenerationTracker;
class GameThreadRuntimeEvidence;
class GameThreadDispatcher;
class SignatureResolver;
class SpawnCharacterTracker;
class WorldEntityRegistry;

struct PopupMessageCommand {
    std::string headerText;
    std::string mainText;
    float durationSeconds{5.0F};
    std::string icon{"Alert"};
    std::uint32_t soundEvent{1};
};

struct SpawnCharacterCommand {
    float x{};
    float y{};
    float z{};
};

struct CharacterDirectMoveCommand {
    std::uintptr_t characterAddress{};
    float directionX{};
    float directionZ{};
    float intensity{1.1F};
    std::uint32_t frameCount{180U};
    bool worldSpace{};
    bool hasReplicationPosition{};
    float replicationPositionX{};
    float replicationPositionY{};
    float replicationPositionZ{};
    bool hasReplicationYaw{};
    float replicationYaw{};
    bool hasMoveType{};
    std::uint32_t moveType{1U};
};

struct CharacterDirectStepCommand {
    std::uintptr_t characterAddress{};
    float deltaX{};
    float deltaY{};
    float deltaZ{};
    float deltaYaw{};
    std::uint32_t frameCount{180U};
};

enum class CharacterActionKind : std::uint8_t {
    AimStart,
    AimStop,
    PrimaryStart,
    PrimaryStop,
    // Retained numeric compatibility only; parser and executor reject these.
    ShootRequestOnce,
    CancelShootRequest,
    InspectShootRequestNode,
    CompareActionNodeVTables,
};

struct CharacterActionTestCommand {
    std::uintptr_t characterAddress{};
    CharacterActionKind action{CharacterActionKind::AimStart};
};

struct CharacterAmmoRefillCommand {
    std::uintptr_t characterAddress{};
};

struct RemoteEquipmentProvisionCommand {
    std::uintptr_t characterAddress{};
    std::string compositeName;
    std::uint32_t equipmentEntityShortGuid{};
    std::uint32_t equipmentOutputShortGuid{};
    std::uint32_t spawnSchedulerParameterShortGuid{};
    std::uint32_t spawnTargetInputShortGuid{};
    std::uint32_t equipmentSlot{};
};

struct WorldRegistryProbeCommand {
    std::string manifestPath;
};

struct WorldManifestLoadCommand {
    std::string manifestPath;
};

struct WorldNetProbeCommand {
    std::uint64_t entityHigh{};
    std::uint64_t entityLow{};
};

struct ItemSyncApplyCommand {
    std::uint64_t entityHigh{};
    std::uint64_t entityLow{};
};

struct ItemSyncSpawnCommand {
    bool drop{};
    std::uint64_t ownerKey{};
    bool spawn{};
    std::string path;
    std::int32_t quantity{};
    std::array<float, 12> transform{};
};

struct SpawnNetZCharacterCommand {
    float x{};
    float y{};
    float z{};
    std::uint32_t participantKey{1};
    std::string displayName{"RemotePlayer"};
};

struct GameCommandExecutionResult {
    bool executed{};
    std::string statusCode;
    std::string message;
};

class IGameCommandExecutor {
public:
    virtual ~IGameCommandExecutor() = default;

    [[nodiscard]] virtual bool CanExecute(
        std::string_view command) const noexcept = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecutePopupMessage(
        const PopupMessageCommand& command) const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteSpawnCharacter(
        const SpawnCharacterCommand& command) const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteSpawnNetZCharacter(
        const SpawnNetZCharacterCommand& command) const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteCharacterDirectMove(
        const CharacterDirectMoveCommand& command) const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteCharacterDirectStep(
        const CharacterDirectStepCommand& command) const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteCharacterActionTest(
        const CharacterActionTestCommand& command) const = 0;
    // Default rejection keeps other executor implementations source-compatible.
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteCharacterAmmoRefill(
        const CharacterAmmoRefillCommand&) const {
        return {false, "AMMO_REFILL_UNAVAILABLE", "Manual test refill is not supported by this executor."};
    }
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteRemoteEquipmentProvision(
        const RemoteEquipmentProvisionCommand& command) const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteWorldRegistryProbe(
        const WorldRegistryProbeCommand& command) const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteWorldManifestLoad(
        const WorldManifestLoadCommand& command) const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteWorldNetProbe(
        const WorldNetProbeCommand& command) const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteWorldInteractionStats() const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteWorldCompositeInputStats() const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteWorldStateDiscoveryBaseline() const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteWorldStateDiscoveryAfter() const = 0;
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteWorldStateDiscoveryStatus() const = 0;
    // Default rejection keeps other executor implementations source-compatible.
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteReturnToFrontend() const {
        return {false, "RETURN_TO_FRONTEND_UNAVAILABLE", "Return to frontend is not supported by this executor."};
    }
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteItemSyncApply(
        const ItemSyncApplyCommand&) const {
        return {false, "ITEM_SYNC_UNAVAILABLE", "ItemSync is not supported by this executor."};
    }
    [[nodiscard]] virtual GameCommandExecutionResult ExecuteItemSyncSpawn(
        const ItemSyncSpawnCommand&) const {
        return {false, "ITEM_SYNC_UNAVAILABLE", "ItemSync is not supported by this executor."};
    }
};

class GameCommandExecutor final : public IGameCommandExecutor {
public:
    GameCommandExecutor(
        const SignatureResolver& resolver,
        CaptureCatalog& captures,
        LevelGenerationTracker& generations,
        const GameThreadRuntimeEvidence& runtimeEvidence,
        SpawnCharacterTracker& spawnTracker,
        Diagnostics& diagnostics,
        bool exactExecutableHash) noexcept;

    [[nodiscard]] bool IsAvailable() const noexcept;
    [[nodiscard]] bool CanExecute(
        std::string_view command) const noexcept override;
    [[nodiscard]] GameCommandExecutionResult ExecutePopupMessage(
        const PopupMessageCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteSpawnCharacter(
        const SpawnCharacterCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteSpawnNetZCharacter(
        const SpawnNetZCharacterCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteCharacterDirectMove(
        const CharacterDirectMoveCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteCharacterDirectStep(
        const CharacterDirectStepCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteCharacterActionTest(
        const CharacterActionTestCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteCharacterAmmoRefill(
        const CharacterAmmoRefillCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteRemoteEquipmentProvision(
        const RemoteEquipmentProvisionCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteWorldRegistryProbe(
        const WorldRegistryProbeCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteWorldManifestLoad(
        const WorldManifestLoadCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteWorldNetProbe(
        const WorldNetProbeCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteWorldInteractionStats() const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteWorldCompositeInputStats() const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteWorldStateDiscoveryBaseline() const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteWorldStateDiscoveryAfter() const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteWorldStateDiscoveryStatus() const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteReturnToFrontend() const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteItemSyncApply(
        const ItemSyncApplyCommand& command) const override;
    [[nodiscard]] GameCommandExecutionResult ExecuteItemSyncSpawn(
        const ItemSyncSpawnCommand& command) const override;

    void AttachWorldEntityRegistry(WorldEntityRegistry& registry) noexcept;
    void AttachGameThreadDispatcher(GameThreadDispatcher& dispatcher) noexcept;

    [[nodiscard]] static bool PrepareBootstrapCivilianPayload(
        std::array<std::byte, 0x130>& payload) noexcept;

    [[nodiscard]] static bool PrepareDynamicSpawnPayload(
        std::array<std::byte, 0x130>& payload,
        std::uint32_t spawnKey,
        std::uint32_t entityKey,
        std::uint32_t selection,
        const SpawnCharacterCommand& command) noexcept;

    [[nodiscard]] static bool PrepareNetZSpawnPayload(
        std::array<std::byte, 0x130>& payload,
        std::uint32_t participantKey) noexcept;

    [[nodiscard]] static bool PrepareRemoteParticipantRecord(
        const std::array<std::byte, 0x158>& localRecord,
        std::array<std::byte, 0x158>& remoteRecord,
        std::uint32_t participantKey,
        std::string_view displayName,
        const std::array<std::byte, 16>& identity) noexcept;

private:
    [[nodiscard]] bool PopupAvailable() const noexcept;
    [[nodiscard]] bool SpawnAvailable() const noexcept;
    [[nodiscard]] bool NetZSpawnAvailable() const noexcept;
    [[nodiscard]] bool CharacterDirectMoveAvailable() const noexcept;
    [[nodiscard]] bool CharacterDirectStepAvailable() const noexcept;
    [[nodiscard]] bool CharacterActionTestAvailable() const noexcept;
    [[nodiscard]] bool RemoteEquipmentProvisionAvailable() const noexcept;
    [[nodiscard]] bool ReturnToFrontendAvailable() const noexcept;

    const void* uiRegistryRootAddress_{};
    const void* findNamedElementAddress_{};
    const void* popupMessageAddress_{};
    const void* spawnRequest130ReceiverAddress_{};
    void* characterRuntimeIdCounterAddress_{};
    const void* globalStateRootAddress_{};
    const void* returnToFrontendAddress_{};
    CaptureCatalog* captures_{};
    LevelGenerationTracker* generations_{};
    const GameThreadRuntimeEvidence* runtimeEvidence_{};
    SpawnCharacterTracker* spawnTracker_{};
    Diagnostics* diagnostics_{};
    bool exactExecutableHash_{};
    WorldEntityRegistry* worldRegistry_{};
    GameThreadDispatcher* gameThreadDispatcher_{};
    mutable std::atomic<std::uint32_t> dynamicNonce_{1};
};

} // namespace aimp
