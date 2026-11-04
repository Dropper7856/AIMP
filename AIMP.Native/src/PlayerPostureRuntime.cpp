#include "aimp/PlayerPostureRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/PlayerAnimationClipCapture.h"
#include "aimp/HookManager.h"

#include <Windows.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <atomic>

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
    bool animatedCrouched{};
    std::uint64_t nextAnimationRefresh{};
};
std::mutex gMutex;
std::array<Slot, 40> gSlots{};
std::uintptr_t gModuleBase{};
std::uint64_t gEpoch{};
AnimationRefresh gAnimationRefresh{};
std::atomic<void*> gAimSetup{};
void* gAimSetupStorage{};
void __fastcall AimSetupHook(void* controller, void*) {
    SetupAim(controller, reinterpret_cast<AimSetup>(gAimSetup.load(std::memory_order_acquire)));
}

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

constexpr std::uint32_t HashName(const char* text) noexcept {
    std::uint32_t hash = 2166136261U;
    for (; *text != '\0'; ++text) hash = (hash ^ static_cast<std::uint8_t>(*text)) * 16777619U;
    return hash;
}
constexpr auto kCrouchedContext = HashName("CROUCHED");

// Retail 00A2DC20/00A36F30 store three reference-counted contexts in this
// object; 00A36160 searches slot zero first. Read the actual animation input,
// rather than treating Character+2AF as proof that a clip was selected.
bool ReadStanceContext(const std::uintptr_t character, std::uint32_t& context,
    std::uint32_t& references) noexcept {
    std::uintptr_t contexts{};
    return Read(character + 0x728U, contexts) && contexts != 0U &&
        Read(contexts + 0x10U, context) && Read(contexts + 0x38U, references);
}

bool RepairCrouchedContext(const std::uintptr_t character, const bool crouch) noexcept {
    std::uint32_t context{}, references{};
    if (!crouch || !ReadStanceContext(character, context, references) ||
        (references != 0U && context == kCrouchedContext)) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        // The stance flag can survive a retail replacement of context zero.
        // 007CAE20 only adds the tag when that flag changes, so calling it
        // again cannot repair this case. Re-add the missing tag once through
        // the same native helper; do not increase an already-present count.
        using AddTag = void(__cdecl*)(std::uintptr_t, std::uint32_t, int);
        reinterpret_cast<AddTag>(gModuleBase + 0x3E0400U)(character, kCrouchedContext, 0);
        return ReadStanceContext(character, context, references) &&
            references != 0U && context == kCrouchedContext;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}

// Read-only projection of retail 00A36160's table search. It reports the
// selected path hash, not a claim that Havok sampled or rendered that clip.
std::uintptr_t TableValue(const std::uintptr_t table, const std::uint32_t hash,
    const std::size_t stride) noexcept {
    std::uintptr_t keys{}, values{};
    std::uint32_t count{};
    if (!Read(table + 8U, keys) || !Read(table + 12U, values) ||
        !Read(table + 16U, count) || count > 100000U || keys == 0U || values == 0U) return 0U;
    std::uint32_t first = 0U, last = count;
    while (first < last) {
        const auto middle = first + (last - first) / 2U;
        std::uint32_t key{}, slot{};
        if (!Read(keys + middle * 8U, key)) return 0U;
        if (key > hash) first = middle + 1U;
        else if (key < hash) last = middle;
        else if (Read(keys + middle * 8U + 4U, slot) && slot < count)
            return values + slot * stride;
        else return 0U;
    }
    return 0U;
}

