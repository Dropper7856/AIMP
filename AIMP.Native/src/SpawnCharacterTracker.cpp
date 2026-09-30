#include "aimp/AISync.h"
#include "aimp/CharacterActionRuntime.h"
#include "aimp/SpawnCharacterTracker.h"
#include "aimp/BuildProfile.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/LifecycleCatalog.h"
#include "aimp/RetailEquipmentPrimitives.h"
#include "aimp/SignatureResolver.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <iterator>
#include <string>
#include <utility>

namespace aimp {
namespace {
using CharacterCreate = void*(__thiscall*)(void*, void*, const void*);
std::atomic<SpawnCharacterTracker*> gTracker{nullptr};
std::atomic<void*> gCharacterCreateTrampoline{nullptr};
std::atomic<std::uint32_t> gActiveDetours{0};


class ActiveDetour final {
public:
    ActiveDetour() noexcept {
        gActiveDetours.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ActiveDetour() {
        gActiveDetours.fetch_sub(1, std::memory_order_acq_rel);
    }
};

void* __fastcall CharacterCreateDetour(
    void* manager,
    void*,
    void* record,
    const void* request) noexcept {
    ActiveDetour active;
    auto* tracker = gTracker.load(std::memory_order_acquire);
    if (tracker != nullptr) {
        tracker->ObserveCreateBegin(
            reinterpret_cast<std::uintptr_t>(manager),
            reinterpret_cast<std::uintptr_t>(record),
            reinterpret_cast<std::uintptr_t>(request));
    }
    const auto spawnKey = crash_guard::SafeRead<std::uint32_t>(request);
    const auto original = reinterpret_cast<CharacterCreate>(
        gCharacterCreateTrampoline.load(std::memory_order_acquire));
    auto* character = original == nullptr
        ? nullptr
        : original(manager, record, request);
    tracker = gTracker.load(std::memory_order_acquire);
    if (tracker != nullptr) {
        tracker->ObserveCreated(
            spawnKey.value_or(0U),
            reinterpret_cast<std::uintptr_t>(manager),
            reinterpret_cast<std::uintptr_t>(character),
            reinterpret_cast<std::uintptr_t>(record),
            reinterpret_cast<std::uintptr_t>(request));
    }
    return character;
}

bool ValidAddress(const std::uintptr_t value) noexcept {
    return value >= 0x10000U &&
        value <= (std::numeric_limits<std::uint32_t>::max)();
}

bool ValidCharacterAddress(const std::uintptr_t value) noexcept {
    return ValidAddress(value) && (value & 0x0FU) == 0U;
}

struct EquipmentSelectionResult final {
    bool selected{};
    bool retryable{};
    const char* statusCode{"REMOTE_EQUIPMENT_SELECT_FAILED"};
    const char* message{"Retail equipment selection failed."};
};

// FUN_00654E60 is a retail NPC equipment action constructor/executor. Keep the
// SEH boundary trivial: this command runs only when PlayerSync changes visible
// equipment, never from a frame hook.
bool InvokeNpcEquipmentCommand(
    void* function,
    void* actionContext,
    const std::uint32_t currentSlot,
    const std::uint32_t targetSlot,
    std::uint32_t& exceptionCode) noexcept {
    exceptionCode = 0U;
    if (function == nullptr || actionContext == nullptr) return false;
    __try {
        reinterpret_cast<NpcEquipmentCommandFn>(function)(
            actionContext,
            static_cast<std::uint8_t>(currentSlot != targetSlot),
            currentSlot,
            1U,
            1U,
            targetSlot,
            1U,
            0U,
            1U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exceptionCode = static_cast<std::uint32_t>(GetExceptionCode());
        return false;
    }
}

EquipmentSelectionResult EnsureEquipmentSelected(
    const std::uintptr_t character,
    const std::uint32_t targetSlot,
    void* npcEquipmentCommand,
    std::uint32_t& exceptionCode) noexcept {
    constexpr std::uint32_t kMaximumRetailEquipmentSlot = 10U;
    if (!ValidCharacterAddress(character) || targetSlot > kMaximumRetailEquipmentSlot) {
        return {false, false, "REMOTE_EQUIPMENT_SELECT_INVALID",
            "Invalid Character or target equipment slot."};
    }
    const auto equipment = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(character + 0x1100U));
    if (!equipment.has_value() || *equipment == 0U) {
        return {false, true, "REMOTE_EQUIPMENT_SELECT_NOT_READY",
            "The Character EquipmentController is not ready."};
    }

    const auto targetWrapper = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*equipment + targetSlot * sizeof(std::uint32_t)));
    const auto currentSlot = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(*equipment + 0x150CU));
    const auto active = crash_guard::SafeRead<std::uint8_t>(
        reinterpret_cast<const void*>(*equipment + 0x1510U));
    if (!targetWrapper.has_value() || *targetWrapper == 0U ||
        !currentSlot.has_value() || !active.has_value()) {
        return {false, true, "REMOTE_EQUIPMENT_SELECT_NOT_READY",
            "The target equipment attachment or controller selection state is not readable yet."};
    }

