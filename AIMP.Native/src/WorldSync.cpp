#include "aimp/WorldSync.h"

#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace aimp::world_sync {
namespace {

// v11.4 deliberately avoids the global CATHODE constructor and signal
// dispatcher hooks used by v11.3. Every hook below is specific to native Door
// or LogicSwitch code reached by Door_Package.
constexpr std::uintptr_t kEntityHandleRva = 0x004C3360U;
constexpr std::uintptr_t kDoorGeneratedConstructorRva = 0x000A3D00U;
constexpr std::uintptr_t kLogicSwitchGeneratedConstructorRva = 0x001B4BB0U;

constexpr std::uintptr_t kStartedOpeningRva = 0x004CF7C0U;
constexpr std::uintptr_t kStartedClosingRva = 0x004CF7F0U;
constexpr std::uintptr_t kFinishedOpeningRva = 0x004CF820U;
constexpr std::uintptr_t kFinishedClosingRva = 0x004CF850U;
constexpr std::uintptr_t kUsedLockedRva = 0x004CF880U;
constexpr std::uintptr_t kUsedUnlockedRva = 0x004CF8B0U;

// Static CATHODE dispatch at preferred VA 0x009159C0 maps the named Door
// inputs to these exact native vtable entries. Proximity opening may bypass
// request_open locally, which is why source observation uses started_opening;
// remote replay can still invoke the retail request method directly.
constexpr std::uintptr_t kDoorRequestOpenRva = 0x000E8B00U;
constexpr std::uintptr_t kDoorRequestCloseRva = 0x000E8C10U;
constexpr std::uintptr_t kDoorRequestLockRva = 0x000E8D10U;
constexpr std::uintptr_t kDoorRequestUnlockRva = 0x000E8E10U;
constexpr std::uintptr_t kDoorForceOpenRva = 0x000E8F20U;
constexpr std::uintptr_t kDoorForceCloseRva = 0x000E9040U;

// Door_Package's authoritative lock state converges on the internal
// LogicSwitch 2DFF01B2. These are its native set/clear methods.
constexpr std::uintptr_t kLockSwitchSetRva = 0x001B4D20U;
constexpr std::uintptr_t kLockSwitchClearRva = 0x001B4D60U;

constexpr std::uintptr_t kDoorVtableRva = 0x00DE0138U;
constexpr std::uintptr_t kLogicSwitchVtableRva = 0x00E62880U;
constexpr std::ptrdiff_t kDoorNativeOffset = 0x18;
constexpr std::ptrdiff_t kLogicSwitchNativeOffset = 0x14;

constexpr std::uint32_t kDoorLockSwitchEntity = 0x2DFF01B2U;

constexpr std::uint32_t kRequestOpen = 0x8DA169A3U;
constexpr std::uint32_t kRequestClose = 0x2F53149CU;
constexpr std::uint32_t kForceOpen = 0x4E85BB2DU;
constexpr std::uint32_t kForceClose = 0x7D4FFD73U;
constexpr std::uint32_t kLockDoor = 0xDBBC7DB3U;
constexpr std::uint32_t kUnlockDoor = 0xBDF60342U;

constexpr std::size_t kRegistryCapacity = 4096U;
constexpr std::size_t kMaximumObservedEvents = 256U;
constexpr std::size_t kMaximumPendingEvents = 256U;
constexpr std::size_t kMaximumInjectionResults = 256U;
constexpr std::size_t kMaximumSuppressions = 64U;
constexpr std::uint64_t kPendingLifetimeMs = 6000U;
// A remote Door request can take several seconds before the matching retail
// started_opening/started_closing output is emitted. The old 1500 ms window
// expired before that output and allowed the replay to echo back over the
// network. Keep the suppression until the expected output is consumed, with a
// bounded safety expiry longer than the observed 3-4 s transition latency.
constexpr std::uint64_t kRemoteOutputSuppressionMs = 10000U;
constexpr std::uint32_t kMaximumReplayAttempts = 6U;

struct EntityHandle final {
    std::uint32_t a{};
    std::uint32_t b{};

    [[nodiscard]] bool Valid() const noexcept {
        return a != 0U || b != 0U;
    }

    [[nodiscard]] std::uint64_t Key() const noexcept {
        return static_cast<std::uint64_t>(a) |
            (static_cast<std::uint64_t>(b) << 32U);
    }
};

enum class RuntimeKind : std::uint8_t {
    Unknown = 0,
    Door = 1,
    LockSwitch = 2
};

struct RuntimeEntry final {
    std::uint64_t key{};
    std::uint32_t levelGeneration{};
    RuntimeKind kind{RuntimeKind::Unknown};
    void* outerEntity{};
    std::array<std::uint32_t, 4> contextA{};
    bool contextAValid{};
    void* contextBObject{};
};

struct PendingDoorEvent final {
    DoorEvent event{};
    std::uint32_t nativeGeneration{};
    std::uint64_t queuedAtMs{};
    std::uint64_t nextAttemptAtMs{};
    std::uint32_t attempts{};
};

enum class ReplayAttemptResult : std::uint8_t {
    Success = 0,
    Retryable = 1,
    TerminalFailure = 2
};

struct Suppression final {
    std::uint64_t key{};
    std::uint32_t signal{};
    std::uint64_t expiresAtMs{};
};

struct TrampolineSlot final {
    void* raw{};
    std::atomic<void*> published{nullptr};
};

using EntityHandleFn = void(__thiscall*)(void*, std::uint32_t*);
using GeneratedConstructorFn = void*(__thiscall*)(void*, void*, void*, void*);
using DoorOutputFn = void(__thiscall*)(void*, void*, void*, double);
using DoorInputFn = std::uint8_t(__thiscall*)(void*, void*, void*);
using LogicSwitchInputFn = std::uint8_t(__thiscall*)(void*, void*, void*);

std::atomic<bool> gInstalled{false};
std::atomic<bool> gShuttingDown{false};
std::atomic<void*> gEntityHandleFunction{nullptr};
std::atomic<std::uintptr_t> gDoorVtable{0U};
std::atomic<std::uintptr_t> gLogicSwitchVtable{0U};
std::atomic<LevelGenerationTracker*> gGenerations{nullptr};
std::atomic<std::uint64_t> gSequence{1U};

TrampolineSlot gDoorConstructor;
TrampolineSlot gLogicSwitchConstructor;
TrampolineSlot gStartedOpening;
TrampolineSlot gStartedClosing;
TrampolineSlot gFinishedOpening;
TrampolineSlot gFinishedClosing;
TrampolineSlot gUsedLocked;
TrampolineSlot gUsedUnlocked;
TrampolineSlot gDoorRequestOpen;
TrampolineSlot gDoorRequestClose;
TrampolineSlot gDoorRequestLock;
TrampolineSlot gDoorRequestUnlock;
TrampolineSlot gDoorForceOpen;
TrampolineSlot gDoorForceClose;
TrampolineSlot gLockSwitchSet;
TrampolineSlot gLockSwitchClear;

std::atomic<std::uint64_t> gStatsVersion{0U};
std::atomic<std::uint64_t> gDoorNativeDispatches{0U};
std::atomic<std::uint64_t> gLockSwitchNativeDispatches{0U};
std::atomic<std::uint64_t> gResolvedTrackedDispatches{0U};
std::atomic<std::uint64_t> gObservedDoorEvents{0U};
std::atomic<std::uint64_t> gReplayAttempts{0U};
std::atomic<std::uint64_t> gReplaySuccesses{0U};
std::atomic<std::uint64_t> gReplayMissingRuntime{0U};
std::atomic<std::uint64_t> gReplayMissingContext{0U};
std::atomic<std::uint64_t> gReplaySyntheticContextAttempts{0U};
std::atomic<std::uint64_t> gReplayInvalidNativeObject{0U};
std::atomic<std::uint64_t> gReplayDeferredRetries{0U};
std::atomic<std::uint64_t> gReplayRetryExhausted{0U};
std::atomic<std::uint64_t> gSuppressedRemoteEchoes{0U};

std::array<RuntimeEntry, kRegistryCapacity> gRegistry{};
std::mutex gRegistryMutex;
std::deque<DoorEvent> gObserved;
std::mutex gObservedMutex;
std::deque<PendingDoorEvent> gPending;
std::mutex gPendingMutex;
std::deque<DoorInjectionResult> gInjectionResults;
std::mutex gInjectionResultsMutex;
std::array<Suppression, kMaximumSuppressions> gSuppressions{};
std::mutex gSuppressionMutex;

thread_local bool gRemoteInjection = false;
thread_local std::uint32_t gLocalForceOpenDepth = 0U;
thread_local std::uint32_t gLocalForceCloseDepth = 0U;

[[nodiscard]] std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(GetTickCount64());
}

