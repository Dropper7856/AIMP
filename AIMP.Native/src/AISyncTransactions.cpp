#include "aimp/AISyncTransactions.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace aimp::ai_sync::transactions {
namespace {
bool ValidScope(const Scope& scope) noexcept {
    return std::any_of(scope.session.begin(), scope.session.end(),
        [](std::uint8_t value) { return value != 0; }) &&
        scope.authorityEpoch != 0 && scope.worldEpoch != 0 && scope.consumerEpoch != 0;
}
bool ValidOwner(const OwnerKey& owner) noexcept {
    return (owner.entityA != 0 || owner.entityB != 0) && owner.birth != 0 &&
        owner.actorClass != 0 && owner.ownerEpoch != 0 && owner.capability == Capability::Movement;
}
bool SameActor(const OwnerKey& a, const OwnerKey& b) noexcept {
    return a.entityA == b.entityA && a.entityB == b.entityB && a.capability == b.capability;
}
bool Terminal(Phase phase) noexcept {
    return phase == Phase::Cancelled || phase == Phase::Stopped ||
        phase == Phase::Completed || phase == Phase::Released;
}
bool CanBypassScalarRequests(std::uint32_t kind) noexcept {
    // Only these lifecycle events and typed constructors may overtake scalar
    // scheduler requests. An unknown high kind grants no ordering exemption.
    return (kind >= 0x100U && kind <= 0x105U) || kind == 0x200U;
}
void Put32(CanonicalRecord& out, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void Put64(CanonicalRecord& out, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
std::uint32_t Get32(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(in[offset + i]) << (i * 8);
    return value;
}
std::uint64_t Get64(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
    std::uint64_t value{};
    for (std::size_t i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(in[offset + i]) << (i * 8);
    return value;
}
bool AddAllocation(std::size_t count, std::size_t size, std::size_t cap, std::size_t& total) noexcept {
    if (total > cap || count > (cap - total) / size) return false;
    total += count * size;
    return true;
}
} // namespace

Result Validate(const Record& r) noexcept {
    const auto op = static_cast<std::uint32_t>(r.opcode);
    const bool diagnostic = r.opcode == Op::Oracle || r.opcode == Op::CoverageLost;
    if (!ValidScope(r.scope) || !ValidOwner(r.owner) || op < 1 || op > 9 ||
        (r.flags & ~kKnownFlags) != 0 || r.globalSeq == 0 || r.ownerSeq == 0 ||
        (!diagnostic && (r.commandId == 0 || r.parameterRevision == 0)) ||
        (diagnostic && (r.flags & ObservationOnly) == 0) ||
        (r.commandId != 0 && r.parentCommandId == r.commandId) ||
        !std::isfinite(r.goalX) || !std::isfinite(r.goalY) || !std::isfinite(r.goalZ) ||
        !std::isfinite(r.distance) || !std::isfinite(r.stopDistance) || !std::isfinite(r.aux) ||
        r.distance < 0.0F || r.stopDistance < 0.0F || r.admission>2 || r.scalarRetention>3) return Result::InvalidRecord;
    return Result::Ok;
}

Result Encode(const Record& r, CanonicalRecord& output) noexcept {
    if (Validate(r) != Result::Ok) return Result::InvalidRecord;
    CanonicalRecord out{};
    Put32(out, 0, kMagic);
    out[4] = static_cast<std::uint8_t>(kVersion);
    out[6] = static_cast<std::uint8_t>(kRecordSize);
    std::copy(r.scope.session.begin(), r.scope.session.end(), out.begin() + 8);
    Put64(out, 24, r.scope.authorityEpoch); Put64(out, 32, r.scope.worldEpoch);
    Put64(out, 40, r.scope.consumerEpoch); Put64(out, 48, r.owner.ownerEpoch);
    Put32(out, 56, r.owner.entityA); Put32(out, 60, r.owner.entityB);
    Put32(out, 64, r.owner.birth); Put32(out, 68, r.owner.actorClass);
    Put64(out, 72, r.commandId); Put64(out, 80, r.parentCommandId);
    Put64(out, 88, r.globalSeq); Put64(out, 96, r.ownerSeq);
    Put32(out, 104, static_cast<std::uint32_t>(r.opcode));
    Put32(out, 108, static_cast<std::uint32_t>(r.owner.capability));
    Put32(out, 112, r.parameterRevision); Put32(out, 116, r.flags);
    Put32(out, 120, r.nativeKind); Put32(out, 124, r.gait);
    Put32(out, 128, r.mode); Put32(out, 132, r.nativeStatus);
    Put32(out, 136, std::bit_cast<std::uint32_t>(r.goalX));
    Put32(out, 140, std::bit_cast<std::uint32_t>(r.goalY));
    Put32(out, 144, std::bit_cast<std::uint32_t>(r.goalZ));
    Put32(out, 148, std::bit_cast<std::uint32_t>(r.distance));
    Put32(out, 152, std::bit_cast<std::uint32_t>(r.stopDistance));
    Put32(out, 156, std::bit_cast<std::uint32_t>(r.aux));
    output = out;
    return Result::Ok;
}

Result Decode(std::span<const std::uint8_t> in, Record& output) noexcept {
    if (in.size() != kRecordSize || Get32(in, 0) != kMagic ||
        in[4] != kVersion || in[5] != 0 || in[6] != kRecordSize || in[7] != 0)
        return Result::InvalidEncoding;
    Record r{};
    std::copy_n(in.begin() + 8, 16, r.scope.session.begin());
    r.scope.authorityEpoch = Get64(in, 24); r.scope.worldEpoch = Get64(in, 32);
    r.scope.consumerEpoch = Get64(in, 40); r.owner.ownerEpoch = Get64(in, 48);
    r.owner.entityA = Get32(in, 56); r.owner.entityB = Get32(in, 60);
    r.owner.birth = Get32(in, 64); r.owner.actorClass = Get32(in, 68);
    r.commandId = Get64(in, 72); r.parentCommandId = Get64(in, 80);
    r.globalSeq = Get64(in, 88); r.ownerSeq = Get64(in, 96);
    r.opcode = static_cast<Op>(Get32(in, 104));
    r.owner.capability = static_cast<Capability>(Get32(in, 108));
    r.parameterRevision = Get32(in, 112); r.flags = Get32(in, 116);
    r.nativeKind = Get32(in, 120); r.gait = Get32(in, 124);
    r.mode = Get32(in, 128); r.nativeStatus = Get32(in, 132);
    r.goalX = std::bit_cast<float>(Get32(in, 136)); r.goalY = std::bit_cast<float>(Get32(in, 140));
    r.goalZ = std::bit_cast<float>(Get32(in, 144)); r.distance = std::bit_cast<float>(Get32(in, 148));
    r.stopDistance = std::bit_cast<float>(Get32(in, 152)); r.aux = std::bit_cast<float>(Get32(in, 156));
    if (Validate(r) != Result::Ok) return Result::InvalidRecord;
    output = r;
    return Result::Ok;
}

Journal::Journal(Limits limits) noexcept {
    capacity_ = (std::min)(limits.maxRecords, limits.maxBytes / kRecordSize);
    if (capacity_ == 0) { coverageLost_ = true; return; }
    try { records_ = std::make_unique<CanonicalRecord[]>(capacity_); }
    catch (...) { capacity_ = 0; coverageLost_ = true; }
}
Result Journal::Append(const Record& record) noexcept {
    if (coverageLost_) return Result::CoverageLost;
    if (Validate(record) != Result::Ok) return Result::InvalidRecord;
    if ((record.flags & ObservationOnly) == 0) return Result::ObservationRejected;
    if (size_ == capacity_) { coverageLost_ = true; return Result::CoverageLost; }
    const auto result = Encode(record, records_[size_]);
    if (result == Result::Ok) ++size_;
    return result;
}
bool Journal::Read(std::size_t index, Record& output) const noexcept {
    return index < size_ && Decode(records_[index], output) == Result::Ok;
}
void Journal::Reset() noexcept { size_ = 0; coverageLost_ = !records_; }

struct Ledger::Operation {
    CanonicalRecord bytes{};
    native_intent::Payload intent{};
    std::uint64_t globalSeq{};
    std::size_t commandIndex{};
    NativeState native{NativeState::NotEntered};
    std::uint32_t admission{},scalarRetention{3};
    Op opcode{};
    bool dispatchable{};
    std::uint32_t nativeKind{};
};
struct Ledger::Owner { OwnerKey key{}; std::uint64_t lastSeq{}, retiredCommand{}; };

Ledger::Ledger(Limits limits,bool nativePipeline) noexcept : limits_(limits),nativePipeline_(nativePipeline) {
    std::size_t bytes{};
    if (limits.maxRecords == 0 || limits.maxCommands == 0 || limits.maxOwners == 0 ||
        !AddAllocation(limits.maxRecords, sizeof(Operation), limits.maxBytes, bytes) ||
        !AddAllocation(limits.maxCommands, sizeof(CommandState), limits.maxBytes, bytes) ||
        !AddAllocation(limits.maxOwners, sizeof(Owner), limits.maxBytes, bytes)) {
        coverageLost_ = true;
        return;
    }
    try {
        operations_ = std::make_unique<Operation[]>(limits.maxRecords);
        commands_ = std::make_unique<CommandState[]>(limits.maxCommands);
        owners_ = std::make_unique<Owner[]>(limits.maxOwners);
        allocatedBytes_ = bytes;
        storageReady_ = true;
    } catch (...) {
        operations_.reset(); commands_.reset(); owners_.reset();
        coverageLost_ = true;
    }
}
Ledger::~Ledger() = default;
Result Ledger::LoseCoverage() noexcept { coverageLost_ = true; return Result::CoverageLost; }
Result Ledger::SetScope(const Scope& scope) noexcept {
    if (!ValidScope(scope)) return Result::InvalidRecord;
    if (!storageReady_) return Result::CoverageLost;
    if (hasScope_ && scope == scope_) return coverageLost_ ? Result::CoverageLost : Result::Ok;
    if (hasScope_ && scope.session == scope_.session &&
        (scope.authorityEpoch < scope_.authorityEpoch || scope.worldEpoch < scope_.worldEpoch ||
         scope.consumerEpoch < scope_.consumerEpoch)) return Result::WrongScope;
    for (std::size_t i = 0; i < recordCount_; ++i)
        if (operations_[i].native == NativeState::Entered) return Result::NativeAlreadyEntered;
    scope_ = scope; hasScope_ = true; coverageLost_ = false;
    recordCount_ = 0; commandCount_ = 0; ownerCount_ = 0; lastGlobal_ = historyFloor_ = 0;
    return Result::Ok;
}
Result Ledger::FenceOwner(const OwnerKey& key) noexcept {
    if (coverageLost_) return Result::CoverageLost;
    if (!hasScope_ || !ValidOwner(key)) return Result::InvalidRecord;
    for (std::size_t i = 0; i < ownerCount_; ++i) {
        auto& owner = owners_[i];
        if (!SameActor(owner.key, key)) continue;
        if (owner.key == key) return Result::Ok;
        if (key.birth < owner.key.birth || key.ownerEpoch <= owner.key.ownerEpoch ||
            (key.birth == owner.key.birth && key.actorClass != owner.key.actorClass)) return Result::StaleOwner;
        for (std::size_t j = 0; j < recordCount_; ++j)
            if (operations_[j].native == NativeState::Entered) return Result::NativeAlreadyEntered;
        owner = Owner{key, 0};
        return Result::Ok;
    }
    for (std::size_t i = 0; i < recordCount_; ++i)
        if (operations_[i].native == NativeState::Entered) return Result::NativeAlreadyEntered;
    if (ownerCount_ == limits_.maxOwners) return LoseCoverage();
    owners_[ownerCount_++] = Owner{key, 0};
    return Result::Ok;
}

Result Ledger::Apply(const Record& r) noexcept {
    if (Validate(r) != Result::Ok || !native_intent::Validate(r.intent)) return Result::InvalidRecord;
    if ((r.flags & (ObservationOnly | Incomplete)) != 0) return Result::ObservationRejected;
    if (!hasScope_ || r.scope != scope_) return Result::WrongScope;
    if (coverageLost_) return Result::CoverageLost;
    std::size_t ownerIndex = ownerCount_;
    for (std::size_t i = 0; i < ownerCount_; ++i) {
        if (SameActor(owners_[i].key, r.owner)) {
            if (owners_[i].key != r.owner) return Result::StaleOwner;
            ownerIndex = i; break;
        }
    }
    CanonicalRecord bytes{};
    if (Encode(r, bytes) != Result::Ok) return Result::InvalidRecord;
    for (std::size_t i = 0; i < recordCount_; ++i)
        if (operations_[i].globalSeq == r.globalSeq)
            return operations_[i].bytes == bytes && operations_[i].admission==r.admission &&
                operations_[i].scalarRetention==r.scalarRetention && operations_[i].intent==r.intent ?
                Result::Duplicate : Result::Conflict;
    if (lastGlobal_ == (std::numeric_limits<std::uint64_t>::max)()) return LoseCoverage();
    if (r.globalSeq != lastGlobal_ + 1) return Result::GlobalGap;
    const auto lastOwner = ownerIndex == ownerCount_ ? 0 : owners_[ownerIndex].lastSeq;
    if (lastOwner == (std::numeric_limits<std::uint64_t>::max)()) return LoseCoverage();
    if (r.ownerSeq != lastOwner + 1) return Result::OwnerGap;
    std::size_t commandIndex = commandCount_;
    for (std::size_t i = 0; i < commandCount_; ++i)
        if (commands_[i].owner == r.owner && commands_[i].commandId == r.commandId) {
            commandIndex = i; break;
        }
    CommandState next{};
    if (r.opcode == Op::Begin) {
        if (commandIndex != commandCount_) return Result::InvalidTransition;
        if (ownerIndex != ownerCount_ && r.commandId <= owners_[ownerIndex].retiredCommand)
            return Result::InvalidTransition;
        if (r.parameterRevision != 1) return Result::RevisionMismatch;
        bool parentFound = r.parentCommandId == 0;
        for (std::size_t i = 0; i < commandCount_; ++i) {
            const auto& command = commands_[i];
            if (command.owner != r.owner) continue;
            if (!nativePipeline_ && !Terminal(command.phase)) return Result::OwnerBusy;
            if (command.commandId == r.parentCommandId) parentFound = true;
        }
        if (!parentFound) return Result::MissingBegin;
        next = CommandState{Phase::Begun, r.parameterRevision, r.commandId, r.parentCommandId, r.owner};
    } else {
        if (commandIndex == commandCount_) return Result::MissingBegin;
        next = commands_[commandIndex];
        if (next.parentCommandId != r.parentCommandId) return Result::Conflict;
        if (r.opcode == Op::Update) {
            if (Terminal(next.phase)) return Result::InvalidTransition;
            if (next.parameterRevision == (std::numeric_limits<std::uint32_t>::max)() ||
                r.parameterRevision != next.parameterRevision + 1) return Result::RevisionMismatch;
            next.parameterRevision = r.parameterRevision;
        } else {
            if (r.parameterRevision != next.parameterRevision) return Result::RevisionMismatch;
            switch (r.opcode) {
            case Op::Submit:
                if (next.phase != Phase::Begun) return Result::InvalidTransition;
                next.phase = Phase::Submitted; next.nativeKind = r.nativeKind; break;
            case Op::Cancel: case Op::Stop:
                if (Terminal(next.phase)) return Result::InvalidTransition;
                next.phase = r.opcode == Op::Cancel ? Phase::Cancelled : Phase::Stopped; break;
            case Op::Complete:
                if (next.phase != Phase::Submitted) return Result::InvalidTransition;
                next.phase = Phase::Completed; break;
            case Op::Release:
                if (!Terminal(next.phase) || next.phase == Phase::Released) return Result::InvalidTransition;
                next.phase = Phase::Released; break;
            default: return Result::InvalidTransition;
            }
        }
    }
    if (recordCount_ == limits_.maxRecords ||
        (commandIndex == commandCount_ && commandCount_ == limits_.maxCommands) ||
        (ownerIndex == ownerCount_ && ownerCount_ == limits_.maxOwners)) return LoseCoverage();
    if (ownerIndex == ownerCount_) owners_[ownerCount_++] = Owner{r.owner, 0};
    if (commandIndex == commandCount_) ++commandCount_;
    commands_[commandIndex] = next;
    owners_[ownerIndex].lastSeq = r.ownerSeq;
    const bool dispatchable=r.opcode==Op::Submit||r.opcode==Op::Cancel||r.opcode==Op::Stop||r.opcode==Op::Release||
        (r.opcode==Op::Update&&next.phase==Phase::Submitted);
    // Kind5 passage reservations do not construct a cycle. If the Host
    // explicitly cancels one before LOCAL native entry, never replay that old
    // reservation merely to cancel it again. Direct Host constructors remain
    // ordered evidence; pairs/attacks and entered/Unknown requests stay intact.
    if(nativePipeline_&&r.nativeKind==5U&&(r.mode==75U||r.mode==76U||r.mode==77U)&&
        (r.opcode==Op::Cancel||r.opcode==Op::Stop)) {
        for(std::size_t i=0;i<recordCount_;++i) {
            auto& earlier=operations_[i];Record pending{};
            if(earlier.commandIndex==commandIndex&&earlier.nativeKind==5U&&earlier.opcode==Op::Submit&&
                earlier.native==NativeState::NotEntered&&Decode(earlier.bytes,pending)==Result::Ok&&pending.mode==r.mode)
                earlier.native=NativeState::Superseded;
        }
    }
    if(nativePipeline_&&!PreserveNativeOperations(r.nativeKind)&&(r.opcode==Op::Cancel||r.opcode==Op::Stop||r.opcode==Op::Complete)) {
        for(std::size_t i=0;i<recordCount_;++i)if(operations_[i].commandIndex==commandIndex&&operations_[i].dispatchable&&
            !PreserveNativeOperations(operations_[i].nativeKind)&&
            operations_[i].native==NativeState::NotEntered&&(operations_[i].opcode==Op::Submit||operations_[i].opcode==Op::Update))
            operations_[i].native=NativeState::Superseded;
    }
    auto native=NativeState::NotEntered;
    if(nativePipeline_) {
        // The Client mirrors the Host's CURRENT intent, never its history.
        // An Update carries the full parameter set: older unapplied Updates of
        // the same command are obsolete.
        if(r.opcode==Op::Update&&!PreserveNativeOperations(r.nativeKind))
            for(std::size_t i=0;i<recordCount_;++i)if(operations_[i].commandIndex==commandIndex&&
                !PreserveNativeOperations(operations_[i].nativeKind)&&operations_[i].opcode==Op::Update&&operations_[i].native==NativeState::NotEntered)
                operations_[i].native=NativeState::Superseded;
        // A replacing Submit (admission 0, 00683700 replace=1) natively cancels
        // every queued request of this Character: unentered older orders of
        // this owner would only be cancelled by it, so they never run. Attack
        // actions (kind5) are kept: an interaction may depend on them.
        if(r.opcode==Op::Submit&&r.admission==0)
            for(std::size_t i=0;i<recordCount_;++i) {
                auto& earlier=operations_[i];
                if(earlier.commandIndex==commandIndex||commands_[earlier.commandIndex].owner!=r.owner||
                    earlier.native!=NativeState::NotEntered||!earlier.dispatchable||earlier.nativeKind==5U||PreserveNativeOperations(earlier.nativeKind)||
                    (earlier.opcode!=Op::Submit&&earlier.opcode!=Op::Update))continue;
                earlier.native=NativeState::Superseded;
                if(earlier.opcode==Op::Submit)commands_[earlier.commandIndex].superseded=true;
            }
        // Later parameter edits of a superseded command are obsolete too,
        // including an entered request fenced by an accepted replacement.
        if(r.opcode==Op::Update&&!PreserveNativeOperations(r.nativeKind)&&commands_[commandIndex].superseded)native=NativeState::Superseded;
    }
    operations_[recordCount_++] = Operation{bytes, r.intent, r.globalSeq, commandIndex, native,
        r.admission,r.scalarRetention,r.opcode,dispatchable,r.nativeKind};
    lastGlobal_ = r.globalSeq;
    return Result::Ok;
}

Ledger::Operation* Ledger::FindOperation(const Record& record) noexcept {
    if (!hasScope_ || record.scope != scope_) return nullptr;
    CanonicalRecord bytes{};
    if (Encode(record, bytes) != Result::Ok) return nullptr;
    for (std::size_t i = 0; i < recordCount_; ++i)
        if (operations_[i].globalSeq == record.globalSeq && operations_[i].bytes == bytes &&
            operations_[i].admission==record.admission&&operations_[i].scalarRetention==record.scalarRetention&&
            operations_[i].intent==record.intent) return &operations_[i];
    return nullptr;
}
Result Ledger::NativeReadiness(const Record& r) noexcept {
    if(Validate(r)!=Result::Ok)return Result::InvalidRecord;
    if(!hasScope_||r.scope!=scope_)return Result::WrongScope;
    if(coverageLost_)return Result::CoverageLost;
    auto* operation=FindOperation(r);if(!operation)return Result::Conflict;
    if(operation->native==NativeState::Superseded)return Result::Duplicate;
    if(operation->native==NativeState::Unknown)return Result::OutcomeUnknown;
    if(operation->native!=NativeState::NotEntered)return Result::NativeAlreadyEntered;
    if(!operation->dispatchable)return Result::InvalidTransition;
    bool owner=false;for(std::size_t i=0;i<ownerCount_;++i)owner=owner||owners_[i].key==r.owner;
    if(!owner)return Result::StaleOwner;
    if(!nativePipeline_)return Result::Ok;
    for(std::size_t i=0;i<recordCount_;++i) {
        const auto& earlier=operations_[i];
        if(earlier.native==NativeState::Entered)return Result::NativeAlreadyEntered;
        if(commands_[earlier.commandIndex].owner!=r.owner)continue;
        if(earlier.native==NativeState::Unknown)return Result::OutcomeUnknown;
        if(earlier.globalSeq>=r.globalSeq||!earlier.dispatchable)continue;
        const bool same=earlier.commandIndex==operation->commandIndex;
        const bool preempt=r.opcode==Op::Cancel||r.opcode==Op::Stop;
        // Lifecycle events and typed constructors are irreversible invocation
        // evidence, even after logical Complete/Cancel. No later native work
        // may skip them; FinishNative settles invocation, not animation.
        // Unknown extension kinds stay conservatively ordered as well; this
        // retention rule does not grant them the lifecycle bypass privilege.
        if((earlier.nativeKind!=5U||same)&&PreserveNativeOperations(earlier.nativeKind)&&
            earlier.native==NativeState::NotEntered)return Result::OwnerBusy;
        // These direct native calls may bypass only another command's scalar
        // requests, as lifecycle already did. In particular retain kind5 holds
        // without waiting for them: a hold can require this constructor's
        // resource, so ordering the constructor behind it would be cyclic.
        // Other scalar replacement/coalescing rules remain unchanged.
        if(CanBypassScalarRequests(r.nativeKind)&&!same&&
            (earlier.nativeKind==5U||!PreserveNativeOperations(earlier.nativeKind)))continue;
        if((r.opcode==Op::Submit||(same&&!preempt))&&earlier.native==NativeState::NotEntered)return Result::OwnerBusy;
    }
    if(r.opcode==Op::Update&&!commands_[operation->commandIndex].nativeAdmitted)return Result::InvalidTransition;
    return Result::Ok;
}
Result Ledger::EnterNative(const Record& r) noexcept {
    if (Validate(r) != Result::Ok) return Result::InvalidRecord;
    if ((r.flags & (ObservationOnly | Incomplete)) != 0) return Result::ObservationRejected;
    if (!hasScope_ || r.scope != scope_) return Result::WrongScope;
    if (coverageLost_) return Result::CoverageLost;
    auto* operation = FindOperation(r);
    if (!operation) return Result::Conflict;
    if (operation->native == NativeState::Unknown) return Result::OutcomeUnknown;
    if (operation->native != NativeState::NotEntered) return Result::NativeAlreadyEntered;
    bool currentOwner = false;
    for (std::size_t i = 0; i < ownerCount_; ++i) currentOwner = currentOwner || owners_[i].key == r.owner;
    if (!currentOwner) return Result::StaleOwner;
    const auto& command = commands_[operation->commandIndex];
    if(nativePipeline_) {
        const auto readiness=NativeReadiness(r);if(readiness!=Result::Ok)return readiness;
    } else if (r.opcode == Op::Submit || r.opcode == Op::Update) {
        if (command.phase != Phase::Submitted || command.parameterRevision != r.parameterRevision)
            return Result::InvalidTransition;
    } else if ((r.opcode == Op::Cancel && command.phase == Phase::Cancelled) ||
               (r.opcode == Op::Stop && command.phase == Phase::Stopped) ||
               (r.opcode == Op::Release && command.phase == Phase::Released)) {
        // Explicit terminal operations may enter only their own native cleanup.
    } else return Result::InvalidTransition;
    for (std::size_t i = 0; i < recordCount_; ++i) {
        if (operations_[i].native == NativeState::Entered) return Result::NativeAlreadyEntered;
        if (commands_[operations_[i].commandIndex].owner == r.owner &&
            operations_[i].native == NativeState::Unknown) return Result::OutcomeUnknown;
    }
    if (r.opcode == Op::Update && !command.nativeAdmitted) return Result::InvalidTransition;
    operation->native = NativeState::Entered;
    return Result::Ok;
}
Result Ledger::FinishNative(const Record& r, NativeOutcome outcome) noexcept {
    if (outcome != NativeOutcome::Admitted && outcome != NativeOutcome::Rejected && outcome != NativeOutcome::Unknown)
        return Result::InvalidRecord;
    if (!hasScope_ || r.scope != scope_) return Result::WrongScope;
    auto* operation = FindOperation(r);
    if (!operation) return Result::Conflict;
    bool currentOwner = false;
    for (std::size_t i = 0; i < ownerCount_; ++i) currentOwner = currentOwner || owners_[i].key == r.owner;
    if (!currentOwner) return Result::StaleOwner;
    if (operation->native == NativeState::Unknown) return Result::OutcomeUnknown;
    if (operation->native != NativeState::Entered) return Result::InvalidTransition;
    operation->native = outcome == NativeOutcome::Admitted ? NativeState::Admitted :
        outcome == NativeOutcome::Rejected ? NativeState::Rejected : NativeState::Unknown;
    if (r.opcode == Op::Submit)
        commands_[operation->commandIndex].nativeAdmitted = outcome == NativeOutcome::Admitted;
    if (nativePipeline_ && r.opcode == Op::Submit && r.admission == 0 &&
        outcome == NativeOutcome::Admitted && !PreserveNativeOperations(r.nativeKind)) {
        // Match the adapter's native preemption fence. A late Host snapshot
        // of an older retained request is journal evidence, not a failure and
        // not permission to restore that request's navigation authority.
        for (std::size_t i = 0; i < commandCount_; ++i) {
            auto& earlier = commands_[i];
            if (i != operation->commandIndex && earlier.nativeAdmitted && !earlier.nativeReleased &&
                !PreserveNativeOperations(earlier.nativeKind) && earlier.owner == r.owner)
                earlier.superseded = true;
        }
    }
    if (r.opcode == Op::Release && outcome == NativeOutcome::Admitted)
        commands_[operation->commandIndex].nativeReleased = true;
    return outcome == NativeOutcome::Unknown ? Result::OutcomeUnknown : Result::Ok;
}
Result Ledger::Checkpoint(const Scope& scope, std::uint64_t through) noexcept {
    if (!hasScope_ || scope != scope_) return Result::WrongScope;
    if (coverageLost_) return Result::CoverageLost;
    if (!through || through < historyFloor_ || through > lastGlobal_) return Result::InvalidRecord;
    if (through == historyFloor_) return Result::Ok;
    for (std::size_t i=0;i<recordCount_;++i) {
        const auto& op=operations_[i];
        if (op.native==NativeState::Entered) return Result::NativeAlreadyEntered;
        if (op.globalSeq<=through && op.dispatchable &&
            op.native!=NativeState::Admitted && op.native!=NativeState::Superseded)
            return op.native==NativeState::Unknown ? Result::OutcomeUnknown : Result::OwnerBusy;
    }
    std::size_t retained{};
    for (std::size_t i=0;i<recordCount_;++i)
        if (operations_[i].globalSeq>through) operations_[retained++]=operations_[i];
    for (auto i=retained;i<recordCount_;++i) operations_[i]={};
    recordCount_=retained;
    for (std::size_t i=0;i<commandCount_;) {
        const auto& command=commands_[i];
        bool referenced=false;
        for (std::size_t j=0;j<recordCount_;++j) referenced=referenced||operations_[j].commandIndex==i;
        for (std::size_t j=0;j<commandCount_;++j) referenced=referenced||
            (j!=i && commands_[j].owner==command.owner && commands_[j].parentCommandId==command.commandId);
        if (command.phase!=Phase::Released || !command.nativeReleased || referenced) {++i;continue;}
        for (std::size_t j=0;j<ownerCount_;++j) if (owners_[j].key==command.owner)
            owners_[j].retiredCommand=(std::max)(owners_[j].retiredCommand,command.commandId);
        for (std::size_t j=0;j<recordCount_;++j) if (operations_[j].commandIndex>i) --operations_[j].commandIndex;
        for (std::size_t j=i+1;j<commandCount_;++j) commands_[j-1]=commands_[j];
        commands_[--commandCount_]={};
    }
    historyFloor_=through;return Result::Ok;
}
bool Ledger::Superseded(std::uint64_t globalSeq) const noexcept {
    for (std::size_t i = 0; i < recordCount_; ++i)
        if (operations_[i].globalSeq == globalSeq) return operations_[i].native == NativeState::Superseded;
    return false;
}
bool Ledger::Find(const Record& identity, CommandState& output) const noexcept {
    if (!hasScope_ || identity.scope != scope_) return false;
    // Command-only queries (including checkpoint bookkeeping) have no token.
    // When a retained sequence is supplied, its extension is evidence too.
    for (std::size_t i = 0; i < recordCount_; ++i)
        if (operations_[i].globalSeq == identity.globalSeq && operations_[i].intent != identity.intent) return false;
    for (std::size_t i = 0; i < commandCount_; ++i) {
        if (commands_[i].owner == identity.owner && commands_[i].commandId == identity.commandId) {
            output = commands_[i]; return true;
        }
    }
    return false;
}

} // namespace aimp::ai_sync::transactions
