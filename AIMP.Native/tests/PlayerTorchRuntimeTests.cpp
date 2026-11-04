#include "aimp/PlayerTorchRuntime.h"

#include <Windows.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {
int failures{};
void Check(bool result, const char* message) {
    if (!result) { std::cerr << message << '\n'; ++failures; }
}

// A synthetic x86 engine verifies the actual native ABI, engine-pump lifecycle,
// show/hide and retry path without launching AI.exe or claiming a render test.
std::array<std::uint8_t, 0x4000> actor{}, localPlayer{};
std::array<std::uint8_t, 0x1600> equipment{};
std::array<std::uint8_t, 0x100> definition{}, context{}, root{}, entity{};
std::array<std::uint8_t, 0x100> composite{}, proxy{}, renderer{};
std::array<std::uint32_t, 4> compositeRef{}, children{}, childRef{}, renderRef{};
std::array<std::uint8_t, 0x80> child{};
std::array<std::uintptr_t, 40> proxyVtable{};
std::array<std::uint8_t, 16> torchManager{};
std::array<std::uint32_t, 4> lightRef{};
std::array<float, 12> lastTransform{};
std::array<std::uint8_t, 0x80> bone{};
std::array<float, 16> socketTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 7, 4, 5, 1};
std::uintptr_t localAddress{};
std::uint32_t creates{}, activates{}, shows{}, hides{}, transforms{};
bool failAsset{}, failActivate{}, failSocket{}, haveSocket{};

template<class T, class Buffer> void Store(Buffer& buffer, std::size_t offset, T value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}
void __fastcall FindAsset(void*, void*, std::uint32_t* output, const char* name) {
    Check(std::strcmp(name, "Remote_Torch_Light_v2") == 0 || std::strcmp(name, "AIMP\\Remote_Torch_Light_v2") == 0,
        "Only isolated remote torch asset may be instantiated");
    *output = failAsset ? 0xFFFFFFFFU : 123U;
}
std::uint32_t* __fastcall Instantiate(void*, void*, std::uint32_t* output,
    const std::uint32_t* asset, const float*) {
    Check(*asset == 123U, "Resolved asset passed to instantiate");
    ++creates;
    *output = reinterpret_cast<std::uintptr_t>(lightRef.data());
    return output;
}
void __fastcall SetKind(void* object, void*, std::uint8_t kind) {
    Check(object == entity.data() && kind == 7U, "Native torch entity kind");
}
std::uint32_t* __fastcall CopyRef(std::uint32_t* output, void*, const std::uint32_t* input) {
    *output = *input;
    return output;
}
bool __fastcall Activate(void*, void*, std::uint32_t reference) {
    Check(reference == reinterpret_cast<std::uintptr_t>(lightRef.data()), "Owned activation reference");
    ++activates;
    return !failActivate;
}
std::uintptr_t* __fastcall FindBone(void*, void*, std::uintptr_t* output, const char* name) {
    Check(std::strcmp(name, "FLASHLIGHT") == 0, "Use the animated torch attachment");
    if (failSocket) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    *output = haveSocket ? reinterpret_cast<std::uintptr_t>(bone.data()) : 0U;
    if (haveSocket) ++*reinterpret_cast<std::int32_t*>(bone.data() + 0x6C);
    return output;
}
float* __fastcall BoneTransform(void* object, void*, float* output) {
    Check(object == bone.data(), "Read the selected animated bone");
    std::memcpy(output, socketTransform.data(), sizeof(socketTransform));
    return output;
}
void __cdecl SetTransform(const std::uint32_t* light, const float* transform) {
    Check(*light == reinterpret_cast<std::uintptr_t>(lightRef.data()), "Transform targets the remote light");
    std::memcpy(lastTransform.data(), transform, sizeof(lastTransform));
    ++transforms;
}
std::uintptr_t __fastcall GetLight(void* object, void*, const std::uint32_t* owner) {
    Check(object == proxy.data() && *owner == reinterpret_cast<std::uintptr_t>(renderRef.data()),
        "Renderer getter receives its own strong reference");
    return reinterpret_cast<std::uintptr_t>(renderer.data());
}
void __fastcall Show(void* light, void*) {
    Check(light == renderer.data(), "Native light activation targets the renderer");
    renderer[0x34] |= 1U; ++shows;
}
void __fastcall Hide(void* light, void*) {
    Check(light == renderer.data(), "Native light extinction targets the renderer");
    renderer[0x34] &= ~1U; ++hides;
}