[[nodiscard]] std::uint32_t CurrentGeneration() noexcept {
    const auto* generations = gGenerations.load(std::memory_order_acquire);
    if (generations == nullptr) return 0U;
    return static_cast<std::uint32_t>((std::min<std::uint64_t>)(
        generations->CurrentGeneration(),
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())));
}

void BumpStat(std::atomic<std::uint64_t>& counter) noexcept {
    counter.fetch_add(1U, std::memory_order_relaxed);
    gStatsVersion.fetch_add(1U, std::memory_order_release);
}

[[nodiscard]] bool ValidateBytes(
    const std::uintptr_t address,
    const std::initializer_list<std::uint8_t> expected) noexcept {
    std::array<std::uint8_t, 16> bytes{};
    if (expected.size() > bytes.size() ||
        !crash_guard::TryCopy(
            bytes.data(),
            reinterpret_cast<const void*>(address),
            expected.size())) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), bytes.begin());
}

[[nodiscard]] EntityHandle ResolveHandle(void* outerEntity) noexcept {
    EntityHandle result{};
    if (outerEntity == nullptr || gShuttingDown.load(std::memory_order_acquire)) {
        return result;
    }
    const auto raw = gEntityHandleFunction.load(std::memory_order_acquire);
    if (raw == nullptr) return result;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        reinterpret_cast<EntityHandleFn>(raw)(
            outerEntity,
            reinterpret_cast<std::uint32_t*>(&result));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        result = {};
    }
#endif
    return result;
}

[[nodiscard]] void* ReadSharedObject(void* holder) noexcept {
    if (holder == nullptr) return nullptr;
    const auto value = crash_guard::SafeRead<void*>(holder);
    return value.has_value() ? *value : nullptr;
}

void RegisterRuntimeEntity(
    const EntityHandle handle,
    void* outerEntity,
    const RuntimeKind kind) noexcept {
    if (!handle.Valid() || outerEntity == nullptr || kind == RuntimeKind::Unknown) return;
    std::lock_guard lock(gRegistryMutex);
    const auto key = handle.Key();
    const auto start = static_cast<std::size_t>(key % kRegistryCapacity);
    for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
        auto& entry = gRegistry[(start + probe) % kRegistryCapacity];
        if (entry.key == 0U || entry.key == key) {
            entry.key = key;
            entry.levelGeneration = CurrentGeneration();
            entry.kind = kind;
            entry.outerEntity = outerEntity;
            return;
        }
    }
}

void RegisterRuntime(
    const EntityHandle handle,
    void* outerEntity,
    const RuntimeKind kind,
    void* contextA,
    void* contextBHolder) noexcept {
    if (!handle.Valid() || outerEntity == nullptr || kind == RuntimeKind::Unknown) return;

    std::array<std::uint32_t, 4> snapshot{};
    const bool contextAValid = contextA != nullptr &&
        crash_guard::TryCopy(snapshot.data(), contextA, sizeof(snapshot));
    const auto contextBObject = ReadSharedObject(contextBHolder);

    std::lock_guard lock(gRegistryMutex);
    const auto key = handle.Key();
    const auto start = static_cast<std::size_t>(key % kRegistryCapacity);
    for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
        auto& entry = gRegistry[(start + probe) % kRegistryCapacity];
        if (entry.key == 0U || entry.key == key) {
            entry.key = key;
            entry.levelGeneration = CurrentGeneration();
            entry.kind = kind;
            entry.outerEntity = outerEntity;
            if (contextAValid) {
                entry.contextA = snapshot;
                entry.contextAValid = true;
                entry.contextBObject = contextBObject;
            }
            return;
        }
    }
}

[[nodiscard]] bool FindRuntime(
    const EntityHandle handle,
    const RuntimeKind expectedKind,
    RuntimeEntry& output) noexcept {
    if (!handle.Valid() || expectedKind == RuntimeKind::Unknown) return false;
    RuntimeEntry candidate{};
    {
        std::lock_guard lock(gRegistryMutex);
        const auto key = handle.Key();
        const auto start = static_cast<std::size_t>(key % kRegistryCapacity);
        for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
            const auto& entry = gRegistry[(start + probe) % kRegistryCapacity];
            if (entry.key == key) {
                candidate = entry;
                break;
            }
            if (entry.key == 0U) return false;
        }
    }

    if (candidate.key == 0U || candidate.kind != expectedKind ||
        candidate.outerEntity == nullptr ||
        !crash_guard::IsReadable(candidate.outerEntity, sizeof(void*))) {
        return false;
    }
    const auto resolved = ResolveHandle(candidate.outerEntity);
    if (!resolved.Valid() || resolved.Key() != handle.Key()) return false;
    candidate.levelGeneration = CurrentGeneration();
    output = candidate;
    return true;
}