    if (*currentSlot == targetSlot && *active != 0U) {
        return {true, false, "REMOTE_EQUIPMENT_SELECTED",
            "The requested equipment slot is already selected and active."};
    }
    if (npcEquipmentCommand == nullptr) {
        return {false, false, "REMOTE_EQUIPMENT_SELECT_UNAVAILABLE",
            "The certified retail NPC equipment command did not resolve."};
    }

    // FUN_0043A360(Character, 3, 0) is exactly:
    //   if (*(Character + 0x388) != 0) return *(Character + 0x388);
    // Use the equivalent direct read so this rare command does not add another
    // resolved native dependency. Retail callers also require bit 1 at +0x08
    // and avoid issuing another equipment action while type 7 is active.
    const auto actionContext = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(character + 0x388U));
    if (!actionContext.has_value() || *actionContext == 0U) {
        return {false, true, "REMOTE_EQUIPMENT_SELECT_NOT_READY",
            "The Character equipment action context is not ready."};
    }
    const auto actionFlags = crash_guard::SafeRead<std::uint8_t>(
        reinterpret_cast<const void*>(*actionContext + 0x08U));
    const auto actionType = crash_guard::SafeRead<std::uint16_t>(
        reinterpret_cast<const void*>(*actionContext + 0x1EU));
    if (!actionFlags.has_value() || !actionType.has_value()) {
        return {false, true, "REMOTE_EQUIPMENT_SELECT_NOT_READY",
            "The Character equipment action context is temporarily unreadable."};
    }
    if (*actionType == 7U || ((*actionFlags & 0x02U) == 0U)) {
        return {false, true, "REMOTE_EQUIPMENT_SELECT_NOT_READY",
            "A retail equipment transition is already active or not yet admissible."};
    }

    if (!InvokeNpcEquipmentCommand(
            npcEquipmentCommand,
            reinterpret_cast<void*>(*actionContext),
            *currentSlot,
            targetSlot,
            exceptionCode)) {
        return {false, false, "REMOTE_EQUIPMENT_SELECT_NATIVE_FAULT",
            "The retail NPC equipment selection command faulted."};
    }

    // FUN_00654E60 enters the normal retail transition; selection becomes
    // observable on a later engine update. The launcher already polls this
    // command at 4 Hz, so return retryable instead of adding any frame hook.
    return {false, true, "REMOTE_EQUIPMENT_SELECT_PENDING",
        "Retail equipment selection was dispatched; waiting for the controller to converge."};
}


} // namespace

SpawnCharacterTracker::SpawnCharacterTracker(
    LevelGenerationTracker& generations) noexcept
    : generations_(&generations) {
    levelGeneration_.store(
        generations.CurrentGeneration(),
        std::memory_order_release);
}

