#pragma once
#include <array>
#include <cstdint>
#include <string_view>

#include "aimp/AnimationSync.h"

// AnimationSync plays only third-person clips on a remote representation.
// The owner still reports the first-person alias its own scripted cycle runs;
// the receiver translates it here into an alias the five representation sets
// (E_RIPLEY, LAMBERT, DALLAS, PARKER, ASH) already hold natively. A
// first-person clip without a row is not played at all: the representation
// keeps its own NPC idle/locomotion. Never add a FEMALEFP clip here.
//
// Source of truth: tools/animationsync-3p/inventory.tsv (emit_table.py prints
// these rows). A = retail third-person version of the same action; B = native
// NPC clip standing in until its Blender rework replaces the target alias;
// C = third-person clip made for AIMP (Blender, built once by OpenCAGE) that
// the launcher adds to the five sets (AIMP.Launcher ThirdPersonAnimationClips).
namespace aimp::animation_sync::third_person {
enum class Tier : std::uint8_t { A, B, C };

struct Row final {
    std::string_view firstPerson, thirdPerson;
    Tier tier;
    // Built frame for frame on the first-person clip's timeline (same frame
    // count and timing): it keeps the owner's frame range and follows the
    // owner's clock, holds included. Staged interactions need this: Lever_2
    // pauses lever_2_use on the handle until the player pulls (click + S).
    bool ownerTimeline{};
};

// CATHODE animation alias token: FNV-1a 32 of the (lowercase) clip name.
[[nodiscard]] constexpr std::uint32_t Alias(const std::string_view name) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const char c : name) { hash ^= static_cast<std::uint8_t>(c); hash *= 16777619U; }
    return hash;
}

