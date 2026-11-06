#include "aimp/AISyncTransactions.h"

#include <bit>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>

using namespace aimp::ai_sync::transactions;

namespace {
void Check(bool condition, int line) {
    if (!condition) { std::cerr << "AISync transaction invariant failed at line " << line << '\n'; std::abort(); }
}
#define CHECK(value) Check((value), __LINE__)

Record Base() {
    Record r{};
    r.scope.session[0] = 1;
    r.scope.authorityEpoch = 1; r.scope.worldEpoch = 1; r.scope.consumerEpoch = 1;
    r.owner = OwnerKey{11, 22, 1, 2, Capability::Movement, 1};
    r.commandId = 100; r.globalSeq = 1; r.ownerSeq = 1;
    r.parameterRevision = 1;
    r.flags = HasGoal | HasGait | HasStopDistance;
    r.goalX = 1.25F; r.goalY = -2.5F; r.goalZ = 3.75F;
    r.distance = 4.5F; r.stopDistance = 0.5F; r.aux = -0.25F;
    return r;
}
Record Golden() {
    auto r = Base();
    r.scope.session = {0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    r.scope.authorityEpoch = 0x0102030405060708ULL;
    r.scope.worldEpoch = 0x1112131415161718ULL;
    r.scope.consumerEpoch = 0x2122232425262728ULL;
    r.owner.ownerEpoch = 0x3132333435363738ULL;
    r.owner.entityA = 0x41424344; r.owner.entityB = 0x51525354;
    r.owner.birth = 0x61626364;
    r.commandId = 0x7172737475767778ULL;
    r.globalSeq = 0x8182838485868788ULL;
    r.ownerSeq = 0x9192939495969798ULL;
    r.nativeKind = 1; r.gait = 2;
    return r;
}
CanonicalRecord Bytes(const Record& record) {
    CanonicalRecord out{};
    CHECK(Encode(record, out) == Result::Ok);
    return out;
}
void Codec(const char* fixturePath) {
    std::ifstream fixture(fixturePath);
    CHECK(fixture.good());
    std::string text;
    std::string line;
    while (fixture >> line) text += line;
    CHECK(text.size() == kRecordSize * 2);
    CanonicalRecord expected{};
    const auto nibble = [](char c) -> unsigned int {
        if (c >= '0' && c <= '9') return static_cast<unsigned int>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned int>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned int>(c - 'A' + 10);
        CHECK(false); return 0;
    };
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<std::uint8_t>((nibble(text[2 * i]) << 4) | nibble(text[2 * i + 1]));
    CHECK(Bytes(Golden()) == expected);
    auto decoded = Base();
    CHECK(Decode(expected, decoded) == Result::Ok);
    CHECK(decoded.scope == Golden().scope && decoded.owner == Golden().owner);
    CHECK(decoded.commandId == Golden().commandId && decoded.parentCommandId == 0);
    CHECK(decoded.globalSeq == Golden().globalSeq && decoded.ownerSeq == Golden().ownerSeq);
    CHECK(decoded.opcode == Op::Begin && decoded.parameterRevision == 1 && decoded.flags == 7);
    CHECK(decoded.nativeKind == 1 && decoded.gait == 2 && decoded.mode == 0 && decoded.nativeStatus == 0);
    CHECK(decoded.goalX == 1.25F && decoded.goalY == -2.5F && decoded.goalZ == 3.75F);
    CHECK(decoded.distance == 4.5F && decoded.stopDistance == 0.5F && decoded.aux == -0.25F);

    // Every short encoding, oversized encoding, and envelope field corruption
    // must fail without publishing a partly decoded record.
    for (std::size_t size = 0; size < kRecordSize; ++size) {
        auto output = Base();
        CHECK(Decode(std::span(expected).first(size), output) == Result::InvalidEncoding);
        CHECK(Bytes(output) == Bytes(Base()));
    }
    std::array<std::uint8_t, kRecordSize + 1> oversized{};
    CHECK(Decode(oversized, decoded) == Result::InvalidEncoding);
    for (const std::size_t offset : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) {
        auto bad = expected; bad[offset] ^= 0x01;
        CHECK(Decode(bad, decoded) == Result::InvalidEncoding);
    }
    for (const std::size_t offset : {104U, 108U, 119U}) {
        auto bad = expected; bad[offset] = 0xFF;
        CHECK(Decode(bad, decoded) == Result::InvalidRecord);
    }
    for (const std::size_t offset : {136U, 140U, 144U, 148U, 152U, 156U}) {
        auto bad = expected;
        bad[offset] = 0; bad[offset + 1] = 0; bad[offset + 2] = 0x80; bad[offset + 3] = 0x7F;
        CHECK(Decode(bad, decoded) == Result::InvalidRecord);
    }
    auto zeros = Base();
    zeros.goalX = -0.0F; zeros.aux = -0.0F;
    const auto zeroBytes = Bytes(zeros);
    CHECK(Decode(zeroBytes, decoded) == Result::Ok && Bytes(decoded) == zeroBytes);
    auto invalid = Base();
    invalid.parentCommandId = invalid.commandId;
    CHECK(Validate(invalid) == Result::InvalidRecord);
    invalid = Base(); invalid.flags = 64; CHECK(Validate(invalid) == Result::InvalidRecord);
    invalid = Base(); invalid.distance = -1; CHECK(Validate(invalid) == Result::InvalidRecord);
    invalid = Base(); invalid.stopDistance = -1; CHECK(Validate(invalid) == Result::InvalidRecord);
    invalid = Base(); invalid.scope.session = {}; CHECK(Validate(invalid) == Result::InvalidRecord);
    invalid = Base(); invalid.scope.consumerEpoch = 0; CHECK(Validate(invalid) == Result::InvalidRecord);
    invalid = Base(); invalid.owner.birth = 0; CHECK(Validate(invalid) == Result::InvalidRecord);
    invalid = Base(); invalid.owner.entityA = 0; invalid.owner.entityB = 0;
    CHECK(Validate(invalid) == Result::InvalidRecord);
    invalid = Base(); invalid.commandId = 0; CHECK(Validate(invalid) == Result::InvalidRecord);
    for (const auto opcode : {Op::Oracle, Op::CoverageLost}) {
        invalid.opcode = opcode; invalid.parameterRevision = 0; invalid.flags = ObservationOnly;
        CHECK(Validate(invalid) == Result::Ok);
        invalid.flags = 0; CHECK(Validate(invalid) == Result::InvalidRecord);
    }
    auto diagnostic = Base();
    diagnostic.nativeKind = 0xFFFFFFFF; diagnostic.gait = 0xFFFFFFFF;
    diagnostic.mode = 0xFFFFFFFF; diagnostic.nativeStatus = 0xFFFFFFFF;
    CHECK(Validate(diagnostic) == Result::Ok);
}

void BoundsAndJournal() {
    Limits limits{2, 2, 2, 2 * kRecordSize};
    Journal journal(limits);
    auto r = Base();
    CHECK(journal.Append(r) == Result::ObservationRejected);
    r.flags |= ObservationOnly;
    CHECK(journal.Append(r) == Result::Ok);
    r.opcode = Op::Cancel; ++r.globalSeq; ++r.ownerSeq;
    CHECK(journal.Append(r) == Result::Ok);
    CHECK(journal.Size() == 2 && journal.Bytes() == 320 && !journal.CoverageLost());
    Record read{};
    CHECK(journal.Read(0, read) && read.opcode == Op::Begin);
    CHECK(journal.Read(1, read) && read.opcode == Op::Cancel);
    CHECK(!journal.Read(2, read));
    CHECK(journal.Append(r) == Result::CoverageLost && journal.CoverageLost());
    CHECK(journal.Size() == 2 && journal.Read(0, read) && read.opcode == Op::Begin);
    journal.Reset(); CHECK(journal.Size() == 0 && !journal.CoverageLost());
    Journal bytesOnly(Limits{10, 10, 10, kRecordSize});
    CHECK(bytesOnly.Append(r) == Result::Ok && bytesOnly.Append(r) == Result::CoverageLost);
    Journal none(Limits{0, 0, 0, 0}); CHECK(none.CoverageLost());
    none.Reset(); CHECK(none.CoverageLost());

    Ledger recordCap(Limits{1, 2, 2, 4096});
    r = Base(); CHECK(recordCap.SetScope(r.scope) == Result::Ok);
    CHECK(recordCap.Apply(r) == Result::Ok);
    auto next = r; next.opcode = Op::Cancel; ++next.globalSeq; ++next.ownerSeq;
    CHECK(recordCap.Apply(next) == Result::CoverageLost);
    CHECK(recordCap.RecordCount() == 1 && recordCap.CommandCount() == 1);
    CHECK(recordCap.SetScope(r.scope) == Result::CoverageLost);
    CHECK(recordCap.EnterNative(r) == Result::CoverageLost);
    ++r.scope.consumerEpoch; CHECK(recordCap.SetScope(r.scope) == Result::Ok);
    CHECK(recordCap.RecordCount() == 0 && recordCap.Apply(r) == Result::Ok);
    Ledger noBytes(Limits{1, 1, 1, 1}); CHECK(noBytes.CoverageLost());
    CHECK(noBytes.SetScope(r.scope) == Result::CoverageLost);
    Ledger overflowing(Limits{(std::numeric_limits<std::size_t>::max)(), 1, 1,
        (std::numeric_limits<std::size_t>::max)()});
    CHECK(overflowing.CoverageLost());
    Ledger commandCap(Limits{16, 1, 2, 8192});
    r = Base(); CHECK(commandCap.SetScope(r.scope) == Result::Ok);
    CHECK(commandCap.Apply(r) == Result::Ok);
    next = r; next.opcode = Op::Cancel; ++next.globalSeq; ++next.ownerSeq;
    CHECK(commandCap.Apply(next) == Result::Ok);
    next.opcode = Op::Release; ++next.globalSeq; ++next.ownerSeq;
    CHECK(commandCap.Apply(next) == Result::Ok);
    next.opcode = Op::Begin; next.parentCommandId = r.commandId; ++next.commandId;
    ++next.globalSeq; ++next.ownerSeq;
    CHECK(commandCap.Apply(next) == Result::CoverageLost);
    CHECK(commandCap.CommandCount() == 1); // Release retained the tombstone.
    Ledger ownerCap(Limits{16, 2, 1, 8192});
    CHECK(ownerCap.SetScope(r.scope) == Result::Ok && ownerCap.Apply(r) == Result::Ok);
    auto other = r.owner; ++other.entityA;
    CHECK(ownerCap.FenceOwner(other) == Result::CoverageLost);
}

void AdmissionBeforeUpdate() {
    // The causal journal may be ahead of execution. Never mutate a request
    // merely because its Submit record was received, or after native refusal.
    for (std::uint32_t outcome = 0; outcome < 4; ++outcome) {
        auto begin = Base(); Ledger ledger;
        CHECK(ledger.SetScope(begin.scope) == Result::Ok && ledger.Apply(begin) == Result::Ok);
        auto submit = begin; submit.opcode = Op::Submit; ++submit.globalSeq; ++submit.ownerSeq;
        CHECK(ledger.Apply(submit) == Result::Ok);
        if (outcome != 0) {
            CHECK(ledger.EnterNative(submit) == Result::Ok);
            const auto native = outcome == 1 ? NativeOutcome::Rejected :
                outcome == 2 ? NativeOutcome::Admitted : NativeOutcome::Unknown;
            CHECK(ledger.FinishNative(submit, native) ==
                (native == NativeOutcome::Unknown ? Result::OutcomeUnknown : Result::Ok));
        }
        auto update = submit; update.opcode = Op::Update;
        ++update.globalSeq; ++update.ownerSeq; ++update.parameterRevision;
        CHECK(ledger.Apply(update) == Result::Ok);
        const auto expected = outcome == 2 ? Result::Ok :
            outcome == 3 ? Result::OutcomeUnknown : Result::InvalidTransition;
        CHECK(ledger.EnterNative(update) == expected);
        if (expected == Result::Ok) CHECK(ledger.FinishNative(update, NativeOutcome::Admitted) == Result::Ok);
    }
}

void FencesAndEntry() {
    auto r = Base(); Ledger ledger(Limits{64, 8, 8, 32768});
    CHECK(ledger.SetScope(r.scope) == Result::Ok && ledger.Apply(r) == Result::Ok);
    auto submit = r; submit.opcode = Op::Submit; ++submit.globalSeq; ++submit.ownerSeq;
    CHECK(ledger.Apply(submit) == Result::Ok);
    auto changed = submit; changed.goalX += 1;
    CHECK(ledger.EnterNative(changed) == Result::Conflict);
    changed = submit; changed.flags |= Incomplete;
    CHECK(ledger.EnterNative(changed) == Result::ObservationRejected);
    changed = submit; changed.flags |= ObservationOnly;
    CHECK(ledger.EnterNative(changed) == Result::ObservationRejected);
    CHECK(ledger.FinishNative(submit, NativeOutcome::Admitted) == Result::InvalidTransition);
    CHECK(ledger.EnterNative(submit) == Result::Ok);
    CHECK(ledger.SetScope(r.scope) == Result::Ok); // Cannot erase the entered mark.
    auto pendingScope = r.scope; ++pendingScope.consumerEpoch;
    auto pendingOwner = r.owner; ++pendingOwner.ownerEpoch;
    CHECK(ledger.SetScope(pendingScope) == Result::NativeAlreadyEntered);
    CHECK(ledger.FenceOwner(pendingOwner) == Result::NativeAlreadyEntered);
    auto pendingOther = r.owner; ++pendingOther.entityA;
    CHECK(ledger.FenceOwner(pendingOther) == Result::NativeAlreadyEntered);
    CHECK(ledger.Apply(submit) == Result::Duplicate && ledger.RecordCount() == 2);
    CHECK(ledger.EnterNative(submit) == Result::NativeAlreadyEntered);
    CHECK(ledger.FinishNative(submit, NativeOutcome::Unknown) == Result::OutcomeUnknown);
    CHECK(ledger.FinishNative(submit, NativeOutcome::Admitted) == Result::OutcomeUnknown);
    CHECK(ledger.EnterNative(submit) == Result::OutcomeUnknown);
    auto cancel = submit; cancel.opcode = Op::Cancel; ++cancel.globalSeq; ++cancel.ownerSeq;
    CHECK(ledger.Apply(cancel) == Result::Ok);
    CHECK(ledger.EnterNative(cancel) == Result::OutcomeUnknown);
    auto newerOwner = r.owner; ++newerOwner.ownerEpoch;
    CHECK(ledger.FenceOwner(newerOwner) == Result::Ok);
    CHECK(ledger.Apply(cancel) == Result::StaleOwner);
    CHECK(ledger.EnterNative(cancel) == Result::StaleOwner);
    CHECK(ledger.FenceOwner(r.owner) == Result::StaleOwner);

    // New birth at the same ActorKey is explicitly fenced; old callbacks can
    // neither cancel nor release the successor even when commandId is reused.
    ++newerOwner.ownerEpoch; ++newerOwner.birth;
    CHECK(ledger.FenceOwner(newerOwner) == Result::Ok);
    auto successor = r; successor.owner = newerOwner; successor.globalSeq = cancel.globalSeq + 1;
    CHECK(ledger.Apply(successor) == Result::Ok);
    CommandState state{};
    CHECK(ledger.Find(successor, state) && state.phase == Phase::Begun);
    CHECK(ledger.Apply(cancel) == Result::StaleOwner);
    CHECK(ledger.Find(successor, state) && state.phase == Phase::Begun);
    auto oldScope = r.scope;
    auto newScope = r.scope; ++newScope.consumerEpoch;
    CHECK(ledger.SetScope(newScope) == Result::Ok);
    CHECK(ledger.Apply(r) == Result::WrongScope && ledger.EnterNative(submit) == Result::WrongScope);
    CHECK(ledger.FinishNative(submit, NativeOutcome::Admitted) == Result::WrongScope);
    CHECK(ledger.SetScope(oldScope) == Result::WrongScope);
    ++newScope.worldEpoch; CHECK(ledger.SetScope(newScope) == Result::Ok);
    auto rollback = newScope; --rollback.worldEpoch; ++rollback.consumerEpoch;
    CHECK(ledger.SetScope(rollback) == Result::WrongScope);
    ++newScope.authorityEpoch; CHECK(ledger.SetScope(newScope) == Result::Ok);
    rollback = newScope; --rollback.authorityEpoch; ++rollback.worldEpoch;
    CHECK(ledger.SetScope(rollback) == Result::WrongScope);
}

void OwnerQuarantineAndReentrance() {
    auto r = Base(); Ledger ledger(Limits{64, 8, 8, 32768});
    CHECK(ledger.SetScope(r.scope) == Result::Ok && ledger.Apply(r) == Result::Ok);
    r.opcode = Op::Submit; ++r.globalSeq; ++r.ownerSeq;
    CHECK(ledger.Apply(r) == Result::Ok && ledger.EnterNative(r) == Result::Ok);
    auto other = Base(); other.owner.entityA += 100; other.globalSeq = r.globalSeq + 1;
    CHECK(ledger.Apply(other) == Result::Ok);
    other.opcode = Op::Submit; ++other.globalSeq; ++other.ownerSeq;
    CHECK(ledger.Apply(other) == Result::Ok);
    CHECK(ledger.EnterNative(other) == Result::NativeAlreadyEntered);
    CHECK(ledger.FinishNative(r, NativeOutcome::Unknown) == Result::OutcomeUnknown);
    // Independent actor remains usable once the call stack has returned.
    CHECK(ledger.EnterNative(other) == Result::Ok);
    CHECK(ledger.FinishNative(other, NativeOutcome::Rejected) == Result::Ok);
    CHECK(ledger.EnterNative(other) == Result::NativeAlreadyEntered);
    auto oldSubmit = r;
    r.opcode = Op::Cancel; r.globalSeq = other.globalSeq + 1; ++r.ownerSeq;
    CHECK(ledger.Apply(r) == Result::Ok);
    r.opcode = Op::Release; ++r.globalSeq; ++r.ownerSeq;
    CHECK(ledger.Apply(r) == Result::Ok);
    r.opcode = Op::Begin; ++r.globalSeq; ++r.ownerSeq;
    r.parentCommandId = r.commandId; ++r.commandId;
    CHECK(ledger.Apply(r) == Result::Ok);
    r.opcode = Op::Submit; ++r.globalSeq; ++r.ownerSeq;
    CHECK(ledger.Apply(r) == Result::Ok);
    CHECK(ledger.EnterNative(r) == Result::OutcomeUnknown);
    auto fenced = r.owner; ++fenced.ownerEpoch;
    CHECK(ledger.FenceOwner(fenced) == Result::Ok);
    CHECK(ledger.FinishNative(oldSubmit, NativeOutcome::Admitted) == Result::StaleOwner);
    CHECK(ledger.EnterNative(r) == Result::StaleOwner);
    r.owner = fenced; r.opcode = Op::Begin; r.ownerSeq = 1; ++r.globalSeq;
    r.parentCommandId = 0; ++r.commandId;
    CHECK(ledger.Apply(r) == Result::Ok);
    r.opcode = Op::Submit; ++r.globalSeq; ++r.ownerSeq;
    CHECK(ledger.Apply(r) == Result::Ok && ledger.EnterNative(r) == Result::Ok);
    CHECK(ledger.FinishNative(r, NativeOutcome::Admitted) == Result::Ok);
}

void NativeIntentEvidenceAndOrdering() {
    const auto next = [](Record r, Op op) {
        ++r.globalSeq; ++r.ownerSeq; r.opcode = op;
        if (op == Op::Update) ++r.parameterRevision;
        return r;
    };
    auto begin = Base(); begin.nativeKind = 0x200U; begin.admission = 1;
    begin.intent.words[0] = 8; begin.intent.words[1] = 1; begin.intent.words[2] = 0x80000000U;
    Ledger ledger({}, true);
    CHECK(ledger.SetScope(begin.scope) == Result::Ok && ledger.Apply(begin) == Result::Ok);
    auto submit = next(begin, Op::Submit); CHECK(ledger.Apply(submit) == Result::Ok);
    auto changed = submit; changed.intent.words[2] = 0;
    CHECK(Bytes(changed) == Bytes(submit));
    CHECK(ledger.Apply(changed) == Result::Conflict && ledger.RecordCount() == 2);
    CHECK(ledger.NativeReadiness(changed) == Result::Conflict);
    CHECK(ledger.EnterNative(changed) == Result::Conflict);
    CommandState state{}; CHECK(!ledger.Find(changed, state));
    CHECK(ledger.Apply(submit) == Result::Duplicate && ledger.Find(submit, state));
    CHECK(ledger.EnterNative(submit) == Result::Ok);
    CHECK(ledger.FinishNative(changed, NativeOutcome::Admitted) == Result::Conflict);
    CHECK(ledger.FinishNative(submit, NativeOutcome::Admitted) == Result::Ok);

    // Even if a caller sends revisions of a direct native command, they are
    // invocation evidence, never scalar current-intent coalescing candidates.
    const auto first = next(submit, Op::Update); CHECK(ledger.Apply(first) == Result::Ok);
    const auto second = next(first, Op::Update); CHECK(ledger.Apply(second) == Result::Ok);
    auto complete = next(second, Op::Complete); complete.intent = {};
    CHECK(ledger.Apply(complete) == Result::Ok);
    auto release = next(complete, Op::Release); CHECK(ledger.Apply(release) == Result::Ok);
    CHECK(!ledger.Superseded(first.globalSeq) && !ledger.Superseded(second.globalSeq));
    CHECK(ledger.NativeReadiness(second) == Result::OwnerBusy);
    CHECK(ledger.NativeReadiness(release) == Result::OwnerBusy);
    CHECK(ledger.EnterNative(first) == Result::Ok && ledger.FinishNative(first, NativeOutcome::Admitted) == Result::Ok);
    CHECK(ledger.EnterNative(second) == Result::Ok && ledger.FinishNative(second, NativeOutcome::Admitted) == Result::Ok);
    CHECK(ledger.EnterNative(release) == Result::Ok && ledger.FinishNative(release, NativeOutcome::Admitted) == Result::Ok);

    // Bypass is an explicit lifecycle/constructor privilege, not >=0x100.
    // Append avoids ordinary replacement supersession hiding the ordering.
    for (const auto kind : {0x100U, 0x105U, 0x106U, 0x1FFU, 0x200U, 0x201U}) {
        Ledger ordered({}, true);
        auto scalar = Base(); scalar.nativeKind = 4; scalar.admission = 1;
        CHECK(ordered.SetScope(scalar.scope) == Result::Ok && ordered.Apply(scalar) == Result::Ok);
        scalar = next(scalar, Op::Submit); CHECK(ordered.Apply(scalar) == Result::Ok);
        auto native = next(scalar, Op::Begin); ++native.commandId; native.nativeKind = kind;
        if (kind == 0x200U) native.intent.words[0] = 1;
        CHECK(ordered.Apply(native) == Result::Ok);
        native = next(native, Op::Submit); CHECK(ordered.Apply(native) == Result::Ok);
        const auto expected = kind == 0x100U || kind == 0x105U || kind == 0x200U ? Result::Ok : Result::OwnerBusy;
        CHECK(ordered.NativeReadiness(native) == expected && !ordered.Superseded(scalar.globalSeq));
    }
}

void CurrentIntentPreemption() {
    const auto next=[](Record r,Op opcode) {++r.globalSeq;++r.ownerSeq;r.opcode=opcode;return r;};
    auto begin=Base();begin.nativeKind=1;begin.admission=1;
    Ledger ledger({},true);
    CHECK(ledger.SetScope(begin.scope)==Result::Ok&&ledger.Apply(begin)==Result::Ok);
    auto submit=next(begin,Op::Submit);
    CHECK(ledger.Apply(submit)==Result::Ok);
    std::array<Record,10> updates{};
    auto latest=submit;
    for(std::size_t i=0;i<updates.size();++i) {
        latest=next(latest,Op::Update);++latest.parameterRevision;
        latest.goalX=static_cast<float>(i+2);
        CHECK(ledger.Apply(latest)==Result::Ok);
        updates[i]=latest;
    }
    for(std::size_t i=0;i+1<updates.size();++i) {
        CHECK(ledger.Superseded(updates[i].globalSeq));
        CHECK(ledger.NativeReadiness(updates[i])==Result::Duplicate);
    }
    // The newest goal still needs its command's Submit, not nine old goals.
    CHECK(ledger.NativeReadiness(latest)==Result::OwnerBusy);
    CHECK(ledger.EnterNative(submit)==Result::Ok);
    CHECK(ledger.FinishNative(submit,NativeOutcome::Admitted)==Result::Ok);
    CHECK(ledger.NativeReadiness(latest)==Result::Ok);
    CHECK(ledger.EnterNative(latest)==Result::Ok);
    CHECK(ledger.FinishNative(latest,NativeOutcome::Admitted)==Result::Ok);

    // Host admission=0 supersedes a different unentered movement command.
    auto older=next(latest,Op::Begin);
    ++older.commandId;older.parentCommandId=begin.commandId;older.parameterRevision=1;
    CHECK(ledger.Apply(older)==Result::Ok);
    auto pending=next(older,Op::Submit);CHECK(ledger.Apply(pending)==Result::Ok);
    auto newCommand=next(pending,Op::Begin);
    ++newCommand.commandId;newCommand.parentCommandId=older.commandId;
    newCommand.parameterRevision=1;newCommand.admission=0;
    CHECK(ledger.Apply(newCommand)==Result::Ok);
    auto replacing=next(newCommand,Op::Submit);CHECK(ledger.Apply(replacing)==Result::Ok);
    CHECK(ledger.Superseded(pending.globalSeq));
    CHECK(ledger.NativeReadiness(pending)==Result::Duplicate);
    CHECK(ledger.NativeReadiness(replacing)==Result::Ok);
}
void SchedulerControlOrdering() {
    const auto next=[](Record r,Op op) {++r.globalSeq;++r.ownerSeq;r.opcode=op;return r;};
    for(auto kind:{6U,8U,11U}) for(auto terminal:{Op::Complete,Op::Cancel,Op::Stop}) {
        auto begin=Base();begin.nativeKind=kind;begin.flags=0;
        begin.goalX=begin.goalY=begin.goalZ=begin.distance=begin.stopDistance=begin.aux=0;
        Ledger ledger({},true);
        CHECK(ledger.SetScope(begin.scope)==Result::Ok&&ledger.Apply(begin)==Result::Ok);
        auto submit=next(begin,Op::Submit);CHECK(ledger.Apply(submit)==Result::Ok);
        auto update=next(submit,Op::Update);++update.parameterRevision;update.scalarRetention=0;
        CHECK(ledger.Apply(update)==Result::Ok);
        auto ended=next(update,terminal);ended.scalarRetention=3;CHECK(ledger.Apply(ended)==Result::Ok);
        auto release=next(ended,Op::Release);CHECK(ledger.Apply(release)==Result::Ok);
        auto later=next(release,Op::Begin);++later.commandId;later.parameterRevision=1;later.nativeKind=0x200;
        later.intent.words[0]=1;later.admission=1;CHECK(ledger.Apply(later)==Result::Ok);
        later=next(later,Op::Submit);CHECK(ledger.Apply(later)==Result::Ok);
        // A short queue control/placement is not a stale goal. Neither its terminal
        // record nor a later direct action can discard or overtake admission.
        CHECK(!ledger.Superseded(submit.globalSeq)&&!ledger.Superseded(update.globalSeq));
        CHECK(ledger.NativeReadiness(later)==Result::OwnerBusy);
        CHECK(ledger.EnterNative(submit)==Result::Ok&&ledger.FinishNative(submit,NativeOutcome::Admitted)==Result::Ok);
        CHECK(ledger.EnterNative(update)==Result::Ok&&ledger.FinishNative(update,NativeOutcome::Admitted)==Result::Ok);
        if(terminal!=Op::Complete)CHECK(ledger.EnterNative(ended)==Result::Ok&&ledger.FinishNative(ended,NativeOutcome::Admitted)==Result::Ok);
        CHECK(ledger.NativeReadiness(later)==Result::OwnerBusy);
        CHECK(ledger.EnterNative(release)==Result::Ok&&ledger.FinishNative(release,NativeOutcome::Admitted)==Result::Ok);
        CHECK(ledger.NativeReadiness(later)==Result::Ok);
    }
}

std::uint32_t Random(std::uint32_t& state) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

// Independent expectations are deliberately held outside the implementation:
// every accepted operation is checked against this simple lifecycle oracle.
struct Expected {
    std::uint32_t revision{1};
    Phase phase{Phase::Begun};
    std::uint64_t global{};
    std::uint64_t owner{};
    std::size_t commands{};
    std::size_t records{};
    std::size_t entries{};
    bool ownerQuarantined{};
};
void GeneratedTraces() {
    std::size_t totalOperations{};
    std::size_t unknownOutcomes{};
    for (std::uint32_t trace = 1; trace <= 10000; ++trace) {
        std::uint32_t random = trace * 747796405U + 2891336453U;
        const Limits limits{96, 4, 4, 65536};
        Ledger ledger(limits);
        Journal journal(Limits{96, 4, 4, 96 * kRecordSize});
        auto current = Base();
        current.scope.session[15] = static_cast<std::uint8_t>(trace);
        current.owner.birth = 1 + trace;
        CHECK(ledger.SetScope(current.scope) == Result::Ok);
        Expected expected{};
        const auto accept = [&](Record operation, Phase phase, std::uint32_t revision) {
            operation.globalSeq = expected.global + 1;
            operation.ownerSeq = expected.owner + 1;
            const auto before = ledger.RecordCount();
            CHECK(ledger.Apply(operation) == Result::Ok);
            if (operation.opcode == Op::Begin) ++expected.commands;
            ++expected.records; ++expected.global; ++expected.owner;
            expected.phase = phase; expected.revision = revision;
            CHECK(ledger.RecordCount() == before + 1 && ledger.RecordCount() == expected.records);
            CHECK(ledger.CommandCount() == expected.commands);
            CHECK(ledger.Apply(operation) == Result::Duplicate && ledger.RecordCount() == expected.records);
            CHECK(ledger.SetScope(operation.scope) == Result::Ok);
            CHECK(ledger.Apply(operation) == Result::Duplicate);
            auto conflicting = operation; conflicting.aux += 1.0F;
            CHECK(ledger.Apply(conflicting) == Result::Conflict && ledger.RecordCount() == expected.records);
            CommandState state{};
            CHECK(ledger.Find(operation, state));
            CHECK(state.phase == phase && state.parameterRevision == revision && state.commandId == operation.commandId);
            CHECK(state.owner == operation.owner && state.parentCommandId == operation.parentCommandId);
            auto observed = operation; observed.flags |= ObservationOnly;
            CHECK(journal.Append(observed) == Result::Ok);
            CHECK(ledger.Apply(observed) == Result::ObservationRejected);
            CHECK(journal.Size() == expected.records && !journal.CoverageLost());
            CHECK(ledger.AllocatedBytes() <= limits.maxBytes && !ledger.CoverageLost());
            current = operation;
            ++totalOperations;
        };
        // Three explicit incarnations with varying mutations and terminal
        // causes. A short Begin/Cancel/Release is never lost between snapshots.
        Record priorTerminal{};
        for (std::uint32_t incarnation = 0; incarnation < 3; ++incarnation) {
            current.opcode = Op::Begin; current.parameterRevision = 1;
            accept(current, Phase::Begun, 1);
            auto bad = current; ++bad.globalSeq; ++bad.ownerSeq; ++bad.commandId;
            CHECK(ledger.Apply(bad) == Result::OwnerBusy);
            bad.opcode = Op::Update; CHECK(ledger.Apply(bad) == Result::MissingBegin);
            bad = current; bad.opcode = Op::Update; bad.globalSeq += 2; ++bad.ownerSeq;
            CHECK(ledger.Apply(bad) == Result::GlobalGap);
            --bad.globalSeq; ++bad.ownerSeq; CHECK(ledger.Apply(bad) == Result::OwnerGap);
            bad = current; ++bad.globalSeq; ++bad.ownerSeq; bad.opcode = Op::Update;
            bad.parameterRevision += 2; CHECK(ledger.Apply(bad) == Result::RevisionMismatch);
            auto stale = current; ++stale.scope.worldEpoch;
            CHECK(ledger.Apply(stale) == Result::WrongScope);
            stale = current; ++stale.owner.ownerEpoch; CHECK(ledger.Apply(stale) == Result::StaleOwner);
            if (incarnation != 0) {
                CHECK(ledger.Apply(priorTerminal) == Result::Duplicate);
                CHECK(ledger.EnterNative(priorTerminal) == Result::InvalidTransition);
            }
            const auto preUpdates = Random(random) % 4;
            for (std::uint32_t i = 0; i < preUpdates; ++i) {
                current.opcode = Op::Update; ++current.parameterRevision;
                current.goalX = static_cast<float>(Random(random) % 1000) * 0.25F;
                accept(current, Phase::Begun, expected.revision + 1);
            }
            const bool shortLived = Random(random) % 5 == 0;
            Record submit{};
            bool unknown = expected.ownerQuarantined;
            if (!shortLived) {
                current.opcode = Op::Submit; accept(current, Phase::Submitted, expected.revision);
                submit = current;
                const bool delayed = Random(random) % 3 == 0;
                if (!delayed) {
                    if (expected.ownerQuarantined) {
                        CHECK(ledger.EnterNative(submit) == Result::OutcomeUnknown);
                    } else {
                        CHECK(ledger.EnterNative(submit) == Result::Ok); ++expected.entries;
                        CHECK(ledger.EnterNative(submit) == Result::NativeAlreadyEntered);
                        unknown = Random(random) % 7 == 0;
                        CHECK(ledger.FinishNative(submit, unknown ? NativeOutcome::Unknown : NativeOutcome::Admitted) ==
                            (unknown ? Result::OutcomeUnknown : Result::Ok));
                        if (unknown) { ++unknownOutcomes; expected.ownerQuarantined = true; }
                        CHECK(ledger.EnterNative(submit) == (unknown ? Result::OutcomeUnknown : Result::NativeAlreadyEntered));
                    }
                }
                const auto postUpdates = Random(random) % 4;
                for (std::uint32_t i = 0; i < postUpdates; ++i) {
                    current.opcode = Op::Update; ++current.parameterRevision;
                    current.gait = Random(random) % 5;
                    accept(current, Phase::Submitted, expected.revision + 1);
                    if (unknown) CHECK(ledger.EnterNative(current) == Result::OutcomeUnknown);
                    else if (delayed) CHECK(ledger.EnterNative(current) == Result::InvalidTransition);
                    else {
                        CHECK(ledger.EnterNative(current) == Result::Ok);
                        CHECK(ledger.FinishNative(current, NativeOutcome::Admitted) == Result::Ok);
                    }
                }
                if (delayed && postUpdates != 0) CHECK(ledger.EnterNative(submit) == Result::InvalidTransition);
            }
            const auto choice = Random(random) % (shortLived ? 2U : 3U);
            current.opcode = choice == 0 ? Op::Cancel : choice == 1 ? Op::Stop : Op::Complete;
            const auto terminal = choice == 0 ? Phase::Cancelled : choice == 1 ? Phase::Stopped : Phase::Completed;
            accept(current, terminal, expected.revision);
            auto terminalRecord = current;
            if (!shortLived) CHECK(ledger.EnterNative(submit) != Result::Ok);
            auto orphan = current; orphan.opcode = Op::Update;
            ++orphan.parameterRevision; ++orphan.globalSeq; ++orphan.ownerSeq;
            CHECK(ledger.Apply(orphan) == Result::InvalidTransition);
            current.opcode = Op::Release; accept(current, Phase::Released, expected.revision);
            CHECK(ledger.EnterNative(terminalRecord) != Result::Ok);
            CommandState old{};
            CHECK(ledger.Find(current, old) && old.phase == Phase::Released);
            // Use the terminal mutation as the delayed token, not Release's
            // own explicitly authorized cleanup operation.
            priorTerminal = terminalRecord;
            current.opcode = Op::Begin;
            current.parentCommandId = current.commandId; ++current.commandId;
        }
        CHECK(expected.commands == 3 && ledger.CommandCount() == 3);
        CHECK(expected.entries <= 3);
        Record observed{};
        CHECK(journal.Read(0, observed) && observed.opcode == Op::Begin);
        CHECK(journal.Read(journal.Size() - 1, observed) && observed.opcode == Op::Release);
        CHECK(totalOperations != 0);
    }
    CHECK(totalOperations > 150000 && unknownOutcomes > 1000);
    std::cout << "10000 generated lifecycle traces: " << totalOperations
        << " accepted operations, " << unknownOutcomes << " unknown outcomes; all invariants passed\n";
}
} // namespace

int main(int argc, char** argv) {
    static_assert(std::is_nothrow_constructible_v<Journal, Limits>);
    static_assert(std::is_nothrow_constructible_v<Ledger, Limits>);
    Codec(argc > 1 ? argv[1] : "tests/fixtures/aisync-transaction-v1.hex");
    BoundsAndJournal();
    AdmissionBeforeUpdate();
    FencesAndEntry();
    OwnerQuarantineAndReentrance();
    NativeIntentEvidenceAndOrdering();
    CurrentIntentPreemption();
    SchedulerControlOrdering();
    GeneratedTraces();
    std::cout << "AISync transaction canonical codec, journal, caps and native entry checks passed\n";
}
