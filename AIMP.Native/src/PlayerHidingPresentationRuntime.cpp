#include "aimp/PlayerHidingPresentationRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/HidingDoorPresentation.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

namespace aimp::player_hiding_presentation {
namespace {
constexpr std::uintptr_t kScriptedType = 0xEADB58U;
struct Cycle final {
    std::uintptr_t address{}, type{}, owner{}, graph{};
    std::uint32_t runtime{}, resource{}, animation{};
    std::uint8_t state{}, serial{}, flags{};
};
// Hiding retains spot identity and occupancy. AnimationSync owns actor playback.
struct Slot final {
    std::uintptr_t character{};
    State desired{};
    std::uint64_t epoch{}, highest{}, receivedAt{};
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
Capture gCapture{};
std::uint64_t gEpoch{}, gCaptureTransition{};
Slot* Find(const std::uintptr_t character) noexcept {
    for (auto& slot : gSlots) if (slot.character == character) return &slot;
    return nullptr;
}
template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    return address >= 0x10000U && crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(value));
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
}

void Initialize(const std::uintptr_t base, const std::uintptr_t nativeIdleTransition) noexcept {
    static_cast<void>(nativeIdleTransition); // compatibility; no legacy playback
    ForgetAll(); gBase.store(base);
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
            slot = &candidate; slot->character = character; slot->epoch = ++gEpoch; break;
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
    const std::lock_guard lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr) return false;
    auto& d = slot->diagnostics;
    d.transition = slot->desired.transition;
    d.phase = slot->desired.phase;
    d.kind = slot->desired.kind;
    d.active = slot->desired.phase == 2U;
    d.playing = false;
    d.stage = d.active ? "hidden-occupancy" : "animation-sync";
    return d.active;
}
void PumpAllOnEngineThread() noexcept {
    std::array<std::uintptr_t, 40U> characters{};
    { const std::lock_guard lock(gMutex); for (std::size_t i = 0; i < gSlots.size(); ++i) characters[i] = gSlots[i].character; }
    for (const auto character : characters) if (character != 0U) static_cast<void>(PumpOnEngineThread(character));
}
void DrainStreamsOnEngineThread() noexcept {} // no hiding-owned clip requests
bool Active(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex); const auto* slot = Find(character);
    return slot != nullptr && slot->desired.phase == 2U;
}
bool HiddenPlacement(const std::uintptr_t character, std::array<float, 3>& position, float& yaw) noexcept {
    const std::lock_guard lock(gMutex); const auto* slot = Find(character);
    if (slot == nullptr || (slot->desired.phase != 2U && slot->desired.phase != 3U)) return false;
    position = slot->desired.position; yaw = slot->desired.yaw;
    return true;
}
bool HiddenExitHeading(const std::uintptr_t character, float& yaw) noexcept {
    static_cast<void>(character); static_cast<void>(yaw);
    return false; // the captured native clip owns its authored turn
}
bool ClipPlaying(const std::uintptr_t character) noexcept {
    static_cast<void>(character);
    return false; // playback ownership belongs exclusively to AnimationSync
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
    std::uint8_t hiding{}, hidingAfter{}, interaction{}, interactionAfter{};
    if (gBase.load() == 0U || !IsLocal(character, local) || !local || !ReadCycle(character, before) ||
        !Read(character + 0x4249U, hiding) || !Read(character + 0x425BU, interaction)) return false;
    const bool scripted = before.type == gBase.load() + kScriptedType && before.state == 0x12U &&
        before.graph != 0U && before.resource != 0U && (before.flags & 6U) == 0U;
    const auto clip = scripted ? ClassifyClip(before.animation) : ClipSelection{};
    const auto door = hiding_door_presentation::ReadLocalSnapshot();
    std::array<float, 3> position{};
    float yaw{};
    if (!Read(character + 0x70U, position) || !Read(character + 0x174U, yaw) ||
        !ReadCycle(character, after) || !SameCycle(before, after) || before.graph != after.graph ||
        before.resource != after.resource || before.animation != after.animation || before.flags != after.flags ||
        !Read(character + 0x4249U, hidingAfter) || hiding != hidingAfter ||
        !Read(character + 0x425BU, interactionAfter) || interaction != interactionAfter) return false;
    const auto now = GetTickCount64();
    const std::lock_guard lock(gCaptureMutex);
    if (gCapture.character != character || gCapture.runtime != before.runtime ||
        (door && gCapture.generation != door->levelGeneration)) gCapture = {};
    if (!door || !hiding_door_presentation::ValidSelection(door->state)) {
        if (hiding == 0U && clip.phase == 0U) { gCapture = {}; return true; }
        return false;
    }
    if (clip.phase != 0U && clip.kind != door->state.kind) return false;
    const bool sameDoor = gCapture.doorTransition == door->state.transition &&
        gCapture.state.entityShortGuid == door->state.entityShortGuid &&
        gCapture.state.hierarchyHash == door->state.hierarchyHash;
    const auto phase = LocalPhase(gCapture.state.phase, sameDoor, door->state,
        clip.phase, hiding != 0U, interaction != 0U);
    // Retain the consumed door identity after completion, so a repeated stale
    // latch cannot start a second entry while its hidden flag is still false.
    if (phase == 0U) {
        gCapture.character = character; gCapture.runtime = before.runtime;
        gCapture.generation = door->levelGeneration; gCapture.doorTransition = door->state.transition;
        gCapture.state = door->state; gCapture.state.phase = 0U;
        return true;
    }
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
