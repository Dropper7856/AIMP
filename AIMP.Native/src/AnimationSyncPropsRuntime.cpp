#include "aimp/AnimationSyncPropsRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>

namespace aimp::animation_sync::props {
namespace {
constexpr std::uintptr_t kAttachmentType = 0xDF0778U;
constexpr std::uintptr_t kCharacterType = 0xDD08D8U;
std::atomic<std::uintptr_t> gBase{}, gLocal{};
std::atomic<std::uint32_t> gLocalRuntime{}, gDetours{};
std::atomic<void*> gOriginal{};
std::atomic<bool> gEnabled{};
struct Sighting { std::uintptr_t node{}; std::uint32_t token{}; std::uint64_t at{}; };
std::array<Sighting, 16> gSightings{};
std::mutex gCaptureMutex;
struct Prop {
    std::uint32_t token{}, root{}, nodeRef{};
    std::uintptr_t node{};
    std::uint64_t retryAt{};
    bool activated{}, attached{}, faulted{};
};
struct Remote { std::uintptr_t character{}; std::uint32_t runtime{}; std::array<Prop, 4> props{}; };
// All native references are game-thread-owned. No loader-thread destructors.
auto& gRemotes = *new std::array<Remote, 40>{};
template<class T> bool Read(std::uintptr_t address, T& value) noexcept {
    return address >= 0x10000U && crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(value));
}
std::uintptr_t Payload(std::uint32_t ref) noexcept {
    std::uintptr_t object{};
    return Read(static_cast<std::uintptr_t>(ref) + 0xCU, object) ? object : 0U;
}
bool Match(std::uintptr_t rva, std::initializer_list<std::uint8_t> bytes) noexcept {
    std::array<std::uint8_t, 32> actual{};
    return bytes.size() <= actual.size() && crash_guard::TryCopy(actual.data(),
        reinterpret_cast<const void*>(gBase.load() + rva), bytes.size()) &&
        std::memcmp(actual.data(), bytes.begin(), bytes.size()) == 0;
}
bool IsRemote(std::uintptr_t character) noexcept {
    std::uintptr_t root{}, context{}, players{};
    std::uint32_t count{};
    if (!Read(gBase.load() + 0x12F0C88U, root) || !Read(root + 8U, context) ||
        !Read(context + 0x80U, players) || !Read(context + 0x84U, count) || count == 0U || count > 4U) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t player{};
        if (!Read(players + i * 4U, player) || player == character) return false;
    }
    return true;
}
#if defined(_MSC_VER) && defined(_M_IX86)
using Transform = float*(__thiscall*)(void*, float*, const std::uint32_t*);
using ResolveCharacter = std::uintptr_t(__thiscall*)(void*, const std::uint32_t*);
using Enter = void(__fastcall*)(void*);
using Leave = void(__cdecl*)();
using Assign = std::uint32_t*(__thiscall*)(std::uint32_t*, const std::uint32_t*);
using Hash = std::uint32_t*(__thiscall*)(std::uint32_t*, const char*);
using Find = void(__thiscall*)(void*, std::uint32_t*, const char*);
using Create = std::uint32_t*(__thiscall*)(void*, std::uint32_t*, const std::uint32_t*, const float*);
using Kind = void(__thiscall*)(void*, std::uint8_t);
using Activate = std::uint8_t(__thiscall*)(void*, std::uint32_t);
using Weak = void(__thiscall*)(void*, void*);
using InitializeAttachment = std::uint8_t(__thiscall*)(void*, const std::uint32_t*);
using UpdateTransform = void(__thiscall*)(void*, const float*);

