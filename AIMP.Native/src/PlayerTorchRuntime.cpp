#include "aimp/PlayerTorchRuntime.h"

#include <Windows.h>
#include <array>
#include <charconv>
#include <cmath>
#include <mutex>

namespace aimp::player_torch {
bool ValidDirection(const std::array<float, 3>& direction) noexcept {
    float length{};
    for (const auto value : direction) {
        if (!std::isfinite(value)) return false;
        length += value * value;
    }
    return length >= 0.99F && length <= 1.01F;
}
bool DecodeDirection(std::string_view text, std::array<float, 3>& direction) noexcept {
    if (text.empty() || text.size() > 96U) return false;
    std::array<float, 3> result{};
    for (std::size_t i = 0U; i < result.size(); ++i) {
        const auto comma = text.find(',');
        if ((i < 2U) != (comma != std::string_view::npos)) return false;
        const auto field = text.substr(0U, comma);
        const auto parsed = std::from_chars(field.data(), field.data() + field.size(), result[i]);
        if (field.empty() || parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) return false;
        if (i < 2U) text.remove_prefix(comma + 1U);
    }
    if (!ValidDirection(result)) return false;
    direction = result;
    return true;
}
bool BuildAimTransform(const std::array<float, 3>& direction, const std::array<float, 3>& origin,
    std::array<float, 12>& output) noexcept {
    if (!ValidDirection(direction)) return false;
    for (const auto value : origin) if (!std::isfinite(value)) return false;
    const auto invLength = 1.0F / std::sqrt(direction[0] * direction[0] +
        direction[1] * direction[1] + direction[2] * direction[2]);
    const auto x = direction[0] * invLength, y = direction[1] * invLength, z = direction[2] * invLength;
    const auto horizontal = std::sqrt(x * x + z * z);
    // At vertical aim the right axis is arbitrary, but the beam stays finite.
    const auto rx = horizontal > 0.0001F ? z / horizontal : 1.0F;
    const auto rz = horizontal > 0.0001F ? -x / horizontal : 0.0F;
    output = {rx, y * rz, x, origin[0], 0, z * rx - x * rz, y, origin[1],
        rz, -y * rx, z, origin[2]};
    return true;
}
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
    std::array<float, 3> direction{};
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

bool TrySocketOrigin(const std::uintptr_t character, std::array<float, 3>& origin) noexcept {
    __try {
        if (!ReadSocketOrigin(character, origin)) return false;
        for (const auto value : origin) if (!std::isfinite(value)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool UpdateRemoteLight(const std::uintptr_t character, std::uint32_t* const light,
    const std::array<float, 3>* direction, bool& socketOrigin) {
    // Actor +174 is the replicated yaw used by CharacterDirectStepRuntime.
    const auto yaw = *reinterpret_cast<const float*>(character + 0x174U);
    std::array<float, 3> origin{};
    socketOrigin = TrySocketOrigin(character, origin);
    if (!socketOrigin) {
        // Skeletons without FLASHLIGHT still get an actor-bound origin. This
        // shoulder-height approximation is deliberately independent of camera.
        const auto* position = reinterpret_cast<const float*>(character + 0x70U);
        origin = {position[0], position[1] + 1.5F, position[2]};
    }
    alignas(16) std::array<float, 12> transform{};
    if (direction != nullptr) {
        if (!BuildAimTransform(*direction, origin, transform)) return false;
    } else if (!BuildYawTransform(yaw, origin, transform)) return false;
    // Follow the animated socket, but emit beyond the character's skin. Keep
    // this clearance along the owner's look ray (including pitch), rather than
    // an animated bone axis that can swing the beam back into the body.
    constexpr float forwardClearance = 0.25F;
    transform[3] += forwardClearance * transform[2];
    transform[7] += forwardClearance * transform[6];
    transform[11] += forwardClearance * transform[10];
    // 7AC502..531 transposes 4x4 to these compact rows then calls 8BC500;
    // 8BC524/558 read TWO stack args and 8BC584 RET (caller cleanup).
    // The setter borrows the controller's ref, guards it with 932CC0, and
    // dispatches the native transform service (+70 -> +60). No ref transfer.
    using SetTransform = void(__cdecl*)(const std::uint32_t*, const float*);
    reinterpret_cast<SetTransform>(gModuleBase + 0x4BC500U)(light, transform.data());
    return true;
}

// Resolve the renderer light, rather than trusting equipment +1574. Retail
// 7C3700 sets that byte even when its scheduled "show" produces no light.
// TemporaryEntity +24 owns CompositeInstance; its +2C counted array owns the
// child entities (9644A0). LightReference +30 owns the render proxy; +8C on that
// proxy returns the light used by 631A70/632260. All offsets below are for the
// certified executable above, and the class checks deliberately fail closed.
std::uintptr_t RendererLight(const std::uint32_t handle, const char*& stage) {
    stage = "light-instance";
    if (handle < 0x10000U) return 0U;
    const auto temporary = *reinterpret_cast<const std::uintptr_t*>(handle + 0xCU);
    if (temporary < 0x10000U ||
        *reinterpret_cast<const std::uintptr_t*>(temporary) != gModuleBase + 0xFC8024U) return 0U;
    const auto compositeRef = *reinterpret_cast<const std::uintptr_t*>(temporary + 0x24U);
    if (compositeRef < 0x10000U) return 0U;
    const auto composite = *reinterpret_cast<const std::uintptr_t*>(compositeRef + 0xCU);
    if (composite < 0x10000U ||
        *reinterpret_cast<const std::uintptr_t*>(composite) != gModuleBase + 0xFE1D2CU) return 0U;
    const auto children = *reinterpret_cast<const std::uintptr_t*>(composite + 0x2CU);
    if (children < 0x10000U) return 0U;
    const auto count = *reinterpret_cast<const std::uint32_t*>(children + 8U);
    const auto data = *reinterpret_cast<const std::uintptr_t*>(children + 0xCU);
    if (count == 0U || count > 8U || data < 0x10000U) return 0U;
    for (std::uint32_t i = 0U; i < count; ++i) {
        const auto childRef = reinterpret_cast<const std::uintptr_t*>(data)[i];
        if (childRef < 0x10000U) continue;
        const auto child = *reinterpret_cast<const std::uintptr_t*>(childRef + 0xCU);
        if (child < 0x10000U ||
            *reinterpret_cast<const std::uintptr_t*>(child) != gModuleBase + 0xEA5EB0U) continue;
        stage = "render-proxy";
        const auto* renderRef = reinterpret_cast<const std::uint32_t*>(child + 0x30U);
        if (*renderRef < 0x10000U) return 0U;
        const auto proxy = *reinterpret_cast<const std::uintptr_t*>(*renderRef + 0xCU);
        if (proxy < 0x10000U) return 0U;
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(proxy);
        const auto getter = *reinterpret_cast<const std::uintptr_t*>(vtable + 0x8CU);
        if (getter < gModuleBase + 0x1000U || getter >= gModuleBase + 0xD9E13BU) return 0U;
        using GetLight = std::uintptr_t(__thiscall*)(void*, const std::uint32_t*);
        return reinterpret_cast<GetLight>(getter)(reinterpret_cast<void*>(proxy), renderRef);
    }
    return 0U;
}
#endif

// Use the dedicated AIMP light graph installed during game preparation. Retail
// Player_Torch_Light also owns local input/battery/camera logic and must never be
// instantiated for a remote actor. The equipment controller owns the reference.
bool Apply(const std::uintptr_t character, const bool enabled,
    std::uint32_t& activationHandle, bool& emitted, bool& created,
    bool& transformSubmitted, bool& socketOrigin, std::uintptr_t& renderer,
    const std::array<float, 3>* direction,
    const char*& stage) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        auto* actor = reinterpret_cast<std::uint8_t*>(character);
        stage = "character";
        const auto definition = *reinterpret_cast<std::uintptr_t*>(actor + 0x64U);
        if (definition < 0x10000U) return false;
        // A representation can reuse player-authored definition flags. Identify
        // the actual local player through retail's table, never its archetype.
        stage = "local-player-guard";
        const auto root = *reinterpret_cast<const std::uintptr_t*>(gModuleBase + 0x12F0C88U);
        if (root < 0x10000U) return false;
        const auto context = *reinterpret_cast<const std::uintptr_t*>(root + 8U);
        if (context < 0x10000U) return false;
        const auto count = *reinterpret_cast<const std::uint32_t*>(context + 0x84U);
        const auto players = *reinterpret_cast<const std::uintptr_t*>(context + 0x80U);
        if (count == 0U || count > 4U || players < 0x10000U) return false;
        for (std::uint32_t i = 0U; i < count; ++i)
            if (reinterpret_cast<const std::uintptr_t*>(players)[i] == character) return false;
        const auto equipment = *reinterpret_cast<std::uintptr_t*>(actor + 0x1100U);
        stage = "equipment";
        if (equipment < 0x10000U ||
            *reinterpret_cast<std::uintptr_t*>(equipment + 0x1528U) != character) return false;
        auto* light = reinterpret_cast<std::uint32_t*>(equipment + 0x156CU);
        if (enabled && *light == 0U) {
            stage = "asset";
            auto* manager = *reinterpret_cast<void**>(gModuleBase + 0x134EF40U);
            if (manager == nullptr) return false;
            using FindAsset = void(__thiscall*)(void*, std::uint32_t*, const char*);
            using Instantiate = std::uint32_t*(__thiscall*)(void*, std::uint32_t*,
                const std::uint32_t*, const float*);
            std::uint32_t asset = *reinterpret_cast<std::uint32_t*>(gModuleBase + 0x134E254U);
            reinterpret_cast<FindAsset>(gModuleBase + 0x522520U)(manager, &asset, "Remote_Torch_Light_v2");
            if (asset == *reinterpret_cast<std::uint32_t*>(gModuleBase + 0x134E254U))
                reinterpret_cast<FindAsset>(gModuleBase + 0x522520U)(manager, &asset, "AIMP\\Remote_Torch_Light_v2");
            if (asset == *reinterpret_cast<std::uint32_t*>(gModuleBase + 0x134E254U)) return false;
            alignas(16) const float identity[12]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            // Transfer the native output reference directly to the empty owner
            // field: no extra reference or custom destructor is introduced.
            stage = "instantiate";
            reinterpret_cast<Instantiate>(gModuleBase + 0x530950U)(manager, light, &asset, identity);
            if (*light == 0U) return false;
            activationHandle = *light;
            created = true;
        }
        if (enabled && *light != 0U && activationHandle == *light) {
            stage = "activate";
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
        stage = "transform";
        transformSubmitted = enabled && *light != 0U &&
            UpdateRemoteLight(character, light, direction, socketOrigin);
        const auto effectiveEnabled = enabled && transformSubmitted;
        // Both retail perception and equipment presentation observe this flag.
        actor[0x2BAU] = static_cast<std::uint8_t>(effectiveEnabled);
        // Keep the NPC perception flag aligned with the renderer readback below.
        const auto torchManager = *reinterpret_cast<std::uintptr_t*>(actor + 0x34C8U);
        if (torchManager >= 0x10000U)
            *reinterpret_cast<std::uint8_t*>(torchManager + 8U) = static_cast<std::uint8_t>(effectiveEnabled);
        if (*light != 0U && (enabled || activationHandle != *light)) {
            renderer = RendererLight(*light, stage);
            if (renderer < 0x10000U) {
                *reinterpret_cast<std::uint8_t*>(equipment + 0x1574U) = 0U;
                actor[0x2BAU] = 0U;
                if (torchManager >= 0x10000U)
                    *reinterpret_cast<std::uint8_t*>(torchManager + 8U) = 0U;
                return false;
            }
            stage = effectiveEnabled ? "render-on" : "render-off";
            const auto visible = (*reinterpret_cast<const std::uint16_t*>(renderer + 0x34U) & 1U) != 0U;
            if (visible != effectiveEnabled) {
                // These are the same renderer routines used by LightReference
                // 631A70. They update both +34 and the render dirty
                // flag. Do not write the renderer bit or schedule a public ROOT.
                using SetVisible = void(__thiscall*)(void*);
                reinterpret_cast<SetVisible>(gModuleBase + (effectiveEnabled ? 0x616D50U : 0x60B7F0U))(
                    reinterpret_cast<void*>(renderer));
            }
            emitted = (*reinterpret_cast<const std::uint16_t*>(renderer + 0x34U) & 1U) != 0U;
        }
        *reinterpret_cast<std::uint8_t*>(equipment + 0x1574U) = static_cast<std::uint8_t>(emitted);
        // CHR_GetTorch uses the NPC manager's effective flag for representations.
        if (torchManager >= 0x10000U)
            *reinterpret_cast<std::uint8_t*>(torchManager + 8U) = static_cast<std::uint8_t>(emitted);
        const auto success = emitted == enabled && (!enabled || transformSubmitted);
        if (success) stage = enabled ? "on" : "off";
        return success;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(character); static_cast<void>(enabled);
    static_cast<void>(activationHandle); static_cast<void>(emitted); static_cast<void>(created);
    static_cast<void>(transformSubmitted); static_cast<void>(socketOrigin);
    static_cast<void>(renderer);
    static_cast<void>(direction); static_cast<void>(stage);
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

bool Publish(const std::uintptr_t character, const bool enabled, std::string& error,
    const std::array<float, 3>* direction) noexcept {
    if (character < 0x10000U) { error = "Invalid remote torch Character."; return false; }
    if (direction != nullptr && (!enabled || !ValidDirection(*direction))) {
        error = "Invalid remote torch direction."; return false;
    }
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
    if (!enabled) slot->diagnostics.hasDirection = false;
    if (direction != nullptr) {
        slot->direction = *direction;
        slot->diagnostics.hasDirection = true;
    }
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
    std::uintptr_t renderer{};
    const auto success = Apply(character, state.diagnostics.desired, state.activationHandle,
        emitted, created, transformSubmitted, socketOrigin, renderer,
        state.diagnostics.hasDirection ? &state.direction : nullptr, state.diagnostics.stage);
    std::scoped_lock lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr || slot->epoch != state.epoch) return;
    slot->activationHandle = state.activationHandle;
    slot->diagnostics.emitted = emitted;
    slot->diagnostics.transformSubmitted = transformSubmitted;
    slot->diagnostics.socketOrigin = socketOrigin;
    slot->diagnostics.stage = state.diagnostics.stage;
    slot->diagnostics.renderer = renderer;
    if (state.diagnostics.desired) {
        ++slot->diagnostics.onFrames;
        if (emitted) ++slot->diagnostics.renderOnFrames;
        slot->diagnostics.lastOnStage = state.diagnostics.stage;
        if (renderer != 0U) slot->diagnostics.lastOnRenderer = renderer;
    }
    slot->retryAt = success || slot->diagnostics.desired != state.diagnostics.desired
        ? 0U : GetTickCount64() + 500U;
    if (success) ++slot->diagnostics.applications;
    else ++slot->diagnostics.failures;
    if (created) ++slot->diagnostics.creations;
}

void PumpAllOnEngineThread() noexcept {
    std::array<std::uintptr_t, 40> characters{};
    {
        std::scoped_lock lock(gMutex);
        for (std::size_t i = 0U; i < gSlots.size(); ++i) characters[i] = gSlots[i].character;
    }
    for (const auto character : characters) if (character != 0U) PumpOnEngineThread(character);
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
