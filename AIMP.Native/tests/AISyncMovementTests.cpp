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
#include <string>
#include <vector>

namespace aimp::ai_sync::execution {
struct NativeRequestAdapterTestAccess {
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
    NativeRequestAdapter adapter;
    alignas(4)std::array<std::uint8_t,0x4300> actor{};
    alignas(4)std::array<std::array<std::uint8_t,0x100>,5> requests{};
    std::array<std::array<LONG,3>,5> controls{{{0,1,1},{0,1,1},{0,1,1},{0,1,1},{0,1,1}}};
    alignas(4)std::array<std::uint8_t,0x198> resource{};std::array<LONG,3> resourceControl{0,1,1};
    LocalActorBinding binding{reinterpret_cast<std::uintptr_t>(actor.data()),19,true};
    Record current{};std::vector<unsigned> calls;bool accepted{true},faultCompanion{},recycleOnAdmission{},reenter{},failCancel{};
    std::vector<std::uintptr_t> cancellations;
    bool wirePermit{true},localOwned{true},distinctMoves{};
    unsigned navigationDuringAdmission{},moveFactoryCalls{};
    template<class T>static void Write(std::uintptr_t at,T v){*reinterpret_cast<T*>(at)=v;}
    NativeHarness(){Write(binding.character+0x50,binding.runtimeId);Write(binding.character+0x2CD0,binding.character);
        Write(binding.character+0x42A8,reinterpret_cast<std::uintptr_t>(resource.data()));
        Write(binding.character+0x42AC,reinterpret_cast<std::uintptr_t>(resourceControl.data()));
        NativeRequestAdapterTestAccess::Prepare(adapter,&Permit,&Resolve,this,&Factory,&Admit,&Goal,&Stop,&Cancel,&Release);}
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
        Write(ref[0]+0xC,1U);Write(ref[0]+0x68,3U);return true;}
    static bool Admit(void* p,std::uintptr_t request,std::uint32_t admission,bool& result)noexcept{
        auto& h=*static_cast<NativeHarness*>(p);h.calls.push_back(admission==1?13U:(admission==2?12U:11U));CHECK(h.resourceControl[1]==2);
        const auto kind=*reinterpret_cast<std::uint32_t*>(request+0x10);
        if(kind==1||kind==3)CHECK(*reinterpret_cast<float*>(request+(kind==3?0x1C:0x28))==
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
        for(std::size_t i=0;i<3;++i)Write(request+(update?0x34:0x28)+i*4,goal[i]);
        if(update)Write<std::uint8_t>(request+0xB8,static_cast<std::uint8_t>(h.requests[0][0xB8]|0x40));return true;}
    static bool Stop(void* p,std::uintptr_t request,float distance,std::uint8_t variant)noexcept{
        static_cast<NativeHarness*>(p)->calls.push_back(25);Write(request+0x58,distance);
        auto& flags=*reinterpret_cast<std::uint8_t*>(request+0xB9);flags=static_cast<std::uint8_t>((flags&~0x20)|(variant<<5)|0x40);return true;}
    static bool Cancel(void* p,std::uintptr_t request)noexcept{auto& h=*static_cast<NativeHarness*>(p);h.calls.push_back(30);h.cancellations.push_back(request);if(h.failCancel)return false;Write(request+0xC,4U);return true;}
    static bool Release(void* p,std::uintptr_t ref)noexcept{static_cast<NativeHarness*>(p)->calls.push_back(31);auto* pair=reinterpret_cast<std::uintptr_t*>(ref);pair[0]=pair[1]=0;return true;}
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
void NativePreemption() {
    // Two separate native Move requests for one actor. The Host-issued
    // replacing admission wins, while old Host cleanup still owns the Ref.
    NativeHarness h;h.distinctMoves=true;
    auto old=Move();old.opcode=Op::Submit;old.movement.parts=1;
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
int main(int argc,char** argv){CHECK(argc==2);Wire(argv[1]);Evidence();RejectedRouteRetainsNativeLifetime();GoalDependencies();NativeRouteInitializationOwnership();NativePreemption();Native();std::cout<<"AISync Move wire, full evidence, compound native ownership PASS (offline)\n";}
