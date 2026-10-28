#include "aimp/AISyncCombat.h"
#include "aimp/HookManager.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <initializer_list>

namespace aimp::ai_sync::combat {
namespace {
constexpr std::size_t kCapacity = 256U;
constexpr std::uintptr_t kShotDecision = 0x003C3B10U, kCurrentItem = 0x003829E0U;
constexpr std::uintptr_t kInventoryItem = 0x00374A10U, kEquipmentTransition = 0x00254E60U;
constexpr std::uintptr_t kCreateAim = 0x00070740U, kSubmitAim = 0x00071A50U;
constexpr std::uintptr_t kCancelAim = 0x00071AE0U, kTickAim = 0x00073540U;
constexpr std::uintptr_t kReleaseRef = 0x0000D770U, kAimType = 0x00DC7688U;
constexpr std::uintptr_t kCreateShotEvent = 0x003A6AA0U, kShotEventType = 0x00EF3F10U;
constexpr std::uintptr_t kSetAmmo = 0x00392200U, kSetWeaponMode = 0x0003A850U;
constexpr std::uintptr_t kAimStart = 0x00374AF0U, kAimStop = 0x00374B00U;
constexpr std::uintptr_t kFireStart = 0x00384A50U, kFireStop = 0x00384AC0U;
constexpr std::uintptr_t kChargeStart = 0x00384B30U, kChargeStop = 0x00384BA0U;
constexpr std::uintptr_t kSecondaryStart = 0x00384C10U;
struct Ref final { std::uintptr_t object{}, control{}; };
struct Lease final {
    Ref aim{};
    std::uintptr_t character{}, controller{}, manager{};
    std::uint32_t runtimeId{};
    std::uint64_t expires{}, tickStamp{};
    bool hasTick{}, aiming{};
};
struct Queued final { Observation observation{}; std::uint64_t epoch{}; };
std::array<Lease, kCapacity> gLeases{};
std::array<Queued, kCapacity> gQueue{};
std::size_t gRead{}, gWrite{};
SRWLOCK gQueueLock = SRWLOCK_INIT;
std::atomic<std::uint64_t> gEpoch{1U};
std::uintptr_t gBase{};
std::atomic<Mode> gMode{Mode::Retail};
std::atomic<std::uint64_t> gMissingMessageOwners{};
// Message 0x14 in 54A240 queries the optional reference at handler+0xC.
// The observed Client crash was ECX=0 at 524353. False already selects
// the retail source-message fallback at 54A61D; do not skip the message.
bool __fastcall MessageOwnerBound(void* owner, void*) noexcept {
    const auto mode = gMode.load(std::memory_order_acquire);
    if (owner == nullptr && (mode == Mode::Client || mode == Mode::FrozenClient)) {
        gMissingMessageOwners.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    return reinterpret_cast<bool(__fastcall*)(void*)>(gBase + 0x00124350U)(owner);
}
std::atomic<NpcPredicate> gNpc{};
std::atomic<void*> gDecisionOriginal{};
void* gDecisionTrampoline{};
std::atomic<std::uint64_t> gCaptured{}, gDropped{}, gDecisionsBlocked{}, gStateApplied{};
std::atomic<std::uint64_t> gShotsApplied{}, gDeferred{}, gFailed{}, gAimSubmitted{}, gAimCancelled{};

template<typename T> bool Read(const std::uintptr_t address, T& value) noexcept {
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
bool Point(const std::uintptr_t address, float (&point)[3]) noexcept {
    for (std::size_t i = 0U; i < 3U; ++i)
        if (!Read(address + i * sizeof(float), point[i]) || !std::isfinite(point[i]) ||
            std::abs(point[i]) > 100000.0F) return false;
    return true;
}
bool Controller(const std::uintptr_t character, const std::uint32_t runtimeId,
    std::uintptr_t& controller) noexcept {
    std::uint32_t currentId{};
    std::uintptr_t owner{};
    return character >= 0x10000U && Read(character + 0x50U, currentId) &&
        currentId == runtimeId && Read(character + 0x1100U, controller) && controller != 0U &&
        Read(controller + 0x1528U, owner) && owner == character;
}
bool Animated(const std::uintptr_t character, std::uintptr_t& animated) noexcept {
    std::uintptr_t table{}, getter{};
    if (!Read(character, table) || !Read(table + 0x1CU, getter) ||
        getter < gBase + 0x1000U || getter >= gBase + 0x00D9F000U) return false;
    __try {
        animated = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(
            reinterpret_cast<void*>(character));
        return animated >= 0x10000U;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool AimManager(const std::uintptr_t character, std::uintptr_t& manager) noexcept {
    std::uintptr_t animated{}, behaviour{}, owner{};
    return Animated(character, animated) && Read(animated + 0x34C4U, manager) && manager != 0U &&
        Read(manager, behaviour) && behaviour != 0U &&
        Read(behaviour + 0x128U, owner) && (owner == character || owner == animated);
}
bool CurrentItem(const std::uintptr_t controller, std::uintptr_t& item) noexcept {
    if (gBase == 0U || controller == 0U) return false;
    __try {
        item = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(gBase + kCurrentItem)(
            reinterpret_cast<void*>(controller));
        return item != 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WeaponMatches(const std::uintptr_t controller, const State& state,
    std::uintptr_t& item) noexcept {
    std::uint32_t slot{}, descriptor{};
    return Read(controller + 0x150CU, slot) && slot == state.slot && CurrentItem(controller, item) &&
        Read(item + 0xD0U, descriptor) && descriptor == state.descriptor;
}
bool Busy(const std::uintptr_t controller) noexcept {
    std::uint8_t pending{}, fire{}, ready{};
    return !Read(controller + 0xA0U, pending) || !Read(controller + 0xA3U, fire) ||
        !Read(controller + 0xA4U, ready) || pending != 0U || fire != 0U || ready != 0U;
}
bool RequestEquipment(const std::uintptr_t character, const std::uintptr_t controller,
    const State& state) noexcept {
    std::uint32_t oldSlot{};
    std::uintptr_t cycle{}, owner{};
    std::uint16_t kind{};
    std::uint8_t flags{}, equipped{};
    if (Busy(controller) || !Read(controller + 0x150CU, oldSlot) || oldSlot > 10U ||
        !Read(controller + 0x1510U, equipped) || !Read(character + 0x388U, cycle) || cycle == 0U ||
        !Read(cycle + 4U, owner) || owner != character || !Read(cycle + 0x1EU, kind) || kind == 7U ||
        !Read(cycle + 8U, flags) || (flags & 2U) == 0U) return false;
    const bool draw = (state.flags & Present) != 0U;
    if (!draw && equipped == 0U) return true;
    const auto slot = draw ? state.slot : oldSlot;
    __try {
        if (draw) {
            const auto item = reinterpret_cast<std::uintptr_t(__thiscall*)(void*, std::uint32_t)>(
                gBase + kInventoryItem)(reinterpret_cast<void*>(controller), slot);
            std::uint32_t descriptor{};
            if (item == 0U || !Read(item + 0xD0U, descriptor) || descriptor != state.descriptor) return false;
        }
        // Same value-only selection/holster boundary used by 006C8Dxx and
        // 0046Bxxx. Its Secondary cycle owns native graph/weapon markers.
        // Inventory objects are resolved locally; no BT or item is synthesized.
        using Transition = void(__thiscall*)(void*, std::uint32_t, std::uint32_t, std::uint32_t,
            std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
        reinterpret_cast<Transition>(gBase + kEquipmentTransition)(reinterpret_cast<void*>(cycle),
            !draw || oldSlot != slot ? 1U : 0U, oldSlot, 1U, draw ? 1U : 0U, slot, 1U, 0U, 1U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool LeaseLive(const Lease& lease) noexcept {
    std::uintptr_t controller{}, manager{}, type{}, owner{};
    if (!Controller(lease.character, lease.runtimeId, controller) || controller != lease.controller) return false;
    if (lease.aim.object == 0U) return lease.aim.control == 0U;
    return AimManager(lease.character, manager) && manager == lease.manager &&
        Read(lease.aim.object, type) && type == gBase + kAimType &&
        Read(lease.aim.object + 0x14U, owner) && owner == manager;
}
void ReleaseAim(Lease& lease, const bool cancelNative) noexcept {
    __try {
        if (cancelNative && lease.aim.object != 0U && LeaseLive(lease)) {
            reinterpret_cast<void(__thiscall*)(void*)>(gBase + kCancelAim)(
                reinterpret_cast<void*>(lease.aim.object));
            gAimCancelled.fetch_add(1U, std::memory_order_relaxed);
        }
        if (lease.aim.control != 0U)
            reinterpret_cast<void(__thiscall*)(Ref*)>(gBase + kReleaseRef)(&lease.aim);
    } __except (EXCEPTION_EXECUTE_HANDLER) { gFailed.fetch_add(1U, std::memory_order_relaxed); }
    lease.aim = {}; lease.manager = 0U; lease.hasTick = false;
}
void Release(Lease& lease, const bool cancelNative) noexcept {
    ReleaseAim(lease, cancelNative);
    lease = {};
}
Lease* FindLease(const std::uintptr_t character) noexcept {
    for (auto& lease : gLeases) if (lease.character == character) return &lease;
    return nullptr;
}
bool AimActive(const Lease& lease) noexcept {
    std::uint32_t status{};
    return lease.aim.object != 0U && LeaseLive(lease) &&
        Read(lease.aim.object + 0xCU, status) && (status == 1U || status == 2U);
}
bool TickAim(Lease& lease) noexcept {
    if (!AimActive(lease)) return false;
    std::uint64_t stamp{};
    if (!Read(gBase + 0x012410A8U, stamp)) return false;
    // Apply and Pump can be called in the same frame. E4's physical FSM and
    // its countdown are updated once per native game time, not per packet.
    if (lease.hasTick && lease.tickStamp == stamp) return true;
    __try {
        // This is manager E4's physical aim pipeline, not manager E0's firing
        // scheduler. Its sole owned request is kind4 with a resolved world point.
        reinterpret_cast<void(__thiscall*)(void*)>(gBase + kTickAim)(
            reinterpret_cast<void*>(lease.manager));
        lease.tickStamp = stamp; lease.hasTick = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SetAim(Lease& lease, const State& state) noexcept {
    if (lease.aim.object == 0U || !LeaseLive(lease)) return false;
    __try {
        // 0046F5C0 explicitly consumes these fields for kind4. No process-local
        // target ObjectRef, selected record, candidate score or BT is copied.
        *reinterpret_cast<std::uint32_t*>(lease.aim.object + 0x10U) = 4U;
        *reinterpret_cast<float*>(lease.aim.object + 0x28U) = state.turnTolerance;
        for (std::size_t i = 0U; i < 3U; ++i)
            *reinterpret_cast<float*>(lease.aim.object + 0x38U + i * sizeof(float)) = state.aim[i];
        *reinterpret_cast<std::uint8_t*>(lease.aim.object + 0x44U) = 0U;
        *reinterpret_cast<std::uint8_t*>(lease.aim.object + 0x2CU) = 1U;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool AdmitAim(Lease& lease) noexcept {
    __try {
        const auto result = reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(gBase + kSubmitAim)(
            reinterpret_cast<void*>(lease.aim.object));
        return result != 0U && AimActive(lease);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CreateAim(Lease& lease) noexcept {
    __try {
        reinterpret_cast<Ref*(__thiscall*)(void*, Ref*)>(gBase + kCreateAim)(
            reinterpret_cast<void*>(lease.manager), &lease.aim);
        return LeaseLive(lease);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeAimInput(const std::uintptr_t controller, const bool aiming) noexcept {
    std::uint8_t current{};
    if (!Read(controller + 0xA1U, current)) return false;
    if ((current != 0U) == aiming) return true;
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(gBase + (aiming ? kAimStart : kAimStop))(
            reinterpret_cast<void*>(controller));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Presentation(const std::uintptr_t controller, const std::uintptr_t item,
    const std::uint32_t flags) noexcept {
    std::uint8_t oldFire{}, oldSecondary{}, itemFlags{};
    if (!Read(controller + 0xA7U, oldFire) || !Read(controller + 0xA6U, oldSecondary) ||
        !Read(item + 0x141U, itemFlags)) return false;
    const auto fire = (flags >> 2U) & 1U;
    const auto secondary = (flags >> 1U) & 1U;
    __try {
        *reinterpret_cast<std::uint8_t*>(controller + 0xA5U) = static_cast<std::uint8_t>(flags & 1U);
        *reinterpret_cast<std::uint8_t*>(controller + 0xA6U) = static_cast<std::uint8_t>(secondary);
        *reinterpret_cast<std::uint8_t*>(controller + 0xA7U) = static_cast<std::uint8_t>(fire);
        // These are the same native message/callback producers used by
        // 007C3B10 after its Host-only decisions. They do not emit damage.
        if (oldSecondary != secondary && secondary != 0U)
            reinterpret_cast<void(__thiscall*)(void*)>(gBase + kSecondaryStart)(reinterpret_cast<void*>(item));
        if ((oldFire != 0U) != (fire != 0U)) {
            reinterpret_cast<void(__thiscall*)(void*)>(gBase + (fire != 0U ? kFireStart : kFireStop))(
                reinterpret_cast<void*>(item));
            if ((itemFlags & 2U) != 0U)
                reinterpret_cast<void(__thiscall*)(void*)>(gBase + (fire != 0U ? kChargeStart : kChargeStop))(
                    reinterpret_cast<void*>(item));
            if (fire == 0U && (itemFlags & 2U) != 0U) {
                const auto mode = *reinterpret_cast<const std::uintptr_t*>(controller + 0x152CU);
                if (mode != 0U && *reinterpret_cast<const std::uint32_t*>(mode + 4U) == 7U)
                    reinterpret_cast<void(__thiscall*)(void*, std::uint32_t, float)>(gBase + kSetWeaponMode)(
                        reinterpret_cast<void*>(mode), 6U, 0.0F);
            }
            if (fire != 0U) {
                *reinterpret_cast<std::uint32_t*>(controller + 0x1568U) = 0U;
                *reinterpret_cast<double*>(controller + 0x1550U) = 0.0;
                *reinterpret_cast<double*>(controller + 0x1558U) =
                    *reinterpret_cast<const double*>(gBase + 0x012410A8U);
                *reinterpret_cast<double*>(controller + 0x1560U) = 0.0;
                *reinterpret_cast<std::uint8_t*>(controller + 0x157DU) = 1U;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadCapture(const std::uintptr_t character, const std::uint32_t runtimeId, State& state,
    std::uintptr_t& controller, std::uintptr_t& item) noexcept {
    std::uint8_t aiming{}, presentation[3]{};
    if (!Controller(character, runtimeId, controller) || !CurrentItem(controller, item) ||
        !Read(controller + 0x150CU, state.slot) || !Read(item + 0xD0U, state.descriptor) ||
        !Read(controller + 0xA1U, aiming)) return false;
    for (std::size_t i = 0U; i < 3U; ++i) {
        if (!Read(controller + 0xA5U + i, presentation[i]) || presentation[i] > 1U) return false;
        state.presentationFlags |= static_cast<std::uint32_t>(presentation[i]) << i;
    }
    state.revision = 1U;
    state.flags = Present;
    // The current controller endpoint is already resolved by native aiming,
    // including the final native target point used for the raycast direction.
    if (Point(controller + 0x50U, state.aim)) {
        state.flags |= AimPoint;
        state.turnTolerance = 3.14159265F;
        std::uintptr_t manager{}, sentinel{}, node{}, request{};
        std::uint32_t count{};
        float tolerance{};
        if (AimManager(character, manager) && Read(manager + 0x18U, count) && count > 0U &&
            Read(manager + 0x14U, sentinel) && sentinel != 0U && Read(sentinel, node) &&
            node != sentinel && Read(node + 8U, request) && request != 0U &&
            Read(request + 0x28U, tolerance) && std::isfinite(tolerance) &&
            tolerance >= 0.0F && tolerance <= 3.141593F) state.turnTolerance = tolerance;
    }
    if (aiming != 0U) state.flags |= Aiming;
    return Valid(state);
}
void ObserveAccepted(const std::uintptr_t controller, const std::uint8_t before,
    const std::uint64_t epoch) noexcept {
    std::uint8_t after{}, requested{};
    if (before != 0U || !Read(controller + 0xA0U, after) || after == 0U ||
        !Read(controller + 0xA3U, requested) || requested != 1U) return;
    Observation observation{};
    if (!Read(controller + 0x1528U, observation.character) || observation.character == 0U ||
        !Read(observation.character + 0x50U, observation.runtimeId)) return;
    const auto predicate = gNpc.load(std::memory_order_acquire);
    if (predicate == nullptr || !predicate(observation.character)) return;
    std::uintptr_t matched{}, item{};
    observation.shot.revision = 1U;
    observation.shot.flags = 1U;
    if (!ReadCapture(observation.character, observation.runtimeId, observation.shot.state, matched, item) ||
        matched != controller || !Read(item + 0xE0U, observation.shot.ammoBefore) ||
        !Valid(observation.shot)) {
        gDropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (!TryAcquireSRWLockExclusive(&gQueueLock)) {
        gDropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (gMode.load(std::memory_order_acquire) == Mode::Host &&
        epoch == gEpoch.load(std::memory_order_acquire) && gWrite - gRead < kCapacity) {
        gQueue[gWrite++ % kCapacity] = {observation, epoch};
        gCaptured.fetch_add(1U, std::memory_order_relaxed);
    } else gDropped.fetch_add(1U, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&gQueueLock);
}
using Decision = std::uint8_t(__thiscall*)(void*);
std::uint8_t __fastcall DecisionDetour(void* raw, void*) noexcept {
    const auto original = reinterpret_cast<Decision>(gDecisionOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    const auto controller = reinterpret_cast<std::uintptr_t>(raw);
    std::uintptr_t character{};
    const auto mode = gMode.load(std::memory_order_acquire);
    const auto predicate = gNpc.load(std::memory_order_acquire);
    const bool npc = predicate != nullptr && Read(controller + 0x1528U, character) && predicate(character);
    if (npc && (mode == Mode::Client || mode == Mode::FrozenClient)) {
        // E0 is suppressed by AISync, but a stale A2 or a scripted equipment
        // call must not let the local weapon controller select another shot.
        std::uint8_t active{};
        static_cast<void>(Read(controller + 0xA0U, active));
        gDecisionsBlocked.fetch_add(1U, std::memory_order_relaxed);
        return active;
    }
    std::uint8_t before{};
    const auto epoch = gEpoch.load(std::memory_order_acquire);
    const bool observe = npc && mode == Mode::Host && Read(controller + 0xA0U, before);
    const auto result = original(raw);
    if (observe) ObserveAccepted(controller, before, epoch);
    return result;
}

bool AdmitShot(const std::uintptr_t character, const std::uintptr_t controller,
    const std::uintptr_t item, const ShotEvent& event) noexcept {
    // 007C41DD..007C430F is the accepted-shot block, after all readiness,
    // cooldown, charge and target decisions. A0/A3 admit the physical shot;
    // 0x13 is the accompanying 20-byte Character notification, allocated by
    // 007A6AA0. Its queue also has AI listeners; it is not an animation marker.
    // Manager E0 cadence accounting and its next random interval are Host-only.
    struct NativeEvent final { std::uint32_t type{}, value{}, kind{0x13U}, stamp{}, next{}; };
    static_assert(sizeof(NativeEvent) == 20U);
    std::uintptr_t animated{};
    if (!Animated(character, animated)) return false;
    __try {
        NativeEvent input{};
        input.type = static_cast<std::uint32_t>(gBase + kShotEventType);
        auto* node = reinterpret_cast<NativeEvent*(__thiscall*)(void*, const NativeEvent*)>(
            gBase + kCreateShotEvent)(reinterpret_cast<void*>(animated + 0x3C4CU), &input);
        if (node == nullptr || node->kind != 0x13U || node->type != input.type) return false;
        auto** head = reinterpret_cast<NativeEvent**>(animated + 0x3BACU + 0x13U * 4U);
        auto** append = head;
        std::size_t depth{};
        while (*append != nullptr) {
            if (++depth > 1024U) return false;
            append = reinterpret_cast<NativeEvent**>(&(*append)->next);
        }
        reinterpret_cast<void(__thiscall*)(void*, std::uint32_t)>(gBase + kSetAmmo)(
            reinterpret_cast<void*>(item), event.ammoBefore);
        *reinterpret_cast<std::uint8_t*>(controller + 0xA0U) = 1U;
        *reinterpret_cast<std::uint16_t*>(controller + 0xA3U) = 1U;
        node->stamp = *reinterpret_cast<const std::uint32_t*>(animated + 0x3C30U);
        *append = node;
        *reinterpret_cast<std::uint8_t*>(controller + 0xA8U) = event.state.slot == 3U ? 1U : 0U;
        const auto itemFlags = *reinterpret_cast<const std::uint8_t*>(item + 0x141U);
        if ((itemFlags & 2U) != 0U || event.state.slot == 3U) {
            const auto weaponMode = *reinterpret_cast<const std::uintptr_t*>(controller + 0x152CU);
            if (weaponMode == 0U) return false;
            // RET8: the native call has both mode=8 and blend=0.0, despite
            // the incomplete AI.exe.c prototype advertising only one argument.
            reinterpret_cast<void(__thiscall*)(void*, std::uint32_t, float)>(gBase + kSetWeaponMode)(
                reinterpret_cast<void*>(weaponMode), 8U, 0.0F);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
ApplyResult Deferred() noexcept { gDeferred.fetch_add(1U, std::memory_order_relaxed); return ApplyResult::Deferred; }
ApplyResult Failed() noexcept { gFailed.fetch_add(1U, std::memory_order_relaxed); return ApplyResult::Failed; }
} // namespace

bool Initialize(const std::uintptr_t base, std::string& error) {
    gBase = base;
    if (!Match(0x00124350U, {0x56,0x8B,0xF1,0x8B,0x46,0x28,0x85,0xC0}) ||
        !Match(0x0014A3B0U, {0x84,0xC0,0x0F,0x84,0x65,0x02,0,0}) ||
        !Match(kShotDecision, {0x64,0xA1,0,0,0,0}) ||
        !Match(kInventoryItem, {0x8B,0x44,0x24,0x04,0x83,0x3C,0x81,0x00}) ||
        !Match(0x00374A24U, {0xC2,0x04,0x00}) ||
        !Match(kEquipmentTransition, {0x56,0x8B,0xF1,0x57,0x85,0xF6}) ||
        !Match(0x00254EB0U, {0xC2,0x20,0x00}) ||
        !Match(0x0024E2F9U, {0xBA,0x07,0x00,0x00,0x00,0x66,0x89,0x50,0x1E}) ||
        !Match(kCurrentItem, {0x80,0xB9,0x10,0x15,0,0,0}) ||
        !Match(kCreateAim, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) ||
        !Match(kCreateAim + 0x18U, {0xC2,0x04,0}) ||
        !Match(kSubmitAim + 0x8FU, {0xC3}) || !Match(kCancelAim, {0x64,0xA1,0,0,0,0}) ||
        !Match(kTickAim, {0x56,0x8B,0xF1,0x8A,0x46,0x10,0x84,0xC0}) ||
        !Match(kReleaseRef, {0x56,0x8B,0x71,0x04}) ||
        !Match(kCreateShotEvent + 0x83U, {0xC2,0x04,0}) ||
        !Match(0x003C41DDU, {0xC6,0x86,0xA0,0,0,0,1,0x66,0xC7,0x86,0xA3,0,0,0,1,0}) ||
        !Match(0x003C4247U, {0xC7,0x44,0x24,0x1C,0x13,0,0,0}) ||
        !Match(kSetAmmo + 0x89U, {0xC2,0x04,0}) ||
        !Match(kSetWeaponMode, {0x56,0x8B,0xF1,0x80,0x7E,8,0}) ||
        !Match(0x003C4303U, {0xF3,0x0F,0x11,0x04,0x24,0x6A,8}) ||
        !Match(kAimStart, {0xC6,0x81,0xA1,0,0,0,1,0xC3}) ||
        !Match(kAimStop, {0x56,0x57,0xC6,0x81,0xA1,0,0,0,0}) ||
        !Match(kFireStart, {0x0F,0x57,0xC0,0x56,0x57}) ||
        !Match(kFireStop, {0x0F,0x57,0xC0,0x56,0x57}) ||
        !Match(kChargeStart, {0x0F,0x57,0xC0,0x56,0x57}) ||
        !Match(kChargeStop, {0x0F,0x57,0xC0,0x56,0x57}) ||
        !Match(kSecondaryStart, {0x0F,0x57,0xC0,0x56,0x57})) {
        error = "AISync native combat ABI mismatch";
        gBase = 0U;
        return false;
    }
    return true;
}
bool Install(HookManager& hooks, std::string& error) {
    if (gBase == 0U) { error = "AISync combat is uninitialized"; return false; }
    const auto checkpoint = hooks.InstalledCount();
    if (!hooks.InstallCallRedirect({"aisync_client_message_owner",
        reinterpret_cast<void*>(gBase + 0x0014A3ABU),
        reinterpret_cast<void*>(gBase + 0x00124350U),
        reinterpret_cast<void*>(&MessageOwnerBound)}, error)) return false;
    HookRequest request;
    request.name = "aisync_host_resolved_shot";
    request.target = reinterpret_cast<void*>(gBase + kShotDecision);
    request.detour = reinterpret_cast<void*>(&DecisionDetour);
    request.expected.bytes = {0x64U,0xA1U,0U,0U,0U,0U};
    request.expected.mask.assign(request.expected.bytes.size(), 'x');
    request.patchLength = request.expected.bytes.size();
    request.relocationSafePrefix = true;
    request.trampolineOut = &gDecisionTrampoline;
    request.publishedTrampolineOut = &gDecisionOriginal;
    if (hooks.Install(request, error)) return true;
    error = request.name + ": " + error;
    std::string rollbackError;
    if (!hooks.UninstallFrom(checkpoint, rollbackError)) error += "; rollback: " + rollbackError;
    return false;
}
void SetNpcPredicate(const NpcPredicate predicate) noexcept { gNpc.store(predicate, std::memory_order_release); }
void SetMode(const Mode mode) noexcept {
    if (gMode.exchange(mode, std::memory_order_acq_rel) != mode) ResetObservations();
}
void ResetObservations() noexcept { gEpoch.fetch_add(1U, std::memory_order_acq_rel); }
bool TryPopObservation(Observation& observation) noexcept {
    observation = {};
    if (!TryAcquireSRWLockExclusive(&gQueueLock)) return false;
    const auto epoch = gEpoch.load(std::memory_order_acquire);
    bool result{};
    while (gRead != gWrite) {
        const auto queued = gQueue[gRead++ % kCapacity];
        if (queued.epoch == epoch && gMode.load(std::memory_order_acquire) == Mode::Host) {
            observation = queued.observation; result = true; break;
        }
    }
    ReleaseSRWLockExclusive(&gQueueLock);
    return result;
}
bool Capture(const std::uintptr_t character, State& state) noexcept {
    state = {};
    std::uint32_t runtimeId{};
    std::uintptr_t controller{}, item{};
    if (gBase == 0U || !Read(character + 0x50U, runtimeId) ||
        !Controller(character, runtimeId, controller)) { state = {}; return false; }
    std::uint8_t equipped{};
    if (Read(controller + 0x1510U, equipped) && equipped == 0U) return true;
    if (!ReadCapture(character, runtimeId, state, controller, item)) { state = {}; return false; }
    return true;
}
ApplyResult ApplyState(const std::uintptr_t character, const std::uint32_t runtimeId,
    const State& state, const std::uint64_t expiresAt) noexcept {
    if (gBase == 0U || !Valid(state)) return Failed();
    if (gMode.load(std::memory_order_acquire) != Mode::Client) return Deferred();
    // No equipment observation is different from an explicit Host holster.
    // NPC families without this controller need no combat presentation work.
    if (state.flags == 0U && state.revision == 0U) { Cancel(character); return ApplyResult::AlreadyMatched; }
    std::uintptr_t controller{};
    if (!Controller(character, runtimeId, controller)) return Failed();
    if (state.flags == 0U) {
        Cancel(character);
        std::uint8_t equipped{};
        if (!Read(controller + 0x1510U, equipped)) return Deferred();
        if (equipped != 0U) { static_cast<void>(RequestEquipment(character, controller, state)); return Deferred(); }
        return ApplyResult::Applied;
    }
    std::uintptr_t item{};
    if (!WeaponMatches(controller, state, item)) {
        static_cast<void>(RequestEquipment(character, controller, state));
        return Deferred();
    }
    auto* lease = FindLease(character);
    if (lease != nullptr && !LeaseLive(*lease)) { Release(*lease, true); lease = nullptr; }
    if (lease != nullptr) lease->expires = expiresAt;
    // A newer idle/weapon snapshot must not cancel an admitted older shot's
    // pending raycasts. Its physical completion is independent of Host HP.
    if (Busy(controller)) return Deferred();
    if (lease == nullptr) {
        for (auto& available : gLeases) if (available.character == 0U) { lease = &available; break; }
        if (lease == nullptr) return Deferred();
        lease->character = character; lease->runtimeId = runtimeId;
        lease->controller = controller; lease->expires = expiresAt;
    }
    lease->aiming = (state.flags & Aiming) != 0U;
    if ((state.flags & AimPoint) != 0U) {
        if (lease->aim.object != 0U && !AimActive(*lease)) ReleaseAim(*lease, true);
        if (lease->aim.object == 0U) {
            std::uintptr_t manager{};
            if (!AimManager(character, manager)) return Deferred();
            lease->manager = manager;
            if (!CreateAim(*lease) || !SetAim(*lease, state) || !AdmitAim(*lease)) {
                Release(*lease, true); return Failed();
            }
            gAimSubmitted.fetch_add(1U, std::memory_order_relaxed);
        } else if (!SetAim(*lease, state)) return Failed();
        if (!TickAim(*lease)) return Deferred();
    } else if (lease->aim.object != 0U) ReleaseAim(*lease, true);
    if (!NativeAimInput(controller, (state.flags & Aiming) != 0U) ||
        !Presentation(controller, item, state.presentationFlags)) return Failed();
    gStateApplied.fetch_add(1U, std::memory_order_relaxed);
    return ApplyResult::Applied;
}
ApplyResult ApplyShot(const std::uintptr_t character, const std::uint32_t runtimeId,
    const ShotEvent& event, ReplayCursor& cursor, const std::uint64_t expiresAt) noexcept {
    if (gBase == 0U || !Valid(event) || event.flags == 0U) return Failed();
    if (gMode.load(std::memory_order_acquire) != Mode::Client || cursor.busy) return Deferred();
    if (cursor.character != 0U && (cursor.character != character || cursor.runtimeId != runtimeId)) return Failed();
    struct Guard final { bool& busy; explicit Guard(bool& value) noexcept : busy(value) { busy = true; } ~Guard() { busy = false; } } guard(cursor.busy);
    std::uintptr_t controller{}, item{};
    if (!Controller(character, runtimeId, controller)) return Failed();
    if (event.revision < cursor.revision) return ApplyResult::AlreadyMatched;
    if (event.revision == cursor.revision && cursor.invoked) {
        if (std::memcmp(&cursor.admitted, &event, sizeof(event)) != 0) return Failed();
        return cursor.failed ? ApplyResult::Failed : ApplyResult::AlreadyMatched;
    }
    if (!WeaponMatches(controller, event.state, item)) {
        static_cast<void>(RequestEquipment(character, controller, event.state)); return Deferred();
    }
    if (Busy(controller)) return Deferred();
    const auto prepared = ApplyState(character, runtimeId, event.state, expiresAt);
    if (prepared != ApplyResult::Applied && prepared != ApplyResult::AlreadyMatched) return prepared;
    std::uintptr_t currentController{}, currentItem{};
    if (gMode.load(std::memory_order_acquire) != Mode::Client ||
        !Controller(character, runtimeId, currentController) || currentController != controller ||
        !WeaponMatches(controller, event.state, currentItem) || currentItem != item) return Deferred();
    cursor.character = character; cursor.runtimeId = runtimeId; cursor.admitted = event;
    cursor.revision = event.revision;
    cursor.invoked = true; // Includes partial native failures: never submit a shot twice.
    cursor.failed = !AdmitShot(character, controller, item, event);
    if (cursor.failed) return Failed();
    gShotsApplied.fetch_add(1U, std::memory_order_relaxed);
    return ApplyResult::Applied;
}
void Cancel(const std::uintptr_t character) noexcept {
    if (auto* lease = FindLease(character); lease != nullptr) {
        const auto controller = lease->controller;
        const bool live = LeaseLive(*lease);
        std::uintptr_t item{};
        if (live && CurrentItem(controller, item)) static_cast<void>(Presentation(controller, item, 0U));
        Release(*lease, true);
        if (live && !Busy(controller)) static_cast<void>(NativeAimInput(controller, false));
    }
}
void Pump(const std::uint64_t now) noexcept {
    const bool client = gMode.load(std::memory_order_acquire) == Mode::Client;
    for (auto& lease : gLeases) {
        if (lease.character == 0U) continue;
        if (!client || now >= lease.expires || !LeaseLive(lease)) { Cancel(lease.character); continue; }
        if (lease.aim.object != 0U) {
            if (!AimActive(lease)) { ReleaseAim(lease, true); continue; }
            if (!TickAim(lease) || !NativeAimInput(lease.controller, lease.aiming))
                gFailed.fetch_add(1U, std::memory_order_relaxed);
        }
    }
}
void Reset(const bool cancelNative) noexcept {
    ResetObservations();
    for (auto& lease : gLeases) if (lease.character != 0U) {
        if (cancelNative) Cancel(lease.character); else Release(lease, false);
    }
}
Counters Snapshot() noexcept {
    return {gCaptured.load(std::memory_order_relaxed), gDropped.load(std::memory_order_relaxed),
        gDecisionsBlocked.load(std::memory_order_relaxed), gStateApplied.load(std::memory_order_relaxed),
        gShotsApplied.load(std::memory_order_relaxed), gDeferred.load(std::memory_order_relaxed),
        gFailed.load(std::memory_order_relaxed), gAimSubmitted.load(std::memory_order_relaxed),
        gAimCancelled.load(std::memory_order_relaxed), gMissingMessageOwners.load(std::memory_order_relaxed)};
}
} // namespace aimp::ai_sync::combat
