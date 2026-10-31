#pragma once
#include "aimp/AISyncExecutionCodec.h"
#include "aimp/AISyncRouteCodec.h"
#include "aimp/AISyncActionCorrelation.h"
#include "aimp/AISyncCausalState.h"
#include <atomic>
#include <bit>
#include <mutex>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::ai_sync::host_producer {
// Typed producer contracts. Neither a pose nor a P1 observation converts here.
struct Command {
    std::uint32_t kind{}, mode{};
    bool point{};
    float x{}, y{}, z{}, heading{}, tolerance{};
    movement::Bundle movement{};
    std::uint32_t admission{},scalarRetention{3};
    std::uint32_t actionFlags{}; // kind5 +20/+21/+22, bits0..2
    native_intent::Payload intent{};
    bool operator==(const Command& other) const noexcept {
        const auto same=[](float a,float b) noexcept {return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);};
        return kind==other.kind && mode==other.mode && point==other.point && same(x,other.x) && same(y,other.y) &&
            same(z,other.z) && same(heading,other.heading) && same(tolerance,other.tolerance) && movement::Same(movement,other.movement) &&
            admission==other.admission&&scalarRetention==other.scalarRetention&&actionFlags==other.actionFlags&&intent==other.intent;
    }
};
struct HostPacket {
    bool control{};
    bool route{};
    bool state{};
    std::size_t size{};
    std::uint64_t token{};
    std::array<std::uint8_t, route::kMaxBytes> bytes{};
};
struct CommandIdentity {
    transactions::Scope scope{};
    transactions::OwnerKey owner{};
    std::uint64_t command{};
    std::uint32_t parameterRevision{}, nativeKind{};
    movement::Navigation navigation{};
    bool operator==(const CommandIdentity&) const noexcept = default;
};
enum class SourceResult : std::uint8_t { Ok, Unbound, Invalid, UnknownRequest, Conflict, Capacity, CoverageLost };
struct HistoryDiagnostics { std::uint64_t through{}; std::size_t commands{}, packets{}; };
// Engine-thread source with a worker Peek/Commit consumer. IDs, sequence and
// tombstones are scoped; a replaced native address never reuses commandId.
// Every accepted observation is retained or closes coverage explicitly.
class Producer final {
public:
    static constexpr std::size_t kCommands = 256, kOwners = 128, kPackets = 256;
    [[nodiscard]] bool Configure(const execution::Control&, std::uint32_t nativeGeneration) noexcept;
    void Retire() noexcept;
    [[nodiscard]] SourceResult Admit(const transactions::OwnerKey&, std::uint64_t localRequest,
        const Command&, std::uint64_t parentCommand = 0) noexcept;
    [[nodiscard]] SourceResult Update(std::uint64_t localRequest, const Command&) noexcept;
    [[nodiscard]] SourceResult Cancel(std::uint64_t localRequest) noexcept;
    // Native one-shot Character calls. These never impersonate scheduler Refs.
    [[nodiscard]] SourceResult NativeEvent(const transactions::OwnerKey&, const Command&) noexcept;
    [[nodiscard]] SourceResult AppendState(const State&) noexcept;
    [[nodiscard]] SourceResult Complete(std::uint64_t localRequest) noexcept;
    // Release is an observed native reference boundary, never a timeout.
    [[nodiscard]] SourceResult Release(std::uint64_t localRequest) noexcept;
    void LoseCoverage(execution::Recovery reason = execution::Recovery::NativeUnavailable) noexcept;
    [[nodiscard]] bool Peek(HostPacket&) noexcept;
    [[nodiscard]] bool Commit(std::uint64_t token) noexcept;
    [[nodiscard]] bool Bound() const noexcept { return active_.load(std::memory_order_acquire); }
    [[nodiscard]] bool Current(const transactions::Scope&, std::uint32_t generation) const noexcept;
    [[nodiscard]] bool Scope(transactions::Scope&) const noexcept;
    // Correlation for execution results, never identity inferred from a pose.
    // Terminal/retired requests cannot acquire a new route under an old ID.
    [[nodiscard]] bool Identify(std::uint64_t localRequest, CommandIdentity&) const noexcept;
    [[nodiscard]] bool ActionReference(std::uint64_t localRequest,std::uint32_t executionRevision,
        action_correlation::Reference&) const noexcept;
    // Snapshot the next route key for this EXACT live command/parameter set.
    // Does not consume a revision: only AppendRoute commits it to the outbox.
    // A mutation or competing publication between capture and append is refused.
    [[nodiscard]] bool NextRouteKey(const CommandIdentity&, std::uint32_t part, route::Key&) const noexcept;
    [[nodiscard]] bool PreviousRouteAuxiliary(const CommandIdentity&,std::uint32_t part,std::array<float,6>&) const noexcept;
    // Uses the SAME causal outbox as Submit/Update and terminal operations.
    // An already terminal command can never publish a delayed solved route.
    [[nodiscard]] SourceResult AppendRoute(const route::Graph&) noexcept;
    [[nodiscard]] SourceResult BlockRoute(const CommandIdentity&,std::uint32_t part,route::Failure) noexcept;
    [[nodiscard]] std::uint32_t Profiles() const noexcept;
    [[nodiscard]] bool Checkpoint(const execution::Control&, std::uint32_t generation) noexcept;
    [[nodiscard]] HistoryDiagnostics History() const noexcept;
    // First closure of the current binding: Producer source line, Recovery
    // reason and the native kind being emitted (0 when not command-bound).
    struct Failure { std::uint32_t site{}, reason{}, kind{}; std::uint64_t count{}; };
    [[nodiscard]] Failure LastFailure() const noexcept;
private:
    struct Owner { transactions::OwnerKey key{}; std::uint64_t sequence{}; bool used{}; };
    struct Entry {
        transactions::OwnerKey owner{};
        Command parameters{};
        std::uint64_t request{}, command{}, parent{}, lastSequence{},lastOwnerSequence{};
        transactions::Op lastOperation{transactions::Op::Begin};
        std::uint32_t revision{1};
        std::array<std::uint32_t,2> routeRevisions{};
        std::array<std::array<float,6>,2> routeAuxiliary{};
        std::array<bool,2> routeAuxiliaryKnown{};
        bool used{}, cancelled{}, completed{}, released{}, routeBlocked{};
    };
    [[nodiscard]] SourceResult Lifecycle(std::uint64_t, transactions::Op) noexcept;
    [[nodiscard]] Entry* Find(std::uint64_t) noexcept;
    [[nodiscard]] Owner* FindOwner(const transactions::OwnerKey&) noexcept;
    [[nodiscard]] bool Emit(Entry&, std::span<const transactions::Op>, bool payload) noexcept;
    void FailLocked(execution::Recovery, std::uint32_t site = __builtin_LINE()) noexcept;
    [[nodiscard]] bool ControlLocked(execution::ControlKind, std::uint32_t, std::uint64_t through = 0) noexcept;
    mutable std::mutex gate_;
    std::array<Entry,kCommands> commands_{};
    std::array<Owner,kOwners> owners_{};
    std::array<HostPacket,kPackets> packets_{};
    execution::Control binding_{};
    std::uint32_t generation_{};
    std::uint64_t command_{}, global_{}, batch_{}, token_{};
    std::uint64_t historyFloor_{};
    std::size_t read_{}, count_{};
    std::atomic<bool> active_{};
    std::uint32_t emittingKind_{};
    Failure failure_{};
};