[[nodiscard]] bool HasReplayContext(const RuntimeEntry& entry) noexcept {
    if (!entry.contextAValid) return false;
    const auto shared = static_cast<std::uintptr_t>(entry.contextA[0]);
    if (shared != 0U &&
        !crash_guard::IsReadable(reinterpret_cast<void*>(shared), sizeof(void*))) {
        return false;
    }
    if (entry.contextBObject != nullptr &&
        !crash_guard::IsReadable(entry.contextBObject, sizeof(void*))) {
        return false;
    }
    return true;
}

[[nodiscard]] EntityHandle CaptureDoor(
    void* nativeDoor,
    void* contextA = nullptr,
    void* contextBHolder = nullptr) noexcept {
    EntityHandle handle{};
    if (nativeDoor == nullptr || gShuttingDown.load(std::memory_order_acquire)) {
        return handle;
    }
    const auto vtable = crash_guard::SafeRead<std::uintptr_t>(nativeDoor);
    if (!vtable.has_value() ||
        *vtable != gDoorVtable.load(std::memory_order_acquire)) {
        return handle;
    }
    auto* outer = reinterpret_cast<std::byte*>(nativeDoor) - kDoorNativeOffset;
    handle = ResolveHandle(outer);
    if (!handle.Valid()) return {};
    if (contextA != nullptr) {
        RegisterRuntime(
            handle, outer, RuntimeKind::Door, contextA, contextBHolder);
    } else {
        RegisterRuntimeEntity(handle, outer, RuntimeKind::Door);
    }
    BumpStat(gResolvedTrackedDispatches);
    return handle;
}

[[nodiscard]] EntityHandle CaptureLockSwitch(
    void* nativeSwitch,
    void* contextA = nullptr,
    void* contextBHolder = nullptr) noexcept {
    EntityHandle handle{};
    if (nativeSwitch == nullptr || gShuttingDown.load(std::memory_order_acquire)) {
        return handle;
    }
    const auto vtable = crash_guard::SafeRead<std::uintptr_t>(nativeSwitch);
    if (!vtable.has_value() ||
        *vtable != gLogicSwitchVtable.load(std::memory_order_acquire)) {
        return handle;
    }
    auto* outer = reinterpret_cast<std::byte*>(nativeSwitch) -
        kLogicSwitchNativeOffset;
    handle = ResolveHandle(outer);
    // The LogicSwitch class is used broadly. Keep the Door_Package-specific
    // ShortGuid filter here so unrelated gameplay switches are never mirrored
    // as door lock state.
    if (!handle.Valid() || handle.a != kDoorLockSwitchEntity) return {};
    if (contextA != nullptr) {
        RegisterRuntime(
            handle, outer, RuntimeKind::LockSwitch, contextA, contextBHolder);
    } else {
        RegisterRuntimeEntity(handle, outer, RuntimeKind::LockSwitch);
    }
    BumpStat(gResolvedTrackedDispatches);
    return handle;
}

void QueueObserved(
    const EntityHandle handle,
    const std::uint32_t semanticSignal,
    const double value = 0.0) noexcept {
    if (!handle.Valid() || semanticSignal == 0U || gRemoteInjection) return;
    DoorEvent event{};
    event.levelGeneration = CurrentGeneration();
    event.sequence = gSequence.fetch_add(1U, std::memory_order_relaxed);
    event.entityA = handle.a;
    event.entityB = handle.b;
    event.signal = semanticSignal;
    event.valueBits = std::bit_cast<std::uint64_t>(value);
    BumpStat(gObservedDoorEvents);
    std::lock_guard lock(gObservedMutex);
    if (gObserved.size() >= kMaximumObservedEvents) gObserved.pop_front();
    gObserved.push_back(event);
}

void QueueInjectionResult(const DoorEvent& event, const bool injected) noexcept {
    DoorInjectionResult result{};
    result.sequence = event.sequence;
    result.entityA = event.entityA;
    result.entityB = event.entityB;
    result.signal = event.signal;
    result.injected = injected;
    std::lock_guard lock(gInjectionResultsMutex);
    if (gInjectionResults.size() >= kMaximumInjectionResults) {
        gInjectionResults.pop_front();
    }
    gInjectionResults.push_back(result);
}

void MarkOutputSuppression(
    const EntityHandle handle,
    const std::uint32_t signal) noexcept {
    if (!handle.Valid() || signal == 0U) return;
    const auto now = NowMs();
    std::lock_guard lock(gSuppressionMutex);
    Suppression* candidate = nullptr;
    for (auto& entry : gSuppressions) {
        if (entry.expiresAtMs <= now) entry = {};
        if (entry.key == handle.Key() && entry.signal == signal) {
            candidate = &entry;
            break;
        }
        if (candidate == nullptr && entry.key == 0U) candidate = &entry;
    }
    if (candidate == nullptr) candidate = &gSuppressions.front();
    *candidate = Suppression{
        handle.Key(), signal, now + kRemoteOutputSuppressionMs};
}

[[nodiscard]] bool ConsumeOutputSuppression(
    const EntityHandle handle,
    const std::uint32_t signal) noexcept {
    if (!handle.Valid() || signal == 0U) return false;
    const auto now = NowMs();
    std::lock_guard lock(gSuppressionMutex);
    for (auto& entry : gSuppressions) {
        if (entry.expiresAtMs <= now) {
            entry = {};
            continue;
        }
        if (entry.key == handle.Key() && entry.signal == signal) {
            entry = {};
            BumpStat(gSuppressedRemoteEchoes);
            return true;
        }
    }
    return false;
}

void ClearOutputSuppression(
    const EntityHandle handle,
    const std::uint32_t signal) noexcept {
    if (!handle.Valid() || signal == 0U) return;
    std::lock_guard lock(gSuppressionMutex);
    for (auto& entry : gSuppressions) {
        if (entry.key == handle.Key() && entry.signal == signal) {
            entry = {};
        }
    }
}

void* __fastcall DoorConstructorDetour(
    void* self,
    void*,
    void* arg1,
    void* arg2,
    void* arg3) noexcept {
    const auto raw = gDoorConstructor.published.load(std::memory_order_acquire);
    if (raw == nullptr) return self;
    auto* result = reinterpret_cast<GeneratedConstructorFn>(raw)(
        self, arg1, arg2, arg3);
    if (gInstalled.load(std::memory_order_acquire) && result != nullptr) {
        (void)CaptureDoor(reinterpret_cast<std::byte*>(result) + kDoorNativeOffset);
    }
    return result;
}

