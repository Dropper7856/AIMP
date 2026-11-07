#include "aimp/AISyncTransactionExecutor.h"

#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace aimp::ai_sync::transactions;

namespace {
void Check(bool condition, int line) {
    if (!condition) {
        std::cerr << "AISync transaction executor invariant failed at line " << line << '\n';
        std::abort();
    }
}
#define CHECK(value) Check((value), __LINE__)

Record Base() {
    Record r{};
    r.scope.session[0] = 17;
    r.scope.authorityEpoch = 1; r.scope.worldEpoch = 2; r.scope.consumerEpoch = 3;
    r.owner = OwnerKey{11, 22, 1, 2, Capability::Movement, 1};
    r.commandId = 100; r.globalSeq = 1; r.ownerSeq = 1;
    r.parameterRevision = 1;
    r.flags = HasGoal | HasGait | HasStopDistance;
    r.goalX = 1.25F; r.goalY = -2.5F; r.goalZ = 3.75F;
    r.distance = 4.5F; r.stopDistance = 0.5F; r.aux = -0.25F;
    return r;
}
Record Next(Record r, Op op) {
    ++r.globalSeq; ++r.ownerSeq; r.opcode = op;
    if (op == Op::Update) ++r.parameterRevision;
    return r;
}
CanonicalRecord Bytes(const Record& r) {
    CanonicalRecord out{};
    CHECK(Encode(r, out) == Result::Ok);
    return out;
}
bool Same(const ExecutionDisposition& a, const ExecutionDisposition& b) {
    return a.status == b.status && a.result == b.result &&
        a.acceptedLogical == b.acceptedLogical && a.nativeEntered == b.nativeEntered &&
        a.hasNativeOutcome == b.hasNativeOutcome && a.nativeOutcome == b.nativeOutcome;
}

// Offline integration adapter only: the production driver has no engine stub.
// Each callback records the complete token, and assertions model native entry
// separately from logical Apply. Hooks inject controlled failures and reentry.
struct Harness {
    Dependency dependency{Dependency::Ready};
    NativeOutcome outcome{NativeOutcome::Admitted};
    bool throwCheck{};
    bool throwInvoke{};
    std::vector<CanonicalRecord> checked;
    std::vector<CanonicalRecord> invoked;
    void (*onCheck)(Harness&, const Record&){};
    void (*onInvoke)(Harness&, const Record&){};
    TransactionExecutor* executor{};
    Record* externalRecord{};

