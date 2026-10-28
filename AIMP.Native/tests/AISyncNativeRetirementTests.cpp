#include "aimp/AISyncNativeRequestAdapter.h"
#include "aimp/AISyncExecutionSession.h"
#include <Windows.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

namespace aimp::ai_sync::execution {
struct NativeRequestAdapterTestAccess {
    static void Prepare(NativeRequestAdapter& adapter, void* context,
        bool(*cancel)(void*,std::uintptr_t) noexcept, bool(*release)(void*,std::uintptr_t) noexcept) {
        // Private synthetic state only. No production image or executable
        // mapping is used, and both native operations are injected below.
        adapter.base_ = 0x10000000;
        adapter.retirementCallsContext_ = context; adapter.retirementCancel_ = cancel; adapter.retirementRelease_ = release;
    }
    static void Seed(NativeRequestAdapter& adapter, std::size_t slot, const transactions::Scope& scope,
        const transactions::OwnerKey& owner, LocalActorBinding actor, std::uintptr_t request, std::uintptr_t control,
        bool cancelled = false, bool unknown = false) {
        auto& lease = adapter.leases_[slot]; lease = {}; lease.used = lease.admitted = true;
        lease.scope = scope; lease.owner = owner; lease.command = slot + 1; lease.actor = actor;
        lease.ref = {request,control}; lease.kind = 4; lease.revision = 1;
        lease.cancelled = cancelled; lease.unknown = unknown;
        lease.refCertified=true;
    }
    static void Kind(NativeRequestAdapter& adapter,std::size_t slot,std::uint32_t kind) {
        auto& lease=adapter.leases_[slot];lease.kind=kind;
        const auto type=adapter.base_+(scheduler_control::Supported(kind)?scheduler_control::kTypeRva:
            kind==5U?0xEB198CU:(kind==7U?0xEB19BCU:(kind==9U?0xEB19ACU:0xEB19CCU)));
        std::memcpy(reinterpret_cast<void*>(lease.ref.object),&type,sizeof(type));
    }
    static void ExecutionCallbacks(NativeRequestAdapter& adapter, Permission permission, ResolveActor resolve, void* context) {
        adapter.permission_ = permission; adapter.resolve_ = resolve; adapter.context_ = context;
    }
    static void Actor(NativeRequestAdapter& adapter,std::size_t slot,LocalActorBinding actor) {adapter.leases_[slot].actor=actor;}
    static void Uncertified(NativeRequestAdapter& adapter,std::size_t slot) {adapter.leases_[slot].refCertified=false;}
    static bool Live(NativeRequestAdapter& adapter,std::size_t slot,const LocalActorBinding& actor) {
        return adapter.Live(adapter.leases_[slot],actor);
    }
};
struct ExecutionSessionTestAccess {
    static NativeRequestAdapter& Native(ExecutionSession& session) { return session.native_; }
    static transactions::TransactionExecutor& Executor(ExecutionSession& session) { return session.executor_; }
    static void ConfigureSynthetic(ExecutionSession& session, const transactions::Scope& scope) {
        session.initialized_ = session.configured_ = true; session.scope_ = scope;
    }
};
}
using namespace aimp::ai_sync;
using namespace transactions;
using namespace execution;
namespace {
void Check(bool value, int line) { if (!value) { std::cerr << "retirement invariant at " << line << '\n'; std::abort(); } }
#define CHECK(value) Check((value),__LINE__)
Scope ScopeValue() { Scope scope{}; scope.session[0] = 5; scope.authorityEpoch = 1; scope.worldEpoch = 2; scope.consumerEpoch = 3; return scope; }
OwnerKey Owner(std::uint32_t id = 1) { return {id,2,1,3,Capability::Movement,1}; }
struct Storage {
    alignas(4) std::array<std::uint8_t,0x4300> actor{};
    alignas(4) std::array<std::uint8_t,0x40> request{};
    alignas(4) std::array<std::uint8_t,0x2C> resource{};
    std::array<LONG,3> requestControl{0,1,1}, resourceControl{0,1,1};
    LocalActorBinding binding{reinterpret_cast<std::uintptr_t>(actor.data()),19,true,1U};
    Storage() {
        std::memcpy(actor.data()+0x50,&binding.runtimeId,4);
        std::memcpy(actor.data()+0x2CD0,&binding.character,4);
        const std::uintptr_t type = 0x10000000 + 0xEB19CC, scheduler = binding.character + 0x2CD0;
        std::memcpy(request.data(),&type,4); std::memcpy(request.data()+0x14,&scheduler,4);
        Resource(true);
    }
    void Resource(bool present) {
        const std::array<std::uintptr_t,2> pair{present ? reinterpret_cast<std::uintptr_t>(resource.data()) : 0,
            present ? reinterpret_cast<std::uintptr_t>(resourceControl.data()) : 0};
        std::memcpy(actor.data()+0x42A8,pair.data(),sizeof(pair));
    }
    void Recycle() { std::uint32_t id = binding.runtimeId + 1; std::memcpy(actor.data()+0x50,&id,4); }
};
struct Harness {
    NativeRequestAdapter adapter;
    std::unique_ptr<Storage[]> storage{std::make_unique<Storage[]>(8)};
    Readiness permission{Readiness::Ready};
    unsigned cancels{}, releases{}, guards{};
    bool cancelOk{true}, releaseOk{true}, changeOnCancel{}, changeOnRelease{}, revokeOnCancel{}, reenter{};
    bool sharedActor{},destroyed{};
    std::uint32_t expectedKind{4};
    ExecutionSession* session{};
    Harness() { NativeRequestAdapterTestAccess::Prepare(adapter,this,&Cancel,&Release); }
    void Seed(std::size_t slot = 0, bool cancelled = false, bool unknown = false, Scope scope = ScopeValue()) {
        auto& data = storage[slot];
        NativeRequestAdapterTestAccess::Seed(Target(),slot,scope,Owner(static_cast<std::uint32_t>(slot+1)),data.binding,
            reinterpret_cast<std::uintptr_t>(data.request.data()),reinterpret_cast<std::uintptr_t>(data.requestControl.data()),cancelled,unknown);
    }
    void SeedShared(std::size_t slot) {
        auto& data=storage[slot];const auto scheduler=storage[0].binding.character+0x2CD0;
        std::memcpy(data.request.data()+0x14,&scheduler,4);
        NativeRequestAdapterTestAccess::Seed(Target(),slot,ScopeValue(),Owner(static_cast<std::uint32_t>(slot+1)),storage[0].binding,
            reinterpret_cast<std::uintptr_t>(data.request.data()),reinterpret_cast<std::uintptr_t>(data.requestControl.data()));
    }
    NativeRequestAdapter& Target() { return session ? ExecutionSessionTestAccess::Native(*session) : adapter; }
    static bool Permit(void*, const Record&) noexcept { return true; }
    static bool Resolve(void* context, const OwnerKey& owner, LocalActorBinding& actor) noexcept {
        auto& h = *static_cast<Harness*>(context);
        if (owner.entityA == 0 || owner.entityA > 8) return false;
        actor = h.storage[owner.entityA-1].binding; return true;
    }
    void Reentry() {
        if (!reenter) return;
        CHECK(!Target().BeginRetirement(ScopeValue()));
        const auto state = Target().DrainRetirement(&Guard,this);
        CHECK(state.reentrant && !state.complete && state.visited == 0);
        CHECK(Target().DrainActorRetirement(storage[0].binding,&Guard,this).reentrant);
        if (session) {
            CHECK(!session->BeginRetirement());
            CHECK(session->DrainRetirement(&Guard,this).reentrant);
            auto next = ScopeValue(); ++next.consumerEpoch;
            CHECK(session->Configure(next,ExecutionBarrier::ExternalQuiescence) == Result::OwnerBusy);
        }
    }
    static Readiness Guard(void* context, const RetirementIdentity& identity) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.guards;
        CHECK(identity.scope == ScopeValue() && identity.owner == Owner(static_cast<std::uint32_t>(identity.command)));
        CHECK(identity.kind == h.expectedKind && identity.revision == 1 && identity.actor.exclusiveExecution);
        return h.permission;
    }
    static bool Cancel(void* context, std::uintptr_t request) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.cancels;
        bool found = false;
        for (std::size_t i = 0; i < 8; ++i) if (reinterpret_cast<std::uintptr_t>(h.storage[i].request.data()) == request) {
            CHECK(h.storage[h.sharedActor?0:i].resourceControl[1] == (h.destroyed?1:2)); found = true;
            if (h.changeOnCancel) h.storage[i].Recycle();
        }
        CHECK(found); h.Reentry();
        if (h.revokeOnCancel) h.permission = Readiness::Waiting;
        return h.cancelOk;
    }
    static bool Release(void* context, std::uintptr_t ref) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.releases;
        auto* pair = reinterpret_cast<std::uintptr_t*>(ref); const auto object = pair[0];
        bool found = false;
        for (std::size_t i = 0; i < 8; ++i) if (reinterpret_cast<std::uintptr_t>(h.storage[i].request.data()) == object) {
            CHECK(h.storage[h.sharedActor?0:i].resourceControl[1] == (h.destroyed?1:2)); found = true;
            if (h.changeOnRelease) h.storage[i].Recycle();
        }
        CHECK(found); h.Reentry();
        if (h.releaseOk) pair[0] = pair[1] = 0;
        return h.releaseOk;
    }
};
void Basic() {
    Harness h; h.Seed();
    CHECK(!h.adapter.DrainRetirement(&Harness::Guard,&h).started && h.cancels == 0);
    CHECK(h.adapter.BeginRetirement(ScopeValue()));
    auto wrong = ScopeValue(); ++wrong.consumerEpoch;
    CHECK(!h.adapter.BeginRetirement(wrong));
    auto state = h.adapter.DrainRetirement(&Harness::Guard,&h,0);
    CHECK(state.started && !state.complete && state.remaining == 1 && h.cancels == 0);
    state = h.adapter.DrainRetirement(&Harness::Guard,&h);
    CHECK(state.complete && state.released == 1 && state.remaining == 0 && h.cancels == 1 && h.releases == 1);
    CHECK(h.storage[0].resourceControl[1] == 1);
    CHECK(h.adapter.DrainRetirement(&Harness::Guard,&h).complete && h.cancels == 1 && h.releases == 1);
    CHECK(h.adapter.BeginRetirement(ScopeValue()));
}
void SupportedKinds() {
    for(const auto kind:{4U,5U,6U,7U,9U,11U}) {
        Harness h;h.Seed();h.expectedKind=kind;
        NativeRequestAdapterTestAccess::Kind(h.adapter,0,kind);
        CHECK(h.adapter.BeginRetirement(ScopeValue()));
        const auto result=h.adapter.DrainRetirement(&Harness::Guard,&h);
        CHECK(result.complete&&result.unknown==0&&h.cancels==1&&h.releases==1);
        CHECK(h.adapter.DrainRetirement(&Harness::Guard,&h).complete&&h.cancels==1&&h.releases==1);
    }
}
void InvalidStatus() {
    Harness h;h.Seed();const std::uint32_t status=7U;
    std::memcpy(h.storage[0].request.data()+0xC,&status,sizeof(status));
    CHECK(h.adapter.BeginRetirement(ScopeValue()));
    const auto result=h.adapter.DrainRetirement(&Harness::Guard,&h);
    CHECK(!result.complete&&result.unknown==1&&h.cancels==0&&h.releases==0);
}
void WaitingAndBudget() {
    Harness h; for (std::size_t i = 0; i < 8; ++i) h.Seed(i);
    CHECK(h.adapter.BeginRetirement(ScopeValue()));
    h.permission = Readiness::Waiting;
    auto state = h.adapter.DrainRetirement(&Harness::Guard,&h,100);
    CHECK(state.visited == 4 && state.remaining == 8 && state.waiting == 8 && h.cancels == 0);
    h.permission = Readiness::Ready;
    for (std::size_t i = 4; i < 8; ++i) h.storage[i].Resource(false);
    state = h.adapter.DrainRetirement(&Harness::Guard,&h);
    CHECK(state.remaining == 8 && h.cancels == 0);
    state = h.adapter.DrainRetirement(&Harness::Guard,&h);
    CHECK(state.released == 4 && state.remaining == 4 && h.cancels == 4);
    for (std::size_t i = 4; i < 8; ++i) h.storage[i].Resource(true);
    state = h.adapter.DrainRetirement(&Harness::Guard,&h);
    CHECK(state.complete && h.cancels == 8 && h.releases == 8);
}
void Faults() {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        Harness h; h.Seed(0,false,scenario == 0);
        if (scenario == 1) h.permission = Readiness::Rejected;
        if (scenario == 2) h.cancelOk = false;
        if (scenario == 3) h.releaseOk = false;
        if (scenario == 4) h.changeOnCancel = true;
        if (scenario == 5) h.revokeOnCancel = true;
        if (scenario == 6) h.storage[0].requestControl[1] = 0;
        if (scenario == 7) h.permission = static_cast<Readiness>(255);
        CHECK(h.adapter.BeginRetirement(ScopeValue()));
        auto state = h.adapter.DrainRetirement(&Harness::Guard,&h);
        CHECK(!state.complete && state.unknown == 1 && state.remaining == 1);
        const auto cancels = h.cancels, releases = h.releases, guards = h.guards;
        h.cancelOk = h.releaseOk = true; h.permission = Readiness::Ready;
        state = h.adapter.DrainRetirement(&Harness::Guard,&h);
        CHECK(state.unknown == 1 && h.cancels == cancels && h.releases == releases && h.guards == guards);
        CHECK(h.storage[0].resourceControl[1] == 1);
    }
    Harness mixed; mixed.Seed(0,false,true); mixed.Seed(1);
    CHECK(mixed.adapter.BeginRetirement(ScopeValue()));
    const auto state = mixed.adapter.DrainRetirement(&Harness::Guard,&mixed);
    CHECK(state.unknown == 1 && state.remaining == 1 && state.released == 1 && mixed.cancels == 1);
}
void IncarnationAndReentry() {
    Harness cancelled; cancelled.Seed(0,true); cancelled.reenter = true;
    CHECK(cancelled.adapter.BeginRetirement(ScopeValue()));
    CHECK(cancelled.adapter.DrainRetirement(&Harness::Guard,&cancelled).complete && cancelled.cancels == 0 && cancelled.releases == 1);
    Harness h; h.Seed(); h.reenter = true; h.changeOnRelease = true;
    CHECK(h.adapter.BeginRetirement(ScopeValue()));
    CHECK(h.adapter.DrainRetirement(&Harness::Guard,&h).complete && h.cancels == 1 && h.releases == 1);
    // A normal RefRelease return consumes our reference even if a destructor
    // invalidates the Character; no post-release pointer is inspected.
    CHECK(h.storage[0].resourceControl[1] == 1);
    Harness foreign; auto old = ScopeValue(); ++old.consumerEpoch; foreign.Seed(0,false,false,old);
    CHECK(foreign.adapter.BeginRetirement(ScopeValue()));
    const auto state = foreign.adapter.DrainRetirement(&Harness::Guard,&foreign);
    CHECK(state.remaining == 1 && state.unknown == 1 && state.visited == 0 && foreign.guards == 0);
}
void SessionBarrier() {
    Harness h; ExecutionSession session; h.session = &session;
    NativeRequestAdapterTestAccess::Prepare(h.Target(),&h,&Harness::Cancel,&Harness::Release);
    ExecutionSessionTestAccess::ConfigureSynthetic(session,ScopeValue());
    auto& executor = ExecutionSessionTestAccess::Executor(session);
    CHECK(executor.Configure(ScopeValue()) == Result::Ok);
    Record operation{}; operation.scope = ScopeValue(); operation.owner = Owner(); operation.commandId = 1;
    operation.globalSeq = operation.ownerSeq = operation.parameterRevision = 1; operation.nativeKind = 4;
    CHECK(executor.Execute(operation).status == ExecutionStatus::Metadata);
    // Exact Waiting is abandoned only behind the retired-scope barrier.
    operation.opcode = Op::Submit; operation.globalSeq = operation.ownerSeq = 2;
    CHECK(executor.Execute(operation).status == ExecutionStatus::Waiting);
    // Native adapter is deliberately not initialized for normal execution, so
    // seed physical ownership independently from the offline logical model.
    h.Seed(); CHECK(!session.OwnerReleased(operation.owner));
    CHECK(session.BeginRetirement() && session.Retired() && !session.RetirementComplete());
    auto next = ScopeValue(); ++next.consumerEpoch;
    CHECK(session.Configure(next,ExecutionBarrier::ExternalQuiescence) == Result::OwnerBusy);
    h.reenter = true;
    const auto progress = session.DrainRetirement(&Harness::Guard,&h);
    CHECK(progress.complete && session.RetirementComplete() && session.OwnerReleased(operation.owner));
    CHECK(session.Configure(ScopeValue(),ExecutionBarrier::ExternalQuiescence) == Result::WrongScope);
    // No Host Release record or synthetic global sequence was introduced.
    CommandState state{}; CHECK(executor.Find(operation,state) && state.phase == Phase::Submitted && !state.nativeAdmitted);
    CHECK(session.Configure(next,ExecutionBarrier::ExternalQuiescence) == Result::Ok && !session.Retired());
    CHECK(!executor.Find(operation,state));
    // A touched command requires an explicit external barrier even after its
    // physical Ref is retired; no Host Release is fabricated in the ledger.
    Harness touched; ExecutionSession second; touched.session = &second;
    NativeRequestAdapterTestAccess::Prepare(touched.Target(),&touched,&Harness::Cancel,&Harness::Release);
    NativeRequestAdapterTestAccess::ExecutionCallbacks(touched.Target(),&Harness::Permit,&Harness::Resolve,&touched);
    ExecutionSessionTestAccess::ConfigureSynthetic(second,ScopeValue());
    auto& logical = ExecutionSessionTestAccess::Executor(second); CHECK(logical.Configure(ScopeValue()) == Result::Ok);
    operation.opcode = Op::Begin; operation.globalSeq = operation.ownerSeq = 1;
    CHECK(logical.Execute(operation).status == ExecutionStatus::Metadata);
    touched.Seed(); operation.opcode = Op::Submit; operation.globalSeq = operation.ownerSeq = 2;
    // Existing synthetic lease makes Submit reject before any native factory.
    const auto disposition = logical.Execute(operation);
    CHECK(disposition.nativeEntered && disposition.nativeOutcome == NativeOutcome::Rejected);
    CHECK(second.BeginRetirement() && second.DrainRetirement(&Harness::Guard,&touched).complete);
    CHECK(second.Configure(next) == Result::OwnerBusy);
    CHECK(second.Configure(next,ExecutionBarrier::ExternalQuiescence) == Result::Ok);
}
void Generated() {
    for (unsigned trace = 0; trace < 1000; ++trace) {
        Harness h; h.Seed(0,(trace & 1) != 0); h.Seed(1,false,(trace % 3) == 0);
        CHECK(h.adapter.BeginRetirement(ScopeValue()));
        h.permission = Readiness::Waiting;
        CHECK(!h.adapter.DrainRetirement(&Harness::Guard,&h).complete && h.cancels == 0);
        h.permission = Readiness::Ready;
        const auto state = h.adapter.DrainRetirement(&Harness::Guard,&h);
        CHECK(state.remaining == ((trace % 3) == 0 ? 1U : 0U));
        const auto releases = h.releases;
        static_cast<void>(h.adapter.DrainRetirement(&Harness::Guard,&h));
        CHECK(h.releases == releases);
    }
}
void ActorBoundary() {
    Harness h;h.sharedActor=true;
    for(std::size_t i=0;i<7;++i)h.SeedShared(i);
    h.Seed(7);CHECK(h.adapter.BeginRetirement(ScopeValue()));
    auto wrong=h.storage[0].binding;++wrong.runtimeId;
    CHECK(h.adapter.DrainActorRetirement(wrong,&Harness::Guard,&h).visited==0&&h.releases==0);
    wrong=h.storage[0].binding;++wrong.creationSerial;
    CHECK(h.adapter.DrainActorRetirement(wrong,&Harness::Guard,&h).visited==0&&h.releases==0);
    CHECK(NativeRequestAdapterTestAccess::Live(h.adapter,0,h.storage[0].binding));
    CHECK(!NativeRequestAdapterTestAccess::Live(h.adapter,0,wrong));
    h.reenter=true;
    const auto swept=h.adapter.DrainActorRetirement(h.storage[0].binding,&Harness::Guard,&h);
    CHECK(swept.visited==7&&swept.released==7&&swept.remaining==1&&!swept.complete);
    CHECK(h.cancels==7&&h.releases==7&&h.storage[0].resourceControl[1]==1);
    CHECK(h.adapter.DrainActorRetirement(h.storage[0].binding,&Harness::Guard,&h).visited==0&&h.releases==7);
    h.sharedActor=false;CHECK(h.adapter.DrainRetirement(&Harness::Guard,&h).complete&&h.releases==8);
    Harness unknown;unknown.Seed(0,false,true);CHECK(unknown.adapter.BeginRetirement(ScopeValue()));
    const auto fenced=unknown.adapter.DrainActorRetirement(unknown.storage[0].binding,&Harness::Guard,&unknown);
    CHECK(fenced.unknown==1&&!fenced.complete&&unknown.cancels==0&&unknown.releases==0);
}
void DestructionReturnProof() {
    // Three retained requests from one dead Character, as in 174337. Its
    // address is intentionally unreadable: no resolver/Cancel/Character read
    // is permitted after the copied destructor-return certificate.
    Harness h;h.destroyed=true;h.permission=Readiness::Rejected;
    const LocalActorBinding dead{0x12340000U,19,true,42U};
    for(std::size_t i=0;i<3;++i) {
        h.Seed(i,false,true);NativeRequestAdapterTestAccess::Actor(h.adapter,i,dead);
        const std::uint32_t status=6;std::memcpy(h.storage[i].request.data()+0xC,&status,4);
    }
    CHECK(h.adapter.BeginRetirement(ScopeValue()));
    auto wrong=dead;++wrong.creationSerial;h.adapter.ConfirmActorDestroyed(wrong);
    CHECK(h.adapter.DrainRetirement(&Harness::Guard,&h).remaining==3&&h.releases==0);
    h.adapter.ConfirmActorDestroyed(dead);
    const auto released=h.adapter.DrainRetirement(&Harness::Guard,&h);
    CHECK(released.complete&&released.released==3&&h.releases==3&&h.cancels==0);
    CHECK(h.adapter.DrainRetirement(&Harness::Guard,&h).released==0&&h.releases==3);
    // An old return certificate cannot affect a reused address/new serial.
    h.Seed(0,false,true);auto next=dead;++next.creationSerial;
    NativeRequestAdapterTestAccess::Actor(h.adapter,0,next);h.adapter.ConfirmActorDestroyed(dead);
    CHECK(h.adapter.DrainRetirement(&Harness::Guard,&h).remaining==1&&h.releases==3);
    for(const bool uncertified:{false,true}) {
        Harness bad;bad.destroyed=true;bad.Seed(0,false,true);
        if(uncertified)NativeRequestAdapterTestAccess::Uncertified(bad.adapter,0);
        else {const std::uint32_t active=2;std::memcpy(bad.storage[0].request.data()+0xC,&active,4);}
        CHECK(bad.adapter.BeginRetirement(ScopeValue()));bad.adapter.ConfirmActorDestroyed(bad.storage[0].binding);
        CHECK(!bad.adapter.DrainRetirement(&Harness::Guard,&bad).complete&&bad.releases==0&&bad.cancels==0);
    }
    Harness partial;partial.Seed();partial.releaseOk=false;
    CHECK(partial.adapter.BeginRetirement(ScopeValue()));
    CHECK(partial.adapter.DrainRetirement(&Harness::Guard,&partial).unknown==1&&partial.releases==1);
    partial.adapter.ConfirmActorDestroyed(partial.storage[0].binding);partial.destroyed=true;partial.releaseOk=true;
    CHECK(!partial.adapter.DrainRetirement(&Harness::Guard,&partial).complete&&partial.releases==1);
}
}
int main() {
    Basic(); SupportedKinds(); InvalidStatus(); WaitingAndBudget(); Faults(); IncarnationAndReentry(); SessionBarrier(); Generated(); ActorBoundary(); DestructionReturnProof();
    std::cout << "AISyncNativeRetirement: 10 groups, 1000 generated traces PASS (injected native calls only)\n";
}
