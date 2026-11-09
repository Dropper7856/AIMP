#include "aimp/AnimationSyncRuntime.h"
#include "aimp/AnimationSyncTimeline.h"
#include "aimp/AnimationSyncPropsRuntime.h"
#include "aimp/AISyncNativeIntent.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CharacterResourceAdmission.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/PlayerHidingPresentationRuntime.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>

// Same native contract as PlayerHidingPresentationRuntime (see
// analysis/hiding-presentation/native-player-contract.md): CMD_PlayAnimation
// entities (0067FC40) construct the target Character's +36C cycle through
// 006579F0 -> 006568B0, which copies 22 parameter words to cycle+44. The
// owner's cycle therefore already names the clip alias (cycle+6C) and its
// timeline; the observer replays that alias on its own representation.
namespace aimp::animation_sync {
namespace {
constexpr std::uintptr_t kScriptedType = 0xEADB58U;
constexpr std::uint8_t kScriptedState = 0x12U;
// Timeouts concern observations, never the legitimate duration of an action.
constexpr std::uint64_t kObservationTimeoutMs = 5000U;
constexpr std::uint64_t kMaximumStartAgeMs = 1500U;
// After the owner's clip ends, a one-shot keeps playing for the network delay.
constexpr std::uint64_t kClearGraceMs = 750U;
constexpr std::size_t kSlots = 40U;

struct Cycle final {
    std::uintptr_t address{}, type{}, owner{}, graph{};
    std::uint32_t runtime{}, resource{}, animation{};
    std::uint8_t state{}, serial{}, flags{};
};
struct RemoteDiagnostics final {
    std::uint64_t applied{}, failed{}, stale{}, unavailable{}, interrupted{}, completed{}, cancelled{}, deferred{};
    std::uint64_t transition{};
    std::uint32_t animation{}, resource{}, clipPath{};
    const char* stage{"pending"};
};
struct Slot final {
    std::uintptr_t character{};
    State desired{};
    std::uint64_t binding{}, epoch{}, highest{}, handled{}, receivedAt{}, clearedAt{}, startedAt{};
    std::int32_t ownedLoops{};
    Cycle owned{};
    RemoteDiagnostics diagnostics{};
};
struct LocalCapture final {
    std::uintptr_t character{}, cycle{};
    std::uint32_t runtime{}, animation{};
    std::uint8_t serial{};
    std::uint64_t startedAt{};
    State state{};
};
struct LocalDiagnostics final {
    std::uint64_t captured{}, cleared{}, hidingSkipped{}, torn{}, unavailable{};
    std::uint32_t animation{}, resource{};
};
struct Stream final {
    std::uintptr_t character{};
    std::uint64_t binding{}, transition{};
    std::uint32_t animation{};
    std::unique_ptr<native_resource::StrongLease> database;
    bool acquired{};
};

std::atomic<std::uintptr_t> gBase{};
std::atomic<std::uintptr_t> gIdleTransition{};
std::mutex gMutex, gLocalMutex;
std::array<Slot, kSlots> gSlots{};
std::uint64_t gEpoch{};
// Engine thread only.
LocalCapture gLocal{};
std::uint64_t gLocalTransition{};
// gLocalMutex.
LocalSnapshot gLocalSnapshot{};
LocalDiagnostics gLocalDiagnostics{};
// Engine-thread owned, process lifetime: see PlayerHidingPresentationRuntime.
// StrongLease destructors must never run on the loader thread.
auto& gStreams = *new std::array<Stream, kSlots>{};
// Engine-thread list: value-only Forget may run on IPC/lifecycle threads;
// native prop references are retired on the next natural engine pump.
std::array<std::uintptr_t, kSlots> gPropCharacters{};

template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    return address >= 0x10000U && crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(value));
}
bool Match(const std::uintptr_t rva, const std::initializer_list<std::uint8_t> bytes) noexcept {
    std::array<std::uint8_t, 64U> actual{};
    return bytes.size() <= actual.size() &&
        crash_guard::TryCopy(actual.data(), reinterpret_cast<const void*>(gBase.load() + rva), bytes.size()) &&
        std::memcmp(actual.data(), bytes.begin(), bytes.size()) == 0;
}
// Identical certification to the hiding presentation's constructor ABI.
bool Abi() noexcept {
    const auto base = gBase.load();
    std::uintptr_t update{}, initialize{}, marker{};
    return base != 0U && Read(base + kScriptedType + 0x20U, update) && update == base + 0x256BE0U &&
        Read(base + kScriptedType + 0x60U, initialize) && initialize == base + 0x26C4C0U &&
        Read(base + kScriptedType + 4U, marker) && marker == base + 0x24F290U &&
        Match(0x257A13U, {0x8B,0x06,0x8B,0x10,0x6A,0x01,0x8B,0xCE,0xFF,0xD2,
            0x8B,0x44,0x24,0x0C,0x8B,0x4C,0x24,0x08,0x50,0x51,0x8B,0xCE,
            0xE8,0x82,0xEE,0xFF,0xFF,0x8B,0x16,0x8B,0x42,0x60,0x8B,0xCE,0xFF,0xD0,
            0x5E,0xC2,0x08,0x00}) &&
        Match(0x256914U, {0x8D,0x78,0x44,0xB9,0x16,0x00,0x00,0x00}) &&
        Match(0x256936U, {0xF3,0xA5,0x5F,0x89,0x90,0x9C,0x00,0x00,0x00,0x5E,0xC2,0x08,0x00});
}
bool StreamAbi() noexcept {
    for (const auto rva : {0x639060U, 0x636970U, 0x636BB0U})
        if (!Match(rva, {0x55,0x8B,0xEC,0x83,0xEC,0x10,0x56,0x57}) ||
            !Match(rva + (rva == 0x636970U ? 0x14CU : 0x13AU), {0xC2,0x08,0x00})) return false;
    return true;
}
bool ReadCycle(const std::uintptr_t character, Cycle& cycle) noexcept {
    cycle = {};
    if (!Read(character + 0x50U, cycle.runtime) || cycle.runtime == 0U ||
        !Read(character + 0x36CU, cycle.address) || !Read(cycle.address, cycle.type) ||
        !Read(cycle.address + 4U, cycle.owner) || cycle.owner != character ||
        !Read(cycle.address + 8U, cycle.flags) || !Read(cycle.address + 0x19U, cycle.serial) ||
        !Read(cycle.address + 0x1AU, cycle.state)) return false;
    if (cycle.type == gBase.load() + kScriptedType && cycle.state == kScriptedState)
        return Read(cycle.address + 0x14U, cycle.graph) && Read(cycle.address + 0x68U, cycle.resource) &&
            Read(cycle.address + 0x6CU, cycle.animation);
    return true;
}
bool Scripted(const Cycle& cycle) noexcept {
    return cycle.type == gBase.load() + kScriptedType && cycle.state == kScriptedState &&
        cycle.graph != 0U && cycle.resource != 0U && cycle.animation != 0U && (cycle.flags & 6U) == 0U;
}
bool SameCycle(const Cycle& a, const Cycle& b) noexcept {
    return a.address == b.address && a.type == b.type && a.owner == b.owner &&
        a.runtime == b.runtime && a.state == b.state && a.serial == b.serial;
}
bool Owned(const Cycle& current, const Cycle& owned) noexcept {
    return owned.address != 0U && SameCycle(current, owned) && (owned.graph == 0U || current.graph == owned.graph) &&
        current.resource == owned.resource && current.animation == owned.animation;
}
bool IsLocal(const std::uintptr_t character, bool& local) noexcept {
    local = false;
    std::uintptr_t root{}, context{}, players{};
    std::uint32_t count{};
    if (!Read(gBase.load() + 0x12F0C88U, root) || !Read(root + 8U, context) ||
        !Read(context + 0x80U, players) || !Read(context + 0x84U, count) ||
        count == 0U || count > 4U || players < 0x10000U) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t player{};
        if (!Read(players + i * 4U, player)) return false;
        local = local || player == character;
    }
    return true;
}
bool RemoteReady(const std::uintptr_t character) noexcept {
    bool local{};
    std::uint32_t health{};
    std::uint8_t grappled{}, vent{};
    return IsLocal(character, local) && !local && Read(character + 0x32CU, health) && health < 3U &&
        Read(character + 0x297U, grappled) && grappled == 0U &&
        Read(character + 0x2B4U, vent) && vent == 0U;
}
// Retail 00A35FF0 base-table search in the representation's own clip DB.
bool LocalClip(const std::uintptr_t character, const std::uint32_t token,
    std::uint32_t& resource, std::uint32_t& path) noexcept {
    std::uintptr_t instance{}, database{}, table{}, keys{}, values{};
    std::uint32_t count{};
    if (!Read(character + 0x720U, instance) || !Read(instance + 0x10U, database) ||
        !Read(database + 0x10U, resource) || resource == 0U ||
        !Read(database + 0x20U, table) || !Read(table + 8U, keys) ||
        !Read(table + 12U, values) || !Read(table + 16U, count) || count > 100000U) return false;
    std::uint32_t first{}, last{count};
    while (first < last) {
        const auto middle = first + (last - first) / 2U;
        std::uint32_t key{}, index{};
        if (!Read(keys + middle * 8U, key)) return false;
        if (key > token) first = middle + 1U;
        else if (key < token) last = middle;
        else return Read(keys + middle * 8U + 4U, index) && index < count &&
            Read(values + index * 8U, path) && path != 0U;
    }
    return false;
}

