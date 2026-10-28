#include "aimp/PlayerSyncObserver.h"

#include "aimp/BuildProfile.h"
#include "aimp/CrashGuard.h"
#include "aimp/SignatureResolver.h"
#include "aimp/RuntimeFeatureFlags.h"

#include <Windows.h>
#include <cstring>

namespace aimp {
namespace {

constexpr std::uintptr_t kRootContextOffset = 0x08U;
constexpr std::uintptr_t kLocalPlayerArrayOffset = 0x80U;
constexpr std::uintptr_t kLocalPlayerCountOffset = 0x84U;
constexpr std::uintptr_t kEquipmentManagerOffset = 0x1100U;
constexpr std::uintptr_t kCurrentEquipmentOffset = 0x150CU;
constexpr std::uintptr_t kPreviousEquipmentOffset = 0x1514U;
constexpr std::uintptr_t kEquipmentActiveOffset = 0x1510U;
constexpr std::uintptr_t kAimPrimaryOffset = 0x00A1U;
constexpr std::uintptr_t kPresentationStateObjectOffset = 0x152CU;
constexpr std::uintptr_t kPresentationStateOffset = 0x04U;

bool ValidEquipmentIndex(const std::int32_t value) noexcept {
    return value >= -1 && value <= 64;
}

bool ValidPresentationState(const std::int32_t value) noexcept {
    return value >= 0 && value <= 64;
}

// Wire-only identities, never written into the retail slot table. Slot 9 is
// shared by all inventory items; +3BC8 selects the actual definition and +6C
// is IS_ACTIVATED (FUN_0079CC60 and inventory restoration FUN_007CC960).
std::int32_t FlareIdentity(std::uintptr_t player, std::int32_t slot) noexcept {
    if (slot != 9) return slot;
#if defined(_MSC_VER)
    __try {
        const auto component = *reinterpret_cast<const std::uintptr_t*>(player + 0x31CU);
        if (component == 0U) return slot;
        const auto inventory = *reinterpret_cast<const std::uintptr_t*>(component + 0x3CU);
        if (inventory == 0U) return slot;
        const auto definition = *reinterpret_cast<const std::uintptr_t*>(inventory + 0x3BC8U);
        if (definition == 0U) return slot;
        const auto name = *reinterpret_cast<const char* const*>(definition + 8U);
        if (name == nullptr || std::strcmp(name, "flare") != 0) return slot;
        return *reinterpret_cast<const std::uint8_t*>(definition + 0x6CU) != 0U ? 33 : 32;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return slot; }
#else
    (void)player;
    return slot;
#endif
}

struct RawEquipmentState {
    std::int32_t currentEquipmentIndex{-1};
    std::int32_t previousEquipmentIndex{-1};
    std::int32_t presentationState{-1};
    std::uintptr_t presentationStateObject{};
    std::uint16_t aimPrimary{};
    std::uint8_t equipmentActive{};
};

bool TryResolveAndReadEquipmentState(
    const void* globalStateRootAddress,
    std::uintptr_t& localPlayer,
    std::uintptr_t& equipmentManager,
    RawEquipmentState& state) noexcept {
#if defined(_MSC_VER)
    // One SEH boundary for the complete local-player -> EquipmentController
    // snapshot. This removes the old chain of VirtualQuery-backed SafeRead
    // calls from every CharacterManager frame while still failing closed on a
    // level transition or stale pointer.
    __try {
        const auto root = *reinterpret_cast<const volatile std::uintptr_t*>(
            globalStateRootAddress);
        if (root == 0U) return false;
        const auto context = *reinterpret_cast<const volatile std::uintptr_t*>(
            root + kRootContextOffset);
        if (context == 0U) return false;
        const auto playerCount = *reinterpret_cast<const volatile std::uint32_t*>(
            context + kLocalPlayerCountOffset);
        const auto playerArray = *reinterpret_cast<const volatile std::uintptr_t*>(
            context + kLocalPlayerArrayOffset);
        if (playerCount == 0U || playerArray == 0U) return false;
        localPlayer = *reinterpret_cast<const volatile std::uintptr_t*>(playerArray);
        if (localPlayer == 0U) return false;
        equipmentManager = *reinterpret_cast<const volatile std::uintptr_t*>(
            localPlayer + kEquipmentManagerOffset);
        if (equipmentManager == 0U) return false;

        state.currentEquipmentIndex = *reinterpret_cast<const volatile std::int32_t*>(
            equipmentManager + kCurrentEquipmentOffset);
        state.previousEquipmentIndex = *reinterpret_cast<const volatile std::int32_t*>(
            equipmentManager + kPreviousEquipmentOffset);
        state.equipmentActive = *reinterpret_cast<const volatile std::uint8_t*>(
            equipmentManager + kEquipmentActiveOffset);
        state.aimPrimary = *reinterpret_cast<const volatile std::uint16_t*>(
            equipmentManager + kAimPrimaryOffset);
        state.presentationStateObject = *reinterpret_cast<const volatile std::uintptr_t*>(
            equipmentManager + kPresentationStateObjectOffset);
        if (state.presentationStateObject == 0U) return false;
        state.presentationState = *reinterpret_cast<const volatile std::int32_t*>(
            state.presentationStateObject + kPresentationStateOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        localPlayer = 0U;
        equipmentManager = 0U;
        return false;
    }
#else
    const auto root = crash_guard::SafeRead<std::uintptr_t>(globalStateRootAddress);
    if (!root.has_value() || *root == 0U) return false;
    const auto context = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*root + kRootContextOffset));
    if (!context.has_value() || *context == 0U) return false;
    const auto playerCount = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(*context + kLocalPlayerCountOffset));
    const auto playerArray = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*context + kLocalPlayerArrayOffset));
    if (!playerCount.has_value() || *playerCount == 0U ||
        !playerArray.has_value() || *playerArray == 0U) return false;
    const auto player = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*playerArray));
    if (!player.has_value() || *player == 0U) return false;
    const auto equipment = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*player + kEquipmentManagerOffset));
    if (!equipment.has_value() || *equipment == 0U) return false;
    localPlayer = *player;
    equipmentManager = *equipment;

    const auto current = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(equipmentManager + kCurrentEquipmentOffset));
    const auto previous = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(equipmentManager + kPreviousEquipmentOffset));
    const auto active = crash_guard::SafeRead<std::uint8_t>(
        reinterpret_cast<const void*>(equipmentManager + kEquipmentActiveOffset));
    const auto aimPrimary = crash_guard::SafeRead<std::uint16_t>(
        reinterpret_cast<const void*>(equipmentManager + kAimPrimaryOffset));
    const auto stateObject = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(equipmentManager + kPresentationStateObjectOffset));
    if (!current.has_value() || !previous.has_value() || !active.has_value() ||
        !aimPrimary.has_value() || !stateObject.has_value() || *stateObject == 0U) {
        return false;
    }
    const auto presentation = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(*stateObject + kPresentationStateOffset));
    if (!presentation.has_value()) return false;
    state.currentEquipmentIndex = *current;
    state.previousEquipmentIndex = *previous;
    state.equipmentActive = *active;
    state.aimPrimary = *aimPrimary;
    state.presentationStateObject = *stateObject;
    state.presentationState = *presentation;
    return true;
