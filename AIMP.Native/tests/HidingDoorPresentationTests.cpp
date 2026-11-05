#include "aimp/HidingDoorPresentation.h"

#include <iostream>
#include <limits>

int main() {
    using namespace aimp::hiding_door_presentation;
    int failures{};
    const auto check = [&](const bool value, const char* text) {
        if (!value) { std::cerr << text << '\n'; ++failures; }
    };
    constexpr std::uint32_t occupied = 0xABCD1234U, adjacent = 0xABCD5678U;
    // All GUID literals here are independently transcribed from the graph's
    // byte strings. These tests catch accidental endian or family swaps.
    const auto tall = RecognizeInputEdge(0x45A72956U, occupied,
        0xFFA58379U, occupied, kEntryEvent, kApplyStart);
    check(tall.kind == 1U && tall.phase == 1U && tall.icon == 0x5DC33E3EU &&
        tall.hierarchy == occupied, "Tall entry retains the selected prop instance");
    const auto low = RecognizeInputEdge(0x4492B8E9U, adjacent,
        0x63527A4DU, adjacent, kExitEvent, kApplyStart);
    check(low.kind == 2U && low.phase == 3U && low.icon == 0x61B51D78U &&
        low.hierarchy == adjacent, "Low exit maps to its exact UI icon sibling");
    check(RecognizeInputEdge(0x3911F0F0U, occupied, 0xFFA58379U, occupied,
        kExitEvent, kApplyStart).phase == 3U, "Tall exit recognized");
    check(RecognizeInputEdge(0x68FA9398U, adjacent, 0x63527A4DU, adjacent,
        kEntryEvent, kApplyStart).phase == 1U, "Low entry recognized");
    check(RecognizeInputEdge(0x45A72956U, occupied, 0xFFA58379U, adjacent,
        kEntryEvent, kApplyStart).kind == 0U, "Reject cross-instance source even for identical prop templates");
    check(RecognizeInputEdge(0x45A72956U, occupied, 0x63527A4DU, occupied,
        kEntryEvent, kApplyStart).kind == 0U, "Reject high/low source family mismatch");
    check(RecognizeInputEdge(0x45A72956U, occupied, 0xFFA58379U, occupied,
        kExitEvent, kApplyStart).kind == 0U, "Reject an entry node on the exit output");
    check(RecognizeInputEdge(0x45A72956U, 0U, 0xFFA58379U, 0U,
        kEntryEvent, kApplyStart).kind == 0U, "Template GUIDs without an instance cannot animate a prop");
    check(RecognizeInputEdge(0x45A72956U, occupied, 0xFFA58379U, occupied,
        kEntryEvent, 0U).kind == 0U, "Reject other inputs");
    check(!IsNewTransition(7U, 100U, 7U, 100U) &&
        !IsNewTransition(7U, 100U, 7U, 99U), "Repeated and out-of-order snapshots do not replay sound");
    check(IsNewTransition(7U, 100U, 7U, 101U) &&
        IsNewTransition(7U, 100U, 8U, 1U), "New transitions and new generations are independent");
    check(!IsNewTransition(7U, 100U, 8U, 0U), "A zero transition is never a native edge");
    check(ReplayAgeAllowed(450U, 50U) && !ReplayAgeAllowed(450U, 51U),
        "Engine mailbox time participates in the stale policy");
    check(!ReplayAgeAllowed(501U, 0U) &&
        !ReplayAgeAllowed(0U, std::numeric_limits<std::uint64_t>::max()),
        "Late or overflowing ages cannot restart animation and audio");
    aimp::player_hiding_presentation::State state{};
    state.phase = 1U; state.kind = 1U; state.entityShortGuid = 0x5DC33E3EU;
    state.hierarchyHash = occupied; state.transition = 1U;
    check(ValidSelection(state), "A baseline can retain exact occupancy without replay");
    state.entityShortGuid = 0x61B51D78U;
    check(!ValidSelection(state), "Managed type assertion cannot select the other prop family");
    return failures == 0 ? 0 : 1;
}