std::uint32_t ClipPath(const std::uintptr_t database, const std::uintptr_t contexts,
    const std::uint32_t clip) noexcept {
    std::uintptr_t names{}, tables{}, base{};
    if (!Read(database + 0x28U, names) || !Read(database + 0x2CU, tables) ||
        !Read(database + 0x20U, base)) return 0U;
    for (std::size_t i = 0U; i < 3U; ++i) {
        std::uint32_t references{}, context{}, index{}, path{};
        if (!Read(contexts + 0x38U + i * 4U, references) || references == 0U ||
            !Read(contexts + 0x10U + i * 16U, context)) continue;
        const auto indexed = TableValue(names, context, 4U);
        if (indexed == 0U || !Read(indexed, index) || index >= 1024U) continue;
        const auto selected = TableValue(tables + index * 0x28U, clip, 8U);
        if (selected != 0U && Read(selected, path)) return path;
    }
    std::uint32_t path{};
    const auto selected = TableValue(base, clip, 8U);
    return selected != 0U && Read(selected, path) ? path : 0U;
}

void SampleAnimation(const std::uintptr_t character, DiagnosticsSnapshot& result) noexcept {
    result.animationContextReadable = ReadStanceContext(character,
        result.animationContext, result.animationContextReferences);
    result.animationDatabase = result.animationIdlePath = result.animationWalkPath = 0U;
    std::uintptr_t instance{}, database{}, contexts{};
    // 00A374D0/00A37510 expose instance+8 resource and instance+10 clip DB;
    // 00A39280 uses clipDB+10 as the per-character database identity.
    if (Read(character + 0x720U, instance) && Read(instance + 0x10U, database) &&
        Read(database + 0x10U, result.animationDatabase) &&
        Read(character + 0x728U, contexts) && contexts != 0U) {
        result.animationIdlePath = ClipPath(database, contexts, HashName("idle"));
        result.animationWalkPath = ClipPath(database, contexts, HashName("walk_gait01_l_100"));
    }
    if (result.desiredCrouched) {
        result.lastCrouchDatabase = result.animationDatabase;
        result.lastCrouchContext = result.animationContext;
        result.lastCrouchReferences = result.animationContextReferences;
        result.lastCrouchIdlePath = result.animationIdlePath;
        result.lastCrouchWalkPath = result.animationWalkPath;
    }
}

// All entry points below are certified for the supported x86 retail image.
// No caller supplies a native pointer over the network.
bool SetCrouch(const std::uintptr_t character, const bool crouch) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        // 687B60 consumes retail NPC StanceRequest kind 7 through these helpers
        // (mode 1=stand, 2=crouch). They update the NPC's CROUCHED animation tag
        // and +2AF, also read by sight/attack/stealth queries (6E3DB0, 7922F0).
        // Writing only the player modifier path misses this NPC entry point.
        using Stance = void(__thiscall*)(void*);
        reinterpret_cast<Stance>(gModuleBase + (crouch ? 0x3CAE20U : 0x3CAE90U))(
            reinterpret_cast<void*>(character));
        const auto desired = static_cast<std::uint8_t>(crouch);
        auto* modifiers = reinterpret_cast<std::uint8_t*>(character + 0x3E2CU);
        const auto current = *reinterpret_cast<std::uint8_t*>(character + 0x2AFU);
        // Keep the later CharacterModifiers consumer consistent with this
        // request, including representations carrying a player component.
        modifiers[0x0EU] = desired;
        if ((current != 0U) != crouch) modifiers[0x1AU] = static_cast<std::uint8_t>(current != 0U);
        using Apply = void(__thiscall*)(void*, void*);
        reinterpret_cast<Apply>(gModuleBase + 0x3D2AD0U)(modifiers, reinterpret_cast<void*>(character));
        return (*reinterpret_cast<const std::uint8_t*>(character + 0x2AFU) != 0U) == crouch;
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