#endif
}
} // namespace

PlayerSyncObserver::PlayerSyncObserver(
    const SignatureResolver& resolver) noexcept
    : globalStateRootAddress_(resolver.Resolve(SymbolId::GlobalStateRoot)) {}

bool PlayerSyncObserver::Available() const noexcept {
    return globalStateRootAddress_ != nullptr;
}

void PlayerSyncObserver::ObserveOnGameThread() noexcept {
    if (!runtime_features::Enabled(runtime_features::Feature::PlayerSyncObserver)) {
        return;
    }
    if (!Available()) {
        PublishUnavailable();
        return;
    }

    std::uintptr_t localPlayer{};
    std::uintptr_t equipmentManager{};
    RawEquipmentState raw{};
    if (!TryResolveAndReadEquipmentState(
            globalStateRootAddress_, localPlayer, equipmentManager, raw) ||
        !ValidEquipmentIndex(raw.currentEquipmentIndex) ||
        !ValidEquipmentIndex(raw.previousEquipmentIndex) ||
        !ValidPresentationState(raw.presentationState)) {
        PublishUnavailable();
        return;
    }

    PlayerEquipmentSnapshot observed{};
    observed.localPlayer = localPlayer;
    observed.equipmentManager = equipmentManager;
    observed.currentEquipmentIndex = FlareIdentity(localPlayer, raw.currentEquipmentIndex);
    observed.previousEquipmentIndex = raw.previousEquipmentIndex;
    observed.presentationState = raw.presentationState;
    observed.equipmentActive = raw.equipmentActive != 0U;
    observed.aimHeld = (raw.aimPrimary & 0x00FFU) != 0U;
    observed.primaryHeld = (raw.aimPrimary & 0xFF00U) != 0U;
    observed.valid = true;

    const bool sameController = hasLastObserved_ && lastObserved_.valid &&
        observed.localPlayer == lastObserved_.localPlayer &&
        observed.equipmentManager == lastObserved_.equipmentManager;
    if (sameController) {
        if (observed.aimHeld != lastObserved_.aimHeld) {
            QueueAction(
                PlayerSyncActionKind::Aim,
                observed.aimHeld,
                observed.currentEquipmentIndex,
                observed.presentationState);
        }
        if (observed.primaryHeld != lastObserved_.primaryHeld) {
            QueueAction(
                PlayerSyncActionKind::Primary,
                observed.primaryHeld,
                observed.currentEquipmentIndex,
                observed.presentationState);
        }
        if (lastObserved_.presentationState != 9 &&
            observed.presentationState == 9) {
            QueueAction(
                PlayerSyncActionKind::Reload,
                true,
                observed.currentEquipmentIndex,
                observed.presentationState);
        }
    }

    // Presentation 6/7/8/9 can oscillate while an action is held. Do not turn
    // those engine-internal transitions into durable PlayerSync publications.
    // The ordered edge ring above preserves the events that matter.
    const bool changed = !hasLastObserved_ ||
        !lastObserved_.valid ||
        observed.localPlayer != lastObserved_.localPlayer ||
        observed.equipmentManager != lastObserved_.equipmentManager ||
        observed.currentEquipmentIndex != lastObserved_.currentEquipmentIndex ||
        observed.previousEquipmentIndex != lastObserved_.previousEquipmentIndex ||
        observed.equipmentActive != lastObserved_.equipmentActive ||
        observed.aimHeld != lastObserved_.aimHeld ||
        observed.primaryHeld != lastObserved_.primaryHeld;

    if (!changed) {
        // Keep the engine-thread baseline current for Reload edge detection,
        // without publishing or allocating anything.
        observed.revision = lastObserved_.revision;
        lastObserved_ = observed;
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

    // Never leave a remote Character with a held input if the local Player
    // disappears during a level transition or teardown.
    if (hasLastObserved_ && lastObserved_.valid) {
        if (lastObserved_.primaryHeld) {
            QueueAction(
                PlayerSyncActionKind::Primary, false,
                lastObserved_.currentEquipmentIndex,
                lastObserved_.presentationState);
        }
        if (lastObserved_.aimHeld) {
            QueueAction(
                PlayerSyncActionKind::Aim, false,
                lastObserved_.currentEquipmentIndex,
                lastObserved_.presentationState);
        }
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
    aimHeld_.store(snapshot.aimHeld ? 1U : 0U, std::memory_order_relaxed);
    primaryHeld_.store(snapshot.primaryHeld ? 1U : 0U, std::memory_order_relaxed);
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
        snapshot.aimHeld = aimHeld_.load(std::memory_order_relaxed) != 0U;
        snapshot.primaryHeld = primaryHeld_.load(std::memory_order_relaxed) != 0U;
        snapshot.valid = valid_.load(std::memory_order_relaxed) != 0U;

        const auto after = sequence_.load(std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U) {
            output = snapshot;
            return true;
        }
    }
    return false;
}

void PlayerSyncObserver::QueueAction(
    const PlayerSyncActionKind action,
    const bool active,
    const std::int32_t currentEquipmentIndex,
    const std::int32_t presentationState) noexcept {
    const auto write = actionWrite_.load(std::memory_order_relaxed);
    const auto read = actionRead_.load(std::memory_order_acquire);
    if (write - read >= kActionRingCapacity) {
        // Extremely unlikely at human input rates. Durable Aim/Primary state in
        // PlayerEquipmentSnapshot still provides convergence if this ever fills.
        return;
    }

    PlayerSyncActionEvent event{};
    event.sequence = nextActionSequence_++;
    event.action = action;
    event.active = active;
    event.currentEquipmentIndex = currentEquipmentIndex;
    event.presentationState = presentationState;
    actionRing_[write % kActionRingCapacity] = event;
    actionWrite_.store(write + 1U, std::memory_order_release);
}

bool PlayerSyncObserver::TryPopAction(
    PlayerSyncActionEvent& output) noexcept {
    const auto read = actionRead_.load(std::memory_order_relaxed);
    const auto write = actionWrite_.load(std::memory_order_acquire);
    if (read == write) {
        return false;
    }

    output = actionRing_[read % kActionRingCapacity];
    actionRead_.store(read + 1U, std::memory_order_release);
    return true;
}


} // namespace aimp