std::uint32_t Observe(std::uintptr_t node, const std::uint32_t* reference) noexcept {
    __try {
        const auto base = gBase.load();
        if (!gEnabled.load() || gLocal.load() == 0U || reference == nullptr ||
            *reinterpret_cast<const std::uintptr_t*>(node) != base + kAttachmentType) return 0U;
        const auto character = reinterpret_cast<ResolveCharacter>(base + 0x4F62C0U)(
            reinterpret_cast<void*>(node + 0x14U), reference);
        if (character != gLocal.load() ||
            *reinterpret_cast<const std::uint32_t*>(character + 0x50U) != gLocalRuntime.load()) return 0U;
        return *reinterpret_cast<const std::uint32_t*>(node + 0xCU);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0U; }
}
float* __fastcall TransformDetour(void* node, void*, float* matrix, const std::uint32_t* reference) noexcept {
    gDetours.fetch_add(1U);
    const auto original = reinterpret_cast<Transform>(gOriginal.load(std::memory_order_acquire));
    auto* result = original == nullptr ? matrix : original(node, matrix, reference);
    const auto token = Observe(reinterpret_cast<std::uintptr_t>(node), reference);
    if (token != 0U && token != 0xFFFFFFFFU) {
        const std::lock_guard lock(gCaptureMutex);
        Sighting* entry{};
        for (auto& sighting : gSightings) if (sighting.node == reinterpret_cast<std::uintptr_t>(node)) { entry = &sighting; break; }
        if (entry == nullptr) entry = &*std::min_element(gSightings.begin(), gSightings.end(),
            [](const auto& a, const auto& b) { return a.at < b.at; });
        *entry = {reinterpret_cast<std::uintptr_t>(node), token, GetTickCount64()};
    }
    gDetours.fetch_sub(1U);
    return result;
}

void ReleaseNative(Prop& prop) {
    if (prop.root == 0U) return;
    const auto base = gBase.load();
    const auto payload = Payload(prop.root);
    if (payload != 0U) {
        auto* flags = reinterpret_cast<std::uint8_t*>(payload + 0x2DU);
        *flags = static_cast<std::uint8_t>(*flags | 1U);
    }
    const std::uint32_t empty{};
    reinterpret_cast<Assign>(base + 0x673C0U)(&prop.root, &empty);
    prop.node = 0U; prop.nodeRef = 0U;
}
bool Children(const Prop& prop, std::uintptr_t& data, std::uint32_t& count) {
    const auto base = gBase.load();
    const auto temporary = Payload(prop.root);
    if (temporary == 0U || *reinterpret_cast<const std::uintptr_t*>(temporary) != base + 0xFC8024U) return false;
    const auto instance = Payload(*reinterpret_cast<const std::uint32_t*>(temporary + 0x24U));
    if (instance == 0U || *reinterpret_cast<const std::uintptr_t*>(instance) != base + 0xFE1D2CU) return false;
    const auto children = *reinterpret_cast<const std::uintptr_t*>(instance + 0x2CU);
    if (children < 0x10000U) return false;
    count = *reinterpret_cast<const std::uint32_t*>(children + 8U);
    data = *reinterpret_cast<const std::uintptr_t*>(children + 0xCU);
    return count > 0U && count <= 64U && data >= 0x10000U;
}
bool StepNative(std::uintptr_t character, Prop& prop) {
    const auto base = gBase.load();
    auto* manager = *reinterpret_cast<void**>(base + 0x134EF40U);
    if (manager == nullptr) return false;
    if (prop.root == 0U) {
        char name[64]{};
        std::snprintf(name, sizeof(name), "AIMP\\AnimationProps\\%08X", static_cast<unsigned>(prop.token));
        auto asset = *reinterpret_cast<const std::uint32_t*>(base + 0x134E254U);
        reinterpret_cast<Find>(base + 0x522520U)(manager, &asset, name);
        if (asset == *reinterpret_cast<const std::uint32_t*>(base + 0x134E254U)) return false;
        const auto* xyz = reinterpret_cast<const float*>(character + 0x70U);
        const float matrix[12]{1, 0, 0, xyz[0], 0, 1, 0, xyz[1], 0, 0, 1, xyz[2]};
        reinterpret_cast<Create>(base + 0x530950U)(manager, &prop.root, &asset, matrix);
        if (prop.root == 0U) return false;
    }
    if (!prop.activated) {
        const auto temporary = Payload(prop.root);
        if (temporary == 0U) return false;
        reinterpret_cast<Kind>(base + 0x62A900U)(reinterpret_cast<void*>(temporary), 7U);
        std::uint32_t copy{};
        reinterpret_cast<Assign>(base + 0x673C0U)(&copy, &prop.root);
        prop.activated = true; // activation consumes its retained argument
        if (reinterpret_cast<Activate>(base + 0x524B60U)(manager, copy) == 0U) return false;
    }
    if (!prop.attached) {
        std::uintptr_t data{}; std::uint32_t count{};
        if (!Children(prop, data, count)) return false;
        std::uintptr_t bridge{};
        std::uint32_t bridgeId{};
        reinterpret_cast<Hash>(base + 0x4BF7F0U)(&bridgeId, "AIMP_AnimationProp_Character");
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto ref = reinterpret_cast<const std::uint32_t*>(data)[i];
            const auto object = Payload(ref);
            if (object == 0U) continue;
            const auto type = *reinterpret_cast<const std::uintptr_t*>(object);
            const auto id = *reinterpret_cast<const std::uint32_t*>(object + 0xCU);
            if (type == base + kCharacterType && id == bridgeId) bridge = object;
            else if (type == base + kAttachmentType && id == prop.token) { prop.node = object; prop.nodeRef = ref; }
        }
        if (bridge == 0U || prop.node == 0U) return false;
        // This bridge belongs to our visual-only graph, never the local
        // player's CATHODE variable. WeakAssign retains Character incarnation.
        reinterpret_cast<Weak>(base + 0xA1D0U)(reinterpret_cast<void*>(bridge + 0xE0U), reinterpret_cast<void*>(character));
        if (reinterpret_cast<ResolveCharacter>(base + 0x4F62C0U)(reinterpret_cast<void*>(prop.node + 0x14U), &prop.nodeRef) != character)
            return false;
        // Same initializer used by retail attach. It creates a transform
        // variable and binds ONLY this clone's visual target references.
        prop.attached = reinterpret_cast<InitializeAttachment>(base + 0xD6690U)(
            reinterpret_cast<void*>(prop.node), &prop.nodeRef) != 0U;
        if (!prop.attached) return false;
    }
    if (*reinterpret_cast<const std::uintptr_t*>(prop.node) != base + kAttachmentType) return false;
    const auto transformRef = *reinterpret_cast<const std::uint32_t*>(prop.node + 0x2CU);
    const auto transform = Payload(transformRef);
    if (transform == 0U) return false;
    alignas(16) float matrix[12]{};
    // Match 004D72A0: evaluated bone transform -> cTransform update. Native
    // attachment offset/rotation remains in the visual's binding, unmodified.
    reinterpret_cast<Transform>(gOriginal.load())(reinterpret_cast<void*>(prop.node), matrix, &prop.nodeRef);
    for (const auto value : matrix) if (!std::isfinite(value)) return false;
    reinterpret_cast<UpdateTransform>(base + 0x4C0F50U)(reinterpret_cast<void*>(transform), matrix);
    return true;
}
bool Step(std::uintptr_t character, Prop& prop, bool release) noexcept {
    __try {
        reinterpret_cast<Enter>(gBase.load() + 0x51E1B0U)(nullptr);
        __try {
            if (release) { ReleaseNative(prop); return true; }
            return StepNative(character, prop);
        } __finally { reinterpret_cast<Leave>(gBase.load() + 0x51E1D0U)(); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { prop.faulted = true; return false; }
}
#endif
void Release(Prop& prop) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    // Faulted native reference state is quarantined until level teardown.
    if (!prop.faulted) static_cast<void>(Step(0U, prop, true));
#endif
    prop = {};
}
}