void* __fastcall LogicSwitchConstructorDetour(
    void* self,
    void*,
    void* arg1,
    void* arg2,
    void* arg3) noexcept {
    const auto raw = gLogicSwitchConstructor.published.load(
        std::memory_order_acquire);
    if (raw == nullptr) return self;
    auto* result = reinterpret_cast<GeneratedConstructorFn>(raw)(
        self, arg1, arg2, arg3);
    if (gInstalled.load(std::memory_order_acquire) && result != nullptr) {
        (void)CaptureLockSwitch(
            reinterpret_cast<std::byte*>(result) + kLogicSwitchNativeOffset);
    }
    return result;
}

void ObserveDoorOutput(
    TrampolineSlot& slot,
    void* self,
    void* contextA,
    void* contextB,
    const double value,
    const std::uint32_t semanticSignal,
    const bool emit) noexcept {
    BumpStat(gDoorNativeDispatches);
    const auto handle = CaptureDoor(self, contextA, contextB);
    const bool suppressed = semanticSignal != 0U &&
        ConsumeOutputSuppression(handle, semanticSignal);

    const auto raw = slot.published.load(std::memory_order_acquire);
    if (raw != nullptr) {
        reinterpret_cast<DoorOutputFn>(raw)(self, contextA, contextB, value);
    }

    if (!emit || !handle.Valid() || suppressed || gRemoteInjection) return;
    if (semanticSignal == kRequestOpen && gLocalForceOpenDepth != 0U) return;
    if (semanticSignal == kRequestClose && gLocalForceCloseDepth != 0U) return;
    QueueObserved(handle, semanticSignal, value);
}

void __fastcall StartedOpeningDetour(
    void* self, void*, void* contextA, void* contextB, double value) noexcept {
    ObserveDoorOutput(
        gStartedOpening, self, contextA, contextB, value, kRequestOpen, true);
}

void __fastcall StartedClosingDetour(
    void* self, void*, void* contextA, void* contextB, double value) noexcept {
    ObserveDoorOutput(
        gStartedClosing, self, contextA, contextB, value, kRequestClose, true);
}

void __fastcall FinishedOpeningDetour(
    void* self, void*, void* contextA, void* contextB, double value) noexcept {
    ObserveDoorOutput(
        gFinishedOpening, self, contextA, contextB, value, 0U, false);
}

void __fastcall FinishedClosingDetour(
    void* self, void*, void* contextA, void* contextB, double value) noexcept {
    ObserveDoorOutput(
        gFinishedClosing, self, contextA, contextB, value, 0U, false);
}

void __fastcall UsedLockedDetour(
    void* self, void*, void* contextA, void* contextB, double value) noexcept {
    ObserveDoorOutput(gUsedLocked, self, contextA, contextB, value, 0U, false);
}

void __fastcall UsedUnlockedDetour(
    void* self, void*, void* contextA, void* contextB, double value) noexcept {
    ObserveDoorOutput(gUsedUnlocked, self, contextA, contextB, value, 0U, false);
}

std::uint8_t CallDoorInput(
    TrampolineSlot& slot,
    void* self,
    void* contextA,
    void* contextB) noexcept {
    BumpStat(gDoorNativeDispatches);
    (void)CaptureDoor(self, contextA, contextB);
    const auto raw = slot.published.load(std::memory_order_acquire);
    if (raw == nullptr) return 0U;
    return reinterpret_cast<DoorInputFn>(raw)(self, contextA, contextB);
}

std::uint8_t __fastcall DoorRequestOpenDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return CallDoorInput(gDoorRequestOpen, self, contextA, contextB);
}

std::uint8_t __fastcall DoorRequestCloseDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return CallDoorInput(gDoorRequestClose, self, contextA, contextB);
}

std::uint8_t __fastcall DoorRequestLockDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return CallDoorInput(gDoorRequestLock, self, contextA, contextB);
}

std::uint8_t __fastcall DoorRequestUnlockDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return CallDoorInput(gDoorRequestUnlock, self, contextA, contextB);
}

std::uint8_t __fastcall DoorForceOpenDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    BumpStat(gDoorNativeDispatches);
    const auto handle = CaptureDoor(self, contextA, contextB);
    const bool local = handle.Valid() && !gRemoteInjection;
    if (local) ++gLocalForceOpenDepth;
    const auto raw = gDoorForceOpen.published.load(std::memory_order_acquire);
    const auto result = raw == nullptr
        ? static_cast<std::uint8_t>(0U)
        : reinterpret_cast<DoorInputFn>(raw)(self, contextA, contextB);
    if (local) {
        --gLocalForceOpenDepth;
        if (result != 0U) QueueObserved(handle, kForceOpen);
    }
    return result;
}

std::uint8_t __fastcall DoorForceCloseDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    BumpStat(gDoorNativeDispatches);
    const auto handle = CaptureDoor(self, contextA, contextB);
    const bool local = handle.Valid() && !gRemoteInjection;
    if (local) ++gLocalForceCloseDepth;
    const auto raw = gDoorForceClose.published.load(std::memory_order_acquire);
    const auto result = raw == nullptr
        ? static_cast<std::uint8_t>(0U)
        : reinterpret_cast<DoorInputFn>(raw)(self, contextA, contextB);
    if (local) {
        --gLocalForceCloseDepth;
        if (result != 0U) QueueObserved(handle, kForceClose);
    }
    return result;
}

std::uint8_t ObserveLockSwitch(
    TrampolineSlot& slot,
    void* self,
    void* contextA,
    void* contextB,
    const bool setLocked) noexcept {
    BumpStat(gLockSwitchNativeDispatches);
    const auto before = crash_guard::SafeRead<std::uint8_t>(
        reinterpret_cast<std::byte*>(self) + 0x0CU);
    const auto handle = CaptureLockSwitch(self, contextA, contextB);
    const auto raw = slot.published.load(std::memory_order_acquire);
    const auto result = raw == nullptr
        ? static_cast<std::uint8_t>(0U)
        : reinterpret_cast<LogicSwitchInputFn>(raw)(self, contextA, contextB);
    const auto after = crash_guard::SafeRead<std::uint8_t>(
        reinterpret_cast<std::byte*>(self) + 0x0CU);

    if (!gRemoteInjection && handle.Valid() && before.has_value() &&
        after.has_value() && *before != *after) {
        if (setLocked && *after != 0U) {
            QueueObserved(handle, kLockDoor);
        } else if (!setLocked && *after == 0U) {
            QueueObserved(handle, kUnlockDoor);
        }
    }
    return result;
}