// --- Streamed clip requests (public acquire/query/release, RET8) -------------
bool ClipOperation(const std::uintptr_t rva, const std::uintptr_t database, const std::uint32_t animation) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    const std::uint32_t alias[3]{0U, 0U, animation};
    const std::uint32_t variant[3]{0U, 0U, 0xADA7AFDBU}; // FNV-1a("none"), as retail
    __try {
        using Operation = std::uint8_t(__thiscall*)(void*, const std::uint32_t*, const std::uint32_t*);
        return reinterpret_cast<Operation>(gBase.load() + rva)(reinterpret_cast<void*>(database), alias, variant) != 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(rva); static_cast<void>(database); static_cast<void>(animation); return false;
#endif
}
Slot* Find(const std::uintptr_t character) noexcept {
    for (auto& slot : gSlots) if (slot.character == character) return &slot;
    return nullptr;
}
void ReleaseStream(Stream& stream) noexcept {
    if (stream.acquired && stream.database)
        static_cast<void>(ClipOperation(0x636BB0U, stream.database->Object(), stream.animation));
    stream.acquired = false;
    stream.database.reset();
    stream.character = 0U;
}
void CollectStreams() noexcept {
    for (auto& stream : gStreams) {
        if (stream.character == 0U) continue;
        bool keep{};
        {
            const std::lock_guard lock(gMutex);
            if (const auto* slot = Find(stream.character); slot != nullptr && slot->binding == stream.binding)
                keep = (slot->owned.address != 0U && slot->owned.animation == stream.animation) ||
                    (slot->desired.transition == stream.transition && slot->handled != stream.transition);
        }
        if (!keep) ReleaseStream(stream);
    }
}
enum class StreamReadiness { Failed, Waiting, Ready };
StreamReadiness PrepareStream(const Slot& slot, const std::uint32_t animation) noexcept {
    if (!StreamAbi()) return StreamReadiness::Failed;
    Stream* stream{};
    for (auto& candidate : gStreams)
        if (candidate.character == slot.character && candidate.binding == slot.binding &&
            candidate.transition == slot.desired.transition && candidate.animation == animation) { stream = &candidate; break; }
    if (stream == nullptr) {
        for (auto& candidate : gStreams) if (candidate.character == 0U) { stream = &candidate; break; }
        if (stream == nullptr) return StreamReadiness::Failed;
        std::uintptr_t instance{}, database{};
        if (!Read(slot.character + 0x720U, instance) || !Read(instance + 0x10U, database)) return StreamReadiness::Failed;
        auto lease = std::make_unique<native_resource::StrongLease>(instance + 0x10U, 0x44U);
        if (!*lease || lease->Object() != database) return StreamReadiness::Failed;
        if (!ClipOperation(0x639060U, database, animation)) return StreamReadiness::Failed;
        stream->character = slot.character; stream->binding = slot.binding;
        stream->transition = slot.desired.transition; stream->animation = animation;
        stream->database = std::move(lease); stream->acquired = true;
    }
    return ClipOperation(0x636970U, stream->database->Object(), animation)
        ? StreamReadiness::Ready : StreamReadiness::Waiting;
}

