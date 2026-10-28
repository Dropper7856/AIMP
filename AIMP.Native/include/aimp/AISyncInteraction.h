#pragma once

#include "aimp/AISyncTransactions.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace aimp::ai_sync::interactions {

// Logical values only. A native definition selector is scoped to the exact
// actor template/resource set certified by the embedding capability adapter.
struct Participant {
    std::uint64_t peer{};
    std::uint32_t entityA{}, entityB{}, birth{}, actorClass{};
    bool operator==(const Participant&) const noexcept = default;
};
struct Key {
    transactions::Scope scope{};
    std::uint64_t command{};
    bool operator==(const Key&) const noexcept = default;
};
enum class Transition : std::uint8_t { A = 1, B = 2 };
enum class Perspective : std::uint8_t { Npc, LocalPlayer, RemotePlayer };
struct Outcome {
    std::uint32_t revision{}, flags{}, definition{}, kind{};
    std::uint32_t hpBefore{}, hpPrepared{}, hpReaction{}, healthReaction{};
    float sourceYaw{};
    std::array<float, 3> targetOffset{};
    bool operator==(const Outcome&) const noexcept = default;
};
// Facts reported only after the corresponding local native operations return.
// A claim/reservation, admission, HP observation or timeout is not evidence.
enum OutcomeEvidence : std::uint32_t {
    PreparationApplied = 1U, ReactionApplied = 2U, NativeDeathApplied = 4U
};
[[nodiscard]] bool SamePreparation(const Outcome&, const Outcome&) noexcept;
struct Command {
    Key key{};
    Participant source{}, victim{};
    std::uint32_t pairRevision{}, transitionRevision{};
    Transition transition{Transition::A};
    std::int32_t sourceDefinition{-1};
    std::uint32_t firstFlag{}, secondFlag{};
    // Exact Host preparation/result, matching State::NativeOutcome fields.
    // The embedding permission callback compares this to its published bound
    // outcome before ClaimOutcome/native preparation may consume it.
    Outcome outcome{};
    bool outcomeRequired{};
    bool operator==(const Command&) const noexcept = default;
};
struct LocalParticipant {
    std::uintptr_t character{};
    std::uint32_t runtimeId{}, entityA{}, entityB{};
    Perspective perspective{Perspective::Npc};
    // Local construction ticket plus the portable player-life token. Neither
    // native address/runtime nor the current round can substitute for these.
    std::uint64_t creationSerial{};
    std::uint32_t participantIncarnation{};
    bool operator==(const LocalParticipant&) const noexcept = default;
};
struct Binding {
    LocalParticipant source{}, victim{};
    // A locally certified capability result, never an unchecked network flag.
    // The family-specific retail pair parameter remains with its existing
    // adapter; this layer does not infer a species from Character class.
    std::uint32_t localPairFlagsHigh{};
    bool exclusiveExecution{}, outcomeBound{};
    bool operator==(const Binding&) const noexcept = default;
};
struct Definition {
    std::uintptr_t manager{}, object{};
    std::uint32_t selector{}, kind{};
    std::int32_t victimAttachment{-1};
    bool operator==(const Definition&) const noexcept = default;
};
struct PairState {
    std::uintptr_t source{}, victim{}, sourceCycle{}, victimCycle{};
    std::uintptr_t sourceGraph{}, victimGraph{}, definition{};
    std::uint32_t sourceRuntimeId{}, victimRuntimeId{}, stage{};
    std::uint8_t sourceSerial{}, victimSerial{}, sourceFlags{}, victimFlags{};
    bool sourcePrepared{}, sourceFinished{}, victimAcquired{}, victimEnded{}, linked{};
};
enum class Check : std::uint8_t { Ready, Waiting, Rejected, Unknown };
enum class Result : std::uint8_t {
    Admitted, Pending, Completed, Released, Duplicate, Waiting, Rejected,
    Conflict, WrongScope, Capacity, Unknown, Reentrant
};
enum class Phase : std::uint8_t { Entered, Registered, Running, Completed, Interrupted, Fenced, Released };
enum class FinishCause : std::uint8_t {
    None, NativeSourceFinish, NativeVictimFinish, NativeGraphEnd,
    NativeInterruption, NativeDestruction
};
struct Receipt {
    Key key{};
    std::uint64_t serial{};
    Phase phase{Phase::Entered};
    bool pairEntered{}, transitionEntered{}, sourceCompleted{}, victimCompleted{};
    bool outcomeApplied{}, resourcesReleased{};
    std::uint32_t outcomeRevision{}, appliedOutcomeRevision{};
    std::uint32_t requiredOutcomeEvidence{}, observedOutcomeEvidence{};
    std::uint32_t failureReason{}; // Bitmask: read/source/cycle/serial/definition/victim/link/revocation.
    std::uint32_t failureSite{}; // First native-adapter fence, diagnostic source line only.
    FinishCause sourceCause{}, victimCause{};
    PairState native{};
};
using Permission = Check(*)(void*, const Command&, const Binding&) noexcept;
using ReleaseProof = Check(*)(void*, const Command&, const Binding&, const Receipt&) noexcept;
// Originals already published by the embedding HookManager. These are local
// pointers and can never be supplied by the wire. Zero selects installed retail
// entries, preserving their guards. The embedding outcome scope must be active.
struct Entrypoints { std::uintptr_t pair{}, transitionA{}, transitionB{}; };