std::uint8_t __fastcall LockSwitchSetDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return ObserveLockSwitch(
        gLockSwitchSet, self, contextA, contextB, true);
}

std::uint8_t __fastcall LockSwitchClearDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return ObserveLockSwitch(
        gLockSwitchClear, self, contextA, contextB, false);
}

[[nodiscard]] bool IsDoorSignal(const std::uint32_t signal) noexcept {
    return signal == kRequestOpen || signal == kRequestClose ||
        signal == kForceOpen || signal == kForceClose;
}

[[nodiscard]] bool IsLockSignal(const std::uint32_t signal) noexcept {
    return signal == kLockDoor || signal == kUnlockDoor;
}

[[nodiscard]] bool IsSupportedRemoteEvent(const DoorEvent& event) noexcept {
    return IsDoorSignal(event.signal) || IsLockSignal(event.signal);
}

[[nodiscard]] RuntimeKind RuntimeKindForSignal(
    const std::uint32_t signal) noexcept {
    if (IsDoorSignal(signal)) return RuntimeKind::Door;
    if (IsLockSignal(signal)) return RuntimeKind::LockSwitch;
    return RuntimeKind::Unknown;
}

[[nodiscard]] bool ValidateNativeObject(
    void* nativeObject,
    const RuntimeKind kind) noexcept {
    if (nativeObject == nullptr) return false;
    const auto actual = crash_guard::SafeRead<std::uintptr_t>(nativeObject);
    if (!actual.has_value()) return false;
    if (kind == RuntimeKind::Door) {
        return *actual == gDoorVtable.load(std::memory_order_acquire);
    }
    if (kind == RuntimeKind::LockSwitch) {
        return *actual == gLogicSwitchVtable.load(std::memory_order_acquire);
    }
    return false;
}

[[nodiscard]] ReplayAttemptResult TryInject(const DoorEvent& event) noexcept {
    BumpStat(gReplayAttempts);
    if (!IsSupportedRemoteEvent(event)) {
        return ReplayAttemptResult::TerminalFailure;
    }

    const auto kind = RuntimeKindForSignal(event.signal);
    const EntityHandle handle{event.entityA, event.entityB};
    RuntimeEntry runtime{};
    if (!FindRuntime(handle, kind, runtime) || runtime.outerEntity == nullptr) {
        BumpStat(gReplayMissingRuntime);
        return ReplayAttemptResult::Retryable;
    }

    void* nativeObject = nullptr;
    TrampolineSlot* slot = nullptr;
    std::uint32_t suppressSignal = 0U;
    if (kind == RuntimeKind::Door) {
        nativeObject = reinterpret_cast<std::byte*>(runtime.outerEntity) +
            kDoorNativeOffset;
        if (event.signal == kRequestOpen) {
            slot = &gDoorRequestOpen;
            suppressSignal = kRequestOpen;
        } else if (event.signal == kRequestClose) {
            slot = &gDoorRequestClose;
            suppressSignal = kRequestClose;
        } else if (event.signal == kForceOpen) {
            slot = &gDoorForceOpen;
            suppressSignal = kRequestOpen;
        } else if (event.signal == kForceClose) {
            slot = &gDoorForceClose;
            suppressSignal = kRequestClose;
        }
    } else if (kind == RuntimeKind::LockSwitch) {
        nativeObject = reinterpret_cast<std::byte*>(runtime.outerEntity) +
            kLogicSwitchNativeOffset;
        slot = event.signal == kLockDoor ? &gLockSwitchSet : &gLockSwitchClear;
    }

    if (slot == nullptr || !ValidateNativeObject(nativeObject, kind)) {
        BumpStat(gReplayInvalidNativeObject);
        return ReplayAttemptResult::TerminalFailure;
    }
    const auto raw = slot->published.load(std::memory_order_acquire);
    if (raw == nullptr) {
        BumpStat(gReplayInvalidNativeObject);
        return ReplayAttemptResult::TerminalFailure;
    }

    auto contextA = runtime.contextA;
    auto* contextBObject = runtime.contextBObject;
    const bool syntheticContext = !HasReplayContext(runtime);
    if (syntheticContext) {
        // A remote door may never have produced a local transition. One
        // synthetic holder attempt is allowed so the first remote request can
        // still bootstrap it, but failures from that unsafe fallback are
        // terminal: never hammer the engine with the same synthetic call on
        // every frame.
        contextA = {};
        contextBObject = nullptr;
        BumpStat(gReplayMissingContext);
        BumpStat(gReplaySyntheticContextAttempts);
    }
    if (suppressSignal != 0U) MarkOutputSuppression(handle, suppressSignal);

#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        gRemoteInjection = true;
        std::uint8_t result = 0U;
        if (kind == RuntimeKind::Door) {
            result = reinterpret_cast<DoorInputFn>(raw)(
                nativeObject,
                contextA.data(),
                &contextBObject);
        } else {
            result = reinterpret_cast<LogicSwitchInputFn>(raw)(
                nativeObject,
                contextA.data(),
                &contextBObject);
        }
        gRemoteInjection = false;
        if (result == 0U) {
            if (suppressSignal != 0U) {
                ClearOutputSuppression(handle, suppressSignal);
            }
            return syntheticContext
                ? ReplayAttemptResult::TerminalFailure
                : ReplayAttemptResult::Retryable;
        }
        BumpStat(gReplaySuccesses);
        return ReplayAttemptResult::Success;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        gRemoteInjection = false;
        if (suppressSignal != 0U) {
            ClearOutputSuppression(handle, suppressSignal);
        }
        BumpStat(gReplayInvalidNativeObject);
        return ReplayAttemptResult::TerminalFailure;
    }
#else
    if (suppressSignal != 0U) ClearOutputSuppression(handle, suppressSignal);
    return ReplayAttemptResult::TerminalFailure;
#endif
}

[[nodiscard]] std::uint64_t ReplayBackoffMs(
    const std::uint32_t attempts) noexcept {
    switch (attempts) {
    case 0U:
    case 1U: return 50U;
    case 2U: return 100U;
    case 3U: return 250U;
    case 4U: return 500U;
    default: return 1000U;
    }
}

void ResetTrampolineSlot(TrampolineSlot& slot) noexcept {
    slot.raw = nullptr;
    slot.published.store(nullptr, std::memory_order_release);
}

