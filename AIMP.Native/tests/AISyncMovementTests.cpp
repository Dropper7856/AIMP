#include "aimp/AISyncExecutionCodec.h"
#include "aimp/AISyncTransactionExecutor.h"
#include "aimp/AISyncNativeRequestAdapter.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace aimp::ai_sync::execution {
struct NativeRequestAdapterTestAccess {
    static void Critical(NativeRequestAdapter& a,const NativeRequestAdapter& source,bool held) {
        if(held) {
            auto actor=source.leases_[0].actor;
            a.leases_.back()={};a.leases_.back().used=a.leases_.back().admitted=true;
            a.leases_.back().actor=actor;a.leases_.back().command=999;a.leases_.back().kind=5;
            a.leases_.back().ref.object=0x12345678U;
        } else a.leases_.back()={};
    }
    static void ContinueNow(NativeRequestAdapter& a) {for(auto& lease:a.leases_)lease.lastGuidanceContinuation=lease.lastGuidanceEdit=0;}
    static bool GoalPending(const NativeRequestAdapter& a) {return a.leases_[0].guidanceGoalPending;}
    static std::array<float,3> ExecutedGoal(const NativeRequestAdapter& a) {return a.leases_[0].executedGoal;}
    static std::uint32_t Revision(const NativeRequestAdapter& a) {return a.leases_[0].revision;}
    static movement::Bundle Desired(const NativeRequestAdapter& a) {return a.leases_[0].movement;}
    static void Clock(NativeRequestAdapter& a,std::uint64_t(*clock)(void*) noexcept) {a.movementClock_=clock;}
    static void RouteQuiet(NativeRequestAdapter& a,bool(*query)(void*,const transactions::Record&) noexcept) {a.movementRouteQuiet_=query;}
    static void Policy(NativeRequestAdapter& a,movement::Navigation policy) {
        for(auto& lease:a.leases_)if(lease.used&&(lease.kind==1||lease.kind==3))lease.movement.navigation=policy;
    }
    static void Prepare(NativeRequestAdapter& a,Permission permission,ResolveActor resolve,void* context,
        bool(*factory)(void*,std::uint32_t,std::uintptr_t,std::uintptr_t*) noexcept,
        bool(*admit)(void*,std::uintptr_t,std::uint32_t,bool&) noexcept,
        bool(*goal)(void*,std::uintptr_t,const float*,bool) noexcept,
        bool(*stop)(void*,std::uintptr_t,float,std::uint8_t) noexcept,
        bool(*cancel)(void*,std::uintptr_t) noexcept,bool(*release)(void*,std::uintptr_t) noexcept) {
        a.base_=0x10000000;a.permission_=permission;a.resolve_=resolve;a.context_=context;
        a.movementCallsContext_=a.retirementCallsContext_=context;
        a.movementFactory_=factory;a.movementAdmission_=admit;a.movementGoal_=goal;a.movementStop_=stop;
        a.retirementCancel_=cancel;a.retirementRelease_=release;
    }
};
}
using namespace aimp::ai_sync;
using namespace transactions;
using namespace execution;
namespace {
void Check(bool value,int line){if(!value){std::cerr<<"movement invariant at "<<line<<'\n';std::abort();}}
#define CHECK(v) Check((v),__LINE__)
Record Move() {
    Record r{};r.scope.session={0x33,0x22,0x11,0x00,0x55,0x44,0x77,0x66,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    r.scope.authorityEpoch=1;r.scope.worldEpoch=2;r.scope.consumerEpoch=3;
    r.owner={11,22,1,2,Capability::Movement,1};r.commandId=100;r.globalSeq=r.ownerSeq=1;r.parameterRevision=1;r.nativeKind=1;
    auto& b=r.movement;b.version=1;b.parts=2;b.gait=3;b.navigation=movement::Navigation::LocalNative;
    b.before={1,2,{1.25F,-2.5F,3.75F},-1,-1,{0,0,0,0,50,.25F,-.5F,8,1.15F},0x21,0x10};
    b.after=b.before;b.after.flags90=1;b.after.flags91=0x11;b.after.stopDistance=.5F;b.companion=b.before;
    b.companion.retained=0;b.companionFlags=2;b.companionRetainedAfter=1;return r;
}
std::vector<std::uint8_t> Fixture(const char* file) {
    std::ifstream in(file);CHECK(in.good());std::string hex((std::istreambuf_iterator<char>(in)),{});
    std::string digits;for(const auto c:hex)if(!std::isspace(static_cast<unsigned char>(c)))digits+=c;
    std::vector<std::uint8_t> bytes;for(std::size_t i=0;i<digits.size();i+=2)bytes.push_back(static_cast<std::uint8_t>(std::stoul(digits.substr(i,2),nullptr,16)));
    return bytes;
}
void Wire(const char* fixture) {
    auto r=Move();Batch batch{};batch.scope=r.scope;batch.batchId=1;batch.count=1;batch.records[0]=r;
    std::vector<std::uint8_t> bytes;CHECK(EncodeBatch(batch,bytes)&&bytes==Fixture(fixture));
    Batch decoded{};CHECK(DecodeBatch(bytes,r.scope,decoded)&&movement::Same(decoded.records[0].movement,r.movement));
    for(std::size_t i=0;i<bytes.size();++i)CHECK(!DecodeBatch(std::span(bytes).first(i),r.scope,decoded));
    Receipt receipt{r.scope,1,r,Stage::Waiting,Result::OwnerBusy};CHECK(EncodeReceipt(receipt,bytes)&&bytes.size()==kHeaderSize+kExtendedRecordSize+8U);
    Receipt echo{};CHECK(DecodeReceipt(bytes,r.scope,echo)&&movement::Same(echo.operation.movement,r.movement));
    for(std::size_t i=0;i<bytes.size();++i)CHECK(!DecodeReceipt(std::span(bytes).first(i),r.scope,echo));
    batch.count=16;for(auto& record:batch.records)record=r;
    CHECK(EncodeBatch(batch,bytes)&&bytes.size()==kBatchHeaderSize+kMaxRecords*kExtendedRecordSize);
    auto bad=r;bad.movement.before.query[5]=std::bit_cast<float>(0x7FC00000U);CHECK(ValidateExecutable(bad)); // Uninitialized native words are copied bit-exact.
    bad=r;bad.movement.before.flags90=0x40;CHECK(!ValidateExecutable(bad));
    bad=r;bad.movement.parts=1;CHECK(!ValidateExecutable(bad));
    bad.movement.companion={};bad.movement.companionFlags=bad.movement.companionRetainedAfter=0;CHECK(ValidateExecutable(bad));
    bad=r;bad.opcode=Op::Release;CHECK(!ValidateExecutable(bad));bad.movement={};CHECK(ValidateExecutable(bad));
}
struct ExecutionHarness {
    movement::Bundle observed{};Dependency readiness{Dependency::Waiting};unsigned enters{};
    std::uint32_t admission{},retention{};
    static Dependency CheckDependencies(void* p,const Record& r){auto& h=*static_cast<ExecutionHarness*>(p);h.observed=r.movement;h.admission=r.admission;h.retention=r.scalarRetention;return h.readiness;}
    static NativeOutcome Invoke(void* p,const Record& r){auto& h=*static_cast<ExecutionHarness*>(p);h.observed=r.movement;h.admission=r.admission;h.retention=r.scalarRetention;++h.enters;return NativeOutcome::Admitted;}
};
void Evidence() {
    auto r=Move();r.admission=2;ExecutionHarness h;TransactionExecutor e({&h,&ExecutionHarness::CheckDependencies,&ExecutionHarness::Invoke});
    CHECK(e.Configure(r.scope)==Result::Ok&&e.Execute(r).acceptedLogical);
    r.opcode=Op::Submit;++r.globalSeq;++r.ownerSeq;CHECK(e.Execute(r).status==ExecutionStatus::Waiting);
    CHECK(movement::Same(h.observed,r.movement));
    auto changed=r;changed.movement.after.query[5]+=.5F;
    CHECK(e.Execute(changed).result==Result::Conflict&&e.Retry(changed).result==Result::Conflict);
    auto directive=r;directive.admission=1;CHECK(e.Retry(directive).result==Result::Conflict);
    ExecutionDisposition disposition{};CHECK(!e.GetDisposition(changed,disposition));
    h.readiness=Dependency::Ready;CHECK(e.Retry(r).nativeEntered&&h.enters==1&&movement::Same(h.observed,r.movement)&&h.admission==2&&h.retention==3);
    CHECK(e.Execute(r).duplicate&&h.enters==1);
}
struct NativeHarness {
    // Native() owns several independent fixtures at once. The adapter's 128
    // leases grew with guidance state; keep them off the default x86 stack.
    std::unique_ptr<NativeRequestAdapter> adapterStorage{std::make_unique<NativeRequestAdapter>()};
    NativeRequestAdapter& adapter{*adapterStorage};
    alignas(4)std::array<std::uint8_t,0x4300> actor{};
    alignas(4)std::array<std::array<std::uint8_t,0x100>,5> requests{};
    std::array<std::array<LONG,3>,5> controls{{{0,1,1},{0,1,1},{0,1,1},{0,1,1},{0,1,1}}};
    alignas(4)std::array<std::uint8_t,0x198> resource{};std::array<LONG,3> resourceControl{0,1,1};
    LocalActorBinding binding{reinterpret_cast<std::uintptr_t>(actor.data()),19,true};
    Record current{};std::vector<unsigned> calls;bool accepted{true},faultCompanion{},recycleOnAdmission{},reenter{},failCancel{};
    std::vector<std::uintptr_t> cancellations;
    bool wirePermit{true},localOwned{true},distinctMoves{},guidanceAllowed{true},localSteering{};
    bool progressAvailable{true},customProgress{},allowDerivedGoal{},refuseGoal{},discardGoal{};
    std::uint64_t now{100000};
    movement::HostProgress progress{};
    std::array<std::uintptr_t,4> schedulerHead{},schedulerFirst{},retiredHead{},retiredFirst{};
    unsigned retainedDrops{};
    void Head(std::uintptr_t request) {
        schedulerHead[0]=reinterpret_cast<std::uintptr_t>(schedulerFirst.data());
        schedulerFirst[0]=reinterpret_cast<std::uintptr_t>(schedulerHead.data());schedulerFirst[2]=request;
        Write(binding.character+0x2CD4U,reinterpret_cast<std::uintptr_t>(schedulerHead.data()));
        Write(binding.character+0x2CD8U,request?1U:0U);
    }
    void Retained(std::uintptr_t request) {
        retiredHead[0]=reinterpret_cast<std::uintptr_t>(retiredFirst.data());
        retiredFirst[0]=reinterpret_cast<std::uintptr_t>(retiredHead.data());retiredFirst[2]=request;
        Write(binding.character+0x2CE0U,reinterpret_cast<std::uintptr_t>(retiredHead.data()));
        Write(binding.character+0x2CE4U,request?1U:0U);
    }
    unsigned navigationDuringAdmission{},moveFactoryCalls{};
    template<class T>static void Write(std::uintptr_t at,T v){*reinterpret_cast<T*>(at)=v;}
    NativeHarness(){Write(binding.character+0x50,binding.runtimeId);Write(binding.character+0x2CD0,binding.character);
        Write(binding.character+0x42A8,reinterpret_cast<std::uintptr_t>(resource.data()));
        Write(binding.character+0x42AC,reinterpret_cast<std::uintptr_t>(resourceControl.data()));
        NativeRequestAdapterTestAccess::Prepare(adapter,&Permit,&Resolve,this,&Factory,&Admit,&Goal,&Stop,&Cancel,&Release);
        SetHostProgressQuery(&Progress,this);NativeRequestAdapterTestAccess::Clock(adapter,&Clock);}
    static std::uint64_t Clock(void* p) noexcept {return static_cast<NativeHarness*>(p)->now;}
    void Advance(std::uint64_t elapsed=200){now+=elapsed;}
    static bool Progress(void* p,const RetirementIdentity& identity,movement::HostProgress& out) noexcept {
        auto& h=*static_cast<NativeHarness*>(p);
        if(!h.progressAvailable||identity.scope!=h.current.scope||identity.owner!=h.current.owner||
            identity.command!=h.current.commandId)return false;
        out=h.customProgress?h.progress:movement::HostProgress{h.current.movement.after.goal,{},h.current.movement.gait,false};
        if(!h.customProgress) {out.observed.position=out.position;out.observed.valid=true;out.observed.atTip=true;}
        return true;
    }
    static bool Permit(void* p,const Record&)noexcept{return static_cast<NativeHarness*>(p)->wirePermit;}
    static bool Resolve(void* p,const OwnerKey&,LocalActorBinding& b)noexcept{auto& h=*static_cast<NativeHarness*>(p);b=h.binding;return h.localOwned;}
    static bool Factory(void* p,std::uint32_t kind,std::uintptr_t scheduler,std::uintptr_t* ref)noexcept{
        auto& h=*static_cast<NativeHarness*>(p);
        const auto i=kind==1?(h.distinctMoves&&h.moveFactoryCalls++>0?4U:0U):(kind==3?1U:(kind==4?2U:3U));
        h.calls.push_back(kind);CHECK(scheduler==h.binding.character+0x2CD0);
        if(kind==3&&h.faultCompanion)return false;
        ref[0]=reinterpret_cast<std::uintptr_t>(h.requests[i].data());ref[1]=reinterpret_cast<std::uintptr_t>(h.controls[i].data());
        const auto type=kind==1?0xEB1A70U:(kind==3?0xEB1A50U:(kind==4?0xEB19CCU:0xEB19BCU));
        Write(ref[0],0x10000000U+type);Write(ref[0]+0x10,kind);Write(ref[0]+0x14,scheduler);
        Write<std::uint8_t>(ref[0]+0xB8,0);Write<std::uint8_t>(ref[0]+0xB9,0);
        Write(ref[0]+0xC,1U);Write(ref[0]+0x68,3U);Write(ref[0]+0x6C,2U);return true;}
    static bool Admit(void* p,std::uintptr_t request,std::uint32_t admission,bool& result)noexcept{
        auto& h=*static_cast<NativeHarness*>(p);h.calls.push_back(admission==1?13U:(admission==2?12U:11U));CHECK(h.resourceControl[1]==2);
        const auto invocation=CurrentNativeInvocation();
        if(h.localSteering)CHECK(invocation.record==nullptr);
        else CHECK(invocation.record!=nullptr&&invocation.actor==h.binding&&invocation.record->commandId!=0);
        const auto kind=*reinterpret_cast<std::uint32_t*>(request+0x10);
        if((kind==1||kind==3)&&!h.allowDerivedGoal)CHECK(*reinterpret_cast<float*>(request+(kind==3?0x1C:0x28))==
            (kind==1&&h.current.opcode==Op::Update?h.current.movement.after.goal[0]:1.25F));
        if(kind==7)CHECK(*reinterpret_cast<std::uint32_t*>(request+0x1C)==2&&*reinterpret_cast<std::uint8_t*>(request+0x18)==0);
        if(admission==1&&kind==3)CHECK(*reinterpret_cast<std::uint8_t*>(request+0xB8)==2&&*reinterpret_cast<std::uint8_t*>(request+0x18)==0);
        if(h.reenter)CHECK(h.adapter.Check(h.current)==Readiness::Rejected);
        if(kind==1||kind==3) {
            const auto n=reinterpret_cast<std::uintptr_t>(h.resource.data())+0xC4;
            NavigationBinding binding{};
            CHECK(h.adapter.IdentifyNavigation(n,request+(kind==1?0x28:0x1C),binding)==NavigationOwnership::Admitting);
            CHECK(binding.command.command!=0&&binding.command.revision!=0);
            if(h.current.commandId)CHECK(binding.command.command==h.current.commandId&&binding.command.revision==h.current.parameterRevision);
            ++h.navigationDuringAdmission;
        }
        if(h.recycleOnAdmission)Write(h.binding.character+0x50,h.binding.runtimeId+1);
        result=h.accepted;
        // Simulate the 00683700 admission=0 scheduler replacement. The old
        // request remains strongly retained but can no longer navigate.
        if(h.distinctMoves&&admission==0&&result&&request==reinterpret_cast<std::uintptr_t>(h.requests[4].data()))
            Write(reinterpret_cast<std::uintptr_t>(h.requests[0].data())+0xC,4U);
        return true;}
    static bool Goal(void* p,std::uintptr_t request,const float* goal,bool update)noexcept{
        auto& h=*static_cast<NativeHarness*>(p);h.calls.push_back(update?24:23);
        auto& pending=*reinterpret_cast<std::uint8_t*>(request+0xB8);
        const auto stop=*reinterpret_cast<const std::uint8_t*>(request+0xB9);
        // Exact006C7550 is void: a refused edit still returns normally.
        if(update&&(h.refuseGoal||*reinterpret_cast<const std::uint32_t*>(request+0x68)!=3U||
            (pending&0x40U)||(stop&3U)==3U))return true;
        for(std::size_t i=0;i<3;++i)Write(request+(update?0x34:0x28)+i*4,goal[i]);
        if(update)pending|=0x40U;
        return true;}
    static bool Stop(void* p,std::uintptr_t request,float distance,std::uint8_t variant)noexcept{
        static_cast<NativeHarness*>(p)->calls.push_back(25);
        auto& flags=*reinterpret_cast<std::uint8_t*>(request+0xB9);
        if(*reinterpret_cast<const float*>(request+0x58)!=distance||((flags>>5)&1U)!=variant) {
            Write(request+0x58,distance);flags=static_cast<std::uint8_t>((flags&~0x20U)|(variant<<5)|0x40U);
        }
        return true;}
    static bool Cancel(void* p,std::uintptr_t request)noexcept{auto& h=*static_cast<NativeHarness*>(p);h.calls.push_back(30);h.cancellations.push_back(request);if(h.failCancel)return false;
        Write<std::uint8_t>(request+0x18,0U);Write(request+0xC,6U);return true;}
    static bool Release(void* p,std::uintptr_t ref)noexcept{auto& h=*static_cast<NativeHarness*>(p);h.calls.push_back(31);
        auto* pair=reinterpret_cast<std::uintptr_t*>(ref);if(pair[0]&&*reinterpret_cast<std::uint8_t*>(pair[0]+0x18))++h.retainedDrops;
        pair[0]=pair[1]=0;return true;}
    void NativeTick() {
        const auto request=reinterpret_cast<std::uintptr_t>(requests[0].data());
        auto& phase=*reinterpret_cast<std::uint32_t*>(request+0x68);
        auto& pending=*reinterpret_cast<std::uint8_t*>(request+0xB8);
        auto& stop=*reinterpret_cast<std::uint8_t*>(request+0xB9);
        if(phase==1U){phase=3U;return;}
        if(phase!=3U)return;
        // 00465E20 handles dirty stop BEFORE pending goal. This is why the
        // adapter must not submit them as one nominally atomic frame.
        if(stop&0x40U){stop&=static_cast<std::uint8_t>(~0x40U);phase=1U;return;}
        if(pending&0x40U) {
            pending&=static_cast<std::uint8_t>(~0x40U);
            const auto dx=*reinterpret_cast<const float*>(request+0x34)-*reinterpret_cast<const float*>(request+0x28);
            const auto dz=*reinterpret_cast<const float*>(request+0x3C)-*reinterpret_cast<const float*>(request+0x30);
            if(!discardGoal&&dx*dx+dz*dz>=0.1F) {
                for(std::size_t i=0;i<3;++i)Write(request+0x28+i*4,*reinterpret_cast<const float*>(request+0x34+i*4));
                if(*reinterpret_cast<const float*>(request+0x58)>0.0F)phase=1U;
            }
        }
    }
    void Steer(){CHECK(CurrentNativeInvocation().record==nullptr);localSteering=true;
        adapter.SteerMovements(+[](void* p,const RetirementIdentity&) noexcept {return static_cast<NativeHarness*>(p)->guidanceAllowed;},this);
        localSteering=false;CHECK(CurrentNativeInvocation().record==nullptr);}
    NativeOutcome Submit(){current=Move();current.opcode=Op::Submit;CHECK(adapter.Check(current)==Readiness::Ready);return adapter.Invoke(current);}
};
void RejectedRouteRetainsNativeLifetime() {
    NativeHarness h;CHECK(h.Submit()==NativeOutcome::Admitted);
    NativeRequestAdapterTestAccess::Policy(h.adapter,movement::Navigation::HostRoute);
    const auto n=reinterpret_cast<std::uintptr_t>(h.resource.data())+0xC4U;
    const auto main=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    const auto companion=reinterpret_cast<std::uintptr_t>(h.requests[1].data());
    NavigationBinding binding{};
    CHECK(h.adapter.IdentifyNavigation(n,main+0x28U,binding)==NavigationOwnership::Ready);
    auto stale=binding;++stale.command.actor.creationSerial;
    CHECK(!h.adapter.RejectNavigation(stale)&&h.cancellations.empty());
    CHECK(h.adapter.RejectNavigation(binding)&&h.adapter.ActiveCount()==2);
    CHECK((h.cancellations==std::vector<std::uintptr_t>{companion,main}));
    CHECK(std::count(h.calls.begin(),h.calls.end(),31U)==0); // Cancellation is not Ref release.
    CHECK(h.adapter.RejectNavigation(binding)&&h.cancellations.size()==2); // No duplicate native Cancel.
    auto update=h.current;update.opcode=Op::Update;++update.parameterRevision;
    update.movement.navigation=movement::Navigation::HostRoute;
    CHECK(h.adapter.Check(update)==Readiness::Rejected);
    auto release=h.current;release.opcode=Op::Release;release.movement={};
    CHECK(h.adapter.Invoke(release)==NativeOutcome::Admitted&&h.adapter.ActiveCount()==0);
    CHECK(h.cancellations.size()==2&&std::count(h.calls.begin(),h.calls.end(),31U)==2);

    NativeHarness ended;CHECK(ended.Submit()==NativeOutcome::Admitted);
    NativeRequestAdapterTestAccess::Policy(ended.adapter,movement::Navigation::HostRoute);
    auto next=ended.current;next.opcode=Op::Update;++next.parameterRevision;
    next.movement.navigation=movement::Navigation::HostRoute;
    NativeHarness::Write(reinterpret_cast<std::uintptr_t>(ended.requests[0].data())+0xCU,3U);
    const auto before=ended.calls.size();
    CHECK(ended.adapter.Check(next)==Readiness::Rejected&&ended.calls.size()==before);

    NativeHarness unknown;CHECK(unknown.Submit()==NativeOutcome::Admitted);
    NativeRequestAdapterTestAccess::Policy(unknown.adapter,movement::Navigation::HostRoute);
    CHECK(unknown.adapter.IdentifyNavigation(reinterpret_cast<std::uintptr_t>(unknown.resource.data())+0xC4U,
        reinterpret_cast<std::uintptr_t>(unknown.requests[0].data())+0x28U,binding)==NavigationOwnership::Ready);
    unknown.failCancel=true;CHECK(!unknown.adapter.RejectNavigation(binding));
    auto replacement=unknown.current;++replacement.commandId;
    CHECK(unknown.adapter.Check(replacement)==Readiness::Rejected&&unknown.adapter.ActiveCount()==2);

    NativeHarness single;single.current=Move();single.current.opcode=Op::Submit;
    single.current.movement.parts=1;single.current.movement.companion={};
    single.current.movement.companionFlags=single.current.movement.companionRetainedAfter=0;
    CHECK(single.adapter.Check(single.current)==Readiness::Ready&&single.adapter.Invoke(single.current)==NativeOutcome::Admitted);
    NativeRequestAdapterTestAccess::Policy(single.adapter,movement::Navigation::HostRoute);
    const auto singleN=reinterpret_cast<std::uintptr_t>(single.resource.data())+0xC4U;
    const auto singleB=reinterpret_cast<std::uintptr_t>(single.requests[0].data())+0x28U;
    CHECK(single.adapter.IdentifyNavigation(singleN,singleB,binding)==NavigationOwnership::Ready);
    CHECK(single.adapter.RejectNavigation(binding));
    single.current.opcode=Op::Update;++single.current.parameterRevision;
    single.current.movement.navigation=movement::Navigation::HostRoute;
    NativeRequestAdapterTestAccess::RouteQuiet(single.adapter,[](void*,const Record&) noexcept{return false;});
    const auto heldCalls=single.calls.size();
    CHECK(single.adapter.Check(single.current)==Readiness::Waiting&&single.calls.size()==heldCalls);
    CHECK(single.adapter.Invoke(single.current)==NativeOutcome::Rejected&&single.calls.size()==heldCalls);
    NativeRequestAdapterTestAccess::RouteQuiet(single.adapter,[](void*,const Record&) noexcept{return true;});
    CHECK(single.adapter.Check(single.current)==Readiness::Ready);
    CHECK(single.adapter.Invoke(single.current)==NativeOutcome::Admitted);
    CHECK(single.adapter.IdentifyNavigation(singleN,singleB,binding)==NavigationOwnership::Ready&&binding.command.revision==2);
    CHECK(std::count(single.calls.begin(),single.calls.end(),31U)==1);
}
Record GuidedMove(std::uint32_t parts=1) {
    auto r=Move();r.opcode=Op::Submit;r.movement.navigation=movement::Navigation::HostGuided;
    if(parts==1) {r.movement.parts=1;r.movement.companion={};r.movement.companionFlags=r.movement.companionRetainedAfter=0;}
    return r;
}
void HostInputsAreNotReplacedByPhysicalCarrier(std::uint32_t parts) {
    NativeHarness h;h.current=GuidedMove(parts);
    h.current.movement.navigation=movement::Navigation::LocalNative;
    h.current.movement.before.goal=h.current.movement.after.goal={6,0,0};
    h.customProgress=h.allowDerivedGoal=true;
    h.progress={{0.4F,0,0},{0,0,0},1,false};
    h.progress.observed={{0.4F,0,0},0,true,true,true};
    CHECK(h.adapter.Check(h.current)==Readiness::Ready);
    CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    CHECK(*reinterpret_cast<float*>(request+0x28U)==6.0F);
    CHECK(*reinterpret_cast<float*>(request+0x58U)==h.current.movement.after.stopDistance);
    CHECK(*reinterpret_cast<std::uint32_t*>(h.resource.data()+0x194U)==h.current.movement.gait);
    const auto calls=h.calls.size();h.Steer();CHECK(h.calls.size()==calls);
    auto update=h.current;update.opcode=Op::Update;++update.parameterRevision;
    update.movement.after.goal={8,0,0};h.current=update;
    CHECK(h.adapter.Check(update)==Readiness::Ready&&h.adapter.Invoke(update)==NativeOutcome::Admitted);
    CHECK(*reinterpret_cast<float*>(request+0x34U)==8.0F);
    CHECK(NativeRequestAdapterTestAccess::Desired(h.adapter).after.goal==update.movement.after.goal);
    const auto updated=h.calls.size();h.Steer();CHECK(h.calls.size()==updated);
    auto release=update;release.opcode=Op::Release;release.movement={};
    CHECK(h.adapter.Invoke(release)==NativeOutcome::Admitted&&h.adapter.ActiveCount()==0);
}
void ReleaseGuided(NativeHarness& h) {
    auto release=h.current;release.opcode=Op::Release;release.movement={};
    CHECK(h.adapter.Invoke(release)==NativeOutcome::Admitted&&h.adapter.ActiveCount()==0);
    CHECK(CurrentNativeInvocation().record==nullptr);
}
void GuidedInitialTargetWaitsWithoutFactory() {
    NativeHarness h;h.current=GuidedMove();h.allowDerivedGoal=h.customProgress=true;
    h.current.movement.before.goal=h.current.movement.after.goal={12,0,0};
    h.progress={{0,0,0},{1,0,0},1,false};
    CHECK(h.adapter.Check(h.current)==Readiness::Waiting&&h.adapter.LastWaitReason()==12U);
    CHECK(h.calls.empty()&&h.adapter.ActiveCount()==0);
    h.progress.position[0]=0.1F;
    CHECK(h.adapter.Check(h.current)==Readiness::Waiting&&h.calls.empty());
    h.progressAvailable=false;
    CHECK(h.adapter.Check(h.current)==Readiness::Waiting&&h.calls.empty());
    h.progressAvailable=true;h.progress.position[0]=0.5F;h.progress.protectedMotion=true;
    CHECK(h.adapter.Check(h.current)==Readiness::Waiting&&h.calls.empty());
    h.progress.protectedMotion=false;
    CHECK(h.adapter.Check(h.current)==Readiness::Ready&&h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==1);
    ReleaseGuided(h);
}
void GuidedUpdatesOnlyRetainValues() {
    NativeHarness h;h.current=GuidedMove();CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    h.customProgress=true;h.progress={h.current.movement.after.goal,{1,0,0},1,true};
    for(std::uint32_t status=0;status<=6;++status) {
        NativeHarness::Write(request+0xC,status);
        NativeHarness::Write(request+0x68,status%2U?1U:3U);
        auto update=h.current;update.opcode=Op::Update;++update.parameterRevision;
        update.movement.after.goal[0]+=1.0F;update.movement.after.style=1;
        update.movement.after.stopDistance=2.0F;update.movement.after.stoppingDistance=1.0F;
        update.movement.gait=status%4U;h.current=update;
        const auto calls=h.calls.size();const auto memory=h.requests;const auto resource=h.resource;
        CHECK(h.adapter.Check(update)==Readiness::Ready&&h.adapter.Invoke(update)==NativeOutcome::Admitted);
        CHECK(h.calls.size()==calls&&h.requests==memory&&h.resource==resource);
        CHECK(NativeRequestAdapterTestAccess::Revision(h.adapter)==update.parameterRevision);
        CHECK(movement::Same(NativeRequestAdapterTestAccess::Desired(h.adapter),update.movement));
        h.Steer();CHECK(h.calls.size()==calls&&h.requests==memory&&h.resource==resource);
    }
    ReleaseGuided(h);
}
void GuidedUpdateWaitsForRetailWithoutRestart() {
    NativeHarness h;h.current=GuidedMove(2);CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    const auto factories=std::count(h.calls.begin(),h.calls.end(),1U);
    const auto cancellations=std::count(h.calls.begin(),h.calls.end(),30U);
    NativeHarness::Write(request+0x68,1U);
    auto update=h.current;update.opcode=Op::Update;update.parameterRevision=2;
    update.movement.after.goal[0]+=2.0F;update.movement.after.style=1;h.current=update;
    const auto before=h.requests;const auto calls=h.calls.size();
    CHECK(h.adapter.Check(update)==Readiness::Ready&&h.adapter.Invoke(update)==NativeOutcome::Admitted);
    CHECK(h.calls.size()==calls&&h.requests==before); // No parameter writes while planning.
    h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),24U)==0);
    NativeHarness::Write(request+0x68,3U);
    SetExecutionOwnedQuery(+[](std::uintptr_t) noexcept {return true;});
    h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),24U)==0);
    SetExecutionOwnedQuery(nullptr);
    h.localOwned=false;h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),24U)==0);
    h.localOwned=true;h.wirePermit=false;h.guidanceAllowed=false;h.Steer();
    CHECK(std::count(h.calls.begin(),h.calls.end(),24U)==0);
    h.guidanceAllowed=true;h.Steer();
    CHECK(std::count(h.calls.begin(),h.calls.end(),24U)==1);
    CHECK(*reinterpret_cast<float*>(request+0x34)==update.movement.after.goal[0]);
    CHECK(NativeRequestAdapterTestAccess::GoalPending(h.adapter));
    CHECK(NativeRequestAdapterTestAccess::ExecutedGoal(h.adapter)==update.movement.before.goal);
    CHECK(*reinterpret_cast<std::uint32_t*>(request+0x78)!=update.movement.after.style);
    h.Advance();h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),24U)==1);
    h.NativeTick();h.Steer(); // XYZ copied, but positive stop distance replans.
    CHECK(NativeRequestAdapterTestAccess::GoalPending(h.adapter));
    h.NativeTick();h.Steer();
    CHECK(!NativeRequestAdapterTestAccess::GoalPending(h.adapter));
    CHECK(NativeRequestAdapterTestAccess::ExecutedGoal(h.adapter)==update.movement.after.goal);
    CHECK(*reinterpret_cast<std::uint32_t*>(request+0x78)==update.movement.after.style);
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==factories&&std::count(h.calls.begin(),h.calls.end(),30U)==cancellations);
    h.wirePermit=true;ReleaseGuided(h);
}
void GuidedGoalConsumptionPrecedesStopAndGait() {
    NativeHarness h;h.current=GuidedMove();CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    const auto oldGoal=NativeRequestAdapterTestAccess::ExecutedGoal(h.adapter);
    const auto oldStop=*reinterpret_cast<float*>(request+0x58);
    auto update=h.current;update.opcode=Op::Update;++update.parameterRevision;
    update.movement.after.goal[0]+=3.0F;h.current=update;
    h.customProgress=true;h.progress={update.movement.after.goal,{1,0,0},1,false};
    h.progress.observed.position=h.progress.position;h.progress.observed.valid=h.progress.observed.atTip=true;
    NativeHarness::Write<std::uint8_t>(request+0xB9,2U); // Native arrival, producer stop clear.
    CHECK(h.adapter.Invoke(update)==NativeOutcome::Admitted);
    const auto stops=std::count(h.calls.begin(),h.calls.end(),25U);
    h.Steer();
    CHECK(NativeRequestAdapterTestAccess::GoalPending(h.adapter));
    CHECK(NativeRequestAdapterTestAccess::ExecutedGoal(h.adapter)==oldGoal);
    CHECK(*reinterpret_cast<float*>(request+0x58)==oldStop&&std::count(h.calls.begin(),h.calls.end(),25U)==stops);
    CHECK(*reinterpret_cast<std::uint32_t*>(h.resource.data()+0x194)==3U);
    CHECK((h.requests[0][0xB9]&3U)==2U); // No Host bit0 write before the setter.
    h.NativeTick();h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),25U)==stops);
    h.NativeTick();h.Steer();
    CHECK(!NativeRequestAdapterTestAccess::GoalPending(h.adapter));
    CHECK(NativeRequestAdapterTestAccess::ExecutedGoal(h.adapter)==update.movement.after.goal);
    CHECK(*reinterpret_cast<float*>(request+0x58)==-1.0F&&std::count(h.calls.begin(),h.calls.end(),25U)==stops+1);
    CHECK(*reinterpret_cast<std::uint32_t*>(h.resource.data()+0x194)==1U);
    CHECK((h.requests[0][0xB9]&3U)==2U); // Through mode preserves the native latch without closing it.
    h.NativeTick();CHECK(*reinterpret_cast<std::uint32_t*>(request+0x68)==1U);
    CHECK(*reinterpret_cast<float*>(request+0x28)==update.movement.after.goal[0]); // Stop replans the NEW goal.
    ReleaseGuided(h);
}
void GuidedRejectedAndDiscardedGoalsRemainPendingIntent() {
    NativeHarness h;h.current=GuidedMove();CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    const auto oldGoal=NativeRequestAdapterTestAccess::ExecutedGoal(h.adapter);
    auto update=h.current;update.opcode=Op::Update;++update.parameterRevision;
    update.movement.after.goal[0]+=2.0F;h.current=update;
    CHECK(h.adapter.Invoke(update)==NativeOutcome::Admitted);
    h.refuseGoal=true;h.Steer();
    CHECK(!NativeRequestAdapterTestAccess::GoalPending(h.adapter)&&NativeRequestAdapterTestAccess::ExecutedGoal(h.adapter)==oldGoal);
    CHECK(h.adapter.Inspect(update).unknown==false);
    h.refuseGoal=false;h.Advance();h.Steer();CHECK(NativeRequestAdapterTestAccess::GoalPending(h.adapter));
    h.discardGoal=true;h.NativeTick();h.Steer();
    CHECK(!NativeRequestAdapterTestAccess::GoalPending(h.adapter)&&NativeRequestAdapterTestAccess::ExecutedGoal(h.adapter)==oldGoal);
    const auto deposits=std::count(h.calls.begin(),h.calls.end(),24U);
    h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),24U)==deposits); // Bounded retry, no loop in one frame.
    h.discardGoal=false;h.Advance();h.Steer();
    h.NativeTick();h.NativeTick();h.Steer();
    CHECK(NativeRequestAdapterTestAccess::ExecutedGoal(h.adapter)==update.movement.after.goal);
    // A small final edit coalesces to the actual goal; it must not wait for a
    // native XYZ copy that 0044D320 deliberately omits.
    ++update.parameterRevision;update.movement.after.goal[0]+=0.1F;update.movement.after.retained=0;h.current=update;
    CHECK(h.adapter.Invoke(update)==NativeOutcome::Admitted);h.Advance();h.Steer();
    CHECK(!NativeRequestAdapterTestAccess::GoalPending(h.adapter));
    CHECK(*reinterpret_cast<std::uint8_t*>(request+0x18)==0U);
    CHECK(*reinterpret_cast<float*>(request+0x28)!=update.movement.after.goal[0]);
    // Same XZ on another floor is NOT a coalescible target.
    ++update.parameterRevision;update.movement.after.goal[1]+=4.0F;update.movement.after.retained=1;h.current=update;
    CHECK(h.adapter.Invoke(update)==NativeOutcome::Admitted);const auto calls=h.calls.size();h.Advance();h.Steer();
    CHECK(h.calls.size()==calls&&*reinterpret_cast<std::uint8_t*>(request+0x18)==0U);
    ReleaseGuided(h);
}
void GuidedContinuationIsBoundedAndAppended(std::uint32_t parts=1) {
    NativeHarness h;h.current=GuidedMove(parts);CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    const auto initial=std::count(h.calls.begin(),h.calls.end(),1U);
    for(int attempt=0;attempt<7;++attempt) {
        NativeHarness::Write(request+0xC,3U);h.Advance(4000);h.Steer();
        CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial+attempt+1);
        NativeHarness::Write(request+0xC,3U);h.Steer();
        CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial+attempt+1); // No same-frame retry.
    }
    CHECK(std::count(h.calls.begin(),h.calls.end(),11U)==1); // Only the original Host replacement.
    CHECK(std::count(h.calls.begin(),h.calls.end(),13U)==7+(parts==2?8:0));
    CHECK(h.adapter.ActiveCount()==parts&&h.retainedDrops==0U);
    h.guidanceAllowed=false;h.Advance(4000);h.Steer();
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial+7);
    ReleaseGuided(h);
}
void RetainedArrivalContinuesWithoutNewHostRevision(std::uint32_t parts) {
    NativeHarness h;h.current=GuidedMove(parts);CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    const auto initial=std::count(h.calls.begin(),h.calls.end(),1U);
    NativeHarness::Write(request+0xC,2U);NativeHarness::Write<std::uint8_t>(request+0xB9,3U);
    NativeHarness::Write(request+0x68,1U);h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial);
    NativeHarness::Write(request+0x68,3U);NativeHarness::Write<std::uint8_t>(request+0xB8,0x40U);
    h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial);
    NativeHarness::Write<std::uint8_t>(request+0xB8,0U);h.guidanceAllowed=false;
    h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial);h.guidanceAllowed=true;
    for(unsigned attempt=0;attempt<5;++attempt) {
        NativeHarness::Write(request+0xC,2U);NativeHarness::Write<std::uint8_t>(request+0xB9,3U);
        if(parts==2&&attempt==0)h.Head(reinterpret_cast<std::uintptr_t>(h.requests[1].data()));
        h.Advance(4000);h.Steer();h.Head(0);
    }
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial+5);
    CHECK(std::count(h.calls.begin(),h.calls.end(),30U)==5*static_cast<int>(parts));
    CHECK(std::count(h.calls.begin(),h.calls.end(),31U)==5*static_cast<int>(parts));
    CHECK(std::count(h.calls.begin(),h.calls.end(),11U)==1);
    CHECK(std::count(h.calls.begin(),h.calls.end(),13U)==5+(parts==2?6:0));
    // A future native action cannot be silently moved in front of an appended
    // continuation. Preserve its queue position until the Host handoff.
    NativeHarness::Write(request+0xC,2U);NativeHarness::Write<std::uint8_t>(request+0xB9,3U);
    NativeRequestAdapterTestAccess::Critical(h.adapter,h.adapter,true);h.Head(request);h.Advance(4000);
    const auto calls=h.calls.size();h.Steer();CHECK(h.calls.size()==calls);
    NativeRequestAdapterTestAccess::Critical(h.adapter,h.adapter,false);h.Head(0);h.Steer();
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial+6);
    ReleaseGuided(h);
}
void ExhaustedThroughWaypointContinues(std::uint32_t parts) {
    NativeHarness h;h.current=GuidedMove(parts);CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    const auto navigation=reinterpret_cast<std::uintptr_t>(h.resource.data())+0xC4U;
    const auto sentinel=reinterpret_cast<std::uintptr_t>(h.schedulerHead.data());
    const auto initial=std::count(h.calls.begin(),h.calls.end(),1U);
    const auto resumed=GuidanceDiagnostics().exhaustedTailContinuations;
    NativeHarness::Write(request+0xC,2U);NativeHarness::Write(request+0x68,3U);
    NativeHarness::Write(request+0x6C,2U);NativeHarness::Write<std::uint8_t>(request+0xB8,0U);
    NativeHarness::Write<std::uint8_t>(request+0xB9,2U);
    NativeHarness::Write(navigation,sentinel);NativeHarness::Write(request+0x64U,sentinel);
    // No continuation is allowed while authority or the worker is unavailable.
    h.guidanceAllowed=false;h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial);
    h.guidanceAllowed=true;NativeHarness::Write(navigation+0x5CU,sentinel);
    h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial);
    NativeHarness::Write<std::uintptr_t>(navigation+0x5CU,0U);
    // The exact exhausted-tail path must preserve a foreign native action,
    // even if its own request is still reported active by the scheduler.
    h.Head(reinterpret_cast<std::uintptr_t>(h.requests[4].data()));
    const auto heldCalls=h.calls.size();h.Steer();CHECK(h.calls.size()==heldCalls);
    h.Head(0);
    NativeRequestAdapterTestAccess::Critical(h.adapter,h.adapter,true);
    h.Steer();CHECK(h.calls.size()==heldCalls);
    NativeRequestAdapterTestAccess::Critical(h.adapter,h.adapter,false);
    h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial+1);
    CHECK(GuidanceDiagnostics().exhaustedTailContinuations==resumed+1U);
    CHECK(std::count(h.calls.begin(),h.calls.end(),30U)==static_cast<int>(parts));
    CHECK(std::count(h.calls.begin(),h.calls.end(),31U)==static_cast<int>(parts));
    CHECK(std::count(h.calls.begin(),h.calls.end(),11U)==1); // Host replacement only; continuation appends.
    CHECK(NativeRequestAdapterTestAccess::Revision(h.adapter)==h.current.parameterRevision);
    ReleaseGuided(h);
}
void GuidedFailureAwaitsNativeSchedulerAndWorker() {
    NativeHarness h;h.current=GuidedMove();CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    const auto navigation=reinterpret_cast<std::uintptr_t>(h.resource.data())+0xC4U;
    const auto initial=std::count(h.calls.begin(),h.calls.end(),1U);
    for(const auto phase:{4U,8U}) {
        NativeHarness::Write(request+0xC,2U);NativeHarness::Write(request+0x68,phase);
        NativeHarness::Write(request+0x6C,phase==8U?3U:1U);h.Advance(4000);h.Steer();
        CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial); // No bypass of0045CE00.
    }
    NativeHarness::Write(request+0xC,3U);NativeHarness::Write(navigation+0x5CU,std::uintptr_t{123});
    h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial);
    NativeHarness::Write(navigation+0x5CU,std::uintptr_t{});NativeHarness::Write(navigation+0x40U,request+0x28U);
    h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial);
    NativeHarness::Write(navigation+0x40U,std::uintptr_t{});h.Steer();
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==initial+1);
    ReleaseGuided(h);
}
void GuidedRetainedReplacementKeepsIdentity() {
    NativeHarness h;h.current=GuidedMove();CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    NativeHarness::Write(request+0xC,5U);h.Retained(request);
    const auto calls=h.calls.size();h.Steer();CHECK(h.calls.size()==calls&&h.adapter.ActiveCount()==1);
    auto update=h.current;update.opcode=Op::Update;++update.parameterRevision;update.movement.after.goal[0]+=1.0F;h.current=update;
    CHECK(h.adapter.Invoke(update)==NativeOutcome::Admitted);h.Advance(4000);h.Steer();
    CHECK(h.calls.size()==calls&&NativeRequestAdapterTestAccess::Revision(h.adapter)==update.parameterRevision);
    // Only an external native retirement has removed the retained entry.
    h.Retained(0);h.Steer();
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==2&&h.retainedDrops==0U);
    ReleaseGuided(h);
}
void PhysicalProgressLimitsNativeGoals() {
    NativeHarness h;h.allowDerivedGoal=h.customProgress=true;h.current=GuidedMove();
    auto& move=h.current.movement;move.gait=2;move.before.goal=move.after.goal={12,0,0};
    h.progress={{2,0,0},{1,0,0},1,false};
    CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    CHECK(*reinterpret_cast<float*>(request+0x28)==2.0F); // No future graph exists in this fixture.
    CHECK(*reinterpret_cast<std::uint32_t*>(h.resource.data()+0x194)==1U);
    CHECK(*reinterpret_cast<float*>(request+0x54)==-1.0F&&(h.requests[0][0xB9]&1U)==0U);
    const auto factories=std::count(h.calls.begin(),h.calls.end(),1U);
    NativeHarness::Write(h.binding.character+0x70,3.0F);h.Steer();
    CHECK(*reinterpret_cast<float*>(request+0x34)==3.0F);
    CHECK(*reinterpret_cast<float*>(request+0x54)==-1.0F); // Goal first, no premature stop replan.
    h.NativeTick();h.Steer();CHECK(*reinterpret_cast<float*>(request+0x54)==0.15F);
    NativeHarness::Write(request+0xC,3U);
    for(unsigned i=0;i<8;++i){h.Advance(4000);h.Steer();}
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==factories);
    h.progress.position[0]=3.1F;h.Steer();CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==factories);
    h.progress.position[0]=3.5F;h.Steer();
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==factories+1);
    CHECK(*reinterpret_cast<float*>(request+0x28)==3.5F);
    NativeRequestAdapterTestAccess::Critical(h.adapter,h.adapter,true);h.Head(request);h.progress.gait=2;
    h.Advance();h.Steer();CHECK(*reinterpret_cast<std::uint32_t*>(h.resource.data()+0x194)==2U);
    h.Head(0x12345678U);const auto protectedCalls=h.calls.size();h.progress.gait=1;h.Advance();h.Steer();
    CHECK(h.calls.size()==protectedCalls&&*reinterpret_cast<std::uint32_t*>(h.resource.data()+0x194)==2U);
    NativeRequestAdapterTestAccess::Critical(h.adapter,h.adapter,false);h.Head(0);
    h.progressAvailable=false;h.Steer();CHECK(*reinterpret_cast<float*>(request+0x34)==3.0F);
    const auto calls=h.calls.size();h.progressAvailable=true;h.progress.protectedMotion=true;
    h.Steer();CHECK(h.calls.size()==calls);
    ReleaseGuided(h);
}
void ObservedBendFeedsNativeRequest() {
    NativeHarness h;h.allowDerivedGoal=h.customProgress=true;h.current=GuidedMove();
    auto& move=h.current.movement;move.before.goal=move.after.goal={12,0,12};move.gait=2;
    movement::ObservedTrail trail;trail.Observe({0,0,0},1000,true);
    trail.Observe({4,0,0},1500,true);trail.Observe({4,0,4},2000,true);
    h.progress={{4,0,4},{0,0,2},2,false,trail.Goal({0,0,0})};
    CHECK(h.adapter.Invoke(h.current)==NativeOutcome::Admitted);
    const auto request=reinterpret_cast<std::uintptr_t>(h.requests[0].data());
    CHECK(*reinterpret_cast<float*>(request+0x28)==4.0F&&*reinterpret_cast<float*>(request+0x30)==0.0F);
    CHECK(*reinterpret_cast<float*>(request+0x54)==-1.0F);
    h.progress.velocity={};h.Steer();CHECK(*reinterpret_cast<float*>(request+0x54)==0.15F);
    h.NativeTick();h.NativeTick(); // Let the native stop-distance replan finish.
    const auto factories=std::count(h.calls.begin(),h.calls.end(),1U);
    NativeHarness::Write(h.binding.character+0x70,4.0F);
    h.progress.velocity={0,0,2};h.progress.observed=trail.Goal({4,0,0});h.Advance();h.Steer();
    CHECK(*reinterpret_cast<float*>(request+0x34)==4.0F&&*reinterpret_cast<float*>(request+0x3C)==4.0F);
    h.NativeTick();h.NativeTick();h.Steer();
    CHECK(*reinterpret_cast<float*>(request+0x54)==-1.0F);
    CHECK(std::count(h.calls.begin(),h.calls.end(),1U)==factories&&h.cancellations.empty());
    ReleaseGuided(h);
}
void GoalDependencies() {
    NativeHarness h;auto move=Move();move.opcode=Op::Submit;move.movement.parts=1;
    move.movement.companion={};move.movement.companionFlags=move.movement.companionRetainedAfter=0;
    h.current=move;CHECK(h.adapter.Invoke(move)==NativeOutcome::Admitted);
    auto update=move;update.opcode=Op::Update;++update.parameterRevision;update.movement.after.goal[0]=8;
    const auto calls=h.calls.size();h.requests[0][0xB8]|=0x40U;
    for(unsigned retry=0;retry<100;++retry)CHECK(h.adapter.Check(update)==Readiness::Waiting&&h.calls.size()==calls);
    h.requests[0][0xB8]&=~0x40U;NativeHarness::Write(reinterpret_cast<std::uintptr_t>(h.requests[0].data())+0x68,2U);
    for(unsigned retry=0;retry<100;++retry)CHECK(h.adapter.Check(update)==Readiness::Waiting&&h.calls.size()==calls);
    NativeHarness::Write(reinterpret_cast<std::uintptr_t>(h.requests[0].data())+0x68,3U);
    h.current=update;CHECK(h.adapter.Check(update)==Readiness::Ready&&h.adapter.Invoke(update)==NativeOutcome::Admitted);
    ++update.parameterRevision;update.movement.after.goal[0]=9;h.current=update;
    h.requests[0][0xB8]&=~0x40U;NativeHarness::Write(reinterpret_cast<std::uintptr_t>(h.requests[0].data())+0x68,4U);
    CHECK(h.adapter.Check(update)==Readiness::Ready&&h.adapter.Invoke(update)==NativeOutcome::Admitted);
    CHECK(h.adapter.ActiveCount()==1&&std::count(h.calls.begin(),h.calls.end(),30U)==1&&std::count(h.calls.begin(),h.calls.end(),31U)==1);
    update.opcode=Op::Release;update.movement={};CHECK(h.adapter.Invoke(update)==NativeOutcome::Admitted&&h.adapter.ActiveCount()==0);
}
void NativeRouteInitializationOwnership() {
    NativeHarness h;auto move=Move();move.opcode=Op::Submit;move.movement.parts=1;
    move.movement.companion={};move.movement.companionFlags=move.movement.companionRetainedAfter=0;
    h.current=move;CHECK(h.adapter.Invoke(move)==NativeOutcome::Admitted);
    auto update=move;update.opcode=Op::Update;update.parameterRevision=2;
    // The local installer consumed reset bit0; a Host snapshot must not rearm it.
    h.requests[0][0xB8]=0x80U;update.movement.after.flags90=0x21U;
    CHECK(h.adapter.Invoke(update)==NativeOutcome::Admitted&&h.requests[0][0xB8]==0xA0U);
    // Conversely, leave an unconsumed local reset intact when the Host cleared its bit.
    h.requests[0][0xB8]=0x81U;++update.parameterRevision;update.movement.after.flags90=0U;
    CHECK(h.adapter.Invoke(update)==NativeOutcome::Admitted&&h.requests[0][0xB8]==0x81U);
    update.opcode=Op::Release;update.movement={};CHECK(h.adapter.Invoke(update)==NativeOutcome::Admitted);
}
void NativePreemption(movement::Navigation policy=movement::Navigation::LocalNative) {
    // Two separate native Move requests for one actor. The Host-issued
    // replacing admission wins, while old Host cleanup still owns the Ref.
    NativeHarness h;h.distinctMoves=true;
    auto old=Move();old.opcode=Op::Submit;old.movement.parts=1;old.movement.navigation=policy;
    old.movement.companion={};old.movement.companionFlags=old.movement.companionRetainedAfter=0;
    h.current=old;
    CHECK(h.adapter.Check(old)==Readiness::Ready&&h.adapter.Invoke(old)==NativeOutcome::Admitted);
    const auto navigation=reinterpret_cast<std::uintptr_t>(h.resource.data())+0xC4U;
    const auto first=reinterpret_cast<std::uintptr_t>(h.requests[0].data())+0x28U;
    const auto second=reinterpret_cast<std::uintptr_t>(h.requests[4].data())+0x28U;
    NavigationBinding binding{};
    CHECK(h.adapter.IdentifyNavigation(navigation,first,binding)==NavigationOwnership::Ready);
    auto replacement=old;++replacement.commandId;replacement.admission=0;
    h.current=replacement;
    CHECK(h.adapter.Check(replacement)==Readiness::Ready&&h.adapter.Invoke(replacement)==NativeOutcome::Admitted);
    CHECK(h.adapter.ActiveCount()==2);
    CHECK(h.adapter.IdentifyNavigation(navigation,first,binding)==NavigationOwnership::Rejected
        &&binding.command.command==old.commandId);
    CHECK(h.adapter.IdentifyNavigation(navigation,second,binding)==NavigationOwnership::Ready
        &&binding.command.command==replacement.commandId);
    auto staleUpdate=old;staleUpdate.opcode=Op::Update;++staleUpdate.parameterRevision;
    CHECK(h.adapter.Check(staleUpdate)==Readiness::Rejected);
    // Supersession is not a RefRelease; cancel and release only when the Host
    // supplies the old command's terminal operation.
    CHECK(h.cancellations.empty()&&std::count(h.calls.begin(),h.calls.end(),31U)==0);
    auto oldRelease=old;oldRelease.opcode=Op::Release;oldRelease.movement={};
    CHECK(h.adapter.Check(oldRelease)==Readiness::Ready&&h.adapter.Invoke(oldRelease)==NativeOutcome::Admitted);
    CHECK(h.adapter.ActiveCount()==1&&h.cancellations.size()==1);
    CHECK(h.adapter.IdentifyNavigation(navigation,second,binding)==NavigationOwnership::Ready);
    auto latestRelease=replacement;latestRelease.opcode=Op::Release;latestRelease.movement={};
    CHECK(h.adapter.Invoke(latestRelease)==NativeOutcome::Admitted&&h.adapter.ActiveCount()==0);
    CHECK(h.cancellations.size()==2&&std::count(h.calls.begin(),h.calls.end(),31U)==2);

    // Rejected replacement is NOT authority. The original native navigation
    // remains valid and must not be fenced by an unsuccessful admission.
    NativeHarness rejected;rejected.distinctMoves=true;
    rejected.current=old;
    CHECK(rejected.adapter.Invoke(old)==NativeOutcome::Admitted);
    rejected.accepted=false;rejected.current=replacement;
    CHECK(rejected.adapter.Invoke(replacement)==NativeOutcome::Rejected);
    const auto firstNavigation=reinterpret_cast<std::uintptr_t>(rejected.resource.data())+0xC4U;
    const auto firstBlock=reinterpret_cast<std::uintptr_t>(rejected.requests[0].data())+0x28U;
    CHECK(rejected.adapter.IdentifyNavigation(firstNavigation,firstBlock,binding)==NavigationOwnership::Ready);

    // A replacing Move also supersedes a prior scalar stance, whose retained
    // Ref must remain releasable but whose old Update must never regain control.
    NativeHarness scalar;
    auto stance=old;stance.commandId=200;stance.nativeKind=7;stance.movement={};
    stance.mode=2;stance.scalarRetention=2;stance.admission=1;
    scalar.current=stance;
    CHECK(scalar.adapter.Check(stance)==Readiness::Ready
        &&scalar.adapter.Invoke(stance)==NativeOutcome::Admitted);
    auto newer=old;newer.commandId=201;newer.admission=0;
    scalar.current=newer;
    CHECK(scalar.adapter.Invoke(newer)==NativeOutcome::Admitted);
    auto oldStanceUpdate=stance;oldStanceUpdate.opcode=Op::Update;++oldStanceUpdate.parameterRevision;
    CHECK(scalar.adapter.Check(oldStanceUpdate)==Readiness::Rejected);
    auto stanceRelease=stance;stanceRelease.opcode=Op::Release;
    stanceRelease.mode=0;stanceRelease.scalarRetention=3;
    CHECK(scalar.adapter.Invoke(stanceRelease)==NativeOutcome::Admitted
        &&scalar.adapter.ActiveCount()==1);
    auto newRelease=newer;newRelease.opcode=Op::Release;newRelease.movement={};
    CHECK(scalar.adapter.Invoke(newRelease)==NativeOutcome::Admitted
        &&scalar.adapter.ActiveCount()==0);
}
void LatePreemptedUpdate() {
    NativeHarness h; h.distinctMoves=true;
    TransactionExecutor journal({&h,
        [](void* p,const Record& r) {
            const auto ready=static_cast<NativeHarness*>(p)->adapter.Check(r);
            return ready==Readiness::Ready?Dependency::Ready:
                ready==Readiness::Waiting?Dependency::Waiting:Dependency::Reject;
        },
        [](void* p,const Record& r) {return static_cast<NativeHarness*>(p)->adapter.Invoke(r);}}, {}, true);
    auto old=Move();old.movement.parts=1;
    old.movement.companion={};old.movement.companionFlags=old.movement.companionRetainedAfter=0;
    h.current=old;
    CHECK(journal.Configure(old.scope)==Result::Ok&&journal.Execute(old).acceptedLogical);
    old.opcode=Op::Submit;old.globalSeq=old.ownerSeq=2;
    CHECK(journal.Execute(old).nativeOutcome==NativeOutcome::Admitted);
    auto current=old;++current.commandId;current.opcode=Op::Begin;current.globalSeq=current.ownerSeq=3;
    h.current=current;
    CHECK(journal.Execute(current).acceptedLogical);
    current.opcode=Op::Submit;current.globalSeq=current.ownerSeq=4;
    CHECK(journal.Execute(current).nativeOutcome==NativeOutcome::Admitted);
    auto late=old;late.opcode=Op::Update;late.globalSeq=late.ownerSeq=5;++late.parameterRevision;
    const auto nativeCalls=h.calls.size();
    const auto skipped=journal.Execute(late);
    CHECK(skipped.status==ExecutionStatus::Superseded&&skipped.result==Result::Ok&&
        skipped.acceptedLogical&&!skipped.nativeEntered&&!skipped.hasNativeOutcome);
    CHECK(h.calls.size()==nativeCalls&&h.adapter.ActiveCount()==2&&h.cancellations.empty());
    CHECK(journal.Retry(late).status==ExecutionStatus::Superseded&&h.calls.size()==nativeCalls);
    CHECK(journal.Checkpoint(late.scope,late.globalSeq)==Result::Ok);
    ++late.globalSeq;++late.ownerSeq;++late.parameterRevision;
    CHECK(journal.Execute(late).status==ExecutionStatus::Superseded&&h.calls.size()==nativeCalls);
    CHECK(journal.HasNativeOwnership(late.owner));
    // Exact terminal operations still cancel/release the old Ref once. The
    // replacement remains live and never loses navigation ownership.
    late.opcode=Op::Cancel;++late.globalSeq;++late.ownerSeq;late.movement={};
    CHECK(journal.Execute(late).nativeOutcome==NativeOutcome::Admitted);
    late.opcode=Op::Release;++late.globalSeq;++late.ownerSeq;
    CHECK(journal.Execute(late).nativeOutcome==NativeOutcome::Admitted);
    CHECK(h.adapter.ActiveCount()==1&&h.cancellations.size()==1);
    NavigationBinding binding{};
    CHECK(h.adapter.IdentifyNavigation(reinterpret_cast<std::uintptr_t>(h.resource.data())+0xC4U,
        reinterpret_cast<std::uintptr_t>(h.requests[4].data())+0x28U,binding)==NavigationOwnership::Ready);
}
void Native() {
    NativeHarness h;h.reenter=true;CHECK(h.Submit()==NativeOutcome::Admitted&&h.adapter.ActiveCount()==2);
    CHECK((h.calls==std::vector<unsigned>{1,23,11,25,3,13}));CHECK(h.actor[0x2CE8]==0&&h.resourceControl[1]==1&&h.requests[1][0x18]==1);
    CHECK(*reinterpret_cast<std::uint32_t*>(h.resource.data()+0x194)==3);
    CHECK(h.navigationDuringAdmission==2);
    NavigationBinding navigation{};const auto n=reinterpret_cast<std::uintptr_t>(h.resource.data())+0xC4;
    const auto primary=reinterpret_cast<std::uintptr_t>(h.requests[0].data())+0x28;
    const auto companion=reinterpret_cast<std::uintptr_t>(h.requests[1].data())+0x1C;
    CHECK(h.adapter.IdentifyNavigation(n,primary,navigation)==NavigationOwnership::Ready&&navigation.part==0&&
        navigation.command.command==h.current.commandId&&navigation.command.scope==h.current.scope&&navigation.command.revision==1);
    CHECK(h.adapter.IdentifyNavigation(n,companion,navigation)==NavigationOwnership::Ready&&navigation.part==1&&
        navigation.command.owner==h.current.owner&&navigation.policy==movement::Navigation::LocalNative);
    NativeRequestAdapterTestAccess::Policy(h.adapter,movement::Navigation::HostRoute);
    CHECK(h.adapter.IdentifyNavigation(n,primary,navigation)==NavigationOwnership::Ready);
    CHECK(h.adapter.IdentifyNavigation(n,primary+4,navigation)==NavigationOwnership::Rejected&&
        navigation.policy==movement::Navigation::HostRoute&&navigation.command.command==h.current.commandId);
    CHECK(h.adapter.IdentifyNavigation(n,0,navigation,false)==NavigationOwnership::Rejected);
    CHECK(h.adapter.IdentifyNavigation(n+4,0,navigation)==NavigationOwnership::Unowned);
    NativeRequestAdapterTestAccess::Policy(h.adapter,movement::Navigation::LocalNative);
    h.wirePermit=false;
    CHECK(h.adapter.IdentifyNavigation(n,primary,navigation)==NavigationOwnership::Ready);
    CHECK(h.adapter.IdentifyNavigation(n,primary,navigation,false)==NavigationOwnership::Rejected&&
        navigation.command.command==h.current.commandId);
    h.localOwned=false;CHECK(h.adapter.IdentifyNavigation(n,primary,navigation)==NavigationOwnership::Rejected);
    h.localOwned=h.wirePermit=true;
    CHECK(h.adapter.IdentifyNavigation(n+4,primary,navigation)==NavigationOwnership::Rejected);
    CHECK(h.adapter.IdentifyNavigation(n,primary+4,navigation)==NavigationOwnership::Unowned);
    auto update=h.current;update.opcode=Op::Update;++update.parameterRevision;update.movement.after.goal[0]=8;
    CHECK(h.adapter.Check(update)==Readiness::Ready&&h.adapter.Invoke(update)==NativeOutcome::Admitted);
    CHECK(h.adapter.IdentifyNavigation(n,primary,navigation)==NavigationOwnership::Ready&&navigation.command.revision==2);
    ++update.parameterRevision;update.movement.after.goal[0]=9;
    const auto count=h.calls.size();CHECK(h.adapter.Check(update)==Readiness::Waiting&&h.calls.size()==count);
    h.requests[0][0xB8]&=~0x40U;CHECK(h.adapter.Check(update)==Readiness::Ready);
    auto release=h.current;release.opcode=Op::Release;release.movement={};CHECK(h.adapter.Check(release)==Readiness::Ready);
    auto cancel=release;cancel.opcode=Op::Cancel;CHECK(h.adapter.Invoke(cancel)==NativeOutcome::Admitted);
    CHECK(h.adapter.Invoke(release)==NativeOutcome::Admitted&&h.adapter.ActiveCount()==0);
    CHECK(h.adapter.IdentifyNavigation(n,primary,navigation)==NavigationOwnership::Unowned);
    CHECK(std::count(h.calls.begin(),h.calls.end(),30U)==2&&std::count(h.calls.begin(),h.calls.end(),31U)==2);
    NativeHarness strict;auto r=Move();r.opcode=Op::Submit;r.movement.gait=0;r.movement.navigation=movement::Navigation::HostRoute;
    CHECK(strict.adapter.Check(r)==Readiness::Waiting&&strict.calls.empty());
    NativeHarness reject;reject.accepted=false;CHECK(reject.Submit()==NativeOutcome::Rejected&&reject.adapter.ActiveCount()==1);
    cancel=reject.current;cancel.opcode=Op::Cancel;cancel.movement={};CHECK(reject.adapter.Invoke(cancel)==NativeOutcome::Admitted);
    cancel.opcode=Op::Release;CHECK(reject.adapter.Invoke(cancel)==NativeOutcome::Admitted&&reject.adapter.ActiveCount()==0);
    NativeHarness partial;partial.faultCompanion=true;CHECK(partial.Submit()==NativeOutcome::Unknown&&partial.adapter.ActiveCount()==2);
    CHECK(partial.adapter.Check(partial.current)==Readiness::Rejected);
    NativeHarness recycle;recycle.recycleOnAdmission=true;CHECK(recycle.Submit()==NativeOutcome::Unknown&&recycle.calls.back()==11);
    NativeHarness stop;auto rStop=Move();rStop.nativeKind=3;rStop.opcode=Op::Submit;
    rStop.movement.parts=1;rStop.movement.companion={};rStop.movement.companionFlags=rStop.movement.companionRetainedAfter=0;
    rStop.movement.primaryFlags=2;rStop.movement.after=rStop.movement.before;
    CHECK(ValidateExecutable(rStop)&&stop.adapter.Check(rStop)==Readiness::Ready&&stop.adapter.Invoke(rStop)==NativeOutcome::Admitted);
    CHECK((stop.calls==std::vector<unsigned>{3,11})&&stop.adapter.ActiveCount()==1&&stop.requests[1][0xB8]==2);
    rStop.opcode=Op::Cancel;rStop.movement={};CHECK(stop.adapter.Invoke(rStop)==NativeOutcome::Admitted);
    rStop.opcode=Op::Release;CHECK(stop.adapter.Invoke(rStop)==NativeOutcome::Admitted&&stop.adapter.ActiveCount()==0);
    NativeHarness retire;CHECK(retire.Submit()==NativeOutcome::Admitted);
    auto releaseRunning=retire.current;releaseRunning.opcode=Op::Release;releaseRunning.movement={};
    CHECK(retire.adapter.Check(releaseRunning)==Readiness::Ready&&retire.adapter.Invoke(releaseRunning)==NativeOutcome::Admitted);
    CHECK(retire.adapter.ActiveCount()==0&&std::count(retire.calls.begin(),retire.calls.end(),30U)==2&&
        std::count(retire.calls.begin(),retire.calls.end(),31U)==2);
    NativeHarness queued;auto movement=Move();movement.opcode=Op::Submit;movement.movement.parts=1;
    movement.movement.companion={};movement.movement.companionFlags=movement.movement.companionRetainedAfter=0;
    CHECK(queued.adapter.Invoke(movement)==NativeOutcome::Admitted);
    auto stance=movement;stance.commandId=101;stance.nativeKind=7;stance.movement={};stance.mode=2;stance.scalarRetention=2;stance.admission=1;
    CHECK(queued.adapter.Check(stance)==Readiness::Ready&&queued.adapter.Invoke(stance)==NativeOutcome::Admitted&&queued.adapter.ActiveCount()==2);
    CHECK(queued.requests[3][0x18]==1&&queued.calls.back()==13);
    auto replacement=movement;replacement.commandId=102;CHECK(queued.adapter.Check(replacement)==Readiness::Ready); // Native Submit replaces the queue itself, as on the Host.
    movement.opcode=Op::Release;movement.movement={};CHECK(queued.adapter.Invoke(movement)==NativeOutcome::Admitted);
    CHECK(queued.adapter.ActiveCount()==1&&queued.requests[3][0x18]==1);
    stance.opcode=Op::Release;stance.mode=0;stance.scalarRetention=3;CHECK(queued.adapter.Invoke(stance)==NativeOutcome::Admitted&&queued.adapter.ActiveCount()==0);
}
}
int main(int argc,char** argv){CHECK(argc==2);Wire(argv[1]);Evidence();HostInputsAreNotReplacedByPhysicalCarrier(1);HostInputsAreNotReplacedByPhysicalCarrier(2);RejectedRouteRetainsNativeLifetime();GuidedInitialTargetWaitsWithoutFactory();GuidedUpdatesOnlyRetainValues();GuidedUpdateWaitsForRetailWithoutRestart();GuidedGoalConsumptionPrecedesStopAndGait();GuidedRejectedAndDiscardedGoalsRemainPendingIntent();GuidedFailureAwaitsNativeSchedulerAndWorker();GuidedRetainedReplacementKeepsIdentity();GuidedContinuationIsBoundedAndAppended();GuidedContinuationIsBoundedAndAppended(2);RetainedArrivalContinuesWithoutNewHostRevision(1);RetainedArrivalContinuesWithoutNewHostRevision(2);ExhaustedThroughWaypointContinues(1);ExhaustedThroughWaypointContinues(2);PhysicalProgressLimitsNativeGoals();ObservedBendFeedsNativeRequest();GoalDependencies();NativeRouteInitializationOwnership();NativePreemption();NativePreemption(movement::Navigation::HostGuided);LatePreemptedUpdate();Native();std::cout<<"AISync Move wire, paced guidance, full evidence, compound native ownership PASS (offline)\n";}