bool Install(HookManager& hooks, std::uintptr_t moduleBase, std::string& error) {
    if (!hooks.ExactBuildValidated() || moduleBase == 0U) { error = "AnimationSync props require the exact retail build."; return false; }
    gBase.store(moduleBase);
#if defined(_MSC_VER) && defined(_M_IX86)
    if (!Match(0xBE2B0U, {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0x74,0x01,0x00,0x00}) ||
        !Match(0xD6690U, {0x6A,0xFF,0x64,0xA1,0x00,0x00,0x00,0x00}) ||
        !Match(0xD72A0U, {0x83,0xEC,0x30,0x53,0x8B,0x5C,0x24,0x38}) ||
        !Match(0x4F62C0U, {0x55,0x8B,0xEC,0x6A,0xFF}) ||
        !Match(0x4C0F50U, {0x55,0x8B,0xEC,0x6A,0xFF}) ||
        !Match(0xA1D0U, {0x8B,0x44,0x24,0x04,0x56,0x8B,0xF1}) ||
        !Match(0x673C0U, {0x55,0x8B,0x6C,0x24,0x08,0x56}) ||
        !Match(0x51E1B0U, {0x56,0x68}) || !Match(0x51E1D0U, {0x68})) {
        error = "AnimationSync attachment ABI certification failed."; return false;
    }
    HookRequest request{};
    request.name = "animation_sync_character_attachment_transform";
    request.target = reinterpret_cast<void*>(moduleBase + 0xBE2B0U);
    request.detour = reinterpret_cast<void*>(&TransformDetour);
    // PUSH EBP; MOV EBP,ESP; AND ESP,-16: three complete instructions, six bytes.
    request.patchLength = 6U; request.relocationSafePrefix = true;
    for (const auto byte : {0x55,0x8B,0xEC,0x83,0xE4,0xF0}) request.expected.bytes.push_back(static_cast<std::uint8_t>(byte));
    request.expected.mask = "xxxxxx";
    request.publishedTrampolineOut = &gOriginal;
    if (!hooks.Install(request, error)) return false;
    gEnabled.store(true);
    return true;
#else
    error = "AnimationSync attachment runtime requires MSVC x86."; return false;
#endif
}

