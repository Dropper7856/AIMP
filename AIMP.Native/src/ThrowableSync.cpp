#include "aimp/ThrowableSync.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/WorldPublicActions.h"
#include "aimp/Diagnostics.h"
#include "aimp/GameThreadDispatcher.h"
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>

namespace aimp::throwable_sync {
namespace {
// AI.exe 00d6bb84..., image base 00400000. Static ABI/ASM exports:
// analysis/throwables/native_contract.md. These are the real released composite
// factories, including the inventory discard/flare path, not equip slot 9.
constexpr std::uintptr_t kManager = 0x012F0DE4U;
constexpr std::uintptr_t kThrow = 0x003B5050U, kPlace = 0x003B5190U;
// The final stack argument is overwritten with a temporary shared reference by
// FUN_005079C0. It is NOT a remaining lifetime float (FUN_007B5050/007B5190).
using ThrowFn = void(__thiscall*)(void*, const float*, const float*, float, const char*, std::uintptr_t);
using PlaceFn = void(__thiscall*)(void*, const float*, const char*, std::uintptr_t);
using KeyFn = void(__thiscall*)(void*, std::uint32_t*);
using RemoveFn = void(__thiscall*)(void*, std::uint32_t, std::uint32_t);
using InventoryActionFn = void(__thiscall*)(void*);
using InventoryDiscardFn = void(__thiscall*)(void*, void*);
std::uintptr_t gBase{};
LevelGenerationTracker* gGenerations{};
GameThreadDispatcher* gDispatcher{};
std::atomic<bool> gInstalled{}, gEnabled{};
void* gThrowTrampoline{}, *gPlaceTrampoline{};
std::atomic<void*> gThrowOriginal{}, gPlaceOriginal{};
void* gInventoryThrowTrampoline{}, *gInventoryPlaceTrampoline{}, *gInventoryDiscardTrampoline{};
std::atomic<void*> gInventoryThrowOriginal{}, gInventoryPlaceOriginal{}, gInventoryDiscardOriginal{};
struct ReleaseContext final {
    std::uintptr_t inventory{}, definition{};
    std::uint32_t kind{};
    std::uint64_t epoch{};
};
thread_local ReleaseContext gReleaseContext{};
std::mutex gMailbox;
Configuration gDesired{}, gCurrent{};
struct Pending final { Event event{}; std::uint64_t epoch{}, deadline{}, nextAttempt{}; };
std::array<Pending, 128U> gIncoming{};
std::array<Event, 128U> gOutgoing{};
std::array<std::uint64_t, 128U> gOutgoingTickets{};
std::uint64_t gNextTicket{};
std::size_t gInRead{}, gInCount{}, gOutRead{}, gOutCount{};
std::uint64_t gNextSequence{};
std::uint64_t gDesiredEpoch{}, gCurrentEpoch{};
std::atomic<std::uint64_t> gExpired{}, gNativeFaults{}, gUnknownPath{}, gTracked{}, gRetried{};
thread_local bool gReplaying{};
bool gPumping{};
struct Live final {
    Event event{};
    std::uintptr_t manager{}, slot{}, ref{};
    std::uint32_t key[2]{};
    bool retiring{}, publishPending{};
};
std::array<Live, 128U> gLive{};
struct Observation final {
    Event event{};
    std::uintptr_t manager{}, slot{}, ref{};
    std::uint32_t key[2]{};
    std::uint64_t epoch{};
};
std::array<Observation, 128U> gObservations{};
std::size_t gObservationRead{}, gObservationCount{};
std::atomic<std::uint64_t> gLocalReleases{}, gCaptureRejected{}, gObservationOverflow{};
struct Watermark final { std::uint64_t peer{}, sequence{}; };
std::array<Watermark, 32U> gWatermarks{};

template<typename T> bool Read(std::uintptr_t address, T& value) noexcept {
    __try { value = *reinterpret_cast<const T*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Stable() noexcept {
    return gDispatcher != nullptr && gDispatcher->IsCertifiedEngineThread(GetCurrentThreadId()) &&
        gEnabled.load(std::memory_order_acquire) && gCurrent.enabled != 0U &&
        gGenerations != nullptr && gGenerations->IsStableGeneration(gCurrent.nativeGeneration) &&
        world_actions::SessionAuthorityBound() && world_actions::WorldBusActive() &&
        world_actions::SessionAuthorityPeer() == gCurrent.localPeer;
}
bool CaptureReady() noexcept {
    if (gReplaying || !gEnabled.load(std::memory_order_acquire) ||
        !world_actions::SessionAuthorityBound() || !world_actions::WorldBusActive()) return false;
    std::lock_guard lock(gMailbox);
    return gDesired.enabled != 0U && gGenerations != nullptr &&
        gGenerations->IsStableGeneration(gDesired.nativeGeneration) &&
        world_actions::SessionAuthorityPeer() == gDesired.localPeer;
}
bool SameContext(const Event& event) noexcept {
    return event.sessionCookie == gCurrent.sessionCookie && event.worldGeneration == gCurrent.worldGeneration;
}
bool LocalInventory(std::uintptr_t inventory) noexcept {
    __try {
        const auto root = *reinterpret_cast<const std::uintptr_t*>(gBase + 0x012F0C88U);
        if (root == 0U) return false;
        const auto manager = *reinterpret_cast<const std::uintptr_t*>(root + 8U);
        if (manager == 0U || *reinterpret_cast<const std::uint32_t*>(manager + 0x84U) == 0U) return false;
        const auto list = *reinterpret_cast<const std::uintptr_t*>(manager + 0x80U);
        if (list == 0U) return false;
        const auto player = *reinterpret_cast<const std::uintptr_t*>(list);
        if (player == 0U) return false;
        const auto component = *reinterpret_cast<const std::uintptr_t*>(player + 0x31CU);
        return component != 0U &&
            *reinterpret_cast<const std::uintptr_t*>(component + 0x3CU) == inventory;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
ReleaseContext InventoryContext(void* inventory, void* definition, std::uint32_t kind) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(inventory);
    if (!CaptureReady() || !LocalInventory(address)) return {};
    gLocalReleases.fetch_add(1U, std::memory_order_relaxed);
    std::uintptr_t selected = reinterpret_cast<std::uintptr_t>(definition);
    if (kind != 3U && !Read(address + 0x3BC8U, selected)) return {};
    std::lock_guard lock(gMailbox);
    return selected == 0U ? ReleaseContext{} : ReleaseContext{address, selected, kind, gDesiredEpoch};
}
struct ReleaseScope final {
    ReleaseContext previous{gReleaseContext};
    explicit ReleaseScope(ReleaseContext context) noexcept { gReleaseContext = context; }
    ~ReleaseScope() { gReleaseContext = previous; }
};
void __fastcall InventoryThrowDetour(void* inventory, void*) {
    const ReleaseScope scope(InventoryContext(inventory, nullptr, 1U));
    const auto original = reinterpret_cast<InventoryActionFn>(gInventoryThrowOriginal.load(std::memory_order_acquire));
    if (original != nullptr) original(inventory);
}
void __fastcall InventoryPlaceDetour(void* inventory, void*) {
    const ReleaseScope scope(InventoryContext(inventory, nullptr, 2U));
    const auto original = reinterpret_cast<InventoryActionFn>(gInventoryPlaceOriginal.load(std::memory_order_acquire));
    if (original != nullptr) original(inventory);
}
void __fastcall InventoryDiscardDetour(void* inventory, void*, void* definition) {
    const ReleaseScope scope(InventoryContext(inventory, definition, 3U));
    const auto original = reinterpret_cast<InventoryDiscardFn>(gInventoryDiscardOriginal.load(std::memory_order_acquire));
    if (original != nullptr) original(inventory, definition);
}
bool LocalReleasePath(const char* path, std::uintptr_t caller, bool placement) noexcept {
    if (gReleaseContext.inventory == 0U || !LocalInventory(gReleaseContext.inventory)) return false;
    if (!placement && (gReleaseContext.kind != 1U || caller != gBase + 0x003B95CFU)) return false;
    if (placement && !((gReleaseContext.kind == 2U && caller == gBase + 0x003D4A72U) ||
        (gReleaseContext.kind == 3U && caller == gBase + 0x003C7FDEU))) return false;
    __try {
        const auto released = *reinterpret_cast<const char* const*>(gReleaseContext.definition + 0x58U);
        return released != nullptr && std::strcmp(path, released) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CopyRelease(Event& event, const float* transform, const float* velocity,
    const char* path, bool placement) noexcept {
    __try {
        event.placement = placement ? 1U : 0U;
        event.timeLeft = 0.0F; // reserved legacy carrier field; retail owns lifetime
        std::memcpy(event.transform, transform, sizeof(event.transform));
        if (!placement) std::memcpy(event.velocity, velocity, sizeof(event.velocity));
        for (std::size_t i = 0U; i < sizeof(event.path); ++i) {
            event.path[i] = path[i];
            if (path[i] == '\0') return i != 0U;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
bool Valid(const Event& event) noexcept {
    if (event.version != 1U || event.nativeGeneration == 0U || event.worldGeneration == 0U ||
        event.sessionCookie == 0U || event.peer == 0U || event.sequence == 0U ||
        (event.operation != 1U && event.operation != 2U) || event.placement > 1U || event.reserved != 0U)
        return false;
    bool nul{};
    for (const char c : event.path) {
        if (c == '\0') nul = true;
        else if (nul || static_cast<unsigned char>(c) < 32U || static_cast<unsigned char>(c) > 126U) return false;
    }
    if (!nul || event.path[0] == '\0' || !std::isfinite(event.timeLeft) ||
        event.timeLeft < 0.0F || event.timeLeft > 100000.0F) return false;
    for (const float v : event.transform) if (!std::isfinite(v) || std::fabs(v) > 100000.0F) return false;
    for (const float v : event.velocity) if (!std::isfinite(v) || std::fabs(v) > 1000.0F) return false;
    return true;
}

// Native item definitions live in the same database used by retail inventory.
// Match the released +58 path against that definition, never a caller-provided
// arbitrary CATHODE graph. The initial scope deliberately admits only the
// non-explosive stimulus items; damage-producing archetypes need certified
// instance provenance before sharing Host projectile authority with AISync.
bool AdmittedPath(const char* path) noexcept {
    __try {
        const auto root = *reinterpret_cast<const std::uintptr_t*>(gBase + 0x012F0C88U);
        if (root == 0U) return false;
        const auto database = *reinterpret_cast<const std::uintptr_t*>(root + 0x40U);
        if (database == 0U) return false;
        auto node = *reinterpret_cast<const std::uintptr_t*>(database + 4U);
        for (std::uint32_t guard = 0U; node != 0U && guard < 8192U; ++guard) {
            const auto definition = *reinterpret_cast<const std::uintptr_t*>(node);
            if (definition != 0U) {
                const auto name = *reinterpret_cast<const char* const*>(definition + 8U);
                if (name != nullptr && (std::strcmp(name, "flare") == 0 ||
                    std::strcmp(name, "noisemaker") == 0 || std::strcmp(name, "smoke_bomb") == 0)) {
                    const auto released = *reinterpret_cast<const char* const*>(definition + 0x58U);
                    if (released != nullptr && std::strcmp(released, path) == 0) return true;
                }
            }
            node = *reinterpret_cast<const std::uintptr_t*>(node + 8U);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
std::uintptr_t FirstFree(std::uintptr_t manager) noexcept {
    for (std::size_t i = 0U; manager != 0U && i < 128U; ++i) {
        const auto slot = manager + 4U + i * 12U;
        std::uint8_t active{};
        if (!Read(slot, active)) return 0U;
        if (active == 0U) return slot;
    }
    return 0U;
}
bool NativeKey(std::uintptr_t ref, std::uint32_t key[2]) noexcept {
    __try {
        const auto object = *reinterpret_cast<const std::uintptr_t*>(ref + 12U);
        if (object == 0U) return false;
        reinterpret_cast<KeyFn>(gBase + 0x004C3360U)(reinterpret_cast<void*>(object), key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Alive(const Live& live) noexcept {
    std::uintptr_t manager{}, ref{};
    std::uint8_t active{};
    std::uint32_t key[2]{};
    return live.ref != 0U && Read(gBase + kManager, manager) && manager == live.manager &&
        Read(live.slot, active) && active != 0U && Read(live.slot + 4U, ref) && ref == live.ref &&
        NativeKey(ref, key) && key[0] == live.key[0] && key[1] == live.key[1];
}
bool Remove(const Live& live) noexcept {
    if (!Alive(live)) return true;
    __try {
        const auto object = *reinterpret_cast<const std::uintptr_t*>(live.ref + 12U);
        if (object == 0U) return false;
        std::uint32_t key[2]{};
        reinterpret_cast<KeyFn>(gBase + 0x004C3360U)(reinterpret_cast<void*>(object), key);
        // Exact retail manager removal: clears in-use, marks composite deleted,
        // releases its shared ref and clears slot+8. No raw free/global clear.
        reinterpret_cast<RemoveFn>(gBase + 0x003A10F0U)(reinterpret_cast<void*>(live.manager), key[0], key[1]);
        return !Alive(live);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Publish(const Event& event) noexcept {
    for (const auto& live : gLive)
        if (live.publishPending && live.event.peer == event.peer && live.event.sequence < event.sequence)
            return false;
    std::lock_guard lock(gMailbox);
    if (gOutCount == gOutgoing.size()) return false;
    const auto slot = (gOutRead + gOutCount++) % gOutgoing.size();
    gOutgoing[slot] = event; gOutgoingTickets[slot] = ++gNextTicket;
    return true;
}
Live* Find(std::uint64_t peer, std::uint64_t sequence) noexcept {
    for (auto& live : gLive)
        if (live.ref != 0U && live.event.peer == peer && live.event.sequence == sequence) return &live;
    return nullptr;
}
bool Fresh(const Event& event, bool commit = false) noexcept {
    Watermark* free{};
    for (auto& mark : gWatermarks) {
        if (mark.peer == event.peer) {
            if (event.sequence <= mark.sequence) return false;
            if (commit) mark.sequence = event.sequence;
            return true;
        }
        if (mark.peer == 0U && free == nullptr) free = &mark;
    }
    if (free == nullptr) return false;
    if (commit) *free = {event.peer, event.sequence};
    return true;
}
Live* AvailableLive() noexcept {
    for (auto& live : gLive) if (live.ref == 0U) return &live;
    return nullptr;
}
bool Track(Event event, std::uintptr_t manager, std::uintptr_t slot, bool publish, Live* reserved = nullptr) noexcept {
    std::uintptr_t ref{};
    std::uint8_t active{};
    std::uint32_t key[2]{};
    if (slot == 0U || !Read(slot, active) || active == 0U || !Read(slot + 4U, ref) || ref == 0U ||
        !NativeKey(ref, key)) return false;
    auto* entry = reserved == nullptr ? AvailableLive() : reserved;
    if (entry != nullptr) {
        auto& live = *entry;
        event.nativeGeneration = gCurrent.nativeGeneration;
        live = {event, manager, slot, ref, {key[0], key[1]}, false, false};
        static_cast<void>(Fresh(event, true)); // native success is proven before consuming sequence
        live.publishPending = publish && !Publish(event);
        gTracked.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    return false;
}
void Observe(std::uintptr_t manager, std::uintptr_t slot, Event& event) noexcept {
    // Character behaviours can release inventory on a worker. Never mutate
    // engine-owned tracking/sequence state there; retain the exact slot identity
    // and consume it on the certified engine callback.
    Observation observation{};
    std::uint8_t active{};
    if (slot == 0U || !Read(slot, active) || active == 0U ||
        !Read(slot + 4U, observation.ref) || observation.ref == 0U ||
        !NativeKey(observation.ref, observation.key)) {
        gCaptureRejected.fetch_add(1U, std::memory_order_relaxed); return;
    }
    std::lock_guard lock(gMailbox);
    if (gDesired.enabled == 0U || gReleaseContext.epoch != gDesiredEpoch) return;
    event.nativeGeneration = gDesired.nativeGeneration;
    event.worldGeneration = gDesired.worldGeneration;
    event.sessionCookie = gDesired.sessionCookie;
    event.peer = gDesired.localPeer;
    observation.event = event; observation.manager = manager; observation.slot = slot;
    observation.epoch = gDesiredEpoch;
    if (gObservationCount == gObservations.size()) {
        gObservationOverflow.fetch_add(1U, std::memory_order_relaxed); return;
    }
    gObservations[(gObservationRead + gObservationCount++) % gObservations.size()] = observation;
}
void __fastcall ThrowDetour(void* manager, void*, const float* transform, const float* velocity,
    float unused, const char* path, std::uintptr_t unusedReference) {
    const auto original = reinterpret_cast<ThrowFn>(gThrowOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    Event event{};
    const bool capture = CaptureReady() &&
        CopyRelease(event, transform, velocity, path, false) &&
        LocalReleasePath(event.path, reinterpret_cast<std::uintptr_t>(_ReturnAddress()), false) && AdmittedPath(event.path);
    const auto slot = capture ? FirstFree(reinterpret_cast<std::uintptr_t>(manager)) : 0U;
    original(manager, transform, velocity, unused, path, unusedReference);
    if (capture) Observe(reinterpret_cast<std::uintptr_t>(manager), slot, event);
}
void __fastcall PlaceDetour(void* manager, void*, const float* transform, const char* path, std::uintptr_t unusedReference) {
    const auto original = reinterpret_cast<PlaceFn>(gPlaceOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    Event event{};
    const bool capture = CaptureReady() &&
        CopyRelease(event, transform, nullptr, path, true) &&
        LocalReleasePath(event.path, reinterpret_cast<std::uintptr_t>(_ReturnAddress()), true) && AdmittedPath(event.path);
    const auto slot = capture ? FirstFree(reinterpret_cast<std::uintptr_t>(manager)) : 0U;
    original(manager, transform, path, unusedReference);
    if (capture) Observe(reinterpret_cast<std::uintptr_t>(manager), slot, event);
}
bool Spawn(const Event& event, std::uintptr_t manager) noexcept {
    __try {
        if (event.placement != 0U)
            reinterpret_cast<PlaceFn>(gPlaceOriginal.load(std::memory_order_acquire))(
                reinterpret_cast<void*>(manager), event.transform, event.path, 0U);
        else
            reinterpret_cast<ThrowFn>(gThrowOriginal.load(std::memory_order_acquire))(
                reinterpret_cast<void*>(manager), event.transform, event.velocity, 0.0F, event.path, 0U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SamePending(const Pending& pending) noexcept {
    if (gInCount == 0U || pending.epoch != gDesiredEpoch) return false;
    const auto& head = gIncoming[gInRead];
    return head.epoch == pending.epoch && head.event.peer == pending.event.peer &&
        head.event.sequence == pending.event.sequence && head.event.operation == pending.event.operation;
}
void PopIncoming(const Pending& pending) noexcept {
    std::lock_guard lock(gMailbox);
    if (SamePending(pending)) { gInRead = (gInRead + 1U) % gIncoming.size(); --gInCount; }
}
void Retry(const Pending& pending) noexcept {
    const auto now = GetTickCount64();
    std::lock_guard lock(gMailbox);
    if (!SamePending(pending)) return;
    if (now >= pending.deadline) {
        gInRead = (gInRead + 1U) % gIncoming.size(); --gInCount;
        gExpired.fetch_add(1U, std::memory_order_relaxed);
    } else {
        gIncoming[gInRead].nextAttempt = now + 100U;
        gRetried.fetch_add(1U, std::memory_order_relaxed);
    }
}
}

bool Install(std::uintptr_t base, HookManager& hooks, LevelGenerationTracker& generations,
    GameThreadDispatcher& dispatcher, std::string& error) {
    gBase = base; gGenerations = &generations; gDispatcher = &dispatcher;
    const auto install = [&](const char* name, std::uintptr_t rva, void* detour, void** trampoline,
        std::atomic<void*>* published) {
        // Complete MOV EAX,FS:[0] is six bytes, with no relative operand.
        HookRequest request;
        request.name = name; request.target = reinterpret_cast<void*>(base + rva); request.detour = detour;
        request.patchLength = 6U; request.expected.bytes = {0x64,0xA1,0x00,0x00,0x00,0x00};
        request.expected.mask = "xxxxxx"; request.relocationSafePrefix = true;
        request.trampolineOut = trampoline; request.publishedTrampolineOut = published;
        return hooks.Install(request, error);
    };
    const auto checkpoint = hooks.InstalledCount();
    const auto inventoryHook = [&](const char* name, std::uintptr_t rva, void* detour,
        std::initializer_list<std::uint8_t> bytes, void** trampoline, std::atomic<void*>* published) {
        HookRequest request;
        request.name = name; request.target = reinterpret_cast<void*>(base + rva); request.detour = detour;
        request.expected.bytes.assign(bytes.begin(), bytes.end());
        request.patchLength = bytes.size(); request.expected.mask.assign(bytes.size(), 'x');
        request.relocationSafePrefix = true; request.trampolineOut = trampoline;
        request.publishedTrampolineOut = published;
        return hooks.Install(request, error);
    };
    if (!install("throwable_release", kThrow, reinterpret_cast<void*>(&ThrowDetour), &gThrowTrampoline, &gThrowOriginal) ||
        !install("throwable_place", kPlace, reinterpret_cast<void*>(&PlaceDetour), &gPlaceTrampoline, &gPlaceOriginal) ||
        !inventoryHook("throwable_local_inventory_release", 0x003D4830U, reinterpret_cast<void*>(&InventoryThrowDetour),
            {0x83,0xEC,0x74,0x53,0x56}, &gInventoryThrowTrampoline, &gInventoryThrowOriginal) ||
        !inventoryHook("throwable_local_inventory_place", 0x003D4A00U, reinterpret_cast<void*>(&InventoryPlaceDetour),
            {0x83,0xEC,0x34,0x53,0x56}, &gInventoryPlaceTrampoline, &gInventoryPlaceOriginal) ||
        !inventoryHook("throwable_local_inventory_discard", 0x003C7EF0U, reinterpret_cast<void*>(&InventoryDiscardDetour),
            {0x83,0xEC,0x3C,0x53,0x8B,0x5C,0x24,0x44}, &gInventoryDiscardTrampoline, &gInventoryDiscardOriginal)) {
        std::string rollback; static_cast<void>(hooks.UninstallFrom(checkpoint, rollback)); return false;
    }
    gInstalled.store(true, std::memory_order_release); return true;
}
bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }
bool Configure(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != sizeof(Configuration)) return false;
    Configuration config{}; std::memcpy(&config, bytes.data(), sizeof(config));
    if (config.version != 1U || config.enabled > 1U || (config.enabled != 0U &&
        (config.nativeGeneration == 0U || config.worldGeneration == 0U || config.sessionCookie == 0U || config.localPeer == 0U))) return false;
    std::lock_guard lock(gMailbox);
    if (std::memcmp(&gDesired, &config, sizeof(config)) != 0) {
        ++gDesiredEpoch; gInRead = gInCount = gOutRead = gOutCount = 0U;
        gObservationRead = gObservationCount = 0U;
    }
    gDesired = config; gEnabled.store(config.enabled != 0U, std::memory_order_release); return true;
}
bool QueueRemote(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != sizeof(Event)) return false;
    Event event{}; std::memcpy(&event, bytes.data(), sizeof(event));
    if (!Valid(event)) return false;
    std::lock_guard lock(gMailbox);
    if (gDesired.enabled == 0U || event.sessionCookie != gDesired.sessionCookie ||
        event.worldGeneration != gDesired.worldGeneration || gInCount == gIncoming.size()) return false;
    gIncoming[(gInRead + gInCount++) % gIncoming.size()] = {event, gDesiredEpoch, GetTickCount64() + 5000U, 0U};
    return true;
}
bool TryPeekObserved(Event& event, std::uint64_t& ticket) noexcept {
    std::lock_guard lock(gMailbox);
    if (gOutCount == 0U) return false;
    event = gOutgoing[gOutRead]; ticket = gOutgoingTickets[gOutRead]; return true;
}
void PopObserved(std::uint64_t ticket) noexcept {
    std::lock_guard lock(gMailbox);
    if (gOutCount != 0U && gOutgoingTickets[gOutRead] == ticket) {
        gOutRead = (gOutRead + 1U) % gOutgoing.size(); --gOutCount;
    }
}
void PumpOnEngineThread() noexcept {
    if (gDispatcher == nullptr || !gDispatcher->IsCertifiedEngineThread(GetCurrentThreadId()) || gPumping) return;
    gPumping = true;
    struct PumpGuard final { ~PumpGuard() { gPumping = false; } } guard;
    Configuration desired{};
    std::uint64_t epoch{};
    { std::lock_guard lock(gMailbox); desired = gDesired; epoch = gDesiredEpoch; }
    if (epoch != gCurrentEpoch) {
        // Old generation pointers are never dereferenced after transition.
        const bool oldStable = gGenerations != nullptr && gGenerations->IsStableGeneration(gCurrent.nativeGeneration);
        gReplaying = true;
        for (auto& live : gLive) {
            if (live.ref == 0U) continue;
            if (oldStable && !Remove(live)) { live.retiring = true; live.publishPending = false; }
            else live = {};
        }
        gReplaying = false;
        gWatermarks = {}; gNextSequence = 0U; gCurrent = desired; gCurrentEpoch = epoch;
        std::lock_guard lock(gMailbox); gOutRead = gOutCount = 0U;
    }
    // Failed native removals keep their exact identity for engine-thread retry.
    // A generation transition retires the token without touching old addresses.
    for (auto& live : gLive) {
        if (!live.retiring) continue;
        if (gGenerations == nullptr || !gGenerations->IsStableGeneration(live.event.nativeGeneration) || Remove(live))
            live = {};
    }
    if (!Stable()) return;
    const bool host = world_actions::SessionAuthorityHost();
    for (std::size_t n = 0U; n < gObservations.size(); ++n) {
        Observation observation{};
        { std::lock_guard lock(gMailbox);
          if (gObservationCount == 0U) break;
          observation = gObservations[gObservationRead];
          gObservationRead = (gObservationRead + 1U) % gObservations.size(); --gObservationCount; }
        if (observation.epoch != gCurrentEpoch || !SameContext(observation.event)) continue;
        auto event = observation.event;
        event.sequence = ++gNextSequence;
        const Live identity{event, observation.manager, observation.slot, observation.ref,
            {observation.key[0], observation.key[1]}, false, false};
        if (!Valid(event) || !Alive(identity) || !Fresh(event) ||
            !Track(event, observation.manager, observation.slot, true))
            gCaptureRejected.fetch_add(1U, std::memory_order_relaxed);
    }
    for (auto& live : gLive) {
        if (live.ref == 0U || live.retiring) continue;
        if (live.publishPending) { if (!Publish(live.event)) continue; live.publishPending = false; }
        if (Alive(live)) continue;
        if (host) { auto ended = live.event; ended.operation = 2U; if (!Publish(ended)) continue; }
        live = {};
    }
    for (std::size_t n = 0U; n < 32U; ++n) {
        Pending pending{};
        { std::lock_guard lock(gMailbox);
          if (gInCount == 0U) break;
          pending = gIncoming[gInRead]; }
        const auto& event = pending.event;
        if (pending.epoch != gCurrentEpoch || !SameContext(event)) { PopIncoming(pending); break; }
        // Keep the original head until admission succeeds or expires. Later
        // release/end carriers cannot overtake a blocked native spawn.
        if (pending.nextAttempt > GetTickCount64()) break;
        if (!AdmittedPath(event.path)) {
            gUnknownPath.fetch_add(1U, std::memory_order_relaxed); PopIncoming(pending); continue;
        }
        auto* live = Find(event.peer, event.sequence);
        if (event.operation == 2U) {
            if (host) { PopIncoming(pending); continue; } // A Client cannot terminate another native item.
            if (live != nullptr) {
                if (Remove(*live)) *live = {};
                else live->retiring = true;
            }
            PopIncoming(pending); continue;
        }
        if (live != nullptr || !Fresh(event)) { PopIncoming(pending); continue; } // includes source's own Host echo
        // Reserve tracking space BEFORE invoking a void retail factory. A full
        // tracker or manager is transient and cannot consume the release.
        auto* reserved = AvailableLive();
        std::uintptr_t manager{}, slot{};
        if (reserved == nullptr || !Read(gBase + kManager, manager) || manager == 0U ||
            (slot = FirstFree(manager)) == 0U) { Retry(pending); break; }
        gReplaying = true;
        const bool spawned = Spawn(event, manager);
        gReplaying = false;
        if (!spawned) {
            // A fault may have occurred after partial retail side effects.
            // Never retry that ambiguous void call and create a second item.
            gNativeFaults.fetch_add(1U, std::memory_order_relaxed); PopIncoming(pending); continue;
        }
        if (!Track(event, manager, slot, host, reserved))
            gNativeFaults.fetch_add(1U, std::memory_order_relaxed);
        PopIncoming(pending);
    }
}
void TransportLost() noexcept {
    gEnabled.store(false, std::memory_order_release);
    std::lock_guard lock(gMailbox); gDesired.enabled = 0U; ++gDesiredEpoch;
    gInRead = gInCount = gOutRead = gOutCount = 0U;
    gObservationRead = gObservationCount = 0U;
}
void Shutdown() noexcept { TransportLost(); }
void LogDiagnostics(Diagnostics& diagnostics) {
    diagnostics.Log("info", "throwables", "Native stimulus releases and Host lifetime; damaging archetypes are excluded pending exact impact provenance.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"tracked", Diagnostics::Unsigned(gTracked.load()), true},
         {"localReleases", Diagnostics::Unsigned(gLocalReleases.load()), true},
         {"captureRejected", Diagnostics::Unsigned(gCaptureRejected.load()), true},
         {"observationOverflow", Diagnostics::Unsigned(gObservationOverflow.load()), true},
         {"retried", Diagnostics::Unsigned(gRetried.load()), true},
         {"expired", Diagnostics::Unsigned(gExpired.load()), true},
         {"nativeFaults", Diagnostics::Unsigned(gNativeFaults.load()), true},
         {"unknownPath", Diagnostics::Unsigned(gUnknownPath.load()), true}});
}
}
