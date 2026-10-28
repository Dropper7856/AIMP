#pragma once

#include "aimp/AISyncInteraction.h"
#include <string>

namespace aimp { class HookManager; }

namespace aimp::ai_sync::pair_retirement {

// Engine-thread scope/key/binding qualification only. The caller additionally
// proves both live construction tickets before Track reads native storage.
// Only Track calls this callback. Hooks and CheckReleased do not, including
// action destruction on a worker or proof delivery after actor revocation.
using Qualifier = bool(*)(const interactions::Key&, const interactions::Binding&) noexcept;

// Exact retail 00d6 only. Installs the concrete 4E/4F cleanup and action
// destructor observers, rolling back only this installation's hook suffix.
// HookManager and its published trampolines must outlive native callbacks.
[[nodiscard]] bool Install(std::uintptr_t imageBase, HookManager&, Qualifier, std::string& error);

// Call after admission and after later exact snapshots while native action /
// graph initialization is pending. A zero graph never proves disposal. Once
// captured, cycle and cycle serial cannot change within this receipt.
// A new scope resets bounded negative ledgers only after every previous native
// obligation completed and CheckReleased delivered its proof. Unknown old
// obligations block that reset; no callback evidence is erased to bypass them.
[[nodiscard]] interactions::Check Track(const interactions::Key&, std::uint64_t receiptSerial,
    const interactions::Binding&, const interactions::PairState&) noexcept;
[[nodiscard]] interactions::Check CheckReleased(const interactions::Key&, std::uint64_t receiptSerial) noexcept;
// A native cycle cleanup RETURN makes that side's old snapshot obsolete even
// while the other side/action still owes cleanup. Does not release resources.
[[nodiscard]] interactions::Check CheckCycleRetired(const interactions::Key&,
    std::uint64_t receiptSerial, bool victim) noexcept;
// A dead victim never leaves its terminal victim cycle, so its cleanup never
// returns. Ready when the SOURCE side is fully cleaned and disposed; the entry
// is then forgotten so the source can enter its next interaction.
[[nodiscard]] interactions::Check ReleaseWithDeadVictim(const interactions::Key&, std::uint64_t receiptSerial) noexcept;

// Prevents new tracking of this construction. Existing return tokens and
// independently tracked source/victim cleanup evidence remain valid.
void Invalidate(const interactions::LocalParticipant&) noexcept;

// Ready releases AIMP's value-only reservation. It is not a worker-join or a
// license to free engine graphs, native Refs, resources or callback storage.
} // namespace aimp::ai_sync::pair_retirement
