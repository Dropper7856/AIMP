#include "aimp/PlayerPostureRuntime.h"
#include "aimp/CrashGuard.h"

#include <Windows.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace aimp::player_posture {
namespace {

constexpr std::size_t kQueueCapacity = 8U;
struct Slot final {
    std::uintptr_t character{};
    std::uint64_t epoch{}, lastSeen{}, activeTraversal{}, pendingSince{}, lease{};
    State desired{};
    State lastTraversal{};
    std::array<State, kQueueCapacity> pending{};
    std::size_t count{};
    bool observed{};
    bool approaching{};
    std::array<float, 3> approach{};
    DiagnosticsSnapshot diagnostics{};
};
std::mutex gMutex;
std::array<Slot, 40> gSlots{};
std::uintptr_t gModuleBase{};
std::uint64_t gEpoch{};

template <typename T>
bool Read(const std::uintptr_t address, T& value) noexcept {
    if (address < 0x10000U) return false;
#if defined(_MSC_VER)
    __try { value = *reinterpret_cast<const T*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(value));
#endif
}

Slot* Find(const std::uintptr_t character) noexcept {
    for (auto& slot : gSlots) if (slot.character == character) return &slot;
    return nullptr;
}

// All entry points below are certified for the supported x86 retail image.
// No caller supplies a native pointer over the network.
bool SetCrouch(const std::uintptr_t character, const bool crouch) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        // CharacterManager consumes Modifiers+E and compares it with +1A.
        // Setting only +2AF/tag lets that later consumer restore its old stance.
        // Use the same native application path for local and NPC Characters.
        const auto desired = static_cast<std::uint8_t>(crouch);
        auto* modifiers = reinterpret_cast<std::uint8_t*>(character + 0x3E2CU);
        const auto current = *reinterpret_cast<std::uint8_t*>(character + 0x2AFU);
        modifiers[0x0EU] = desired;
        if ((current != 0U) != crouch) modifiers[0x1AU] = static_cast<std::uint8_t>(current != 0U);
        using Apply = void(__thiscall*)(void*, void*);
        reinterpret_cast<Apply>(gModuleBase + 0x3D2AD0U)(modifiers, reinterpret_cast<void*>(character));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(character); static_cast<void>(crouch); return false;
#endif
}

bool EnterVent(const std::uintptr_t character, const std::uint32_t a,
    const std::uint32_t b) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        using Resolve = void*(__thiscall*)(void*, const std::uint32_t*);
        using Enter = void(__thiscall*)(void*, void*);
        const std::uint32_t identity[2]{a, b};
        const auto manager = *reinterpret_cast<void**>(gModuleBase + 0x134EF40U);
        if (manager == nullptr || a == 0U || b == 0U) return false;
        auto* vent = reinterpret_cast<Resolve>(gModuleBase + 0x3962A0U)(manager, identity);
        if (vent == nullptr || *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(vent) + 0x1CU) != a ||
            *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(vent) + 0x20U) != b) return false;
        reinterpret_cast<Enter>(gModuleBase + 0x3CFD10U)(reinterpret_cast<void*>(character), vent);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(character); static_cast<void>(a); static_cast<void>(b); return false;
#endif
}

bool ExitVent(const std::uintptr_t character) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        using Current = void*(__thiscall*)(void*);
        using Exit = void(__thiscall*)(void*, void*, bool);
        auto* chr = reinterpret_cast<void*>(character);
        auto* vent = reinterpret_cast<Current>(gModuleBase + 0x3A26E0U)(chr);
        reinterpret_cast<Exit>(gModuleBase + 0x3CB370U)(chr, vent, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(character); return false;
#endif
}