// --- Native transitions ------------------------------------------------------
bool Invoke(const std::uintptr_t cycle, const ai_sync::native_intent::Payload& intent) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        using Transition = void(__thiscall*)(void*, const std::uint32_t*, std::uint32_t);
        reinterpret_cast<Transition>(gBase.load() + 0x2579F0U)(
            reinterpret_cast<void*>(cycle), intent.words.data() + 12U, 0U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(cycle); static_cast<void>(intent); return false;
#endif
}
bool InvokeIdle(const std::uintptr_t cycle) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(gIdleTransition.load())(reinterpret_cast<void*>(cycle));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(cycle); return false;
#endif
}
// Touches only our exact constructor result: never a retail replacement,
// paired action, death or a recycled same-address cycle.
bool Cancel(const std::uintptr_t character, const Cycle& owned) noexcept {
    Cycle current{};
    if (!ReadCycle(character, current) || !Owned(current, owned)) return true;
    if (!RemoteReady(character) || gIdleTransition.load() == 0U) return false;
    const character_direct_step_runtime::MovementResourceLease resource(character);
    if (!resource) return false;
    if (!InvokeIdle(current.address)) return false;
    return ReadCycle(character, current) && !Owned(current, owned);
}
// The owner's timeline, but none of its spatial/convergence references,
// external clock, camera or equipment flags: an isolated actor clip.
bool BuildIntent(const State& state, const std::uint32_t resource, ai_sync::native_intent::Payload& output) noexcept {
    if (!Valid(state) || !Active(state) || resource == 0U) return false;
    ai_sync::native_intent::Payload intent{};
    auto& w = intent.words;
    w[0] = 20U; w[21] = resource; w[22] = state.animation;
    w[23] = std::bit_cast<std::uint32_t>(state.startFrame);
    w[24] = std::bit_cast<std::uint32_t>(state.endFrame);
    w[25] = std::bit_cast<std::uint32_t>(state.loops);
    w[26] = std::bit_cast<std::uint32_t>(state.speed);
    w[27] = std::bit_cast<std::uint32_t>(state.blendIn);
    w[30] = std::bit_cast<std::uint32_t>(-1.0F);
    if (!ai_sync::native_intent::Validate(intent)) return false;
    output = intent; return true;
}

