#include "aimp/PlayerPostureRuntime.h"
#include "aimp/PlayerAnimationClipCapture.h"
#include "aimp/HookManager.h"
#include "aimp/Diagnostics.h"
#include "aimp/RuntimeFeatureFlags.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
int failures{};
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
std::array<std::uint8_t, 0x4400> actor{};
std::array<std::uint8_t, 0x100> definition{}, cycle{}, root{}, manager{};
std::array<std::uint8_t, 0x80> animationContexts{}, animationInstance{}, animationDatabase{};
std::array<std::uint8_t, 0x28> baseClips{}, contextNames{}, crouchClips{};
std::array<std::uint32_t, 4> baseKeys{}, crouchKeys{};
std::array<std::uint32_t, 2> contextKeys{};
std::array<std::uint32_t, 4> baseValues{}, crouchValues{};
std::uint32_t contextIndex{};
std::uintptr_t localPlayer = 0x123456U;
bool crouchTag{}, playerComponent{}, ignoreNative{};
std::uint32_t stanceCalls{};
std::uint32_t repairCalls{};
std::uint32_t refreshCalls{};
bool refreshSucceeds{true};
std::uint32_t moveRefreshes{}, idleRefreshes{};
std::array<std::uint8_t, 0x220> aimController{};
std::array<std::uint8_t, 0x100> motion{}, aimTree{};
std::uint32_t aimCalls{};
bool expectAimAdaptation{}, failAim{};
void __fastcall RefreshMove(void* self, void*) {
    Check(self == cycle.data() && cycle[0x1A] == 0x38U, "Moving refresh enters Move directly");
    ++moveRefreshes;
}
void __fastcall RefreshIdle(void* self, void*) {
    Check(self == cycle.data() && cycle[0x1A] == 1U, "Idle refresh keeps its ordinary idle state");
    ++idleRefreshes;
}
bool RefreshCurrentCycle(std::uintptr_t character) noexcept {
    return aimp::player_posture::RefreshAnimationCycle(character,
        reinterpret_cast<std::uintptr_t>(&RefreshIdle), reinterpret_cast<std::uintptr_t>(&RefreshMove));
}
bool RefreshAnimation(std::uintptr_t character) noexcept {
    Check(character == reinterpret_cast<std::uintptr_t>(actor.data()), "Only the owned actor's animation is refreshed");
    ++refreshCalls;
    return refreshSucceeds;
}
void __cdecl AddCrouchedTag(std::uintptr_t character, std::uint32_t tag, int slot) {
    Check(character == reinterpret_cast<std::uintptr_t>(actor.data()) && tag == 4114047242U && slot == 0,
        "Repair uses the native CROUCHED context in stance slot zero");
    ++repairCalls;
    std::memcpy(animationContexts.data() + 0x10, &tag, 4);
    const std::uint32_t references = 1U;
    std::memcpy(animationContexts.data() + 0x38, &references, 4);
}