bool StartTraversal(const std::uintptr_t character, const std::uintptr_t cycle,
    const std::uintptr_t record, const std::uint16_t index) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    struct ResourceRef { void* object; void* control; };
    struct AnimationKey { std::uint32_t words[4]; };
    using Acquire = ResourceRef*(__thiscall*)(void*, ResourceRef*);
    using Lookup = void*(__thiscall*)(void*, const AnimationKey*);
    using Reset = void(__thiscall*)(ResourceRef*, void*, void*);
    ResourceRef resource{};
    __try {
        __try {
            auto* manager = *reinterpret_cast<void**>(character + 0x720U);
            if (manager == nullptr) return false;
            reinterpret_cast<Acquire>(gModuleBase + 0x6374D0U)(manager, &resource);
            if (resource.object == nullptr ||
                *reinterpret_cast<void**>(static_cast<std::uint8_t*>(resource.object) + 0x18U) == nullptr)
                return false;
            AnimationKey key{};
            key.words[2] = *reinterpret_cast<std::uint32_t*>(record + 0x104U);
            if (reinterpret_cast<Lookup>(gModuleBase + 0x631400U)(resource.object, &key) == nullptr)
                return false;
            // The initializer does not check for a missing animation. Keep its
            // exact resource pinned until the native traversal has initialized.
            using Start = void(__thiscall*)(void*, std::uint32_t, bool);
            reinterpret_cast<Start>(gModuleBase + 0x2581D0U)(reinterpret_cast<void*>(cycle), index, false);
            return true;
        } __finally {
            if (resource.object != nullptr || resource.control != nullptr)
                reinterpret_cast<Reset>(gModuleBase + 0x62D0B0U)(&resource, nullptr, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(character); static_cast<void>(cycle); static_cast<void>(record);
    static_cast<void>(index); return false;
#endif
}

bool ResolveLink(const State& state, std::uintptr_t& record) noexcept {
    std::uintptr_t root{}, nav{}, records{};
    std::uint32_t count{}, owner{};
    std::uint16_t index{};
    std::uint8_t type{}, enabled{};
    if (!Read(gModuleBase + 0x12F0C88U, root) || !Read(root + 0x20U, nav) ||
        !Read(nav + 0x23F0U, count) || state.traversalIndex > count ||
        !Read(nav + 0x23ECU, records)) return false;
    record = records + (static_cast<std::uintptr_t>(state.traversalIndex) - 1U) * 0x10CU;
    return Read(record, index) && index == state.traversalIndex &&
        Read(record + 2U, type) && type == state.traversalType &&
        Read(record + 4U, owner) && owner == state.traversalOwner &&
        Read(record + 0x108U, enabled) && (enabled & 1U) != 0U;
}

bool EntryApproach(const std::uintptr_t character, const std::uintptr_t record,
    std::array<float, 3>& goal, float& distance) noexcept {
    std::array<float, 3> outer{}, inner{}, current{};
    if (!Read(record + 0x10U, outer) || !Read(record + 0x1CU, inner) ||
        !Read(character + 0x70U, current)) return false;
    float length{}, dot{};
    for (std::size_t i = 0U; i < 3U; ++i) {
        if (!std::isfinite(outer[i]) || !std::isfinite(inner[i]) || !std::isfinite(current[i])) return false;
        const auto delta = inner[i] - outer[i];
        length += delta * delta;
        dot += (current[i] - outer[i]) * delta;
    }
    const auto t = length > 0.0001F ? std::clamp(dot / length, 0.0F, 1.0F) : 0.0F;
    distance = 0.0F;
    for (std::size_t i = 0U; i < 3U; ++i) {
        goal[i] = outer[i] + t * (inner[i] - outer[i]);
        distance += (goal[i] - current[i]) * (goal[i] - current[i]);
    }
    distance = std::sqrt(distance);
    return std::isfinite(distance);
}

void ApplyPosture(const std::uintptr_t character, const State& desired) noexcept {
    std::uint8_t inVent{}, crouched{};
    std::uint32_t a{}, b{};
    if (!Read(character + 0x2B4U, inVent)) return;
    if ((desired.flags & 4U) != 0U) {
        if (!Read(character + 0x4290U, a) || !Read(character + 0x4294U, b)) return;
        if ((inVent == 0U || a != desired.ventIdA || b != desired.ventIdB) &&
            !EnterVent(character, desired.ventIdA, desired.ventIdB)) return;
    } else if (inVent != 0U && !ExitVent(character)) return;
    const auto shouldCrouch = (desired.flags & 5U) != 0U;
    if (Read(character + 0x2AFU, crouched))
        static_cast<void>(SetCrouch(character, shouldCrouch));
    // Retail's low-ceiling component writes +2B0 only. +2B1 belongs to
    // interactions and must never be inferred from deep crouch.
    const auto deep = static_cast<std::uint8_t>((desired.flags & 2U) != 0U);
    std::uint8_t current{};
    if (Read(character + 0x2B0U, current) && current != deep)
        static_cast<void>(crash_guard::TryWrite(reinterpret_cast<void*>(character + 0x2B0U), &deep, sizeof(deep)));
}

void Complete(const Slot& work, const bool started, const bool discard) noexcept {
    std::lock_guard lock(gMutex);
    auto* slot = Find(work.character);
    if (slot == nullptr || slot->epoch != work.epoch) return;
    if (started) slot->activeTraversal = work.pending[0].traversalRevision;
    slot->approaching = false;
    if (started) ++slot->diagnostics.traversalApplications;
    if (discard) ++slot->diagnostics.traversalRejections;
    if ((started || discard) && slot->count != 0U &&
        slot->pending[0].traversalRevision == work.pending[0].traversalRevision) {
        for (std::size_t i = 1U; i < slot->count; ++i) slot->pending[i - 1U] = slot->pending[i];
        --slot->count;
        slot->pendingSince = 0U;
    }
}

} // namespace

void Initialize(const std::uintptr_t moduleBase) noexcept { gModuleBase = moduleBase; }

bool Publish(const std::uintptr_t character, const State& state, std::string& error) noexcept {
    if (gModuleBase == 0U || character < 0x10000U || !Valid(state)) {
        error = "Invalid PlayerSync posture state."; return false;
    }
    std::lock_guard lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr) {
        for (auto& candidate : gSlots) if (candidate.character == 0U) {
            slot = &candidate; *slot = {}; slot->character = character; slot->epoch = ++gEpoch; break;
        }
    }
    if (slot == nullptr) { error = "PlayerSync posture capacity reached."; return false; }
    if (slot->observed && state.traversalRevision != 0U && state.traversalRevision < slot->lastSeen) {
        error = "Stale PlayerSync traversal revision."; return false;
    }
    if (state.traversalRevision != 0U && state.traversalRevision == slot->lastSeen &&
        (state.traversalIndex != slot->lastTraversal.traversalIndex ||
         state.traversalType != slot->lastTraversal.traversalType ||
         state.traversalOwner != slot->lastTraversal.traversalOwner)) {
        error = "PlayerSync traversal identity changed within one revision."; return false;
    }
    const auto newTraversal = state.traversalRevision > slot->lastSeen &&
        (slot->observed || (state.flags & 8U) != 0U);
    // Backpressure must not strand old entries behind an expired lease. The
    // movement command remains admissible while its posture edge is retried.
    slot->lease = GetTickCount64() + 1000U;
    if (newTraversal && slot->count == kQueueCapacity) {
        error = "PlayerSync traversal queue is full."; return false;
    }
    if (newTraversal) {
        if (slot->count == 0U) slot->pendingSince = GetTickCount64();
        slot->pending[slot->count++] = state;
    }
    if (state.traversalRevision != 0U) {
        slot->lastSeen = state.traversalRevision;
        slot->lastTraversal = state;
    }
    slot->observed = true;
    slot->desired = state;
    return true;
}