void Advance(Slot& slot, const std::uint64_t now) noexcept {
    auto& d = slot.diagnostics;
    Cycle current{};
    if (!RemoteReady(slot.character) || !ReadCycle(slot.character, current)) {
        if (slot.owned.address != 0U) ++d.interrupted;
        slot.owned = {}; d.stage = "actor-unavailable";
        ++d.deferred; // temporary traversal/paired/streaming state can clear
        return;
    }
    if (slot.owned.address != 0U) {
        const bool expired = Active(slot.desired) &&
            now - slot.receivedAt + slot.desired.elapsedMilliseconds > kObservationTimeoutMs;
        const bool cleared = !Active(slot.desired) && slot.clearedAt != 0U &&
            (slot.ownedLoops != 1 || now - slot.clearedAt > kClearGraceMs);
        if (!Owned(current, slot.owned)) {
            slot.owned = {}; ++d.interrupted; d.stage = "retail-interrupted";
        } else if ((current.flags & 6U) != 0U) {
            if (Cancel(slot.character, slot.owned)) { slot.owned = {}; ++d.completed; d.stage = "completed"; }
        } else if (expired || cleared) {
            if (Cancel(slot.character, slot.owned)) {
                slot.owned = {}; ++d.cancelled; d.stage = expired ? "observation-timeout" : "cancelled";
                if (expired) slot.handled = 0U; // a fresh sample may resume the ongoing action
            }
            else { ++d.deferred; d.stage = "cancel-deferred"; }
        } else if (slot.owned.graph == 0U && current.graph != 0U) {
            slot.owned.graph = current.graph; d.stage = "playing";
        }
    }
    if (!Active(slot.desired) || slot.handled == slot.desired.transition) return;
    d.transition = slot.desired.transition; d.animation = slot.desired.animation;
    const auto age = static_cast<std::uint64_t>(slot.desired.elapsedMilliseconds) + now - slot.receivedAt;
    if (age > kMaximumStartAgeMs) {
        ++d.stale; d.stage = "stale-observation";
        // A later, fresh phase sample may still join this ongoing action.
        return;
    }
    // Lockers keep priority, but do not consume an interaction that can still
    // start from a subsequent fresh phase once their presentation releases.
    if (player_hiding_presentation::Active(slot.character)) { ++d.deferred; d.stage = "hiding-priority"; return; }
    if (!Abi() || gIdleTransition.load() == 0U) {
        slot.handled = slot.desired.transition; ++d.failed; d.stage = "abi"; return;
    }
    // Replacing our own clip is allowed; other scripts, physical actions,
    // deaths and paired attacks keep priority over a presentation.
    if (!Owned(current, slot.owned) && current.state != 1U && current.state != 3U && current.state != 0x38U) {
        ++d.deferred; d.stage = "cycle-busy"; return;
    }
    if (!LocalClip(slot.character, slot.desired.animation, d.resource, d.clipPath)) {
        slot.handled = slot.desired.transition; ++d.unavailable; d.stage = "clip-unavailable"; return;
    }
    const character_direct_step_runtime::MovementResourceLease resources(slot.character);
    if (!resources) { ++d.deferred; d.stage = "resources"; return; }
    const auto readiness = PrepareStream(slot, slot.desired.animation);
    if (readiness == StreamReadiness::Failed) {
        slot.handled = slot.desired.transition; ++d.failed; d.stage = "stream-request"; return;
    }
    if (readiness == StreamReadiness::Waiting) {
        // Polled again next frame, until ready or the start-age limit.
        ++d.deferred; d.stage = "stream-loading"; return;
    }
    ai_sync::native_intent::Payload intent{};
    auto playback = slot.desired;
    float phase{};
    if (!timeline::Project(playback, age, phase, playback.loops)) {
        slot.handled = slot.desired.transition; ++d.stale; d.stage = "finished-before-admission"; return;
    }
    if (!BuildIntent(playback, d.resource, intent)) {
        slot.handled = slot.desired.transition; ++d.failed; d.stage = "parameters"; return;
    }
    // One placement at the start; afterwards the actor follows the owner's
    // own network path (the owner's Character is the one converging).
    if (!character_direct_step_runtime::ApplyAIRecoveryTransform(slot.character,
        slot.desired.position.data(), slot.desired.yaw)) { ++d.deferred; d.stage = "alignment"; return; }
    const auto before = current;
    if (!ReadCycle(slot.character, current) || !SameCycle(before, current) || !RemoteReady(slot.character)) {
        ++d.deferred; d.stage = "actor-changed"; return;
    }
    slot.handled = slot.desired.transition; // native call is attempted exactly once
    const bool invoked = Invoke(current.address, intent);
    const bool constructed = ReadCycle(slot.character, current) && current.runtime == before.runtime &&
        current.type == gBase.load() + kScriptedType && current.state == kScriptedState &&
        current.serial == static_cast<std::uint8_t>(before.serial + 1U) &&
        current.resource == d.resource && current.animation == slot.desired.animation;
    // Keep provenance even for a synchronous initializer failure (graph=0),
    // so the next pump neutralizes exactly that constructor result.
    if (constructed) { slot.owned = current; slot.startedAt = now; slot.ownedLoops = playback.loops; }
    if (!invoked || !constructed || (current.flags & 4U) != 0U || current.graph == 0U) {
        ++d.failed; d.stage = "native-admission"; return;
    }
    // Project the owner's live native clock through local IPC/network/stream
    // waiting time. Seek keeps the original trim interval for future repeats;
    // shifting startFrame would incorrectly shorten every subsequent loop.
    if (!timeline::Seek(current.graph, phase)) {
        if (Cancel(slot.character, slot.owned)) slot.owned = {};
        ++d.failed; d.stage = "phase-seek"; return;
    }
    ++d.applied; d.stage = "playing";
}
bool PumpOne(const std::uintptr_t character) noexcept {
    Slot copy{};
    { const std::lock_guard lock(gMutex); const auto* slot = Find(character); if (slot == nullptr) return false; copy = *slot; }
    Advance(copy, GetTickCount64());
    const std::lock_guard lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr || slot->binding != copy.binding) return false;
    if (slot->epoch == copy.epoch) { *slot = copy; return true; }
    // A packet arrived during native playback: keep its desired state and
    // retain the provenance of what this pump actually did.
    slot->owned = copy.owned; slot->startedAt = copy.startedAt; slot->ownedLoops = copy.ownedLoops;
    slot->diagnostics = copy.diagnostics;
    if (slot->desired.transition == copy.desired.transition) slot->handled = copy.handled;
    return true;
}

