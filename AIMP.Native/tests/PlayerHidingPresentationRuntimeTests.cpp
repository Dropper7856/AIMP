#include "aimp/PlayerHidingPresentationRuntime.h"
#include "aimp/AnimationSync.h"
#include <cmath>
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>

namespace {
int failures{};
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; ++failures; }
}
template<class Buffer, class T> void Store(Buffer& bytes, std::size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

}
int main() {
    using namespace aimp::player_hiding_presentation;
    State state{1U, 1U, 0x5DC33E3EU, 123U, 1U, 0U, {1.0F, 2.0F, 3.0F}, 0.5F, true};
    Check(LocalPhase(0, false, state, 0, false, true) == 1U,
        "Accepted entry starts before a scripted NPC clip or hidden flag exists");
    state.elapsedMilliseconds = 750U;
    Check(LocalPhase(1, true, state, 0, false, true) == 1U,
        "First-person entry remains active on a different Character cycle");
    Check(LocalPhase(1, true, state, 0, true, true) == 2U,
        "Delayed native hiding flag settles the accepted entry");
    Check(LocalPhase(1, true, state, 0, false, false) == 0U,
        "Interrupted entry releases when its retail interaction flag clears");
    Check(LocalPhase(0, false, state, 0, false, true) == 0U,
        "An old entry latch cannot start late");
    state.phase = 3U; state.elapsedMilliseconds = 0U;
    Check(LocalPhase(2, false, state, 0, true, true) == 3U,
        "Exact exit edge identifies exit before its Character clip is sampled");
    Check(LocalPhase(3, true, state, 3, false, false) == 3U,
        "An active exit survives hidden flag cleanup");
    Check(LocalPhase(3, true, state, 0, false, false) == 0U,
        "Completed exit clears once without replaying the retained door latch");
    state.phase = 1U;
    Check(ClassifyClip(0xCF2A9F36U).phase == 1U && ClassifyClip(0x3FDD3AF4U).phase == 3U,
        "Native clip aliases still identify occupancy phases");
    Check(ClassifyClip(0x12345678U).phase == 0U, "Unrelated animation cannot become hiding");
    Check(ClipToken(1U, 1U) == 0xCF2A9F36U && ClipToken(2U, 3U) == 0x3FDD3AF4U,
        "Exact native locker aliases bridge scriptless first-person edges");
    Check(ClipToken(0U, 1U) == 0U && ClipToken(1U, 0U) == 0U,
        "Unrelated interactions never synthesize a locker animation");
    Check(std::abs(ExitYaw(1.0F) - (1.0F - 3.14159265358979F)) < 0.0001F &&
        std::abs(ExitYaw(-1.0F) - (-1.0F + 3.14159265358979F)) < 0.0001F,
        "Exit faces out of the locker on either side of the yaw wrap");
    Check(std::abs(aimp::animation_sync::PosturalHeightOffset(0.03F, 2.0F, 1.85F) - 0.18F) < 0.0001F &&
        std::abs(aimp::animation_sync::PosturalHeightOffset(0.03F, 2.0F, 2.0F) - 0.03F) < 0.0001F,
        "A small first-person crouch does not lower the remote actor");
    Check(std::abs(aimp::animation_sync::PosturalHeightOffset(0.03F, 2.0F, 1.5F) - 0.3425F) < 0.0001F,
        "Intermediate first-person lean fades height correction without popping");
    Check(std::abs(aimp::animation_sync::PosturalHeightOffset(0.03F, 2.0F, 1.25F) - 0.03F) < 0.0001F,
        "Real traversal over a larger vertical gap remains synchronized");
    Check(std::abs(aimp::animation_sync::PosturalHeightOffset(0.03F, 2.0F, 1.65F) -
        aimp::animation_sync::PosturalHeightOffset(0.03F, 2.0F, 1.6499F)) < 0.001F,
        "Height correction is continuous at the old snapping transition");
    Check(DeferExitReveal(3U, true, false, 300U) &&
        !DeferExitReveal(3U, true, true, 300U) &&
        !DeferExitReveal(3U, true, false, 1500U),
        "Native exit admission or bounded timeout controls actor reveal");
    Check(!DeferExitReveal(2U, true, false, 100U) &&
        !DeferExitReveal(3U, false, false, 100U),
        "Entry, hidden idle and already-visible characters do not delay reveal");
    state.hierarchyHash = 123U; state.animate = false;
    Initialize(0U);
    std::string error;
    constexpr std::uintptr_t actor = 0x12340000U;
    Check(Publish(actor, state, error) && !Active(actor), "Baseline publication does not arm animation");
    state.animate = true;
    Check(Publish(actor, state, error) && !Active(actor), "Same revision cannot turn a baseline into a replay");
    state.phase = 3U;
    Check(!Publish(actor, state, error), "Same revision cannot change phase");
    state.transition = 2U;
    Check(Publish(actor, state, error) && !Active(actor), "Exit leaves actor playback to AnimationSync");
    State old = state; old.transition = 1U;
    Check(Publish(actor, old, error) && !Active(actor), "Old publication cannot replace the exit transition");
    Check(Publish(actor, {}, error) && !Active(actor), "Clear cancels pending value work without dereferencing actor");
    Check(Publish(actor, state, error) && !Active(actor), "Cleared revision cannot resurrect a stay");
    state.transition = 3U; state.phase = 2U; state.animate = false;
    Check(Publish(actor, state, error) && Active(actor), "Hidden baseline retains movement ownership");
    Forget(actor);
    Check(!Active(actor), "Lifecycle forget releases value ownership");
    state.transition = 1U; state.phase = 1U; state.animate = true;
    Check(Publish(actor, state, error) && !Active(actor), "Fresh entry does not arm legacy actor playback");
    Check(!PumpOnEngineThread(actor) && !ClipPlaying(actor), "Occupancy pump never dereferences the synthetic actor or plays a clip");
    float yaw = 0.5F;
    Check(!HiddenExitHeading(actor, yaw) && yaw == 0.5F, "No manually forced half-turn on locker exit");
    ForgetAll();

    // Read-only native layout fixtures exercise early classification without
    // calling any retail function. No exact door latch means no guessed key.
    std::array<std::uint8_t, 0x4300U> local{};
    std::array<std::uint8_t, 0x200U> cycle{}, root{}, context{};
    const auto localAddress = reinterpret_cast<std::uintptr_t>(local.data());
    const auto cycleAddress = reinterpret_cast<std::uintptr_t>(cycle.data());
    std::uintptr_t players[1]{localAddress};
    auto* image = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1300000U, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (image == nullptr) return 2;
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    const auto rootAddress = reinterpret_cast<std::uintptr_t>(root.data());
    std::memcpy(image + 0x12F0C88U, &rootAddress, sizeof(rootAddress));
    Store(root, 8U, reinterpret_cast<std::uintptr_t>(context.data()));
    Store(context, 0x80U, reinterpret_cast<std::uintptr_t>(players));
    Store(context, 0x84U, std::uint32_t{1U});
    Store(local, 0x50U, std::uint32_t{7U});
    Store(local, 0x36CU, cycleAddress);
    Store(cycle, 4U, localAddress);
    cycle[0x1AU] = 1U;
    Initialize(base);
    State captured{};
    Check(CaptureLocalOnEngineThread(localAddress, captured) && captured.phase == 0U,
        "Visible actual local Character returns empty State");
    Store(cycle, 0U, base + 0xEADB58U);
    Store(cycle, 0x14U, std::uintptr_t{0x12340000U});
    Store(cycle, 0x68U, std::uint32_t{2554777613U});
    Store(cycle, 0x6CU, std::uint32_t{0xCF2A9F36U});
    cycle[0x1AU] = 0x12U;
    Check(!CaptureLocalOnEngineThread(localAddress, captured),
        "Early active entry with flag13 false requires exact native door latch");
    local[0x4249U] = 1U;
    Check(!CaptureLocalOnEngineThread(localAddress, captured),
        "Delayed hidden flag cannot manufacture exact interaction identity");
    Store(local, 0x50U, std::uint32_t{0U});
    Check(!CaptureLocalOnEngineThread(localAddress, captured), "Unready Character is not exported");
    ForgetAll();

    Initialize(0U);
    VirtualFree(image, 0U, MEM_RELEASE);
    return failures == 0 ? 0 : 1;
}
