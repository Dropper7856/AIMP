#include "aimp/WorldAnimatedAdapters.h"

#include "aimp/BuildProfile.h"
#include "aimp/CathodePrimitiveExecutor.h"
#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"
#include "aimp/ReplicationCore.h"
#include "aimp/SignatureResolver.h"
#include "aimp/WorldEntityRegistry.h"
#include "aimp/WorldPublicActions.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <mutex>

namespace aimp::world_animated {
namespace {

constexpr std::ptrdiff_t kDoorNativeOffset = 0x18;
constexpr std::size_t kSharedObjectOffset = 0x0CU;
constexpr std::size_t kObservationSlots = 512U;
constexpr std::size_t kMatchSlots = 256U;
constexpr std::size_t kProbeCount = 8U;
constexpr std::uint64_t kAnimatedLocalMatchLifetimeMs = 1500U;
constexpr std::uint64_t kDoorRemoteSuppressionLifetimeMs = 10000U;

constexpr std::uint32_t kRequestOpen = 0x8DA169A3U;
constexpr std::uint32_t kRequestClose = 0x2F53149CU;
// Runtime-validated DoorNative semantic inputs. These are the exact ShortGuids
// dispatched by AI.exe+0x5159C0 to vtable +0x3A0/+0x3A4 and were confirmed
// against the retail executable used by the WS3 session. Capture occurs at the
// native RequestLock/RequestUnlock boundary. Patch 5.12 keeps these hooks as
// passive witnesses only; replication is owned by the generic upstream
// StateTransition carrier that already works in the Host -> Client direction.
constexpr std::uint32_t kRequestLock = kDoorRequestLockInput;
constexpr std::uint32_t kRequestUnlock = kDoorRequestUnlockInput;
constexpr std::uint32_t kForceOpen = 0x4E85BB2DU;
constexpr std::uint32_t kForceClose = 0x7D4FFD73U;

using DoorOutputFn = void (__thiscall*)(void*, void*, void*, double);
using DoorInputFn = std::uint8_t (__thiscall*)(void*, void*, void*);

struct TrampolineSlot final {
    void* raw{};
    std::atomic<void*> published{nullptr};
};

struct TimedMatchSlot final {
    std::atomic<std::uint64_t> hash{0U};
    std::atomic<std::uint64_t> expiresAtMs{0U};
};

std::atomic<bool> gInitialized{false};
std::atomic<bool> gDoorHooksInstalled{false};
std::atomic<bool> gDoorUnlockProbeInstalled{false};
std::atomic<bool> gDoorSemanticProbeInstalled{false};
std::mutex gDoorUnlockProbeArmMutex;
std::atomic<WorldEntityRegistry*> gRegistry{nullptr};
const SignatureResolver* gResolver{};
HookManager* gHooks{};
std::atomic<const void*> gDoorNativeVtable{nullptr};
std::atomic<const void*> gPlayEnvironmentAnimationVtable{nullptr};
std::atomic<const void*> gAnimatedModelAttachmentNodeVtable{nullptr};

TrampolineSlot gStartedOpening;
TrampolineSlot gStartedClosing;
TrampolineSlot gRequestLock;
TrampolineSlot gRequestUnlock;
TrampolineSlot gForceOpen;
TrampolineSlot gForceClose;

std::atomic<std::uint64_t> gObservationSequence{1U};
std::uint64_t gObservationSalt{1U};
std::array<std::atomic<std::uint64_t>, kObservationSlots> gLocalDoorObservations{};
std::array<TimedMatchSlot, kMatchSlots> gAnimatedLocalMatches{};
std::array<TimedMatchSlot, kMatchSlots> gDoorRemoteSuppressions{};

std::atomic<std::uint64_t> gVersion{0U};
std::atomic<std::uint64_t> gInitializationCount{0U};
std::atomic<std::uint64_t> gShutdownCount{0U};
std::atomic<std::uint64_t> gDoorOutputCalls{0U};
std::atomic<std::uint64_t> gDoorInputCalls{0U};
std::atomic<std::uint64_t> gDoorUnlockProbeCalls{0U};
std::atomic<std::uint64_t> gDoorUnlockProbeSuccess{0U};
std::atomic<std::uint64_t> gDoorUnlockProbeResolved{0U};
std::atomic<std::uint64_t> gDoorUnlockProbeUnresolved{0U};
std::atomic<std::uint64_t> gDoorUnlockProbeLastEntityHigh{0U};
std::atomic<std::uint64_t> gDoorUnlockProbeLastEntityLow{0U};
std::atomic<std::uint64_t> gDoorRequestLockProbeCalls{0U};
std::atomic<std::uint64_t> gDoorRequestLockProbeSuccess{0U};
std::atomic<std::uint64_t> gDoorRequestLockProbeResolved{0U};
std::atomic<std::uint64_t> gDoorForceOpenProbeCalls{0U};
std::atomic<std::uint64_t> gDoorForceOpenProbeSuccess{0U};
std::atomic<std::uint64_t> gDoorForceOpenProbeResolved{0U};
std::atomic<std::uint64_t> gDoorForceCloseProbeCalls{0U};
std::atomic<std::uint64_t> gDoorForceCloseProbeSuccess{0U};
std::atomic<std::uint64_t> gDoorForceCloseProbeResolved{0U};
std::atomic<std::uint64_t> gDoorSemanticProbeLastAction{0U};
std::atomic<std::uint64_t> gDoorSemanticProbeLastEntityHigh{0U};
std::atomic<std::uint64_t> gDoorSemanticProbeLastEntityLow{0U};
std::atomic<std::uint64_t> gDoorLockStateSourceResolved{0U};
std::atomic<std::uint64_t> gDoorLockStateSourceUnresolved{0U};
std::atomic<std::uint64_t> gDoorLockStateSelfSourceRejected{0U};
std::atomic<std::uint64_t> gDoorLockStateLastSourceHigh{0U};
std::atomic<std::uint64_t> gDoorLockStateLastSourceLow{0U};
std::atomic<std::uint64_t> gDoorObservedActions{0U};
std::atomic<std::uint64_t> gDoorUnresolved{0U};
std::atomic<std::uint64_t> gDoorRemoteSuppressed{0U};
std::atomic<std::uint64_t> gDoorOriginEchoSuppressed{0U};
std::atomic<std::uint64_t> gDoorTransitionRootAttempts{0U};
std::atomic<std::uint64_t> gDoorTransitionRootSubmitted{0U};
std::atomic<std::uint64_t> gDoorTransitionRootRetailOnly{0U};
std::atomic<std::uint64_t> gDoorTransitionRootAwaitAuthority{0U};
std::atomic<std::uint64_t> gDoorTransitionRootAuthoritySuppressed{0U};
std::atomic<std::uint64_t> gAnimatedObservedEdges{0U};
std::atomic<std::uint64_t> gAnimatedSubmittedIntents{0U};
std::atomic<std::uint64_t> gAnimatedOriginMatchesSuppressed{0U};
std::atomic<std::uint64_t> gAnimatedTargetClassMismatch{0U};
std::atomic<std::uint64_t> gExecuteAttempts{0U};
std::atomic<std::uint64_t> gQueuedRemoteActions{0U};
std::atomic<std::uint64_t> gQueuedRemoteAnimatedEdges{0U};
std::atomic<std::uint64_t> gInvalidPayloads{0U};
std::atomic<std::uint64_t> gRuntimeUnavailable{0U};
std::atomic<std::uint64_t> gNativeFaults{0U};
std::atomic<std::uint64_t> gActiveDoorDetours{0U};

thread_local std::uint32_t gForceOpenDepth = 0U;
thread_local std::uint32_t gForceCloseDepth = 0U;

void TouchVersion() noexcept {
    gVersion.fetch_add(1U, std::memory_order_acq_rel);
}

class ActiveDoorDetour final {
public:
    ActiveDoorDetour() noexcept { gActiveDoorDetours.fetch_add(1U, std::memory_order_acq_rel); }
    ~ActiveDoorDetour() { gActiveDoorDetours.fetch_sub(1U, std::memory_order_acq_rel); }
};

[[nodiscard]] std::uint64_t TickMs() noexcept {
    return static_cast<std::uint64_t>(GetTickCount64());
}

[[nodiscard]] std::uint64_t Mix64(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t HashDoorSuppression(
    const WorldEntityId& entity,
    const DoorAction action) noexcept {
    auto value = Mix64(entity.high ^ 0xD00A7E0000000001ULL);
    value ^= Mix64(entity.low + static_cast<std::uint8_t>(action));
    value = Mix64(value);
    return value == 0U ? 1U : value;
}

[[nodiscard]] std::uint64_t HashAnimatedEdge(
    const AnimatedInputEdge& edge,
    const WorldEntityId& targetEntity) noexcept {
    auto value = Mix64(targetEntity.high ^ targetEntity.low);
    value ^= Mix64(edge.sourceEntity.high ^ edge.sourceEntity.low);
    value ^= Mix64(
        (static_cast<std::uint64_t>(edge.schedulerParameterShortGuid) << 32U) |
        edge.targetInputShortGuid);
    value ^= Mix64(
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(edge.targetCategory)) << 32U) |
        std::bit_cast<std::uint32_t>(edge.relativeDelaySeconds));
    value = Mix64(value);
    return value == 0U ? 1U : value;
}

void RememberTimed(
    std::array<TimedMatchSlot, kMatchSlots>& slots,
    const std::uint64_t hash,
    const std::uint64_t lifetimeMs) noexcept {
    const auto now = TickMs();
    const auto start = static_cast<std::size_t>(hash) & (kMatchSlots - 1U);
    for (std::size_t probe = 0U; probe < kProbeCount; ++probe) {
        auto& slot = slots[(start + probe) & (kMatchSlots - 1U)];
        const auto currentHash = slot.hash.load(std::memory_order_acquire);
        const auto expiry = slot.expiresAtMs.load(std::memory_order_acquire);
        if (currentHash == hash || currentHash == 0U || expiry <= now) {
            slot.hash.store(hash, std::memory_order_release);
            slot.expiresAtMs.store(now + lifetimeMs, std::memory_order_release);
            return;
        }
    }
    auto& slot = slots[start];
    slot.hash.store(hash, std::memory_order_release);
    slot.expiresAtMs.store(now + lifetimeMs, std::memory_order_release);
}

[[nodiscard]] bool ConsumeTimed(
    std::array<TimedMatchSlot, kMatchSlots>& slots,
    const std::uint64_t hash) noexcept {
    const auto now = TickMs();
    const auto start = static_cast<std::size_t>(hash) & (kMatchSlots - 1U);
    for (std::size_t probe = 0U; probe < kProbeCount; ++probe) {
        auto& slot = slots[(start + probe) & (kMatchSlots - 1U)];
        const auto expiry = slot.expiresAtMs.load(std::memory_order_acquire);
        auto currentHash = slot.hash.load(std::memory_order_acquire);
        if (currentHash == 0U) continue;
        if (expiry <= now) {
            slot.hash.compare_exchange_strong(
                currentHash, 0U, std::memory_order_acq_rel, std::memory_order_acquire);
            continue;
        }
        if (currentHash == hash && slot.hash.compare_exchange_strong(
                currentHash, 0U, std::memory_order_acq_rel, std::memory_order_acquire)) {
            slot.expiresAtMs.store(0U, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void ClearTimed(
    std::array<TimedMatchSlot, kMatchSlots>& slots,
    const std::uint64_t hash) noexcept {
    const auto start = static_cast<std::size_t>(hash) & (kMatchSlots - 1U);
    for (std::size_t probe = 0U; probe < kProbeCount; ++probe) {
        auto& slot = slots[(start + probe) & (kMatchSlots - 1U)];
        auto currentHash = slot.hash.load(std::memory_order_acquire);
        if (currentHash == hash && slot.hash.compare_exchange_strong(
                currentHash, 0U, std::memory_order_acq_rel, std::memory_order_acquire)) {
            slot.expiresAtMs.store(0U, std::memory_order_release);
            return;
        }
    }
}

void ClearAllTransient() noexcept {
    for (auto& slot : gLocalDoorObservations) slot.store(0U, std::memory_order_release);
    for (auto& slot : gAnimatedLocalMatches) {
        slot.hash.store(0U, std::memory_order_release);
        slot.expiresAtMs.store(0U, std::memory_order_release);
    }
    for (auto& slot : gDoorRemoteSuppressions) {
        slot.hash.store(0U, std::memory_order_release);
        slot.expiresAtMs.store(0U, std::memory_order_release);
    }
}

[[nodiscard]] bool RememberDoorObservation(const std::uint64_t id) noexcept {
    const auto start = static_cast<std::size_t>(id) & (kObservationSlots - 1U);
    for (std::size_t probe = 0U; probe < kProbeCount; ++probe) {
        auto& slot = gLocalDoorObservations[(start + probe) & (kObservationSlots - 1U)];
        auto expected = std::uint64_t{0U};
        if (slot.compare_exchange_strong(
                expected, id, std::memory_order_acq_rel, std::memory_order_acquire) ||
            expected == id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool RemoveDoorObservation(const std::uint64_t id) noexcept {
    if (id == 0U) return false;
    const auto start = static_cast<std::size_t>(id) & (kObservationSlots - 1U);
    for (std::size_t probe = 0U; probe < kProbeCount; ++probe) {
        auto& slot = gLocalDoorObservations[(start + probe) & (kObservationSlots - 1U)];
        auto expected = id;
        if (slot.compare_exchange_strong(
                expected, 0U, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<WorldEntityId> IdentifyDoor(void* nativeDoor) noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    const auto expectedVtable = gDoorNativeVtable.load(std::memory_order_acquire);
    if (registry == nullptr || nativeDoor == nullptr || expectedVtable == nullptr) return std::nullopt;
    const auto vtable = crash_guard::SafeRead<const void*>(nativeDoor);
    if (!vtable.has_value() || *vtable != expectedVtable) return std::nullopt;
    const auto* outer = static_cast<const std::byte*>(nativeDoor) - kDoorNativeOffset;
    return registry->IdentifyRuntimeEntity(outer);
}

struct DoorContextSourceResolution final {
    std::optional<WorldEntityId> entity{};
    bool selfSource{};
};

[[nodiscard]] DoorContextSourceResolution IdentifyDoorContextSource(
    WorldEntityRegistry& registry,
    void* contextA,
    const WorldEntityId& door) noexcept {
    DoorContextSourceResolution result{};
    if (contextA == nullptr || !door.Valid()) return result;

    // Retail RequestLock/RequestUnlock receives a pointer to the source shared
    // reference as its first context argument. Resolve only the local execution
    // object behind that ref; neither native pointer is ever serialized.
    const auto shared = crash_guard::SafeRead<void*>(contextA);
    if (!shared.has_value() || *shared == nullptr) return result;
    const auto execution = crash_guard::SafeRead<void*>(
        static_cast<const std::byte*>(*shared) + kSharedObjectOffset);
    if (!execution.has_value() || *execution == nullptr) return result;

    const auto source = registry.IdentifyRuntimeWorldEntity(*execution);
    if (!source.has_value() || !source->Valid()) return result;
    if (*source == door) {
        result.selfSource = true;
        return result;
    }
    result.entity = *source;
    return result;
}

[[nodiscard]] bool SubmitDoorAction(
    const WorldEntityId& entity,
    const DoorAction action) noexcept {
    DoorActionPayload payloadValue{action, NextObservationId()};
    if (!RememberDoorObservation(payloadValue.observationId)) return false;
    WorldPayload payload{};
    if (!EncodeDoorActionPayload(payloadValue, payload) ||
        !replication_core::SubmitLocalIntent(
            entity,
            WorldMutationKind::Event,
            kOperationDoorAction,
            std::span<const std::byte>(payload.bytes.data(), payload.size))) {
        static_cast<void>(RemoveDoorObservation(payloadValue.observationId));
        return false;
    }
    gDoorObservedActions.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return true;
}

[[nodiscard]] bool DoorRemoteSuppressed(
    const WorldEntityId& entity,
    const DoorAction action) noexcept {
    if (ConsumeTimed(
            gDoorRemoteSuppressions,
            HashDoorSuppression(entity, action))) {
        gDoorRemoteSuppressed.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return true;
    }
    return false;
}

void DoorOutput(
    TrampolineSlot& slot,
    void* self,
    void* contextA,
    void* contextB,
    const double value,
    const DoorAction action) noexcept {
    ActiveDoorDetour active;
    gDoorOutputCalls.fetch_add(1U, std::memory_order_acq_rel);
    const auto entity = IdentifyDoor(self);
    const bool remoteSuppressed = entity.has_value() && DoorRemoteSuppressed(*entity, action);
    const bool forceDepth =
        (action == DoorAction::Open && gForceOpenDepth != 0U) ||
        (action == DoorAction::Close && gForceCloseDepth != 0U);
    const auto original = reinterpret_cast<DoorOutputFn>(
        slot.published.load(std::memory_order_acquire));
    if (original != nullptr) original(self, contextA, contextB, value);
    if (!gInitialized.load(std::memory_order_acquire) || remoteSuppressed || forceDepth) return;
    if (!entity.has_value()) {
        gDoorUnresolved.fetch_add(1U, std::memory_order_acq_rel);
        return;
    }

    // Patch 5.3.3 regression hotfix: observation only. Do not synthesize a
    // request_open/request_close SchedulerEdge from a started_* output. The
    // 5.3.2 Door->Door, callbackless self-edge had no proven retail provenance
    // and is the only new active ROOT correlated with the client crash.
    if (action != DoorAction::Open && action != DoorAction::Close) return;
    gDoorObservedActions.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
}

void __fastcall StartedOpeningDetour(
    void* self, void*, void* contextA, void* contextB, const double value) noexcept {
    DoorOutput(gStartedOpening, self, contextA, contextB, value, DoorAction::Open);
}

void __fastcall StartedClosingDetour(
    void* self, void*, void* contextA, void* contextB, const double value) noexcept {
    DoorOutput(gStartedClosing, self, contextA, contextB, value, DoorAction::Close);
}

[[nodiscard]] std::uint8_t DoorInput(
    TrampolineSlot& slot,
    void* self,
    void* contextA,
    void* contextB,
    const DoorAction action,
    std::uint32_t* forceDepth = nullptr) noexcept {
    ActiveDoorDetour active;
    gDoorInputCalls.fetch_add(1U, std::memory_order_acq_rel);
    const auto entity = IdentifyDoor(self);
    const bool remoteSuppressed = entity.has_value() && DoorRemoteSuppressed(*entity, action);
    if (forceDepth != nullptr) ++*forceDepth;
    const auto original = reinterpret_cast<DoorInputFn>(
        slot.published.load(std::memory_order_acquire));
    const auto result = original == nullptr
        ? static_cast<std::uint8_t>(0U)
        : original(self, contextA, contextB);
    if (forceDepth != nullptr) --*forceDepth;
    if (!gInitialized.load(std::memory_order_acquire) || remoteSuppressed || result == 0U) return result;
    if (!entity.has_value()) {
        gDoorUnresolved.fetch_add(1U, std::memory_order_acq_rel);
        return result;
    }
    static_cast<void>(SubmitDoorAction(*entity, action));
    return result;
}

struct DoorSemanticProbeCounters final {
    std::atomic<std::uint64_t>* calls{};
    std::atomic<std::uint64_t>* success{};
    std::atomic<std::uint64_t>* resolved{};
};

[[nodiscard]] DoorSemanticProbeCounters ProbeCountersForAction(
    const DoorAction action) noexcept {
    switch (action) {
    case DoorAction::Lock:
        return {&gDoorRequestLockProbeCalls, &gDoorRequestLockProbeSuccess,
            &gDoorRequestLockProbeResolved};
    case DoorAction::Unlock:
        return {&gDoorUnlockProbeCalls, &gDoorUnlockProbeSuccess,
            &gDoorUnlockProbeResolved};
    case DoorAction::ForceOpen:
        return {&gDoorForceOpenProbeCalls, &gDoorForceOpenProbeSuccess,
            &gDoorForceOpenProbeResolved};
    case DoorAction::ForceClose:
        return {&gDoorForceCloseProbeCalls, &gDoorForceCloseProbeSuccess,
            &gDoorForceCloseProbeResolved};
    default:
        return {};
    }
}

[[nodiscard]] std::uint8_t DoorSemanticProbeInput(
    TrampolineSlot& slot,
    void* self,
    void* contextA,
    void* contextB,
    const DoorAction action) noexcept {
    ActiveDoorDetour active;
    gDoorInputCalls.fetch_add(1U, std::memory_order_acq_rel);
    const auto original = reinterpret_cast<DoorInputFn>(
        slot.published.load(std::memory_order_acquire));
    const auto result = original == nullptr
        ? static_cast<std::uint8_t>(0U)
        : original(self, contextA, contextB);
    if (!gInitialized.load(std::memory_order_acquire)) return result;
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr || !registry->RuntimeCaptureReady()) return result;

    const auto counters = ProbeCountersForAction(action);
    if (counters.calls == nullptr || counters.success == nullptr || counters.resolved == nullptr)
        return result;
    counters.calls->fetch_add(1U, std::memory_order_acq_rel);
    gDoorSemanticProbeLastAction.store(
        static_cast<std::uint64_t>(static_cast<std::uint8_t>(action)),
        std::memory_order_release);
    if (result == 0U) {
        TouchVersion();
        return result;
    }
    counters.success->fetch_add(1U, std::memory_order_acq_rel);
    const auto entity = IdentifyDoor(self);
    if (!entity.has_value()) {
        if (action == DoorAction::Unlock)
            gDoorUnlockProbeUnresolved.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return result;
    }
    counters.resolved->fetch_add(1U, std::memory_order_acq_rel);
    gDoorSemanticProbeLastEntityHigh.store(entity->high, std::memory_order_release);
    gDoorSemanticProbeLastEntityLow.store(entity->low, std::memory_order_release);
    if (action == DoorAction::Unlock) {
        gDoorUnlockProbeLastEntityHigh.store(entity->high, std::memory_order_release);
        gDoorUnlockProbeLastEntityLow.store(entity->low, std::memory_order_release);
    }

    // Patch 5.12: RequestLock/RequestUnlock returns to probe-only. Runtime
    // evidence showed that the working Host -> Client path does not transmit a
    // synthetic Door semantic call; it publishes the generic certified
    // StateTransition carrier upstream, which then naturally reaches the local
    // Door RequestLock/RequestUnlock boundary on the receiver. Keep observing
    // this native boundary, but never author ROOT 7 from it.
    TouchVersion();
    return result;
}

std::uint8_t __fastcall RequestLockDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return DoorSemanticProbeInput(
        gRequestLock, self, contextA, contextB, DoorAction::Lock);
}

std::uint8_t __fastcall RequestUnlockDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return DoorSemanticProbeInput(
        gRequestUnlock, self, contextA, contextB, DoorAction::Unlock);
}

std::uint8_t __fastcall ForceOpenDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return DoorSemanticProbeInput(
        gForceOpen, self, contextA, contextB, DoorAction::ForceOpen);
}

std::uint8_t __fastcall ForceCloseDetour(
    void* self, void*, void* contextA, void* contextB) noexcept {
    return DoorSemanticProbeInput(
        gForceClose, self, contextA, contextB, DoorAction::ForceClose);
}

[[nodiscard]] std::uint32_t DoorActionInput(const DoorAction action) noexcept {
    switch (action) {
    case DoorAction::Open: return kRequestOpen;
    case DoorAction::Close: return kRequestClose;
    case DoorAction::Lock: return kRequestLock;
    case DoorAction::Unlock: return kRequestUnlock;
    case DoorAction::ForceOpen: return kForceOpen;
    case DoorAction::ForceClose: return kForceClose;
    default: return 0U;
    }
}

void RememberDoorRemoteAction(
    const WorldEntityId& entity,
    const DoorAction action) noexcept {
    RememberTimed(
        gDoorRemoteSuppressions,
        HashDoorSuppression(entity, action),
        kDoorRemoteSuppressionLifetimeMs);
    if (action == DoorAction::ForceOpen) {
        RememberTimed(
            gDoorRemoteSuppressions,
            HashDoorSuppression(entity, DoorAction::Open),
            kDoorRemoteSuppressionLifetimeMs);
    } else if (action == DoorAction::ForceClose) {
        RememberTimed(
            gDoorRemoteSuppressions,
            HashDoorSuppression(entity, DoorAction::Close),
            kDoorRemoteSuppressionLifetimeMs);
    }
}

void ClearDoorRemoteAction(
    const WorldEntityId& entity,
    const DoorAction action) noexcept {
    ClearTimed(gDoorRemoteSuppressions, HashDoorSuppression(entity, action));
    if (action == DoorAction::ForceOpen) {
        ClearTimed(gDoorRemoteSuppressions, HashDoorSuppression(entity, DoorAction::Open));
    } else if (action == DoorAction::ForceClose) {
        ClearTimed(gDoorRemoteSuppressions, HashDoorSuppression(entity, DoorAction::Close));
    }
}

[[nodiscard]] bool InstallHook(
    const SignatureResolver& resolver,
    HookManager& hooks,
    const SymbolId symbol,
    const char* name,
    void* detour,
    TrampolineSlot& slot,
    std::string& error) {
    const auto& profile = Retail2015Profile();
    const auto* spec = profile.Find(symbol);
    auto* target = resolver.ResolveMutable(symbol);
    if (spec == nullptr || target == nullptr || spec->approvedPatchLength < 5U ||
        !spec->trampolineRelocationSafe || !spec->expected.IsValid()) {
        error = std::string{name} + ": symbol is unavailable or not hook-certified";
        return false;
    }
    HookRequest request{};
    request.name = name;
    request.target = target;
    request.detour = detour;
    request.patchLength = spec->approvedPatchLength;
    request.expected = spec->expected;
    request.relocationSafePrefix = spec->trampolineRelocationSafe;
    request.trampolineOut = &slot.raw;
    request.publishedTrampolineOut = &slot.published;
    return hooks.Install(request, error);
}

} // namespace

bool Initialize(
    const SignatureResolver& resolver,
    HookManager& hooks,
    WorldEntityRegistry& registry,
    std::string& error) noexcept {
    gResolver = &resolver;
    gHooks = &hooks;
    // Keep dormant compatibility detours referenced so /W4 /WX does not emit
    // C4505 for the intentionally disabled direct-hook implementation.
    (void)&StartedOpeningDetour;
    (void)&StartedClosingDetour;
    (void)&DoorInput;
    (void)&IdentifyDoorContextSource;
    (void)&RequestLockDetour;
    (void)&RequestUnlockDetour;
    (void)&ForceOpenDetour;
    (void)&ForceCloseDetour;
    (void)&InstallHook;
    error.clear();
    bool expected = false;
    if (!gInitialized.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }

    // Initialization remains hook-free so level construction is untouched.
    // Patch 5.3.3 keeps all object-specific Door capture dormant. The generic
    // WS3 StateTransition path remains the only active Door-state owner while
    // event-boundary investigation continues through passive probes.
    gRegistry.store(&registry, std::memory_order_release);
    gDoorNativeVtable.store(nullptr, std::memory_order_release);
    gPlayEnvironmentAnimationVtable.store(nullptr, std::memory_order_release);
    gAnimatedModelAttachmentNodeVtable.store(nullptr, std::memory_order_release);
    gDoorHooksInstalled.store(false, std::memory_order_release);
    gDoorUnlockProbeInstalled.store(false, std::memory_order_release);
    gDoorSemanticProbeInstalled.store(false, std::memory_order_release);
    ClearAllTransient();
    gInitializationCount.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return true;
}

bool ArmDoorTransitionCapture(std::string& error) noexcept {
    // Patch 5.3.3: deliberate fail-closed compatibility no-op. Never re-arm
    // started_opening/started_closing from this compatibility entry point.
    error.clear();
    gDoorHooksInstalled.store(false, std::memory_order_release);
    return true;
}

bool ArmDoorSemanticInputProbe(std::string& error) noexcept {
    error.clear();
    std::lock_guard lock(gDoorUnlockProbeArmMutex);
    if (!gInitialized.load(std::memory_order_acquire) ||
        gRegistry.load(std::memory_order_acquire) == nullptr ||
        gResolver == nullptr || gHooks == nullptr) {
        error = "Door semantic-input probe is not initialized";
        return false;
    }
    if (gDoorSemanticProbeInstalled.load(std::memory_order_acquire)) return true;

    const auto registryState = gRegistry.load(std::memory_order_acquire)->DiagnosticsSnapshot();
    if (!registryState.manifestLoaded || registryState.levelGeneration == 0U) {
        error = "Door semantic-input probe requires an active runtime manifest";
        return false;
    }

    const auto* doorVtable = gResolver->Resolve(SymbolId::CathodeDoorNativeVtable);
    if (doorVtable == nullptr) {
        error = "Door semantic-input probe: DoorNative vtable is unavailable";
        return false;
    }
    gDoorNativeVtable.store(doorVtable, std::memory_order_release);

    // Preflight every boundary before installing the first detour. The probes
    // are diagnostics-only, but all four semantic-input boundaries must be
    // available so the next run can distinguish lock/unlock from force open/close.
    const auto& profile = Retail2015Profile();
    constexpr std::array symbols{
        SymbolId::CathodeDoorRequestLock, SymbolId::CathodeDoorRequestUnlock,
        SymbolId::CathodeDoorForceOpen, SymbolId::CathodeDoorForceClose};
    for (const auto symbol : symbols) {
        const auto* spec = profile.Find(symbol);
        if (spec == nullptr || gResolver->Resolve(symbol) == nullptr ||
            spec->approvedPatchLength < 5U || !spec->trampolineRelocationSafe ||
            !spec->expected.IsValid()) {
            error = "Door semantic-input probe: one or more certified boundaries are unavailable";
            gDoorNativeVtable.store(nullptr, std::memory_order_release);
            return false;
        }
    }

    const auto hookCheckpoint = gHooks->InstalledCount();
    if (!InstallHook(*gResolver, *gHooks, SymbolId::CathodeDoorRequestLock,
            "ws3_door_request_lock_probe", reinterpret_cast<void*>(&RequestLockDetour),
            gRequestLock, error) ||
        !InstallHook(*gResolver, *gHooks, SymbolId::CathodeDoorRequestUnlock,
            "ws3_door_request_unlock_probe", reinterpret_cast<void*>(&RequestUnlockDetour),
            gRequestUnlock, error) ||
        !InstallHook(*gResolver, *gHooks, SymbolId::CathodeDoorForceOpen,
            "ws3_door_force_open_probe", reinterpret_cast<void*>(&ForceOpenDetour),
            gForceOpen, error) ||
        !InstallHook(*gResolver, *gHooks, SymbolId::CathodeDoorForceClose,
            "ws3_door_force_close_probe", reinterpret_cast<void*>(&ForceCloseDetour),
            gForceClose, error)) {
        std::string rollbackError;
        if (!gHooks->UninstallFrom(hookCheckpoint, rollbackError) && !rollbackError.empty())
            error += "; rollback: " + rollbackError;
        return false;
    }

    gDoorUnlockProbeInstalled.store(true, std::memory_order_release);
    gDoorSemanticProbeInstalled.store(true, std::memory_order_release);
    TouchVersion();
    return true;
}

bool ArmDoorUnlockProbe(std::string& error) noexcept {
    return ArmDoorSemanticInputProbe(error);
}

bool Initialized() noexcept {
    return gInitialized.load(std::memory_order_acquire);
}

RuntimeDiagnostics DiagnosticsSnapshot() noexcept {
    RuntimeDiagnostics result{};
    result.initialized = Initialized();
    result.doorHooksInstalled = gDoorHooksInstalled.load(std::memory_order_acquire);
    result.doorUnlockProbeInstalled = gDoorUnlockProbeInstalled.load(std::memory_order_acquire);
    result.doorSemanticProbeInstalled = gDoorSemanticProbeInstalled.load(std::memory_order_acquire);
    result.version = gVersion.load(std::memory_order_acquire);
    result.initializationCount = gInitializationCount.load(std::memory_order_acquire);
    result.shutdownCount = gShutdownCount.load(std::memory_order_acquire);
    result.doorOutputCalls = gDoorOutputCalls.load(std::memory_order_acquire);
    result.doorInputCalls = gDoorInputCalls.load(std::memory_order_acquire);
    result.doorUnlockProbeCalls = gDoorUnlockProbeCalls.load(std::memory_order_acquire);
    result.doorUnlockProbeSuccess = gDoorUnlockProbeSuccess.load(std::memory_order_acquire);
    result.doorUnlockProbeResolved = gDoorUnlockProbeResolved.load(std::memory_order_acquire);
    result.doorUnlockProbeUnresolved = gDoorUnlockProbeUnresolved.load(std::memory_order_acquire);
    result.doorUnlockProbeLastEntityHigh = gDoorUnlockProbeLastEntityHigh.load(std::memory_order_acquire);
    result.doorUnlockProbeLastEntityLow = gDoorUnlockProbeLastEntityLow.load(std::memory_order_acquire);
    result.doorRequestLockProbeCalls = gDoorRequestLockProbeCalls.load(std::memory_order_acquire);
    result.doorRequestLockProbeSuccess = gDoorRequestLockProbeSuccess.load(std::memory_order_acquire);
    result.doorRequestLockProbeResolved = gDoorRequestLockProbeResolved.load(std::memory_order_acquire);
    result.doorForceOpenProbeCalls = gDoorForceOpenProbeCalls.load(std::memory_order_acquire);
    result.doorForceOpenProbeSuccess = gDoorForceOpenProbeSuccess.load(std::memory_order_acquire);
    result.doorForceOpenProbeResolved = gDoorForceOpenProbeResolved.load(std::memory_order_acquire);
    result.doorForceCloseProbeCalls = gDoorForceCloseProbeCalls.load(std::memory_order_acquire);
    result.doorForceCloseProbeSuccess = gDoorForceCloseProbeSuccess.load(std::memory_order_acquire);
    result.doorForceCloseProbeResolved = gDoorForceCloseProbeResolved.load(std::memory_order_acquire);
    result.doorSemanticProbeLastAction = gDoorSemanticProbeLastAction.load(std::memory_order_acquire);
    result.doorSemanticProbeLastEntityHigh = gDoorSemanticProbeLastEntityHigh.load(std::memory_order_acquire);
    result.doorSemanticProbeLastEntityLow = gDoorSemanticProbeLastEntityLow.load(std::memory_order_acquire);
    result.doorLockStateSourceResolved = gDoorLockStateSourceResolved.load(std::memory_order_acquire);
    result.doorLockStateSourceUnresolved = gDoorLockStateSourceUnresolved.load(std::memory_order_acquire);
    result.doorLockStateSelfSourceRejected = gDoorLockStateSelfSourceRejected.load(std::memory_order_acquire);
    result.doorLockStateLastSourceHigh = gDoorLockStateLastSourceHigh.load(std::memory_order_acquire);
    result.doorLockStateLastSourceLow = gDoorLockStateLastSourceLow.load(std::memory_order_acquire);
    result.doorObservedActions = gDoorObservedActions.load(std::memory_order_acquire);
    result.doorUnresolved = gDoorUnresolved.load(std::memory_order_acquire);
    result.doorRemoteSuppressed = gDoorRemoteSuppressed.load(std::memory_order_acquire);
    result.doorOriginEchoSuppressed = gDoorOriginEchoSuppressed.load(std::memory_order_acquire);
    result.doorTransitionRootAttempts = gDoorTransitionRootAttempts.load(std::memory_order_acquire);
    result.doorTransitionRootSubmitted = gDoorTransitionRootSubmitted.load(std::memory_order_acquire);
    result.doorTransitionRootRetailOnly = gDoorTransitionRootRetailOnly.load(std::memory_order_acquire);
    result.doorTransitionRootAwaitAuthority = gDoorTransitionRootAwaitAuthority.load(std::memory_order_acquire);
    result.doorTransitionRootAuthoritySuppressed = gDoorTransitionRootAuthoritySuppressed.load(std::memory_order_acquire);
    result.animatedObservedEdges = gAnimatedObservedEdges.load(std::memory_order_acquire);
    result.animatedSubmittedIntents = gAnimatedSubmittedIntents.load(std::memory_order_acquire);
    result.animatedOriginMatchesSuppressed = gAnimatedOriginMatchesSuppressed.load(std::memory_order_acquire);
    result.animatedTargetClassMismatch = gAnimatedTargetClassMismatch.load(std::memory_order_acquire);
    result.executeAttempts = gExecuteAttempts.load(std::memory_order_acquire);
    result.queuedRemoteActions = gQueuedRemoteActions.load(std::memory_order_acquire);
    result.queuedRemoteAnimatedEdges = gQueuedRemoteAnimatedEdges.load(std::memory_order_acquire);
    result.invalidPayloads = gInvalidPayloads.load(std::memory_order_acquire);
    result.runtimeUnavailable = gRuntimeUnavailable.load(std::memory_order_acquire);
    result.nativeFaults = gNativeFaults.load(std::memory_order_acquire);
    result.activeDoorDetours = gActiveDoorDetours.load(std::memory_order_acquire);
    return result;
}

AnimatedTargetCategory ClassifyAnimatedTarget(const void* executionObject) noexcept {
    if (!Initialized() || executionObject == nullptr) {
        return static_cast<AnimatedTargetCategory>(0U);
    }
    const auto vtable = crash_guard::SafeRead<const void*>(executionObject);
    if (!vtable.has_value() || *vtable == nullptr) {
        return static_cast<AnimatedTargetCategory>(0U);
    }
    if (*vtable == gPlayEnvironmentAnimationVtable.load(std::memory_order_acquire)) {
        return AnimatedTargetCategory::PlayEnvironmentAnimation;
    }
    if (*vtable == gAnimatedModelAttachmentNodeVtable.load(std::memory_order_acquire)) {
        return AnimatedTargetCategory::AnimatedModelAttachmentNode;
    }
    return static_cast<AnimatedTargetCategory>(0U);
}

std::uint64_t NextObservationId() noexcept {
    const auto sequence = gObservationSequence.fetch_add(1U, std::memory_order_acq_rel);
    auto value = Mix64(gObservationSalt ^ sequence);
    if (value == 0U) value = sequence == 0U ? 1U : sequence;
    return value;
}

bool ObserveAnimatedInputEdge(
    const AnimatedTargetCategory targetCategory,
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const std::uint64_t observationId,
    const WorldEntityId& sourceEntity,
    const WorldEntityId& targetEntity,
    const float relativeDelaySeconds) noexcept {
    AnimatedInputEdge edge{
        targetCategory,
        schedulerParameterShortGuid,
        targetInputShortGuid,
        observationId,
        sourceEntity,
        relativeDelaySeconds};
    WorldPayload payload{};
    if (!EncodeAnimatedInputEdgePayload(edge, payload) || !targetEntity.Valid()) {
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        return false;
    }
    gAnimatedObservedEdges.fetch_add(1U, std::memory_order_acq_rel);
    RememberTimed(
        gAnimatedLocalMatches,
        HashAnimatedEdge(edge, targetEntity),
        kAnimatedLocalMatchLifetimeMs);
    if (!replication_core::SubmitLocalIntent(
            targetEntity,
            WorldMutationKind::Event,
            kOperationAnimatedInputEdge,
            std::span<const std::byte>(payload.bytes.data(), payload.size))) {
        return false;
    }
    gAnimatedSubmittedIntents.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return true;
}

bool ConsumeLocalAnimatedMatch(
    const AnimatedInputEdge& edge,
    const WorldEntityId& targetEntity) noexcept {
    if (ConsumeTimed(gAnimatedLocalMatches, HashAnimatedEdge(edge, targetEntity))) {
        gAnimatedOriginMatchesSuppressed.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return true;
    }
    return false;
}

bool ConsumeLocalDoorObservation(const std::uint64_t observationId) noexcept {
    if (!RemoveDoorObservation(observationId)) return false;
    gDoorOriginEchoSuppressed.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return true;
}

ExecuteResult ExecuteDoorAction(
    const DoorActionPayload& payload,
    const WorldEntityId& targetEntity,
    const WorldEntityLease& target) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    if (!Initialized() || !target || !targetEntity.Valid()) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    const auto input = DoorActionInput(payload.action);
    if (input == 0U || payload.observationId == 0U) {
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::InvalidPayload, false};
    }
    RememberDoorRemoteAction(targetEntity, payload.action);
    const auto result = cathode_primitives::ExecuteInputEdge(
        input, input, 0.0F, target, target);
    if (result.status == cathode_primitives::ExecuteStatus::Queued && result.queued) {
        gQueuedRemoteActions.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return {ExecuteStatus::Queued, true};
    }
    ClearDoorRemoteAction(targetEntity, payload.action);
    if (result.status == cathode_primitives::ExecuteStatus::RuntimeUnavailable) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (result.status == cathode_primitives::ExecuteStatus::NativeFault) {
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    }
    gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
    return {ExecuteStatus::InvalidPayload, false};
}

ExecuteResult ExecuteDoorLockState(
    const std::uint32_t semanticInput,
    const WorldEntityId& sourceEntity,
    const WorldEntityLease& source,
    const WorldEntityId& targetEntity,
    const WorldEntityLease& target) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    if (!Initialized() || !source || !target || !sourceEntity.Valid() ||
        !targetEntity.Valid() || sourceEntity == targetEntity ||
        !IsDoorLockStateInput(semanticInput)) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }

    const auto expectedVtable = gDoorNativeVtable.load(std::memory_order_acquire);
    auto* outer = static_cast<std::byte*>(target.Get());
    if (expectedVtable == nullptr || outer == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    auto* nativeDoor = outer + kDoorNativeOffset;
    const auto liveVtable = crash_guard::SafeRead<const void*>(nativeDoor);
    if (!liveVtable.has_value() || *liveVtable != expectedVtable) {
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::InvalidPayload, false};
    }

    const auto action = semanticInput == kDoorRequestLockInput
        ? DoorAction::Lock : DoorAction::Unlock;
    RememberDoorRemoteAction(targetEntity, action);
    const auto result = cathode_primitives::ExecuteInputEdge(
        semanticInput, semanticInput, 0.0F, target, source);
    if (result.status == cathode_primitives::ExecuteStatus::Queued && result.queued) {
        gQueuedRemoteActions.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return {ExecuteStatus::Queued, true};
    }
    ClearDoorRemoteAction(targetEntity, action);
    if (result.status == cathode_primitives::ExecuteStatus::RuntimeUnavailable) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (result.status == cathode_primitives::ExecuteStatus::NativeFault) {
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    }
    gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
    return {ExecuteStatus::InvalidPayload, false};
}

ExecuteResult ExecuteAnimatedInputEdge(
    const AnimatedInputEdge& edge,
    const WorldEntityId& targetEntity,
    const WorldEntityLease& target,
    const WorldEntityLease& source) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    if (!Initialized() || !target || !source || !targetEntity.Valid()) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (ClassifyAnimatedTarget(target.Get()) != edge.targetCategory) {
        gAnimatedTargetClassMismatch.fetch_add(1U, std::memory_order_acq_rel);
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return {ExecuteStatus::InvalidPayload, false};
    }
    const auto result = cathode_primitives::ExecuteInputEdge(
        edge.schedulerParameterShortGuid,
        edge.targetInputShortGuid,
        edge.relativeDelaySeconds,
        target,
        source);
    if (result.status == cathode_primitives::ExecuteStatus::Queued && result.queued) {
        gQueuedRemoteAnimatedEdges.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return {ExecuteStatus::Queued, true};
    }
    if (result.status == cathode_primitives::ExecuteStatus::RuntimeUnavailable) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (result.status == cathode_primitives::ExecuteStatus::NativeFault) {
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    }
    gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
    return {ExecuteStatus::InvalidPayload, false};
}

void ResetForGeneration() noexcept {
    ClearAllTransient();
    TouchVersion();
}

void BeginLogicalShutdown() noexcept {
    if (!gInitialized.exchange(false, std::memory_order_acq_rel)) return;
    gRegistry.store(nullptr, std::memory_order_release);
    gResolver = nullptr;
    gHooks = nullptr;
    ClearAllTransient();
    gDoorHooksInstalled.store(false, std::memory_order_release);
    gDoorUnlockProbeInstalled.store(false, std::memory_order_release);
    gDoorSemanticProbeInstalled.store(false, std::memory_order_release);
    gDoorNativeVtable.store(nullptr, std::memory_order_release);
    gShutdownCount.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
}

} // namespace aimp::world_animated