bool ApplyPosture(const std::uintptr_t character, const State& desired) noexcept {
    std::uint8_t inVent{};
    std::uint32_t a{}, b{};
    if (!Read(character + 0x2B4U, inVent)) return false;
    if ((desired.flags & 4U) != 0U) {
        if (!Read(character + 0x4290U, a) || !Read(character + 0x4294U, b)) return false;
        if ((inVent == 0U || a != desired.ventIdA || b != desired.ventIdB) &&
            !EnterVent(character, desired.ventIdA, desired.ventIdB)) return false;
    } else if (inVent != 0U && !ExitVent(character)) return false;
    // A low-ceiling/deep crouch is still crouched even if its independent
    // source flag was observed between two ordinary crouch transitions.
    const auto shouldCrouch = (desired.flags & 7U) != 0U;
    if (!SetCrouch(character, shouldCrouch)) return false;
    // Retail's low-ceiling component writes +2B0 only. +2B1 belongs to
    // interactions and must never be inferred from deep crouch.
    const auto deep = static_cast<std::uint8_t>((desired.flags & 2U) != 0U);
    std::uint8_t current{};
    if (!Read(character + 0x2B0U, current)) return false;
    return current == deep || crash_guard::TryWrite(reinterpret_cast<void*>(character + 0x2B0U), &deep, sizeof(deep));
}

