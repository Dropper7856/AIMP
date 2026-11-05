#include "aimp/HidingPresentationAudio.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <string>

namespace {
int failures{};
void Check(bool result, const char* message) {
    if (!result) { std::cerr << message << '\n'; ++failures; }
}
}

// Value-level contract only: no engine call is reachable before the retail
// SOUND_INTERFACE ABI is certified, and each revision is consumed once.
int main() {
    using namespace aimp::hiding_presentation_audio;
    using aimp::player_hiding_presentation::State;
    Check(std::strcmp(Selection(1U, 1U).name, "play_hiding_locker_in") == 0, "High entry event");
    Check(std::strcmp(Selection(1U, 3U).name, "play_hiding_locker_out") == 0, "High exit event");
    Check(std::strcmp(Selection(2U, 1U).name, "play_hiding_locker_low_in") == 0, "Low entry event");
    Check(std::strcmp(Selection(2U, 3U).name, "play_hiding_locker_low_out") == 0, "Low exit event");
    Check(Selection(1U, 2U).name == nullptr && Selection(2U, 0U).name == nullptr,
        "Settled hidden and visible phases are silent");

    std::string error;
    Check(!Initialize(0U, error) && !error.empty(), "A missing image is not a certified audio ABI");
    auto* image = VirtualAlloc(nullptr, 0x460000U, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (image == nullptr) return 1;
    Check(!Initialize(reinterpret_cast<std::uintptr_t>(image), error), "Zeroed code is rejected");

    std::array<std::uint8_t, 0x4000> actor{};
    const auto character = reinterpret_cast<std::uintptr_t>(actor.data());
    State state{1U, 1U, 0x5DC33E3EU, 123U, 1U, 0U, {1.0F, 2.0F, 3.0F}, 0.5F, true};
    Check(!PlayOnEngineThread(character, state, 0U), "Uncertified ABI never posts");
    auto d = Snapshot(character);
    Check(d.attempted == 1U && d.failed == 1U && std::strcmp(d.stage, "not-installed") == 0,
        "Uncertified ABI reports its own stage");
    Check(!PlayOnEngineThread(character, state, 0U) && Snapshot(character).duplicate == 1U,
        "A revision is consumed once");
    state.transition = 2U;
    Check(!PlayOnEngineThread(character, state, 900U) && Snapshot(character).stale == 1U,
        "A late transition is not replayed as a sound");
    state.transition = 3U; state.animate = false;
    Check(!PlayOnEngineThread(character, state, 0U) && Snapshot(character).baseline == 1U,
        "A baseline (join-in-progress) state is silent");
    state.transition = 4U; state.phase = 2U; state.animate = true;
    Check(!PlayOnEngineThread(character, state, 0U) &&
        std::strcmp(Snapshot(character).stage, "hidden") == 0, "Settled hidden state is silent");
    Forget(character);
    Check(Snapshot(character).attempted == 0U, "Forget drops the value binding");
    VirtualFree(image, 0U, MEM_RELEASE);
    if (failures == 0) std::cout << "HidingPresentationAudio tests passed\n";
    return failures == 0 ? 0 : 1;
}