using QualifyHost = bool(*)(const transactions::Scope&, std::uint32_t generation) noexcept;
// QueueBindingHost is called only after the IPC layer has authenticated Host
// WorldSync authority and the FULL session GUID. The qualifier runs on engine.
[[nodiscard]] bool Initialize(QualifyHost) noexcept;
[[nodiscard]] bool QueueBindingHost(std::span<const std::uint8_t>, std::uint32_t nativeGeneration) noexcept;
void PumpHost() noexcept;
void RetireHost() noexcept;
[[nodiscard]] bool PeekHost(HostPacket&) noexcept;
[[nodiscard]] bool CommitHost(std::uint64_t token) noexcept;
[[nodiscard]] bool HostScope(transactions::Scope&) noexcept;
[[nodiscard]] Producer& EngineProducer() noexcept;
// Diagnostic: every operation the Host actually emitted. Runs under the outbox
// mutex; callback must only copy values and must never reenter this producer.
using CommandTrace = void(*)(const transactions::Record&) noexcept;
void SetCommandTrace(CommandTrace) noexcept;
using NativeContext = bool(*)(std::uintptr_t character, transactions::OwnerKey&) noexcept;
using NativeGuard = bool(*)() noexcept;
// Engine-qualified exact loaded marker identity, never a raw marker index on wire.
using NativeMarkerCapture = bool(*)(std::uint32_t localMarker,native_intent::Payload&) noexcept;
void SetNativeMarkerCapture(NativeMarkerCapture) noexcept;
// Only the certified0068192F pre-insertion seam may supply authored geometry.
void BeforeNativeMarkerInsertion(std::uintptr_t navigation,std::uintptr_t block) noexcept;
using NativeSchedulerObserver = void(*)(std::uintptr_t scheduler,bool entering) noexcept;
void SetNativeSchedulerObserver(NativeSchedulerObserver) noexcept;
// Exact status before00683470 can overwrite a terminal disposition with6.
void BeforeNativeRetention(std::uintptr_t scheduler,std::uintptr_t request) noexcept;
struct NativePlacementIdentity {
    CommandIdentity command{};
    std::uintptr_t scheduler{},request{},character{};
    std::uint32_t runtime{},branch{};
    bool operator==(const NativePlacementIdentity&) const noexcept = default;
};
// Exact current head, held by our live native observer lease. Read-only:
// returns false for provisional/unpublished/mutated/foreign requests. The
// caller must refresh pending source parameters before entering mode8.
[[nodiscard]] bool IdentifyNativePlacement(std::uintptr_t scheduler,NativePlacementIdentity&) noexcept;
using NativeRetirementGuard = bool(*)(const transactions::Scope&, const transactions::OwnerKey&,
    std::uintptr_t character, std::uint32_t runtime) noexcept;
