#include "aimp/PlayerTorchRuntime.h"

#include <Windows.h>
#include <array>
#include <cmath>
#include <mutex>

namespace aimp::player_torch {
bool BuildYawTransform(const float yaw, const std::array<float, 3>& origin,
    std::array<float, 12>& output) noexcept {
    if (!std::isfinite(yaw)) return false;
    for (const auto value : origin) if (!std::isfinite(value)) return false;
    const auto s = std::sin(yaw), c = std::cos(yaw);
    output = {c, 0, s, origin[0], 0, 1, 0, origin[1], -s, 0, c, origin[2]};
    return true;
}
namespace {
struct Slot final {
    std::uintptr_t character{};
    std::uint64_t epoch{}, retryAt{};
    std::uint32_t activationHandle{};
    DiagnosticsSnapshot diagnostics{};
};
std::array<Slot, 40> gSlots{};
std::mutex gMutex;
std::uintptr_t gModuleBase{};
std::uint64_t gEpoch{};

Slot* Find(const std::uintptr_t character) noexcept {
    for (auto& slot : gSlots) if (slot.character == character) return &slot;
    return nullptr;
}

#if defined(_MSC_VER) && defined(_M_IX86)
// Certified AI.exe SHA256 00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364.
// 7AC0BA..107: lookup returns a counted bone (+6C), accessor uses ECX=bone,
// one output argument / RET4. Release the bone even if its transform faults.
bool ReadSocketOrigin(const std::uintptr_t character, std::array<float, 3>& origin) {
    using FindBone = std::uintptr_t*(__thiscall*)(void*, std::uintptr_t*, const char*);
    using BoneTransform = float*(__thiscall*)(void*, float*);
    std::uintptr_t bone{};
    bool found{};
    __try {
        reinterpret_cast<FindBone>(gModuleBase + 0x2B4E70U)(
            reinterpret_cast<void*>(character + 0x3F0U), &bone, "FLASHLIGHT");
        if (bone >= 0x10000U) {
            alignas(16) float matrix[16]{};
            reinterpret_cast<BoneTransform>(gModuleBase + 0xBE1F0U)(
                reinterpret_cast<void*>(bone), matrix);
            // Accessor expands compact rows to a column-major 4x4.
            origin = {matrix[12], matrix[13], matrix[14]};
            found = true;
        }
    } __finally {
        if (bone >= 0x10000U) --*reinterpret_cast<std::int32_t*>(bone + 0x6CU);
    }
    return found;
}

bool UpdateRemoteLight(const std::uintptr_t character, std::uint32_t* const light,
    bool& socketOrigin) {
    // Actor +174 is the replicated yaw used by CharacterDirectStepRuntime.
    const auto yaw = *reinterpret_cast<const float*>(character + 0x174U);
    std::array<float, 3> origin{};
    socketOrigin = ReadSocketOrigin(character, origin);
    if (!socketOrigin) {
        // Skeletons without FLASHLIGHT still get an actor-bound origin. This
        // shoulder-height approximation is deliberately independent of camera.
        const auto* position = reinterpret_cast<const float*>(character + 0x70U);
        origin = {position[0], position[1] + 1.5F, position[2]};
    }
    alignas(16) std::array<float, 12> transform{};
    if (!BuildYawTransform(yaw, origin, transform)) return false;
    // 7AC502..531 transposes 4x4 to these compact rows then calls 8BC500;
    // 8BC524/558 read TWO stack args and 8BC584 RET (caller cleanup).
    // The setter borrows the controller's ref, guards it with 932CC0, and
    // dispatches the native transform service (+70 -> +60). No ref transfer.
    using SetTransform = void(__cdecl*)(const std::uint32_t*, const float*);
    reinterpret_cast<SetTransform>(gModuleBase + 0x4BC500U)(light, transform.data());
    return true;
}
#endif

// Reproduce only the Player_Torch_Light portion of EquipmentController's
// constructor. The existing controller owns the returned strong reference and
// destroys it through its normal reset/destructor; inventory is left intact.
bool Apply(const std::uintptr_t character, const bool enabled,
    std::uint32_t& activationHandle, bool& emitted, bool& created,
    bool& transformSubmitted, bool& socketOrigin) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        auto* actor = reinterpret_cast<std::uint8_t*>(character);
        const auto definition = *reinterpret_cast<std::uintptr_t*>(actor + 0x64U);
        if (definition < 0x10000U ||
            (*reinterpret_cast<std::uint8_t*>(definition + 0x1CU) != 0U &&
             *reinterpret_cast<std::uint8_t*>(definition + 0x1FU) != 0U)) return false;
        const auto equipment = *reinterpret_cast<std::uintptr_t*>(actor + 0x1100U);
        if (equipment < 0x10000U ||
            *reinterpret_cast<std::uintptr_t*>(equipment + 0x1528U) != character) return false;
        auto* light = reinterpret_cast<std::uint32_t*>(equipment + 0x156CU);
        if (enabled && *light == 0U) {
            auto* manager = *reinterpret_cast<void**>(gModuleBase + 0x134EF40U);
            if (manager == nullptr) return false;
            using FindAsset = void(__thiscall*)(void*, std::uint32_t*, const char*);
            using Instantiate = std::uint32_t*(__thiscall*)(void*, std::uint32_t*,
                const std::uint32_t*, const float*);
            std::uint32_t asset = *reinterpret_cast<std::uint32_t*>(gModuleBase + 0x134E254U);
            reinterpret_cast<FindAsset>(gModuleBase + 0x522520U)(manager, &asset, "Player_Torch_Light");
            if (asset == *reinterpret_cast<std::uint32_t*>(gModuleBase + 0x134E254U)) return false;
            const float identity[12]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            // Transfer the native output reference directly to the empty owner
            // field: no extra reference or custom destructor is introduced.
            reinterpret_cast<Instantiate>(gModuleBase + 0x530950U)(manager, light, &asset, identity);
            if (*light == 0U) return false;
            activationHandle = *light;
            created = true;
        }
        if (*light != 0U && activationHandle == *light) {
            auto* entity = *reinterpret_cast<void**>(static_cast<std::uintptr_t>(*light) + 0xCU);
            if (entity == nullptr) return false;
            using SetKind = void(__thiscall*)(void*, std::uint8_t);
            using CopyRef = std::uint32_t*(__thiscall*)(std::uint32_t*, const std::uint32_t*);
            using Activate = bool(__thiscall*)(void*, std::uint32_t);
            reinterpret_cast<SetKind>(gModuleBase + 0x62A900U)(entity, 7U);
            std::uint32_t copy{};
            reinterpret_cast<CopyRef>(gModuleBase + 0x673C0U)(&copy, light);
            // Activate consumes the by-value strong-reference copy (ret 4).
            if (!reinterpret_cast<Activate>(gModuleBase + 0x524B60U)(
                    *reinterpret_cast<void**>(gModuleBase + 0x134EF40U), copy)) return false;
            activationHandle = 0U;
        }
        // Never call 7ABFB0 here: 7AC055 onward resolves the process-global
        // camera even for an NPC light. Place the remote light before showing.
        transformSubmitted = enabled && *light != 0U &&
            UpdateRemoteLight(character, light, socketOrigin);
        const auto effectiveEnabled = enabled && transformSubmitted;
        using Update = void(__thiscall*)(void*);
        // Both retail perception and equipment presentation observe this flag.
        actor[0x2BAU] = static_cast<std::uint8_t>(effectiveEnabled);
        // Publish the NPC-visible state before show/hide emits its native
        // notification, so observers querying CHR_GetTorch see the new state.
        const auto torchManager = *reinterpret_cast<std::uintptr_t*>(actor + 0x34C8U);
        if (torchManager >= 0x10000U)
            *reinterpret_cast<std::uint8_t*>(torchManager + 8U) = static_cast<std::uint8_t>(effectiveEnabled);
        if (*light != 0U) {
            reinterpret_cast<Update>(gModuleBase + (effectiveEnabled ? 0x3C3700U : 0x3C3840U))(
                reinterpret_cast<void*>(equipment));
        }
        emitted = *light != 0U && *reinterpret_cast<std::uint8_t*>(equipment + 0x1574U) != 0U;
        // CHR_GetTorch uses the NPC manager's effective flag for representations.
        if (torchManager >= 0x10000U)
            *reinterpret_cast<std::uint8_t*>(torchManager + 8U) = static_cast<std::uint8_t>(emitted);
        return emitted == enabled && (!enabled || transformSubmitted);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(character); static_cast<void>(enabled);
    static_cast<void>(activationHandle); static_cast<void>(emitted); static_cast<void>(created);
    static_cast<void>(transformSubmitted); static_cast<void>(socketOrigin);
    return false;
#endif
}
}

