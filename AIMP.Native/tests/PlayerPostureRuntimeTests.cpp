#include "aimp/PlayerPostureRuntime.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>

namespace {
int failures{};
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
std::array<std::uint8_t, 0x4400> actor{};
std::array<std::uint8_t, 0x100> definition{}, cycle{}, root{}, manager{};
std::uintptr_t localPlayer = 0x123456U;
bool crouchTag{}, playerComponent{}, ignoreNative{};
std::uint32_t stanceCalls{};

template<class T, class Buffer> void Store(Buffer& buffer, std::size_t offset, T value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
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
    VirtualFree(memory, 0, MEM_RELEASE);
    return failures == 0 ? 0 : 1;
}
