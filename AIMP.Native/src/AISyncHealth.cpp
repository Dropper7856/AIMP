#include "aimp/AISyncHealth.h"
#include "aimp/HookManager.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <mutex>

namespace aimp::ai_sync::health {
namespace {
constexpr std::uintptr_t kHealth = 0x320U;
constexpr std::uintptr_t kCommit = 0x003B4AD0U;
constexpr std::uintptr_t kDamage = 0x003CC900U, kHealthDamage = 0x003CAAC0U;
std::uintptr_t gBase{};
std::atomic<std::uint64_t> gCaptured{}, gCaptureRefused{}, gCommits{}, gApplied{}, gAlreadyMatched{};
std::atomic<std::uint64_t> gDeferred{}, gFailed{}, gRevivalRefused{}, gLifetimeRejected{};
std::atomic<std::uint64_t> gDamageCaptured{}, gDamageDropped{}, gDamageReplayed{}, gDamageRejected{};
std::atomic<std::uint64_t> gCommitsResolved{};
std::atomic<std::uint64_t> gNativeDamageBlocked{};
std::atomic<NativeDamageGuard> gNativeDamageGuard{};
std::atomic<PairedDamagePredicate> gPairedDamagePredicate{};
std::atomic<bool> gObserveHost{};
std::atomic<std::uint64_t> gObservationEpoch{1U};
void* gDamageTrampoline{};
std::atomic<void*> gDamageOriginal{};
struct QueuedDamage final { DamageObservation observation{}; std::uint64_t epoch{}; };
std::array<QueuedDamage, 256U> gDamageQueue{};
std::mutex gDamageMutex;
std::uint32_t gDamageRead{}, gDamageWrite{};
struct ReplayScope final {
    std::uintptr_t character{};
    std::uint32_t runtimeId{}, hp{};
    bool entered{};
};
thread_local ReplayScope* gReplay{};
thread_local unsigned gDamageDepth{};
// Both routines consume exactly nine x86 stack words and RET 24h. The first
// uses ECX=Character; the common receiver uses ECX=Character+320.
using NativeDamage = void(__thiscall*)(void*, std::uint32_t, std::uintptr_t, std::uint32_t,
    std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);

template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    if (address < 0x10000U || address > 0xFFFF0000U) return false;
    __try { value = *reinterpret_cast<const T*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Match(const std::uintptr_t rva, const std::initializer_list<unsigned char> bytes) noexcept {
    std::size_t offset{};
    for (const auto expected : bytes) {
        unsigned char actual{};
        if (!Read(gBase + rva + offset++, actual) || actual != expected) return false;
    }
    return true;
}
bool ReadState(const std::uintptr_t character, State& state) noexcept {
    state = {};
    std::uintptr_t owner{};
    std::uint16_t hp{};
    if (character == 0U || !Read(character + kHealth + 0x24U, owner) || owner != character ||
        !Read(character + kHealth + 0x1CU, hp) ||
        !Read(character + kHealth + 0xCU, state.healthState)) return false;
    state.revision = 1U;
    state.flags = Present;
    state.hp = hp;
    return Valid(state);
}
bool Live(const std::uintptr_t character, const std::uint32_t expectedRuntimeId) noexcept {
    std::uint32_t runtimeId{};
    return expectedRuntimeId != 0U && Read(character + 0x50U, runtimeId) && runtimeId == expectedRuntimeId;
}
void QueueDamage(const DamageObservation& observation, const std::uint64_t epoch) noexcept {
    if (!gObserveHost.load(std::memory_order_acquire) ||
        epoch != gObservationEpoch.load(std::memory_order_acquire)) return;
    std::unique_lock lock(gDamageMutex, std::try_to_lock);
    if (!lock.owns_lock() || gDamageWrite - gDamageRead >= gDamageQueue.size()) {
        gDamageDropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (!gObserveHost.load(std::memory_order_acquire) ||
        epoch != gObservationEpoch.load(std::memory_order_acquire)) return;
    gDamageQueue[gDamageWrite++ % gDamageQueue.size()] = {observation, epoch};
    gDamageCaptured.fetch_add(1U, std::memory_order_relaxed);
}
void __fastcall HealthDamageDetour(void* context, void*, const std::uint32_t damage,
    const std::uintptr_t source, const std::uint32_t flag3, const std::uint32_t flag4,
    const std::uint32_t flag5, const std::uint32_t flag6, const std::uint32_t descriptor,
    const std::uint32_t flag8, const std::uint32_t flag9) noexcept {
    const auto original = reinterpret_cast<NativeDamage>(gDamageOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    DamageObservation observation{};
    State before{};
    const auto address = reinterpret_cast<std::uintptr_t>(context);
    const auto epoch = gObservationEpoch.load(std::memory_order_acquire);
    const bool owned = Read(address + 0x24U, observation.character) &&
        observation.character != 0U && address == observation.character + kHealth;
    const bool replay = gReplay != nullptr && address == gReplay->character + kHealth &&
        Live(gReplay->character, gReplay->runtimeId);
    if (replay) gReplay->entered = true;
    const auto guard = gNativeDamageGuard.load(std::memory_order_acquire);
    if (!replay && owned && guard != nullptr && !guard(observation.character, source)) {
        gNativeDamageBlocked.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const auto paired = gPairedDamagePredicate.load(std::memory_order_acquire);
    const bool capture = !replay && gReplay == nullptr &&
        (paired == nullptr || !paired(observation.character, source)) && gObserveHost.load(std::memory_order_acquire) &&
        owned &&
        Read(observation.character + 0x50U, observation.runtimeId) && observation.runtimeId != 0U &&
        ReadState(observation.character, before);
    if (capture && source != 0U) {
        std::uint32_t sourceId{};
        if (Read(source + 0x50U, sourceId) && sourceId != 0U) observation.source = {source, sourceId};
    }
    ++gDamageDepth;
    __try { original(context, damage, source, flag3, flag4, flag5, flag6, descriptor, flag8, flag9); }
    __finally { --gDamageDepth; }
    if (!capture || !Live(observation.character, observation.runtimeId) ||
        !ReadState(observation.character, observation.event.result)) return;
    auto& event = observation.event;
    event.revision = 1U; event.flags = Present;
    event.damage = damage; event.descriptor = descriptor;
    // The native callee reads these stack words as bytes. Their upper bits
    // are outside the ABI value and may contain caller register residue.
    event.flag3 = flag3 & 0xFFU; event.flag4 = flag4 & 0xFFU;
    event.flag5 = flag5 & 0xFFU; event.flag6 = flag6 & 0xFFU;
    event.flag8 = flag8 & 0xFFU; event.flag9 = flag9 & 0xFFU;
    event.hpBefore = before.hp; event.healthStateBefore = before.healthState;
    if (Valid(event)) QueueDamage(observation, epoch);
    else gDamageDropped.fetch_add(1U, std::memory_order_relaxed);
}
bool ReplayDamage(const std::uintptr_t character, const std::uint32_t runtimeId,
    const DamageEvent& event, const Source source, bool& entered) noexcept {
    ReplayScope scope{character, runtimeId, event.result.hp, false};
    auto* previous = gReplay;
    gReplay = &scope;
    bool success{};
    __try {
        reinterpret_cast<NativeDamage>(gBase + kDamage)(reinterpret_cast<void*>(character),
            event.damage, source.character, event.flag3, event.flag4, event.flag5,
            event.flag6, event.descriptor, event.flag8, event.flag9);
        success = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { success = false; }
    entered = scope.entered;
    gReplay = previous;
    return success;
}
bool Commit(const std::uintptr_t character, const std::uint32_t hp, const Source source) noexcept {
    // 007B4AD0: ECX=Character+320, two stack words, RET 8. Its short HP
    // argument occupies a full x86 stack slot. The installed AISync health
    // hook retains this entry's original ABI and supplies the retail call.
    using NativeCommit = void(__thiscall*)(void*, std::uint32_t, std::uintptr_t);
    std::uint32_t runtimeId{};
    if (!Read(character + 0x50U, runtimeId) || runtimeId == 0U) return false;
    ReplayScope scope{character, runtimeId, hp, false};
    auto* previous = gReplay;
    gReplay = &scope;
    bool success{};
    __try {
        reinterpret_cast<NativeCommit>(gBase + kCommit)(
            reinterpret_cast<void*>(character + kHealth), hp, source.character);
        success = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { success = false; }
    gReplay = previous;
    return success;
}
ApplyResult Fail() noexcept {
    gFailed.fetch_add(1U, std::memory_order_relaxed);
    return ApplyResult::Failed;
}
ApplyResult Defer() noexcept {
    gDeferred.fetch_add(1U, std::memory_order_relaxed);
    return ApplyResult::Deferred;
}
} // namespace

bool Initialize(const std::uintptr_t base, std::string& error) {
    gBase = base;
    // Validate before AISync installs its existing health-commit detour.
    // Commit invokes 007AD730 after changing HP; that function owns native
    // state 4 and the scheduler death request. No health enum is written here.
    if (base == 0U ||
        !Match(kCommit, {0x56,0x8B,0xF1,0x0F,0xB7,0x46,0x1C,0x66,0x85,0xC0,0x74,0x62}) ||
        !Match(0x003B4B3EU, {0x5E,0xC2,0x08,0x00}) ||
        !Match(kDamage, {0x56,0x8B,0xF1,0x8B,0x46,0x64}) ||
        !Match(0x003CC952U, {0xC2,0x24,0x00}) ||
        !Match(kHealthDamage, {0x64,0xA1,0x00,0x00,0x00,0x00}) ||
        !Match(0x003C451EU, {0x80,0x7C,0x24,0x68,0x00}) ||
        !Match(0x003CAB7EU, {0x38,0x44,0x24,0x34}) ||
        !Match(0x003CAB04U, {0x8B,0x5C,0x24,0x3C,0x84,0xDB}) ||
        !Match(0x003CAD02U, {0xC2,0x24,0x00})) {
        gBase = 0U;
        error = "AISync native health commit ABI mismatch";
        return false;
    }
    return true;
}
bool InstallDamageObserver(HookManager& hooks, std::string& error) {
    if (gBase == 0U) { error = "AISync health must be initialized before installing damage observation"; return false; }
    HookRequest request;
    request.name = "aisync_native_npc_health_damage";
    request.target = reinterpret_cast<void*>(gBase + kHealthDamage);
    request.detour = reinterpret_cast<void*>(&HealthDamageDetour);
    request.patchLength = 6U;
    request.expected.bytes = {0x64U,0xA1U,0U,0U,0U,0U};
    request.expected.mask = "xxxxxx";
    request.relocationSafePrefix = true;
    request.trampolineOut = &gDamageTrampoline;
    request.publishedTrampolineOut = &gDamageOriginal;
    return hooks.Install(request, error);
}
void SetNativeDamageGuard(const NativeDamageGuard guard) noexcept {
    gNativeDamageGuard.store(guard, std::memory_order_release);
}
void SetPairedDamagePredicate(const PairedDamagePredicate predicate) noexcept {
    gPairedDamagePredicate.store(predicate, std::memory_order_release);
}
void SetObservingHost(const bool enabled) noexcept {
    if (gObserveHost.exchange(enabled, std::memory_order_acq_rel) != enabled)
        ResetObservations();
}
void ResetObservations() noexcept {
    gObservationEpoch.fetch_add(1U, std::memory_order_acq_rel);
    // Epoch invalidation does not wait for a worker. The consumer discards
    // queued entries from an earlier epoch before returning any observation.
}
bool TryPopDamageObservation(DamageObservation& observation) noexcept {
    std::unique_lock lock(gDamageMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const auto epoch = gObservationEpoch.load(std::memory_order_acquire);
    while (gDamageRead != gDamageWrite) {
        const auto queued = gDamageQueue[gDamageRead++ % gDamageQueue.size()];
        if (queued.epoch != epoch || !gObserveHost.load(std::memory_order_acquire)) continue;
        observation = queued.observation;
        return true;
    }
    return false;
}
void ObserveCommit(const std::uintptr_t character, const std::uint32_t runtimeId,
    const Source source, const State& before) noexcept {
    if (gDamageDepth != 0U || gReplay != nullptr || !gObserveHost.load(std::memory_order_acquire) ||
        !Valid(before) || before.flags != Present || !Live(character, runtimeId)) return;
    const auto paired = gPairedDamagePredicate.load(std::memory_order_acquire);
    if (paired != nullptr && paired(character, source.character)) return;
    const auto epoch = gObservationEpoch.load(std::memory_order_acquire);
    DamageObservation observation{};
    observation.character = character; observation.runtimeId = runtimeId; observation.source = source;
    if (!ReadState(character, observation.event.result) || !Live(character, runtimeId) ||
        SameValue(before, observation.event.result)) return;
    auto& event = observation.event;
    event.revision = 1U; event.flags = Present | CommitOnly;
    event.hpBefore = before.hp; event.healthStateBefore = before.healthState;
    if (Valid(event)) QueueDamage(observation, epoch);
}
bool Capture(const std::uintptr_t character, State& observation) noexcept {
    if (gBase == 0U || !ReadState(character, observation)) {
        observation = {};
        gCaptureRefused.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    gCaptured.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
bool CaptureDeathSource(const std::uintptr_t character, Source& source) noexcept {
    source = {};
    State state{};
    std::uintptr_t schedulerOwner{}, request{}, requestOwner{}, killer{};
    std::uint32_t kind{}, recordedRuntimeId{}, currentRuntimeId{};
    // 007AD730 stores the killer at request+24/+28 only on the scheduler's
    // kind-10 death request. Both scheduler ownership and source incarnation
    // are revalidated; a stale source never becomes a network identity.
    if (gBase == 0U || !ReadState(character, state) || !Terminal(state) ||
        !Read(character + 0x2CD0U, schedulerOwner) || schedulerOwner != character ||
        !Read(character + 0x2FF4U, request) || request == 0U ||
        !Read(request + 0x10U, kind) || kind != 10U ||
        !Read(request + 0x14U, requestOwner) || requestOwner != character + 0x2CD0U ||
        !Read(request + 0x24U, killer) || killer == 0U ||
        !Read(request + 0x28U, recordedRuntimeId) || recordedRuntimeId == 0U ||
        !Read(killer + 0x50U, currentRuntimeId) || currentRuntimeId != recordedRuntimeId) return false;
    source.character = killer;
    source.runtimeId = currentRuntimeId;
    return true;
}
ApplyResult Apply(const std::uintptr_t character, const std::uint32_t expectedRuntimeId,
    const State& authority, const Source source) noexcept {
    if (gBase == 0U || !Valid(authority)) return Fail();
    if (authority.flags == 0U) return ApplyResult::AlreadyMatched;
    if (!Live(character, expectedRuntimeId)) {
        gLifetimeRejected.fetch_add(1U, std::memory_order_relaxed);
        return Fail();
    }
    State current{};
    if (!ReadState(character, current)) return Defer();
    if (SameValue(current, authority)) {
        gAlreadyMatched.fetch_add(1U, std::memory_order_relaxed);
        return ApplyResult::AlreadyMatched;
    }
    if (current.hp == 0U) {
        // Retail commit deliberately cannot resurrect a terminal Character.
        // Do not acknowledge the Host's living state against this incarnation.
        gRevivalRefused.fetch_add(1U, std::memory_order_relaxed);
        return Fail();
    }
    if (source.character != 0U && !Live(source.character, source.runtimeId)) {
        gLifetimeRejected.fetch_add(1U, std::memory_order_relaxed);
        return Defer();
    }
    if (source.character == 0U && source.runtimeId != 0U) return Fail();
    // An unchanged HP with a different intermediate health phase may be owned
    // by another native transition (notably phase 3). Commit would do nothing;
    // wait for native execution instead of fabricating that enum transition.
    if (current.hp == authority.hp) return Defer();
    if (!Commit(character, authority.hp, source)) return Fail();
    gCommits.fetch_add(1U, std::memory_order_relaxed);
    if (!Live(character, expectedRuntimeId)) {
        gLifetimeRejected.fetch_add(1U, std::memory_order_relaxed);
        return Fail();
    }
    State observed{};
    if (!ReadState(character, observed) || !SameValue(observed, authority)) return Defer();
    gApplied.fetch_add(1U, std::memory_order_relaxed);
    return ApplyResult::Applied;
}
bool ResolveCommit(const std::uintptr_t healthContext, std::uint32_t& hp) noexcept {
    if (gReplay == nullptr || healthContext != gReplay->character + kHealth ||
        !Live(gReplay->character, gReplay->runtimeId)) return false;
    hp = gReplay->hp;
    gCommitsResolved.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
ApplyResult ApplyDamageEvent(const std::uintptr_t character, const std::uint32_t expectedRuntimeId,
    const DamageEvent& event, const Source source, DamageReplayCursor& cursor) noexcept {
    if (gBase == 0U || !Valid(event) || event.flags == 0U || !Live(character, expectedRuntimeId)) {
        gDamageRejected.fetch_add(1U, std::memory_order_relaxed);
        return Fail();
    }
    if (cursor.busy) return Defer();
    if (cursor.character != 0U && (cursor.character != character || cursor.runtimeId != expectedRuntimeId)) {
        gLifetimeRejected.fetch_add(1U, std::memory_order_relaxed);
        return Fail();
    }
    struct Guard final { bool& busy; explicit Guard(bool& value) noexcept : busy(value) { busy = true; } ~Guard() { busy = false; } } guard(cursor.busy);
    if (event.revision < cursor.revision) return ApplyResult::AlreadyMatched;
    if (event.revision == cursor.revision && cursor.invoked) {
        if (std::memcmp(&cursor.admitted, &event, sizeof(event)) != 0 ||
            cursor.source.character != source.character || cursor.source.runtimeId != source.runtimeId) return Fail();
        return Apply(character, expectedRuntimeId, event.result, source);
    }
    const auto entered = [&]() noexcept {
        cursor.character = character; cursor.runtimeId = expectedRuntimeId;
        cursor.source = source; cursor.admitted = event;
        cursor.revision = event.revision; cursor.invoked = true;
    };
    if ((event.flags & CommitOnly) != 0U) {
        entered();
        return Apply(character, expectedRuntimeId, event.result, source);
    }
    State before{};
    if (!ReadState(character, before)) return Defer();
    if (source.character != 0U && !Live(source.character, source.runtimeId)) return Defer();
    if (source.character == 0U && source.runtimeId != 0U) return Fail();
    if (before.hp == 0U) {
        if (!Terminal(event.result)) {
            gRevivalRefused.fetch_add(1U, std::memory_order_relaxed);
            return Fail();
        }
        entered();
        return Apply(character, expectedRuntimeId, event.result, source);
    }
    // The caller can converge this observed precondition with ApplySnapshot.
    // Do not infer a native pre-hit phase from HP alone.
    if (before.hp != event.hpBefore || before.healthState != event.healthStateBefore) return Defer();
    entered();
    bool nativeEntered{};
    if (!ReplayDamage(character, expectedRuntimeId, event, source, nativeEntered) || !nativeEntered) {
        gDamageRejected.fetch_add(1U, std::memory_order_relaxed);
        return Fail();
    }
    gDamageReplayed.fetch_add(1U, std::memory_order_relaxed);
    // Any later retry of this exact event can only converge its absolute
    // result. It cannot repeat the native hit, even after a partial failure.
    return Apply(character, expectedRuntimeId, event.result, source);
}
Counters Snapshot() noexcept {
    return {gCaptured.load(std::memory_order_relaxed), gCaptureRefused.load(std::memory_order_relaxed),
        gCommits.load(std::memory_order_relaxed), gApplied.load(std::memory_order_relaxed),
        gAlreadyMatched.load(std::memory_order_relaxed), gDeferred.load(std::memory_order_relaxed),
        gFailed.load(std::memory_order_relaxed), gRevivalRefused.load(std::memory_order_relaxed),
        gLifetimeRejected.load(std::memory_order_relaxed), gDamageCaptured.load(std::memory_order_relaxed),
        gDamageDropped.load(std::memory_order_relaxed), gDamageReplayed.load(std::memory_order_relaxed),
        gDamageRejected.load(std::memory_order_relaxed), gCommitsResolved.load(std::memory_order_relaxed),
        gNativeDamageBlocked.load(std::memory_order_relaxed)};
}

} // namespace aimp::ai_sync::health
