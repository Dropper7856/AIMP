#pragma once

#include <atomic>
#include <cstdint>

namespace aimp {

class SignatureResolver;

struct PlayerEquipmentSnapshot {
    std::uint64_t revision{};
    std::uintptr_t localPlayer{};
    std::uintptr_t equipmentManager{};
    std::int32_t currentEquipmentIndex{-1};
    std::int32_t previousEquipmentIndex{-1};
    std::int32_t presentationState{-1};
    bool equipmentActive{};
    bool valid{};
};

class PlayerSyncObserver final {
public:
    explicit PlayerSyncObserver(
        const SignatureResolver& resolver) noexcept;

    void ObserveOnGameThread() noexcept;
    [[nodiscard]] bool TrySnapshot(PlayerEquipmentSnapshot& output) const noexcept;
    [[nodiscard]] bool Available() const noexcept;

private:
    void Publish(const PlayerEquipmentSnapshot& snapshot) noexcept;
    void PublishUnavailable() noexcept;

    const void* globalStateRootAddress_{};
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
    std::atomic<std::uint8_t> valid_{0};

    // Written only from the engine thread.
    PlayerEquipmentSnapshot lastObserved_{};
    bool hasLastObserved_{};
};

} // namespace aimp