SpawnCharacterTracker::SpawnCharacterTracker(
    LevelGenerationTracker& generations,
    Diagnostics& diagnostics) noexcept
    : generations_(&generations), diagnostics_(&diagnostics) {
    levelGeneration_.store(
        generations.CurrentGeneration(),
        std::memory_order_release);
}

SpawnCharacterTracker::~SpawnCharacterTracker() {
    BeginLogicalShutdown();
}



bool SpawnCharacterTracker::Install(
    const SignatureResolver& resolver,
    HookManager& hooks,
    std::string& error) {
    if (HookInstalled()) {
        error = "CharacterCreate tracking hook is already installed";
        return false;
    }
    SpawnCharacterTracker* expected = nullptr;
    if (!gTracker.compare_exchange_strong(
            expected,
            this,
            std::memory_order_acq_rel)) {
        error = "another SpawnCharacter tracker is active";
        return false;
    }

    const auto* spec = Retail2015Profile().Find(SymbolId::CharacterCreate);
    auto* target = resolver.ResolveMutable(SymbolId::CharacterCreate);
    if (spec == nullptr || target == nullptr) {
        gTracker.store(nullptr, std::memory_order_release);
        error = "CharacterCreate tracking symbol did not resolve";
        return false;
    }

    cathodeEquipmentBase_.store(cathode_equipment::Validate(resolver), std::memory_order_release);
    npcEquipmentCommandAddress_.store(
        resolver.ResolveMutable(SymbolId::CathodeNpcEquipmentCommand),
        std::memory_order_release);

    HookRequest request{};
    request.name = "track_spawn_character_result";
    request.target = target;
    request.detour = reinterpret_cast<void*>(&CharacterCreateDetour);
    request.patchLength = spec->approvedPatchLength;
    request.expected = spec->expected;
    request.relocationSafePrefix = spec->trampolineRelocationSafe;
    request.trampolineOut = &characterCreateTrampoline_;
    request.publishedTrampolineOut = &gCharacterCreateTrampoline;
    if (!hooks.Install(request, error)) {
        BeginLogicalShutdown();
        return false;
    }
    hookInstalled_.store(true, std::memory_order_release);
    processLifetimeRequired_.store(true, std::memory_order_release);
    return true;
}

void SpawnCharacterTracker::BeginLogicalShutdown() noexcept {
    ClearRemoteEquipmentRecords();
    lifecycle_.store(nullptr, std::memory_order_release);
    gTracker.store(nullptr, std::memory_order_release);
}

void SpawnCharacterTracker::ClearRemoteEquipmentRecords() noexcept {
    std::lock_guard lock(remoteEquipmentMutex_);
    // Generation invalidation/shutdown can run after the native heap vanished,
    // or off the game thread. Never call native destructors here. Outstanding
    // references are quarantined, not presumed freed by level teardown.
    remoteEquipmentRecords_.clear();
}

void SpawnCharacterTracker::ReapDestroyedRemoteEquipmentRecords() noexcept {
    auto* lifecycle = lifecycle_.load(std::memory_order_acquire);
    if (lifecycle == nullptr || generations_ == nullptr) return;
    try {
        const auto events = lifecycle->Snapshot();
        const auto last = lastLifecycleCleanupSequence_.load(std::memory_order_acquire);
        auto newest = last;
        std::lock_guard lock(remoteEquipmentMutex_);
        for (const auto& event : events) {
            if (event.sequence <= last) continue;
            newest = (std::max)(newest, event.sequence);
            if (event.kind != LifecycleEventKind::CharacterFullDestroyEnd ||
                event.levelGeneration != generations_->CurrentGeneration()) continue;

            const auto found = remoteEquipmentRecords_.find(event.first);
            if (found == remoteEquipmentRecords_.end()) continue;

            auto& records = found->second;
            for (auto record = records.begin(); record != records.end();) {
                if (record->generation != generations_->CurrentGeneration()) {
                    ++record;
                    continue;
                }

                const auto releasable =
                    record->exceptionCode == 0U &&
                    (record->stage == cathode_equipment::Stage::Equipped ||
                     (record->stage == cathode_equipment::Stage::Failed &&
                      !record->giveStarted));
                if (!releasable) {
                    // Faulted or in-flight native state is deliberately kept
                    // quarantined until generation teardown. Never guess that
                    // CATHODE has stopped borrowing its ObjectRefs.
                    ++record;
                    continue;
                }

                cathode_equipment::ReleaseCompleted(
                    cathodeEquipmentBase_.load(std::memory_order_acquire),
                    *record);
                record = records.erase(record);
            }

            if (records.empty())
                remoteEquipmentRecords_.erase(found);
        }
        lastLifecycleCleanupSequence_.store(newest, std::memory_order_release);
    } catch (...) {
        // Teardown observation must not unwind through a game-thread command.
    }
}

