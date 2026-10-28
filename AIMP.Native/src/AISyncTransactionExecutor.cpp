#include "aimp/AISyncTransactionExecutor.h"

#include <new>
#include <utility>

namespace aimp::ai_sync::transactions {
namespace {
// Reentry barrier, not a data lock. Ledger mutations finish before callbacks.
struct CallGuard {
    bool& busy;
    explicit CallGuard(bool& value) noexcept : busy(value) { busy = true; }
    ~CallGuard() { busy = false; }
};
ExecutionDisposition Refuse(Result result,
    ExecutionStatus status = ExecutionStatus::Refused) noexcept {
    ExecutionDisposition disposition{};
    disposition.status = status;
    disposition.result = result;
    return disposition;
}
bool SameActor(const OwnerKey& a, const OwnerKey& b) noexcept {
    return a.entityA == b.entityA && a.entityB == b.entityB && a.capability == b.capability;
}
bool ValidBarrier(ExecutionBarrier barrier) noexcept {
    return barrier == ExecutionBarrier::RequireReleased || barrier == ExecutionBarrier::ExternalQuiescence;
}
} // namespace

struct TransactionExecutor::Operation {
    CanonicalRecord bytes{};
    std::uint64_t globalSeq{};
    std::size_t commandIndex{};
    ExecutionDisposition disposition{};
    movement::Bundle movement{};
    native_intent::Payload intent{};
    std::uint32_t admission{},scalarRetention{3};
    bool requiresNative{};
};
struct TransactionExecutor::Command {
    OwnerKey owner{};
    std::uint64_t id{};
    std::size_t waiting{noIndex};
    bool nativeTouched{};
    bool released{};
    bool retired{};
    bool profileBound{};
    std::uint32_t nativeKind{};
    std::uint32_t admission{};
};

Limits TransactionExecutor::LedgerLimits(Limits limits) noexcept {
    std::size_t remaining = limits.maxBytes;
    for (const auto allocation : {std::pair{limits.maxRecords, sizeof(Operation)},
                                  std::pair{limits.maxCommands, sizeof(Command)}}) {
        if (allocation.first > remaining / allocation.second) {
            limits.maxBytes = 0;
            return limits;
        }
        remaining -= allocation.first * allocation.second;
    }
    limits.maxBytes = remaining;
    return limits;
}
TransactionExecutor::TransactionExecutor(TransactionAdapter adapter, Limits limits,bool nativePipeline) noexcept
    : ledger_(LedgerLimits(limits),nativePipeline), adapter_(adapter), limits_(limits),nativePipeline_(nativePipeline) {
    if (ledger_.CoverageLost()) return;
    try {
        operations_ = std::make_unique<Operation[]>(limits.maxRecords);
        commands_ = std::make_unique<Command[]>(limits.maxCommands);
        allocatedBytes_ = ledger_.AllocatedBytes() + limits.maxRecords * sizeof(Operation) +
            limits.maxCommands * sizeof(Command);
        storageReady_ = true;
    } catch (...) {
        operations_.reset(); commands_.reset();
        allocatedBytes_ = ledger_.AllocatedBytes();
    }
}
TransactionExecutor::~TransactionExecutor() = default;
bool TransactionExecutor::CoverageLost() const noexcept {
    return !storageReady_ || ledger_.CoverageLost();
}
bool TransactionExecutor::Unreleased(const OwnerKey* owner) const noexcept {
    for (std::size_t i = 0; i < commandCount_; ++i) {
        const auto& command = commands_[i];
        if ((!owner || SameActor(command.owner, *owner)) && !command.retired &&
            command.nativeTouched && !command.released) return true;
    }
    return false;
}
Result TransactionExecutor::Configure(const Scope& scope, ExecutionBarrier barrier) noexcept {
    if (busy_) return Result::NativeAlreadyEntered;
    CallGuard call(busy_);
    if (!ValidBarrier(barrier)) return Result::InvalidRecord;
    if (!storageReady_) return Result::CoverageLost;
    if (configured_ && scope == scope_) return ledger_.SetScope(scope);
    if (barrier == ExecutionBarrier::RequireReleased && Unreleased()) return Result::OwnerBusy;
    const auto result = ledger_.SetScope(scope);
    if (result != Result::Ok) return result;
    scope_ = scope; configured_ = true;
    recordCount_ = 0; commandCount_ = 0;
    return Result::Ok;
}
Result TransactionExecutor::FenceOwner(const OwnerKey& owner, ExecutionBarrier barrier) noexcept {
    if (busy_) return Result::NativeAlreadyEntered;
    CallGuard call(busy_);
    if (!ValidBarrier(barrier)) return Result::InvalidRecord;
    if (!storageReady_) return Result::CoverageLost;
    // A fence cannot discard this actor's pending work. Other actors do not
    // depend on it and must not be stalled by an unrelated resource load.
    const auto current = CurrentCommand(owner);
    const bool changesOwner = current != noIndex && commands_[current].owner != owner;
    if (changesOwner && HasWaiting(owner)) return Result::OwnerBusy;
    if (changesOwner && barrier == ExecutionBarrier::RequireReleased && Unreleased(&owner))
        return Result::OwnerBusy;
    const auto result = ledger_.FenceOwner(owner);
    if (result == Result::Ok && changesOwner) {
        for (std::size_t i = 0; i < commandCount_; ++i)
            if (SameActor(commands_[i].owner, owner) && commands_[i].owner != owner)
                commands_[i].retired = true;
    }
    return result;
}
std::size_t TransactionExecutor::CurrentCommand(const OwnerKey& owner) const noexcept {
    for (std::size_t i = commandCount_; i != 0; --i)
        if (!commands_[i - 1].retired && SameActor(commands_[i - 1].owner, owner)) return i - 1;
    return noIndex;
}
bool TransactionExecutor::HasWaiting(const OwnerKey& owner) const noexcept {
    if(nativePipeline_) {
        for(std::size_t i=0;i<recordCount_;++i)if(!commands_[operations_[i].commandIndex].retired&&
            SameActor(commands_[operations_[i].commandIndex].owner,owner)&&operations_[i].disposition.status==ExecutionStatus::Waiting)return true;
        return false;
    }
    const auto current = CurrentCommand(owner);
    return current != noIndex && commands_[current].waiting != noIndex;
}
std::size_t TransactionExecutor::WaitingCount() const noexcept {
    std::size_t count = 0;
    if(nativePipeline_) {
        for(std::size_t i=0;i<recordCount_;++i)if(!commands_[operations_[i].commandIndex].retired&&
            operations_[i].disposition.status==ExecutionStatus::Waiting)++count;
        return count;
    }
    for (std::size_t i = 0; i < commandCount_; ++i)
        if (!commands_[i].retired && commands_[i].waiting != noIndex) ++count;
    return count;
}
std::size_t TransactionExecutor::FindOperation(const CanonicalRecord& bytes,
    std::uint64_t globalSeq) const noexcept {
    for (std::size_t i = 0; i < recordCount_; ++i)
        if (operations_[i].globalSeq == globalSeq && operations_[i].bytes == bytes) return i;
    return noIndex;
}
ExecutionDisposition TransactionExecutor::Remembered(std::size_t index) const noexcept {
    auto result = operations_[index].disposition;
    result.duplicate = true;
    return result;
}
bool TransactionExecutor::GetDisposition(const Record& operation, ExecutionDisposition& output) const noexcept {
    if (!configured_ || operation.scope != scope_) return false;
    CanonicalRecord bytes{};
    if (Encode(operation, bytes) != Result::Ok) return false;
    const auto index = FindOperation(bytes, operation.globalSeq);
    if (index == noIndex) return false;
    if (!movement::Same(operations_[index].movement,operation.movement)||operations_[index].admission!=operation.admission||
        operations_[index].scalarRetention!=operation.scalarRetention||operations_[index].intent!=operation.intent) return false;
    output = operations_[index].disposition;
    return true;
}
bool TransactionExecutor::Find(const Record& identity, CommandState& output) const noexcept {
    return ledger_.Find(identity, output);
}
bool TransactionExecutor::OwnerOutcomeUnknown(const OwnerKey& owner) const noexcept {
    for(std::size_t i=0;i<recordCount_;++i) {
        const auto& operation=operations_[i];
        if(commands_[operation.commandIndex].owner==owner&&operation.disposition.hasNativeOutcome&&
            operation.disposition.nativeOutcome==NativeOutcome::Unknown)return true;
    }
    return false;
}
bool TransactionExecutor::ReadOperation(std::uint64_t sequence,Record& record,ExecutionDisposition& disposition) const noexcept {
    if(!configured_||busy_)return false;
    for(std::size_t i=0;i<recordCount_;++i)if(operations_[i].globalSeq==sequence) {
        Record value{};if(Decode(operations_[i].bytes,value)!=Result::Ok)return false;
        value.movement=operations_[i].movement;value.admission=operations_[i].admission;
        value.scalarRetention=operations_[i].scalarRetention;
        value.intent=operations_[i].intent;
        record=value;disposition=operations_[i].disposition;return true;
    }
    return false;
}
Result TransactionExecutor::OperationReadiness(const Record& record) noexcept {
    return busy_?Result::NativeAlreadyEntered:ledger_.NativeReadiness(record);
}
Result TransactionExecutor::Checkpoint(const Scope& scope, std::uint64_t through) noexcept {
    if (busy_) return Result::NativeAlreadyEntered;
    CallGuard call(busy_);
    if (!configured_ || scope!=scope_) return Result::WrongScope;
    for (std::size_t i=0;i<recordCount_;++i) {
        const auto& op=operations_[i];if(op.globalSeq>through)continue;
        const auto& d=op.disposition;
        if (d.status!=ExecutionStatus::Metadata && d.status!=ExecutionStatus::Superseded &&
            !(d.status==ExecutionStatus::NativeFinished && d.result==Result::Ok &&
              d.hasNativeOutcome && d.nativeOutcome==NativeOutcome::Admitted))
            return d.status==ExecutionStatus::OutcomeUnknown ? Result::OutcomeUnknown : Result::OwnerBusy;
    }
    const auto result=ledger_.Checkpoint(scope,through);if(result!=Result::Ok)return result;
    std::size_t retained{};
    for(std::size_t i=0;i<recordCount_;++i) if(operations_[i].globalSeq>through) operations_[retained++]=operations_[i];
    for(auto i=retained;i<recordCount_;++i)operations_[i]={};
    recordCount_=retained;
    for(std::size_t i=0;i<commandCount_;) {
        Record identity{};identity.scope=scope_;identity.owner=commands_[i].owner;identity.commandId=commands_[i].id;
        CommandState state{};
        if(ledger_.Find(identity,state)){commands_[i].waiting=noIndex;++i;continue;}
        for(std::size_t j=0;j<recordCount_;++j)if(operations_[j].commandIndex>i)--operations_[j].commandIndex;
        for(std::size_t j=i+1;j<commandCount_;++j)commands_[j-1]=commands_[j];
        commands_[--commandCount_]={};
    }
    for(std::size_t i=0;i<recordCount_;++i)if(operations_[i].disposition.status==ExecutionStatus::Waiting)
        commands_[operations_[i].commandIndex].waiting=i;
    return Result::Ok;
}
ExecutionDisposition TransactionExecutor::Execute(const Record& input) noexcept {
    if (busy_) return Refuse(Result::NativeAlreadyEntered, ExecutionStatus::Reentrant);
    CallGuard call(busy_);
    // Freeze caller-owned input before invoking any callback.
    const Record operation = input;
    if (!movement::Validate(operation.movement)) return Refuse(Result::InvalidRecord);
    if (Validate(operation) != Result::Ok || !native_intent::Validate(operation.intent)) return Refuse(Result::InvalidRecord);
    if ((operation.flags & (ObservationOnly | Incomplete)) != 0) return Refuse(Result::ObservationRejected);
    if (!configured_ || operation.scope != scope_) return Refuse(Result::WrongScope);
    if (operation.globalSeq<=ledger_.HistoryFloor()) return Refuse(Result::GlobalGap);
    CanonicalRecord bytes{};
    if (Encode(operation, bytes) != Result::Ok) return Refuse(Result::InvalidRecord);
    // Retained evidence remains readable after capacity closes admission.
    // Retirement mirrors successful owner fences; no ledger mutation or
    // callback is needed to report an exact duplicate.
    for (std::size_t i = 0; i < recordCount_; ++i) {
        if (operations_[i].globalSeq != operation.globalSeq) continue;
        if (operations_[i].bytes != bytes || !movement::Same(operations_[i].movement,operation.movement)||
            operations_[i].admission!=operation.admission||operations_[i].scalarRetention!=operation.scalarRetention||
            operations_[i].intent!=operation.intent) return Refuse(Result::Conflict);
        if (commands_[operations_[i].commandIndex].retired) return Refuse(Result::StaleOwner);
        return Remembered(i);
    }
    if (CoverageLost()) return Refuse(Result::CoverageLost);
    const auto latest = CurrentCommand(operation.owner);
    if (latest != noIndex && commands_[latest].owner != operation.owner) return Refuse(Result::StaleOwner);
    auto current=latest;
    if(nativePipeline_&&operation.opcode!=Op::Begin) {
        current=noIndex;
        for(std::size_t i=0;i<commandCount_;++i)if(!commands_[i].retired&&commands_[i].owner==operation.owner&&commands_[i].id==operation.commandId)
            current=i;
    }
    const auto waiting = current == noIndex ? noIndex : commands_[current].waiting;
    bool cancelsWaiting = false;
    if (!nativePipeline_ && waiting != noIndex) {
        const auto& pending = operations_[waiting];
        const auto& command = commands_[pending.commandIndex];
        Record waitingRecord{};
        if (Decode(pending.bytes, waitingRecord) != Result::Ok) return Refuse(Result::InvalidEncoding);
        cancelsWaiting = operation.opcode == Op::Cancel && operation.owner == command.owner &&
            operation.commandId == command.id &&
            (waitingRecord.opcode == Op::Submit || waitingRecord.opcode == Op::Update);
        if (!cancelsWaiting) return Refuse(Result::OwnerBusy, ExecutionStatus::Blocked);
    }
    if (operation.opcode == Op::Begin) {
        if (!nativePipeline_ && Unreleased(&operation.owner)) return Refuse(Result::OwnerBusy);
    } else if (current != noIndex && commands_[current].id != operation.commandId) {
        // Late cleanup must not resolve through a replacement incarnation.
        return Refuse(Result::InvalidTransition);
    }
    CommandState before{};
    const bool found = ledger_.Find(operation, before);
    // Once submitted, the native request type belongs to this incarnation.
    // Reject mismatches before consuming a revision or terminal transition.
    if (operation.opcode != Op::Begin && current != noIndex && commands_[current].profileBound &&
        (operation.nativeKind != commands_[current].nativeKind || operation.admission!=commands_[current].admission))
        return Refuse(Result::InvalidRecord);
    const bool native = operation.opcode == Op::Submit || operation.opcode == Op::Cancel ||
        operation.opcode == Op::Stop || operation.opcode == Op::Release ||
        (operation.opcode == Op::Update && found && before.phase == Phase::Submitted);
    if (native && (!adapter_.checkDependencies || !adapter_.invoke))
        return Refuse(Result::InvalidTransition, ExecutionStatus::AdapterUnavailable);
    // An Update after rejected/nonentered Submit cannot be applied as metadata.
    if (!nativePipeline_ && operation.opcode == Op::Update && found && before.phase == Phase::Submitted && !before.nativeAdmitted)
        return Refuse(Result::InvalidTransition);
    const auto applied = ledger_.Apply(operation);
    if (applied != Result::Ok) return Refuse(applied);
    std::size_t commandIndex = current;
    if (operation.opcode == Op::Begin) {
        commandIndex = commandCount_++;
        commands_[commandIndex] = Command{operation.owner, operation.commandId};
    }
    if (operation.opcode == Op::Submit) {
        commands_[commandIndex].profileBound = true;
        commands_[commandIndex].nativeKind = operation.nativeKind;
        commands_[commandIndex].admission = operation.admission;
    }
    // Apply is the sole admission authority; matching limits guarantee these
    // preallocated slots exist whenever Apply succeeds.
    const auto index = recordCount_++;
    ExecutionDisposition accepted{};
    accepted.status = ExecutionStatus::Metadata;
    accepted.result = Result::Ok;
    accepted.acceptedLogical = true;
    operations_[index] = Operation{bytes, operation.globalSeq, commandIndex, accepted, operation.movement,
        operation.intent,operation.admission,operation.scalarRetention,native};
    if (cancelsWaiting) {
        operations_[waiting].disposition.status = ExecutionStatus::Superseded;
        commands_[commandIndex].waiting = noIndex;
    }
    if(nativePipeline_) {
        // Mirror the ledger's current-intent superseding onto waiting orders.
        for(std::size_t i=0;i<index;++i)if(operations_[i].requiresNative&&!operations_[i].disposition.nativeEntered&&
            operations_[i].disposition.status==ExecutionStatus::Waiting&&ledger_.Superseded(operations_[i].globalSeq)) {
            operations_[i].disposition.status=ExecutionStatus::Superseded;
            operations_[i].disposition.result=Result::Ok;
            if(commands_[operations_[i].commandIndex].waiting==i)commands_[operations_[i].commandIndex].waiting=noIndex;
        }
    }
    return native ? Dispatch(index) : accepted;
}
ExecutionDisposition TransactionExecutor::Retry(const Record& input) noexcept {
    if (busy_) return Refuse(Result::NativeAlreadyEntered, ExecutionStatus::Reentrant);
    CallGuard call(busy_);
    const Record operation = input;
    if (Validate(operation) != Result::Ok || !native_intent::Validate(operation.intent)) return Refuse(Result::InvalidRecord);
    if ((operation.flags & (ObservationOnly | Incomplete)) != 0) return Refuse(Result::ObservationRejected);
    if (!configured_ || operation.scope != scope_) return Refuse(Result::WrongScope);
    CanonicalRecord bytes{};
    if (Encode(operation, bytes) != Result::Ok) return Refuse(Result::InvalidRecord);
    const auto index = FindOperation(bytes, operation.globalSeq);
    if (index == noIndex) return Refuse(Result::Conflict);
    if (!movement::Same(operations_[index].movement,operation.movement)||operations_[index].admission!=operation.admission||
        operations_[index].scalarRetention!=operation.scalarRetention||operations_[index].intent!=operation.intent) return Refuse(Result::Conflict);
    if (commands_[operations_[index].commandIndex].retired) return Refuse(Result::StaleOwner);
    // Only Waiting is retryable. Completed, rejected and Unknown tokens retain
    // their disposition and can never reenter Invoke or dependencies.
    if (nativePipeline_ ? operations_[index].disposition.status!=ExecutionStatus::Waiting :
        index != commands_[operations_[index].commandIndex].waiting) return Remembered(index);
    if (CoverageLost()) return Refuse(Result::CoverageLost);
    return Dispatch(index);
}
ExecutionDisposition TransactionExecutor::Dispatch(std::size_t index) noexcept {
    auto& entry = operations_[index];
    auto& disposition = entry.disposition;
    auto& command = commands_[entry.commandIndex];
    Record token{};
    if (Decode(entry.bytes, token) != Result::Ok) {
        disposition.status = ExecutionStatus::Refused;
        disposition.result = Result::InvalidEncoding;
        return disposition;
    }
    token.movement = entry.movement;
    token.intent = entry.intent;
    token.admission=entry.admission;token.scalarRetention=entry.scalarRetention;
    if(nativePipeline_) {
        const auto readiness=ledger_.NativeReadiness(token);
        if(readiness!=Result::Ok) {
            if(readiness==Result::OwnerBusy){disposition.status=ExecutionStatus::Waiting;return disposition;}
            if(readiness==Result::Duplicate){disposition.status=ExecutionStatus::Superseded;return disposition;}
            disposition.status=ExecutionStatus::Refused;disposition.result=readiness;return disposition;
        }
    }
    Dependency dependency{};
    try {
        const Record callbackRecord = token;
        dependency = adapter_.checkDependencies(adapter_.context, callbackRecord);
    } catch (...) {
        disposition.status = ExecutionStatus::CallbackError;
        if (command.waiting == index) command.waiting = noIndex;
        return disposition;
    }
    if (dependency == Dependency::Waiting) {
        disposition.status = ExecutionStatus::Waiting;
        command.waiting = index;
        return disposition;
    }
    if (command.waiting == index) command.waiting = noIndex;
    if (dependency != Dependency::Ready) {
        disposition.status = dependency == Dependency::Reject ?
            ExecutionStatus::DependencyRejected : ExecutionStatus::CallbackError;
        return disposition;
    }
    const auto entered = ledger_.EnterNative(token);
    if (entered != Result::Ok) {
        disposition.status = entered == Result::OutcomeUnknown ?
            ExecutionStatus::OutcomeUnknown : ExecutionStatus::Refused;
        disposition.result = entered;
        return disposition;
    }
    disposition.nativeEntered = true;
    disposition.status = ExecutionStatus::NativeEntered;
    // Publish entry and ownership BEFORE invoking foreign code. These writes
    // and EnterNative have finished; callbacks see stable state read-only.
    command.nativeTouched = true;
    invoking_ = true;
    NativeOutcome outcome = NativeOutcome::Unknown;
    try {
        const Record callbackRecord = token;
        outcome = adapter_.invoke(adapter_.context, callbackRecord);
    } catch (...) {
        outcome = NativeOutcome::Unknown;
    }
    invoking_ = false;
    if (outcome != NativeOutcome::Admitted && outcome != NativeOutcome::Rejected && outcome != NativeOutcome::Unknown)
        outcome = NativeOutcome::Unknown;
    disposition.hasNativeOutcome = true;
    disposition.nativeOutcome = outcome;
    disposition.result = ledger_.FinishNative(token, outcome);
    disposition.status = outcome == NativeOutcome::Unknown ?
        ExecutionStatus::OutcomeUnknown : ExecutionStatus::NativeFinished;
    if (token.opcode == Op::Release && outcome == NativeOutcome::Admitted && disposition.result == Result::Ok)
        command.released = true;
    return disposition;
}

} // namespace aimp::ai_sync::transactions
