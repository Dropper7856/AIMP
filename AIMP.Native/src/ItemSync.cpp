#include "aimp/ItemSync.h"

#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/WorldPublicActions.h"

#include <Windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

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


// ---- Spawn authority and drops --------------------------------------------
// See analysis/itemsync/spawn_drop_re.md. FUN_007A2B90 is the only owner spawn
// request (GCIP, PickupSpawner, MultiplePickupSpawner); it copies every
// argument into a bus record, so it can be called later with our buffers.
constexpr std::uintptr_t kGlobalRoot = 0x012F0C88U;
constexpr std::uintptr_t kItemDatabase = 0x40U;        // [root+40]: node list at +4 {itemDef, ?, next}
constexpr std::uintptr_t kItemDefPath = 0x44U;         // char* composite path
constexpr std::uintptr_t kSpawnRequest = 0x003A2B90U;  // thiscall(PM, path, qty, xf[12], owner*) RET 10
constexpr std::uintptr_t kGcipSpawnCall = 0x001CD40BU; // CALL in FUN_005CD390 (GCIP spawn/restore helper)
constexpr std::uintptr_t kGcipSpawnReturn = 0x001CD410U;
constexpr std::uintptr_t kGcipSpawn = 0x001CD430U;     // GCIP spawn method thiscall(iface, ctx, ?) RET 8
constexpr std::uintptr_t kGcipInterface = 0x14U;       // iface = entity + 0x14
constexpr std::uintptr_t kDropRequest = 0x003A2D50U;   // thiscall(PM, itemDef, qty, pos[3]) RET C
constexpr std::size_t kSpawnObservedCapacity = 1024U; // a level load proposes every GCIP at once
constexpr std::size_t kOwnerCapacity = 4096U;
constexpr std::size_t kDropTagCapacity = 16U;
constexpr std::uint64_t kDropTagTimeoutMs = 5000U;
constexpr float kDropTagRadius = 1.0F;

using SpawnRequestFn = void(__thiscall*)(void* manager, const char* path, std::int32_t quantity,
    const float* transform, const std::uint32_t* ownerKey);
using GcipSpawnFn = std::uint8_t(__thiscall*)(void* iface, void* context, void* argument);
using DropRequestFn = void(__thiscall*)(void* manager, void* itemDef, std::int32_t quantity,
    const float* position);

struct OwnerState final {
    std::uint64_t key{};
    bool ran{}, proposed{}, decided{}, applied{};
    SpawnData decision{};
};

struct DropTag final {
    std::uint64_t key{};
    float position[3]{};
    std::array<std::uint32_t, kSlotCount / 32U> before{};
    std::uint64_t deadline{};
};

// The GCIP spawn method running on the engine thread, if any.
struct GcipContext final {
    bool active{};
    std::uint64_t key{};
    bool sawRequest{};
};

void* gSpawnTrampoline{};
std::atomic<void*> gSpawnOriginal{};
void* gGcipTrampoline{};
std::atomic<void*> gGcipOriginal{};
void* gDropTrampoline{};
std::atomic<void*> gDropOriginal{};

SRWLOCK gSpawnObservedLock = SRWLOCK_INIT;
std::array<SpawnEvent, kSpawnObservedCapacity> gSpawnObserved{};
std::size_t gSpawnObservedRead{}, gSpawnObservedCount{};
std::uint64_t gNextSpawnSequence{1U};

// Owner table and drop tags are guarded by gStateLock.
std::array<OwnerState, kOwnerCapacity> gOwners{};
std::size_t gOwnerCount{};
std::uint64_t gOwnerGeneration{};
std::array<DropTag, kDropTagCapacity> gDropTags{};
std::size_t gDropTagCount{};
std::uint32_t gDropNonce{};
std::uint32_t gNextDropSequence{1U};
GcipContext gGcip{}; // engine thread only

std::atomic<std::uint64_t> gGcipRuns{}, gOwnerRequestsBlocked{}, gOwnerProposals{};
std::atomic<std::uint64_t> gOwnerNoneProposals{}, gOwnerDecisions{}, gOwnerSpawned{};
std::atomic<std::uint64_t> gOwnerSuppressedCollected{}, gOwnerOverflow{}, gSpawnFaults{};
std::atomic<std::uint64_t> gUnknownPaths{}, gSpawnObservedOverflow{};
std::atomic<std::uint64_t> gDropsObserved{}, gDropsTagged{}, gDropTagFailed{};
std::atomic<std::uint64_t> gDropsSpawned{}, gDropsDuplicate{};

