#include "aimp/AISyncNativeRequestAdapter.h"
#include "aimp/AISyncExecutionCodec.h"
#include <Windows.h>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

namespace aimp::ai_sync::execution {
struct NativeRequestAdapterTestAccess {
    static void Prepare(NativeRequestAdapter& a,void* context,Permission permit,ResolveActor resolve,
        bool(*factory)(void*,std::uint32_t,std::uintptr_t,std::uintptr_t*) noexcept,
        bool(*admit)(void*,std::uintptr_t,std::uint32_t,bool&) noexcept,
        bool(*goal)(void*,std::uintptr_t,const float*) noexcept,
        bool(*insert)(void*,std::uintptr_t,std::uintptr_t) noexcept,
        bool(*cancel)(void*,std::uintptr_t) noexcept,bool(*release)(void*,std::uintptr_t) noexcept) {
        a.base_=0x10000000;a.permission_=permit;a.resolve_=a.lifecycleResolver_=resolve;a.context_=context;
        a.movementCallsContext_=a.retirementCallsContext_=context;
        a.movementFactory_=factory;a.movementAdmission_=admit;a.markerGoal_=goal;a.markerInsertion_=insert;
        a.retirementCancel_=cancel;a.retirementRelease_=release;
        a.movementRoutesAvailable_=+[]() noexcept {return true;};
    }
    static bool Fenced(const NativeRequestAdapter& a) {return a.leases_[0].routeRejected;}
    static void Unknown(NativeRequestAdapter& a) {a.leases_[0].unknown=true;}
    static void LocalResolver(NativeRequestAdapter& a,ResolveActor resolver) {a.lifecycleResolver_=resolver;}
};
}
using namespace aimp::ai_sync;
using namespace execution;
using namespace transactions;
namespace {
void Check(bool value,int line) {if(!value){std::cerr<<"marker invariant at "<<line<<'\n';std::abort();}}
#define CHECK(v) Check((v),__LINE__)
template<class T> void Put(std::uintptr_t at,T value) {std::memcpy(reinterpret_cast<void*>(at),&value,sizeof(value));}
template<class T> T Get(std::uintptr_t at) {T value{};std::memcpy(&value,reinterpret_cast<const void*>(at),sizeof(value));return value;}
bool resolveMarker=true;
std::uint32_t localMarker=73;
unsigned resolutions{};
bool Marker(const Record& r,std::uint32_t& marker) noexcept {
    ++resolutions;marker=localMarker;
    CHECK(ValidateExecutable(r)&&r.commandId==100&&r.parameterRevision!=0&&r.owner.entityA==11&&r.globalSeq==1);
    return resolveMarker&&r.intent.words[0]==24&&r.intent.words[2]==0x12345678U;
}
Record Command() {
    Record r{};r.scope.session[0]=5;r.scope.authorityEpoch=1;r.scope.worldEpoch=2;r.scope.consumerEpoch=3;
    r.owner={11,22,1,2,Capability::Movement,1};r.commandId=100;r.globalSeq=r.ownerSeq=1;
    r.parameterRevision=1;r.nativeKind=2;r.opcode=Op::Submit;r.admission=1;
    r.intent.words[0]=24;r.intent.words[1]=1;r.intent.words[2]=0x12345678U;r.intent.words[10]=2;
    r.intent.words[11]=3;r.intent.words[12]=1;r.intent.words[13]=0;
    auto& b=r.movement;b.version=b.parts=1;b.navigation=movement::Navigation::HostRoute;
    b.before={0,2,{1.25F,-2.5F,3.75F},1.75F,-1,{4,5,6,7,50,.25F,-.5F,8,1.15F},0x1E,0x10};
    // Raw Host query bits, including uninitialized words, must survive.
    b.before.query[8]=std::bit_cast<float>(0x7FC01234U);
    b.after=b.before;b.after.retained=1;return r;
}
struct Harness {
    NativeRequestAdapter adapter;
    alignas(4) std::array<std::uint8_t,0x4300> actor{};
    alignas(4) std::array<std::uint8_t,0xC0> request{};
    alignas(4) std::array<std::uint8_t,0x198> resource{},replacement{};
    std::array<LONG,3> control{0,1,1},resourceControl{0,1,1};
    LocalActorBinding binding{reinterpret_cast<std::uintptr_t>(actor.data()),19,true,1};
    Record command{Command()};
    unsigned factories{},admissions{},goals{},insertions{},cancels{},releases{};
    bool synchronous{true},allowed{true},owned{true},accepted{true},currentLifetime{true};
    bool failInsert{},recycleGoal{},revokeInsert{},reenterInsert{},swapResource{},revokeMarkerGoal{},revokeMarkerInsert{};
    MarkerInitialization initialized{MarkerInitialization::Unowned};
    Harness() {
        resolveMarker=true;localMarker=73;resolutions=0;SetMarkerResolver(&Marker);
        Put(binding.character+0x50,binding.runtimeId);Put(binding.character+0x2CD0,binding.character);
        Put(binding.character+0x42A8,reinterpret_cast<std::uintptr_t>(resource.data()));
        Put(binding.character+0x42AC,reinterpret_cast<std::uintptr_t>(resourceControl.data()));
        NativeRequestAdapterTestAccess::Prepare(adapter,this,&Permit,&Resolve,&Factory,&Admit,&Goal,&Insert,&Cancel,&Release);
        CHECK(ValidateExecutable(command));
    }
    std::uintptr_t Request() {return reinterpret_cast<std::uintptr_t>(request.data());}
    std::uintptr_t Navigation() {return reinterpret_cast<std::uintptr_t>(resource.data())+0xC4;}
    static bool Permit(void* p,const Record&) noexcept {return static_cast<Harness*>(p)->allowed;}
    static bool Resolve(void* p,const OwnerKey&,LocalActorBinding& actor) noexcept {
        auto& h=*static_cast<Harness*>(p);actor=h.binding;return h.owned;
    }
    static bool Factory(void* p,std::uint32_t kind,std::uintptr_t scheduler,std::uintptr_t* ref) noexcept {
        auto& h=*static_cast<Harness*>(p);++h.factories;
        CHECK(kind==2&&scheduler==h.binding.character+0x2CD0&&resolutions>=2);
        ref[0]=h.Request();ref[1]=reinterpret_cast<std::uintptr_t>(h.control.data());
        Put(ref[0],std::uintptr_t{0x10000000U+0xEB1A60U});Put(ref[0]+0x10,2U);
        Put(ref[0]+0x14,scheduler);return true;
    }
    static bool Admit(void* p,std::uintptr_t request,std::uint32_t admission,bool& acceptedOut) noexcept {
        auto& h=*static_cast<Harness*>(p);++h.admissions;
        CHECK(admission==h.command.admission&&h.resourceControl[1]==2);
        CHECK(Get<std::uint32_t>(request+0xB0)==73&&Get<std::uint8_t>(request+0xB4)==1);
        CHECK(Get<std::uint8_t>(request+0x18)==0);
        CHECK(h.adapter.Check(h.command)==Readiness::Rejected);
        if(h.synchronous)h.initialized=h.adapter.InitializeMarker(request,h.binding.character,h.currentLifetime);
        Put(request+0xC,h.synchronous?1U:0U);acceptedOut=h.accepted;return true;
    }
    static bool Goal(void* p,std::uintptr_t block,const float* goal) noexcept {
        auto& h=*static_cast<Harness*>(p);++h.goals;
        CHECK(block==h.Request()+0x1C&&h.resourceControl[1]>=2);
        for(std::size_t i=0;i<3;++i){Put(block+i*4,goal[i]);Put(block+0xC+i*4,goal[i]);}
        if(h.recycleGoal)Put(h.binding.character+0x50,h.binding.runtimeId+1);
        if(h.revokeMarkerGoal)resolveMarker=false;
        return true;
    }
    static bool Insert(void* p,std::uintptr_t navigation,std::uintptr_t block) noexcept {
        auto& h=*static_cast<Harness*>(p);++h.insertions;
        CHECK(navigation==h.Navigation()&&block==h.Request()+0x1C&&h.resourceControl[1]>=2);
        const auto& expected=h.command.movement.before;
        CHECK(Get<float>(block)==expected.goal[0]&&Get<float>(block+0xC)==expected.goal[0]);
        CHECK(Get<float>(block+0x2C)==expected.stoppingDistance&&Get<float>(block+0x30)==expected.stopDistance);
        CHECK(Get<std::uint32_t>(block+0x50)==expected.style);
        for(std::size_t i=0;i<9;++i)CHECK(Get<std::uint32_t>(block+0x58+i*4)==std::bit_cast<std::uint32_t>(expected.query[i]));
        CHECK((Get<std::uint8_t>(block+0x90)&0x3FU)==expected.flags90);
        NavigationBinding binding{};
        CHECK(h.adapter.IdentifyNavigation(navigation,block,binding)==NavigationOwnership::Admitting);
        CHECK(binding.command.kind==2&&binding.command.command==100&&binding.part==0);
        if(h.reenterInsert)CHECK(h.adapter.InitializeMarker(h.Request(),h.binding.character,true)==MarkerInitialization::Rejected);
        if(h.revokeInsert)h.owned=false;
        if(h.revokeMarkerInsert)resolveMarker=false;
        if(h.swapResource)Put(h.binding.character+0x42A8,reinterpret_cast<std::uintptr_t>(h.replacement.data()));
        Put(block+0x40,1U);return !h.failInsert;
    }
    static bool Cancel(void* p,std::uintptr_t request) noexcept {
        auto& h=*static_cast<Harness*>(p);++h.cancels;Put(request+0xC,4U);return true;
    }
    static bool Release(void* p,std::uintptr_t ref) noexcept {
        ++static_cast<Harness*>(p)->releases;auto* pair=reinterpret_cast<std::uintptr_t*>(ref);pair[0]=pair[1]=0;return true;
    }
    NativeOutcome Submit() {return adapter.Invoke(command);}
    MarkerInitialization Initialize() {return adapter.InitializeMarker(Request(),binding.character,currentLifetime);}
    Record Update() {auto r=command;r.opcode=Op::Update;++r.parameterRevision;r.intent.words[13]=1;r.movement.after.retained=0;return r;}
    void Cleanup() {
        auto r=command;r.opcode=Op::Release;r.movement={};r.intent={};
        CHECK(adapter.Invoke(r)==NativeOutcome::Admitted&&adapter.ActiveCount()==0&&cancels==1&&releases==1);
        CHECK(resourceControl[1]==1);
    }
};
void SynchronousAndDelayed() {
    for(const auto admission:{0U,1U,2U}) {
        auto h=std::make_unique<Harness>();h->command.admission=admission;
        CHECK(h->Submit()==NativeOutcome::Admitted&&h->initialized==MarkerInitialization::Applied);
        CHECK(h->factories==1&&h->goals==1&&h->insertions==1&&h->resourceControl[1]==1);
        CHECK(Get<std::uint8_t>(h->Request()+0xB4)==0&&Get<std::uint8_t>(h->Request()+0x18)==1);
        h->Cleanup();
    }
    auto h=std::make_unique<Harness>();h->synchronous=false;
    CHECK(h->Submit()==NativeOutcome::Admitted&&h->insertions==0);
    auto update=h->Update();CHECK(h->adapter.Invoke(update)==NativeOutcome::Admitted); // status0 queue
    h->allowed=false; // Mailbox permission is closed; lifecycle ownership is still live.
    CHECK(h->Initialize()==MarkerInitialization::Applied&&h->insertions==1);
    CHECK(Get<std::uint8_t>(h->Request()+0xB4)==1&&Get<std::uint8_t>(h->Request()+0x18)==0);
    h->allowed=true;h->Cleanup();
}
void ResolverAndUpdates() {
    {auto h=std::make_unique<Harness>();resolveMarker=false;CHECK(h->Submit()==NativeOutcome::Rejected&&h->factories==0);}
    {auto h=std::make_unique<Harness>();SetMarkerResolver(nullptr);CHECK(h->Submit()==NativeOutcome::Rejected&&h->factories==0);}
    {auto h=std::make_unique<Harness>();localMarker=UINT32_MAX;CHECK(h->Submit()==NativeOutcome::Rejected&&h->factories==0);}
    auto h=std::make_unique<Harness>();CHECK(h->Submit()==NativeOutcome::Admitted);
    auto r=h->Update();const auto original=h->request;
    CHECK(h->adapter.Invoke(r)==NativeOutcome::Admitted&&h->goals==1&&h->insertions==1&&h->factories==1);
    auto expected=original;expected[0xB4]=1;expected[0x18]=0;CHECK(h->request==expected);
    for(unsigned edit=0;edit<5;++edit) {
        auto bad=r;++bad.parameterRevision;
        if(edit==0)++bad.intent.words[10];
        if(edit==1){bad.movement.before.goal[0]+=1;bad.movement.after.goal[0]+=1;}
        if(edit==2){bad.movement.before.query[0]+=1;bad.movement.after.query[0]+=1;}
        if(edit==3)bad.movement.navigation=movement::Navigation::LocalNative;
        if(edit==4)++bad.admission;
        CHECK(h->adapter.Invoke(bad)==NativeOutcome::Rejected&&h->request==expected);
    }
    localMarker=74;++r.parameterRevision;CHECK(h->adapter.Check(r)==Readiness::Rejected);localMarker=73;
    NavigationBinding binding{};CHECK(h->adapter.IdentifyNavigation(h->Navigation(),h->Request()+0x1C,binding)==NavigationOwnership::Ready);
    CHECK(h->adapter.IdentifyNavigation(h->Navigation(),h->Request()+0x20,binding)==NavigationOwnership::Rejected);
    CHECK(h->adapter.RejectNavigation(binding));CHECK(h->adapter.Check(r)==Readiness::Rejected);h->Cleanup();
}
void OwnershipAndFailures() {
    {auto h=std::make_unique<Harness>();CHECK(h->adapter.InitializeMarker(123,456,true)==MarkerInitialization::Unowned);}
    for(unsigned failure=0;failure<16;++failure) {
        auto h=std::make_unique<Harness>();h->synchronous=false;CHECK(h->Submit()==NativeOutcome::Admitted);
        if(failure==0)h->currentLifetime=false;
        if(failure==1)h->owned=false;
        if(failure==2)Put(h->binding.character+0x50,h->binding.runtimeId+1);
        if(failure==3)Put(h->Request()+0xB0,74U);
        if(failure==4)NativeRequestAdapterTestAccess::Unknown(h->adapter);
        if(failure==5)h->failInsert=true;
        if(failure==6)h->recycleGoal=true;
        if(failure==7)h->revokeInsert=true;
        if(failure==8)h->swapResource=true;
        if(failure==9)h->reenterInsert=true;
        if(failure==10)CHECK(h->adapter.BeginRetirement(h->command.scope));
        if(failure==11)resolveMarker=false; // Loader binding revoked after Submit.
        if(failure==12)localMarker=74; // Same identity no longer maps to the retained marker.
        if(failure==13)h->revokeMarkerGoal=true; // Reentrant revoke before insertion.
        if(failure==14)SetMarkerResolver(nullptr);
        if(failure==15)h->revokeMarkerInsert=true;
        CHECK(h->Initialize()==MarkerInitialization::Rejected&&NativeRequestAdapterTestAccess::Fenced(h->adapter));
        if(failure>=11&&failure<=14)CHECK(h->insertions==0);
        const auto calls=h->insertions;CHECK(h->Initialize()==MarkerInitialization::Rejected&&h->insertions==calls);
        CHECK(h->resourceControl[1]==1);
    }
    {auto h=std::make_unique<Harness>();h->synchronous=false;CHECK(h->Submit()==NativeOutcome::Admitted);
        CHECK(h->adapter.InitializeMarker(h->Request(),h->binding.character+4,true)==MarkerInitialization::Rejected);
        CHECK(NativeRequestAdapterTestAccess::Fenced(h->adapter));}
    {auto h=std::make_unique<Harness>();h->synchronous=false;CHECK(h->Submit()==NativeOutcome::Admitted);
        CHECK(h->adapter.InitializeMarker(h->Request()+4,h->binding.character,true)==MarkerInitialization::Rejected);
        CHECK(NativeRequestAdapterTestAccess::Fenced(h->adapter));}
    {auto h=std::make_unique<Harness>();h->currentLifetime=false;
        CHECK(h->Submit()==NativeOutcome::Unknown&&h->insertions==0&&NativeRequestAdapterTestAccess::Fenced(h->adapter));}
    {auto h=std::make_unique<Harness>();h->accepted=false;CHECK(h->Submit()==NativeOutcome::Rejected);h->Cleanup();}
}
}
int main() {
    static_assert(sizeof(std::uintptr_t)==4,"native request tests require Win32");
    SynchronousAndDelayed();ResolverAndUpdates();OwnershipAndFailures();SetMarkerResolver(nullptr);
    std::cout<<"AISync native marker ordering, geometry, delayed ownership and refusal PASS (offline injected calls)\n";
}