bool PumpOnEngineThread(const std::uintptr_t character) noexcept {
    Slot work{};
    {
        std::unique_lock lock(gMutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        auto* slot = Find(character);
        if (slot == nullptr) return false;
        slot->approaching = false;
        work = *slot;
    }
    std::uintptr_t cycle{}, owner{};
    std::uint8_t state{}, grappled{}, ready{};
    std::uint32_t health{};
    if (!Read(character + 0x36CU, cycle) || !Read(cycle + 4U, owner) || owner != character ||
        !Read(cycle + 0x1AU, state)) return false;
    if (state == 0x16U) return true;
    // Death/paired interaction has priority over stance and queued traversal.
    if (!Read(character + 0x297U, grappled) || grappled != 0U ||
        !Read(character + 0x32CU, health) || health >= 3U ||
        state == 0x42U || state == 0x4EU || state == 0x4FU || state == 0x47U) return false;
    if (work.activeTraversal != 0U) {
        std::lock_guard lock(gMutex);
        if (auto* slot = Find(character); slot != nullptr && slot->epoch == work.epoch)
            slot->activeTraversal = 0U;
    }
    // Expire abandoned admission work even when no fresh movement lease exists.
    if (work.count != 0U && work.pendingSince != 0U &&
        GetTickCount64() - work.pendingSince > 10000U) {
        Complete(work, false, true);
        return false;
    }
    // An unchanged idle posture sends no movement heartbeat. Keep its native
    // stance request until replacement/Forget; only traversal admission needs
    // the short motion lease.
    if (work.count != 0U && GetTickCount64() >= work.lease) return false;
    if (work.count != 0U) {
        if (work.pendingSince == 0U) {
            std::lock_guard lock(gMutex);
            if (auto* slot = Find(character); slot != nullptr && slot->epoch == work.epoch)
                slot->pendingSince = GetTickCount64();
        }
        std::uintptr_t record{};
        if (!ResolveLink(work.pending[0], record)) {
            Complete(work, false, true);
            OutputDebugStringA("[AIMP PlayerSync] Rejected mismatched or disabled vent traversal link.\n");
            return false;
        }
        std::array<float, 3> goal{};
        float distance{};
        if (!EntryApproach(character, record, goal, distance) || distance > 8.0F) {
            Complete(work, false, true);
            return false;
        }
        const auto holdEntrance = [&]() noexcept {
            std::lock_guard lock(gMutex);
            if (auto* slot = Find(character); slot != nullptr && slot->epoch == work.epoch) {
                slot->approach = goal;
                slot->approaching = true;
            }
        };
        if (distance > 0.8F) {
            holdEntrance();
            return false;
        }
        if (Read(cycle + 8U, ready) && (ready & 2U) != 0U) {
            const auto started = StartTraversal(character, cycle, record, work.pending[0].traversalIndex);
            std::uint8_t entered{};
            const auto applied = started && Read(cycle + 0x1AU, entered) && entered == 0x16U;
            if (applied) {
                Complete(work, true, false);
                return true;
            }
            // A just-spawned representation can still be acquiring its resource.
            // Retry within the bounded admission window, leaving its cycle intact.
            holdEntrance();
            return false;
        }
        // Allow retail to finish its current action, without constructing a
        // traversal over a cycle that does not currently accept transitions.
        holdEntrance();
        return false;
    }
    ApplyPosture(character, work.desired);
    {
        std::lock_guard lock(gMutex);
        if (auto* slot = Find(character); slot != nullptr && slot->epoch == work.epoch)
            ++slot->diagnostics.postureApplications;
    }
    return false;
}

void Forget(const std::uintptr_t character) noexcept {
    std::lock_guard lock(gMutex);
    if (auto* slot = Find(character); slot != nullptr) *slot = {};
}

void ForgetAll() noexcept {
    std::lock_guard lock(gMutex);
    for (auto& slot : gSlots) slot = {};
}

bool ApproachTarget(const std::uintptr_t character, float& x, float& y, float& z) noexcept {
    std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    if (slot == nullptr || !slot->approaching || GetTickCount64() >= slot->lease) return false;
    x = slot->approach[0]; y = slot->approach[1]; z = slot->approach[2];
    return true;
}

DiagnosticsSnapshot Snapshot(const std::uintptr_t character) noexcept {
    std::lock_guard lock(gMutex);
    if (const auto* slot = Find(character)) {
        auto result = slot->diagnostics;
        result.activeRevision = slot->activeTraversal;
        result.pendingTraversals = slot->count;
        return result;
    }
    return {};
}

} // namespace aimp::player_posture
