#include "aimp/AnimationSyncThirdPerson.h"
#include "aimp/PlayerHidingPresentationRuntime.h"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
namespace tp = aimp::animation_sync::third_person;
using aimp::animation_sync::State;

void Require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

State Owner(const std::string_view alias) {
    State state{};
    state.transition = 7U; state.animation = tp::Alias(alias);
    state.startFrame = 12; state.endFrame = 40; state.loops = -1;
    state.durationSeconds = 3.0F; state.phaseSeconds = 1.0F;
    state.posed = true; state.effectors.fill(0.25F);
    return state;
}
}

int main() {
    try {
        // The alias token the engine uses (memory: enter_vent = 256590389).
        static_assert(tp::Alias("enter_vent") == 256590389U);

        // Retail third-person take of the same action: the owner's clock applies.
        auto vent = Owner("enter_vent");
        bool ownerClock{};
        Require(tp::Translate(vent, ownerClock) && ownerClock, "enter_vent keeps its own retail 3P take");
        Require(vent.animation == tp::Alias("enter_vent") && vent.startFrame == 12 && vent.endFrame == 40,
            "same take keeps the owner's frame range");
        Require(!vent.posed && vent.effectors == std::array<float, 12>{}, "first-person effectors are dropped");

        // Former NPC stand-ins are now AIMP clips on the owner's timeline.
        auto computer = Owner("keyboard_generic_use_01");
        Require(tp::Translate(computer, ownerClock) && ownerClock && computer.animation == tp::Alias("keyboard_generic_use_01_3p") &&
                    computer.startFrame == 12, "keyboard_generic_use_01 -> keyboard_generic_use_01_3p on the owner's timeline");

        auto button = Owner("push_button_01");
        Require(tp::Translate(button, ownerClock) && ownerClock && button.animation == tp::Alias("push_button_01_3p") &&
                    button.startFrame == 12 && button.endFrame == 40, "push_button_01 -> push_button_01_3p on the owner's timeline");

        auto floorVent = Owner("exit_floor_vent");
        Require(tp::Translate(floorVent, ownerClock) && floorVent.animation == tp::Alias("exit_vent"),
            "exit_floor_vent uses exit_vent, present in all five representation sets");

        // AIMP-made third-person clip (launcher-installed), built on the FP timeline.
        auto lever = Owner("lever_2_use");
        Require(tp::Translate(lever, ownerClock) && ownerClock && lever.animation == tp::Alias("lever_2_use_3p") &&
                    lever.startFrame == 12 && lever.endFrame == 40 && !lever.posed,
            "lever_2_use -> lever_2_use_3p, on the owner's timeline: owner clock and frames (Lever_2 holds on the handle)");

        // Locker clips: the representation plays *_3p; receiver-side hiding logic
        // must classify them exactly like the owner's first-person aliases.
        for (const auto alias : {"player_hiding_high_enter", "player_hiding_high_idle", "player_hiding_high_exit",
                 "player_hiding_low_enter", "player_hiding_low_idle", "player_hiding_low_exit"}) {
            auto state = Owner(alias);
            const auto fp = aimp::player_hiding_presentation::ClassifyClip(state.animation);
            Require(fp.phase != 0U && tp::Translate(state, ownerClock), std::string(alias) + " is a mapped locker phase");
            const auto tp3 = aimp::player_hiding_presentation::ClassifyClip(state.animation);
            Require(tp3.phase == fp.phase && tp3.kind == fp.kind, std::string(alias) + "_3p keeps its locker phase");
        }

        // No third-person version: nothing is played.
        for (const auto alias : {"gas_mask_equip", "amb_light_in_face_in", "gas_reaction"}) {
            auto state = Owner(alias);
            Require(!tp::Translate(state, ownerClock), std::string(alias) + " has no third-person clip");
        }
        auto cleared = State{};
        Require(tp::Translate(cleared, ownerClock) && cleared == State{}, "a clear stays a clear");

        // Table hygiene: unique first-person keys, no first-person target names.
        std::set<std::uint32_t> keys;
        for (const auto& row : tp::kRows) {
            Require(keys.insert(tp::Alias(row.firstPerson)).second, "duplicate row: " + std::string(row.firstPerson));
            // AIMP clips (tier C) are named after their FP source plus _3p and
            // are built on its timeline; retail targets must look third-person.
            if (row.tier == tp::Tier::C) {
                Require(row.ownerTimeline && row.thirdPerson == std::string(row.firstPerson) + "_3p",
                    "AIMP clip naming/timeline: " + std::string(row.thirdPerson));
                continue;
            }
            Require(row.thirdPerson.find("fp_") == std::string_view::npos &&
                    row.thirdPerson.find("player") == std::string_view::npos,
                "first-person-looking target: " + std::string(row.thirdPerson));
        }
        std::cout << "AnimationSync third-person table: " << tp::kRows.size() << " rows verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