void ResetAllTrampolines() noexcept {
    ResetTrampolineSlot(gDoorConstructor);
    ResetTrampolineSlot(gLogicSwitchConstructor);
    ResetTrampolineSlot(gStartedOpening);
    ResetTrampolineSlot(gStartedClosing);
    ResetTrampolineSlot(gFinishedOpening);
    ResetTrampolineSlot(gFinishedClosing);
    ResetTrampolineSlot(gUsedLocked);
    ResetTrampolineSlot(gUsedUnlocked);
    ResetTrampolineSlot(gDoorRequestOpen);
    ResetTrampolineSlot(gDoorRequestClose);
    ResetTrampolineSlot(gDoorRequestLock);
    ResetTrampolineSlot(gDoorRequestUnlock);
    ResetTrampolineSlot(gDoorForceOpen);
    ResetTrampolineSlot(gDoorForceClose);
    ResetTrampolineSlot(gLockSwitchSet);
    ResetTrampolineSlot(gLockSwitchClear);
}

bool InstallTargetedHook(
    HookManager& hooks,
    const std::uintptr_t moduleBase,
    const std::uintptr_t rva,
    const char* name,
    void* detour,
    const std::size_t patchLength,
    const std::initializer_list<std::uint8_t> expected,
    const char* mask,
    TrampolineSlot& slot,
    std::string& error) {
    HookRequest request{};
    request.name = name;
    request.target = reinterpret_cast<void*>(moduleBase + rva);
    request.detour = detour;
    request.patchLength = patchLength;
    request.expected.bytes.assign(expected.begin(), expected.end());
    request.expected.mask = mask;
    request.relocationSafePrefix = true;
    request.trampolineOut = &slot.raw;
    request.publishedTrampolineOut = &slot.published;
    return hooks.Install(request, error);
}

void ResetRuntimeState() noexcept {
    gStatsVersion.store(0U, std::memory_order_release);
    gDoorNativeDispatches.store(0U, std::memory_order_release);
    gLockSwitchNativeDispatches.store(0U, std::memory_order_release);
    gResolvedTrackedDispatches.store(0U, std::memory_order_release);
    gObservedDoorEvents.store(0U, std::memory_order_release);
    gReplayAttempts.store(0U, std::memory_order_release);
    gReplaySuccesses.store(0U, std::memory_order_release);
    gReplayMissingRuntime.store(0U, std::memory_order_release);
    gReplayMissingContext.store(0U, std::memory_order_release);
    gReplaySyntheticContextAttempts.store(0U, std::memory_order_release);
    gReplayInvalidNativeObject.store(0U, std::memory_order_release);
    gReplayDeferredRetries.store(0U, std::memory_order_release);
    gReplayRetryExhausted.store(0U, std::memory_order_release);
    gSuppressedRemoteEchoes.store(0U, std::memory_order_release);
    gSequence.store(1U, std::memory_order_release);
    {
        std::lock_guard lock(gRegistryMutex);
        for (auto& entry : gRegistry) entry = {};
    }
    {
        std::lock_guard lock(gSuppressionMutex);
        for (auto& entry : gSuppressions) entry = {};
    }
}

} // namespace