bool IsRemoteCharacter(const std::uintptr_t character) noexcept {
    std::uintptr_t root{}, manager{}, players{}, definition{};
    std::uint32_t count{};
    if (!Read(character + 0x64U, definition) || definition < 0x10000U ||
        !Read(gModuleBase + 0x12F0C88U, root) || !Read(root + 8U, manager) ||
        !Read(manager + 0x84U, count) || count == 0U || count > 4U ||
        !Read(manager + 0x80U, players) || players < 0x10000U) return false;
    for (std::uint32_t i = 0U; i < count; ++i) {
        std::uintptr_t player{};
        if (!Read(players + i * sizeof(std::uintptr_t), player) || player == character) return false;
    }
    return true;
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

bool InstallAimHook(const std::uintptr_t moduleBase, HookManager& hooks, std::string& error) {
    if (!hooks.ExactBuildValidated()) { error = "Remote aim requires the certified retail image."; return false; }
    HookRequest request{};
    request.name = "remote_crouch_aim_reference_pose";
    request.target = reinterpret_cast<void*>(moduleBase + 0x272B30U);
    request.detour = reinterpret_cast<void*>(&AimSetupHook);
    // One complete instruction: sub esp,224h. No relative operands.
    request.patchLength = 6U;
    request.expected = MaskedBytes{{0x81U, 0xECU, 0x24U, 0x02U, 0x00U, 0x00U}, "xxxxxx"};
    request.relocationSafePrefix = true;
    request.trampolineOut = &gAimSetupStorage;
    request.publishedTrampolineOut = &gAimSetup;
    return hooks.Install(request, error);
}

namespace {
// Keep SEH restoration separate from C++ lock objects. The movement stance
// is borrowed only while retail selects/binds its animation tree; gameplay
// flags, animation contexts and the physical locomotion cycle are untouched.
void BindStandardAim(void* controller, const AimSetup original, const std::uintptr_t motion,
    const std::uint32_t stance) {
    auto* selector = reinterpret_cast<std::uint32_t*>(motion + 0x3CU);
    auto* movementMask = reinterpret_cast<float*>(static_cast<std::uint8_t*>(controller) + 0x1D0U);
    __try {
        if (stance == 1U || stance == 5U) *selector = 0U;
        *movementMask = 1.0F;
        original(controller);
    } __finally {
        if (stance == 1U || stance == 5U) *selector = stance;
    }
}
}

void SetupAim(void* controller, const AimSetup original) {
    if (original == nullptr) return;
    std::uintptr_t character{}, motion{}, instance{}, database{}, cycle{}, owner{}, definition{};
    std::uint32_t identity{}, stance{}, health{};
    std::uint8_t crouched{}, cycleState{}, grappled{}, inVent{}, player{}, firstPerson{};
    const auto address = reinterpret_cast<std::uintptr_t>(controller);
    std::uint64_t epoch{};
    if (Read(address + 4U, character)) {
        std::unique_lock lock(gMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            const auto* slot = Find(character);
            if (slot != nullptr && (slot->desired.flags & 3U) != 0U && (slot->desired.flags & 12U) == 0U)
                epoch = slot->epoch;
        }
    }
    const bool adapt = epoch != 0U && IsRemoteCharacter(character) &&
        Read(character + 0x64U, definition) && Read(definition + 0x1CU, player) &&
        Read(definition + 0x1FU, firstPerson) && !(player != 0U && firstPerson != 0U) &&
        Read(character + 0x720U, instance) && Read(instance + 0x10U, database) &&
        Read(database + 0x10U, identity) && identity == HashName("E_RIPLEY") &&
        Read(character + 0x2AFU, crouched) && crouched != 0U &&
        Read(character + 0x2B4U, inVent) && inVent == 0U &&
        Read(character + 0x297U, grappled) && grappled == 0U &&
        Read(character + 0x32CU, health) && health < 3U &&
        Read(character + 0x36CU, cycle) && Read(cycle + 4U, owner) && owner == character &&
        Read(cycle + 0x1AU, cycleState) && (cycleState == 1U || cycleState == 3U || cycleState == 0x38U) &&
        Read(character + 0x1100U, motion) && Read(motion + 0x3CU, stance) &&
        (stance == 0U || stance == 1U || stance == 5U);
    if (!adapt) { original(controller); return; }

    // Retail 00673110 writes movement_mask_weight (+1D0) from speed, then
    // calls 00672B30. At rest it is zero, allowing the aim layer to replace
    // the primary hips/legs. Always retain those crouched bones. The standard
    // aim tree supplies aim_centre + the directional additive blend and weapon
    // IK; the crouched tree has only the directional blend. E_RIPLEY has no
    // separate crouched weapon reference/blend to support that latter path.
    BindStandardAim(controller, original, motion, stance);
    std::uintptr_t tree{};
    std::uint32_t treeName{};
    if (Read(address + 0x14U, tree)) Read(tree + 0x38U, treeName);
    std::unique_lock lock(gMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        if (auto* slot = Find(character); slot != nullptr && slot->epoch == epoch) {
            ++slot->diagnostics.crouchedAimApplications;
            slot->diagnostics.lastAimTree = treeName;
            slot->diagnostics.lastAimStance = stance;
        }
    }
}

void Initialize(const std::uintptr_t moduleBase, const AnimationRefresh refresh) noexcept {
    gModuleBase = moduleBase; gAnimationRefresh = refresh;
}

bool RefreshAnimationCycle(const std::uintptr_t character, const std::uintptr_t idleEntry,
    const std::uintptr_t moveEntry) noexcept {
    if (gModuleBase == 0U || !IsRemoteCharacter(character)) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        std::uintptr_t cycle{}, owner{};
        std::uint8_t state{}, grappled{};
        std::uint32_t health{};
        if (!Read(character + 0x36CU, cycle) || !Read(cycle + 4U, owner) || owner != character ||
            !Read(cycle + 0x1AU, state) || (state != 1U && state != 0x38U) ||
            !Read(character + 0x297U, grappled) || grappled != 0U ||
            !Read(character + 0x32CU, health) || health >= 3U) return false;
        const auto entry = state == 0x38U ? moveEntry : idleEntry;
        if (entry == 0U) return false;
        using Enter = void(__thiscall*)(void*);
        reinterpret_cast<Enter>(entry)(reinterpret_cast<void*>(cycle));
        std::uintptr_t after{};
        std::uint8_t afterState{};
        return Read(character + 0x36CU, after) && after == cycle &&
            Read(cycle + 0x1AU, afterState) && afterState == state;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}

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
    if (!IsRemoteCharacter(character)) return false;
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
    return false;
}

