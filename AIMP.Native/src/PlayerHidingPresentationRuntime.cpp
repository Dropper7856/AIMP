#include "aimp/PlayerHidingPresentationRuntime.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CharacterResourceAdmission.h"
#include "aimp/CrashGuard.h"
#include "aimp/HidingDoorPresentation.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>

namespace aimp::player_hiding_presentation {
namespace {
constexpr std::uintptr_t kScriptedType = 0xEADB58U;
constexpr std::uint64_t kMaximumPlaybackMs = 15000U;
struct Cycle final {
    std::uintptr_t address{}, type{}, owner{}, graph{};
    std::uint32_t runtime{}, resource{}, animation{};
    std::uint8_t state{}, serial{}, flags{};
};
struct Slot final {
    std::uintptr_t character{};
    State desired{};
    std::uint64_t binding{}, epoch{}, highest{}, handled{}, receivedAt{}, startedAt{};
    Cycle owned{};
    DiagnosticsSnapshot diagnostics{};
};
struct Capture final {
    std::uintptr_t character{};
    std::uint32_t runtime{}, generation{};
    std::uint64_t doorTransition{}, phaseAt{};
    State state{};
};
std::array<Slot, 40U> gSlots{};
std::mutex gMutex, gCaptureMutex;
std::atomic<std::uintptr_t> gBase{};
std::atomic<std::uintptr_t> gIdleTransition{};
Capture gCapture{};
std::uint64_t gEpoch{}, gCaptureTransition{};
struct Stream final {
    std::uintptr_t character{};
    std::uint64_t binding{}, transition{};
    std::uint32_t animation{};
    std::unique_ptr<native_resource::StrongLease> database;
    bool acquired{};
};
// Engine-thread owned. Forget only removes value bindings; the next engine
// pump releases requests through their pinned DB, without reading old actors.
// Intentionally process-lifetime storage: logical shutdown can stop the engine
// before it drains. Running StrongLease destructors during DLL/static teardown
// would call retail code on the loader thread. Only CollectStreams releases;
// at most 40 requests remain inert if that engine-thread drain is impossible.
auto& gStreams = *new std::array<Stream, 40U>{};

template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    return address >= 0x10000U && crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(value));
}
bool Match(const std::uintptr_t rva, const std::initializer_list<std::uint8_t> bytes) noexcept {
    std::array<std::uint8_t, 64U> actual{};
    return bytes.size() <= actual.size() &&
        crash_guard::TryCopy(actual.data(), reinterpret_cast<const void*>(gBase.load() + rva), bytes.size()) &&
        std::memcmp(actual.data(), bytes.begin(), bytes.size()) == 0;
}
bool Abi() noexcept {
    const auto base = gBase.load();
    std::uintptr_t update{}, initialize{}, marker{};
    // AISync already detours the constructor's entry. Validate its untouched
    // RET8 call/copy body and the same scripted vtable as AISyncAnimation.cpp.
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
Slot* Find(const std::uintptr_t character) noexcept {
    for (auto& slot : gSlots) if (slot.character == character) return &slot;
    return nullptr;
}
bool ReadCycle(const std::uintptr_t character, Cycle& cycle) noexcept {
    cycle = {};
    if (!Read(character + 0x50U, cycle.runtime) || cycle.runtime == 0U ||
        !Read(character + 0x36CU, cycle.address) || !Read(cycle.address, cycle.type) ||
        !Read(cycle.address + 4U, cycle.owner) || cycle.owner != character ||
        !Read(cycle.address + 8U, cycle.flags) || !Read(cycle.address + 0x19U, cycle.serial) ||
        !Read(cycle.address + 0x1AU, cycle.state)) return false;
    if (cycle.type == gBase.load() + kScriptedType && cycle.state == 0x12U)
        return Read(cycle.address + 0x14U, cycle.graph) && Read(cycle.address + 0x68U, cycle.resource) &&
            Read(cycle.address + 0x6CU, cycle.animation);
    return true;
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
// Exact base-clip table search used by retail 00A35FF0/00A36160. Parent adds
// hiding aliases to the representation's own database, before game loading.
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
bool StreamAbi() noexcept {
    for (const auto rva : {0x639060U, 0x636970U, 0x636BB0U}) {
        // Stable complete prefix; the following PUSH absolute string operand
        // is relocated by the loader and deliberately excluded.
        if (!Match(rva, {0x55,0x8B,0xEC,0x83,0xEC,0x10,0x56,0x57}) ||
            !Match(rva + (rva == 0x636970U ? 0x14CU : 0x13AU), {0xC2,0x08,0x00})) return false;
    }
    return true;
}
bool ClipOperation(const std::uintptr_t rva, const std::uintptr_t database,
    const std::uint32_t animation) noexcept {
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
                    (slot->desired.transition == stream.transition && slot->desired.animate &&
                     slot->desired.phase != 0U && slot->desired.phase != 2U && slot->handled != stream.transition);
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
        // 00A37510 returns this exact shared reference: instance+10 object,
        // instance+14 control. It pins the DB through asynchronous readiness.
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
// Cleanup touches only our exact constructor result. Never cancel a retail
// replacement, paired action, death, or recycled same-address cycle.
bool Cancel(const std::uintptr_t character, const Cycle& owned) noexcept {
    Cycle current{};
    if (!ReadCycle(character, current) || !Owned(current, owned)) return true;
    if (!RemoteReady(character) || gIdleTransition.load() == 0U) return false;
    const character_direct_step_runtime::MovementResourceLease resource(character);
    if (!resource) return false;
    // 0065BDD0's original trampoline: bypass our own movement detour's
    // suppression, using the ABI already validated by its installer.
    if (!InvokeIdle(current.address)) return false;
    return ReadCycle(character, current) && !Owned(current, owned);
}
void Advance(Slot& slot, const std::uint64_t now) noexcept {
    auto& d = slot.diagnostics;
    Cycle current{};
    if (!RemoteReady(slot.character) || !ReadCycle(slot.character, current)) {
        if (slot.owned.address != 0U) ++d.interrupted;
        slot.owned = {}; d.active = d.playing = false; d.stage = "actor-unavailable";
        slot.handled = slot.desired.transition;
        return;
    }
    if (slot.owned.address != 0U) {
        if (!Owned(current, slot.owned)) {
            slot.owned = {}; d.playing = false; ++d.interrupted; d.stage = "retail-interrupted";
        } else if ((current.flags & 6U) != 0U) {
            // A completed graph can remain the current cycle until retail or
            // locomotion replaces it. Release movement only after cleanup.
            if (Cancel(slot.character, slot.owned)) {
                slot.owned = {}; d.playing = false; ++d.completed; d.stage = "completed";
            }
        } else if (now - slot.startedAt > kMaximumPlaybackMs || slot.desired.phase == 0U) {
            if (Cancel(slot.character, slot.owned)) {
                slot.owned = {}; d.playing = false; ++d.interrupted; d.stage = "cancelled";
            } else { d.stage = "cancel-deferred"; }
        } else if (slot.owned.graph == 0U && current.graph != 0U) {
            slot.owned.graph = current.graph; // same constructor serial/tokens
            d.playing = true; d.stage = "playing";
        }
    }
    d.active = slot.owned.address != 0U || slot.desired.phase == 2U;
    if (slot.desired.phase == 0U || slot.handled == slot.desired.transition) return;
    slot.handled = slot.desired.transition; // one attempt, including failures
    d.transition = slot.desired.transition; d.phase = slot.desired.phase; d.kind = slot.desired.kind;
    if (slot.desired.phase == 2U) { d.active = true; d.stage = "hidden"; return; }
    if (!slot.desired.animate) { ++d.baseline; d.stage = "baseline"; return; }
    const auto age = static_cast<std::uint64_t>(slot.desired.elapsedMilliseconds) + now - slot.receivedAt;
    if (age > hiding_door_presentation::kMaximumReplayAgeMilliseconds) { ++d.stale; d.stage = "stale"; return; }
    d.stage = "abi";
    if (!Abi() || gIdleTransition.load() == 0U) { ++d.failed; return; }
    // Replacing an entry with its exit is allowed only while our exact cycle
    // still owns this actor. Other scripts and physical actions have priority.
    if (!ReadCycle(slot.character, current) || (!Owned(current, slot.owned) &&
        current.state != 1U && current.state != 3U && current.state != 0x38U)) {
        ++d.failed; d.stage = "cycle-busy"; return;
    }
    d.animationToken = ClipToken(slot.desired.kind, slot.desired.phase);
    if (!LocalClip(slot.character, d.animationToken, d.resourceToken, d.clipPath)) {
        ++d.failed; d.stage = "clip-unavailable"; return;
    }
    ai_sync::native_intent::Payload intent{};
    if (!BuildPlaybackIntent(slot.desired, d.resourceToken, intent)) { ++d.failed; d.stage = "parameters"; return; }
    const character_direct_step_runtime::MovementResourceLease resources(slot.character);
    if (!resources) { ++d.failed; d.stage = "resources"; return; }
    const auto readiness = PrepareStream(slot, d.animationToken);
    if (readiness == StreamReadiness::Failed) { ++d.failed; d.stage = "stream-request"; return; }
    if (readiness == StreamReadiness::Waiting) {
        // No constructor or placement while loading. The same acquired request
        // is polled until ready or the existing replay-age policy consumes it.
        slot.handled = 0U; d.active = true; d.stage = "stream-loading"; return;
    }
    // One placement at the phase boundary; repeated packets never overwrite
    // the native animation's root motion. This reuses certified placement ABI.
    if (!character_direct_step_runtime::ApplyAIRecoveryTransform(slot.character,
        slot.desired.position.data(), slot.desired.yaw)) { ++d.failed; d.stage = "alignment"; return; }
    const auto before = current;
    if (!ReadCycle(slot.character, current) || !SameCycle(before, current) || !RemoteReady(slot.character)) {
        ++d.failed; d.stage = "actor-changed"; return;
    }
    const bool invoked = Invoke(current.address, intent);
    const bool constructed = ReadCycle(slot.character, current) && current.runtime == before.runtime &&
        current.type == gBase.load() + kScriptedType && current.state == 0x12U &&
        current.serial == static_cast<std::uint8_t>(before.serial + 1U) &&
        current.resource == d.resourceToken && current.animation == d.animationToken;
    // Keep provenance even when an initializer synchronously reports failure
    // with graph=0. Otherwise movement might inherit a half-finished script.
    // Static 0066C4C0 marks not-ready as flags6, rather than deferred success.
    if (constructed) { slot.owned = current; slot.startedAt = now; d.active = true; d.playing = current.graph != 0U; }
    if (!invoked || !constructed || (current.flags & 4U) != 0U || current.graph == 0U) {
        ++d.failed; d.stage = "native-admission"; return;
    }
    slot.owned = current; slot.startedAt = now;
    ++d.applied; d.active = d.playing = true; d.stage = "playing";
}
}

bool BuildPlaybackIntent(const State& state, const std::uint32_t resource,
    ai_sync::native_intent::Payload& output) noexcept {
    if (!hiding_door_presentation::ValidSelection(state) || (state.phase != 1U && state.phase != 3U) || resource == 0U)
        return false;
    ai_sync::native_intent::Payload intent{};
    auto& w = intent.words;
    w[0] = 20U; w[21] = resource; w[22] = ClipToken(state.kind, state.phase);
    w[23] = w[24] = UINT32_MAX; // native start/end frame -1 = full clip
    w[25] = 1U; // one completion; no looping entrance/exit
    w[26] = std::bit_cast<std::uint32_t>(1.0F);
    w[27] = std::bit_cast<std::uint32_t>(0.2F);
    w[30] = std::bit_cast<std::uint32_t>(-1.0F);
    // In particular w12..19 and flags w33 remain zero: no THE_PLAYER,
    // convergence target, external clock, cinematic view or equipment request.
    if (!ai_sync::native_intent::Validate(intent)) return false;
    output = intent; return true;
}
void Initialize(const std::uintptr_t base, const std::uintptr_t nativeIdleTransition) noexcept {
    ForgetAll(); gBase.store(base); gIdleTransition.store(nativeIdleTransition);
}
bool Publish(const std::uintptr_t character, const State& state, std::string& error) noexcept {
    if (character < 0x10000U || !Valid(state) ||
        (state.phase != 0U && !hiding_door_presentation::ValidSelection(state))) {
        error = "Hiding presentation requires a valid Character binding and exact hiding icon key"; return false;
    }
    const std::lock_guard lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr) {
        if (state.phase == 0U) return true;
        for (auto& candidate : gSlots) if (candidate.character == 0U) {
            slot = &candidate; slot->character = character; slot->binding = ++gEpoch; break;
        }
    }
    if (slot == nullptr) { error = "Hiding presentation mailbox is full"; return false; }
    if (state.phase != 0U && state.transition <= slot->highest) {
        if (state.transition < slot->highest) return true;
        const auto& old = slot->desired;
        if (old.phase == 0U) return true; // a cleared stay cannot be resurrected
        if (old.phase != state.phase || old.kind != state.kind || old.entityShortGuid != state.entityShortGuid ||
            old.hierarchyHash != state.hierarchyHash) {
            error = "Hiding presentation transition identity changed its phase or interaction"; return false;
        }
        // Same transition is immutable; elapsed updates are not a new attempt,
        // and baseline animate=false cannot become a live replay later.
        return true;
    }
    slot->desired = state; slot->highest = (std::max)(slot->highest, state.transition);
    slot->receivedAt = GetTickCount64(); slot->epoch = ++gEpoch;
    return true;
}
bool PumpOnEngineThread(const std::uintptr_t character) noexcept {
    Slot copy{};
    { const std::lock_guard lock(gMutex); const auto* slot = Find(character); if (slot == nullptr) return false; copy = *slot; }
    Advance(copy, GetTickCount64());
    const std::lock_guard lock(gMutex);
    if (auto* slot = Find(character); slot != nullptr && slot->epoch == copy.epoch) {
        *slot = copy; return slot->diagnostics.active;
    }
    // A synchronous callback may publish a newer packet while native playback
    // starts. Preserve provenance of that playback for the next engine pump.
    if (auto* slot = Find(character); slot != nullptr && slot->binding == copy.binding) {
        slot->owned = copy.owned; slot->startedAt = copy.startedAt;
        slot->diagnostics = copy.diagnostics; slot->handled = copy.handled;
        return slot->owned.address != 0U || slot->desired.phase == 2U;
    }
    return false;
}
void PumpAllOnEngineThread() noexcept {
    CollectStreams();
    std::array<std::uintptr_t, 40U> characters{};
    { const std::lock_guard lock(gMutex); for (std::size_t i = 0; i < gSlots.size(); ++i) characters[i] = gSlots[i].character; }
    for (const auto character : characters) if (character != 0U) static_cast<void>(PumpOnEngineThread(character));
    CollectStreams();
}
void DrainStreamsOnEngineThread() noexcept { CollectStreams(); }
bool Active(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex); const auto* slot = Find(character);
    return slot != nullptr && (slot->desired.phase == 2U || slot->owned.address != 0U ||
        (slot->desired.phase != 0U && slot->desired.animate && slot->handled != slot->desired.transition));
}
DiagnosticsSnapshot Snapshot(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex); const auto* slot = Find(character);
    return slot != nullptr ? slot->diagnostics : DiagnosticsSnapshot{};
}
void Forget(const std::uintptr_t character) noexcept {
    { const std::lock_guard lock(gMutex); if (auto* slot = Find(character)) *slot = {}; }
    { const std::lock_guard lock(gCaptureMutex); if (gCapture.character == character) gCapture = {}; }
}
void ForgetAll() noexcept {
    { const std::lock_guard lock(gMutex); gSlots = {}; ++gEpoch; }
    { const std::lock_guard lock(gCaptureMutex); gCapture = {}; }
}

