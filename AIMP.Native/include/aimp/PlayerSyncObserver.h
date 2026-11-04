#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace aimp {

class SignatureResolver;

enum class PlayerSyncActionKind : std::uint8_t {
    Aim = 0,
    Primary = 1,
    Reload = 2,
};

struct PlayerSyncActionEvent {
    std::uint64_t sequence{};
    PlayerSyncActionKind action{PlayerSyncActionKind::Aim};
    std::int32_t currentEquipmentIndex{-1};
    std::int32_t presentationState{-1};
    bool active{};
};

struct PlayerEquipmentSnapshot {
    std::uint64_t revision{};
    std::uintptr_t localPlayer{};
    std::uintptr_t equipmentManager{};
    std::int32_t currentEquipmentIndex{-1};
    std::int32_t previousEquipmentIndex{-1};
    std::int32_t presentationState{-1};
    bool equipmentActive{};
    bool aimHeld{};
    bool primaryHeld{};
    bool valid{};
};

class PlayerSyncObserver final {
public:
    explicit PlayerSyncObserver(
        const SignatureResolver& resolver) noexcept;

    void ObserveOnGameThread() noexcept;
    [[nodiscard]] bool TrySnapshot(PlayerEquipmentSnapshot& output) const noexcept;
    [[nodiscard]] bool TryPopAction(PlayerSyncActionEvent& output) noexcept;
    [[nodiscard]] bool Available() const noexcept;

private:
    void Publish(const PlayerEquipmentSnapshot& snapshot) noexcept;
    void PublishUnavailable() noexcept;
    void QueueAction(
        PlayerSyncActionKind action,
        bool active,
        std::int32_t currentEquipmentIndex,
        std::int32_t presentationState) noexcept;

    const void* globalStateRootAddress_{};
    const void* currentItemAddress_{};
    // Single engine-thread writer, IPC-thread readers. Even values are stable;
    // odd values mean a publication is in progress.
    mutable std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> revision_{0};
    std::atomic<std::uintptr_t> localPlayer_{0};
    std::atomic<std::uintptr_t> equipmentManager_{0};
    std::atomic<std::int32_t> currentEquipmentIndex_{-1};
    std::atomic<std::int32_t> previousEquipmentIndex_{-1};
    std::atomic<std::int32_t> presentationState_{-1};
    std::atomic<std::uint8_t> equipmentActive_{0};
    std::atomic<std::uint8_t> aimHeld_{0};
    std::atomic<std::uint8_t> primaryHeld_{0};
    std::atomic<std::uint8_t> valid_{0};

    static constexpr std::uint32_t kActionRingCapacity = 64U;
    std::array<PlayerSyncActionEvent, kActionRingCapacity> actionRing_{};
    std::atomic<std::uint32_t> actionWrite_{0};
    std::atomic<std::uint32_t> actionRead_{0};
    std::uint64_t nextActionSequence_{1U};

    // Written only from the engine thread.
    PlayerEquipmentSnapshot lastObserved_{};
    bool hasLastObserved_{};
};

} // namespace aimp