// --- Owner capture -----------------------------------------------------------
float WrapYaw(const float yaw) noexcept {
    return static_cast<float>(std::remainder(yaw, 6.28318530717958647692F));
}
// true + inactive State = no scripted clip; false = torn/unavailable sample.
bool CaptureLocal(const std::uintptr_t character, const std::array<std::uint32_t, 4>& attachments, State& output) noexcept {
    output = {};
    Cycle before{}, after{};
    if (gBase.load() == 0U || !ReadCycle(character, before)) return false;
    const auto now = GetTickCount64();
    const bool hiding = Scripted(before) && player_hiding_presentation::ClassifyClip(before.animation).phase != 0U;
    if (!Scripted(before) || hiding) {
        if (hiding) { const std::lock_guard lock(gLocalMutex); ++gLocalDiagnostics.hidingSkipped; }
        if (gLocal.character == character && Active(gLocal.state)) {
            const std::lock_guard lock(gLocalMutex); ++gLocalDiagnostics.cleared;
        }
        gLocal = {}; gLocal.character = character;
        return true;
    }
    const bool same = gLocal.character == character && gLocal.cycle == before.address &&
        gLocal.serial == before.serial && gLocal.runtime == before.runtime &&
        gLocal.animation == before.animation && Active(gLocal.state);
    // +70/+74 are the original bounds. The completion callback 0064F290
    // decrements +A8, not the constructor's +78 count; use the remaining
    // count when joining a finite multi-loop action after it has started.
    State candidate{};
    std::array<float, 3> position{};
    float yaw{};
    if (!Read(before.address + 0x70U, candidate.startFrame) || !Read(before.address + 0x74U, candidate.endFrame) ||
        !Read(before.address + 0xA8U, candidate.loops) || !Read(before.address + 0x7CU, candidate.speed) ||
        !Read(before.address + 0x80U, candidate.blendIn) ||
        !Read(character + 0x70U, position) || !Read(character + 0x174U, yaw) ||
        !timeline::Read(before.graph, candidate.phaseSeconds, candidate.durationSeconds) ||
        !ReadCycle(character, after) || !SameCycle(before, after) || before.graph != after.graph ||
        before.resource != after.resource || before.animation != after.animation || before.flags != after.flags) {
        const std::lock_guard lock(gLocalMutex); ++gLocalDiagnostics.torn;
        return false;
    }
    candidate.transition = same ? gLocal.state.transition : ++gLocalTransition;
    candidate.animation = before.animation;
    candidate.attachments = attachments;
    candidate.position = position;
    candidate.yaw = WrapYaw(yaw);
    // Out-of-contract parameters fall back to the retail defaults rather than
    // dropping the action: the receiver only needs the alias.
    if (candidate.startFrame < -1 || candidate.startFrame > 1000000) candidate.startFrame = -1;
    if (candidate.endFrame < -1 || candidate.endFrame > 1000000) candidate.endFrame = -1;
    if (candidate.loops < -1 || candidate.loops > 65535) candidate.loops = 1;
    if (!std::isfinite(candidate.speed) || candidate.speed <= 0.0F || candidate.speed > 64.0F) candidate.speed = 1.0F;
    if (!std::isfinite(candidate.blendIn) || candidate.blendIn < 0.0F || candidate.blendIn > 60.0F) candidate.blendIn = 0.2F;
    if (!Valid(candidate)) {
        const std::lock_guard lock(gLocalMutex); ++gLocalDiagnostics.unavailable;
        return false;
    }
    gLocal = {character, before.address, before.runtime, before.animation, before.serial, now, candidate};
    {
        const std::lock_guard lock(gLocalMutex);
        if (!same) ++gLocalDiagnostics.captured;
        gLocalDiagnostics.animation = before.animation; gLocalDiagnostics.resource = before.resource;
    }
    output = candidate;
    return true;
}
}

