#include "aimp/AISyncExecutionSession.h"
#include <chrono>
#include <atomic>
#include <algorithm>

namespace aimp::ai_sync::execution {
using namespace transactions;
namespace {
std::atomic<WaitTrace> gWaitTrace{};
struct BusyScope {
    bool& flag;
    explicit BusyScope(bool& value) noexcept : flag(value) { flag = true; }
    ~BusyScope() { flag = false; }
};
std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
void SetWaitTrace(WaitTrace trace) noexcept {gWaitTrace.store(trace,std::memory_order_release);}
bool ExecutionSession::GuidanceCurrent(void* context,const RetirementIdentity& identity) noexcept {
    auto& session=*static_cast<ExecutionSession*>(context);
    if(!session.configured_||!session.initialized_||session.retired_||identity.scope!=session.scope_)return false;
    Record record{};record.scope=identity.scope;record.owner=identity.owner;record.commandId=identity.command;
    CommandState command{};
    return session.executor_.Find(record,command)&&command.phase==Phase::Submitted&&command.nativeAdmitted&&
        !command.nativeReleased&&!command.superseded&&command.nativeKind==1&&command.parameterRevision==identity.revision;
}
void ExecutionSession::Steer() noexcept {
    if(busy_||!configured_||!initialized_||retired_)return;
    const BusyScope callbackGuard(busy_);
    native_.SteerMovements(&GuidanceCurrent,this);
}
void ExecutionSession::NoteWait(const Record& record, const ExecutionDisposition& disposition) noexcept {
    const auto now = NowMs();
    // Orders superseded or checkpointed elsewhere leave their probe here.
    for (auto& probe : waits_) {
        if (probe.seq == 0 || probe.seq == record.globalSeq) continue;
        ExecutionDisposition held{}; Record operation{};
        if (!executor_.ReadOperation(probe.seq, operation, held) || held.status != ExecutionStatus::Waiting) probe = {};
    }
    WaitProbe* slot{};
    for (auto& probe : waits_) if (probe.seq == record.globalSeq && probe.seq != 0) { slot = &probe; break; }
    if (disposition.status == ExecutionStatus::Waiting) {
        if (slot == nullptr) for (auto& probe : waits_) if (probe.seq == 0) { slot = &probe; break; }
        if (slot == nullptr) return;
        if (slot->seq != record.globalSeq) *slot = {record.globalSeq, now};
        slot->kind = record.nativeKind; slot->opcode = static_cast<std::uint32_t>(record.opcode);
        slot->admission = record.admission;
        slot->reason = native_.LastWaitReason() != 0 ? native_.LastWaitReason() : 1U;
        const auto age=now-slot->start;
        const auto bit=1U<<std::min(slot->reason,31U);
        if(age>=100U&&(slot->tracedReasons&bit)==0U) {
            slot->tracedReasons|=bit;
            if(const auto trace=gWaitTrace.load(std::memory_order_acquire))trace(record,age,slot->reason,disposition);
        }
        return;
    }
    if (slot == nullptr) return;
    const auto age = now - slot->start;
    if(age>=100U)if(const auto trace=gWaitTrace.load(std::memory_order_acquire))trace(record,age,slot->reason,disposition);
    if (age >= worst_.maxAgeMs) {
        worst_.maxAgeMs = age; worst_.maxSeq = slot->seq; worst_.maxKind = slot->kind;
        worst_.maxOpcode = slot->opcode; worst_.maxReason = slot->reason;
    }
    *slot = {};
}
WaitReport ExecutionSession::Waits() const noexcept {
    WaitReport report = worst_;
    report.transitionPlacements = native_.TransitionPlacements();
    report.lifecycleEvents = native_.LifecycleEvents();
    const auto now = NowMs();
    for (const auto& probe : waits_) {
        if (probe.seq == 0) continue;
        ExecutionDisposition disposition{}; Record operation{};
        // Superseded or checkpointed orders are no longer held.
        if (!executor_.ReadOperation(probe.seq, operation, disposition) ||
            disposition.status != ExecutionStatus::Waiting) continue;
        ++report.waiting;
        if (report.oldestSeq == 0 || probe.seq < report.oldestSeq) {
            report.oldestSeq = probe.seq; report.oldestAgeMs = now - probe.start; report.oldestKind = probe.kind;
            report.oldestOpcode = probe.opcode; report.oldestAdmission = probe.admission; report.oldestReason = probe.reason;
        }
    }
    if (report.oldestAgeMs > report.maxAgeMs) {
        report.maxAgeMs = report.oldestAgeMs; report.maxSeq = report.oldestSeq; report.maxKind = report.oldestKind;
        report.maxOpcode = report.oldestOpcode; report.maxReason = report.oldestReason;
    }
    return report;
}
ExecutionSession::ExecutionSession(Limits limits) noexcept
    : executor_(TransactionAdapter{this, &Check, &Invoke}, limits,true) {}
bool ExecutionSession::Initialize(std::uintptr_t base, bool exact, Permission permission,
    ResolveActor resolver, void* context, DependencyGate dependencyGate, ResolveActor lifecycleResolver) noexcept {
    if (busy_ || initialized_ || configured_ || !native_.Initialize(base, exact, permission, resolver, context,dependencyGate,lifecycleResolver)) return false;
    permission_ = permission; context_ = context; initialized_ = true; return true;
}
Result ExecutionSession::Configure(const Scope& scope, ExecutionBarrier barrier) noexcept {
    if (busy_ || !initialized_ || native_.ActiveCount() != 0) return Result::OwnerBusy;
    if (barrier != ExecutionBarrier::RequireReleased && barrier != ExecutionBarrier::ExternalQuiescence) return Result::InvalidRecord;
    if (retired_ && scope == scope_) return Result::WrongScope;
    if (retired_ && !retirementComplete_) return Result::OwnerBusy;
    if (barrier == ExecutionBarrier::ExternalQuiescence && !RetirementComplete()) return Result::OwnerBusy;
    const auto result = executor_.Configure(scope,barrier);
    if (result == Result::Ok || result == Result::Duplicate) {
        if (retired_ && !native_.EndRetirement(scope)) return Result::OutcomeUnknown;
        if(scope_!=scope)acceptedThrough_=0;
        scope_ = scope; configured_ = true; retired_ = retirementComplete_ = false;
    }
    return result;
}
Result ExecutionSession::FenceOwner(const OwnerKey& owner) noexcept {
    if (busy_ || !configured_ || retired_) return Result::OwnerBusy;
    return executor_.FenceOwner(owner);
}
Result ExecutionSession::Checkpoint(const Scope& scope,std::uint64_t through) noexcept {
    if(busy_||!configured_||retired_)return Result::OwnerBusy;
    const BusyScope guard(busy_);return executor_.Checkpoint(scope,through);
}
Dependency ExecutionSession::Check(void* context, const Record& operation) {
    auto& self = *static_cast<ExecutionSession*>(context);
    switch (self.native_.Check(operation)) {
    case Readiness::Ready: return Dependency::Ready;
    case Readiness::Waiting: return Dependency::Waiting;
    default: return Dependency::Reject;
    }
}
NativeOutcome ExecutionSession::Invoke(void* context, const Record& operation) {
    return static_cast<ExecutionSession*>(context)->native_.Invoke(operation);
}
Receipt ExecutionSession::MakeReceipt(const Record& operation, std::uint64_t batchId,
    const ExecutionDisposition& disposition) noexcept {
    Stage stage = Stage::Rejected;
    if (disposition.status == ExecutionStatus::Superseded) stage = Stage::Superseded;
    else if (disposition.status == ExecutionStatus::Waiting) stage = Stage::Waiting;
    else if (disposition.status == ExecutionStatus::Metadata) stage = Stage::Received;
    else if (disposition.hasNativeOutcome) {
        if (disposition.nativeOutcome == NativeOutcome::Unknown) stage = Stage::Unknown;
        else if (disposition.nativeOutcome == NativeOutcome::Admitted)
            stage = operation.opcode == Op::Release ? Stage::Released : Stage::Admitted;
    } else if (disposition.nativeEntered) stage = Stage::Unknown;
    return {operation.scope, batchId, operation, stage, disposition.result};
}
bool ExecutionSession::Handle(std::span<const std::uint8_t> bytes, const Scope& current,
    bool host, std::span<Receipt> receipts, std::size_t& count) noexcept {
    count = 0;
    if (busy_ || retired_ || !initialized_ || !configured_ || !host || current != scope_) return false;
    Batch batch{};
    if (!DecodeBatch(bytes, current, batch) || receipts.size() < batch.count) return false;
    const BusyScope callbackGuard(busy_);
    // Current authority is checked even for metadata/duplicates; those records
    // must not acquire the permission of a subsequently reconfigured session.
    for (std::size_t i = 0; i < batch.count; ++i)
        if (!permission_(context_, batch.records[i])) return false;
    for (std::size_t i = 0; i < batch.count; ++i) {
        const auto& record = batch.records[i];
        if (current != scope_ || !permission_(context_, record)) break;
        native_.ClearWaitReason();
        const auto disposition=executor_.Execute(record);
        NoteWait(record, disposition);
        receipts[count++] = MakeReceipt(record, batch.batchId, disposition);
        if(disposition.acceptedLogical&&record.globalSeq>acceptedThrough_)acceptedThrough_=record.globalSeq;
    }
    return count == batch.count;
}
bool ExecutionSession::Retry(const Record& operation, std::uint64_t batchId, const Scope& current,
    bool host, Receipt& receipt) noexcept {
    if (busy_ || retired_ || !initialized_ || !configured_ || !host || current != scope_ || operation.scope != current ||
        batchId == 0 || !ValidateExecutable(operation)) return false;
    const BusyScope callbackGuard(busy_);
    if (!permission_(context_, operation)) return false;
    native_.ClearWaitReason();
    const auto disposition = executor_.Retry(operation);
    NoteWait(operation, disposition);
    receipt = MakeReceipt(operation, batchId, disposition);
    return true;
}
NativeRequestStatus ExecutionSession::Inspect(const Record& record) noexcept {
    if (busy_ || retired_ || !configured_ || record.scope != scope_) return {};
    const BusyScope callbackGuard(busy_);
    return native_.Inspect(record);
}
bool ExecutionSession::RejectNavigation(const NavigationBinding& binding) noexcept {
    if(busy_||!configured_||retired_||binding.command.scope!=scope_)return false;
    const BusyScope callbackGuard(busy_);
    return native_.RejectNavigation(binding);
}
bool ExecutionSession::IsWaiting(const Record& record) const noexcept {
    ExecutionDisposition disposition{};
    return !busy_ && configured_ && !retired_ && record.scope == scope_ &&
        executor_.GetDisposition(record,disposition) && disposition.acceptedLogical &&
        !disposition.nativeEntered && disposition.status == ExecutionStatus::Waiting;
}
bool ExecutionSession::BeginRetirement() noexcept {
    if (busy_ || !initialized_ || !configured_) return false;
    if (retired_) return true;
    if (!native_.BeginRetirement(scope_)) return false;
    retired_ = true; retirementComplete_ = native_.ActiveCount() == 0;
    return true;
}
action_correlation::Readiness ExecutionSession::ActionReadiness(const action_correlation::Reference& reference,
    const OwnerKey& actor,bool admittedHistory) noexcept {
    using Ready=action_correlation::Readiness;
    if(busy_)return Ready::Waiting;
    if(!configured_||!initialized_||retired_||executor_.CoverageLost()||action_correlation::Empty(reference)||
        !action_correlation::Valid(reference,actor))return Ready::Rejected;
    if(reference.source.session!=scope_.session||reference.source.authorityEpoch!=scope_.authorityEpoch||
        reference.source.worldEpoch!=scope_.worldEpoch)return Ready::Rejected;
    Record operation{};ExecutionDisposition disposition{};
    if(!executor_.ReadOperation(reference.globalSequence,operation,disposition))
        return reference.globalSequence<=executor_.HistoryFloor()?Ready::Superseded:Ready::Waiting;
    if(!action_correlation::Matches(reference,actor,operation,scope_)||!disposition.acceptedLogical||
        executor_.OwnerOutcomeUnknown(actor))return Ready::Rejected;
    CommandState command{};
    if(!executor_.Find(operation,command))return Ready::Superseded;
    // An ordered Host interaction can outlive its scheduler reservation. The
    // copied, successfully admitted operation is its causal evidence; later
    // Cancel/Release must not erase it. This does not admit the request again.
    if(admittedHistory&&disposition.status==ExecutionStatus::NativeFinished&&disposition.result==Result::Ok&&
        disposition.hasNativeOutcome&&disposition.nativeOutcome==NativeOutcome::Admitted)
        return Ready::Ready;
    if(admittedHistory&&disposition.hasNativeOutcome&&disposition.nativeOutcome!=NativeOutcome::Admitted)
        return Ready::Rejected;
    // Complete describes the Host request, not consumption of the
    // Client's already queued interaction event. The original admitted action
    // remains valid causal evidence for that event after its scheduler request
    // ended. This grants no new native request entry and never accepts an
    // unentered, rejected, cancelled or unknown action.
    if(command.phase==Phase::Completed&&command.parameterRevision==reference.parameterRevision&&
        disposition.status==ExecutionStatus::NativeFinished&&disposition.result==Result::Ok&&
        disposition.hasNativeOutcome&&disposition.nativeOutcome==NativeOutcome::Admitted)
        return Ready::Ready;
    const bool pendingHistory=admittedHistory&&operation.nativeKind==kActionKind&&
        disposition.status==ExecutionStatus::Waiting;
    if((!pendingHistory&&command.phase!=Phase::Submitted)||
        (!pendingHistory&&command.parameterRevision>reference.parameterRevision)||
        disposition.status==ExecutionStatus::Superseded)return Ready::Superseded;
    if(!pendingHistory&&command.parameterRevision!=reference.parameterRevision)return Ready::Rejected;
    if(disposition.status==ExecutionStatus::Waiting) {
        const auto causal=executor_.OperationReadiness(operation);
        if(causal==Result::Ok)return Ready::Ready; // Native kind5 may need this cycle to start first.
        if(causal==Result::OwnerBusy||causal==Result::NativeAlreadyEntered)return Ready::Waiting;
        return causal==Result::Duplicate?Ready::Superseded:Ready::Rejected;
    }
    return disposition.status==ExecutionStatus::NativeFinished&&disposition.result==Result::Ok&&
        disposition.hasNativeOutcome&&disposition.nativeOutcome==NativeOutcome::Admitted?Ready::Ready:Ready::Rejected;
}
std::size_t ExecutionSession::ActionBlockingRequests(const action_correlation::Reference& reference,
    const OwnerKey& owner,std::uintptr_t character) const noexcept {
    Record operation{};ExecutionDisposition disposition{};
    if(!busy_&&!retired_&&configured_&&executor_.ReadOperation(reference.globalSequence,operation,disposition)&&
        disposition.acceptedLogical&&action_correlation::Matches(reference,owner,operation,scope_))
        return native_.ActorRequests(character,&operation);
    return native_.ActorRequests(character);
}
RetirementProgress ExecutionSession::DrainRetirement(RetirementGuard guard, void* context, std::size_t budget) noexcept {
    if (busy_ || !retired_) { RetirementProgress progress{}; progress.reentrant = busy_; return progress; }
    const BusyScope callbackGuard(busy_);
    const auto progress = native_.DrainRetirement(guard,context,budget);
    retirementComplete_ = progress.complete;
    return progress;
}
void ExecutionSession::ConfirmActorDestroyed(const LocalActorBinding& actor) noexcept {
    if(busy_||!initialized_||!configured_)return;
    native_.ConfirmActorDestroyed(actor);
}
RetirementProgress ExecutionSession::DrainActorRetirement(const LocalActorBinding& actor,RetirementGuard guard,void* context) noexcept {
    if(busy_||!retired_){RetirementProgress result{};result.reentrant=busy_;return result;}
    const BusyScope callbackGuard(busy_);
    const auto progress=native_.DrainActorRetirement(actor,guard,context);
    retirementComplete_=progress.complete;return progress;
}
} // namespace aimp::ai_sync::execution
