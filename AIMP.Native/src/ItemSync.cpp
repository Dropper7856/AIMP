#include "aimp/ItemSync.h"

#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"

#include <Windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace aimp::item_sync {
namespace {

// Retail pickup manager ("PM", ctor FUN_007C0570). Slots are registered by
// the spawned pickup's interactable; see analysis/itemsync/pickup_re.md.
constexpr std::uintptr_t kPickupManager = 0x012F0DE8U;
constexpr std::uintptr_t kRequestRemoval = 0x003A2B30U; // FUN_007A2B30 thiscall(PM, ref*) RET 4
constexpr std::uintptr_t kCollectCall = 0x003AFDE4U;    // CALL in the pickup_collected callback
constexpr std::uintptr_t kCollectReturn = 0x003AFDE9U;
constexpr std::uintptr_t kRemove = 0x003A3200U;         // FUN_007A3200 thiscall(PM+1C, key*) RET 4
constexpr std::uintptr_t kFindSlot = 0x00393F30U;       // FUN_00793F30 thiscall(PM, key*) RET 4
constexpr std::uintptr_t kExtractKey = 0x004C3360U;     // FUN_008C3360 thiscall(entity, out*) RET 4
constexpr std::uintptr_t kSlotTable = 0x3CU;
constexpr std::uintptr_t kSlotStride = 0x9CU;
constexpr std::uint32_t kSlotCount = 0x400U;
constexpr std::uintptr_t kSlotPosition = 0x4CU;
constexpr std::uintptr_t kSlotOwner = 0x68U;
constexpr std::uintptr_t kSlotRef = 0x70U;
constexpr std::uintptr_t kSlotFlags = 0x96U;
constexpr std::uint8_t kSlotInUse = 0x04U;
constexpr std::uintptr_t kRefObject = 0x0CU;
constexpr std::uintptr_t kRemovalContext = 0x1CU;

constexpr std::size_t kObservedCapacity = 64U;
constexpr std::size_t kPendingCapacity = 256U;
constexpr std::size_t kAppliedCapacity = 1024U;
constexpr std::uint64_t kPumpIntervalMs = 250U;
constexpr float kMultipleMatchRadius = 1.5F;
constexpr float kPositionMismatchRadius = 3.0F;

using RequestRemovalFn = std::uintptr_t(__thiscall*)(void* manager, int* sharedRef);
using RemoveFn = void(__thiscall*)(void* context, const std::uint32_t* compositeKey);
using FindSlotFn = std::uintptr_t(__thiscall*)(void* manager, const std::uint32_t* compositeKey);
using ExtractKeyFn = std::uint32_t*(__thiscall*)(void* entity, std::uint32_t* output);

struct Observed final {
    std::uint32_t ownerShortGuid{}, ownerInstance{};
    float x{}, y{}, z{};
};

struct Entry final {
    WorldEntityId entity{};
    std::uint64_t generation{};
};

std::uintptr_t gBase{};
LevelGenerationTracker* gGenerations{};
void* gRequestRemovalTrampoline{};
std::atomic<void*> gRequestRemovalOriginal{};
std::atomic<bool> gInstalled{}, gEnabled{};

SRWLOCK gObservedLock = SRWLOCK_INIT;
std::array<PickupEvent, kObservedCapacity> gObserved{};
std::size_t gObservedRead{}, gObservedCount{};
std::uint64_t gNextSequence{1U};

// Engine-thread state, still locked: the dispatcher and the CharacterManager
// pump both run on the engine thread, but diagnostics read it elsewhere.
SRWLOCK gStateLock = SRWLOCK_INIT;
std::array<Entry, kPendingCapacity> gPending{};
std::size_t gPendingCount{};
std::array<Entry, kAppliedCapacity> gApplied{};
std::size_t gAppliedCount{}, gAppliedNext{};
std::uint64_t gLastPumpTick{};

std::atomic<std::uint64_t> gCollectSeen{}, gOtherCallers{}, gNoOwner{}, gReadFaults{};
std::atomic<std::uint64_t> gObservedOverflow{}, gObservedQueued{};
std::atomic<std::uint64_t> gApplyRequests{}, gAppliedTotal{}, gAlreadyApplied{}, gDeferred{};
std::atomic<std::uint64_t> gPendingApplied{}, gPendingDropped{}, gAmbiguous{};
std::atomic<std::uint64_t> gPositionMismatch{}, gApplyFaults{}, gRejected{};
std::atomic<std::uint64_t> gLastLocal{}, gLastRemote{};

template<typename T>
bool Read(const std::uintptr_t address, T& value) noexcept {
    __try {
        value = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Match(const std::uintptr_t rva, const std::initializer_list<unsigned char> bytes) noexcept {
    std::size_t offset{};
    for (const auto expected : bytes) {
        unsigned char actual{};
        if (!Read(gBase + rva + offset++, actual) || actual != expected) return false;
    }
    return true;
}

std::uintptr_t PickupManager() noexcept {
    std::uintptr_t manager{};
    return gBase != 0U && Read(gBase + kPickupManager, manager) ? manager : 0U;
}

bool ActiveGeneration(std::uint64_t& generation) noexcept {
    if (gGenerations == nullptr) return false;
    generation = gGenerations->CurrentGeneration();
    return generation != 0U && gGenerations->IsStableGeneration(generation);
}

// Engine thread, inside the retail collection request: the slot still holds
// the pickup reference (FUN_007A3200 releases it later).
bool ReadCollected(const std::uintptr_t manager, int* sharedRef, Observed& output) noexcept {
    __try {
        const auto block = *reinterpret_cast<const std::uintptr_t*>(sharedRef);
        if (block == 0U) return false;
        const auto object = *reinterpret_cast<const std::uintptr_t*>(block + kRefObject);
        if (object == 0U) return false;
        std::uint32_t key[2]{};
        reinterpret_cast<ExtractKeyFn>(gBase + kExtractKey)(reinterpret_cast<void*>(object), key);
        const auto slot = reinterpret_cast<FindSlotFn>(gBase + kFindSlot)(
            reinterpret_cast<void*>(manager), key);
        if (slot == 0U) return false;
        output.ownerShortGuid = *reinterpret_cast<const std::uint32_t*>(slot + kSlotOwner);
        output.ownerInstance = *reinterpret_cast<const std::uint32_t*>(slot + kSlotOwner + 4U);
        output.x = *reinterpret_cast<const float*>(slot + kSlotPosition);
        output.y = *reinterpret_cast<const float*>(slot + kSlotPosition + 4U);
        output.z = *reinterpret_cast<const float*>(slot + kSlotPosition + 8U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ContainsApplied(const WorldEntityId& entity, const std::uint64_t generation) noexcept {
    for (std::size_t i = 0U; i < gAppliedCount; ++i)
        if (gApplied[i].generation == generation && gApplied[i].entity == entity) return true;
    return false;
}

void RememberApplied(const WorldEntityId& entity, const std::uint64_t generation) noexcept {
    if (ContainsApplied(entity, generation)) return;
    gApplied[gAppliedNext] = Entry{entity, generation};
    gAppliedNext = (gAppliedNext + 1U) % kAppliedCapacity;
    if (gAppliedCount < kAppliedCapacity) ++gAppliedCount;
}

void Publish(const Observed& observed, const std::uint64_t generation) noexcept {
    ItemKey key{};
    key.ownerShortGuid = observed.ownerShortGuid;
    key.ownerInstance = observed.ownerInstance;
    key.x = QuantizeItemCoordinate(observed.x);
    key.y = QuantizeItemCoordinate(observed.y);
    key.z = QuantizeItemCoordinate(observed.z);
    const auto entity = EncodeItemKey(key);
    gLastLocal.store(entity.low, std::memory_order_relaxed);

    AcquireSRWLockExclusive(&gStateLock);
    // The local player owns this pickup now. A late ItemCollected for the same
    // key (simultaneous collection elsewhere) must not wait for a pickup that
    // no longer exists here.
    RememberApplied(entity, generation);
    ReleaseSRWLockExclusive(&gStateLock);

    AcquireSRWLockExclusive(&gObservedLock);
    if (gObservedCount == kObservedCapacity) {
        ReleaseSRWLockExclusive(&gObservedLock);
        gObservedOverflow.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    gObserved[(gObservedRead + gObservedCount) % kObservedCapacity] =
        PickupEvent{gNextSequence++, entity};
    ++gObservedCount;
    ReleaseSRWLockExclusive(&gObservedLock);
    gObservedQueued.fetch_add(1U, std::memory_order_relaxed);
}

std::uintptr_t __fastcall RequestRemovalDetour(void* manager, void*, int* sharedRef) noexcept {
    const auto original = reinterpret_cast<RequestRemovalFn>(
        gRequestRemovalOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (gEnabled.load(std::memory_order_acquire) && sharedRef != nullptr) {
        // Only the pickup_collected output callback is a collection. The other
        // caller is PickupSpawner's despawn, which is level logic, not a pickup.
        if (caller == gBase + kCollectReturn) {
            gCollectSeen.fetch_add(1U, std::memory_order_relaxed);
            Observed observed{};
            std::uint64_t generation{};
            if (!ReadCollected(reinterpret_cast<std::uintptr_t>(manager), sharedRef, observed)) {
                gReadFaults.fetch_add(1U, std::memory_order_relaxed);
            } else if (observed.ownerShortGuid == 0U && observed.ownerInstance == 0U) {
                // Player-dropped pickup: no level-authored identity.
                gNoOwner.fetch_add(1U, std::memory_order_relaxed);
            } else if (ActiveGeneration(generation)) {
                Publish(observed, generation);
            }
        } else {
            gOtherCallers.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    return original(manager, sharedRef);
}

enum class FindResult : std::uint8_t { Found, NotFound, Ambiguous, Fault };

// Engine thread. Every in-use slot spawned by the owner; one candidate is
// accepted as is, several (MultiplePickupSpawner) need the nearest position.
FindResult FindPickup(const std::uintptr_t manager, const ItemKey& key, std::uintptr_t& object,
    bool& positionMismatch) noexcept {
    __try {
        const float x = ItemCoordinate(key.x), y = ItemCoordinate(key.y), z = ItemCoordinate(key.z);
        std::uintptr_t first{}, nearest{};
        float firstDistance{}, nearestDistance{kMultipleMatchRadius};
        std::uint32_t candidates{};
        for (std::uint32_t i = 0U; i < kSlotCount; ++i) {
            const auto slot = manager + kSlotTable + i * kSlotStride;
            if ((*reinterpret_cast<const std::uint8_t*>(slot + kSlotFlags) & kSlotInUse) == 0U) continue;
            const auto block = *reinterpret_cast<const std::uintptr_t*>(slot + kSlotRef);
            if (block == 0U ||
                *reinterpret_cast<const std::uint32_t*>(slot + kSlotOwner) != key.ownerShortGuid ||
                *reinterpret_cast<const std::uint32_t*>(slot + kSlotOwner + 4U) != key.ownerInstance)
                continue;
            const auto candidate = *reinterpret_cast<const std::uintptr_t*>(block + kRefObject);
            if (candidate == 0U) continue;
            const float dx = *reinterpret_cast<const float*>(slot + kSlotPosition) - x;
            const float dy = *reinterpret_cast<const float*>(slot + kSlotPosition + 4U) - y;
            const float dz = *reinterpret_cast<const float*>(slot + kSlotPosition + 8U) - z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (++candidates == 1U) { first = candidate; firstDistance = distance; }
            if (std::isfinite(distance) && distance <= nearestDistance) {
                nearest = candidate;
                nearestDistance = distance;
            }
        }
        if (candidates == 0U) return FindResult::NotFound;
        if (candidates == 1U) {
            positionMismatch = !std::isfinite(firstDistance) || firstDistance > kPositionMismatchRadius;
            object = first;
            return FindResult::Found;
        }
        if (nearest == 0U) return FindResult::Ambiguous;
        object = nearest;
        return FindResult::Found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FindResult::Fault;
    }
}

bool RemovePickup(const std::uintptr_t manager, const std::uintptr_t object) noexcept {
    __try {
        std::uint32_t key[2]{};
        reinterpret_cast<ExtractKeyFn>(gBase + kExtractKey)(reinterpret_cast<void*>(object), key);
        // Same removal the retail collection reaches: marks the spawned
        // composite for destruction, releases the slot and tells the owner it
        // was collected (GCIP persistence). The inventory is never touched.
        reinterpret_cast<RemoveFn>(gBase + kRemove)(
            reinterpret_cast<void*>(manager + kRemovalContext), key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum class TryResult : std::uint8_t { Applied, NotYet, Fault };

// Caller holds gStateLock.
TryResult TryApply(const WorldEntityId& entity, const std::uint64_t generation) noexcept {
    ItemKey key{};
    if (!DecodeItemKey(entity, key)) return TryResult::Fault;
    const auto manager = PickupManager();
    if (manager == 0U) return TryResult::NotYet;
    std::uintptr_t object{};
    bool positionMismatch{};
    switch (FindPickup(manager, key, object, positionMismatch)) {
    case FindResult::NotFound: return TryResult::NotYet;
    case FindResult::Ambiguous:
        gAmbiguous.fetch_add(1U, std::memory_order_relaxed);
        return TryResult::NotYet;
    case FindResult::Fault: return TryResult::Fault;
    case FindResult::Found: break;
    }
    if (positionMismatch) gPositionMismatch.fetch_add(1U, std::memory_order_relaxed);
    if (!RemovePickup(manager, object)) return TryResult::Fault;
    RememberApplied(entity, generation);
    gLastRemote.store(entity.low, std::memory_order_relaxed);
    return TryResult::Applied;
}

bool AddPending(const WorldEntityId& entity, const std::uint64_t generation) noexcept {
    for (std::size_t i = 0U; i < gPendingCount; ++i)
        if (gPending[i].generation == generation && gPending[i].entity == entity) return true;
    if (gPendingCount == kPendingCapacity) return false;
    gPending[gPendingCount++] = Entry{entity, generation};
    return true;
}

} // namespace

bool Install(const std::uintptr_t moduleBase, HookManager& hooks,
    LevelGenerationTracker& generations, std::string& error) {
    if (Installed()) return true;
    if (moduleBase == 0U || !hooks.MutationAllowed()) {
        error = "ItemSync requires the SHA-256-certified executable and hook opt-in.";
        return false;
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    gBase = moduleBase;
    // CALL rel32 at 007AFDE4 must target 007A2B30: it identifies the
    // pickup_collected caller by its return address.
    std::int32_t collectRelative{};
    if (!Match(kRequestRemoval, {0x8B,0x44,0x24,0x04,0x8B,0x00,0x83,0xEC,0x08,0x56,0x8B,0xF1}) ||
        !Match(kRequestRemoval + 0x5CU, {0xC2,0x04,0x00}) ||
        !Match(kCollectCall, {0xE8}) ||
        !Read(gBase + kCollectCall + 1U, collectRelative) ||
        kCollectReturn + static_cast<std::uintptr_t>(collectRelative) != kRequestRemoval ||
        !Match(kRemove, {0x55,0x8B,0xEC,0x83,0xE4,0xC0,0x6A,0xFF}) ||
        !Match(kRemove + 0xD7U, {0xC2,0x04,0x00}) ||
        !Match(kFindSlot, {0x83,0xEC,0x08,0x53,0x55,0x8B,0x6C,0x24,0x14}) ||
        !Match(kFindSlot + 0x55U, {0xC2,0x04,0x00}) ||
        !Match(kExtractKey, {0x55,0x8B,0xEC,0x51,0x56,0x8B,0xF1})) {
        gBase = 0U;
        error = "ItemSync retail pickup-manager ABI mismatch.";
        return false;
    }
    gGenerations = &generations;
    HookRequest request;
    request.name = "itemsync_pickup_collection_request";
    request.target = reinterpret_cast<void*>(gBase + kRequestRemoval);
    request.detour = reinterpret_cast<void*>(&RequestRemovalDetour);
    // mov eax,[esp+4] ; mov eax,[eax]  (6 bytes, no relative operand)
    request.expected.bytes = {0x8BU,0x44U,0x24U,0x04U,0x8BU,0x00U};
    request.expected.mask.assign(request.expected.bytes.size(), 'x');
    request.patchLength = request.expected.bytes.size();
    request.relocationSafePrefix = true;
    request.trampolineOut = &gRequestRemovalTrampoline;
    request.publishedTrampolineOut = &gRequestRemovalOriginal;
    gEnabled.store(true, std::memory_order_release);
    if (!hooks.Install(request, error)) {
        gEnabled.store(false, std::memory_order_release);
        error = request.name + ": " + error;
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    return true;
#else
    (void)hooks;
    (void)generations;
    error = "ItemSync requires the MSVC x86 retail ABI.";
    return false;
#endif
}

bool Installed() noexcept {
    return gInstalled.load(std::memory_order_acquire);
}

void Shutdown() noexcept {
    gEnabled.store(false, std::memory_order_release);
}

bool TryPeekObservedPickup(PickupEvent& event) noexcept {
    AcquireSRWLockShared(&gObservedLock);
    const bool available = gObservedCount != 0U;
    if (available) event = gObserved[gObservedRead];
    ReleaseSRWLockShared(&gObservedLock);
    return available;
}

void PopObservedPickup() noexcept {
    AcquireSRWLockExclusive(&gObservedLock);
    if (gObservedCount != 0U) {
        gObservedRead = (gObservedRead + 1U) % kObservedCapacity;
        --gObservedCount;
    }
    ReleaseSRWLockExclusive(&gObservedLock);
}

ApplyStatus ApplyRemoteCollected(const WorldEntityId& entity) noexcept {
    gApplyRequests.fetch_add(1U, std::memory_order_relaxed);
    ItemKey key{};
    if (!DecodeItemKey(entity, key)) {
        gRejected.fetch_add(1U, std::memory_order_relaxed);
        return ApplyStatus::Rejected;
    }
    std::uint64_t generation{};
    if (!gEnabled.load(std::memory_order_acquire) || !ActiveGeneration(generation))
        return ApplyStatus::Unavailable;

    AcquireSRWLockExclusive(&gStateLock);
    ApplyStatus status{};
    if (ContainsApplied(entity, generation)) {
        status = ApplyStatus::AlreadyApplied;
    } else {
        switch (TryApply(entity, generation)) {
        case TryResult::Applied: status = ApplyStatus::Applied; break;
        case TryResult::Fault: status = ApplyStatus::Fault; break;
        case TryResult::NotYet:
            status = AddPending(entity, generation) ? ApplyStatus::Deferred : ApplyStatus::Rejected;
            break;
        }
    }
    ReleaseSRWLockExclusive(&gStateLock);

    switch (status) {
    case ApplyStatus::Applied: gAppliedTotal.fetch_add(1U, std::memory_order_relaxed); break;
    case ApplyStatus::AlreadyApplied: gAlreadyApplied.fetch_add(1U, std::memory_order_relaxed); break;
    case ApplyStatus::Deferred: gDeferred.fetch_add(1U, std::memory_order_relaxed); break;
    case ApplyStatus::Fault: gApplyFaults.fetch_add(1U, std::memory_order_relaxed); break;
    case ApplyStatus::Rejected: gRejected.fetch_add(1U, std::memory_order_relaxed); break;
    case ApplyStatus::Unavailable: break;
    }
    return status;
}

void PumpOnEngineThread() noexcept {
    if (!gEnabled.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    if (now - gLastPumpTick < kPumpIntervalMs) return;
    gLastPumpTick = now;
    std::uint64_t generation{};
    const bool active = ActiveGeneration(generation);

    AcquireSRWLockExclusive(&gStateLock);
    std::size_t kept = 0U;
    for (std::size_t i = 0U; i < gPendingCount; ++i) {
        const auto entry = gPending[i];
        if (!active) {
            // Loading or frontend: keep only the current generation's entries.
            if (entry.generation == generation) gPending[kept++] = entry;
            else gPendingDropped.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (entry.generation != generation) {
            gPendingDropped.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (ContainsApplied(entry.entity, generation)) continue;
        switch (TryApply(entry.entity, generation)) {
        case TryResult::Applied:
            gPendingApplied.fetch_add(1U, std::memory_order_relaxed);
            break;
        case TryResult::Fault:
            gApplyFaults.fetch_add(1U, std::memory_order_relaxed);
            break;
        case TryResult::NotYet:
            gPending[kept++] = entry;
            break;
        }
    }
    gPendingCount = kept;
    ReleaseSRWLockExclusive(&gStateLock);
}

const char* ApplyStatusCode(const ApplyStatus status) noexcept {
    switch (status) {
    case ApplyStatus::Applied: return "ITEM_SYNC_APPLIED";
    case ApplyStatus::AlreadyApplied: return "ITEM_SYNC_ALREADY_APPLIED";
    case ApplyStatus::Deferred: return "ITEM_SYNC_DEFERRED";
    case ApplyStatus::Unavailable: return "ITEM_SYNC_UNAVAILABLE";
    case ApplyStatus::Rejected: return "ITEM_SYNC_REJECTED";
    case ApplyStatus::Fault: return "ITEM_SYNC_NATIVE_FAULT";
    }
    return "ITEM_SYNC_UNKNOWN";
}

const char* ApplyStatusMessage(const ApplyStatus status) noexcept {
    switch (status) {
    case ApplyStatus::Applied: return "Pickup removed through the pickup manager.";
    case ApplyStatus::AlreadyApplied: return "Pickup was already removed or collected locally.";
    case ApplyStatus::Deferred: return "Pickup is not spawned locally yet; removal stays pending.";
    case ApplyStatus::Unavailable: return "No stable gameplay level or ItemSync is disabled.";
    case ApplyStatus::Rejected: return "Item key is invalid or the pending queue is full.";
    case ApplyStatus::Fault: return "The native pickup removal raised a protected exception.";
    }
    return "Unknown ItemSync status.";
}

std::string EntityIdHex(const WorldEntityId& entity) {
    std::array<char, 33> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%016llX%016llX",
        static_cast<unsigned long long>(entity.high),
        static_cast<unsigned long long>(entity.low));
    return std::string(buffer.data());
}

void LogDiagnostics(Diagnostics& diagnostics) {
    std::size_t pending{};
    AcquireSRWLockShared(&gStateLock);
    pending = gPendingCount;
    ReleaseSRWLockShared(&gStateLock);
    diagnostics.Log("info", "itemsync",
        "World pickups: local collections are reported to the Host; pickups collected elsewhere are removed through the retail pickup manager.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"enabled", Diagnostics::Boolean(gEnabled.load()), true},
         {"collectSeen", Diagnostics::Unsigned(gCollectSeen.load()), true},
         {"otherCallers", Diagnostics::Unsigned(gOtherCallers.load()), true},
         {"noOwner", Diagnostics::Unsigned(gNoOwner.load()), true},
         {"readFaults", Diagnostics::Unsigned(gReadFaults.load()), true},
         {"observedQueued", Diagnostics::Unsigned(gObservedQueued.load()), true},
         {"observedOverflow", Diagnostics::Unsigned(gObservedOverflow.load()), true},
         {"applyRequests", Diagnostics::Unsigned(gApplyRequests.load()), true},
         {"applied", Diagnostics::Unsigned(gAppliedTotal.load()), true},
         {"alreadyApplied", Diagnostics::Unsigned(gAlreadyApplied.load()), true},
         {"deferred", Diagnostics::Unsigned(gDeferred.load()), true},
         {"pendingApplied", Diagnostics::Unsigned(gPendingApplied.load()), true},
         {"pendingDropped", Diagnostics::Unsigned(gPendingDropped.load()), true},
         {"pending", Diagnostics::Unsigned(pending), true},
         {"ambiguous", Diagnostics::Unsigned(gAmbiguous.load()), true},
         {"positionMismatch", Diagnostics::Unsigned(gPositionMismatch.load()), true},
         {"applyFaults", Diagnostics::Unsigned(gApplyFaults.load()), true},
         {"rejected", Diagnostics::Unsigned(gRejected.load()), true},
         {"lastLocalOwner", Diagnostics::Unsigned(gLastLocal.load()), true},
         {"lastRemoteOwner", Diagnostics::Unsigned(gLastRemote.load()), true}});
}

} // namespace aimp::item_sync