inline constexpr std::array kRows{
    // Interactions
    Row{"cut_panel_fp_cut", "cut_panel_fp_cut_3p", Tier::C, true},
    Row{"cut_panel_fp_enter", "cut_panel_fp_enter_3p", Tier::C, true},
    Row{"cut_panel_fp_pull_lever", "cut_panel_fp_pull_lever_3p", Tier::C, true},
    Row{"cut_panel_fp_put_cutter_away", "cut_panel_fp_put_cutter_away_3p", Tier::C, true},
    Row{"cut_push_out_vent_panel", "cut_push_out_vent_panel_3p", Tier::C, true},
    Row{"door_brace_end", "door_brace_end_3p", Tier::C, true},
    Row{"door_brace_full", "door_brace_full_3p", Tier::C, true},
    Row{"door_brace_loop", "door_brace_loop_3p", Tier::C, true},
    Row{"door_brace_start", "door_brace_start_3p", Tier::C, true},
    Row{"hacking_tool_confirm", "hacking_tool_confirm_3p", Tier::C, true},
    Row{"hacking_tool_intro", "hacking_tool_intro_3p", Tier::C, true},
    Row{"hacking_tool_loop", "hacking_tool_loop_3p", Tier::C, true},
    Row{"hacking_tool_nudge1", "hacking_tool_nudge1_3p", Tier::C, true},
    Row{"hacking_tool_nudge2", "hacking_tool_nudge2_3p", Tier::C, true},
    Row{"hacking_tool_nudge3", "hacking_tool_nudge3_3p", Tier::C, true},
    Row{"hacking_tool_nudge_down", "hacking_tool_nudge_down_3p", Tier::C, true},
    Row{"hacking_tool_nudge_left", "hacking_tool_nudge_left_3p", Tier::C, true},
    Row{"hacking_tool_nudge_right", "hacking_tool_nudge_right_3p", Tier::C, true},
    Row{"hacking_tool_nudge_up", "hacking_tool_nudge_up_3p", Tier::C, true},
    Row{"hacking_tool_outro", "hacking_tool_outro_3p", Tier::C, true},
    Row{"hacking_tool_twist_switch", "hacking_tool_twist_switch_3p", Tier::C, true},
    Row{"hacking_tool_twist_switch_scrub", "hacking_tool_twist_switch_scrub_3p", Tier::C, true},
    Row{"interact_keycard_stand_add", "interact_keycard_stand_add_3p", Tier::C, true},
    Row{"keyboard_generic_use_01", "keyboard_generic_use_01_3p", Tier::C, true},
    Row{"kill_fp_floor_vent_grab_001", "kill_fp_floor_vent_grab_001_3p", Tier::C, true},
    Row{"kill_fp_hiding_locker_001", "kill_fp_hiding_locker_001_3p", Tier::C, true},
    Row{"kill_fp_hiding_locker_low_001", "kill_fp_hiding_locker_low_001_3p", Tier::C, true},
    Row{"kill_fp_hiding_locker_low_002", "kill_fp_hiding_locker_low_002_3p", Tier::C, true},
    Row{"kill_fp_hiding_locker_low_003", "kill_fp_hiding_locker_low_003_3p", Tier::C, true},
    Row{"kill_fp_hiding_locker_low_004", "kill_fp_hiding_locker_low_004_3p", Tier::C, true},
    Row{"kill_fp_wall_vent_grab_back_001", "kill_fp_wall_vent_grab_back_001_3p", Tier::C, true},
    Row{"kill_fp_wall_vent_grab_back_002", "kill_fp_wall_vent_grab_back_002_3p", Tier::C, true},
    Row{"kill_fp_wall_vent_grab_back_003", "kill_fp_wall_vent_grab_back_003_3p", Tier::C, true},
    Row{"lever_2_use", "lever_2_use_3p", Tier::C, true},
    Row{"lift_button_press", "lift_button_press_3p", Tier::C, true},
    Row{"player_hiding_draggedout_android", "player_hiding_draggedout_android_3p", Tier::C, true},
    Row{"player_hiding_high_enter", "player_hiding_high_enter_3p", Tier::C, true},
    Row{"player_hiding_high_exit", "player_hiding_high_exit_3p", Tier::C, true},
    Row{"player_hiding_high_idle", "player_hiding_high_idle_3p", Tier::C, true},
    Row{"player_hiding_high_torrens", "player_hiding_high_torrens_3p", Tier::C, true},
    Row{"player_hiding_low_draggedout_android", "player_hiding_low_draggedout_android_3p", Tier::C, true},
    Row{"player_hiding_low_enter", "player_hiding_low_enter_3p", Tier::C, true},
    Row{"player_hiding_low_exit", "player_hiding_low_exit_3p", Tier::C, true},
    Row{"player_hiding_low_idle", "player_hiding_low_idle_3p", Tier::C, true},
    Row{"portable_computer_use_01", "portable_computer_use_01_3p", Tier::C, true},
    Row{"push_button_01", "push_button_01_3p", Tier::C, true},
    Row{"replace_lift_component", "replace_lift_component_3p", Tier::C, true},
    Row{"rewire_box_close_player", "rewire_box_close_player_3p", Tier::C, true},
    Row{"rewire_box_open_player", "rewire_box_open_player_3p", Tier::C, true},
    Row{"rewire_box_use_player", "rewire_box_use_player_3p", Tier::C, true},
    Row{"trunction_activate", "trunction_activate_3p", Tier::C, true},
    Row{"use_terminal_breathe", "use_terminal_breathe_3p", Tier::C, true},
    Row{"use_terminal_exit", "use_terminal_exit_3p", Tier::C, true},
    Row{"use_terminal_intro", "use_terminal_intro_3p", Tier::C, true},
    // Traversal (retail FEMALE vent clips; NPC MALE ladders/leaps/mantles/vaults)
    Row{"cut_panel_fp_exit", "cut_panel_fp_exit_3p", Tier::C, true},
    Row{"cut_panel_fp_put_cutter_away_fast", "cut_panel_fp_put_cutter_away_fast_3p", Tier::C, true},
    Row{"cut_panel_trans_to_panel", "cut_panel_trans_to_panel_3p", Tier::C, true},
    Row{"cut_vent_panel", "cut_vent_panel_3p", Tier::C, true},
    Row{"cut_vent_trans_to_vent", "cut_vent_trans_to_vent_3p", Tier::C, true},
    Row{"cutterupgradecanister", "cutterupgradecanister_3p", Tier::C, true},
    Row{"cutting_tool_pickup", "cutting_tool_pickup_3p", Tier::C, true},
    Row{"deep_crouch_fail_attach", "deep_crouch_fail_attach_3p", Tier::C, true},
    Row{"fp_flame_pickup", "fp_flame_pickup_3p", Tier::C, true},
    Row{"fp_flame_pickup_lower", "fp_flame_pickup_lower_3p", Tier::C, true},
    Row{"fp_radiation_vent_entry", "fp_radiation_vent_entry_3p", Tier::C, true},
    Row{"hackingupgradecap", "hackingupgradecap_3p", Tier::C, true},
    Row{"m21_internal_lift_hatch_climb", "m21_internal_lift_hatch_climb_3p", Tier::C, true},
    Row{"m21_lift_shaft_leap", "m21_lift_shaft_leap_3p", Tier::C, true},
    Row{"m21_piperoom_end", "m21_piperoom_end_3p", Tier::C, true},
    Row{"m21_piperoom_end_mantle", "m21_piperoom_end_mantle_3p", Tier::C, true},
    Row{"m21_piperoom_mid_mantle", "m21_piperoom_mid_mantle_3p", Tier::C, true},
    Row{"m21_piperoom_start", "m21_piperoom_start_3p", Tier::C, true},
    Row{"melee_ground_01_p1", "melee_ground_01_p1_3p", Tier::C, true},
    Row{"melee_ground_01_p2", "melee_ground_01_p2_3p", Tier::C, true},
    Row{"open_vent_from_inside", "open_vent_from_inside_3p", Tier::C, true},
    Row{"player_floor_vent_extract_b", "player_floor_vent_extract_b_3p", Tier::C, true},
    Row{"player_floor_vent_extract_f", "player_floor_vent_extract_f_3p", Tier::C, true},
    Row{"player_wall_vent_extract_b", "player_wall_vent_extract_b_3p", Tier::C, true},
    Row{"player_wall_vent_extract_f", "player_wall_vent_extract_f_3p", Tier::C, true},
    Row{"sd_arm_botleft", "sd_arm_botleft_3p", Tier::C, true},
    Row{"sd_arm_botright", "sd_arm_botright_3p", Tier::C, true},
    Row{"sd_arm_breathe", "sd_arm_breathe_3p", Tier::C, true},
    Row{"sd_arm_exit", "sd_arm_exit_3p", Tier::C, true},
    Row{"sd_arm_intro", "sd_arm_intro_3p", Tier::C, true},
    Row{"sd_arm_intro_open", "sd_arm_intro_open_3p", Tier::C, true},
    Row{"sd_arm_topleft", "sd_arm_topleft_3p", Tier::C, true},
    Row{"sd_arm_topright", "sd_arm_topright_3p", Tier::C, true},
    Row{"tracker_pickup", "tracker_pickup_3p", Tier::C, true},
    Row{"trav_sidle_idle", "trav_sidle_idle_3p", Tier::C, true},
    Row{"trav_sidle_in", "trav_sidle_in_3p", Tier::C, true},
    Row{"trav_sidle_move_l", "trav_sidle_move_l_3p", Tier::C, true},
    Row{"trav_sidle_move_r", "trav_sidle_move_r_3p", Tier::C, true},
    Row{"trav_sidle_out", "trav_sidle_out_3p", Tier::C, true},
    Row{"enter_vent", "enter_vent", Tier::A},
    Row{"exit_floor_vent", "exit_vent", Tier::A},
    Row{"fp_ladder_crouch_onto_bottom", "trav_ladder_stand_to_ladder_idle", Tier::A},
    Row{"fp_ladder_crouch_onto_bottom_left", "trav_ladder_stand_to_ladder_idle", Tier::A},
    Row{"fp_ladder_crouch_onto_top", "trav_ladder_stand_down_to_ladder_idle", Tier::A},
    Row{"fp_ladder_crouch_onto_top_left", "trav_ladder_stand_down_to_ladder_idle", Tier::A},
    Row{"fp_ladder_down_lhand", "trav_ladder_down_cycle_40cm_leftstart", Tier::A},
    Row{"fp_ladder_down_rhand", "trav_ladder_down_cycle_40cm_leftstart", Tier::A},
    Row{"fp_ladder_idle_lhand", "trav_ladder_idle", Tier::A},
    Row{"fp_ladder_idle_rhand", "trav_ladder_idle", Tier::A},
    Row{"fp_ladder_leap_onto_bottom", "trav_ladder_stand_to_ladder_idle", Tier::A},
    Row{"fp_ladder_leap_onto_bottom_left", "trav_ladder_stand_to_ladder_idle", Tier::A},
    Row{"fp_ladder_off_bottom_lhand", "trav_ladder_down_to_stand", Tier::A},
    Row{"fp_ladder_off_bottom_rhand", "trav_ladder_down_to_stand", Tier::A},
    Row{"fp_ladder_off_bottom_to_crouch_lhand", "trav_ladder_down_to_stand", Tier::A},
    Row{"fp_ladder_off_bottom_to_crouch_rhand", "trav_ladder_down_to_stand", Tier::A},
    Row{"fp_ladder_off_top_left_lhand", "trav_ladder_idle_off_top_to_stand", Tier::A},
    Row{"fp_ladder_off_top_left_rhand", "trav_ladder_idle_off_top_to_stand", Tier::A},
    Row{"fp_ladder_off_top_lhand", "trav_ladder_idle_off_top_to_stand", Tier::A},
    Row{"fp_ladder_off_top_rhand", "trav_ladder_idle_off_top_to_stand", Tier::A},
    Row{"fp_ladder_off_top_right_lhand", "trav_ladder_idle_off_top_to_stand", Tier::A},
    Row{"fp_ladder_off_top_right_rhand", "trav_ladder_idle_off_top_to_stand", Tier::A},
    Row{"fp_ladder_off_top_to_crouch_lhand", "trav_ladder_idle_off_top_to_stand", Tier::A},
    Row{"fp_ladder_off_top_to_crouch_rhand", "trav_ladder_idle_off_top_to_stand", Tier::A},
    Row{"fp_ladder_onto_bottom", "trav_ladder_stand_to_ladder_idle", Tier::A},
    Row{"fp_ladder_onto_bottom_head_on", "trav_ladder_stand_to_ladder_idle", Tier::A},
    Row{"fp_ladder_onto_bottom_left", "trav_ladder_stand_to_ladder_idle", Tier::A},
    Row{"fp_ladder_onto_top", "trav_ladder_stand_down_to_ladder_idle", Tier::A},
    Row{"fp_ladder_onto_top_left", "trav_ladder_stand_down_to_ladder_idle", Tier::A},
    Row{"fp_ladder_onto_top_right", "trav_ladder_stand_down_to_ladder_idle", Tier::A},
    Row{"fp_ladder_up_lhand", "trav_ladder_up_cycle_40cm_leftstart", Tier::A},
    Row{"fp_ladder_up_rhand", "trav_ladder_up_cycle_40cm_leftstart", Tier::A},
    Row{"trav_fp_leap_1_5m", "trav_leap_1_5m", Tier::A},
    Row{"trav_fp_leap_2_5m", "trav_leap_2_5m", Tier::A},
    Row{"trav_fp_leap_4m", "trav_fp_leap_4m_3p", Tier::C, true},
    Row{"trav_fp_mantle_up_1_5m", "trav_mantle_up_1_5m", Tier::A},
    Row{"trav_fp_mantle_up_1m", "trav_mantle_up_1m", Tier::A},
    Row{"trav_fp_mantle_up_2m", "trav_mantle_up_2m", Tier::A},
    Row{"trav_fp_vault_1_5m", "trav_vault_1_5m", Tier::A},
    Row{"trav_fp_vault_1m", "trav_vault_1m", Tier::A},
    Row{"trav_fp_vault_2m", "trav_vault_2m", Tier::A},
};