std::array<std::uint32_t, 4> CaptureOnEngineThread(std::uintptr_t character) noexcept {
    std::array<std::uint32_t, 4> tokens{};
    std::uint32_t runtime{};
    if (!gEnabled.load() || !Read(character + 0x50U, runtime) || runtime == 0U) return tokens;
    const std::lock_guard lock(gCaptureMutex);
    const auto priorCharacter = gLocal.exchange(character);
    const auto priorRuntime = gLocalRuntime.exchange(runtime);
    if (priorCharacter != character || priorRuntime != runtime) gSightings = {};
    const auto now = GetTickCount64();
    std::size_t count{};
    for (const auto& sighting : gSightings) {
        std::uintptr_t type{}; std::uint32_t transform{}, token{};
        if (sighting.token == 0U || now - sighting.at > 500U ||
            !Read(sighting.node, type) || type != gBase.load() + kAttachmentType ||
            !Read(sighting.node + 0xCU, token) || token != sighting.token ||
            !Read(sighting.node + 0x2CU, transform) || transform == 0U) continue;
        if (std::find(tokens.begin(), tokens.end(), token) != tokens.end()) continue;
        if (count == tokens.size()) break;
        tokens[count++] = token;
    }
    std::sort(tokens.begin(), tokens.begin() + count);
    return tokens;
}
void ReconcileOnEngineThread(std::uintptr_t character, const std::array<std::uint32_t, 4>& tokens) noexcept {
    if (!gEnabled.load() || character < 0x10000U || !IsRemote(character)) return;
    std::uint32_t runtime{};
    if (!Read(character + 0x50U, runtime) || runtime == 0U) return;
    Remote* remote{};
    for (auto& candidate : gRemotes) if (candidate.character == character) { remote = &candidate; break; }
    if (remote != nullptr && remote->runtime != runtime) { ForgetOnEngineThread(character); remote = nullptr; }
    if (remote == nullptr) for (auto& candidate : gRemotes) if (candidate.character == 0U) { remote = &candidate; break; }
    if (remote == nullptr) return;
    remote->character = character; remote->runtime = runtime;
    for (auto& prop : remote->props)
        if (prop.token != 0U && std::find(tokens.begin(), tokens.end(), prop.token) == tokens.end()) Release(prop);
    const auto now = GetTickCount64();
    for (const auto token : tokens) {
        if (token == 0U || token == 0xFFFFFFFFU) continue;
        Prop* prop{};
        for (auto& candidate : remote->props) if (candidate.token == token) { prop = &candidate; break; }
        if (prop == nullptr) for (auto& candidate : remote->props) if (candidate.token == 0U) { prop = &candidate; prop->token = token; break; }
        if (prop == nullptr || prop->faulted || now < prop->retryAt) continue;
#if defined(_MSC_VER) && defined(_M_IX86)
        if (!Step(character, *prop, false)) prop->retryAt = now + 250U;
#endif
    }
}
void ForgetOnEngineThread(std::uintptr_t character) noexcept {
    for (auto& remote : gRemotes) if (remote.character == character) {
        for (auto& prop : remote.props) Release(prop);
        remote = {}; break;
    }
}
void ResetOnEngineThread() noexcept {
    for (auto& remote : gRemotes) { for (auto& prop : remote.props) Release(prop); remote = {}; }
    const std::lock_guard lock(gCaptureMutex);
    gSightings = {}; gLocal.store(0U); gLocalRuntime.store(0U);
}
void BeginLogicalShutdown() noexcept { gEnabled.store(false); }
std::uint32_t ActiveDetours() noexcept { return gDetours.load(); }
}
