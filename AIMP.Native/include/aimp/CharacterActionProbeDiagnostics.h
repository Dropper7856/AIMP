#pragma once

#include <cstdint>
#include <string>

namespace aimp {

class Diagnostics;
class HookManager;

namespace character_action_probe {

struct NativeShootingRequestRef final {
    void* object{};
    void* control{};
};

struct ShootTraceArm final {
    std::uintptr_t characterAddress{};
    std::uint64_t generation{};
    std::uintptr_t shootingManager{};
    std::uintptr_t equipmentController{};
    NativeShootingRequestRef request{};
    const void* releaseAddress{};
    const void* cancelAddress{};
    Diagnostics* diagnostics{};
};

// Installs two exact-build, read-only observation redirects:
//  - the three retail calls to FUN_007342A0 from FUN_00734690;
//  - the retail call to FUN_00473570 from FUN_007342A0.
// They only increment counters for the currently armed Character/manager and
// always forward to the untouched retail target.
bool InstallExactBuildObservationHooks(
    std::uintptr_t moduleBase,
    bool exactExecutableHash,
    HookManager& hooks,
    std::string& error);

[[nodiscard]] bool ObservationHooksInstalled() noexcept;

// Exact-build, read-only snapshot of the two tables installed by the
// CATHODE::NPC_TriggerShootRequest implementation. The inspector never calls
// any table entry and never writes to game memory. It records table entries
// that point into the loaded .text section, a bounded byte window for each
// candidate, and heuristic direct E8 call targets to help map the runtime
// callback back to AI.exe.c.
bool InspectNpcTriggerShootRequestTables(
    std::uintptr_t moduleBase,
    bool exactExecutableHash,
    Diagnostics& diagnostics,
    std::string& message);

// Exact-build, read-only comparison of the main/interface tables installed by
// NPC_TriggerAimRequest, NPC_TriggerShootRequest, and CMD_ForceReloadWeapon.
// The comparator never invokes a table entry. It compares slots, resolves a
// bounded one-level near-jump thunk, and uses the already-known reload executor
// (FUN_00603290) only as an anchor to identify the corresponding Shoot/Aim slot.
bool CompareCathodeActionNodeVTables(
    std::uintptr_t moduleBase,
    bool exactExecutableHash,
    Diagnostics& diagnostics,
    std::string& message);

bool CanArmTrace(std::string& reason);

// Takes ownership of arm.request. The request shared-ref remains held for the
// bounded trace so R can be read safely even if the retail queue removes it.
// When the trace completes naturally, the reference is released unless the
// request is still stuck in the manager; a stuck request is retained only so a
// later explicit CancelHeldRequest can remove it through the retail path.
bool ArmTrace(ShootTraceArm arm, std::string& error);

// Called after the natural Character-processing callback and command drain.
// It performs reads only, records transitions, and emits one bounded summary.
void ObserveEngineFrame(std::uint64_t currentGeneration) noexcept;

// Used only by the exact-build call-site redirects.
void ObserveCharacterControlUpdate(void* control) noexcept;
void ObserveShootingManagerTick(void* manager) noexcept;

// Explicit recovery tool for a probe request that remained queued. This is not
// used automatically by the trace and therefore does not alter the diagnostic
// run. It invokes FUN_00471E90 on the retained request, then releases our ref.
bool CancelHeldRequest(std::uintptr_t characterAddress, std::string& message);

// Compatibility only: called once at admission, from the natural equipment
// update. May cancel ONLY the exact request reference retained in this module.
// Rejects other active engine requests instead of clearing their queue.
bool RetireOwnedRequestForActionRuntime(std::uintptr_t characterAddress,
    std::uint64_t generation, std::string& error);

void BeginLogicalShutdown() noexcept;

} // namespace character_action_probe
} // namespace aimp