std::string Encode(const State& value) {
    if (!Active(value)) return "0";
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<float>::max_digits10)
         << value.transition << ',' << value.animation << ',' << value.elapsedMilliseconds << ','
         << value.startFrame << ',' << value.endFrame << ',' << value.loops << ','
         << value.speed << ',' << value.blendIn << ',' << value.position[0] << ','
         << value.position[1] << ',' << value.position[2] << ',' << value.yaw << ','
         << value.phaseSeconds << ',' << value.durationSeconds;
    for (const auto attachment : value.attachments) text << ',' << attachment;
    return text.str();
}

void Initialize(const std::uintptr_t moduleBase, const std::uintptr_t nativeIdleTransition) noexcept {
    ForgetAll();
    gBase.store(moduleBase);
    gIdleTransition.store(nativeIdleTransition);
    timeline::Initialize(moduleBase);
}

void PumpLocalOnEngineThread() noexcept {
    auto& registry = character_incarnations::NativeRegistry();
    const auto ticket = registry.Local();
    LocalSnapshot capture{};
    if (ticket.character != 0U && registry.Current(ticket)) {
        capture.ticket = ticket;
        const auto attachments = props::CaptureOnEngineThread(ticket.character);
        capture.valid = CaptureLocal(ticket.character, attachments, capture.state) &&
            registry.Current(ticket) && registry.Local() == ticket;
        capture.capturedAt = GetTickCount64();
    } else {
        gLocal = {};
        static_cast<void>(props::CaptureOnEngineThread(0U));
    }
    const std::lock_guard lock(gLocalMutex);
    gLocalSnapshot = capture;
}
LocalSnapshot ReadLocal() noexcept {
    const std::lock_guard lock(gLocalMutex);
    return gLocalSnapshot;
}

