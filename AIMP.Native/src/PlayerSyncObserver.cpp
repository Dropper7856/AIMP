#include "aimp/PlayerSyncObserver.h"

#include "aimp/BuildProfile.h"
#include "aimp/CrashGuard.h"
#include "aimp/SignatureResolver.h"

namespace aimp {
namespace {

constexpr std::uintptr_t kRootContextOffset = 0x08U;
constexpr std::uintptr_t kLocalPlayerArrayOffset = 0x80U;
constexpr std::uintptr_t kLocalPlayerCountOffset = 0x84U;
constexpr std::uintptr_t kEquipmentManagerOffset = 0x1100U;
constexpr std::uintptr_t kCurrentEquipmentOffset = 0x150CU;
constexpr std::uintptr_t kPreviousEquipmentOffset = 0x1514U;
constexpr std::uintptr_t kEquipmentActiveOffset = 0x1510U;
constexpr std::uintptr_t kPresentationStateObjectOffset = 0x152CU;
constexpr std::uintptr_t kPresentationStateOffset = 0x04U;

bool ValidEquipmentIndex(const std::int32_t value) noexcept {
    return value >= -1 && value <= 64;
}

bool ValidPresentationState(const std::int32_t value) noexcept {
    return value >= 0 && value <= 64;
}

} // namespace

PlayerSyncObserver::PlayerSyncObserver(
    const SignatureResolver& resolver) noexcept
    : globalStateRootAddress_(resolver.Resolve(SymbolId::GlobalStateRoot)) {}

bool PlayerSyncObserver::Available() const noexcept {
    return globalStateRootAddress_ != nullptr;
}

void PlayerSyncObserver::ObserveOnGameThread() noexcept {
    if (!Available()) {
        PublishUnavailable();
        return;
    }

    const auto root = crash_guard::SafeRead<std::uintptr_t>(globalStateRootAddress_);
    if (!root.has_value() || *root == 0U) {
        PublishUnavailable();
        return;
    }

    const auto context = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*root + kRootContextOffset));
    if (!context.has_value() || *context == 0U) {
        PublishUnavailable();
        return;
    }

    const auto playerCount = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(*context + kLocalPlayerCountOffset));
    const auto playerArray = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*context + kLocalPlayerArrayOffset));
    if (!playerCount.has_value() || *playerCount == 0U ||
        !playerArray.has_value() || *playerArray == 0U) {
        PublishUnavailable();
        return;
    }

    const auto localPlayer = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*playerArray));
    if (!localPlayer.has_value() || *localPlayer == 0U) {
        PublishUnavailable();
        return;
    }

    const auto equipmentManager = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*localPlayer + kEquipmentManagerOffset));
    if (!equipmentManager.has_value() || *equipmentManager == 0U) {
        PublishUnavailable();
        return;
    }

    const auto currentEquipment = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(*equipmentManager + kCurrentEquipmentOffset));
    const auto previousEquipment = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(*equipmentManager + kPreviousEquipmentOffset));
    const auto active = crash_guard::SafeRead<std::uint8_t>(
        reinterpret_cast<const void*>(*equipmentManager + kEquipmentActiveOffset));
    const auto stateObject = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*equipmentManager + kPresentationStateObjectOffset));

    if (!currentEquipment.has_value() || !previousEquipment.has_value() ||
        !active.has_value() || !stateObject.has_value() || *stateObject == 0U ||
        !ValidEquipmentIndex(*currentEquipment) ||
        !ValidEquipmentIndex(*previousEquipment)) {
        PublishUnavailable();
        return;
    }

    const auto presentationState = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(*stateObject + kPresentationStateOffset));
    if (!presentationState.has_value() ||
        !ValidPresentationState(*presentationState)) {
        PublishUnavailable();
        return;
    }

    PlayerEquipmentSnapshot observed{};
    observed.localPlayer = *localPlayer;
    observed.equipmentManager = *equipmentManager;
    observed.currentEquipmentIndex = *currentEquipment;
    observed.previousEquipmentIndex = *previousEquipment;
    observed.presentationState = *presentationState;
    observed.equipmentActive = *active != 0U;
    observed.valid = true;

    const bool changed = !hasLastObserved_ ||
        !lastObserved_.valid ||
        observed.localPlayer != lastObserved_.localPlayer ||
        observed.equipmentManager != lastObserved_.equipmentManager ||
        observed.currentEquipmentIndex != lastObserved_.currentEquipmentIndex ||
        observed.previousEquipmentIndex != lastObserved_.previousEquipmentIndex ||
        observed.presentationState != lastObserved_.presentationState ||
        observed.equipmentActive != lastObserved_.equipmentActive;
    if (!changed) {
        return;
    }

    observed.revision = revision_.load(std::memory_order_relaxed) + 1U;
    lastObserved_ = observed;
    hasLastObserved_ = true;
    Publish(observed);
}

void PlayerSyncObserver::PublishUnavailable() noexcept {
    if (hasLastObserved_ && !lastObserved_.valid) {
        return;
    }

    PlayerEquipmentSnapshot unavailable{};
    unavailable.revision = revision_.load(std::memory_order_relaxed) + 1U;
    unavailable.valid = false;
    lastObserved_ = unavailable;
    hasLastObserved_ = true;
    Publish(unavailable);
}

void PlayerSyncObserver::Publish(
    const PlayerEquipmentSnapshot& snapshot) noexcept {
    sequence_.fetch_add(1U, std::memory_order_acq_rel);
    revision_.store(snapshot.revision, std::memory_order_relaxed);
    localPlayer_.store(snapshot.localPlayer, std::memory_order_relaxed);
    equipmentManager_.store(snapshot.equipmentManager, std::memory_order_relaxed);
    currentEquipmentIndex_.store(
        snapshot.currentEquipmentIndex,
        std::memory_order_relaxed);
    previousEquipmentIndex_.store(
        snapshot.previousEquipmentIndex,
        std::memory_order_relaxed);
    presentationState_.store(snapshot.presentationState, std::memory_order_relaxed);
    equipmentActive_.store(snapshot.equipmentActive ? 1U : 0U, std::memory_order_relaxed);
    valid_.store(snapshot.valid ? 1U : 0U, std::memory_order_relaxed);
    sequence_.fetch_add(1U, std::memory_order_release);
}

bool PlayerSyncObserver::TrySnapshot(
    PlayerEquipmentSnapshot& output) const noexcept {
    for (std::uint32_t attempt = 0; attempt < 8U; ++attempt) {
        const auto before = sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }

        PlayerEquipmentSnapshot snapshot{};
        snapshot.revision = revision_.load(std::memory_order_relaxed);
        snapshot.localPlayer = localPlayer_.load(std::memory_order_relaxed);
        snapshot.equipmentManager = equipmentManager_.load(std::memory_order_relaxed);
        snapshot.currentEquipmentIndex =
            currentEquipmentIndex_.load(std::memory_order_relaxed);
        snapshot.previousEquipmentIndex =
            previousEquipmentIndex_.load(std::memory_order_relaxed);
        snapshot.presentationState =
            presentationState_.load(std::memory_order_relaxed);
        snapshot.equipmentActive =
            equipmentActive_.load(std::memory_order_relaxed) != 0U;
        snapshot.valid = valid_.load(std::memory_order_relaxed) != 0U;

        const auto after = sequence_.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) {
            output = snapshot;
            return true;
        }
    }
    return false;
}

} // namespace aimp