void SpawnCharacterTracker::AttachLifecycleCatalog(
    LifecycleCatalog& lifecycle) noexcept {
    lifecycle_.store(&lifecycle, std::memory_order_release);
}

void SpawnCharacterTracker::DetachLifecycleCatalog() noexcept {
    lifecycle_.store(nullptr, std::memory_order_release);
}

void SpawnCharacterTracker::Begin(
    const std::uint64_t levelGeneration,
    const std::uint32_t spawnKey) noexcept {
    Publish(
        levelGeneration,
        spawnKey,
        0,
        0,
        SpawnCharacterTrackingState::Pending);
}

void SpawnCharacterTracker::Fail(
    const std::uint64_t levelGeneration,
    const std::uint32_t spawnKey) noexcept {
    if (state_.load(std::memory_order_acquire) !=
            SpawnCharacterTrackingState::Pending ||
        levelGeneration_.load(std::memory_order_acquire) != levelGeneration ||
        spawnKey_.load(std::memory_order_acquire) != spawnKey) {
        return;
    }
    Publish(
        levelGeneration,
        spawnKey,
        0,
        0,
        SpawnCharacterTrackingState::Failed);
}

void SpawnCharacterTracker::ResetGeneration(
    const std::uint64_t levelGeneration) noexcept {
    character_action_runtime::InvalidateGeneration(levelGeneration);
    if (levelGeneration_.load(std::memory_order_acquire) == levelGeneration) {
        return;
    }
    ClearRemoteEquipmentRecords();
    Publish(
        levelGeneration,
        0,
        0,
        0,
        SpawnCharacterTrackingState::None);
}

void SpawnCharacterTracker::ObserveCreateBegin(
    const std::uintptr_t managerAddress,
    const std::uintptr_t recordAddress,
    const std::uintptr_t requestAddress) noexcept {
    auto* lifecycle = lifecycle_.load(std::memory_order_acquire);
    if (lifecycle == nullptr || generations_ == nullptr) return;
    lifecycle->CaptureFromHook(
        LifecycleEventKind::CharacterCreateBegin,
        generations_->CurrentGeneration(),
        managerAddress,
        recordAddress,
        requestAddress,
        0);
}

void SpawnCharacterTracker::ObserveCreated(
    const std::uint32_t spawnKey,
    const std::uintptr_t managerAddress,
    const std::uintptr_t characterAddress,
    const std::uintptr_t recordAddress,
    const std::uintptr_t requestAddress) noexcept {
    const auto levelGeneration = generations_ == nullptr
        ? 0U
        : generations_->CurrentGeneration();
    auto* lifecycle = lifecycle_.load(std::memory_order_acquire);
    if (lifecycle != nullptr) {
        lifecycle->CaptureFromHook(
            LifecycleEventKind::CharacterCreateEnd,
            levelGeneration,
            managerAddress,
            recordAddress,
            requestAddress,
            characterAddress);
    }
    const bool owned =
        state_.load(std::memory_order_acquire) == SpawnCharacterTrackingState::Pending &&
        levelGeneration_.load(std::memory_order_acquire) == levelGeneration &&
        spawnKey_.load(std::memory_order_acquire) == spawnKey &&
        ValidCharacterAddress(characterAddress) && ValidAddress(recordAddress);
    if (owned) ai_sync::ObserveOwnedCharacter(characterAddress);
    character_action_runtime::ObserveCreated(
        characterAddress, spawnKey, levelGeneration, owned);
    if (state_.load(std::memory_order_acquire) !=
            SpawnCharacterTrackingState::Pending ||
        levelGeneration_.load(std::memory_order_acquire) != levelGeneration ||
        spawnKey_.load(std::memory_order_acquire) != spawnKey) {
        return;
    }
    if (!ValidCharacterAddress(characterAddress) || !ValidAddress(recordAddress)) {
        Fail(levelGeneration, spawnKey);
        return;
    }
    Publish(
        levelGeneration,
        spawnKey,
        characterAddress,
        recordAddress,
        SpawnCharacterTrackingState::Ready);
}