// Saved x86 00d6 layout, exposed separately so the same inspection code can be
// tested with a memory reader without launching or emulating the game.
using ReadMemory = bool(*)(void*, std::uintptr_t, void*, std::size_t) noexcept;
struct Memory { std::uintptr_t imageBase{}; void* context{}; ReadMemory read{}; };
[[nodiscard]] bool InspectNativePair(const Memory&, std::uintptr_t source,
    std::uint32_t runtimeId, PairState&) noexcept;
[[nodiscard]] bool OwnsSource(const PairState&) noexcept;

// Engine-thread adapter for the existing pair registration and native
// transition A (4E/4F). B constructs cycle42 and is rejected before entry
// until its separate lifetime contract is implemented. This adapter does not
// run a producer, place actors, pump a BT, manufacture
// markers, or infer completion from HP/time. Partial entry is never retried.
// Caller establishes exact session/thread/world/participant/outcome guards.
// Worker callbacks must be queued by the embedding owner and drained here.
// Diagnostic only: code of the last check that refused a pair before any
// native entry (1xx runtime, 2xx adapter, 3xx Client permission). 0 = none.
std::uint32_t& RefusalSite() noexcept;
class NativePairAdapter final {
public:
    static constexpr std::size_t kCapacity = 32;
    NativePairAdapter() noexcept = default;
    NativePairAdapter(const NativePairAdapter&) = delete;
    NativePairAdapter& operator=(const NativePairAdapter&) = delete;
    [[nodiscard]] bool Initialize(std::uintptr_t imageBase, bool exactBuildValidated,
        Entrypoints, Permission, void* context) noexcept;
    [[nodiscard]] Result Configure(const transactions::Scope&) noexcept;
    [[nodiscard]] Result Admit(const Command&, const Binding&, Receipt&) noexcept;
    [[nodiscard]] Result Inspect(const Key&, Receipt&) noexcept;
    // Source/victim callbacks are independent facts. A victim finish called
    // from destruction is interruption, never successful graph completion.
    [[nodiscard]] Result Observe(const Key&, std::uint64_t serial,
        const LocalParticipant&, std::uintptr_t cycle, std::uint8_t cycleSerial,
        FinishCause, Receipt&) noexcept;
    // Preparation identity never changes. Progress can add finish obligations
    // and change the reaction result without replaying registration/preparation.
    [[nodiscard]] Result AdvanceOutcome(const Key&, std::uint64_t serial, const Outcome&) noexcept;
    // Echo the exact latest outcome, including revision. Stale events cannot
    // discharge a newer reaction or a newly required native death operation.
    [[nodiscard]] Result OutcomeApplied(const Key&, std::uint64_t serial,
        const Outcome&, std::uint32_t evidence) noexcept;
    [[nodiscard]] Result Release(const Key&, ReleaseProof, void* context, Receipt&) noexcept;
    void Invalidate(const LocalParticipant&) noexcept;
    [[nodiscard]] bool Find(const Key&, Receipt&) const noexcept;
private:
    friend struct NativePairAdapterTestAccess;
    struct Lease { Command command{}; Binding binding{}; Definition definition{}, preparedDefinition{}; Receipt receipt{}; Outcome outcome{}; bool revoked{}, captured{}, preparedSelected{}; };
    [[nodiscard]] Lease* FindLease(const Key&) noexcept;
    [[nodiscard]] Check CheckBinding(const Command&, const Binding&) noexcept;
    [[nodiscard]] bool ResolveDefinition(const Binding&, const Command&, Definition&) noexcept;
    [[nodiscard]] bool Register(const Binding&, const Definition&, bool& accepted) noexcept;
    [[nodiscard]] bool TransitionNative(const Binding&, const Command&) noexcept;
    [[nodiscard]] bool SnapshotNative(const Binding&, PairState&) noexcept;
    [[nodiscard]] bool DefinitionLive(const Binding&, const Definition&) noexcept;
    [[nodiscard]] bool DefinitionMatches(Lease&, const PairState&) noexcept;
    [[nodiscard]] Result AdvanceAdmission(Lease&, Receipt&) noexcept;
    [[nodiscard]] Result Fence(Lease&, Receipt&, std::uint32_t site=__builtin_LINE()) noexcept;
    std::array<Lease, kCapacity> leases_{};
    std::size_t count_{};
    std::uint64_t serial_{};
    std::uint64_t revocation_{};
    std::uintptr_t base_{};
    Entrypoints entries_{};
    Permission permission_{};
    void* context_{};
    transactions::Scope scope_{};
    bool configured_{}, busy_{};
    // Private primitive seam: tests exercise Admit/Inspect/Observe/Release,
    // including partial entry and revocation, through the production code.
    void* primitivesContext_{};
    Check (*bindingPrimitive_)(void*, const Command&, const Binding&) noexcept{};
    bool (*definitionPrimitive_)(void*, const Binding&, const Command&, Definition&) noexcept{};
    bool (*registerPrimitive_)(void*, const Binding&, const Definition&, bool&) noexcept{};
    bool (*transitionPrimitive_)(void*, const Binding&, const Command&) noexcept{};
    bool (*snapshotPrimitive_)(void*, const Binding&, PairState&) noexcept{};
    bool (*definitionLivePrimitive_)(void*, const Binding&, const Definition&) noexcept{};
};
} // namespace aimp::ai_sync::interactions