// Exact-build native capture. The existing Submit detour forwards successful
// admissions here; status and scheduler boundaries are installed only once.
[[nodiscard]] bool InstallNative(std::uintptr_t imageBase, HookManager&, NativeContext, NativeGuard,
    NativeRetirementGuard, std::string& error);
void ObserveNativeAdmission(std::uintptr_t request, bool accepted) noexcept;
// Brackets the certified 0070A940 producer, including nested/unbound frames.
// Admission inputs are captured BEFORE synchronous native callbacks run.
void BeginNativeMove(std::uintptr_t character, std::uintptr_t state) noexcept;
void EndNativeMove() noexcept;
void BeforeNativeAdmission(std::uintptr_t request, std::uint32_t admission = 0) noexcept;
// Diagnostics: admissions of scheduler kinds with no executable contract.
[[nodiscard]] std::uint64_t UncoveredKind(std::uint32_t kind) noexcept;
// Source closures by producer site (source line of AISyncHostProducerNative.cpp).
struct CoverageDiagnostics { std::uint64_t losses{}; std::uint32_t firstSite{}, lastSite{}; };
[[nodiscard]] CoverageDiagnostics Coverage() noexcept;
// Unobserved requests cancelled at binding so the Host brain reissues them.
[[nodiscard]] std::uint64_t BootstrapCancels() noexcept;
void ObserveNativeMutation(std::uintptr_t request) noexcept;
// Bracket callback-capable setters, including the shared P1 hook path.
void BeginNativeMutation() noexcept;
void EndNativeMutation() noexcept;
[[nodiscard]] bool BeforeNativeActorDestruction(std::uintptr_t character, std::uint32_t runtime) noexcept;
void PumpNative() noexcept;
// After the retail scheduler executes: associate only its active head kind5
// with the exact resulting cycle. Does not infer ownership from a state alone.
void ObserveNativeActionExecution(std::uintptr_t scheduler) noexcept;
void RetireNative(bool liveWorld) noexcept;
// Correlates only an exact scheduler-executed kind5/cycle binding.
// Missing/ambiguous evidence returns false, without closing capture coverage.
[[nodiscard]] bool IdentifyNativeAction(std::uintptr_t character,std::uintptr_t cycle,std::uint32_t typeRva,
    std::uint8_t state,std::uint8_t serial,std::uint32_t executionRevision,action_correlation::Reference&) noexcept;
void InvalidateNativeActor(std::uintptr_t character, std::uint32_t runtime) noexcept;
[[nodiscard]] bool NativeIdle() noexcept;
// New-world startup may ignore ONLY old-world, terminally fenced native
// observations. Their strong Refs remain pinned and cannot be reused as new
// requests. Same-world restart and active/unknown current-world work still
// require the ordinary NativeIdle proof.
[[nodiscard]] bool NativeReadyForNewWorld(const transactions::Scope&) noexcept;
[[nodiscard]] std::uint32_t RetainedNativeLeases() noexcept;
[[nodiscard]] std::uint32_t FencedOldNativeLeases() noexcept;
// Resolve a real native N/B pair to its already accepted logical command.
// part=0 primary, part=1 companion. Local pointers never leave this boundary.
[[nodiscard]] bool IdentifyNativeNavigation(std::uintptr_t navigation, std::uintptr_t block,
    CommandIdentity&, std::uint32_t& part) noexcept;
// Engine-thread checkpoint of a single already admitted complete scalar
// request, or an empty scheduler. Multiple/unsupported native work refuses.
[[nodiscard]] bool BootstrapNativeActor(std::uintptr_t character) noexcept;
} // namespace aimp::ai_sync::host_producer