// The row for an owner's first-person alias token, nullptr = none.
[[nodiscard]] constexpr const Row* Find(const std::uint32_t firstPerson) noexcept {
    for (const auto& row : kRows)
        if (Alias(row.firstPerson) == firstPerson) return &row;
    return nullptr;
}

// Third-person alias for an owner's first-person alias token, 0 = none.
[[nodiscard]] constexpr std::uint32_t Lookup(const std::uint32_t firstPerson) noexcept {
    const auto* row = Find(firstPerson);
    return row == nullptr ? 0U : Alias(row->thirdPerson);
}

// Rewrites an owner's state for a representation. Returns false when the
// first-person clip has no third-person version (play nothing). First-person
// body effectors are always dropped. `ownerClock` tells whether the owner's
// clip clock and frame range still apply: the same take (same alias, the
// retail rig variant) or a clip built on the owner's timeline. A stand-in
// with its own timing plays whole on its own clock.
[[nodiscard]] inline bool Translate(State& state, bool& ownerClock) noexcept {
    ownerClock = false;
    if (!Active(state)) return true;
    const auto* row = Find(state.animation);
    if (row == nullptr) return false;
    const auto target = Alias(row->thirdPerson);
    ownerClock = target == state.animation || row->ownerTimeline;
    state.animation = target;
    if (!ownerClock) { state.startFrame = -1; state.endFrame = -1; }
    state.posed = false;
    state.effectors = {};
    return true;
}
}