    static Dependency CheckDependencies(void* context, const Record& r) {
        auto& h = *static_cast<Harness*>(context);
        h.checked.push_back(Bytes(r));
        if (h.onCheck) h.onCheck(h, r);
        if (h.throwCheck) throw std::runtime_error("dependency failure");
        return h.dependency;
    }
    static NativeOutcome Invoke(void* context, const Record& r) {
        auto& h = *static_cast<Harness*>(context);
        h.invoked.push_back(Bytes(r));
        if (h.onInvoke) h.onInvoke(h, r);
        if (h.throwInvoke) throw std::runtime_error("native failure after entry");
        return h.outcome;
    }
    TransactionAdapter Adapter() noexcept { return {this, &CheckDependencies, &Invoke}; }
};
ExecutionDisposition Accepted(TransactionExecutor& executor, const Record& r, bool native) {
    const auto disposition = executor.Execute(r);
    CHECK(disposition.result == Result::Ok && disposition.acceptedLogical);
    CHECK(disposition.nativeEntered == native && disposition.hasNativeOutcome == native);
    CHECK(disposition.status == (native ? ExecutionStatus::NativeFinished : ExecutionStatus::Metadata));
    if (native) CHECK(disposition.nativeOutcome == NativeOutcome::Admitted);
    return disposition;
}
void Duplicate(TransactionExecutor& executor, Harness& h, const Record& r,
    const ExecutionDisposition& expected) {
    const auto checked = h.checked.size();
    const auto invoked = h.invoked.size();
    const auto count = executor.RecordCount();
    auto actual = executor.Execute(r);
    CHECK(actual.duplicate && Same(actual, expected));
    actual = executor.Retry(r);
    if (expected.status != ExecutionStatus::Waiting) CHECK(actual.duplicate && Same(actual, expected));
    CHECK(h.invoked.size() == invoked);
    if (expected.status != ExecutionStatus::Waiting) CHECK(h.checked.size() == checked);
    CHECK(executor.RecordCount() == count);
}

void AcceptedReplacementFencesLateEdits() {
    struct Case { std::uint32_t oldKind, newKind, admission; NativeOutcome outcome; bool otherActor, superseded; };
    const Case cases[]{
        {1,1,0,NativeOutcome::Admitted,false,true},
        {7,1,0,NativeOutcome::Admitted,false,true},
        {1,1,1,NativeOutcome::Admitted,false,false},
        {1,1,0,NativeOutcome::Rejected,false,false},
        {1,1,0,NativeOutcome::Unknown,false,false},
        {1,1,0,NativeOutcome::Admitted,true,false},
        {5,1,0,NativeOutcome::Admitted,false,false},
        {1,5,0,NativeOutcome::Admitted,false,false},
    };
    for(const auto& test:cases) {
        Harness h;TransactionExecutor e(h.Adapter(),{},true);
        auto old=Base();old.nativeKind=test.oldKind;
        CHECK(e.Configure(old.scope)==Result::Ok);
        Accepted(e,old,false);old=Next(old,Op::Submit);Accepted(e,old,true);
        // The old Submit may already be checkpointed when replacement wins.
        CHECK(e.Checkpoint(old.scope,old.globalSeq)==Result::Ok);
        auto current=Next(old,Op::Begin);++current.commandId;current.nativeKind=test.newKind;
        current.admission=test.admission;
        if(test.otherActor){++current.owner.entityA;current.ownerSeq=1;}
        Accepted(e,current,false);current=Next(current,Op::Submit);h.outcome=test.outcome;
        static_cast<void>(e.Execute(current));
        auto late=Next(old,Op::Update);late.globalSeq=current.globalSeq+1;
        if(!test.otherActor)late.ownerSeq=current.ownerSeq+1;
        h.outcome=NativeOutcome::Admitted;
        const auto checks=h.checked.size(),calls=h.invoked.size();
        const auto result=e.Execute(late);
        if(test.superseded) {
            CHECK(result.status==ExecutionStatus::Superseded&&result.result==Result::Ok);
            CHECK(result.acceptedLogical&&!result.nativeEntered&&!result.hasNativeOutcome);
            CHECK(h.checked.size()==checks&&h.invoked.size()==calls);
            CHECK(e.HasNativeOwnership(old.owner));
        } else if(test.outcome==NativeOutcome::Unknown) {
            CHECK(result.result==Result::OutcomeUnknown&&!result.nativeEntered&&h.invoked.size()==calls);
        } else {
            CHECK(result.nativeEntered&&result.nativeOutcome==NativeOutcome::Admitted);
            CHECK(h.invoked.size()==calls+1);
        }
    }
}
void Lifecycle() {
    Harness h;
    TransactionExecutor e(h.Adapter());
    CHECK(!e.CoverageLost() && e.AllocatedBytes() <= Limits{}.maxBytes);
    auto r = Base();
    CHECK(e.Configure(r.scope) == Result::Ok);
    const auto begin = Accepted(e, r, false);
    Duplicate(e, h, r, begin);
    r = Next(r, Op::Update);
    r.goalX = 45.5F; r.gait = 3; r.nativeKind = 7;
    Accepted(e, r, false);
    CHECK(h.checked.empty() && h.invoked.empty());
    r = Next(r, Op::Submit);
    const auto submit = Accepted(e, r, true);
    CHECK(h.invoked.size() == 1 && h.invoked.back() == Bytes(r));
    CommandState state{};
    CHECK(e.Find(r, state) && state.nativeAdmitted && state.phase == Phase::Submitted);
    Duplicate(e, h, r, submit);
    r = Next(r, Op::Update); r.goalX = 55; r.mode = 4;
    Accepted(e, r, true);
    CHECK(h.invoked.back() == Bytes(r));
    r = Next(r, Op::Complete);
    Accepted(e, r, false);
    CHECK(h.invoked.size() == 2);
    CHECK(e.Find(r, state) && state.phase == Phase::Completed);
    auto replacement = Next(r, Op::Begin);
    ++replacement.commandId; replacement.parentCommandId = r.commandId; replacement.parameterRevision = 1;
    CHECK(e.Execute(replacement).result == Result::OwnerBusy);
    auto newScope = r.scope; ++newScope.consumerEpoch;
    CHECK(e.Configure(newScope) == Result::OwnerBusy);
    auto newOwner = r.owner; ++newOwner.ownerEpoch;
    CHECK(e.FenceOwner(newOwner) == Result::OwnerBusy);
    r = Next(r, Op::Release);
    const auto release = Accepted(e, r, true);
    CHECK(h.invoked.size() == 3);
    replacement = Next(r, Op::Begin);
    ++replacement.commandId; replacement.parentCommandId = r.commandId; replacement.parameterRevision = 1;
    Accepted(e, replacement, false);
    Duplicate(e, h, r, release);
    auto lateCleanup = Next(replacement, Op::Release);
    lateCleanup.commandId = r.commandId; lateCleanup.parentCommandId = 0;
    lateCleanup.parameterRevision = r.parameterRevision;
    CHECK(e.Execute(lateCleanup).result == Result::InvalidTransition);
    CHECK(h.invoked.size() == 3);
    CHECK(e.Configure(newScope) == Result::Ok);
    CHECK(e.RecordCount() == 0 && e.Execute(r).result == Result::WrongScope);
}

void ProfileBinding() {
    Harness h; TransactionExecutor e(h.Adapter()); auto r = Base();
    CHECK(e.Configure(r.scope) == Result::Ok); Accepted(e, r, false);
    r = Next(r, Op::Submit); r.nativeKind = 4; Accepted(e, r, true);
    auto update = Next(r, Op::Update); update.nativeKind = 7;
    const auto count = e.RecordCount();
    CHECK(e.Execute(update).result == Result::InvalidRecord && e.RecordCount() == count);
    update.nativeKind = 4; Accepted(e, update, true);
    r = Next(update, Op::Complete); Accepted(e, r, false);
    r = Next(r, Op::Release); r.nativeKind = 7;
    CHECK(e.Execute(r).result == Result::InvalidRecord);
    r.nativeKind = 4;
    h.dependency = Dependency::Waiting;
    const auto calls = h.invoked.size();
    CHECK(e.Execute(r).status == ExecutionStatus::Waiting && h.invoked.size() == calls);
    auto scope = r.scope; ++scope.consumerEpoch;
    CHECK(e.Configure(scope) == Result::OwnerBusy);
    h.dependency = Dependency::Ready;
    CHECK(e.Retry(r).nativeOutcome == NativeOutcome::Admitted && h.invoked.size() == calls + 1);
    auto replacement = Next(r, Op::Begin); ++replacement.commandId;
    replacement.parentCommandId = r.commandId; replacement.parameterRevision = 1; replacement.nativeKind = 7;
    Accepted(e, replacement, false);
    CHECK(e.Configure(scope) == Result::Ok);
}
void NativePipeline() {
    Harness h;TransactionExecutor e(h.Adapter(),{},true);auto begin=Base();begin.nativeKind=4;begin.admission=1;begin.scalarRetention=2;
    CHECK(e.Configure(begin.scope)==Result::Ok);Accepted(e,begin,false);
    auto submit=Next(begin,Op::Submit);h.dependency=Dependency::Waiting;
    CHECK(e.Execute(submit).status==ExecutionStatus::Waiting);
    auto first=Next(submit,Op::Update);first.goalX=8;auto second=Next(first,Op::Update);second.goalX=9;
    CHECK(e.Execute(first).status==ExecutionStatus::Waiting&&e.Execute(second).status==ExecutionStatus::Waiting);
    // Current intent: the newer Update makes the older unapplied one obsolete.
    CHECK(e.WaitingCount()==2&&h.checked.size()==1&&h.invoked.empty());
    auto changed=submit;changed.admission=2;CHECK(e.Retry(changed).result==Result::Conflict);
    changed=submit;changed.scalarRetention=3;CHECK(e.Execute(changed).result==Result::Conflict);
    h.dependency=Dependency::Ready;
    CHECK(e.Retry(second).status==ExecutionStatus::Waiting&&h.invoked.empty());
    CHECK(e.Retry(submit).nativeEntered&&h.invoked.size()==1);
    CHECK(e.Retry(second).nativeEntered&&h.invoked.size()==2);
    CHECK(e.Retry(first).status==ExecutionStatus::Superseded&&h.invoked.size()==2&&!e.HasWaiting());
    auto sibling=Next(second,Op::Begin);++sibling.commandId;sibling.parameterRevision=1;sibling.admission=2;sibling.nativeKind=7;
    Accepted(e,sibling,false);auto siblingSubmit=Next(sibling,Op::Submit);Accepted(e,siblingSubmit,true);
    // Exact older-command cleanup must not resolve through its new sibling.
    auto complete=Next(second,Op::Complete);complete.globalSeq=complete.ownerSeq=siblingSubmit.globalSeq+1;
    Accepted(e,complete,false);auto release=Next(complete,Op::Release);Accepted(e,release,true);
    auto finish=Next(siblingSubmit,Op::Complete);finish.globalSeq=finish.ownerSeq=release.globalSeq+1;
    Accepted(e,finish,false);Accepted(e,Next(finish,Op::Release),true);
    CHECK(!e.HasNativeOwnership(begin.owner));
    // Completion supersedes all causally queued work without native entry.
    Harness pending;TransactionExecutor queue(pending.Adapter(),{},true);begin=Base();CHECK(queue.Configure(begin.scope)==Result::Ok);
    Accepted(queue,begin,false);submit=Next(begin,Op::Submit);pending.dependency=Dependency::Waiting;
    CHECK(queue.Execute(submit).status==ExecutionStatus::Waiting);
    first=Next(submit,Op::Update);second=Next(first,Op::Update);
    CHECK(queue.Execute(first).status==ExecutionStatus::Waiting&&queue.Execute(second).status==ExecutionStatus::Waiting);
    complete=Next(second,Op::Complete);Accepted(queue,complete,false);CHECK(!queue.HasWaiting());
    CHECK(queue.Retry(submit).status==ExecutionStatus::Superseded&&queue.Retry(first).status==ExecutionStatus::Superseded);
    pending.dependency=Dependency::Ready;release=Next(complete,Op::Release);Accepted(queue,release,true);
    CHECK(pending.invoked.size()==1&&pending.invoked[0]==Bytes(release));
    // A later same-actor Submit preserves native call order across commands.
    Harness ordered;TransactionExecutor order(ordered.Adapter(),{},true);begin=Base();CHECK(order.Configure(begin.scope)==Result::Ok);
    Accepted(order,begin,false);submit=Next(begin,Op::Submit);ordered.dependency=Dependency::Waiting;
    CHECK(order.Execute(submit).status==ExecutionStatus::Waiting);
    sibling=Next(submit,Op::Begin);++sibling.commandId;sibling.parameterRevision=1;sibling.admission=1;
    Accepted(order,sibling,false);siblingSubmit=Next(sibling,Op::Submit);CHECK(order.Execute(siblingSubmit).status==ExecutionStatus::Waiting);
    ordered.dependency=Dependency::Ready;CHECK(order.Retry(siblingSubmit).status==ExecutionStatus::Waiting&&ordered.invoked.empty());
    CHECK(order.Retry(submit).nativeEntered&&order.Retry(siblingSubmit).nativeEntered&&ordered.invoked.size()==2);
}
void NativeIntentEvidence() {
    Harness h; TransactionExecutor e(h.Adapter(), {}, true);
    auto begin = Base(); begin.nativeKind = 0x200U; begin.admission = 1;
    begin.intent.words[0] = 8; begin.intent.words[1] = 1;
    begin.intent.words[2] = 0x80000000U;
    CHECK(e.Configure(begin.scope) == Result::Ok); Accepted(e, begin, false);
    auto submit = Next(begin, Op::Submit); const auto exact = submit;
    h.onCheck = [](Harness&, const Record& r) {
        CHECK(r.intent.words[0] == 8 && r.intent.words[2] == 0x80000000U);
    };
    h.dependency = Dependency::Waiting;
    const auto waiting = e.Execute(submit);
    CHECK(waiting.status == ExecutionStatus::Waiting && !waiting.nativeEntered);
    auto changed = exact; changed.intent.words[2] = 0;
    CHECK(Bytes(changed) == Bytes(exact)); // AITX160 deliberately omits intent.
    const auto checks = h.checked.size();
    CHECK(e.Execute(changed).result == Result::Conflict);
    CHECK(e.Retry(changed).result == Result::Conflict);
    ExecutionDisposition disposition{}; CommandState state{};
    CHECK(!e.GetDisposition(changed, disposition) && !e.Find(changed, state));
    CHECK(e.GetDisposition(exact, disposition) && Same(disposition, waiting));
    CHECK(e.Find(exact, state) && state.phase == Phase::Submitted);
    CHECK(h.checked.size() == checks && h.invoked.empty() && e.RecordCount() == 2);
    Record read{};
    CHECK(e.ReadOperation(exact.globalSeq, read, disposition) && read.intent == exact.intent);
    read.intent = {}; // A read is an owned copy, never the retained evidence.
    CHECK(e.Execute(exact).duplicate && h.checked.size() == checks);
    // Retry freezes its caller input even if the dependency callback edits it.
    h.externalRecord = &submit; h.dependency = Dependency::Ready;
    h.onCheck = [](Harness& harness, const Record& r) {
        CHECK(r.intent.words[2] == 0x80000000U);
        harness.externalRecord->intent = {};
    };
    h.onInvoke = [](Harness&, const Record& r) {
        CHECK(r.intent.words[0] == 8 && r.intent.words[2] == 0x80000000U);
    };
    const auto finished = e.Retry(submit);
    CHECK(finished.status == ExecutionStatus::NativeFinished && finished.nativeEntered && h.invoked.size() == 1);
    CHECK(e.ReadOperation(exact.globalSeq, read, disposition) && read.intent == exact.intent);
    Duplicate(e, h, exact, finished);
    h.onCheck = nullptr; h.onInvoke = nullptr;
    auto complete = Next(exact, Op::Complete); complete.intent = {};
    Accepted(e, complete, false);
    auto release = Next(complete, Op::Release); Accepted(e, release, true);
    CHECK(e.ReadOperation(release.globalSeq, read, disposition) && read.intent == release.intent);
    CHECK(!e.HasNativeOwnership(begin.owner));

    // Synchronous invocation finishing does not relax the default ownership
    // barrier. Complete is metadata; successful Release settles bookkeeping.
    Harness strictHarness; TransactionExecutor strict(strictHarness.Adapter());
    CHECK(strict.Configure(begin.scope) == Result::Ok); Accepted(strict, begin, false);
    Accepted(strict, exact, true);
    auto scope = begin.scope; ++scope.consumerEpoch;
    CHECK(strict.Configure(scope) == Result::OwnerBusy);
    Accepted(strict, complete, false);
    CHECK(strict.Configure(scope) == Result::OwnerBusy);
    Accepted(strict, release, true); CHECK(strict.Configure(scope) == Result::Ok);
}

void NativeIntentOrdering() {
    // Terminals can arrive before the constructor is ready. None may erase its
    // invocation or allow another command's scalar cancel to overtake it.
    for (const auto terminal : {Op::Complete, Op::Cancel, Op::Stop}) {
        Harness h; TransactionExecutor e(h.Adapter(), {}, true);
        auto begin = Base(); begin.nativeKind = 0x200U; begin.admission = 1; begin.intent.words[0] = 1;
        CHECK(e.Configure(begin.scope) == Result::Ok); Accepted(e, begin, false);
        auto submit = Next(begin, Op::Submit); h.dependency = Dependency::Waiting;
        CHECK(e.Execute(submit).status == ExecutionStatus::Waiting);
        auto finish = Next(submit, terminal); finish.intent = {};
        if (terminal == Op::Complete) Accepted(e, finish, false);
        else CHECK(e.Execute(finish).status == ExecutionStatus::Waiting);
        auto release = Next(finish, Op::Release);
        CHECK(e.Execute(release).status == ExecutionStatus::Waiting);
        auto move = Next(release, Op::Begin); ++move.commandId; move.nativeKind = 4;
        Accepted(e, move, false); auto moveSubmit = Next(move, Op::Submit); moveSubmit.admission = 0;
        CHECK(e.Execute(moveSubmit).status == ExecutionStatus::Waiting);
        auto cancel = Next(moveSubmit, Op::Cancel);
        CHECK(e.Execute(cancel).status == ExecutionStatus::Waiting);
        ExecutionDisposition disposition{};
        CHECK(e.GetDisposition(submit, disposition) && disposition.status == ExecutionStatus::Waiting);
        h.dependency = Dependency::Ready;
        CHECK(e.Retry(release).status == ExecutionStatus::Waiting);
        CHECK(e.Retry(cancel).status == ExecutionStatus::Waiting && h.invoked.empty());
        CHECK(e.Retry(submit).nativeEntered && h.invoked[0] == Bytes(submit));
        if (terminal != Op::Complete) CHECK(e.Retry(finish).nativeEntered);
        CHECK(e.Retry(release).nativeEntered && e.Retry(cancel).nativeEntered);
        CHECK(e.Retry(submit).duplicate);
    }
    // Keep direct native calls in order across command boundaries, in both
    // lifecycle/constructor directions and constructor/constructor order.
    for (const auto firstKind : {0x100U, 0x101U, 0x102U, 0x103U, 0x104U, 0x105U, 0x200U}) {
        for (const auto secondKind : {0x100U, 0x200U}) {
            Harness h; TransactionExecutor e(h.Adapter(), {}, true);
            auto first = Base(); first.nativeKind = firstKind; first.admission = 1;
            if (firstKind == 0x200U) first.intent.words[0] = 1;
            CHECK(e.Configure(first.scope) == Result::Ok); Accepted(e, first, false);
            first = Next(first, Op::Submit); h.dependency = Dependency::Waiting;
            CHECK(e.Execute(first).status == ExecutionStatus::Waiting);
            auto second = Next(first, Op::Begin); ++second.commandId; second.nativeKind = secondKind;
            second.intent = {}; if (secondKind == 0x200U) second.intent.words[0] = 2;
            Accepted(e, second, false); second = Next(second, Op::Submit);
            CHECK(e.Execute(second).status == ExecutionStatus::Waiting);
            h.dependency = Dependency::Ready;
            CHECK(e.Retry(second).status == ExecutionStatus::Waiting && h.invoked.empty());
            CHECK(e.Retry(first).nativeEntered && e.Retry(second).nativeEntered);
            CHECK(h.invoked.size() == 2 && h.invoked[0] == Bytes(first) && h.invoked[1] == Bytes(second));
        }
    }
    // Constructor bypass is limited to scalar requests. A kind5 hold may need
    // the constructor to exist; retain it and retry it after that invocation.
    for (const auto scalarKind : {4U, 5U}) {
        Harness h; TransactionExecutor e(h.Adapter(), {}, true);
        auto scalar = Base(); scalar.nativeKind = scalarKind; scalar.admission = 1;
        CHECK(e.Configure(scalar.scope) == Result::Ok); Accepted(e, scalar, false);
        scalar = Next(scalar, Op::Submit); h.dependency = Dependency::Waiting;
        CHECK(e.Execute(scalar).status == ExecutionStatus::Waiting);
        auto constructor = Next(scalar, Op::Begin); ++constructor.commandId;
        constructor.nativeKind = 0x200U; constructor.intent.words[0] = 1;
        Accepted(e, constructor, false); constructor = Next(constructor, Op::Submit);
        h.dependency = Dependency::Ready; Accepted(e, constructor, true);
        CHECK(e.Retry(scalar).nativeEntered && h.invoked.size() == 2 && h.invoked[0] == Bytes(constructor));
    }
}
void Waiting() {
    Harness h;
    TransactionExecutor e(h.Adapter());
    auto r = Base(); CHECK(e.Configure(r.scope) == Result::Ok);
    Accepted(e, r, false);
    r = Next(r, Op::Submit);
    h.dependency = Dependency::Waiting;
    const auto waiting = e.Execute(r);
    CHECK(waiting.acceptedLogical && !waiting.nativeEntered && !waiting.hasNativeOutcome);
    CHECK(waiting.status == ExecutionStatus::Waiting && e.HasWaiting());
    CHECK(e.FenceOwner(r.owner) == Result::Ok && e.HasWaiting(r.owner));
    const auto checked = h.checked.size();
    CHECK(Same(e.Execute(r), waiting) && h.checked.size() == checked);
    auto tampered = r; tampered.goalZ += 1;
    CHECK(e.Retry(tampered).result == Result::Conflict);
    CHECK(e.Execute(tampered).result == Result::Conflict && e.HasWaiting());
    auto later = Next(r, Op::Update);
    CHECK(e.Execute(later).status == ExecutionStatus::Blocked);
    later = Next(r, Op::Complete);
    CHECK(e.Execute(later).status == ExecutionStatus::Blocked);
    later = Next(r, Op::Stop);
    CHECK(e.Execute(later).status == ExecutionStatus::Blocked);
    auto owner = r.owner; ++owner.ownerEpoch;
    CHECK(e.FenceOwner(owner, ExecutionBarrier::ExternalQuiescence) == Result::OwnerBusy);
    CHECK(e.Configure(r.scope, ExecutionBarrier::ExternalQuiescence) == Result::Ok && e.HasWaiting());
    CHECK(e.Retry(r).status == ExecutionStatus::Waiting);
    CHECK(h.invoked.empty() && h.checked.back() == Bytes(r));
    h.dependency = Dependency::Ready;
    const auto admitted = e.Retry(r);
    CHECK(admitted.nativeEntered && admitted.hasNativeOutcome && !e.HasWaiting());
    CHECK(h.invoked.size() == 1 && h.invoked.back() == Bytes(r));
    Duplicate(e, h, r, admitted);

    // A waiting Update has an admitted Submit and retains its full revision.
    r = Next(r, Op::Update); r.goalX = 91;
    h.dependency = Dependency::Waiting;
    CHECK(e.Execute(r).status == ExecutionStatus::Waiting);
    auto scope = r.scope; ++scope.worldEpoch;
    CHECK(e.Configure(scope) == Result::OwnerBusy);
    auto cancel = Next(r, Op::Cancel);
    auto badCancel = cancel; ++badCancel.commandId;
    CHECK(e.Execute(badCancel).status == ExecutionStatus::Blocked);
    badCancel = cancel; ++badCancel.globalSeq;
    CHECK(e.Execute(badCancel).result == Result::GlobalGap && e.HasWaiting());
    badCancel = cancel; ++badCancel.parameterRevision;
    CHECK(e.Execute(badCancel).result == Result::RevisionMismatch && e.HasWaiting());
    h.dependency = Dependency::Ready;
    Accepted(e, cancel, true);
    CHECK(!e.HasWaiting() && h.invoked.size() == 2);
    const auto superseded = e.Execute(r);
    CHECK(superseded.status == ExecutionStatus::Superseded && !superseded.nativeEntered);
    Duplicate(e, h, r, superseded);
    CHECK(e.Configure(scope, ExecutionBarrier::ExternalQuiescence) == Result::Ok);
    CHECK(e.Retry(r).result == Result::WrongScope);

    // Changed-scope reset of a Submit that never entered calls no cleanup.
    r = Base(); r.scope = scope; h.dependency = Dependency::Waiting;
    Accepted(e, r, false); r = Next(r, Op::Submit);
    CHECK(e.Execute(r).status == ExecutionStatus::Waiting);
    const auto calls = h.invoked.size();
    ++scope.consumerEpoch;
    CHECK(e.Configure(scope) == Result::Ok && !e.HasWaiting());
    CHECK(h.invoked.size() == calls && e.RecordCount() == 0);

    // Waiting cleanup is also exact work: replacement cannot bypass it, and
    // another Cancel cannot supersede a Release after logical termination.
    r = Base(); r.scope = scope; h.dependency = Dependency::Ready;
    Accepted(e, r, false); r = Next(r, Op::Submit); Accepted(e, r, true);
    r = Next(r, Op::Stop); Accepted(e, r, true);
    r = Next(r, Op::Release); h.dependency = Dependency::Waiting;
    CHECK(e.Execute(r).status == ExecutionStatus::Waiting);
    auto replacement = Next(r, Op::Begin); ++replacement.commandId; replacement.parameterRevision = 1;
    CHECK(e.Execute(replacement).status == ExecutionStatus::Blocked);
    CHECK(e.Execute(Next(r, Op::Cancel)).status == ExecutionStatus::Blocked);
    h.dependency = Dependency::Ready;
    CHECK(e.Retry(r).nativeOutcome == NativeOutcome::Admitted && !e.HasWaiting());
    Accepted(e, replacement, false);
}

void IndependentOwners() {
    Harness h; TransactionExecutor e(h.Adapter());
    auto a = Base(); CHECK(e.Configure(a.scope) == Result::Ok);
    Accepted(e, a, false);
    a = Next(a, Op::Submit); h.dependency = Dependency::Waiting;
    CHECK(e.Execute(a).status == ExecutionStatus::Waiting);
    const auto aSubmit = a;
    auto b = Base(); ++b.owner.entityA; b.commandId += 100; b.globalSeq = 3;
    Accepted(e, b, false);
    b = Next(b, Op::Submit);
    CHECK(e.Execute(b).status == ExecutionStatus::Waiting);
    const auto bSubmit = b;
    CHECK(e.WaitingCount() == 2 && e.HasWaiting(a.owner) && e.HasWaiting(b.owner));
    auto c = Base(); c.owner.entityA += 2; c.commandId += 200; c.globalSeq = 5;
    CHECK(e.FenceOwner(c.owner) == Result::Ok);
    Accepted(e, c, false); c = Next(c, Op::Submit);
    h.dependency = Dependency::Ready; Accepted(e, c, true);
    CHECK(e.WaitingCount() == 2 && !e.HasWaiting(c.owner));
    auto blocked = Next(b, Op::Update); blocked.globalSeq = 7;
    CHECK(e.Execute(blocked).status == ExecutionStatus::Blocked);
    CHECK(e.Retry(aSubmit).nativeOutcome == NativeOutcome::Admitted);
    CHECK(e.WaitingCount() == 1 && !e.HasWaiting(a.owner) && e.HasWaiting(b.owner));
    // Retry does not consume a new global sequence. Cancel only supersedes B.
    b = Next(b, Op::Cancel); b.globalSeq = 7; Accepted(e, b, true);
    CHECK(e.WaitingCount() == 0 && e.Retry(bSubmit).status == ExecutionStatus::Superseded);
    CHECK(e.Retry(aSubmit).duplicate && h.invoked.size() == 3);
    a = Next(a, Op::Update); a.globalSeq = 8; h.dependency = Dependency::Waiting;
    CHECK(e.Execute(a).status == ExecutionStatus::Waiting);
    c = Next(c, Op::Update); c.globalSeq = 9;
    CHECK(e.Execute(c).status == ExecutionStatus::Waiting && e.WaitingCount() == 2);
    auto newC = c.owner; ++newC.ownerEpoch;
    CHECK(e.FenceOwner(newC, ExecutionBarrier::ExternalQuiescence) == Result::OwnerBusy);
    h.throwCheck = true;
    CHECK(e.Retry(c).status == ExecutionStatus::CallbackError);
    CHECK(e.WaitingCount() == 1 && e.HasWaiting(a.owner));
    h.throwCheck = false; h.dependency = Dependency::Ready;
    CHECK(e.Retry(a).nativeOutcome == NativeOutcome::Admitted && !e.HasWaiting());
    CHECK(e.Retry(c).status == ExecutionStatus::CallbackError); // No implicit reentry.
    auto changed = a.scope; ++changed.consumerEpoch;
    CHECK(e.Configure(changed) == Result::OwnerBusy); // Live refs of any owner still fence scope.
    CHECK(e.Configure(changed, ExecutionBarrier::ExternalQuiescence) == Result::Ok);
    CHECK(e.WaitingCount() == 0 && e.Retry(a).result == Result::WrongScope);
}

void Failures() {
    for (unsigned int scenario = 0; scenario != 7; ++scenario) {
        Harness h;
        TransactionExecutor e(h.Adapter());
        auto r = Base(); CHECK(e.Configure(r.scope) == Result::Ok);
        Accepted(e, r, false); r = Next(r, Op::Submit);
        if (scenario == 0) h.dependency = Dependency::Reject;
        if (scenario == 1) h.throwCheck = true;
        if (scenario == 2) h.dependency = static_cast<Dependency>(255);
        if (scenario == 3) h.outcome = NativeOutcome::Rejected;
        if (scenario == 4) h.outcome = NativeOutcome::Unknown;
        if (scenario == 5) h.throwInvoke = true;
        if (scenario == 6) h.outcome = static_cast<NativeOutcome>(255);
        const auto result = e.Execute(r);
        CHECK(result.acceptedLogical && result.nativeEntered == (scenario >= 3));
        CHECK(result.hasNativeOutcome == (scenario >= 3));
        if (scenario == 0) CHECK(result.status == ExecutionStatus::DependencyRejected);
        if (scenario == 1 || scenario == 2) CHECK(result.status == ExecutionStatus::CallbackError);
        if (scenario == 3) CHECK(result.nativeOutcome == NativeOutcome::Rejected);
        if (scenario >= 4) CHECK(result.result == Result::OutcomeUnknown && result.nativeOutcome == NativeOutcome::Unknown);
        h.throwCheck = false; h.throwInvoke = false;
        h.dependency = Dependency::Ready; h.outcome = NativeOutcome::Admitted;
        Duplicate(e, h, r, result);
        const auto calls = h.invoked.size();
        auto update = Next(r, Op::Update);
        CHECK(e.Execute(update).result == Result::InvalidTransition);
        CHECK(h.invoked.size() == calls);
        auto cancel = Next(r, Op::Cancel);
        const auto cancellation = e.Execute(cancel);
        if (scenario >= 4) {
            CHECK(cancellation.result == Result::OutcomeUnknown && !cancellation.nativeEntered);
            CHECK(h.invoked.size() == calls);
            CHECK(e.Configure(r.scope, ExecutionBarrier::ExternalQuiescence) == Result::Ok);
            auto scope = r.scope; ++scope.consumerEpoch;
            CHECK(e.Configure(scope) == Result::OwnerBusy);
            auto owner = r.owner; ++owner.ownerEpoch;
            CHECK(e.FenceOwner(owner) == Result::OwnerBusy);
            CHECK(e.Configure(scope, ExecutionBarrier::ExternalQuiescence) == Result::Ok);
        } else {
            CHECK(cancellation.nativeEntered && cancellation.nativeOutcome == NativeOutcome::Admitted);
            auto release = Next(cancel, Op::Release);
            Accepted(e, release, true);
            auto owner = r.owner; ++owner.ownerEpoch;
            CHECK(e.FenceOwner(owner) == Result::Ok);
        }
    }
    Harness h;
    TransactionExecutor e(h.Adapter());
    auto r = Base(); CHECK(e.Configure(r.scope) == Result::Ok);
    Accepted(e, r, false); r = Next(r, Op::Submit);
    h.dependency = Dependency::Waiting;
    CHECK(e.Execute(r).status == ExecutionStatus::Waiting);
    h.throwCheck = true;
    const auto failure = e.Retry(r);
    CHECK(failure.status == ExecutionStatus::CallbackError && !e.HasWaiting());
    h.throwCheck = false; h.dependency = Dependency::Ready;
    Duplicate(e, h, r, failure);

    // Irreversible uncertainty is fenced for every dispatchable operation,
    // including post-admission mutation and terminal resource cleanup.
    for (const auto uncertain : {Op::Submit, Op::Update, Op::Cancel, Op::Stop, Op::Release}) {
        Harness adapter; TransactionExecutor executor(adapter.Adapter());
        auto operation = Base(); CHECK(executor.Configure(operation.scope) == Result::Ok);
        Accepted(executor, operation, false);
        operation = Next(operation, Op::Submit);
        if (uncertain != Op::Submit) {
            Accepted(executor, operation, true);
            if (uncertain == Op::Release) {
                operation = Next(operation, Op::Cancel); Accepted(executor, operation, true);
            }
            operation = Next(operation, uncertain);
        }
        adapter.outcome = NativeOutcome::Unknown;
        const auto unknown = executor.Execute(operation);
        CHECK(unknown.nativeEntered && unknown.hasNativeOutcome && unknown.result == Result::OutcomeUnknown);
        adapter.outcome = NativeOutcome::Admitted;
        Duplicate(executor, adapter, operation, unknown);
        const auto nativeCalls = adapter.invoked.size();
        if (uncertain == Op::Submit || uncertain == Op::Update) {
            const auto next = executor.Execute(Next(operation, Op::Cancel));
            CHECK(next.result == Result::OutcomeUnknown && !next.nativeEntered);
        } else if (uncertain != Op::Release) {
            const auto next = executor.Execute(Next(operation, Op::Release));
            CHECK(next.result == Result::OutcomeUnknown && !next.nativeEntered);
        }
        CHECK(adapter.invoked.size() == nativeCalls);
        auto owner = operation.owner; ++owner.ownerEpoch;
        CHECK(executor.FenceOwner(owner) == Result::OwnerBusy);
        CHECK(executor.FenceOwner(owner, ExecutionBarrier::ExternalQuiescence) == Result::Ok);
        CHECK(executor.Retry(operation).result == Result::StaleOwner);
    }
}

void Reentry(Harness& h, const Record& r) {
    auto& e = *h.executor;
    CHECK(e.InvocationInFlight());
    ExecutionDisposition state{};
    CHECK(e.GetDisposition(r, state) && state.nativeEntered && !state.hasNativeOutcome);
    CHECK(state.status == ExecutionStatus::NativeEntered);
    CommandState command{};
    CHECK(e.Find(r, command));
    auto scope = r.scope; ++scope.consumerEpoch;
    const auto count = e.RecordCount();
    CHECK(e.Configure(scope, ExecutionBarrier::ExternalQuiescence) == Result::NativeAlreadyEntered);
    CHECK(e.Configure(r.scope) == Result::NativeAlreadyEntered);
    auto owner = r.owner; ++owner.ownerEpoch;
    CHECK(e.FenceOwner(owner, ExecutionBarrier::ExternalQuiescence) == Result::NativeAlreadyEntered);
    CHECK(e.FenceOwner(r.owner) == Result::NativeAlreadyEntered);
    CHECK(e.Execute(r).status == ExecutionStatus::Reentrant);
    CHECK(e.Retry(r).status == ExecutionStatus::Reentrant);
    auto next = Next(r, Op::Cancel);
    CHECK(e.Execute(next).status == ExecutionStatus::Reentrant);
    CHECK(e.RecordCount() == count && e.InvocationInFlight());
    // Mutating caller-owned input cannot replace the driver's private token.
    if (h.externalRecord) *h.externalRecord = Next(r, Op::Update);
}
void CallbackReentry() {
    Harness h;
    TransactionExecutor e(h.Adapter()); h.executor = &e;
    auto r = Base(); CHECK(e.Configure(r.scope) == Result::Ok);
    Accepted(e, r, false); r = Next(r, Op::Submit);
    const auto original = r;
    h.externalRecord = &r;
    h.onCheck = [](Harness& adapter, const Record& token) {
        auto& executor = *adapter.executor;
        CHECK(!executor.InvocationInFlight());
        CHECK(executor.Execute(token).status == ExecutionStatus::Reentrant);
        CHECK(executor.Retry(token).status == ExecutionStatus::Reentrant);
        auto scope = token.scope; ++scope.worldEpoch;
        CHECK(executor.Configure(scope, ExecutionBarrier::ExternalQuiescence) == Result::NativeAlreadyEntered);
        CHECK(executor.FenceOwner(token.owner) == Result::NativeAlreadyEntered);
        CHECK(executor.Checkpoint(token.scope,token.globalSeq)==Result::NativeAlreadyEntered);
    };
    h.onInvoke = &Reentry;
    const auto result = e.Execute(r);
    CHECK(result.nativeOutcome == NativeOutcome::Admitted && result.result == Result::Ok);
    CHECK(!e.InvocationInFlight() && r.opcode == Op::Update);
    CHECK(h.invoked.size() == 1 && h.invoked[0] == Bytes(original));
    CommandState state{};
    CHECK(e.Find(original, state) && state.nativeAdmitted && state.parameterRevision == 1);
    h.externalRecord = nullptr;
    Duplicate(e, h, original, result);
    h.onInvoke = nullptr; h.onCheck = nullptr;
    Accepted(e, r, true);
    CHECK(h.invoked.size() == 2 && h.invoked.back() == Bytes(r));
}

void ValidationAndOrdering() {
    Harness h;
    TransactionExecutor e(h.Adapter());
    auto r = Base();
    CHECK(e.Execute(r).result == Result::WrongScope);
    CHECK(e.Configure(r.scope) == Result::Ok);
    auto bad = r; bad.flags |= ObservationOnly;
    CHECK(e.Execute(bad).result == Result::ObservationRejected);
    bad = r; bad.flags |= Incomplete;
    CHECK(e.Execute(bad).result == Result::ObservationRejected);
    bad = r; bad.goalX = std::numeric_limits<float>::infinity();
    CHECK(e.Execute(bad).result == Result::InvalidRecord);
    bad = r; bad.globalSeq = 2;
    CHECK(e.Execute(bad).result == Result::GlobalGap);
    bad = r; bad.ownerSeq = 2;
    CHECK(e.Execute(bad).result == Result::OwnerGap);
    CHECK(e.RecordCount() == 0 && h.invoked.empty());
    Accepted(e, r, false);
    r = Next(r, Op::Submit);
    const auto accepted = Accepted(e, r, true);
    // Every canonical payload byte is covered by duplicate equality, including
    // diagnostics and signed zero, which float equality would incorrectly merge.
    const auto canonical = Bytes(r);
    for (std::size_t offset = 8; offset < canonical.size(); ++offset) {
        auto bytes = canonical; bytes[offset] ^= 1;
        Record changed{};
        if (Decode(bytes, changed) != Result::Ok) continue;
        const auto count = e.RecordCount();
        const auto result = e.Execute(changed);
        CHECK(!result.acceptedLogical && e.RecordCount() == count);
        CHECK(h.invoked.size() == 1);
    }
    auto zero = r; zero.goalX = 0.0F;
    Harness z; TransactionExecutor zeroExecutor(z.Adapter());
    auto begin = Base(); CHECK(zeroExecutor.Configure(begin.scope) == Result::Ok);
    Accepted(zeroExecutor, begin, false); Accepted(zeroExecutor, zero, true);
    zero.goalX = std::bit_cast<float>(0x80000000U);
    CHECK(zeroExecutor.Execute(zero).result == Result::Conflict);
    Duplicate(e, h, r, accepted);
    auto oldOwner = r.owner;
    auto owner = r.owner; ++owner.ownerEpoch;
    CHECK(e.FenceOwner(owner, ExecutionBarrier::ExternalQuiescence) == Result::Ok);
    CHECK(e.Execute(r).result == Result::StaleOwner && e.Retry(r).result == Result::StaleOwner);
    CHECK(e.FenceOwner(oldOwner) == Result::StaleOwner);
    auto newer = Base(); newer.owner = owner; newer.globalSeq = 3;
    Accepted(e, newer, false);
    CHECK(e.RecordCount() == 3 && h.invoked.size() == 1);
    auto scope = newer.scope; --scope.worldEpoch;
    CHECK(e.Configure(scope, ExecutionBarrier::ExternalQuiescence) == Result::WrongScope);
    CHECK(e.RecordCount() == 3);
}

void MissingAdapterAndTerminals() {
    for (unsigned int missing = 0; missing != 3; ++missing) {
        Harness h;
        auto adapter = h.Adapter();
        if (missing != 1) adapter.checkDependencies = nullptr;
        if (missing != 0) adapter.invoke = nullptr;
        TransactionExecutor e(adapter);
        auto r = Base(); CHECK(e.Configure(r.scope) == Result::Ok);
        Accepted(e, r, false); r = Next(r, Op::Update);
        Accepted(e, r, false); r = Next(r, Op::Submit);
        const auto refused = e.Execute(r);
        CHECK(refused.status == ExecutionStatus::AdapterUnavailable && !refused.acceptedLogical);
        CHECK(e.RecordCount() == 2 && h.checked.empty() && h.invoked.empty());
    }
    for (const auto terminal : {Op::Cancel, Op::Stop}) {
        Harness h; TransactionExecutor e(h.Adapter());
        auto r = Base(); CHECK(e.Configure(r.scope) == Result::Ok);
        Accepted(e, r, false);
        r = Next(r, terminal); Accepted(e, r, true);
        CHECK(h.invoked.back() == Bytes(r));
        r = Next(r, Op::Release); Accepted(e, r, true);
        CHECK(h.invoked.size() == 2 && h.invoked.back() == Bytes(r));
    }
}

void Capacity() {
    const auto max = (std::numeric_limits<std::size_t>::max)();
    for (const Limits limits : {Limits{0, 1, 1, 4096}, Limits{1, 0, 1, 4096},
                               Limits{1, 1, 0, 4096}, Limits{100, 10, 5, 1},
                               Limits{max, max, max, max}, Limits{2, max, 2, 4096}}) {
        Harness h; TransactionExecutor e(h.Adapter(), limits);
        CHECK(e.CoverageLost() && e.AllocatedBytes() <= limits.maxBytes);
        CHECK(e.Configure(Base().scope) == Result::CoverageLost);
    }
    // Record exhaustion closes the ledger without dropping the pending Submit.
    Harness h; TransactionExecutor e(h.Adapter(), Limits{2, 1, 1, 4096});
    auto r = Base(); CHECK(e.Configure(r.scope) == Result::Ok);
    Accepted(e, r, false); r = Next(r, Op::Submit);
    h.dependency = Dependency::Waiting;
    CHECK(e.Execute(r).status == ExecutionStatus::Waiting);
    auto cancel = Next(r, Op::Cancel);
    CHECK(e.Execute(cancel).result == Result::CoverageLost && e.HasWaiting());
    CHECK(e.RecordCount() == 2 && h.invoked.empty() && e.CoverageLost());
    const auto checks = h.checked.size();
    CHECK(e.Execute(r).duplicate && e.Execute(r).status == ExecutionStatus::Waiting);
    CHECK(h.checked.size() == checks && h.invoked.empty());
    CHECK(e.Retry(r).result == Result::CoverageLost);
    CHECK(e.Configure(r.scope) == Result::CoverageLost);
    auto scope = r.scope; ++scope.consumerEpoch;
    CHECK(e.Configure(scope) == Result::Ok && !e.HasWaiting());
    CHECK(!e.CoverageLost() && e.RecordCount() == 0);
    Harness admittedAdapter;
    TransactionExecutor admitted(admittedAdapter.Adapter(), Limits{2, 1, 1, 4096});
    auto admittedRecord = Base(); CHECK(admitted.Configure(admittedRecord.scope) == Result::Ok);
    Accepted(admitted, admittedRecord, false); admittedRecord = Next(admittedRecord, Op::Submit);
    const auto nativeResult = Accepted(admitted, admittedRecord, true);
    CHECK(admitted.Execute(Next(admittedRecord, Op::Cancel)).result == Result::CoverageLost);
    Duplicate(admitted, admittedAdapter, admittedRecord, nativeResult);
    // Command tombstones consume capacity even after native release.
    Harness c; TransactionExecutor commands(c.Adapter(), Limits{16, 1, 2, 16384});
    r = Base(); CHECK(commands.Configure(r.scope) == Result::Ok);
    Accepted(commands, r, false); r = Next(r, Op::Cancel); Accepted(commands, r, true);
    r = Next(r, Op::Release); Accepted(commands, r, true);
    r = Next(r, Op::Begin); ++r.commandId; r.parameterRevision = 1;
    CHECK(commands.Execute(r).result == Result::CoverageLost && commands.RecordCount() == 3);
    // Owner slots, including fences, also have a hard cap.
    Harness o; TransactionExecutor owners(o.Adapter(), Limits{16, 4, 1, 16384});
    r = Base(); CHECK(owners.Configure(r.scope) == Result::Ok);
    CHECK(owners.FenceOwner(r.owner) == Result::Ok);
    auto other = r.owner; ++other.entityA;
    CHECK(owners.FenceOwner(other) == Result::CoverageLost && owners.CoverageLost());
    // Exactly the reported budget succeeds; one byte less fails, no resizing.
    Limits sizing{12, 3, 2, 65536};
    Harness s; TransactionExecutor sized(s.Adapter(), sizing);
    sizing.maxBytes = sized.AllocatedBytes();
    TransactionExecutor exact(s.Adapter(), sizing);
    CHECK(!exact.CoverageLost() && exact.AllocatedBytes() == sizing.maxBytes);
    --sizing.maxBytes;
    TransactionExecutor shortBudget(s.Adapter(), sizing);
    CHECK(shortBudget.CoverageLost());
}

std::uint32_t Random(std::uint32_t& state) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
void Generated() {
    Harness h;
    TransactionExecutor e(h.Adapter(), Limits{32, 4, 2, 32768});
    const auto bytes = e.AllocatedBytes();
    std::uint32_t random = 0x7417A52DU;
    for (std::uint64_t trace = 1; trace <= 10000; ++trace) {
        h.checked.clear(); h.invoked.clear();
        h.dependency = Dependency::Ready; h.outcome = NativeOutcome::Admitted;
        h.throwCheck = false; h.throwInvoke = false;
        auto r = Base(); r.scope.consumerEpoch += trace;
        r.commandId += trace; r.owner.entityA += Random(random) % 100;
        CHECK(e.Configure(r.scope, ExecutionBarrier::ExternalQuiescence) == Result::Ok);
        auto disposition = Accepted(e, r, false);
        Duplicate(e, h, r, disposition);
        const auto variant = Random(random) % 10;
        const auto updates = Random(random) % 4;
        for (std::uint32_t i = 0; i < updates; ++i) {
            r = Next(r, Op::Update);
            r.goalX = static_cast<float>(Random(random) % 1000);
            disposition = Accepted(e, r, false); Duplicate(e, h, r, disposition);
        }
        r = Next(r, Op::Submit);
        r.goalY = static_cast<float>(Random(random) % 500);
        auto gap = r; ++gap.globalSeq;
        CHECK(e.Execute(gap).result == Result::GlobalGap);
        gap = r; ++gap.ownerSeq;
        CHECK(e.Execute(gap).result == Result::OwnerGap);
        if (variant == 0 || variant == 1 || variant == 2) h.dependency = Dependency::Waiting;
        if (variant == 3) h.outcome = NativeOutcome::Unknown;
        if (variant == 4) h.outcome = NativeOutcome::Rejected;
        if (variant == 5) h.throwInvoke = true;
        if (variant == 6) h.dependency = Dependency::Reject;
        disposition = e.Execute(r);
        CHECK(disposition.acceptedLogical);
        if (variant <= 2) {
            CHECK(disposition.status == ExecutionStatus::Waiting && h.invoked.empty());
            auto blocked = Next(r, Op::Update);
            CHECK(e.Execute(blocked).status == ExecutionStatus::Blocked);
            CHECK(e.Execute(r).duplicate && h.checked.size() == 1);
            auto changed = r; changed.nativeStatus ^= 0x10;
            CHECK(e.Retry(changed).result == Result::Conflict);
            if (variant == 0) {
                h.dependency = Dependency::Ready;
                disposition = e.Retry(r);
                CHECK(disposition.nativeEntered && h.invoked.size() == 1);
            } else if (variant == 1) {
                const auto pending = r;
                r = Next(r, Op::Cancel); h.dependency = Dependency::Ready;
                disposition = Accepted(e, r, true);
                CHECK(e.Retry(pending).status == ExecutionStatus::Superseded && h.invoked.size() == 1);
            } else {
                auto scope = r.scope; ++scope.consumerEpoch;
                CHECK(e.Configure(scope) == Result::Ok);
                CHECK(e.Retry(r).result == Result::WrongScope && h.invoked.empty());
                CHECK(e.AllocatedBytes() == bytes); continue;
            }
        }
        h.throwInvoke = false; h.dependency = Dependency::Ready; h.outcome = NativeOutcome::Admitted;
        Duplicate(e, h, r, disposition);
        auto changed = r; changed.goalX += 0.25F;
        const auto calls = h.invoked.size();
        CHECK(e.Execute(changed).result == Result::Conflict && h.invoked.size() == calls);
        if (variant == 3 || variant == 5) {
            CHECK(disposition.nativeOutcome == NativeOutcome::Unknown && calls == 1);
            auto cancel = Next(r, Op::Cancel);
            CHECK(e.Execute(cancel).result == Result::OutcomeUnknown && h.invoked.size() == calls);
            auto scope = r.scope; ++scope.worldEpoch;
            CHECK(e.Configure(scope) == Result::OwnerBusy);
        } else {
            if (r.opcode != Op::Cancel) {
                if (variant != 4 && variant != 6) {
                    r = Next(r, Op::Update); Accepted(e, r, true);
                }
                r = Next(r, (Random(random) & 1U) != 0 ? Op::Cancel : Op::Stop);
                Accepted(e, r, true);
            }
            const auto old = r;
            auto replacement = Next(r, Op::Begin); ++replacement.commandId; replacement.parameterRevision = 1;
            CHECK(e.Execute(replacement).result == Result::OwnerBusy);
            r = Next(r, Op::Release); Accepted(e, r, true);
            replacement = Next(r, Op::Begin); ++replacement.commandId; replacement.parameterRevision = 1;
            replacement.parentCommandId = r.commandId;
            Accepted(e, replacement, false);
            auto delayed = Next(replacement, Op::Release); delayed.commandId = old.commandId;
            delayed.parentCommandId = 0; delayed.parameterRevision = old.parameterRevision;
            const auto before = h.invoked.size();
            CHECK(e.Execute(delayed).result == Result::InvalidTransition && h.invoked.size() == before);
        }
        CHECK(e.AllocatedBytes() == bytes && e.AllocatedBytes() <= 32768 && !e.CoverageLost());
    }
}
} // namespace

