#pragma once
#include "aimp/AISyncExecutionMailbox.h"
#include "aimp/AISyncNativeRequestAdapter.h"
#include "aimp/AISyncOwnership.h"
#include "aimp/AISyncActionCorrelation.h"
#include "aimp/AISyncExecutionSession.h"

namespace aimp::ai_sync::execution_runtime {
using QualifyScope = bool(*)(const transactions::Scope&, std::uint32_t) noexcept;
// Reserved for a Host interaction: orders after globalSeq must wait.
using ReservationQuery = bool(*)(const transactions::Scope&, const transactions::OwnerKey&, std::uint64_t globalSeq) noexcept;
using ContextReady = bool(*)(const transactions::Scope&) noexcept;
// The Host's spawn baseline has been applied and confirmed for this Character.
using PlacementReady = bool(*)(std::uintptr_t character) noexcept;
// Optional endpoint, no automatic actor ownership. Local hooks must prove a
// safe legacy drain, native quiescence and restoration. Absent hooks leave
// Submit Waiting. commandAdmission is a bootstrap-only opt-in requiring ALL
// local hooks. Only a retained Waiting Submit may then request the local drain;
// no decoded/rejected command or duplicate can grant ownership by itself.
[[nodiscard]] bool Initialize(std::uintptr_t imageBase, bool exactBuild, QualifyScope,
    execution::OwnershipAdapter localOwnership = {}, QualifyScope retirementQualify = nullptr,
    bool commandAdmission = false, ReservationQuery reserved = nullptr, ContextReady contextReady = nullptr,
    PlacementReady placementReady = nullptr) noexcept;
// Qualified engine thread only. No network/IPC entry points for these grants.
[[nodiscard]] execution::OwnershipResult RequestOwnership(const transactions::OwnerKey&) noexcept;
[[nodiscard]] execution::OwnershipResult RetryOwnership(const transactions::OwnerKey&) noexcept;
[[nodiscard]] execution::OwnershipResult RetireOwnership(const transactions::OwnerKey&) noexcept;
[[nodiscard]] execution::OwnershipResult ReleaseOwnership(const transactions::OwnerKey&) noexcept;
[[nodiscard]] bool AllowsLegacy(const execution::ActorLifetime&) noexcept;
void InvalidateActor(const execution::ActorLifetime&) noexcept;
// Exact first pre-destruction boundary while the published Actor is still
// alive. Closes the stream and retires known request Refs synchronously;
// ownership/route quiescence remains separately fenced by InvalidateActor.
void BeforeActorDestruction(const execution::ActorLifetime&) noexcept;
// Called only AFTER the original CharacterFullDestroy returns, using its
// pre-call construction ticket. Thread-safe copied values; no native reads.
void AfterActorDestruction(const execution::ActorLifetime&) noexcept;
[[nodiscard]] bool QueueBinding(std::span<const std::uint8_t>, std::uint32_t localNativeGeneration) noexcept;
[[nodiscard]] bool QueueBatch(std::span<const std::uint8_t>, bool retry = false) noexcept;
[[nodiscard]] bool QueueRoute(std::span<const std::uint8_t>) noexcept;
[[nodiscard]] bool QueueCarrier(std::span<const std::uint8_t>) noexcept;
void Retire() noexcept;
[[nodiscard]] bool Available() noexcept;
[[nodiscard]] execution::MailboxFailure LastMailboxFailure() noexcept;
// Published by the engine-thread pump; safe to read from the diagnostics writer.
[[nodiscard]] execution::WaitReport WaitSnapshot() noexcept;
[[nodiscard]] bool CurrentScope(transactions::Scope&) noexcept; // Qualified engine thread only.
[[nodiscard]] action_correlation::Readiness ActionReadiness(const action_correlation::Reference&,
    const transactions::OwnerKey&, const execution::ActorLifetime&, std::size_t* blockingRequests = nullptr,
    bool admittedHistory = false) noexcept;
[[nodiscard]] std::size_t ActorRequests(std::uintptr_t) noexcept; // Value-only engine query.
// Hybrid follower may coexist with a fully granted scheduler capability, but
// must not restart locomotion while its initial drain or a native action runs.
[[nodiscard]] bool OrdinaryMotionAvailable(const execution::ActorLifetime&) noexcept;
[[nodiscard]] execution::NavigationOwnership IdentifyNavigation(std::uintptr_t n,std::uintptr_t b,
    execution::NavigationBinding&) noexcept;
[[nodiscard]] bool Matches(const transactions::Scope&) noexcept; // Copied IPC values only.
void Fail(const transactions::Scope&,execution::Recovery) noexcept;
void Pump(bool includeRetirements = true) noexcept;
// Run before changing the legacy context as well as from Pump. No network
// command is dispatched here. The retirement qualifier may admit FrozenClient.
void PumpRetirements() noexcept;
[[nodiscard]] bool ContextRetirementComplete() noexcept; // Engine thread only, no native calls.
// Diagnostic: what last kept ContextRetirementComplete false (0 none).
[[nodiscard]] std::uint32_t ContextRetirementBlocker() noexcept;
struct RetirementDiagnostics {
    std::uint32_t references{}, unknownReferences{}, fencedOwners{};
    bool retired{}, referencesReleased{}, contextReleased{};
};
[[nodiscard]] RetirementDiagnostics RetirementSnapshot() noexcept; // Atomic value snapshot, any thread.
struct HistoryDiagnostics { std::uint64_t through{}, checkpoints{}, records{}; };
[[nodiscard]] HistoryDiagnostics HistorySnapshot() noexcept; // Approximate counters, no native reads.
[[nodiscard]] bool Peek(execution::MailboxPacket&) noexcept;
[[nodiscard]] bool Commit(std::uint64_t token) noexcept;
} // namespace aimp::ai_sync::execution_runtime
