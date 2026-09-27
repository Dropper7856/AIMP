#pragma once

#include "aimp/CathodeEquipmentGive.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aimp {

class Diagnostics;
class HookManager;
class LevelGenerationTracker;
class LifecycleCatalog;
class SignatureResolver;

enum class SpawnCharacterTrackingState : std::uint8_t {
    None,
    Pending,
    Ready,
    Failed,
};

struct SpawnCharacterTrackingSnapshot {
    std::uint64_t version{};
    std::uint64_t levelGeneration{};
    std::uint32_t spawnKey{};
    std::uintptr_t characterAddress{};
    std::uintptr_t recordAddress{};
    SpawnCharacterTrackingState state{SpawnCharacterTrackingState::None};
    bool hookInstalled{};
};

struct RemoteEquipmentProvisionResult {
    bool executed{};
    bool retryable{};
    std::string statusCode;
    std::string message;
};


class SpawnCharacterTracker final {
public:
    explicit SpawnCharacterTracker(LevelGenerationTracker& generations) noexcept;
    SpawnCharacterTracker(
        LevelGenerationTracker& generations,
        Diagnostics& diagnostics) noexcept;
    ~SpawnCharacterTracker();

    SpawnCharacterTracker(const SpawnCharacterTracker&) = delete;
    SpawnCharacterTracker& operator=(const SpawnCharacterTracker&) = delete;

    bool Install(
        const SignatureResolver& resolver,
        HookManager& hooks,
        std::string& error);
    void BeginLogicalShutdown() noexcept;
    void AttachLifecycleCatalog(LifecycleCatalog& lifecycle) noexcept;
    void DetachLifecycleCatalog() noexcept;
    void Begin(
        std::uint64_t levelGeneration,
        std::uint32_t spawnKey) noexcept;
    void Fail(
        std::uint64_t levelGeneration,
        std::uint32_t spawnKey) noexcept;
    void ResetGeneration(std::uint64_t levelGeneration) noexcept;
    void ObserveCreateBegin(
        std::uintptr_t managerAddress,
        std::uintptr_t recordAddress,
        std::uintptr_t requestAddress) noexcept;
    void ObserveCreated(
        std::uint32_t spawnKey,
        std::uintptr_t managerAddress,
        std::uintptr_t characterAddress,
        std::uintptr_t recordAddress,
        std::uintptr_t requestAddress) noexcept;

    // Manual command only, certified game thread. Instantiate the selected
    // CATHODE equipment archetype, resolve its certified public equipment
    // reference, then bind Character + Weapon to a real WEAPON_GiveToCharacter.
    [[nodiscard]] RemoteEquipmentProvisionResult
        ProvisionRemoteEquipmentOnGameThread(
            std::uintptr_t characterAddress,
            std::string compositeName,
            std::uint32_t equipmentEntityShortGuid,
            std::uint32_t equipmentOutputShortGuid,
            std::uint32_t spawnSchedulerParameterShortGuid,
            std::uint32_t spawnTargetInputShortGuid,
            std::uint32_t equipmentSlot);

    [[nodiscard]] SpawnCharacterTrackingSnapshot Snapshot() const noexcept;
    [[nodiscard]] bool HookInstalled() const noexcept;
    [[nodiscard]] bool ProcessLifetimeRequired() const noexcept;
    [[nodiscard]] static std::uint32_t ActiveDetours() noexcept;

private:
    void Publish(
        std::uint64_t levelGeneration,
        std::uint32_t spawnKey,
        std::uintptr_t characterAddress,
        std::uintptr_t recordAddress,
        SpawnCharacterTrackingState state) noexcept;
    void ClearRemoteEquipmentRecords() noexcept;
    void ReapDestroyedRemoteEquipmentRecords() noexcept;

    LevelGenerationTracker* generations_{};
    Diagnostics* diagnostics_{};
    void* characterCreateTrampoline_{};

    std::atomic<std::uintptr_t> cathodeEquipmentBase_{0U};
    std::atomic<void*> npcEquipmentCommandAddress_{nullptr};

    std::atomic_flag publishLock_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> version_{0};
    std::atomic<std::uint64_t> levelGeneration_{0};
    std::atomic<std::uint32_t> spawnKey_{0};
    std::atomic<std::uintptr_t> characterAddress_{0};
    std::atomic<std::uintptr_t> recordAddress_{0};
    std::atomic<SpawnCharacterTrackingState> state_{
        SpawnCharacterTrackingState::None};
    std::atomic<bool> hookInstalled_{false};
    std::atomic<bool> processLifetimeRequired_{false};
    std::atomic<LifecycleCatalog*> lifecycle_{nullptr};
    std::atomic<std::uint64_t> lastLifecycleCleanupSequence_{0U};

    std::mutex remoteEquipmentMutex_;
    // Keep every successfully attributed graph alive for the lifetime of the
    // Character. Selecting another item must not release a graph that CATHODE
    // can still reference through equipment/animation state.
    std::unordered_map<std::uintptr_t, std::vector<cathode_equipment::Record>>
        remoteEquipmentRecords_;
};

[[nodiscard]] const char* ToString(
    SpawnCharacterTrackingState state) noexcept;

} // namespace aimp