bool Publish(const std::uintptr_t character, const State& state, std::string& error) noexcept {
    if (character < 0x10000U || !Valid(state)) {
        error = "Animation sync requires a Character binding and a valid clip state"; return false;
    }
    const std::lock_guard lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr) {
        if (!Active(state)) return true;
        for (auto& candidate : gSlots) if (candidate.character == 0U) {
            candidate = {}; candidate.character = character; candidate.binding = ++gEpoch;
            slot = &candidate; break;
        }
        if (slot == nullptr) { error = "Animation sync mailbox is full"; return false; }
    }
    const auto now = GetTickCount64();
    if (!Active(state)) {
        if (Active(slot->desired)) { slot->desired = {}; slot->clearedAt = now; slot->epoch = ++gEpoch; }
        return true;
    }
    if (state.transition < slot->highest) return true;
    if (state.transition == slot->highest) {
        // A phase refresh renews the observation lease without restarting the
        // native cycle. A clear is final for this ordered transition.
        if (Active(slot->desired) && slot->desired.animation != state.animation) {
            error = "Animation sync transition identity changed its clip"; return false;
        }
        if (Active(slot->desired)) {
            slot->desired = state; slot->receivedAt = now; slot->epoch = ++gEpoch;
        }
        return true;
    }
    slot->desired = state; slot->highest = state.transition;
    slot->receivedAt = now; slot->clearedAt = 0U; slot->epoch = ++gEpoch;
    return true;
}