void Initialize(const std::uintptr_t moduleBase) noexcept {
    std::scoped_lock lock(gMutex);
    gModuleBase = moduleBase;
    gSlots = {};
    ++gEpoch;
}

bool Publish(const std::uintptr_t character, const bool enabled, std::string& error) noexcept {
    if (character < 0x10000U) { error = "Invalid remote torch Character."; return false; }
    std::scoped_lock lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr) {
        for (auto& candidate : gSlots) if (candidate.character == 0U) { slot = &candidate; break; }
        if (slot == nullptr) { error = "Remote torch mailbox capacity reached."; return false; }
        slot->character = character;
        slot->epoch = ++gEpoch;
    }
    if (slot->diagnostics.desired != enabled) slot->retryAt = 0U;
    slot->diagnostics.desired = enabled;
    return true;
}

void PumpOnEngineThread(const std::uintptr_t character) noexcept {
    Slot state{};
    {
        std::scoped_lock lock(gMutex);
        const auto* slot = Find(character);
        if (slot == nullptr || gModuleBase == 0U || GetTickCount64() < slot->retryAt) return;
        state = *slot;
    }
    bool emitted{}, created{}, transformSubmitted{}, socketOrigin{};
    const auto success = Apply(character, state.diagnostics.desired, state.activationHandle,
        emitted, created, transformSubmitted, socketOrigin);
    std::scoped_lock lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr || slot->epoch != state.epoch) return;
    slot->activationHandle = state.activationHandle;
    slot->diagnostics.emitted = emitted;
    slot->diagnostics.transformSubmitted = transformSubmitted;
    slot->diagnostics.socketOrigin = socketOrigin;
    slot->retryAt = success || slot->diagnostics.desired != state.diagnostics.desired
        ? 0U : GetTickCount64() + 500U;
    if (success) ++slot->diagnostics.applications;
    else ++slot->diagnostics.failures;
    if (created) ++slot->diagnostics.creations;
}

void Forget(const std::uintptr_t character) noexcept {
    std::scoped_lock lock(gMutex);
    if (auto* slot = Find(character)) *slot = {};
}
void ForgetAll() noexcept {
    std::scoped_lock lock(gMutex);
    gSlots = {};
    ++gEpoch;
}
DiagnosticsSnapshot Snapshot(const std::uintptr_t character) noexcept {
    std::scoped_lock lock(gMutex);
    const auto* slot = Find(character);
    return slot == nullptr ? DiagnosticsSnapshot{} : slot->diagnostics;
}
}