template<class T, class Buffer> void Store(Buffer& buffer, std::size_t offset, T value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}
void __fastcall BindAim(void* controller, void*) {
    Check(controller == aimController.data(), "Aim setup preserves the retail ECX receiver");
    ++aimCalls;
    if (expectAimAdaptation) {
        Check(*reinterpret_cast<const std::uint32_t*>(motion.data() + 0x3C) == 0U,
            "Crouched remote uses the standard aim reference and directional/IK tree");
        Check(*reinterpret_cast<const float*>(aimController.data() + 0x1D0) == 1.0F,
            "Stationary and moving aim retain primary crouched hips/legs");
        Check(actor[0x2AF] == 1U, "Aim selection never clears the gameplay crouch flag");
    }
    if (failAim) RaiseException(0xE0414D50U, 0U, 0U, nullptr);
    Store(aimController, 0x14U, reinterpret_cast<std::uintptr_t>(aimTree.data()));
}
bool AimExceptionPropagates(const aimp::player_posture::AimSetup original) {
    __try { aimp::player_posture::SetupAim(aimController.data(), original); }
    __except (GetExceptionCode() == 0xE0414D50U ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
// These mocks model the distinct NPC helper and later modifier consumer ABIs.
// They are not a simulation of Alien sight or a test of rendered animations.
void __fastcall Crouch(void* character, void*) {
    Check(character == actor.data(), "Native NPC crouch receives the remote Character");
    ++stanceCalls;
    if (ignoreNative) return;
    actor[0x3E3A] = actor[0x3E2E] != 0U;
    if (!playerComponent) { actor[0x2AF] = 1; crouchTag = true; }
}
void __fastcall Stand(void* character, void*) {
    Check(character == actor.data(), "Native NPC stand receives the remote Character");
    ++stanceCalls;
    if (ignoreNative) return;
    actor[0x3E3A] = 0;
    if (!playerComponent) { actor[0x2AF] = 0; crouchTag = false; }
}
void __fastcall ApplyModifiers(void* modifiers, void*, void* character) {
    Check(modifiers == actor.data() + 0x3E2C && character == actor.data(), "Modifier consumer ABI");
    if (ignoreNative) return;
    if (actor[0x3E3A] != actor[0x3E46]) {
        actor[0x2AF] = actor[0x3E3A] != 0;
        crouchTag = actor[0x2AF] != 0;
    }
    actor[0x3E46] = actor[0x3E3A];
}
void Jump(std::uintptr_t image, std::uintptr_t rva, std::uintptr_t target) {
    auto* code = reinterpret_cast<std::uint8_t*>(image + rva);
    code[0] = 0xE9;
    const auto displacement = static_cast<std::uint32_t>(target - (image + rva + 5U));
    std::memcpy(code + 1, &displacement, sizeof(displacement));
}
}

int main() {
    using namespace aimp::player_posture;
    constexpr std::size_t imageSize = 0x1350000U;
    auto* memory = VirtualAlloc(nullptr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!memory) return 1;
    const auto image = reinterpret_cast<std::uintptr_t>(memory);
    const auto character = reinterpret_cast<std::uintptr_t>(actor.data());
    Store(actor, 0x64, reinterpret_cast<std::uintptr_t>(definition.data()));
    Store(actor, 0x36C, reinterpret_cast<std::uintptr_t>(cycle.data()));
    Store(cycle, 4, character); cycle[0x1A] = 1;
    Store(root, 8, reinterpret_cast<std::uintptr_t>(manager.data()));
    Store(manager, 0x80, reinterpret_cast<std::uintptr_t>(&localPlayer));
    Store(manager, 0x84, 1U);
    *reinterpret_cast<std::uintptr_t*>(image + 0x12F0C88) = reinterpret_cast<std::uintptr_t>(root.data());
    Jump(image, 0x3CAE20, reinterpret_cast<std::uintptr_t>(&Crouch));
    Jump(image, 0x3CAE90, reinterpret_cast<std::uintptr_t>(&Stand));
    Jump(image, 0x3E0400, reinterpret_cast<std::uintptr_t>(&AddCrouchedTag));
    Jump(image, 0x3D2AD0, reinterpret_cast<std::uintptr_t>(&ApplyModifiers));
    FlushInstructionCache(GetCurrentProcess(), memory, imageSize);
    Initialize(image); ForgetAll();
    std::string error;
    Check(Publish(character, State{1}, error), "Publish crouch while stationary");
    Check(stanceCalls == 0, "IPC publication does not touch the actor");
    ReconcileAllOnEngineThread();
    Check(actor[0x2AF] == 1 && crouchTag && actor[0x3E46] == 1 && Snapshot(character).appliedCrouched,
        "Native crouch updates both animation/perception flag and modifier state");
    Check(Snapshot(character).crouchTransitions == 1 && Snapshot(character).postureFailures == 0,
        "Diagnostics confirm actual stance application");
    // Retail Move/Idle setup can reset stance after the network command.
    actor[0x2AF] = actor[0x3E3A] = actor[0x3E46] = 0; crouchTag = false;
    ReconcileAllOnEngineThread();
    Check(actor[0x2AF] == 1 && crouchTag, "Reconcile repairs a stance overwritten by native movement");
    Sleep(1100); // Posture must outlive the one-second movement/traversal lease.
    ReconcileAllOnEngineThread();
    Check(actor[0x2AF] == 1 && Snapshot(character).postureApplications == 3,
        "No movement mailbox or heartbeat is needed to remain crouched");
    Check(Publish(character, State{}, error), "Publish stand");
    ReconcileAllOnEngineThread();
    Check(actor[0x2AF] == 0 && !crouchTag && !Snapshot(character).appliedCrouched,
        "Stand restores the ordinary native posture");
    actor[0x2B1] = 0x5A;
    Check(Publish(character, State{2}, error), "Publish deep crouch without the separate ordinary flag");
    ReconcileAllOnEngineThread();
    Check(actor[0x2AF] == 1 && actor[0x2B0] == 1 && actor[0x2B1] == 0x5A &&
        Snapshot(character).appliedDeepCrouched, "Deep crouch lowers posture without changing interaction flags");
    Check(Publish(character, State{}, error), "Stand after leaving the low ceiling");
    ReconcileAllOnEngineThread();
    Check(actor[0x2AF] == 0 && actor[0x2B0] == 0 && !crouchTag, "Leaving cover clears deep crouch");

    // A correct gameplay flag can coexist with a lost animation tag. A flag
    // readback must not hide this case, nor may reconciliation add one native
    // reference per frame. Use actual native table layouts for path evidence.
    const auto hashName = [](const char* name) {
        std::uint32_t hash = 2166136261U;
        for (; *name != '\0'; ++name) hash = (hash ^ static_cast<std::uint8_t>(*name)) * 16777619U;
        return hash;
    };
    const auto idleHash = hashName("idle"), walkHash = hashName("walk_gait01_l_100");
    baseKeys = {idleHash, 0U, walkHash, 1U};
    if (idleHash < walkHash) baseKeys = {walkHash, 1U, idleHash, 0U};
    crouchKeys = baseKeys;
    baseValues = {1001U, 0U, 2001U, 0U};
    crouchValues = {1002U, 0U, 2002U, 0U};
    contextKeys = {4114047242U, 0U};
    const auto table = [](auto& buffer, auto& keys, auto& values, std::uint32_t count) {
        Store(buffer, 8U, reinterpret_cast<std::uintptr_t>(keys.data()));
        Store(buffer, 12U, reinterpret_cast<std::uintptr_t>(values.data()));
        Store(buffer, 16U, count); Store(buffer, 20U, count);
    };
    table(baseClips, baseKeys, baseValues, 2U);
    table(crouchClips, crouchKeys, crouchValues, 2U);
    Store(contextNames, 8U, reinterpret_cast<std::uintptr_t>(contextKeys.data()));
    Store(contextNames, 12U, reinterpret_cast<std::uintptr_t>(&contextIndex));
    Store(contextNames, 16U, 1U); Store(contextNames, 20U, 1U);
    Store(animationDatabase, 0x10U, 569978346U);
    Store(animationDatabase, 0x20U, reinterpret_cast<std::uintptr_t>(baseClips.data()));
    Store(animationDatabase, 0x28U, reinterpret_cast<std::uintptr_t>(contextNames.data()));
    Store(animationDatabase, 0x2CU, reinterpret_cast<std::uintptr_t>(crouchClips.data()));
    Store(animationInstance, 0x10U, reinterpret_cast<std::uintptr_t>(animationDatabase.data()));
    Store(actor, 0x720U, reinterpret_cast<std::uintptr_t>(animationInstance.data()));
    Store(actor, 0x728U, reinterpret_cast<std::uintptr_t>(animationContexts.data()));
    actor[0x2AF] = actor[0x3E46] = 1U; // no gameplay stance transition left to trigger a tag
    Check(Publish(character, State{1}, error), "Existing crouch with missing animation context");
    ReconcileAllOnEngineThread();
    auto animated = Snapshot(character);
    Check(repairCalls == 1U && animated.animationContextRepairs == 1U && animated.animationContextReadable &&
        animated.animationContext == 4114047242U && animated.animationContextReferences == 1U,
        "Missing CROUCHED context is repaired even if native stance already matches");
    Check(animated.animationDatabase == 569978346U && animated.animationIdlePath == 1002U &&
        animated.animationWalkPath == 2002U, "Diagnostics resolve the actual context's clip paths");
    Store(animationContexts, 0x38U, 2U);
    ReconcileAllOnEngineThread();
    Check(repairCalls == 1U && Snapshot(character).animationContextReferences == 2U,
        "Existing tag reference count is never increased by repeated reconciliation");
    Store(animationContexts, 0x38U, 0U);
    Check(Publish(character, State{}, error), "Stand after animation repair");
    ReconcileAllOnEngineThread();
    animated = Snapshot(character);
    Check(animated.animationIdlePath == 1001U && animated.animationWalkPath == 2001U &&
        animated.lastCrouchDatabase == 569978346U && animated.lastCrouchIdlePath == 1002U &&
        animated.lastCrouchWalkPath == 2002U, "Standing uses base paths and retains the last crouched evidence");
    Store(actor, 0x720U, std::uintptr_t{}); Store(actor, 0x728U, std::uintptr_t{});

    playerComponent = true;
    Check(Publish(character, State{1}, error), "Player-authored representation crouch");
    ReconcileAllOnEngineThread();
    Check(actor[0x2AF] == 1 && crouchTag, "Modifier path covers a representation carrying a player component");
    Check(Publish(character, State{}, error), "Player-authored representation stand");
    ReconcileAllOnEngineThread();
    Check(actor[0x2AF] == 0 && !crouchTag, "Player-authored representation can stand again");
    playerComponent = false;
    Check(Publish(character, State{1}, error), "Desired stance while retail owns an action");
    auto calls = stanceCalls;
    for (const auto state : {0x16, 0x42, 0x4E, 0x4F, 0x47}) {
        cycle[0x1A] = static_cast<std::uint8_t>(state);
        ReconcileAllOnEngineThread();
        Check(stanceCalls == calls, "Traversal/paired/death cycles retain physical ownership");
    }
    cycle[0x1A] = 1; actor[0x297] = 1;
    ReconcileAllOnEngineThread();
    Check(stanceCalls == calls, "Grapple is not overwritten by crouch");
    actor[0x297] = 0; Store(actor, 0x32C, 3U);
    ReconcileAllOnEngineThread();
    Check(stanceCalls == calls, "Dead actor is not modified");
    Store(actor, 0x32C, 0U);
    ignoreNative = true;
    ReconcileAllOnEngineThread();
    Check(Snapshot(character).postureFailures == 1 && !Snapshot(character).appliedCrouched,
        "A refused native posture is reported as a failure, not an application");
    ignoreNative = false;
    ReconcileAllOnEngineThread();
    Check(actor[0x2AF] == 1, "Application recovers after retail releases the posture");
    actor[0x2AF] = 0; ignoreNative = true;
    ReconcileAllOnEngineThread();
    Check(!Snapshot(character).appliedCrouched && std::strcmp(Snapshot(character).stage, "apply-failed") == 0,
        "Failed reapplication reports current readback rather than a previous successful stance");
    ignoreNative = false;
    localPlayer = character; calls = stanceCalls;
    ReconcileAllOnEngineThread();
    Check(stanceCalls == calls && std::strcmp(Snapshot(character).stage, "local-player-guard") == 0,
        "Actual local player is never overwritten by replicated posture");
    localPlayer = 0x123456U;
    Forget(character); calls = stanceCalls;
    ReconcileAllOnEngineThread();
    Check(stanceCalls == calls, "Representation destruction clears the posture mailbox");
    Check(Publish(character, State{1}, error), "Replacement actor can acquire posture");
    ForgetAll(); ReconcileAllOnEngineThread();
    Check(stanceCalls == calls, "Level teardown stops all actor accesses");

    Initialize(image, &RefreshAnimation);
    actor[0x2AF] = actor[0x3E46] = 0U; cycle[0x1A] = 1U;
    Check(Publish(character, State{1}, error), "Crouch requires fresh animation selection");
    ReconcileAllOnEngineThread();
    Check(refreshCalls == 1U && Snapshot(character).animationRefreshes == 1U,
        "Crouch refreshes the cached clip selection once");
    ReconcileAllOnEngineThread(); ReconcileAllOnEngineThread();
    Check(refreshCalls == 1U, "Held posture does not restart the cycle every frame");
    Check(Publish(character, State{}, error), "Stand changes the required animation");
    ReconcileAllOnEngineThread();
    Check(refreshCalls == 2U, "Stand re-selects its clips immediately after crouch");
    refreshSucceeds = false;
    Check(Publish(character, State{1}, error), "Temporarily unavailable animation admission");
    ReconcileAllOnEngineThread(); ReconcileAllOnEngineThread();
    Check(refreshCalls == 3U && Snapshot(character).animationRefreshFailures == 1U,
        "A failed refresh is throttled and does not masquerade as a successful one");
    cycle[0x1A] = 0x42U; Sleep(260);
    ReconcileAllOnEngineThread();
    Check(refreshCalls == 3U, "Paired action ownership defers animation replacement");
    cycle[0x1A] = 1U; refreshSucceeds = true;
    ReconcileAllOnEngineThread();
    Check(refreshCalls == 4U && Snapshot(character).animationRefreshes == 3U,
        "A deferred stance refresh recovers after retail action completion");
    ForgetAll();

    cycle[0x1A] = 0x38U;
    const auto movingCycle = cycle;
    Initialize(image, &RefreshCurrentCycle);
    Check(Publish(character, State{1}, error), "Crouch edge during an active Move cycle");
    ReconcileAllOnEngineThread();
    Check(Publish(character, State{}, error), "Stand edge during the same Move cycle");
    ReconcileAllOnEngineThread();
    Check(moveRefreshes == 2U && idleRefreshes == 0U && cycle == movingCycle && Snapshot(character).animationRefreshes == 2U,
        "Both stance edges re-enter Move without any forced idle transition");
    ReconcileAllOnEngineThread();
    Check(moveRefreshes == 2U, "A held stance never rebuilds Move per frame");
    cycle[0x1A] = 1U;
    Check(Publish(character, State{1}, error), "Crouch edge at rest");
    ReconcileAllOnEngineThread();
    Check(idleRefreshes == 1U && moveRefreshes == 2U, "Stationary pose re-enters Idle immediately");
    cycle[0x1A] = 0x42U;
    Check(!RefreshCurrentCycle(character) && idleRefreshes == 1U && moveRefreshes == 2U,
        "Paired cycles are never reconstructed by stance refresh");
    ForgetAll(); Initialize(image);

    // Exercise both real detour/trampoline ABIs on executable x86 stubs with
    // the certified prologues, hidden result pointer and ret 12 convention.
    const std::array<std::uint8_t, 22> resolverCode{0x55,0x8B,0xEC,0x83,0xEC,0x10,
        0x8B,0x45,0x08,0x8B,0x51,0x04,0x89,0x10,0x89,0x48,0x04,0xC9,0xC2,0x0C,0x00,0xCC};
    for (const auto rva : {0x635FF0U, 0x636160U}) {
        std::memcpy(reinterpret_cast<void*>(image + rva), resolverCode.data(), resolverCode.size());
        if (rva == 0x636160U) *reinterpret_cast<std::uint8_t*>(image + rva + 5U) = 0x24U;
    }
    FlushInstructionCache(GetCurrentProcess(), memory, imageSize);
    aimp::HookManager hooks(true, true);
    const auto probesInstalled = aimp::player_animation_clip_capture::Install(image, hooks, error);
    if (!probesInstalled) std::cerr << "Clip probe installation: " << error << '\n';
    Check(probesInstalled, "Install passive clip resolver probes");
    using ResolveClip = std::uint32_t*(__thiscall*)(void*, std::uint32_t*, const void*, const void*);
    std::array<std::uint32_t, 5> probeDatabase{0U, 0x12345678U, 0U, 0U, 0U};
    std::array<std::uint32_t, 2> result{};
    for (const auto rva : {0x635FF0U, 0x636160U}) {
        const auto returned = reinterpret_cast<ResolveClip>(image + rva)(probeDatabase.data(), result.data(), nullptr, nullptr);
        Check(returned == result.data() && result[0] == 0x12345678U &&
            result[1] == reinterpret_cast<std::uintptr_t>(probeDatabase.data()),
            "Probe preserves ECX, result fields, EAX, hidden output and stack cleanup");
    }
    std::array<std::uint32_t, 6> descriptor{};
    descriptor[4] = 1442749584U;
    probeDatabase[1] = reinterpret_cast<std::uintptr_t>(descriptor.data());
    probeDatabase[4] = 569978346U;
    std::array<std::uint32_t, 3> nameKey{0U, 0U, idleHash};
    Store(animationContexts, 8U, 4114047242U); // single-context argument
    Store(animationContexts, 16U, 4114047242U); Store(animationContexts, 56U, 1U);
    for (const auto rva : {0x635FF0U, 0x636160U}) {
        const auto returned = reinterpret_cast<ResolveClip>(image + rva)(probeDatabase.data(),
            result.data(), nameKey.data(), animationContexts.data());
        Check(returned == result.data() && result[0] == reinterpret_cast<std::uintptr_t>(descriptor.data()) &&
            result[1] == reinterpret_cast<std::uintptr_t>(probeDatabase.data()),
            "Matching E_RIPLEY instrumentation observes the resource without changing its result");
    }
    // Reproduce the retail tree's post-crouch hand/face preload flood. It
    // must not erase the crouched resolver evidence before the user exports.
    aimp::player_animation_clip_capture::Clear();
    nameKey[2] = idleHash;
    Store(animationContexts, 8U, 4114047242U);
    reinterpret_cast<ResolveClip>(image + 0x635FF0U)(probeDatabase.data(), result.data(),
        nameKey.data(), animationContexts.data());
    Store(animationContexts, 8U, 0U);
    for (std::uint32_t i = 0U; i < 200U; ++i) {
        nameKey[2] = 0x10000000U + i;
        reinterpret_cast<ResolveClip>(image + 0x635FF0U)(probeDatabase.data(), result.data(),
            nameKey.data(), animationContexts.data());
    }
    const auto logging = aimp::runtime_features::Enabled(aimp::runtime_features::Feature::NativeDiagnostics);
    aimp::runtime_features::SetEnabled(aimp::runtime_features::Feature::NativeDiagnostics, true);
    aimp::Diagnostics diagnostic;
    Check(diagnostic.Initialize(), "Open resolver regression evidence");
    aimp::player_animation_clip_capture::Export(diagnostic);
    const auto diagnosticPath = diagnostic.Path();
    diagnostic.Shutdown();
    std::ifstream evidence{std::filesystem::path(diagnosticPath)};
    const std::string exported{std::istreambuf_iterator<char>(evidence), std::istreambuf_iterator<char>()};
    Check(exported.find("\"name\":" + std::to_string(idleHash) + ",\"context\":4114047242") != std::string::npos,
        "Crouched resource evidence survives standing tree preload eviction");
    std::size_t exportedRows{}, position{};
    while ((position = exported.find("\"event\":\"remote_animation_clip_resolution\"", position)) != std::string::npos) {
        ++exportedRows; ++position;
    }
    Check(exportedRows == 65U, "Other-context requests remain bounded independently of crouched evidence");
    evidence.close(); DeleteFileW(diagnosticPath.c_str());
    aimp::runtime_features::SetEnabled(aimp::runtime_features::Feature::NativeDiagnostics, logging);
    Check(hooks.UninstallAll(error), "Uninstall passive clip probes");
    Check(hooks.ReleaseRetiredTrampolines(true, error), "Retire clip trampolines after quiescence");

    // Exercise the actual aim hook on a certified x86 prologue, including
    // the trampoline's 224h stack allocation, receiver and return ABI.
    const std::array<std::uint8_t, 12> aimCode{0x81,0xEC,0x24,0x02,0x00,0x00,
        0x81,0xC4,0x24,0x02,0x00,0x00};
    std::memcpy(reinterpret_cast<void*>(image + 0x272B30U), aimCode.data(), aimCode.size());
    Jump(image, 0x272B3CU, reinterpret_cast<std::uintptr_t>(&BindAim));
    FlushInstructionCache(GetCurrentProcess(), memory, imageSize);
    aimp::HookManager unvalidated(false, true);
    Check(!InstallAimHook(image, unvalidated, error), "Aim adaptation rejects an uncertified image");
    Check(InstallAimHook(image, hooks, error), "Install certified remote aim hook");
    const auto callAim = reinterpret_cast<AimSetup>(image + 0x272B30U);
    Store(aimController, 4U, character);
    Store(actor, 0x1100U, reinterpret_cast<std::uintptr_t>(motion.data()));
    Store(actor, 0x720U, reinterpret_cast<std::uintptr_t>(animationInstance.data()));
    Store(aimTree, 0x38U, hashName("PERSISTENT_ACT_AIM_STANDARD"));
    Store(animationDatabase, 0x10U, 569978346U);
    cycle[0x1A] = 1U;
    actor[0x2AF] = 1U;
    Check(Publish(character, State{1}, error), "Publish remote crouch for aim admission");
    expectAimAdaptation = true;
    for (const auto state : {1U, 3U, 0x38U}) {
        cycle[0x1A] = static_cast<std::uint8_t>(state);
        for (const auto stance : {0U, 1U, 5U}) {
            Store(motion, 0x3CU, stance);
            Store(aimController, 0x1D0U, 0.0F);
            const auto beforeActor = actor;
            const auto beforeCycle = cycle;
            callAim(aimController.data());
            Check(*reinterpret_cast<const std::uint32_t*>(motion.data() + 0x3C) == stance &&
                actor == beforeActor && cycle == beforeCycle,
                "Aim restores its borrowed selector and preserves physical/stealth/cycle state");
        }
    }
    Check(aimCalls == 9U && Snapshot(character).crouchedAimApplications == 9U &&
        Snapshot(character).lastAimTree == hashName("PERSISTENT_ACT_AIM_STANDARD"),
        "Aim diagnostics retain actual tree identity after retail binding");
    Store(motion, 0x3CU, 5U); failAim = true;
    Check(AimExceptionPropagates(reinterpret_cast<AimSetup>(&BindAim)) &&
        *reinterpret_cast<const std::uint32_t*>(motion.data() + 0x3C) == 5U,
        "A native setup fault restores the selector and propagates to the original crash boundary");
    failAim = false; expectAimAdaptation = false;
    auto passthrough = [&] {
        Store(aimController, 0x1D0U, 0.25F);
        const auto beforeMotion = motion;
        const auto beforeActor = actor;
        const auto calls = aimCalls;
        const auto adapted = Snapshot(character).crouchedAimApplications;
        callAim(aimController.data());
        Check(aimCalls == calls + 1U && Snapshot(character).crouchedAimApplications == adapted &&
            motion == beforeMotion && actor == beforeActor &&
            *reinterpret_cast<const float*>(aimController.data() + 0x1D0U) == 0.25F,
            "Excluded aim delegates once without altering native stance or mask");
    };
    cycle[0x1A] = 0x42U; passthrough(); cycle[0x1A] = 1U;
    actor[0x297] = 1U; passthrough(); actor[0x297] = 0U;
    actor[0x2B4] = 1U; passthrough(); actor[0x2B4] = 0U;
    Store(actor, 0x32CU, 3U); passthrough(); Store(actor, 0x32CU, 0U);
    Store(motion, 0x3CU, 2U); passthrough(); Store(motion, 0x3CU, 1U);
    localPlayer = character; passthrough(); localPlayer = 0x123456U;
    definition[0x1C] = definition[0x1F] = 1U; passthrough();
    definition[0x1C] = definition[0x1F] = 0U;
    Store(animationDatabase, 0x10U, 123U); passthrough();
    Store(animationDatabase, 0x10U, 569978346U);
    actor[0x2AF] = 0U; passthrough(); actor[0x2AF] = 1U;
    Check(Publish(character, State{}, error), "Standing remote aim admission"); passthrough();
    Check(Publish(character, State{2}, error), "Deep crouch uses the same aim adaptation");
    expectAimAdaptation = true; callAim(aimController.data()); expectAimAdaptation = false;
    ForgetAll(); passthrough();
    Check(hooks.UninstallAll(error), "Uninstall aim hook");
    Check(hooks.ReleaseRetiredTrampolines(true, error), "Retire aim trampoline after quiescence");
    VirtualFree(memory, 0, MEM_RELEASE);
    return failures == 0 ? 0 : 1;
}