RemoteEquipmentProvisionResult
SpawnCharacterTracker::ProvisionRemoteEquipmentOnGameThread(
    const std::uintptr_t characterAddress,
    std::string compositeName,
    const std::uint32_t equipmentEntityShortGuid,
    const std::uint32_t equipmentOutputShortGuid,
    const std::uint32_t spawnSchedulerParameterShortGuid,
    const std::uint32_t spawnTargetInputShortGuid,
    const std::uint32_t equipmentSlot) {
    using cathode_equipment::Result;
    using cathode_equipment::Stage;

    if (!ValidCharacterAddress(characterAddress) || generations_ == nullptr)
        return {false, false, "REMOTE_EQUIPMENT_INVALID_CHARACTER", "Invalid CharacterBase."};
    if (compositeName.empty() || compositeName.size() > 255U)
        return {false, false, "REMOTE_EQUIPMENT_INVALID_CATALOG", "Invalid equipment composite name."};

    const auto runtimeId = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(characterAddress + 0x50U));
    if (!runtimeId.has_value())
        return {false, false, "REMOTE_EQUIPMENT_INVALID_CHARACTER", "Unreadable Character runtime identity."};

    ReapDestroyedRemoteEquipmentRecords();
    std::lock_guard lock(remoteEquipmentMutex_);
    auto& records = remoteEquipmentRecords_[characterAddress];
    const auto currentGeneration = generations_->CurrentGeneration();

    const auto sameRuntime = [&](const cathode_equipment::Record& candidate) {
        return candidate.generation == currentGeneration &&
            candidate.runtimeId == *runtimeId;
    };
    const auto sameCatalog = [&](const cathode_equipment::Record& candidate) {
        return sameRuntime(candidate) &&
            candidate.compositeName == compositeName &&
            candidate.equipmentEntityId == equipmentEntityShortGuid &&
            candidate.outputId == equipmentOutputShortGuid &&
            candidate.spawnId == spawnSchedulerParameterShortGuid &&
            candidate.targetInputId == spawnTargetInputShortGuid &&
            candidate.expectedSlot == equipmentSlot;
    };

    // A native exception means we no longer know which holders CATHODE may
    // still borrow. Quarantine the whole Character equipment bridge rather
    // than retrying a different archetype over uncertain state.
    if (std::any_of(records.begin(), records.end(),
            [&](const cathode_equipment::Record& candidate) {
                return sameRuntime(candidate) && candidate.exceptionCode != 0U;
            })) {
        return {false, false, "REMOTE_EQUIPMENT_QUARANTINED",
            "A previous native equipment operation faulted for this Character; no automatic retry is allowed."};
    }

    auto found = std::find_if(
        records.begin(), records.end(),
        [&](const cathode_equipment::Record& candidate) {
            if (!sameCatalog(candidate)) return false;
            return candidate.stage != Stage::Equipped ||
                cathode_equipment::IsAttached(characterAddress, candidate);
        });
    bool inserted = false;
    if (found == records.end()) {
        const auto operationActive = std::any_of(
            records.begin(), records.end(),
            [&](const cathode_equipment::Record& candidate) {
                if (!sameRuntime(candidate)) return false;
                if (candidate.stage == Stage::Empty ||
                    candidate.stage == Stage::WaitingOutput ||
                    candidate.stage == Stage::GiveDispatched)
                    return true;
                return candidate.stage == Stage::Failed && candidate.giveStarted;
            });
        if (operationActive) {
            return {false, false, "REMOTE_EQUIPMENT_OPERATION_ACTIVE",
                "A different equipment provisioning operation is still active or owns an in-flight Give for this Character."};
        }

        records.emplace_back();
        found = std::prev(records.end());
        inserted = true;
        found->generation = currentGeneration;
        found->runtimeId = *runtimeId;
        found->compositeName = std::move(compositeName);
        found->equipmentEntityId = equipmentEntityShortGuid;
        found->outputId = equipmentOutputShortGuid;
        found->spawnId = spawnSchedulerParameterShortGuid;
        found->targetInputId = spawnTargetInputShortGuid;
        found->expectedSlot = equipmentSlot;
    }

    auto& record = *found;
    const auto previous = record.stage;
    const auto* previousOperation = record.operation;
    const auto result = cathode_equipment::Step(
        cathodeEquipmentBase_.load(std::memory_order_acquire),
        characterAddress,
        record);

    if (diagnostics_ != nullptr &&
        (inserted || record.stage != previous ||
         record.operation != previousOperation ||
         (result != Result::Pending && previous != Stage::Equipped))) {
        diagnostics_->Log(
            result == Result::Pending || result == Result::Equipped
                ? "info" : "warning",
            "PlayerSync.RemoteEquipment",
            cathode_equipment::Describe(result),
            {{"stage", record.operation},
             {"provisionStage", Diagnostics::Unsigned(
                 static_cast<std::uint32_t>(record.stage)), true},
             {"composite", record.compositeName},
             {"expectedEquipmentSlot", Diagnostics::Unsigned(record.expectedSlot), true},
             {"equipmentSlot", Diagnostics::Unsigned(record.equipmentSlot), true},
             {"characterBase", Diagnostics::Pointer(
                 reinterpret_cast<void*>(characterAddress))},
             {"characterRuntimeId", Diagnostics::Unsigned(record.runtimeId), true},
             {"retainedEquipmentGraphs", Diagnostics::Unsigned(
                 static_cast<std::uint64_t>(records.size())), true},
             {"rootSharedRef", Diagnostics::Pointer(record.root)},
             {"runtimeCompositeSharedRef", Diagnostics::Pointer(record.composite)},
             {"outputQuerySucceeded", Diagnostics::Boolean(
                 record.outputQuerySucceeded), true},
             {"weaponSharedRef", Diagnostics::Pointer(record.weapon)},
             {"characterNodeSharedRef", Diagnostics::Pointer(record.characterNode)},
             {"giveNodeSharedRef", Diagnostics::Pointer(record.giveNode)},
             {"resolvedCharacter", Diagnostics::Pointer(record.resolvedCharacter)},
             {"equipmentInstance", Diagnostics::Pointer(record.resolvedEquipment)},
             {"initialResourcePolicy", record.initialResourcePolicy},
             {"initialResourcesBefore", std::to_string(record.initialResourcesBefore), true},
             {"initialResourcesAfter", std::to_string(record.initialResourcesAfter), true},
             {"nativeResourceCapacity", std::to_string(record.nativeResourceCapacity), true},
             {"exceptionCode", Diagnostics::Unsigned(record.exceptionCode), true},
             {"polls", Diagnostics::Unsigned(record.polls), true}});
    }

    if (result == Result::Equipped) {
        std::uint32_t selectionException = 0U;
        const auto selection = EnsureEquipmentSelected(
            characterAddress,
            record.equipmentSlot,
            npcEquipmentCommandAddress_.load(std::memory_order_acquire),
            selectionException);
        if (selection.selected) {
            return {
                true,
                false,
                selection.statusCode,
                std::string(selection.message) + " slot=" +
                    std::to_string(record.equipmentSlot) + "."};
        }
        if (selection.retryable) {
            return {
                false,
                true,
                selection.statusCode,
                std::string(selection.message) + " slot=" +
                    std::to_string(record.equipmentSlot) + "."};
        }
        if (diagnostics_ != nullptr) {
            diagnostics_->Log(
                "warning",
                "PlayerSync.RemoteEquipmentSelect",
                selection.message,
                {{"statusCode", selection.statusCode},
                 {"characterBase", Diagnostics::Pointer(
                     reinterpret_cast<void*>(characterAddress))},
                 {"equipmentSlot", Diagnostics::Unsigned(record.equipmentSlot), true},
                 {"exceptionCode", Diagnostics::Unsigned(selectionException), true}});
        }
        return {
            false,
            false,
            selection.statusCode,
            std::string(selection.message) + " slot=" +
                std::to_string(record.equipmentSlot) + "."};
    }

    const auto slotText = result == Result::Equipped
        ? " slot=" + std::to_string(record.equipmentSlot) + "."
        : std::string{};
    return {
        false,
        result == Result::Pending,
        result == Result::Pending ? "REMOTE_EQUIPMENT_NOT_READY" :
                                    "REMOTE_EQUIPMENT_CATHODE_FAILED",
        std::string(record.operation) + ": " +
            cathode_equipment::Describe(result) + slotText};
}