void ReconcileAllOnEngineThread() noexcept {
    std::array<Slot, 40> work{};
    {
        std::lock_guard lock(gMutex);
        work = gSlots;
    }
    for (const auto& item : work) {
        if (item.character == 0U) continue;
        auto result = item.diagnostics;
        result.desiredCrouched = (item.desired.flags & 7U) != 0U;
        result.desiredDeepCrouched = (item.desired.flags & 2U) != 0U;
        result.stage = "character";
        bool success{}, attempted{};
        auto animatedCrouched = item.animatedCrouched;
        auto nextAnimationRefresh = item.nextAnimationRefresh;
        std::uintptr_t cycle{}, owner{};
        std::uint8_t state{}, grappled{}, before{}, applied{}, deep{};
        std::uint32_t health{};
        if (!IsRemoteCharacter(item.character)) result.stage = "local-player-guard";
        else if (!Read(item.character + 0x36CU, cycle) || !Read(cycle + 4U, owner) || owner != item.character ||
            !Read(cycle + 0x1AU, state)) result.stage = "cycle";
        else if (state == 0x16U || item.count != 0U) result.stage = "traversal";
        else if (!Read(item.character + 0x297U, grappled) || grappled != 0U ||
            !Read(item.character + 0x32CU, health) || health >= 3U ||
            state == 0x42U || state == 0x4EU || state == 0x4FU || state == 0x47U) result.stage = "paired-or-dead";
        else {
            attempted = true;
            result.stage = "apply";
            const auto accepted = Read(item.character + 0x2AFU, before) && ApplyPosture(item.character, item.desired);
            if (accepted && RepairCrouchedContext(item.character, result.desiredCrouched))
                ++result.animationContextRepairs;
            const auto readback = Read(item.character + 0x2AFU, applied) && Read(item.character + 0x2B0U, deep);
            if (readback) {
                result.appliedCrouched = applied != 0U;
                result.appliedDeepCrouched = deep != 0U;
            }
            success = accepted && readback && (applied != 0U) == result.desiredCrouched &&
                (deep != 0U) == result.desiredDeepCrouched;
            result.stage = "apply-failed";
            if (success) {
                result.stage = applied != 0U ? "crouched" : "standing";
                if ((before != 0U) != (applied != 0U)) ++result.crouchTransitions;
            }
            SampleAnimation(item.character, result);
            // Replace the cached graph on a stance edge without routing Move
            // through Idle. Merely adding the context does not replace an
            // already constructed locomotion blend.
            if (success && gAnimationRefresh != nullptr &&
                (state == 1U || state == 0x38U) &&
                animatedCrouched != result.desiredCrouched &&
                GetTickCount64() >= nextAnimationRefresh) {
                if (gAnimationRefresh(item.character)) {
                    ++result.animationRefreshes;
                    animatedCrouched = result.desiredCrouched;
                    nextAnimationRefresh = 0U;
                } else {
                    ++result.animationRefreshFailures;
                    nextAnimationRefresh = GetTickCount64() + 250U;
                }
            }
        }
        std::lock_guard lock(gMutex);
        auto* slot = Find(item.character);
        if (slot == nullptr || slot->epoch != item.epoch) continue;
        // IPC can publish newer desired state during application. These fields
        // describe this engine application; never replace the newer request.
        if (success) ++result.postureApplications;
        else if (attempted || result.stage == std::string_view("local-player-guard") ||
            result.stage == std::string_view("cycle")) ++result.postureFailures;
        slot->diagnostics = result;
        slot->animatedCrouched = animatedCrouched;
        slot->nextAnimationRefresh = nextAnimationRefresh;
    }
}

void Forget(const std::uintptr_t character) noexcept {
    std::lock_guard lock(gMutex);
    if (auto* slot = Find(character); slot != nullptr) *slot = {};
}

void ForgetAll() noexcept {
    std::lock_guard lock(gMutex);
    for (auto& slot : gSlots) slot = {};
    player_animation_clip_capture::Clear();
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