void PumpAllOnEngineThread() noexcept {
    CollectStreams();
    std::array<std::uintptr_t, kSlots> characters{};
    { const std::lock_guard lock(gMutex); for (std::size_t i = 0; i < gSlots.size(); ++i) characters[i] = gSlots[i].character; }
    for (const auto previous : gPropCharacters)
        if (previous != 0U && std::find(characters.begin(), characters.end(), previous) == characters.end())
            props::ForgetOnEngineThread(previous);
    gPropCharacters = characters;
    for (const auto character : characters) if (character != 0U) {
        static_cast<void>(PumpOne(character));
        std::array<std::uint32_t, 4> attachments{};
        {
            const std::lock_guard lock(gMutex);
            if (const auto* slot = Find(character); slot != nullptr && slot->owned.address != 0U &&
                Active(slot->desired) && GetTickCount64() - slot->receivedAt + slot->desired.elapsedMilliseconds <= kObservationTimeoutMs)
                attachments = slot->desired.attachments;
        }
        // Tokens can change during a single clip. Reconcile after admission
        // and outside the mailbox lock: native graph creation calls back.
        props::ReconcileOnEngineThread(character, attachments);
    }
    CollectStreams();
}

bool OwnsMovement(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    // Deferred requests cannot freeze locomotion or steal a retail cycle.
    return slot != nullptr && slot->owned.address != 0U;
}
bool ClipPlaying(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    return slot != nullptr && slot->owned.address != 0U;
}
void Forget(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex);
    if (auto* slot = Find(character)) *slot = {};
}
void ForgetAll() noexcept {
    { const std::lock_guard lock(gMutex); gSlots = {}; ++gEpoch; }
    { const std::lock_guard lock(gLocalMutex); gLocalSnapshot = {}; }
}

void Export(Diagnostics& diagnostics, const std::uintptr_t remoteCharacter) {
    LocalDiagnostics local{};
    LocalSnapshot snapshot{};
    { const std::lock_guard lock(gLocalMutex); local = gLocalDiagnostics; snapshot = gLocalSnapshot; }
    RemoteDiagnostics remote{};
    bool playing{};
    std::uint32_t slots{};
    {
        const std::lock_guard lock(gMutex);
        for (const auto& slot : gSlots) slots += slot.character != 0U ? 1U : 0U;
        if (const auto* slot = Find(remoteCharacter)) { remote = slot->diagnostics; playing = slot->owned.address != 0U; }
    }
    diagnostics.Log("info", "native_animation_sync_snapshot",
        "Generic scripted-animation replication: local THE_PLAYER capture and remote representation playback.",
        {{"localCaptured", Diagnostics::Unsigned(local.captured), true},
         {"localCleared", Diagnostics::Unsigned(local.cleared), true},
         {"localHidingSkipped", Diagnostics::Unsigned(local.hidingSkipped), true},
         {"localTorn", Diagnostics::Unsigned(local.torn), true},
         {"localUnavailable", Diagnostics::Unsigned(local.unavailable), true},
         {"localAnimation", Diagnostics::Unsigned(local.animation), true},
         {"localResource", Diagnostics::Unsigned(local.resource), true},
         {"localActive", Diagnostics::Boolean(snapshot.valid && Active(snapshot.state)), true},
         {"remoteSlots", Diagnostics::Unsigned(slots), true},
         {"stage", remote.stage, false},
         {"transition", Diagnostics::Unsigned(remote.transition), true},
         {"animation", Diagnostics::Unsigned(remote.animation), true},
         {"resource", Diagnostics::Unsigned(remote.resource), true},
         {"clipPath", Diagnostics::Unsigned(remote.clipPath), true},
         {"playing", Diagnostics::Boolean(playing), true},
         {"applied", Diagnostics::Unsigned(remote.applied), true},
         {"failed", Diagnostics::Unsigned(remote.failed), true},
         {"stale", Diagnostics::Unsigned(remote.stale), true},
         {"clipUnavailable", Diagnostics::Unsigned(remote.unavailable), true},
         {"interrupted", Diagnostics::Unsigned(remote.interrupted), true},
         {"completed", Diagnostics::Unsigned(remote.completed), true},
         {"cancelled", Diagnostics::Unsigned(remote.cancelled), true},
         {"cancelDeferred", Diagnostics::Unsigned(remote.deferred), true}});
}
}