bool Install(
    const std::uintptr_t moduleBase,
    const std::size_t moduleSize,
    HookManager& hooks,
    LevelGenerationTracker& generations,
    std::string& error) {
#if !defined(_MSC_VER) || !defined(_M_IX86)
    error = "Door_Package targeted runtime replication requires MSVC x86";
    return false;
#else
    if (gInstalled.load(std::memory_order_acquire)) return true;
    if (moduleBase == 0U || moduleSize == 0U) {
        error = "AI.exe module range is unavailable";
        return false;
    }

    const auto inRange = [moduleSize](
        const std::uintptr_t rva,
        const std::size_t size) noexcept {
        return rva < moduleSize && size <= moduleSize - rva;
    };
    const std::array<std::uintptr_t, 17> requiredRvas{
        kEntityHandleRva,
        kDoorGeneratedConstructorRva,
        kLogicSwitchGeneratedConstructorRva,
        kStartedOpeningRva,
        kStartedClosingRva,
        kFinishedOpeningRva,
        kFinishedClosingRva,
        kUsedLockedRva,
        kUsedUnlockedRva,
        kDoorRequestOpenRva,
        kDoorRequestCloseRva,
        kDoorRequestLockRva,
        kDoorRequestUnlockRva,
        kDoorForceOpenRva,
        kDoorForceCloseRva,
        kLockSwitchSetRva,
        kLockSwitchClearRva};
    for (const auto rva : requiredRvas) {
        if (!inRange(rva, 8U)) {
            error = "targeted Door_Package RVA falls outside AI.exe";
            return false;
        }
    }
    if (!inRange(kDoorVtableRva + 0x3ACU, sizeof(std::uintptr_t)) ||
        !inRange(kLogicSwitchVtableRva + 0x3A0U, sizeof(std::uintptr_t))) {
        error = "Door/LogicSwitch vtable range falls outside AI.exe";
        return false;
    }

    if (!ValidateBytes(
            moduleBase + kEntityHandleRva,
            {0x55U, 0x8BU, 0xECU, 0x51U, 0x56U, 0x8BU, 0xF1U}) ||
        !ValidateBytes(
            moduleBase + kStartedOpeningRva,
            {0x55U, 0x8BU, 0xECU, 0xDDU, 0x45U, 0x10U}) ||
        !ValidateBytes(
            moduleBase + kStartedClosingRva,
            {0x55U, 0x8BU, 0xECU, 0xDDU, 0x45U, 0x10U})) {
        error = "targeted Door_Package resolver/output bytes do not match the validated retail build";
        return false;
    }

    const auto doorVtable = moduleBase + kDoorVtableRva;
    const auto switchVtable = moduleBase + kLogicSwitchVtableRva;
    const auto readSlot = [](const std::uintptr_t address) noexcept {
        return crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(address));
    };
    const std::array<std::pair<std::uintptr_t, std::uintptr_t>, 8> slots{
        std::pair{doorVtable + 0x398U, moduleBase + kDoorRequestOpenRva},
        std::pair{doorVtable + 0x39CU, moduleBase + kDoorRequestCloseRva},
        std::pair{doorVtable + 0x3A0U, moduleBase + kDoorRequestLockRva},
        std::pair{doorVtable + 0x3A4U, moduleBase + kDoorRequestUnlockRva},
        std::pair{doorVtable + 0x3A8U, moduleBase + kDoorForceOpenRva},
        std::pair{doorVtable + 0x3ACU, moduleBase + kDoorForceCloseRva},
        std::pair{switchVtable + 0x39CU, moduleBase + kLockSwitchSetRva},
        std::pair{switchVtable + 0x3A0U, moduleBase + kLockSwitchClearRva}};
    for (const auto& [slotAddress, expected] : slots) {
        const auto actual = readSlot(slotAddress);
        if (!actual.has_value() || *actual != expected) {
            error = "Door/LogicSwitch targeted vtable layout does not match the validated retail build";
            return false;
        }
    }

    gEntityHandleFunction.store(
        reinterpret_cast<void*>(moduleBase + kEntityHandleRva),
        std::memory_order_release);
    gDoorVtable.store(doorVtable, std::memory_order_release);
    gLogicSwitchVtable.store(switchVtable, std::memory_order_release);
    gGenerations.store(&generations, std::memory_order_release);
    gShuttingDown.store(false, std::memory_order_release);
    ResetRuntimeState();
    ResetAllTrampolines();

    const auto checkpoint = hooks.InstalledCount();
    const auto install = [&](
        const std::uintptr_t rva,
        const char* name,
        void* detour,
        const std::size_t length,
        const std::initializer_list<std::uint8_t> bytes,
        const char* mask,
        TrampolineSlot& slot) -> bool {
        if (InstallTargetedHook(
                hooks,
                moduleBase,
                rva,
                name,
                detour,
                length,
                bytes,
                mask,
                slot,
                error)) {
            return true;
        }
        error = std::string{name} + ": " + error;
        return false;
    };

    bool ok = true;
    ok = ok && install(kDoorGeneratedConstructorRva,
        "world_sync_door_generated_constructor",
        reinterpret_cast<void*>(&DoorConstructorDetour), 7U,
        {0x6AU, 0xFFU, 0x68U, 0x46U, 0x0FU, 0x12U, 0x01U},
        "xxx????", gDoorConstructor);
    ok = ok && install(kLogicSwitchGeneratedConstructorRva,
        "world_sync_lock_switch_generated_constructor",
        reinterpret_cast<void*>(&LogicSwitchConstructorDetour), 7U,
        {0x6AU, 0xFFU, 0x68U, 0xA0U, 0x21U, 0x12U, 0x01U},
        "xxx????", gLogicSwitchConstructor);

    ok = ok && install(kStartedOpeningRva, "world_sync_door_started_opening",
        reinterpret_cast<void*>(&StartedOpeningDetour), 6U,
        {0x55U, 0x8BU, 0xECU, 0xDDU, 0x45U, 0x10U}, "xxxxxx",
        gStartedOpening);
    ok = ok && install(kStartedClosingRva, "world_sync_door_started_closing",
        reinterpret_cast<void*>(&StartedClosingDetour), 6U,
        {0x55U, 0x8BU, 0xECU, 0xDDU, 0x45U, 0x10U}, "xxxxxx",
        gStartedClosing);
    ok = ok && install(kFinishedOpeningRva, "world_sync_door_finished_opening",
        reinterpret_cast<void*>(&FinishedOpeningDetour), 6U,
        {0x55U, 0x8BU, 0xECU, 0xDDU, 0x45U, 0x10U}, "xxxxxx",
        gFinishedOpening);
    ok = ok && install(kFinishedClosingRva, "world_sync_door_finished_closing",
        reinterpret_cast<void*>(&FinishedClosingDetour), 6U,
        {0x55U, 0x8BU, 0xECU, 0xDDU, 0x45U, 0x10U}, "xxxxxx",
        gFinishedClosing);
    ok = ok && install(kUsedLockedRva, "world_sync_door_used_locked",
        reinterpret_cast<void*>(&UsedLockedDetour), 6U,
        {0x55U, 0x8BU, 0xECU, 0xDDU, 0x45U, 0x10U}, "xxxxxx",
        gUsedLocked);
    ok = ok && install(kUsedUnlockedRva, "world_sync_door_used_unlocked",
        reinterpret_cast<void*>(&UsedUnlockedDetour), 6U,
        {0x55U, 0x8BU, 0xECU, 0xDDU, 0x45U, 0x10U}, "xxxxxx",
        gUsedUnlocked);

    const auto doorInputPrefix = std::initializer_list<std::uint8_t>{
        0x6AU, 0xFFU, 0x68U, 0x18U, 0x23U, 0x12U, 0x01U};
    ok = ok && install(kDoorRequestOpenRva, "world_sync_door_request_open",
        reinterpret_cast<void*>(&DoorRequestOpenDetour), 7U,
        doorInputPrefix, "xxx????", gDoorRequestOpen);
    ok = ok && install(kDoorRequestCloseRva, "world_sync_door_request_close",
        reinterpret_cast<void*>(&DoorRequestCloseDetour), 7U,
        doorInputPrefix, "xxx????", gDoorRequestClose);
    ok = ok && install(kDoorRequestLockRva, "world_sync_door_request_lock",
        reinterpret_cast<void*>(&DoorRequestLockDetour), 7U,
        doorInputPrefix, "xxx????", gDoorRequestLock);
    ok = ok && install(kDoorRequestUnlockRva, "world_sync_door_request_unlock",
        reinterpret_cast<void*>(&DoorRequestUnlockDetour), 7U,
        doorInputPrefix, "xxx????", gDoorRequestUnlock);
    ok = ok && install(kDoorForceOpenRva, "world_sync_door_force_open",
        reinterpret_cast<void*>(&DoorForceOpenDetour), 7U,
        doorInputPrefix, "xxx????", gDoorForceOpen);
    ok = ok && install(kDoorForceCloseRva, "world_sync_door_force_close",
        reinterpret_cast<void*>(&DoorForceCloseDetour), 7U,
        {0x6AU, 0xFFU, 0x68U, 0xC0U, 0x20U, 0x12U, 0x01U},
        "xxx????", gDoorForceClose);

    ok = ok && install(kLockSwitchSetRva, "world_sync_lock_switch_set",
        reinterpret_cast<void*>(&LockSwitchSetDetour), 7U,
        {0x56U, 0x8BU, 0xF1U, 0x80U, 0x7EU, 0x0CU, 0x00U},
        "xxxxxxx", gLockSwitchSet);
    ok = ok && install(kLockSwitchClearRva, "world_sync_lock_switch_clear",
        reinterpret_cast<void*>(&LockSwitchClearDetour), 7U,
        {0x56U, 0x8BU, 0xF1U, 0x80U, 0x7EU, 0x0CU, 0x01U},
        "xxxxxxx", gLockSwitchClear);

    if (!ok) {
        std::string rollbackError;
        hooks.UninstallFrom(checkpoint, rollbackError);
        gEntityHandleFunction.store(nullptr, std::memory_order_release);
        gDoorVtable.store(0U, std::memory_order_release);
        gLogicSwitchVtable.store(0U, std::memory_order_release);
        gGenerations.store(nullptr, std::memory_order_release);
        // HookManager deliberately retains retired trampolines until process
        // shutdown/quiescence is proven. Keep published pointers alive too so
        // an in-flight targeted detour can always complete its retail call.
        if (!rollbackError.empty()) error += "; rollback: " + rollbackError;
        return false;
    }

    gInstalled.store(true, std::memory_order_release);
    return true;