SpawnCharacterTrackingSnapshot SpawnCharacterTracker::Snapshot() const noexcept {
    for (;;) {
        const auto before = version_.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;
        SpawnCharacterTrackingSnapshot snapshot{};
        snapshot.levelGeneration =
            levelGeneration_.load(std::memory_order_acquire);
        snapshot.spawnKey = spawnKey_.load(std::memory_order_acquire);
        snapshot.characterAddress =
            characterAddress_.load(std::memory_order_acquire);
        snapshot.recordAddress = recordAddress_.load(std::memory_order_acquire);
        snapshot.state = state_.load(std::memory_order_acquire);
        snapshot.hookInstalled = HookInstalled();
        const auto after = version_.load(std::memory_order_acquire);
        if (before == after) {
            snapshot.version = after;
            return snapshot;
        }
    }
}

bool SpawnCharacterTracker::HookInstalled() const noexcept {
    return hookInstalled_.load(std::memory_order_acquire) &&
        characterCreateTrampoline_ != nullptr;
}

bool SpawnCharacterTracker::ProcessLifetimeRequired() const noexcept {
    return processLifetimeRequired_.load(std::memory_order_acquire);
}

std::uint32_t SpawnCharacterTracker::ActiveDetours() noexcept {
    return gActiveDetours.load(std::memory_order_acquire);
}

void SpawnCharacterTracker::Publish(
    const std::uint64_t levelGeneration,
    const std::uint32_t spawnKey,
    const std::uintptr_t characterAddress,
    const std::uintptr_t recordAddress,
    const SpawnCharacterTrackingState state) noexcept {
    while (publishLock_.test_and_set(std::memory_order_acquire)) {
    }
    version_.fetch_add(1, std::memory_order_acq_rel);
    levelGeneration_.store(levelGeneration, std::memory_order_release);
    spawnKey_.store(spawnKey, std::memory_order_release);
    characterAddress_.store(characterAddress, std::memory_order_release);
    recordAddress_.store(recordAddress, std::memory_order_release);
    state_.store(state, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_release);
    publishLock_.clear(std::memory_order_release);
}

const char* ToString(const SpawnCharacterTrackingState state) noexcept {
    switch (state) {
    case SpawnCharacterTrackingState::None: return "None";
    case SpawnCharacterTrackingState::Pending: return "Pending";
    case SpawnCharacterTrackingState::Ready: return "Ready";
    case SpawnCharacterTrackingState::Failed: return "Failed";
    }
    return "None";
}

} // namespace aimp