void Jump(std::uintptr_t image, std::uintptr_t rva, std::uintptr_t target) {
    auto* code = reinterpret_cast<std::uint8_t*>(image + rva);
    code[0] = 0xE9U;
    const auto displacement = static_cast<std::uint32_t>(target - (image + rva + 5U));
    std::memcpy(code + 1, &displacement, sizeof(displacement));
}
}

int main() {
    using namespace aimp::player_torch;
    constexpr std::size_t imageSize = 0x1350000U;
    auto* memory = VirtualAlloc(nullptr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (memory == nullptr) return 1;
    const auto image = reinterpret_cast<std::uintptr_t>(memory);
    const auto character = reinterpret_cast<std::uintptr_t>(actor.data());
    Store(actor, 0x64, reinterpret_cast<std::uintptr_t>(definition.data()));
    // Player-authored flags on a remote representation must not reject it.
    definition[0x1C] = definition[0x1F] = 1U;
    Store(actor, 0x1100, reinterpret_cast<std::uintptr_t>(equipment.data()));
    Store(actor, 0x34C8, reinterpret_cast<std::uintptr_t>(torchManager.data()));
    Store(actor, 0x70, 2.0F); Store(actor, 0x74, 3.0F); Store(actor, 0x78, 4.0F);
    Store(equipment, 0x1528, character);
    lightRef[3] = reinterpret_cast<std::uintptr_t>(entity.data());
    Store(entity, 0, image + 0xFC8024U);
    Store(entity, 0x24, reinterpret_cast<std::uintptr_t>(compositeRef.data()));
    compositeRef[3] = reinterpret_cast<std::uintptr_t>(composite.data());
    Store(composite, 0, image + 0xFE1D2CU);
    Store(composite, 0x2C, reinterpret_cast<std::uintptr_t>(children.data()));
    children[2] = 1U;
    auto childAddress = reinterpret_cast<std::uintptr_t>(childRef.data());
    children[3] = reinterpret_cast<std::uintptr_t>(&childAddress);
    childRef[3] = reinterpret_cast<std::uintptr_t>(child.data());
    Store(child, 0, image + 0xEA5EB0U);
    Store(child, 0x30, reinterpret_cast<std::uintptr_t>(renderRef.data()));
    renderRef[3] = reinterpret_cast<std::uintptr_t>(proxy.data());
    Store(proxy, 0, reinterpret_cast<std::uintptr_t>(proxyVtable.data()));
    proxyVtable[0x8C / 4] = image + 0x2000U;
    localAddress = reinterpret_cast<std::uintptr_t>(localPlayer.data());
    Store(context, 0x80, reinterpret_cast<std::uintptr_t>(&localAddress));
    Store(context, 0x84, 1U);
    Store(root, 8, reinterpret_cast<std::uintptr_t>(context.data()));
    *reinterpret_cast<std::uintptr_t*>(image + 0x12F0C88U) = reinterpret_cast<std::uintptr_t>(root.data());
    *reinterpret_cast<std::uintptr_t*>(image + 0x134EF40U) = reinterpret_cast<std::uintptr_t>(entity.data());
    *reinterpret_cast<std::uint32_t*>(image + 0x134E254U) = 0xFFFFFFFFU;
    Jump(image, 0x522520U, reinterpret_cast<std::uintptr_t>(&FindAsset));
    Jump(image, 0x530950U, reinterpret_cast<std::uintptr_t>(&Instantiate));
    Jump(image, 0x62A900U, reinterpret_cast<std::uintptr_t>(&SetKind));
    Jump(image, 0x673C0U, reinterpret_cast<std::uintptr_t>(&CopyRef));
    Jump(image, 0x524B60U, reinterpret_cast<std::uintptr_t>(&Activate));
    Jump(image, 0x2B4E70U, reinterpret_cast<std::uintptr_t>(&FindBone));
    Jump(image, 0xBE1F0U, reinterpret_cast<std::uintptr_t>(&BoneTransform));
    Jump(image, 0x4BC500U, reinterpret_cast<std::uintptr_t>(&SetTransform));
    Jump(image, 0x2000U, reinterpret_cast<std::uintptr_t>(&GetLight));
    Jump(image, 0x616D50U, reinterpret_cast<std::uintptr_t>(&Show));
    Jump(image, 0x60B7F0U, reinterpret_cast<std::uintptr_t>(&Hide));
    FlushInstructionCache(GetCurrentProcess(), memory, imageSize);
    Initialize(image);
    std::string error;
    const std::array<float, 3> aim{0, 0.6F, 0.8F};
    Check(Publish(character, true, error, &aim), "Publish effective on and aim");
    Check(creates == 0U && shows == 0U, "IPC publication does not touch native objects");
    // Retail show's guard would swallow this edge. The renderer starts off.
    equipment[0x1574] = 1U;
    failSocket = true;
    PumpAllOnEngineThread();
    auto state = Snapshot(character);
    Check(state.emitted && state.transformSubmitted && !state.socketOrigin && state.failures == 0U,
        "Missing or faulting socket falls back without swallowing light creation");
    Check(creates == 1U && activates == 1U && shows == 1U && actor[0x2BA] == 1U && torchManager[8] == 1U,
        "Native creation, activation and visible state complete");
    Check(std::abs(lastTransform[6] - 0.6F) < 0.00001F && lastTransform[3] == 2.0F &&
        std::abs(lastTransform[7] - 4.65F) < 0.00001F && std::abs(lastTransform[11] - 4.2F) < 0.00001F,
        "Fallback beam origin advances 25cm along the full source look ray");
    Store(actor, 0x70, 7.0F);
    PumpAllOnEngineThread();
    Check(lastTransform[3] == 7.0F && creates == 1U && activates == 1U && transforms == 2U,
        "Light follows actor every engine pump without another movement message or allocation");
    Check(Publish(character, false, error), "Battery off publication");
    PumpAllOnEngineThread();
    Check(!Snapshot(character).emitted && actor[0x2BA] == 0U && torchManager[8] == 0U && hides == 1U,
        "Effective off hides the native light and perception state");
    Check(Publish(character, true, error, &aim), "Re-enable after battery replacement");
    PumpAllOnEngineThread();
    Check(Snapshot(character).emitted && creates == 1U, "Existing controller light is reused");
    Check(Snapshot(character).renderOnFrames == 3U && Snapshot(character).lastOnRenderer != 0U,
        "Enabled history survives subsequent off samples");
    Check(Publish(character, false, error), "Switch off before renderer failure");
    PumpAllOnEngineThread();
    Store(child, 0x30, 0U);
    equipment[0x1574] = 1U;
    Check(Publish(character, true, error, &aim), "Publish while render proxy is absent");
    PumpAllOnEngineThread();
    Check(!Snapshot(character).emitted && Snapshot(character).failures == 1U &&
        std::strcmp(Snapshot(character).stage, "render-proxy") == 0 && actor[0x2BA] == 0U,
        "Equipment acceptance cannot masquerade as an emitting renderer");
    Store(child, 0x30, reinterpret_cast<std::uintptr_t>(renderRef.data()));
    Check(Publish(character, false, error), "Off clears missing-renderer retry");
    PumpAllOnEngineThread();
    Check(Publish(character, true, error, &aim), "Restore renderer");
    PumpAllOnEngineThread();
    Check(Snapshot(character).emitted, "Renderer recovery turns the actual light on");
    haveSocket = true; failSocket = false;
    Store(bone, 0x6C, std::int32_t{1});
    PumpAllOnEngineThread();
    Check(Snapshot(character).socketOrigin && lastTransform[3] == 7.0F &&
        std::abs(lastTransform[7] - 4.15F) < 0.00001F && std::abs(lastTransform[11] - 5.2F) < 0.00001F,
        "Animated socket supplies the origin with forward body clearance");
    socketTransform[12] = 7.15F; socketTransform[13] = 3.8F; socketTransform[14] = 5.1F;
    PumpAllOnEngineThread();
    Check(lastTransform[3] == 7.15F && std::abs(lastTransform[7] - 3.95F) < 0.00001F &&
        std::abs(lastTransform[11] - 5.3F) < 0.00001F &&
        *reinterpret_cast<const std::int32_t*>(bone.data() + 0x6C) == 1,
        "Animation changes follow every pump without a movement message or a leaked bone reference");
    Check(Publish(character, false, error), "Clear aim for the yaw fallback");
    PumpAllOnEngineThread();
    haveSocket = false;
    Store(actor, 0x174, 1.570796327F);
    Check(Publish(character, true, error), "Enable with yaw and no socket or aim sample");
    PumpAllOnEngineThread();
    Check(!Snapshot(character).socketOrigin && std::abs(lastTransform[3] - 7.25F) < 0.00001F &&
        lastTransform[7] == 4.5F && std::abs(lastTransform[11] - 4.0F) < 0.00001F,
        "Yaw fallback advances with the character's heading");
    Forget(character);
    const auto oldTransforms = transforms;
    PumpAllOnEngineThread();
    Check(transforms == oldTransforms, "Destruction cleanup stops accessing the actor");

    // A failed lookup or activation must be observable and immediately retry on off.
    Store(equipment, 0x156C, 0U); equipment[0x1574] = 0;
    failAsset = true;
    Check(Publish(character, true, error, &aim), "Publish before asset available");
    PumpAllOnEngineThread();
    Check(Snapshot(character).failures == 1U && std::strcmp(Snapshot(character).stage, "asset") == 0,
        "Missing asset identifies the failing stage");
    Check(Publish(character, false, error), "Off bypasses failed creation retry delay");
    PumpAllOnEngineThread();
    Check(std::strcmp(Snapshot(character).stage, "off") == 0, "Off succeeds even with no asset");
    failAsset = false; failActivate = true;
    Check(Publish(character, true, error, &aim), "Retry creation");
    PumpAllOnEngineThread();
    Check(std::strcmp(Snapshot(character).stage, "activate") == 0 && !Snapshot(character).emitted,
        "Activation failure is not reported as emission");
    Check(Publish(character, false, error), "Off while activation pending");
    PumpAllOnEngineThread();
    Check(std::strcmp(Snapshot(character).stage, "off") == 0, "Pending activation cannot prevent an off edge");
    failActivate = false;
    Check(Publish(character, true, error, &aim), "Retry pending activation");
    PumpAllOnEngineThread();
    Check(Snapshot(character).emitted, "Pending activation recovers");

    ForgetAll();
    localAddress = character;
    Check(Publish(character, true, error, &aim), "Publish a local address to test native guard");
    PumpAllOnEngineThread();
    Check(std::strcmp(Snapshot(character).stage, "local-player-guard") == 0 && Snapshot(character).failures == 1U,
        "Actual local player is rejected independently of definition flags");
    ForgetAll();
    VirtualFree(memory, 0U, MEM_RELEASE);
    return failures == 0 ? 0 : 1;
}
