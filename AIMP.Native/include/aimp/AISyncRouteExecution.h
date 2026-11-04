#pragma once
#include "aimp/AISyncNativeRoute.h"
#include "aimp/AISyncOwnership.h"
#include <string>
namespace aimp { class HookManager; }
namespace aimp::ai_sync::execution {struct NavigationBinding;}
namespace aimp::ai_sync::route_execution {
using EngineThread = bool(*)() noexcept;
using Qualify = bool(*)(const transactions::Scope&) noexcept;
// Exact-build native route executor. No spatial steering or local path search.
// Supported admission: ordinary native common-area start. Installed routes
// survive parameter-only Updates; proven terminal-only edits preserve progress.
// A newer corridor can replace the old range through native invalidation and
// a fresh, worker-free query in the same mapped start area. Until compatible,
// the Client retains the previous Host range; it never solves a connector.
// These static contracts do not certify runtime corridor/traversal fidelity.
[[nodiscard]] bool Install(std::uintptr_t,HookManager&,EngineThread,Qualify,std::string&);
[[nodiscard]] bool Offer(const route::Graph&) noexcept;
void Pump() noexcept;
// Original Character destruction returned for this exact local construction.
// Does not itself release storage or imply that native B/R lists are empty.
void ConfirmActorDestroyed(const execution::ActorLifetime&) noexcept;
// Revoke old-world route ownership before reconfiguration. Only independently
// pinned M/mesh resources with no live native B/R/work may be drained; this
// never treats an in-flight request or a world transition as a Ref release.
void RevokePreviousWorld() noexcept;
[[nodiscard]] bool BeginAdmission(std::uintptr_t navigation) noexcept;
[[nodiscard]] bool Owns(std::uintptr_t navigation) noexcept;
void EndAdmission(std::uintptr_t navigation) noexcept;
[[nodiscard]] bool PendingEndpoint(std::uintptr_t navigation,std::uintptr_t block) noexcept;
[[nodiscard]] bool Awaiting(std::uintptr_t navigation) noexcept;
[[nodiscard]] bool Retired(const transactions::Scope&) noexcept;
// Value-only recovery permission; caller must revalidate the local actor and
// exclude native multi-actor actions before applying a Host transform.
[[nodiscard]] bool HybridBinding(std::uintptr_t character,execution::NavigationBinding&) noexcept;
// Called only after the engine accepted a new full execution scope. Old
// recovery permissions must not be reused with new carriers in the same world.
void BeginScope(const transactions::Scope&) noexcept;
[[nodiscard]] bool CommandRetired(const transactions::Scope&,const transactions::OwnerKey&,std::uint64_t command) noexcept;
// Compatibility setter retained for existing callers. A live Host pose cannot
// prove an imported graph's native start; the executor does not consume it.
using HostPose = bool(*)(const transactions::OwnerKey&, std::array<float,3>&) noexcept;
void SetHostPoseQuery(HostPose) noexcept;
// Diagnostic: retained route slots and the first slot's native quiet checks.
struct SlotReport { std::uint32_t total{}, local{}, quietMask{}, quarantined{}, destroyed{}, revoked{}; }; // local is always zero.
[[nodiscard]] SlotReport LiveSlots() noexcept;
struct Counters {
    std::uint64_t suppressed{},published{},installed{},replacements{},rejected{},retired{},failureCommand{};
    std::uint32_t failureSite{},failureResult{},importStage{},contextCheck{};
    std::uint64_t appendPublished{},continuationPublished{},compoundRequests{};
    std::uint32_t lastAdmissionMode{},lastIncomingCount{},lastCompoundParts{};
    std::uint64_t corridorJoins{}; // routes admitted from the actor's area on the Host corridor
};
[[nodiscard]] Counters Diagnostics() noexcept;
// Copied values only. Wait reasons: 1=start area, 2=native boundary busy,
// 3=compound footprint not ready/owned, 4=remaining suffix unproven,
// 5=Host compound graphs do not share the required exact tail.
// Samples are frame counts.
struct ReplacementReport {
    std::uint64_t requested{},committed{},waitSamples{},command{};
    std::uint32_t reason{},currentArea{},expectedArea{},parameterRevision{},routeRevision{};
};
[[nodiscard]] ReplacementReport ReplacementDiagnostics() noexcept;
}
