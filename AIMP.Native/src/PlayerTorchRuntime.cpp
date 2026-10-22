#include "aimp/PlayerTorchRuntime.h"

#include <Windows.h>
#include <array>
#include <mutex>

namespace aimp::player_torch {
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

// Reproduce only the Player_Torch_Light portion of EquipmentController's
// constructor. The existing controller owns the returned strong reference and
// destroys it through its normal reset/destructor; inventory is left intact.
bool Apply(const std::uintptr_t character, const bool enabled,
    std::uint32_t& activationHandle, bool& emitted, bool& created) noexcept {
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
        using Update = void(__thiscall*)(void*);
        // Both retail perception and equipment presentation observe this flag.
        actor[0x2BAU] = static_cast<std::uint8_t>(enabled);
        // Publish the NPC-visible state before show/hide emits its native
        // notification, so observers querying CHR_GetTorch see the new state.
        const auto torchManager = *reinterpret_cast<std::uintptr_t*>(actor + 0x34C8U);
        if (torchManager >= 0x10000U)
            *reinterpret_cast<std::uint8_t*>(torchManager + 8U) = static_cast<std::uint8_t>(enabled);
        if (*light != 0U) {
            reinterpret_cast<Update>(gModuleBase + (enabled ? 0x3C3700U : 0x3C3840U))(
                reinterpret_cast<void*>(equipment));
            if (enabled) reinterpret_cast<Update>(gModuleBase + 0x3ABFB0U)(reinterpret_cast<void*>(equipment));
        }
        emitted = *light != 0U && *reinterpret_cast<std::uint8_t*>(equipment + 0x1574U) != 0U;
        // CHR_GetTorch uses the NPC manager's effective flag for representations.
        if (torchManager >= 0x10000U)
            *reinterpret_cast<std::uint8_t*>(torchManager + 8U) = static_cast<std::uint8_t>(emitted);
        return emitted == enabled;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(character); static_cast<void>(enabled);
    static_cast<void>(activationHandle); static_cast<void>(emitted); static_cast<void>(created);
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
    bool emitted{}, created{};
    const auto success = Apply(character, state.diagnostics.desired, state.activationHandle, emitted, created);
    std::scoped_lock lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr || slot->epoch != state.epoch) return;
    slot->activationHandle = state.activationHandle;
    slot->diagnostics.emitted = emitted;
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