namespace {
void Checkpoints() {
    Harness h;TransactionExecutor executor(h.Adapter(),Limits{32,4,2,65536},true);
    auto r=Base();const auto scope=r.scope;
    CHECK(executor.Configure(scope)==Result::Ok);
    const auto allocated=executor.AllocatedBytes();const auto oldest=r;
    for(std::uint64_t command=100;command<10100;++command) {
        r.commandId=command;r.opcode=Op::Begin;r.parameterRevision=1;
        Accepted(executor,r,false);
        r=Next(r,Op::Submit);Accepted(executor,r,true);
        // A live native command survives prefix retirement and still updates.
        CHECK(executor.Checkpoint(scope,r.globalSeq)==Result::Ok);
        CHECK(executor.RecordCount()==0&&executor.HasNativeOwnership(r.owner));
        r=Next(r,Op::Update);Accepted(executor,r,true);
        r=Next(r,Op::Complete);Accepted(executor,r,false);
        r=Next(r,Op::Release);Accepted(executor,r,true);
        CHECK(executor.Checkpoint(scope,r.globalSeq)==Result::Ok);
        CHECK(executor.Checkpoint(scope,r.globalSeq)==Result::Ok);
        CHECK(!executor.HasNativeOwnership(r.owner)&&executor.RecordCount()==0);
        const auto calls=h.invoked.size();
        CHECK(!executor.Execute(oldest).acceptedLogical&&h.invoked.size()==calls);
        auto replay=Next(r,Op::Begin);replay.commandId=command;replay.parameterRevision=1;
        CHECK(executor.Execute(replay).result==Result::InvalidTransition);
        CHECK(executor.AllocatedBytes()==allocated&&!executor.CoverageLost());
        ++r.globalSeq;++r.ownerSeq;
    }
    r.commandId=10100;r.opcode=Op::Begin;r.parameterRevision=1;Accepted(executor,r,false);
    r=Next(r,Op::Submit);h.dependency=Dependency::Waiting;
    CHECK(executor.Execute(r).status==ExecutionStatus::Waiting);
    CHECK(executor.Checkpoint(scope,r.globalSeq)==Result::OwnerBusy);
    h.dependency=Dependency::Ready;CHECK(executor.Retry(r).nativeEntered);
    auto wrong=scope;++wrong.consumerEpoch;
    CHECK(executor.Checkpoint(wrong,r.globalSeq)==Result::WrongScope);
    CHECK(executor.Checkpoint(scope,r.globalSeq+1)==Result::InvalidRecord);
    r=Next(r,Op::Update);h.outcome=NativeOutcome::Unknown;
    CHECK(executor.Execute(r).status==ExecutionStatus::OutcomeUnknown);
    CHECK(executor.Checkpoint(scope,r.globalSeq)==Result::OutcomeUnknown);
}
}
int main() {
    static_assert(!std::is_copy_constructible_v<TransactionExecutor>);
    static_assert(noexcept(TransactionExecutor(TransactionAdapter{})));
    AcceptedReplacementFencesLateEdits(); Lifecycle(); ProfileBinding(); NativePipeline(); NativeIntentEvidence(); NativeIntentOrdering();
    Waiting(); IndependentOwners(); Failures(); CallbackReentry();
    ValidationAndOrdering(); MissingAdapterAndTerminals(); Capacity(); Generated(); Checkpoints();
    std::cout << "AISyncTransactionExecutor: 15 groups PASS; 10000 generated traces and 10000 checkpointed commands PASS (offline only)\n";
}