#endif
}

bool Installed() noexcept {
    return gInstalled.load(std::memory_order_acquire);
}

DoorRuntimeDiagnostics DiagnosticsSnapshot() noexcept {
    DoorRuntimeDiagnostics result{};
    result.version = gStatsVersion.load(std::memory_order_acquire);
    result.doorNativeDispatches =
        gDoorNativeDispatches.load(std::memory_order_relaxed);
    result.lockSwitchNativeDispatches =
        gLockSwitchNativeDispatches.load(std::memory_order_relaxed);
    result.resolvedTrackedDispatches =
        gResolvedTrackedDispatches.load(std::memory_order_relaxed);
    result.observedEvents = gObservedDoorEvents.load(std::memory_order_relaxed);
    result.replayAttempts = gReplayAttempts.load(std::memory_order_relaxed);
    result.replaySuccesses = gReplaySuccesses.load(std::memory_order_relaxed);
    result.replayMissingRuntime =
        gReplayMissingRuntime.load(std::memory_order_relaxed);
    result.replayMissingContext =
        gReplayMissingContext.load(std::memory_order_relaxed);
    result.replaySyntheticContextAttempts =
        gReplaySyntheticContextAttempts.load(std::memory_order_relaxed);
    result.replayInvalidNativeObject =
        gReplayInvalidNativeObject.load(std::memory_order_relaxed);
    result.replayDeferredRetries =
        gReplayDeferredRetries.load(std::memory_order_relaxed);
    result.replayRetryExhausted =
        gReplayRetryExhausted.load(std::memory_order_relaxed);
    result.suppressedRemoteEchoes =
        gSuppressedRemoteEchoes.load(std::memory_order_relaxed);
    return result;
}

bool QueueRemoteDoorEvent(const DoorEvent& event) noexcept {
    if (!Installed() || gShuttingDown.load(std::memory_order_acquire) ||
        event.sequence == 0U || !IsSupportedRemoteEvent(event) ||
        (event.entityA == 0U && event.entityB == 0U)) {
        return false;
    }
    const auto nativeGeneration = CurrentGeneration();
    if (nativeGeneration == 0U) return false;
    std::lock_guard lock(gPendingMutex);
    if (gPending.size() >= kMaximumPendingEvents) gPending.pop_front();
    const auto now = NowMs();
    // Do not queue the exact same remote semantic transition more than once at
    // a time. Network sequence validation already removes wire duplicates;
    // this additionally protects the engine queue from echo/retry overlap.
    for (const auto& pending : gPending) {
        if (pending.event.entityA == event.entityA &&
            pending.event.entityB == event.entityB &&
            pending.event.signal == event.signal) {
            return true;
        }
    }
    gPending.push_back(PendingDoorEvent{
        event, nativeGeneration, now, now, 0U});
    return true;
}

bool TryPopObservedDoorEvent(DoorEvent& event) noexcept {
    std::lock_guard lock(gObservedMutex);
    if (gObserved.empty()) return false;
    event = gObserved.front();
    gObserved.pop_front();
    return true;
}

bool TryPopInjectionResult(DoorInjectionResult& result) noexcept {
    std::lock_guard lock(gInjectionResultsMutex);
    if (gInjectionResults.empty()) return false;
    result = gInjectionResults.front();
    gInjectionResults.pop_front();
    return true;
}

void PumpOnEngineThread() noexcept {
    if (!Installed() || gShuttingDown.load(std::memory_order_acquire)) return;
    std::deque<PendingDoorEvent> work;
    {
        std::lock_guard lock(gPendingMutex);
        work.swap(gPending);
    }
    if (work.empty()) return;

    const auto now = NowMs();
    std::deque<PendingDoorEvent> retry;
    while (!work.empty()) {
        auto pending = work.front();
        work.pop_front();
        if (pending.nativeGeneration != CurrentGeneration()) {
            QueueInjectionResult(pending.event, false);
            continue;
        }
        if (now < pending.nextAttemptAtMs) {
            retry.push_back(pending);
            continue;
        }

        ++pending.attempts;
        const auto attempt = TryInject(pending.event);
        if (attempt == ReplayAttemptResult::Success) {
            QueueInjectionResult(pending.event, true);
            continue;
        }
        if (attempt == ReplayAttemptResult::TerminalFailure) {
            QueueInjectionResult(pending.event, false);
            continue;
        }

        const auto age = now - pending.queuedAtMs;
        if (age <= kPendingLifetimeMs &&
            pending.attempts < kMaximumReplayAttempts) {
            pending.nextAttemptAtMs = now + ReplayBackoffMs(pending.attempts);
            BumpStat(gReplayDeferredRetries);
            retry.push_back(pending);
        } else {
            BumpStat(gReplayRetryExhausted);
            QueueInjectionResult(pending.event, false);
        }
    }
    if (!retry.empty()) {
        std::lock_guard lock(gPendingMutex);
        while (!retry.empty() && gPending.size() < kMaximumPendingEvents) {
            gPending.push_back(retry.front());
            retry.pop_front();
        }
    }
}

void BeginLogicalShutdown() noexcept {
    gShuttingDown.store(true, std::memory_order_release);
    gInstalled.store(false, std::memory_order_release);
    gGenerations.store(nullptr, std::memory_order_release);
    gEntityHandleFunction.store(nullptr, std::memory_order_release);
    gDoorVtable.store(0U, std::memory_order_release);
    gLogicSwitchVtable.store(0U, std::memory_order_release);

    {
        std::lock_guard lock(gPendingMutex);
        gPending.clear();
    }
    {
        std::lock_guard lock(gObservedMutex);
        gObserved.clear();
    }
    {
        std::lock_guard lock(gInjectionResultsMutex);
        gInjectionResults.clear();
    }
    {
        std::lock_guard lock(gRegistryMutex);
        for (auto& entry : gRegistry) entry = {};
    }
    {
        std::lock_guard lock(gSuppressionMutex);
        for (auto& entry : gSuppressions) entry = {};
    }
}

} // namespace aimp::world_sync
