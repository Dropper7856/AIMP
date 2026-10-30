#pragma once

#include "aimp/AISyncTransactions.h"
#include <array>

namespace aimp::ai_sync::execution {
struct LocalActorBinding {
    std::uintptr_t character{};
    std::uint32_t runtimeId{};
    // The coordinator must have retired the legacy writer at a safe boundary.
    // This flag is a local proof, NEVER a field received from the network.
    bool exclusiveExecution{};
    std::uint64_t creationSerial{};
    bool operator==(const LocalActorBinding&) const noexcept = default;
};
using ResolveActor = bool(*)(void*, const transactions::OwnerKey&, LocalActorBinding&) noexcept;
using Permission = bool(*)(void*, const transactions::Record&) noexcept;
enum class Readiness : std::uint8_t { Ready, Waiting, Rejected };
// Temporary capability ownership is a dependency, not an authority failure.
using DependencyGate = Readiness(*)(void*, const transactions::Record&) noexcept;
// Exact native call dispatch. Payload/ABI are certified locally; snapshots
// never select an operation. The embedding validates current world/lifetime.
using IntentCheck = Readiness(*)(const transactions::Record&,const LocalActorBinding&) noexcept;
using IntentInvoke = transactions::NativeOutcome(*)(const transactions::Record&,const LocalActorBinding&) noexcept;
void SetNativeIntentAdapter(IntentCheck,IntentInvoke) noexcept;
// Hybrid AISync consumes navigation/ordinary-turn/control contracts as causal bookkeeping.
// Its single pose follower owns ordinary locomotion; no second native path
// request is allocated. The embedding still validates scope/incarnation.
void SetHybridMovementAdapter(IntentCheck,IntentInvoke) noexcept;
// Diagnostic trace of one native Character lifecycle event (placement,
// backstage, frontstage). Host stages: 1 capture guard closed, 2 actor not
// tracked, 3 nested call skipped, 4 emitted, 5 emission failed. Client
// stages: 10 rejected by validation, 12 replayed (result = native outcome).
struct LifecycleTraceSample {
    std::uint32_t stage{}, kind{}, mode{}, result{}, caller{}, callbackThread{};
    std::uint64_t seq{};
    float x{}, y{}, z{}, yaw{};
    float beforeX{}, beforeY{}, beforeZ{}, afterX{}, afterY{}, afterZ{};
    bool hasGoal{};
};
using LifecycleTrace = void(*)(const LifecycleTraceSample&) noexcept;
void SetLifecycleTrace(LifecycleTrace) noexcept;
// Diagnostic: every adapter Invoke result. A zero Character denotes a check
// refusal or logical-only cleanup, not proof of entry into CATHODE.
using CommandTrace = void(*)(const transactions::Record&, std::uintptr_t character, std::uint32_t outcome) noexcept;
void SetCommandTrace(CommandTrace) noexcept;
// A Host action whose native effect this Client already entered through its
// ordered interaction transaction (the attack's paired cycle). Executing it
// again would replace that running cycle. True = already executed natively.
using ConsumedAction = bool(*)(const transactions::Record&) noexcept;
void SetConsumedAction(ConsumedAction) noexcept;
[[nodiscard]] std::uint64_t ConsumedActions() noexcept;
// Latest authoritative Host pose of this owner, as already received in the
// ordered stream. Used only to keep the Client from running AHEAD of the Host.
using HostPoseQuery = bool(*)(const transactions::OwnerKey&, std::array<float,3>&) noexcept;
void SetHostPoseQuery(HostPoseQuery) noexcept;
// True while another native executor (paired interaction source/victim)
// owns this Character. Path steering never edits a Move during that time.
using ExecutionOwnedQuery = bool(*)(std::uintptr_t character) noexcept;
void SetExecutionOwnedQuery(ExecutionOwnedQuery) noexcept;
[[nodiscard]] bool MovementExecutionOwned(std::uintptr_t character) noexcept;
[[nodiscard]] std::uint64_t SteersSuspended() noexcept;
[[nodiscard]] std::uint64_t HostProgressCatchups() noexcept;
[[nodiscard]] std::uint64_t HostStartHolds() noexcept;
struct NativeRequestStatus {
    bool found{}, admitted{}, cancelled{}, unknown{};
    std::uint32_t status{};
};
struct RetirementIdentity {
    transactions::Scope scope{};
    transactions::OwnerKey owner{};
    std::uint64_t command{};
    LocalActorBinding actor{};
    std::uint32_t kind{}, revision{};
    bool operator==(const RetirementIdentity&) const noexcept = default;
};
struct NavigationBinding {
    RetirementIdentity command{};
    movement::Navigation policy{};
    std::uint32_t part{};
    bool operator==(const NavigationBinding&) const noexcept = default;
};
enum class NavigationOwnership : std::uint8_t { Unowned, Waiting, Ready, Rejected, Admitting };
// Local teardown permission, independent of the now-closed network binding.
// Ready must prove qualified engine thread/world, exact owner and actor life.
using RetirementGuard = Readiness(*)(void*, const RetirementIdentity&) noexcept;
struct RetirementProgress {
    std::size_t remaining{}, visited{}, released{}, waiting{}, unknown{};
    bool started{}, complete{}, reentrant{};
};

// Engine-thread adapter for native Move bundles, Turn and Stance contracts.
// It is not installed automatically, does not capture/promote observations,
// and never changes a Character position. Caller owns actor lifetime and must
// provide exact session/role/thread/world/ownership checks through Permission.
class NativeRequestAdapter final {
public:
    NativeRequestAdapter() noexcept = default;
    NativeRequestAdapter(const NativeRequestAdapter&) = delete;
    NativeRequestAdapter& operator=(const NativeRequestAdapter&) = delete;
    NativeRequestAdapter(NativeRequestAdapter&&) = delete;
    NativeRequestAdapter& operator=(NativeRequestAdapter&&) = delete;
    static constexpr std::size_t kCapacity = 128;
    [[nodiscard]] bool Initialize(std::uintptr_t imageBase, bool exactBuildValidated,
        Permission, ResolveActor, void* context, DependencyGate = nullptr, ResolveActor lifecycleResolver = nullptr) noexcept;
    [[nodiscard]] Readiness Check(const transactions::Record&) noexcept;
    // Value-only proof delivered AFTER the exact Character destructor returns.
    // It permits dropping our independent Ref, never Cancel on a dead owner.
    void ConfirmActorDestroyed(const LocalActorBinding&) noexcept;
    // Diagnostic only: which native dependency the last Waiting Check reported.
    // 2 dependency gate, 3 actor/ownership, 4 locomotion resources,
    // 5 Host route provider, 6 goal edit not yet accepted natively,
    // 7 ended Move awaiting native restart, 8 Host has not started this Move yet.
    [[nodiscard]] std::uint32_t LastWaitReason() const noexcept { return waitReason_; }
    void ClearWaitReason() noexcept { waitReason_ = 0; }
    // Backstage/frontstage transitions that needed the Host pose applied.
    [[nodiscard]] std::uint64_t TransitionPlacements() const noexcept { return transitionPlacements_; }
    [[nodiscard]] std::uint64_t LifecycleEvents() const noexcept { return lifecycleEvents_; }
    [[nodiscard]] transactions::NativeOutcome Invoke(const transactions::Record&) noexcept;
    [[nodiscard]] NativeRequestStatus Inspect(const transactions::Record&) noexcept;
    [[nodiscard]] std::size_t ActiveCount() const noexcept;
    [[nodiscard]] std::size_t ActiveCount(const transactions::OwnerKey&) const noexcept;
    [[nodiscard]] std::size_t ActorRequests(std::uintptr_t character, const transactions::Record* compatibleAction = nullptr) const noexcept;
    // Native seam correlation only. The embedding owner first qualifies its
    // current scope/world/thread. Unlike wire dispatch permission, the local
    // ownership resolver is valid between mailbox operations. Admitting means
    // a synchronous Submit/Update callback of this exact command, not success.
    // Unknown typed ownership must never run a local solver.
    [[nodiscard]] NavigationOwnership IdentifyNavigation(std::uintptr_t navigation,
        std::uintptr_t block, NavigationBinding&, bool currentLifetime = true) noexcept;
    // Compatibility entry point; HostRoute movement is executed by the native
    // route importer. This method no longer generates intermediate goals.
    void SteerMovements() noexcept;
    [[nodiscard]] bool BeginRetirement(const transactions::Scope&) noexcept;
    [[nodiscard]] RetirementProgress DrainRetirement(RetirementGuard, void* context,
        std::size_t budget = 4) noexcept;
    // Synchronous pre-destruction boundary. Exact actor only, full bounded
    // sweep rather than a next-frame budget. It does not certify world/route
    // quiescence or authorize clearing an Unknown reference.
    [[nodiscard]] RetirementProgress DrainActorRetirement(const LocalActorBinding&,
        RetirementGuard, void* context) noexcept;
    // There is no destructive Reset: native Release is required per command.
    // After a lost lifetime, external teardown must destroy this adapter only
    // after the owning engine resources are proven quiescent. Unknown is fenced.
private:
    friend struct NativeRequestAdapterTestAccess;
    friend class ExecutionSession;
    struct Ref { std::uintptr_t object{}, control{}; };
    struct Lease {
        transactions::Scope scope{};
        transactions::OwnerKey owner{};
        std::uint64_t command{};
        LocalActorBinding actor{};
        Ref ref{};
        std::uint32_t kind{}, revision{}, admission{};
        bool used{}, admitted{}, cancelled{}, unknown{}, secondary{};
        bool refCertified{}, releaseEntered{}, actorDestroyed{};
        movement::Bundle movement{};
        Ref movementResource{};
        // Host-guided Move: last goal written natively and path progress.
        std::array<float,3> steeredGoal{};
        std::uint32_t steerRoute{};
        std::uint32_t steerParameters{};
        std::size_t steerSegment{};
        bool steerFinal{}, steerStopApplied{};
    };
    [[nodiscard]] Lease* Find(const transactions::Record&) noexcept;
    [[nodiscard]] bool Resolve(const transactions::Record&, LocalActorBinding&) noexcept;
    [[nodiscard]] bool Live(const Lease&, const LocalActorBinding&) const noexcept;
    [[nodiscard]] transactions::NativeOutcome InvokePinned(const transactions::Record&, const LocalActorBinding&) noexcept;
    [[nodiscard]] transactions::NativeOutcome InvokeLifecycle(const transactions::Record&, const LocalActorBinding&) noexcept;
    void PlaceAtTransition(const transactions::Record&, const LocalActorBinding&) noexcept;
    [[nodiscard]] Readiness CheckMovement(const transactions::Record&, const LocalActorBinding&) noexcept;
    [[nodiscard]] Readiness Resources(std::uintptr_t character) noexcept;
    [[nodiscard]] transactions::NativeOutcome InvokeMovement(const transactions::Record&, const LocalActorBinding&) noexcept;
    [[nodiscard]] bool MovementFactory(Lease&, std::uint32_t) noexcept;
    [[nodiscard]] bool MovementAdmit(Lease&, std::uint32_t admission, bool& accepted) noexcept;
    [[nodiscard]] bool MovementGoal(Lease&, const std::array<float,3>&, bool update) noexcept;
    [[nodiscard]] bool MovementStop(Lease&, float, std::uint8_t) noexcept;
    [[nodiscard]] bool MovementParameters(Lease&, const movement::Parameters&, bool initial) noexcept;
    [[nodiscard]] bool MovementRevalidate(const transactions::Record&, Lease&) noexcept;
    [[nodiscard]] bool RestartMovement(Lease&, const transactions::Record&, const LocalActorBinding&) noexcept;
    void MovementUnknown(const transactions::Record&) noexcept;
    [[nodiscard]] bool CheckImage() const noexcept;
    [[nodiscard]] bool EndRetirement(const transactions::Scope&) noexcept;
    [[nodiscard]] RetirementProgress RetirementState() const noexcept;
    [[nodiscard]] bool RetirementLive(const Lease&) const noexcept;
    [[nodiscard]] bool CancelRetiring(std::uintptr_t request) noexcept;
    [[nodiscard]] bool ReleaseRetiring(Ref&) noexcept;
    [[nodiscard]] bool ReleaseLeaseRef(Lease&) noexcept;
    void DrainDestroyedLease(Lease&) noexcept;
    void DrainLease(Lease&, RetirementGuard, void* context) noexcept;
    std::array<Lease, kCapacity> leases_{};
    std::uintptr_t base_{};
    Permission permission_{};
    DependencyGate dependencyGate_{};
    ResolveActor resolve_{};
    ResolveActor lifecycleResolver_{};
    void* context_{};
    bool busy_{};
    std::uint32_t waitReason_{};
    std::uint64_t transitionPlacements_{}, lifecycleEvents_{};
    const transactions::Record* invokingRecord_{}; // Borrowed synchronous frame only.
    bool retiring_{};
    transactions::Scope retirementScope_{};
    std::size_t retirementCursor_{};
    // Private test seam; production uses the validated native ABI below.
    void* retirementCallsContext_{};
    bool (*retirementCancel_)(void*, std::uintptr_t) noexcept{};
    bool (*retirementRelease_)(void*, std::uintptr_t) noexcept{};
    void* movementCallsContext_{};
    bool (*movementFactory_)(void*,std::uint32_t,std::uintptr_t,std::uintptr_t*) noexcept{};
    bool (*movementAdmission_)(void*,std::uintptr_t,std::uint32_t,bool&) noexcept{};
    bool (*movementGoal_)(void*,std::uintptr_t,const float*,bool) noexcept{};
    bool (*movementStop_)(void*,std::uintptr_t,float,std::uint8_t) noexcept{};
};
// Host Updates accepted as no-op because the local Move had already ended.
[[nodiscard]] std::uint64_t UpdatesAfterLocalEnd() noexcept;
[[nodiscard]] std::uint64_t HostPathSteers() noexcept;
} // namespace aimp::ai_sync::execution