bool CaptureLocalOnEngineThread(const std::uintptr_t character, State& output) noexcept {
    output = {};
    bool local{};
    Cycle before{}, after{};
    std::uint8_t hiding{}, hidingAfter{};
    if (gBase.load() == 0U || !IsLocal(character, local) || !local || !ReadCycle(character, before) ||
        !Read(character + 0x4249U, hiding)) return false;
    const bool scripted = before.type == gBase.load() + kScriptedType && before.state == 0x12U &&
        before.graph != 0U && before.resource != 0U && (before.flags & 6U) == 0U;
    const auto clip = scripted ? ClassifyClip(before.animation) : ClipSelection{};
    const auto door = hiding_door_presentation::ReadLocalSnapshot();
    std::array<float, 3> position{};
    float yaw{};
    if (!Read(character + 0x70U, position) || !Read(character + 0x174U, yaw) ||
        !ReadCycle(character, after) || !SameCycle(before, after) || before.graph != after.graph ||
        before.resource != after.resource || before.animation != after.animation || before.flags != after.flags ||
        !Read(character + 0x4249U, hidingAfter) || hiding != hidingAfter) return false;
    const auto now = GetTickCount64();
    const std::lock_guard lock(gCaptureMutex);
    if (gCapture.character != character || gCapture.runtime != before.runtime ||
        (door && gCapture.generation != door->levelGeneration)) gCapture = {};
    if (clip.phase == 0U && hiding == 0U) { gCapture = {}; return true; }
    if (!door || !hiding_door_presentation::ValidSelection(door->state)) return false;
    auto phase = clip.phase;
    const auto kind = phase != 0U ? clip.kind : gCapture.state.kind;
    if (phase == 0U) {
        // A late hidden flag cannot rewind an already-observed exit.
        if (gCapture.state.phase == 3U) { gCapture = {}; return true; }
        if (hiding == 0U || gCapture.state.phase == 0U) return false;
        phase = 2U;
    }
    if (kind != door->state.kind || (phase == 3U ? door->state.phase != 3U : door->state.phase != 1U)) return false;
    const bool edge = gCapture.state.phase != phase || gCapture.doorTransition != door->state.transition ||
        gCapture.state.entityShortGuid != door->state.entityShortGuid || gCapture.state.hierarchyHash != door->state.hierarchyHash;
    State candidate = edge ? door->state : gCapture.state;
    if (edge) {
        candidate.phase = phase; candidate.position = position; candidate.yaw = yaw;
        candidate.transition = ++gCaptureTransition;
        candidate.elapsedMilliseconds = phase == 2U ? 0U : door->state.elapsedMilliseconds;
        candidate.animate = true;
    } else candidate.elapsedMilliseconds = static_cast<std::uint32_t>((std::min)(now - gCapture.phaseAt, std::uint64_t{60000U}));
    if (!Valid(candidate)) return false;
    if (edge) gCapture.phaseAt = now - candidate.elapsedMilliseconds;
    gCapture.character = character; gCapture.runtime = before.runtime; gCapture.generation = door->levelGeneration;
    gCapture.doorTransition = door->state.transition; gCapture.state = candidate;
    output = candidate; return true;
}
}
