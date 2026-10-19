#pragma once

#include "aimp/CapabilityMatrix.h"
#include "aimp/DummyPlayerController.h"
#include "aimp/GameCommandExecutor.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aimp::experimental {

inline constexpr std::uint32_t kSchemaVersion = 1;
[[nodiscard]] bool IsResolutionMode(std::string_view value) noexcept;

struct RequestEnvelope {
    std::string requestId;
    std::uint32_t levelGeneration{};
    std::string portableDummyId;
};

struct TestModeConfigureRequest : RequestEnvelope {
    bool enabled{};
};

struct CapabilityQueryRequest : RequestEnvelope {};

struct DummyCreateRequest : RequestEnvelope {
    DummyRequestIdentity identity;
    float offsetX{};
    float offsetY{};
    float offsetZ{};
};

struct DummyRemoveRequest : RequestEnvelope {};

struct DummyMoveRequest : RequestEnvelope {
    std::string command;
    float amount{};
    std::uint32_t durationMilliseconds{};
};

struct DiagnosticSnapshotRequest : RequestEnvelope {
    bool flush{};
};

struct GameCommandRequest : RequestEnvelope {
    std::string command;
    bool worldSync3EmptyCallbackEnabled{};
    std::string performanceIsolationModule;
    bool performanceIsolationEnabled{};
    PopupMessageCommand popupMessage;
    SpawnCharacterCommand spawnCharacter;
    CharacterDirectMoveCommand characterDirectMove;
    CharacterDirectStepCommand characterDirectStep;
    CharacterActionTestCommand characterActionTest;
    CharacterAmmoRefillCommand characterAmmoRefill;
    RemoteEquipmentProvisionCommand remoteEquipmentProvision;
    SpawnNetZCharacterCommand spawnNetZCharacter;
    WorldRegistryProbeCommand worldRegistryProbe;
    WorldManifestLoadCommand worldManifestLoad;
    WorldNetProbeCommand worldNetProbe;
    ItemSyncApplyCommand itemSyncApply;
    ItemSyncSpawnCommand itemSyncSpawn;
};

bool DecodeTestModeConfigure(
    std::span<const std::byte> payload,
    TestModeConfigureRequest& output) noexcept;
bool DecodeCapabilityQuery(
    std::span<const std::byte> payload,
    CapabilityQueryRequest& output) noexcept;
bool DecodeDummyCreate(
    std::span<const std::byte> payload,
    DummyCreateRequest& output) noexcept;
bool DecodeDummyRemove(
    std::span<const std::byte> payload,
    DummyRemoveRequest& output) noexcept;
bool DecodeDummyMove(
    std::span<const std::byte> payload,
    DummyMoveRequest& output) noexcept;
bool DecodeDiagnosticSnapshotRequest(
    std::span<const std::byte> payload,
    DiagnosticSnapshotRequest& output) noexcept;
bool DecodeGameCommand(
    std::span<const std::byte> payload,
    GameCommandRequest& output) noexcept;

std::vector<std::byte> EncodeCapabilityState(
    const RequestEnvelope& request,
    std::string_view profileId,
    std::string_view resolutionMode,
    bool staticValidated,
    bool runtimeValidated,
    const std::vector<CapabilityRecord>& capabilities,
    const std::vector<std::string>& blockedGates,
    std::string_view nextAction);

std::vector<std::byte> EncodeCaptureState(
    const RequestEnvelope& request,
    bool f0Captured,
    bool request130Captured,
    bool participantRegistryAvailable,
    std::uint32_t participantCount,
    bool contextPoolAvailable,
    bool freeChannelAvailable,
    bool gameThreadValidated,
    std::uint32_t captureCatalogCount,
    std::string_view lastOperation,
    std::uint32_t gameThreadCandidateId = 0,
    std::uint32_t gameThreadSourceCount = 0,
    std::string_view engineThreadSource = "Unknown",
    std::uint64_t firstEngineCallbackPerformanceCounter = 0,
    std::uint64_t lastEngineCallbackPerformanceCounter = 0,
    std::size_t pendingCommandCount = 0,
    bool mainThreadCharacterProcessingHookInstalled = false,
    bool characterManagerUpdateHookInstalled = false,
    bool characterReadinessProducerHookInstalled = false,
    std::string_view remoteCharacterState = "None",
    std::uint32_t remoteCharacterSpawnKey = 0,
    std::uintptr_t remoteCharacterAddress = 0,
    std::uintptr_t remoteCharacterRecordAddress = 0,
    bool characterCreateHookInstalled = false);

std::vector<std::byte> EncodeDummyOperationResult(
    const RequestEnvelope& request,
    const DummyOperationResult& result);

std::vector<std::byte> EncodeDummyMoveResult(
    const RequestEnvelope& request,
    std::string_view statusCode,
    std::string_view message,
    std::string_view method,
    bool applied);

std::vector<std::byte> EncodeDummyBindingState(
    const RequestEnvelope& request,
    DummyAchievement achieved,
    std::string_view lastError);

std::vector<std::byte> EncodeDiagnosticSnapshotResult(
    const RequestEnvelope& request,
    std::string_view runId,
    std::string_view nativeLogPath,
    std::string_view statusCode,
    std::string_view message);

std::vector<std::byte> EncodeExperimentalError(
    const RequestEnvelope& request,
    std::string_view code,
    std::string_view message,
    const std::vector<std::string>& blockedGates);

std::vector<std::byte> EncodeLevelState(
    const RequestEnvelope& request,
    std::string_view levelName,
    bool transitioning,
    bool gameThreadValidated);

std::vector<std::byte> EncodeGameCommandResult(
    const RequestEnvelope& request,
    std::string_view command,
    const GameCommandExecutionResult& result);

} // namespace aimp::experimental