bool SessionActive() noexcept {
    return gEnabled.load(std::memory_order_acquire) && world_actions::SessionAuthorityBound();
}

std::uint64_t SpawnGeneration() noexcept {
    // Owner spawns run while the level loads, before the generation is stable.
    return gGenerations == nullptr ? 0U : gGenerations->CurrentGeneration();
}

std::uint64_t PackKey(const std::uint32_t key[2]) noexcept {
    return (static_cast<std::uint64_t>(key[0]) << 32U) | key[1];
}

void UnpackKey(const std::uint64_t packed, std::uint32_t key[2]) noexcept {
    key[0] = static_cast<std::uint32_t>(packed >> 32U);
    key[1] = static_cast<std::uint32_t>(packed);
}

bool ReadKey(const std::uint32_t* source, std::uint32_t key[2]) noexcept {
    __try {
        key[0] = source[0];
        key[1] = source[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ExtractEntityKey(const std::uintptr_t entity, std::uint32_t key[2]) noexcept {
    __try {
        reinterpret_cast<ExtractKeyFn>(gBase + kExtractKey)(reinterpret_cast<void*>(entity), key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies a NUL-terminated path; false when unreadable, empty or too long.
bool CopyPath(const char* source, char* output) noexcept {
    __try {
        for (std::size_t i = 0U; i < kSpawnPathCapacity; ++i) {
            output[i] = source[i];
            if (source[i] == '\0') return i != 0U;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    output[0] = '\0';
    return false;
}

bool CopyFloats(const float* source, float* output, const std::size_t count) noexcept {
    __try {
        for (std::size_t i = 0U; i < count; ++i) output[i] = source[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadItemDefPath(const std::uintptr_t itemDef, char* output) noexcept {
    const char* path{};
    if (itemDef == 0U || !Read(itemDef + kItemDefPath, path) || path == nullptr) return false;
    return CopyPath(path, output);
}

// FUN_007AFE00 writes the owner into the PM slot the spawned composite
// registers: only item definition paths ("Pickups\...") are ever spawned.
bool ItemPathKnown(const char* path) noexcept {
    __try {
        const auto root = *reinterpret_cast<const std::uintptr_t*>(gBase + kGlobalRoot);
        if (root == 0U) return false;
        const auto database = *reinterpret_cast<const std::uintptr_t*>(root + kItemDatabase);
        if (database == 0U) return false;
        auto node = *reinterpret_cast<const std::uintptr_t*>(database + 4U);
        for (std::uint32_t guard = 0U; node != 0U && guard < 8192U; ++guard) {
            const auto itemDef = *reinterpret_cast<const std::uintptr_t*>(node);
            if (itemDef != 0U) {
                const auto* itemPath = *reinterpret_cast<const char* const*>(itemDef + kItemDefPath);
                if (itemPath != nullptr && std::strcmp(itemPath, path) == 0) return true;
            }
            node = *reinterpret_cast<const std::uintptr_t*>(node + 8U);
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Engine thread. The trampoline is the retail request: our own spawns never
// reach the detour.
bool CallSpawn(const SpawnData& data) noexcept {
    const auto original = reinterpret_cast<SpawnRequestFn>(
        gSpawnOriginal.load(std::memory_order_acquire));
    const auto manager = PickupManager();
    if (original == nullptr || manager == 0U) return false;
    std::uint32_t key[2]{};
    UnpackKey(data.ownerKey, key);
    __try {
        original(reinterpret_cast<void*>(manager), data.path, data.quantity, data.transform, key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void PublishSpawn(const SpawnData& data) noexcept {
    AcquireSRWLockExclusive(&gSpawnObservedLock);
    if (gSpawnObservedCount == kSpawnObservedCapacity) {
        ReleaseSRWLockExclusive(&gSpawnObservedLock);
        gSpawnObservedOverflow.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    auto& event = gSpawnObserved[(gSpawnObservedRead + gSpawnObservedCount) % kSpawnObservedCapacity];
    event.sequence = gNextSpawnSequence++;
    event.data = data;
    ++gSpawnObservedCount;
    ReleaseSRWLockExclusive(&gSpawnObservedLock);
}

// Caller holds gStateLock.
void ResetOwnersIfStaleLocked(const std::uint64_t generation) noexcept {
    if (generation == 0U || generation == gOwnerGeneration) return;
    gOwnerCount = 0U;
    gDropTagCount = 0U;
    gOwnerGeneration = generation;
}

// Caller holds gStateLock.
OwnerState* FindOwnerLocked(const std::uint64_t key, const bool create) noexcept {
    for (std::size_t i = 0U; i < gOwnerCount; ++i)
        if (gOwners[i].key == key) return &gOwners[i];
    if (!create) return nullptr;
    if (gOwnerCount == kOwnerCapacity) {
        gOwnerOverflow.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    gOwners[gOwnerCount] = OwnerState{};
    gOwners[gOwnerCount].key = key;
    return &gOwners[gOwnerCount++];
}

// Caller holds gStateLock. A pickup already collected elsewhere is not spawned:
// consume its pending removal instead.
bool TakePendingRemovalLocked(const std::uint64_t ownerKey) noexcept {
    for (std::size_t i = 0U; i < gPendingCount; ++i) {
        ItemKey key{};
        if (!DecodeItemKey(gPending[i].entity, key)) continue;
        const std::uint32_t owner[2]{key.ownerShortGuid, key.ownerInstance};
        if (PackKey(owner) != ownerKey) continue;
        RememberApplied(gPending[i].entity, gPending[i].generation);
        gPending[i] = gPending[--gPendingCount];
        return true;
    }
    return false;
}

enum class SpawnOutcome : std::uint8_t { None, Spawned, Suppressed, UnknownPath, Fault };

// Engine thread, gStateLock NOT held: the native request must not run under
// our lock. Applies one decided owner spawn or one remote drop.
SpawnOutcome SpawnDecided(const SpawnData& decision, const bool suppressed) noexcept {
    if (!decision.spawn) return SpawnOutcome::None;
    if (suppressed) {
        gOwnerSuppressedCollected.fetch_add(1U, std::memory_order_relaxed);
        return SpawnOutcome::Suppressed;
    }
    if (!ItemPathKnown(decision.path)) {
        gUnknownPaths.fetch_add(1U, std::memory_order_relaxed);
        return SpawnOutcome::UnknownPath;
    }
    if (!CallSpawn(decision)) {
        gSpawnFaults.fetch_add(1U, std::memory_order_relaxed);
        return SpawnOutcome::Fault;
    }
    if (decision.kind == SpawnKind::Drop) gDropsSpawned.fetch_add(1U, std::memory_order_relaxed);
    else gOwnerSpawned.fetch_add(1U, std::memory_order_relaxed);
    return SpawnOutcome::Spawned;
}

// Engine thread: the local GCIP owner ran. `proposal` is its own request
// (blocked) or null when it decided not to spawn (collected, difficulty skip,
// empty pool). A Host decision is applied; otherwise the local result is
// proposed to the Host once.
void OnOwnerRan(const std::uint64_t key, const SpawnData* proposal) noexcept {
    SpawnData apply{};
    bool doApply{}, suppressed{};
    AcquireSRWLockExclusive(&gStateLock);
    ResetOwnersIfStaleLocked(SpawnGeneration());
    if (auto* owner = FindOwnerLocked(key, true); owner != nullptr) {
        owner->ran = true;
        if (owner->decided) {
            if (!owner->applied) {
                owner->applied = true;
                apply = owner->decision;
                doApply = true;
                suppressed = apply.spawn && TakePendingRemovalLocked(key);
            }
        } else if (!owner->proposed) {
            owner->proposed = true;
            SpawnData data{};
            if (proposal != nullptr) data = *proposal;
            data.kind = SpawnKind::Owner;
            data.ownerKey = key;
            data.spawn = proposal != nullptr;
            PublishSpawn(data);
            (data.spawn ? gOwnerProposals : gOwnerNoneProposals)
                .fetch_add(1U, std::memory_order_relaxed);
        }
    }
    ReleaseSRWLockExclusive(&gStateLock);
    if (doApply) SpawnDecided(apply, suppressed);
}

void __fastcall SpawnRequestDetour(void* manager, void*, const char* path, const std::int32_t quantity,
    const float* transform, const std::uint32_t* ownerKey) noexcept {
    const auto original = reinterpret_cast<SpawnRequestFn>(
        gSpawnOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    // Only GCIP is random. PickupSpawner and MultiplePickupSpawner take item,
    // quantity and transform from level parameters and stay retail.
    if (caller != gBase + kGcipSpawnReturn || !SessionActive()) {
        original(manager, path, quantity, transform, ownerKey);
        return;
    }
    std::uint32_t key[2]{};
    SpawnData data{};
    if (!ReadKey(ownerKey, key) || (key[0] == 0U && key[1] == 0U) ||
        !CopyPath(path, data.path) || !CopyFloats(transform, data.transform, 12U)) {
        original(manager, path, quantity, transform, ownerKey);
        return;
    }
    const auto packed = PackKey(key);
    data.quantity = quantity;
    if (gGcip.active && gGcip.key == packed) gGcip.sawRequest = true;
    gOwnerRequestsBlocked.fetch_add(1U, std::memory_order_relaxed);
    // Blocked: the Host decision spawns the pickup, on every peer alike.
    OnOwnerRan(packed, &data);
}

std::uint8_t __fastcall GcipSpawnDetour(void* iface, void*, void* context, void* argument) noexcept {
    const auto original = reinterpret_cast<GcipSpawnFn>(gGcipOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 1U;
    std::uint32_t key[2]{};
    if (!SessionActive() || gGcip.active || iface == nullptr ||
        !ExtractEntityKey(reinterpret_cast<std::uintptr_t>(iface) - kGcipInterface, key) ||
        (key[0] == 0U && key[1] == 0U))
        return original(iface, context, argument);
    gGcipRuns.fetch_add(1U, std::memory_order_relaxed);
    gGcip = GcipContext{true, PackKey(key), false};
    const auto result = original(iface, context, argument);
    const auto observed = gGcip;
    gGcip = GcipContext{};
    // No spawn request: collected, difficulty skip or empty pool.
    if (!observed.sawRequest) OnOwnerRan(observed.key, nullptr);
    return result;
}

// Engine thread. In-use slots with no owner (player drops) as a bitmap.
bool SnapshotOwnerlessSlots(const std::uintptr_t manager,
    std::array<std::uint32_t, kSlotCount / 32U>& bitmap) noexcept {
    __try {
        for (std::uint32_t i = 0U; i < kSlotCount; ++i) {
            const auto slot = manager + kSlotTable + i * kSlotStride;
            if ((*reinterpret_cast<const std::uint8_t*>(slot + kSlotFlags) & kSlotInUse) == 0U ||
                *reinterpret_cast<const std::uintptr_t*>(slot + kSlotRef) == 0U ||
                *reinterpret_cast<const std::uint32_t*>(slot + kSlotOwner) != 0U ||
                *reinterpret_cast<const std::uint32_t*>(slot + kSlotOwner + 4U) != 0U) continue;
            bitmap[i / 32U] |= 1U << (i % 32U);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Engine thread: give the local drop's new ownerless slot its network owner.
// The nearest new slot wins; a single new slot is accepted at any distance.
enum class TagResult : std::uint8_t { Tagged, NotYet, Fault };

TagResult TagDroppedSlot(const std::uintptr_t manager, const DropTag& tag) noexcept {
    __try {
        std::uintptr_t best{}, only{};
        float bestDistance{kDropTagRadius};
        std::uint32_t fresh{};
        for (std::uint32_t i = 0U; i < kSlotCount; ++i) {
            if ((tag.before[i / 32U] & (1U << (i % 32U))) != 0U) continue;
            const auto slot = manager + kSlotTable + i * kSlotStride;
            if ((*reinterpret_cast<const std::uint8_t*>(slot + kSlotFlags) & kSlotInUse) == 0U ||
                *reinterpret_cast<const std::uintptr_t*>(slot + kSlotRef) == 0U ||
                *reinterpret_cast<const std::uint32_t*>(slot + kSlotOwner) != 0U ||
                *reinterpret_cast<const std::uint32_t*>(slot + kSlotOwner + 4U) != 0U) continue;
            ++fresh;
            only = slot;
            const float dx = *reinterpret_cast<const float*>(slot + kSlotPosition) - tag.position[0];
            const float dy = *reinterpret_cast<const float*>(slot + kSlotPosition + 4U) - tag.position[1];
            const float dz = *reinterpret_cast<const float*>(slot + kSlotPosition + 8U) - tag.position[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (std::isfinite(distance) && distance <= bestDistance) {
                best = slot;
                bestDistance = distance;
            }
        }
        const auto target = best != 0U ? best : (fresh == 1U ? only : 0U);
        if (target == 0U) return TagResult::NotYet;
        *reinterpret_cast<std::uint32_t*>(target + kSlotOwner) = static_cast<std::uint32_t>(tag.key >> 32U);
        *reinterpret_cast<std::uint32_t*>(target + kSlotOwner + 4U) = static_cast<std::uint32_t>(tag.key);
        return TagResult::Tagged;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return TagResult::Fault;
    }
}

void __fastcall DropRequestDetour(void* manager, void*, void* itemDef, const std::int32_t quantity,
    const float* position) noexcept {
    const auto original = reinterpret_cast<DropRequestFn>(gDropOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    SpawnData data{};
    float pos[3]{};
    DropTag tag{};
    const bool replicate = SessionActive() && manager != nullptr &&
        ReadItemDefPath(reinterpret_cast<std::uintptr_t>(itemDef), data.path) &&
        CopyFloats(position, pos, 3U) &&
        SnapshotOwnerlessSlots(reinterpret_cast<std::uintptr_t>(manager), tag.before);
    // The retail drop keeps the inventory, refund and pickup flag semantics.
    original(manager, itemDef, quantity, position);
    if (!replicate) return;
    gDropsObserved.fetch_add(1U, std::memory_order_relaxed);

    AcquireSRWLockExclusive(&gStateLock);
    ResetOwnersIfStaleLocked(SpawnGeneration());
    if (gDropNonce == 0U)
        gDropNonce = (GetCurrentProcessId() ^ static_cast<std::uint32_t>(GetTickCount64())) & 0x000FFFFFU;
    const std::uint32_t key[2]{gNextDropSequence++, kDropOwnerInstanceTag | gDropNonce};
    const auto packed = PackKey(key);
    if (auto* owner = FindOwnerLocked(packed, true); owner != nullptr) {
        owner->ran = owner->decided = owner->applied = owner->proposed = true;
    }
    if (gDropTagCount < kDropTagCapacity) {
        tag.key = packed;
        tag.position[0] = pos[0];
        tag.position[1] = pos[1];
        tag.position[2] = pos[2];
        tag.deadline = GetTickCount64() + kDropTagTimeoutMs;
        gDropTags[gDropTagCount++] = tag;
    } else {
        gDropTagFailed.fetch_add(1U, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&gStateLock);

    data.kind = SpawnKind::Drop;
    data.ownerKey = packed;
    data.spawn = true;
    data.quantity = quantity;
    // FUN_007A2D50 drops with no rotation: identity basis, translation = pos.
    data.transform[0] = data.transform[5] = data.transform[10] = 1.0F;
    data.transform[3] = pos[0];
    data.transform[7] = pos[1];
    data.transform[11] = pos[2];
    PublishSpawn(data);
}

// Engine thread, every pump call: drop spawns land a few bus pumps later.
void TagPendingDrops() noexcept {
    AcquireSRWLockExclusive(&gStateLock);
    if (gDropTagCount == 0U) {
        ReleaseSRWLockExclusive(&gStateLock);
        return;
    }
    const auto manager = PickupManager();
    const auto now = GetTickCount64();
    std::size_t kept = 0U;
    for (std::size_t i = 0U; i < gDropTagCount; ++i) {
        const auto& tag = gDropTags[i];
        const auto result = manager == 0U ? TagResult::NotYet : TagDroppedSlot(manager, tag);
        if (result == TagResult::Tagged) {
            gDropsTagged.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (result == TagResult::Fault || now >= tag.deadline) {
            gDropTagFailed.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        gDropTags[kept++] = tag;
    }
    gDropTagCount = kept;
    ReleaseSRWLockExclusive(&gStateLock);
}

bool ValidSpawnData(const SpawnData& data) noexcept {
    if (data.ownerKey == 0U || (data.kind != SpawnKind::Owner && data.kind != SpawnKind::Drop))
        return false;
    if (data.kind == SpawnKind::Drop &&
        ((static_cast<std::uint32_t>(data.ownerKey) & kDropOwnerInstanceMask) != kDropOwnerInstanceTag ||
         !data.spawn))
        return false;
    if (!data.spawn) return true;
    const auto length = strnlen(data.path, kSpawnPathCapacity);
    if (length == 0U || length == kSpawnPathCapacity || data.quantity < 0 || data.quantity > 100000)
        return false;
    for (const float value : data.transform)
        if (!std::isfinite(value) || std::fabs(value) > 100000.0F) return false;
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
    // Spawn authority: FUN_007A2B90 and its GCIP call site (identified by the
    // return address), the GCIP spawn method, and the player drop request.
    std::int32_t gcipRelative{};
    if (!Match(kSpawnRequest, {0x83,0xEC,0x68,0x8B,0x44,0x24,0x6C,0x56,0x8B,0xF1}) ||
        !Match(kSpawnRequest + 0x1B3U, {0xC2,0x10,0x00}) ||
        !Match(kGcipSpawnCall, {0xE8}) ||
        !Read(gBase + kGcipSpawnCall + 1U, gcipRelative) ||
        kGcipSpawnReturn + static_cast<std::uintptr_t>(gcipRelative) != kSpawnRequest ||
        !Match(kGcipSpawn, {0xA1}) ||
        !Match(kGcipSpawn + 5U, {0x83,0xEC,0x08,0x53,0x8B,0x98,0xA0,0x00,0x00,0x00}) ||
        !Match(kGcipSpawn + 0xA0U, {0xB0,0x01}) ||
        !Match(kDropRequest, {0x81,0xEC,0xA8,0x00,0x00,0x00,0x56,0x57}) ||
        !Match(kDropRequest + 0x234U, {0xC2,0x0C,0x00})) {
        gBase = 0U;
        error = "ItemSync retail pickup spawn/drop ABI mismatch.";
        return false;
    }
    gGenerations = &generations;

    // Detours forward to retail while gEnabled is false.
    const auto install = [&](const char* name, const std::uintptr_t rva, void* detour,
        std::vector<std::uint8_t> bytes, std::string mask, void** trampoline,
        std::atomic<void*>* published) {
        HookRequest request;
        request.name = name;
        request.target = reinterpret_cast<void*>(gBase + rva);
        request.detour = detour;
        request.expected.bytes = std::move(bytes);
        request.expected.mask = std::move(mask);
        request.patchLength = request.expected.bytes.size();
        request.relocationSafePrefix = true;
        request.trampolineOut = trampoline;
        request.publishedTrampolineOut = published;
        if (hooks.Install(request, error)) return true;
        error = request.name + ": " + error;
        return false;
    };
    // mov eax,[esp+4] ; mov eax,[eax]  (6 bytes, no relative operand)
    if (!install("itemsync_pickup_collection_request", kRequestRemoval,
            reinterpret_cast<void*>(&RequestRemovalDetour),
            {0x8BU,0x44U,0x24U,0x04U,0x8BU,0x00U}, "xxxxxx",
            &gRequestRemovalTrampoline, &gRequestRemovalOriginal) ||
        // sub esp,68 ; mov eax,[esp+6C]  (7 bytes)
        !install("itemsync_pickup_spawn_request", kSpawnRequest,
            reinterpret_cast<void*>(&SpawnRequestDetour),
            {0x83U,0xECU,0x68U,0x8BU,0x44U,0x24U,0x6CU}, "xxxxxxx",
            &gSpawnTrampoline, &gSpawnOriginal) ||
        // mov eax,[GlobalRoot]  (5 bytes, absolute operand already relocated)
        !install("itemsync_gcip_spawn", kGcipSpawn,
            reinterpret_cast<void*>(&GcipSpawnDetour),
            {0xA1U,0x00U,0x00U,0x00U,0x00U}, "x????",
            &gGcipTrampoline, &gGcipOriginal) ||
        // sub esp,A8  (6 bytes)
        !install("itemsync_player_drop_request", kDropRequest,
            reinterpret_cast<void*>(&DropRequestDetour),
            {0x81U,0xECU,0xA8U,0x00U,0x00U,0x00U}, "xxxxxx",
            &gDropTrampoline, &gDropOriginal))
        return false;
    gEnabled.store(true, std::memory_order_release);
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

bool TryPeekObservedSpawn(SpawnEvent& event) noexcept {
    AcquireSRWLockShared(&gSpawnObservedLock);
    const bool available = gSpawnObservedCount != 0U;
    if (available) event = gSpawnObserved[gSpawnObservedRead];
    ReleaseSRWLockShared(&gSpawnObservedLock);
    return available;
}

void PopObservedSpawn() noexcept {
    AcquireSRWLockExclusive(&gSpawnObservedLock);
    if (gSpawnObservedCount != 0U) {
        gSpawnObservedRead = (gSpawnObservedRead + 1U) % kSpawnObservedCapacity;
        --gSpawnObservedCount;
    }
    ReleaseSRWLockExclusive(&gSpawnObservedLock);
}

ApplyStatus ApplySpawnDecision(const SpawnData& decision) noexcept {
    if (!ValidSpawnData(decision)) {
        gRejected.fetch_add(1U, std::memory_order_relaxed);
        return ApplyStatus::Rejected;
    }
    if (!gEnabled.load(std::memory_order_acquire)) return ApplyStatus::Unavailable;
    if (decision.kind == SpawnKind::Owner) gOwnerDecisions.fetch_add(1U, std::memory_order_relaxed);

    SpawnData apply{};
    bool doApply{}, suppressed{};
    ApplyStatus status{ApplyStatus::Deferred};
    AcquireSRWLockExclusive(&gStateLock);
    ResetOwnersIfStaleLocked(SpawnGeneration());
    auto* owner = FindOwnerLocked(decision.ownerKey, true);
    if (owner == nullptr) {
        status = ApplyStatus::Rejected;
    } else if (decision.kind == SpawnKind::Drop) {
        if (owner->applied) {
            status = ApplyStatus::AlreadyApplied;
            gDropsDuplicate.fetch_add(1U, std::memory_order_relaxed);
        } else {
            owner->ran = owner->proposed = owner->decided = owner->applied = true;
            owner->decision = decision;
            apply = decision;
            doApply = true;
            suppressed = TakePendingRemovalLocked(decision.ownerKey);
        }
    } else if (owner->decided) {
        status = ApplyStatus::AlreadyApplied;
    } else {
        owner->decided = true;
        owner->decision = decision;
        // Applied now only if the local owner already ran; otherwise when it runs.
        if (owner->ran && !owner->applied) {
            owner->applied = true;
            apply = decision;
            doApply = true;
            suppressed = decision.spawn && TakePendingRemovalLocked(decision.ownerKey);
        }
    }
    ReleaseSRWLockExclusive(&gStateLock);

    if (doApply) {
        switch (SpawnDecided(apply, suppressed)) {
        case SpawnOutcome::Fault: status = ApplyStatus::Fault; break;
        case SpawnOutcome::UnknownPath: status = ApplyStatus::Rejected; break;
        default: status = ApplyStatus::Applied; break;
        }
    }
    return status;
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
    TagPendingDrops();
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
         {"lastRemoteOwner", Diagnostics::Unsigned(gLastRemote.load()), true},
         {"gcipRuns", Diagnostics::Unsigned(gGcipRuns.load()), true},
         {"ownerRequestsBlocked", Diagnostics::Unsigned(gOwnerRequestsBlocked.load()), true},
         {"ownerProposals", Diagnostics::Unsigned(gOwnerProposals.load()), true},
         {"ownerNoneProposals", Diagnostics::Unsigned(gOwnerNoneProposals.load()), true},
         {"ownerDecisions", Diagnostics::Unsigned(gOwnerDecisions.load()), true},
         {"ownerSpawned", Diagnostics::Unsigned(gOwnerSpawned.load()), true},
         {"ownerSuppressedCollected", Diagnostics::Unsigned(gOwnerSuppressedCollected.load()), true},
         {"ownerOverflow", Diagnostics::Unsigned(gOwnerOverflow.load()), true},
         {"spawnFaults", Diagnostics::Unsigned(gSpawnFaults.load()), true},
         {"unknownPaths", Diagnostics::Unsigned(gUnknownPaths.load()), true},
         {"spawnObservedOverflow", Diagnostics::Unsigned(gSpawnObservedOverflow.load()), true},
         {"dropsObserved", Diagnostics::Unsigned(gDropsObserved.load()), true},
         {"dropsTagged", Diagnostics::Unsigned(gDropsTagged.load()), true},
         {"dropTagFailed", Diagnostics::Unsigned(gDropTagFailed.load()), true},
         {"dropsSpawned", Diagnostics::Unsigned(gDropsSpawned.load()), true},
         {"dropsDuplicate", Diagnostics::Unsigned(gDropsDuplicate.load()), true}});
}

} // namespace aimp::item_sync
