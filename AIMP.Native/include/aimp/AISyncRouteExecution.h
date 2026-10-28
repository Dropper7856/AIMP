#pragma once
#include "aimp/AISyncNativeRoute.h"
#include "aimp/AISyncOwnership.h"
#include <string>
namespace aimp { class HookManager; }
namespace aimp::ai_sync::route_execution {
using EngineThread = bool(*)() noexcept;
using Qualify = bool(*)(const transactions::Scope&) noexcept;
// Exact-build native route executor. No spatial steering or local path search.
[[nodiscard]] bool Install(std::uintptr_t,HookManager&,EngineThread,Qualify,std::string&);
[[nodiscard]] bool Offer(const route::Graph&) noexcept;
void Pump() noexcept;
// Original Character destruction returned for this exact local construction.
// Does not itself release storage or imply that native B/R lists are empty.
void ConfirmActorDestroyed(const execution::ActorLifetime&) noexcept;
[[nodiscard]] bool BeginAdmission(std::uintptr_t navigation) noexcept;
[[nodiscard]] bool Owns(std::uintptr_t navigation) noexcept;
void EndAdmission(std::uintptr_t navigation) noexcept;
[[nodiscard]] bool PendingEndpoint(std::uintptr_t navigation,std::uintptr_t block) noexcept;
[[nodiscard]] bool Awaiting(std::uintptr_t navigation) noexcept;
[[nodiscard]] bool Retired(const transactions::Scope&) noexcept;
// Host published pose of the command owner (latest causal state).
using HostPose = bool(*)(const transactions::OwnerKey&, std::array<float,3>&) noexcept;
void SetHostPoseQuery(HostPose) noexcept;
// Diagnostic: retained route slots and the first slot's native quiet checks.
struct SlotReport { std::uint32_t total{}, local{}, quietMask{}; };
[[nodiscard]] SlotReport LiveSlots() noexcept;
struct Counters {
    std::uint64_t suppressed{},published{},installed{},replacements{},rejected{},retired{},failureCommand{};
    std::uint32_t failureSite{},failureResult{},importStage{},contextCheck{};
};
[[nodiscard]] Counters Diagnostics() noexcept;
}
