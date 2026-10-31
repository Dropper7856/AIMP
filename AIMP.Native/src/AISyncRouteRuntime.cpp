#include "aimp/AISyncRouteRuntime.h"
#include "aimp/AISyncProfiling.h"
#include "aimp/AISyncExecutionRuntime.h"
#include "aimp/AISyncHostProducer.h"
#include "aimp/AISyncMeshStorage.h"
#include "aimp/AISyncRouteExecution.h"
#include "aimp/AISyncRouteContract.h"
#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <bit>
#include <cstring>
#include <memory>
#include <mutex>

namespace aimp::ai_sync::route_runtime {
namespace {
constexpr std::size_t kPackets=64;
struct CaptureContext {std::uintptr_t navigation{},block{},result{},resultControl{};};
struct CommandFault {route::Key key{};route::Failure reason{};bool used{};};
struct State {
    std::array<CommandFault,host_producer::Producer::kCommands> faults{};
    std::array<Packet,kPackets> incoming{};
    std::size_t inRead{},inCount{};
    std::mutex gate;
};
State* gState{};
std::uintptr_t gBase{};
void* gGoalTrampoline{};
std::atomic<void*> gGoalOriginal{};
EngineThread gEngine{};
Qualify gHost{},gClient{};
using NavigationCall=void(__thiscall*)(void*);
using PendingGoalCall=void(__thiscall*)(void*,void*);
std::atomic<bool> gAvailable{};
std::atomic<std::uint64_t> gCaptured{},gImported{},gBlocked{},gPending{},gRetained{};
std::atomic<std::uint64_t> gQueued{},gQueueRejected{},gEngineQueries{},gWorkerQueries{},gStrictQueries{},gUnownedQueries{},gRequeues{};
std::atomic<std::uint32_t> gLastQueueRejection{},gLastQueryOwnership{},gLastQueryPolicy{},gLastQueryKind{};
std::atomic<std::uint64_t> gInvalidations{},gReplanSuppressed{},gGoalDeferred{},gAwaitingHost{};
std::atomic<std::uint64_t> gReplacements{},gAdmissions{},gSuperseded{};
std::atomic<std::uint64_t> gCommandFaults{},gDiscarded{},gGlobalFailures{},gHostRoutesUnavailable{};
std::uint64_t HostRoutesUnavailable() noexcept;
std::atomic<std::uint32_t> gHostFailureSite{},gHostFailureResult{},gHostNavigationNodes{},gHostResultNodes{};
std::atomic<std::uint32_t> gHostCaptureCheck{};
std::atomic<std::uint32_t> gHostCaptureNode{UINT32_MAX},gHostMarkerIndex{UINT32_MAX},gHostMarkerKind{UINT32_MAX},gHostMarkerTypeRva{UINT32_MAX};
std::atomic<std::uint64_t> gHostCaptureDeferred{};
std::atomic<std::uint32_t> gHostBlockState{},gHostBlockMode{},gHostStopBits{},gHostNodeType{},gHostMetadataBytes{};
std::mutex gFailureGate;
std::array<ClientFailure,64> gFailures{};
std::size_t gFailureRead{},gFailureCount{};
std::atomic<std::uint64_t> gFailureDropped{};
// 00466638 is the FIRST of three blocking navigation task-list submissions.
// Its result is discarded. 009BB220 consumes one native task (ECX=list,
// AL=hasTask, RET), including its five cdecl arguments and native Ref lifetime.
// Execute this list at its original barrier: enqueue-time execution would
// change ordering, and forwarding it to workers bypasses all engine-owned
// capture/admission state. The two following locomotion lists stay native.
int __cdecl NavigationBatch(void* list,int wait) {
    using Submit=int(__cdecl*)(void*,int);
    transactions::Scope scope{};
    // Revocation closes new admissions, not ownership of already retained
    // requests. Keep their task boundary on the engine thread while draining:
    // moving it back to workers would bypass typed ownership and allow an old
    // strict request to submit a local solver after the Client was frozen.
    const bool retained=route_execution::LiveSlots().total!=0U||
        execution_runtime::RetirementSnapshot().references!=0U;
    const bool authoritative=Available()&&gEngine&&gEngine()&&
        (retained||(host_producer::HostScope(scope)&&gHost(scope))||
         (execution_runtime::CurrentScope(scope)&&gClient(scope)));
    if(!authoritative)return reinterpret_cast<Submit>(gBase+0x5E9530U)(list,wait);
    using Step=bool(__thiscall*)(void*);
    const auto step=reinterpret_cast<Step>(gBase+0x5BB220U);
    while(step(list)) {}
    return -1; // No worker job was allocated; this callsite never uses the ID.
}
void CaptureHostGoalEdit(std::uintptr_t,std::uintptr_t,const std::array<float,3>&) noexcept;
template<class T> bool Read(std::uintptr_t at,T& value) noexcept {
    return crash_guard::TryCopy(&value,reinterpret_cast<const void*>(at),sizeof(value));
}
void HostCaptureFailure(const CaptureContext& local,std::uint32_t site,route::Result result) noexcept {
    const auto value=[](std::uintptr_t at) noexcept {std::uint32_t out=UINT32_MAX;static_cast<void>(Read(at,out));return out;};
    gHostNavigationNodes=value(local.navigation+4);gHostResultNodes=value(local.result+0x3C);
    gHostBlockState=value(local.block+0x40);gHostBlockMode=value(local.block+0x48);gHostStopBits=value(local.block+0x30);
    std::uintptr_t sentinel{},first{},node{},begin{},end{};
    gHostNodeType=UINT32_MAX;gHostMetadataBytes=UINT32_MAX;
    const bool nodeList=local.result?
        (Read(local.result+0x38,sentinel)&&sentinel&&Read(sentinel,first)):
        (Read(local.navigation,sentinel)&&sentinel&&Read(local.navigation+0xC,first));
    if(nodeList&&first&&first!=sentinel&&
        Read(first+8,node)&&node) {
        gHostNodeType=value(node+4);
        if(Read(node+0x34,begin)&&Read(node+0x38,end)&&end>=begin)gHostMetadataBytes=static_cast<std::uint32_t>(end-begin);
    }
    gHostFailureResult=static_cast<std::uint32_t>(result);gHostFailureSite=site;
}
bool SameCommand(const route::Key& a,const route::Key& b) noexcept {
    return a.scope==b.scope&&a.owner==b.owner&&a.commandId==b.commandId;
}
void GlobalFailure(const transactions::Scope& scope,execution::Recovery reason) noexcept {
    ++gGlobalFailures;execution_runtime::Fail(scope,reason);
}
bool Blocked(const route::Key& key) noexcept {
    if(!gState)return false;
    const std::lock_guard lock(gState->gate);
    for(const auto& fault:gState->faults)if(fault.used&&SameCommand(fault.key,key))return true;
    return false;
}
bool RememberFault(const route::Key& key,route::Failure reason) noexcept {
    if(!gState||!route::ValidKey(key)||reason==route::Failure::None)return false;
    const std::lock_guard lock(gState->gate);
    for(const auto& fault:gState->faults)if(fault.used&&SameCommand(fault.key,key))return true;
    CommandFault* next{};
    for(auto& fault:gState->faults)if(!fault.used){next=&fault;break;}
    if(!next)return false; // Cannot silently evict a command's safety fence.
    *next={key,reason,true};++gCommandFaults;
    // Only this failed command's bytes are disposable. Compact the bounded
    // ring in place; unrelated actors retain their exact packets and order.
    std::size_t kept{};
    for(std::size_t i=0;i<gState->inCount;++i) {
        auto& packet=gState->incoming[(gState->inRead+i)%kPackets];route::Key pending{};
        if(route::DecodeKey(std::span(packet.bytes).first(packet.size),pending)==route::Result::Ok&&SameCommand(pending,key)){++gDiscarded;continue;}
        if(kept!=i)gState->incoming[(gState->inRead+kept)%kPackets]=packet;
        ++kept;
    }
    gState->inCount=kept;return true;
}
bool Mesh(mesh_storage::Lease& lease) noexcept {
    std::uintptr_t root{},manager{},mesh{};
    // 004551F0 constructs root+24; 00461280 uses root+2C for query dispatch.
    // The root is qualified by the embedding current engine/world callback.
    return Read(gBase+0x12F0C88U,root)&&root&&Read(root+0x24,manager)&&manager&&
        Read(manager+0x18,mesh)&&mesh&&mesh_storage::Acquire(manager,mesh,lease);
}
struct HostProtection {
    host_producer::CommandIdentity command{};CaptureContext local{};std::uint32_t part{};
    std::array<float,6> auxiliary{};std::array<std::uint32_t,4> statuses{};
};
struct HostMetadata {
    HostProtection value{};
    std::unique_ptr<route::Graph> complete;
    // Provisional observations are value-only and may be consumed only by the
    // producer's immediate, independently qualified post-Submit callback.
    std::uintptr_t start{},finish{},cursor{};
    std::array<float,3> goal{};
    bool used{},provisional{};
};
std::array<HostMetadata,host_producer::Producer::kCommands*2> gHostMetadata{};
bool SameHostPart(const HostProtection& a,const HostProtection& b) noexcept {
    return a.local.navigation==b.local.navigation&&a.local.block==b.local.block&&
        a.command.scope==b.command.scope&&a.command.owner==b.command.owner&&
        a.command.command==b.command.command&&a.part==b.part;
}
HostMetadata* FindHostMetadata(const HostProtection& pin) noexcept {
    for(auto& item:gHostMetadata)if(item.used&&!item.provisional&&SameHostPart(item.value,pin))return &item;
    return nullptr;
}
bool SaveHostMetadata(const HostProtection& pin) noexcept {
    HostMetadata* empty{};
    for(auto& item:gHostMetadata) {
        if(item.used&&(item.value.command.scope!=pin.command.scope))item={};
        if(item.used&&item.value.local.navigation==pin.local.navigation&&item.value.local.block==pin.local.block){
            if(!SameHostPart(item.value,pin))item={};
            item.value=pin;item.used=true;item.provisional=false;return true;
        }
        if(!item.used&&!empty)empty=&item;
    }
    if(!empty)return false;
    empty->value=pin;empty->used=true;return true;
}
struct HostAdmission {HostProtection pin{};std::uintptr_t result{};bool ready{},identified{};};
HostAdmission BeforeHostAdmission(std::uintptr_t n) noexcept {
    HostAdmission input{};auto& pin=input.pin;pin.local.navigation=n;std::uint32_t ready{},status{};
    if(!gEngine||!gEngine()||!Read(n+0x40,pin.local.block)||!Read(n+0x5C,input.result)||!input.result||
        !Read(input.result+4,ready)||ready!=1||!Read(input.result+0xC,status)||status!=1)return input;
    input.identified=host_producer::IdentifyNativeNavigation(n,pin.local.block,pin.command,pin.part);
    if(input.identified) {
        if(!gHost(pin.command.scope)||pin.command.navigation!=movement::Navigation::HostRoute)return input;
    } else if(!host_producer::HostScope(pin.command.scope)||!gHost(pin.command.scope))return input;
    pin.local.result=input.result;
    for(std::size_t i=0;i<6;++i)if(!Read(input.result+0x10+i*4,pin.auxiliary[i]))return input;
    for(std::size_t i=0;i<4;++i)if(!Read(input.result+0x28+i*4,pin.statuses[i]))return input;
    if(input.identified&&!SaveHostMetadata(pin)){static_cast<void>(host_producer::EngineProducer().BlockRoute(pin.command,pin.part,route::Failure::Capacity));return input;}
    input.ready=true;
    return input;
}
// Preserve the actual installed native corridor: element ordinals, portals,
// link kinds and ordered markers. Positions alone do not describe this route.
struct HostRead {const HostProtection* pin{};const mesh_storage::Lease* mesh{};};
struct HostRegistry {
    transactions::Scope scope{};
    std::unique_ptr<route_identity::Registry> value;
    std::uint64_t load{};
};
HostRegistry gHostRegistry;
bool HostGuard(void* opaque,const route::Key& key,const route::LocalContext& local,
    std::span<const route_identity::ElementMapping>,route::GuardPhase phase) noexcept {
    const auto& read=*static_cast<HostRead*>(opaque);const auto& pin=*read.pin;
    host_producer::CommandIdentity current{};std::uint32_t part{};
    return phase==route::GuardPhase::Read&&gEngine&&gEngine()&&gHost(pin.command.scope)&&
        mesh_storage::Current(*read.mesh)&&local.navigation==pin.local.navigation&&local.block==pin.local.block&&
        key.scope==pin.command.scope&&key.owner==pin.command.owner&&key.commandId==pin.command.command&&key.part==pin.part&&
        host_producer::IdentifyNativeNavigation(local.navigation,local.block,current,part)&&current==pin.command&&part==pin.part;
}
route::Result BuildHostPath(const HostProtection& pin,const mesh_storage::Lease& mesh,route::Graph& graph,bool complete) noexcept {
    if(!gHostRegistry.value||gHostRegistry.scope!=pin.command.scope) {
        gHostRegistry.value=std::make_unique<route_identity::Registry>();gHostRegistry.scope=pin.command.scope;gHostRegistry.load=0;
        if(gHostRegistry.value->BeginWorld(pin.command.scope.worldEpoch)!=route_identity::Result::Ok)return route::Result::LifetimeLost;
    }
    auto& registry=*gHostRegistry.value;
    if(gHostRegistry.load&&gHostRegistry.load!=mesh.load)
        static_cast<void>(registry.RevokeStorageLifetime(pin.command.scope.worldEpoch,gHostRegistry.load));
    gHostRegistry.load=mesh.load;
    route_identity::BindingIncarnation binding{};
    if(mesh_storage::BindOrdinary(registry,pin.command.scope.worldEpoch,mesh,binding)!=route_identity::Result::Ok)return route::Result::MissingResource;
    route::NativeRouteResult native;
    if(!native.Initialize(gBase,true,registry))return route::Result::NotInitialized;
    HostRead read{&pin,&mesh};route::Protection protection{mesh.storage,mesh.mesh,0,&HostGuard,&read};
    route::LocalContext local{pin.local.navigation,pin.local.block,0,0};
    route::Graph captured{};
    const auto result=native.CaptureInstalled(graph.key,local,protection,binding,pin.auxiliary,captured,complete);
    gHostCaptureCheck=native.ContextCheck();gHostCaptureNode=native.CaptureNode();
    gHostMarkerIndex=native.MarkerIndex();gHostMarkerKind=native.MarkerKind();gHostMarkerTypeRva=native.MarkerTypeRva();
    if(result!=route::Result::Ok)return result;
    captured.statuses=pin.statuses;
    const auto valid=route::Validate(captured);
    if(valid!=route::Result::Ok){gHostCaptureCheck=111;return valid;}
    graph=captured;return route::Result::Ok;
}
void PublishHostPath(const HostProtection& pin,std::uint32_t site,bool admission) noexcept {
    auto& source=host_producer::EngineProducer();
    mesh_storage::Lease mesh{};route::Graph graph{};
    // No flattened fallback: failure cannot silently start a Client solver.
    if(!Mesh(mesh)){HostCaptureFailure(pin.local,1,route::Result::MissingResource);++gBlocked;static_cast<void>(source.BlockRoute(pin.command,pin.part,route::Failure::Unavailable));return;}
    if(!source.NextRouteKey(pin.command,pin.part,graph.key)){HostCaptureFailure(pin.local,3,route::Result::WrongKey);++gBlocked;return;}
    auto* metadata=FindHostMetadata(pin);
    if(!metadata||(!admission&&!metadata->complete)) {
        gHostCaptureCheck=112;HostCaptureFailure(pin.local,site,route::Result::InvalidNativeState);++gBlocked;
        static_cast<void>(source.BlockRoute(pin.command,pin.part,route::Failure::Unavailable));return;
    }
    const auto captured=BuildHostPath(pin,mesh,graph,admission);
    // No route was rejected: the native replacement will publish at its own
    // admission boundary, or Complete will retire an already consumed B.
    if(captured==route::Result::Deferred){++gHostCaptureDeferred;return;}
    if(captured!=route::Result::Ok){HostCaptureFailure(pin.local,site,captured);++gBlocked;static_cast<void>(source.BlockRoute(pin.command,pin.part,route::Failure::Unsupported));return;}
    if(!admission) {
        route::Graph preserved{};
        if(!route::PreserveEndpointPrefix(*metadata->complete,graph,preserved)) {
            gHostCaptureCheck=113;HostCaptureFailure(pin.local,site,route::Result::LocalInvalidity);++gBlocked;
            static_cast<void>(source.BlockRoute(pin.command,pin.part,route::Failure::Unsupported));return;
        }
        graph=preserved;
    }
    if(source.AppendRoute(graph)==host_producer::SourceResult::Ok) {
        if(!metadata->complete)metadata->complete=std::make_unique<route::Graph>();
        *metadata->complete=graph;++gCaptured;
    }
    else{HostCaptureFailure(pin.local,6,route::Result::InvalidGraph);++gBlocked;}
}
void AfterHostAdmission(HostAdmission& input) noexcept {
    if(!input.ready)return;
    auto& pin=input.pin;const auto n=pin.local.navigation;
    std::uintptr_t result{};std::uint32_t state{};
    // Retail can defer admission (mode6). Publish once it installed this path.
    if(!Read(n+0x5C,result)||result||!Read(pin.local.block+0x40,state)||state!=3)return;
    // 004659D0 released its R. Only copied auxiliary/status values survive.
    pin.local.result=pin.local.resultControl=0;
    host_producer::CommandIdentity current{};std::uint32_t part{};
    if(!gHost(pin.command.scope))return;
    if(!input.identified) {
        // The producer has not published Submit yet. Never identify or retain
        // a native owner speculatively. Save scalar metadata at this seam and
        // let ObserveHostNavigation prove the owning lease after finalization.
        HostMetadata* pending{};
        for(auto& item:gHostMetadata) {
            if(item.used&&item.value.command.scope!=pin.command.scope)item={};
            if(item.used&&item.value.local.navigation==n&&item.value.local.block==pin.local.block){pending=&item;break;}
            if(!item.used&&!pending)pending=&item;
        }
        if(!pending){++gBlocked;return;}
        *pending={};pending->value=pin;pending->used=pending->provisional=true;
        if(!Read(pin.local.block+0x38,pending->start)||!Read(pin.local.block+0x3C,pending->finish)||
            !Read(n+0xC,pending->cursor)||!Read(pin.local.block,pending->goal)) {*pending={};return;}
        ++gHostCaptureDeferred;return;
    }
    if(!host_producer::IdentifyNativeNavigation(n,pin.local.block,current,part)||current!=pin.command||part!=pin.part)return;
    PublishHostPath(pin,5,true);
}
void CaptureHostGoalEdit(std::uintptr_t n,std::uintptr_t b,const std::array<float,3>& before) noexcept {
    std::array<float,3> after{};std::uint32_t state{};std::uintptr_t cursor{},sentinel{};
    if(!Read(b,after[0])||!Read(b+4,after[1])||!Read(b+8,after[2])||std::memcmp(before.data(),after.data(),sizeof(after))==0||
        !Read(b+0x40,state)||state!=3||!Read(n+0xC,cursor)||!Read(n,sentinel)||cursor==sentinel)return;
    HostProtection pin{};pin.local={n,b,0,0};
    if(!host_producer::IdentifyNativeNavigation(n,b,pin.command,pin.part)||!gHost(pin.command.scope)||
        pin.command.navigation!=movement::Navigation::HostRoute)return;
    bool found{};
    for(const auto& metadata:gHostMetadata)if(metadata.used&&!metadata.provisional&&metadata.value.local.navigation==n&&metadata.value.local.block==b&&
        metadata.value.command.scope==pin.command.scope&&metadata.value.command.owner==pin.command.owner&&
        metadata.value.command.command==pin.command.command) {
        pin.auxiliary=metadata.value.auxiliary;pin.statuses=metadata.value.statuses;found=true;break;
    }
    if(!found){HostCaptureFailure(pin.local,9,route::Result::InvalidNativeState);++gBlocked;static_cast<void>(host_producer::EngineProducer().BlockRoute(pin.command,pin.part,route::Failure::Unavailable));return;}
    // An in-place endpoint edit: republish the adjusted native corridor.
    ++gGoalDeferred;PublishHostPath(pin,8,false);
}
void __fastcall PendingGoalDetour(void* navigation,void*,void* block) {
    const auto original=reinterpret_cast<PendingGoalCall>(gGoalOriginal.load(std::memory_order_acquire));
    if(!original)return;
    const auto n=reinterpret_cast<std::uintptr_t>(navigation),b=reinterpret_cast<std::uintptr_t>(block);
    std::array<float,3> before{};transactions::Scope scope{};
    const bool observe=gEngine&&gEngine()&&gHost&&host_producer::HostScope(scope)&&gHost(scope)&&
        Read(b,before[0])&&Read(b+4,before[1])&&Read(b+8,before[2]);
    if(gEngine&&gEngine()&&route_execution::PendingEndpoint(n,b))return;
    original(navigation,block);
    if(observe)CaptureHostGoalEdit(n,b,before);
}
void __fastcall AdmissionDetour(void* navigation,void*) {
    auto host=BeforeHostAdmission(reinterpret_cast<std::uintptr_t>(navigation));
    const auto n=reinterpret_cast<std::uintptr_t>(navigation);
    const bool imported=route_execution::BeginAdmission(n);
    if(route_execution::Owns(n)&&!imported)return;
    reinterpret_cast<NavigationCall>(gBase+0x659D0U)(navigation);
    if(imported)route_execution::EndAdmission(n);
    AfterHostAdmission(host);
}
}
bool Install(std::uintptr_t base,HookManager& hooks,EngineThread engine,Qualify host,Qualify client,std::string& error) {
    if(gState||!base||!engine||!host||!client||!hooks.ExactBuildValidated())return false;
    auto state=std::make_unique<State>();gBase=base;gEngine=engine;gHost=host;gClient=client;gState=state.release();
    const auto before=hooks.InstalledCount();
    const auto redirect=[&](const char* name,std::uintptr_t site,std::uintptr_t target,void* replacement) {
        return hooks.InstallCallRedirect({name,reinterpret_cast<void*>(base+site),reinterpret_cast<void*>(base+target),replacement},error);
    };
    const auto entry=[&](const char* name,std::uintptr_t offset,void* detour,const std::vector<std::uint8_t>& prefix,
        void** trampoline,std::atomic<void*>* published) {
        HookRequest request{};request.name=name;request.target=reinterpret_cast<void*>(base+offset);request.detour=detour;
        request.patchLength=prefix.size();request.relocationSafePrefix=true;request.expected.bytes=prefix;request.expected.mask=std::string(prefix.size(),'x');
        request.trampolineOut=trampoline;request.publishedTrampolineOut=published;return hooks.Install(request,error);
    };
    if(!redirect("AISync engine navigation barrier",0x66638,0x5E9530,reinterpret_cast<void*>(&NavigationBatch))||
        !entry("AISync Host endpoint observation",0x4D320U,reinterpret_cast<void*>(&PendingGoalDetour),{0x83,0xEC,0x68,0x55,0x57},&gGoalTrampoline,&gGoalOriginal)||
        !redirect("AISync Host path admission observation",0x65E35,0x659D0,reinterpret_cast<void*>(&AdmissionDetour))||
        !route_execution::Install(base,hooks,engine,client,error)) {
        std::string rollback;const auto undone=hooks.UninstallFrom(before,rollback);if(!rollback.empty())error+="; "+rollback;
        if(undone){delete gState;gState=nullptr;}return false;
    }
    gAvailable.store(true,std::memory_order_release);return true;
}
bool Available() noexcept{return gAvailable.load(std::memory_order_acquire);}
std::uint64_t HostRoutesUnavailable() noexcept{return gHostRoutesUnavailable.load(std::memory_order_relaxed);}
// Native route pins remain owned until actual B removal and Ref-list retirement.
bool AwaitingHostRoute(std::uintptr_t n) noexcept{return route_execution::Awaiting(n);}
bool RetirementComplete(const transactions::Scope& scope) noexcept{return route_execution::Retired(scope);}
void Disable() noexcept{gAvailable.store(false,std::memory_order_release);}
void ObserveHostNavigation(std::uintptr_t n,std::uintptr_t b) noexcept {
    if(!Available()||!gEngine||!gEngine())return;
    host_producer::CommandIdentity identity{};std::uint32_t part{};
    // This is the first operation which can inspect a provisional candidate:
    // the producer still pins the exact request and independently proves M/N/B.
    if(!host_producer::IdentifyNativeNavigation(n,b,identity,part)||!gHost(identity.scope)||
        identity.navigation!=movement::Navigation::HostRoute)return;
    for(auto& item:gHostMetadata)if(item.used&&item.provisional&&item.value.local.navigation==n&&item.value.local.block==b) {
        auto pin=item.value;const auto start=item.start,finish=item.finish,cursor=item.cursor;const auto goal=item.goal;
        item={};
        if(pin.command.scope!=identity.scope)return;
        pin.command=identity;pin.part=part;
        std::uintptr_t actualStart{},actualFinish{},actualCursor{};std::array<float,3> actualGoal{};
        if(!Read(b+0x38,actualStart)||actualStart!=start||!Read(b+0x3C,actualFinish)||actualFinish!=finish||
            !Read(n+0xC,actualCursor)||actualCursor!=cursor||!Read(b,actualGoal)||actualGoal!=goal) {
            gHostCaptureCheck=114;HostCaptureFailure(pin.local,10,route::Result::LocalInvalidity);++gBlocked;
            static_cast<void>(host_producer::EngineProducer().BlockRoute(identity,part,route::Failure::Unavailable));return;
        }
        if(!SaveHostMetadata(pin)){++gBlocked;static_cast<void>(host_producer::EngineProducer().BlockRoute(identity,part,route::Failure::Capacity));return;}
        PublishHostPath(pin,10,true);return;
    }
}
bool QueueCurrent(std::span<const std::uint8_t> bytes,const route::Key& key) noexcept {
    if(Blocked(key)){++gDiscarded;return true;}
    route::Graph graph{};
    const auto decoded=route::Decode(bytes,key,graph);
    if(decoded!=route::Result::Ok) {
        static_cast<void>(RememberFault(key,route::Failure::InvalidStream));
        GlobalFailure(key.scope,execution::Recovery::NativeUnavailable);
        return true;
    }
    if(graph.failure!=route::Failure::None) {
        // A strict HostRoute cannot substitute a Client solve. Fence only the
        // failed command; independent actors and command lifecycles continue.
        static_cast<void>(RememberFault(key,graph.failure));++gHostRoutesUnavailable;
        return true;
    }
    bool accepted{},conflict{};
    {const std::lock_guard lock(gState->gate);
        std::size_t commandPackets{};
        for(std::size_t i=0;i<gState->inCount;++i) {
            const auto& packet=gState->incoming[(gState->inRead+i)%kPackets];route::Key queued{};
            if(route::DecodeKey(std::span(packet.bytes).first(packet.size),queued)==route::Result::Ok&&SameCommand(queued,key)) {
                ++commandPackets;
                if(queued==key) {
                    if(packet.size==bytes.size()&&std::equal(bytes.begin(),bytes.end(),packet.bytes.begin())){++gDiscarded;return true;}
                    conflict=true;
                }
            }
        }
        // A single absent/blocked actor must not consume the entire queue.
        if(!conflict&&gState->inCount<kPackets&&commandPackets<8) {
            auto& packet=gState->incoming[(gState->inRead+gState->inCount++)%kPackets];packet={};packet.size=bytes.size();
            std::copy(bytes.begin(),bytes.end(),packet.bytes.begin());accepted=true;
        }}
    if(!accepted) {
        static_cast<void>(RememberFault(key,conflict?route::Failure::InvalidStream:route::Failure::Capacity));
        GlobalFailure(key.scope,execution::Recovery::Capacity);
    }
    return true;
}
bool QueueClient(std::span<const std::uint8_t> bytes) noexcept {
    if(!Available()){++gQueueRejected;gLastQueueRejection=1;return false;}
    route::Key key{};
    if(route::DecodeKey(bytes,key)!=route::Result::Ok){++gQueueRejected;gLastQueueRejection=2;return false;}
    if(!execution_runtime::Matches(key.scope)){++gQueueRejected;gLastQueueRejection=3;return false;}
    const bool accepted=QueueCurrent(bytes,key);
    if(accepted)++gQueued;else{++gQueueRejected;gLastQueueRejection=4;}
    return accepted;
}
bool CommandBlocked(const route::Key& key,route::Failure& reason) noexcept {
    if(!gState)return false;
    const std::lock_guard lock(gState->gate);
    for(const auto& fault:gState->faults)if(fault.used&&SameCommand(fault.key,key)){reason=fault.reason;return true;}
    return false;
}
void Pump() noexcept {
    const profiling::Timer timing(profiling::Section::RoutePump);
    if(!gState||!gEngine||!gEngine())return;
    // Provisional metadata is synchronous only: a later frame must never
    // reinterpret an old N/B address as a new request incarnation.
    for(auto& metadata:gHostMetadata)if(metadata.provisional)metadata={};
    route_execution::Pump();
    {const std::lock_guard lock(gState->gate);
        for(auto& fault:gState->faults)if(fault.used&&!execution_runtime::Matches(fault.key.scope))fault={};}
    if(!Available()) {
        const std::lock_guard lock(gState->gate);
        gState->inRead=gState->inCount=0;
        return;
    }
    std::size_t budget{};
    {const std::lock_guard lock(gState->gate);budget=(std::min)(gState->inCount,std::size_t{32});}
    for(std::size_t i=0;i<budget;++i) {
        Packet packet{};
        {const std::lock_guard lock(gState->gate);
            if(!gState->inCount)break;
            packet=gState->incoming[gState->inRead];gState->inRead=(gState->inRead+1)%kPackets;--gState->inCount;
        }
        const auto bytes=std::span(packet.bytes).first(packet.size);
        route::Key key{};route::Graph graph{};
        if(route::DecodeKey(bytes,key)!=route::Result::Ok||!execution_runtime::Matches(key.scope)||!gClient(key.scope))continue;
        if(route::Decode(bytes,key,graph)!=route::Result::Ok||graph.failure!=route::Failure::None){++gDiscarded;continue;}
        if(route_execution::Offer(graph))++gImported;
        else {++gBlocked;static_cast<void>(RememberFault(key,route::Failure::Unsupported));}
    }
    route_execution::Pump();
}

bool PopClientFailure(ClientFailure& output) noexcept {
    const std::lock_guard lock(gFailureGate);
    if(!gFailureCount)return false;
    output=gFailures[gFailureRead];gFailureRead=(gFailureRead+1)%gFailures.size();--gFailureCount;return true;
}
std::uint64_t ClientFailuresDropped() noexcept{return gFailureDropped.load();}
Counters Diagnostics() noexcept{
    std::uint32_t queued{};if(gState){const std::lock_guard lock(gState->gate);queued=static_cast<std::uint32_t>(gState->inCount);}
    return {gCaptured.load(),gImported.load(),gBlocked.load(),gPending.load(),gRetained.load(),
    gInvalidations.load(),gReplanSuppressed.load(),gGoalDeferred.load(),gAwaitingHost.load(),gReplacements.load(),gAdmissions.load(),gSuperseded.load(),gCommandFaults.load(),gDiscarded.load(),gGlobalFailures.load(),
    gHostFailureSite.load(),gHostFailureResult.load(),gHostNavigationNodes.load(),gHostResultNodes.load(),
    gHostBlockState.load(),gHostBlockMode.load(),gHostStopBits.load(),gHostNodeType.load(),gHostMetadataBytes.load(),
    gQueued.load(),gQueueRejected.load(),gEngineQueries.load(),gWorkerQueries.load(),gStrictQueries.load(),gUnownedQueries.load(),gRequeues.load(),
    queued,gLastQueueRejection.load(),gLastQueryOwnership.load(),gLastQueryPolicy.load(),gLastQueryKind.load(),
    gHostCaptureDeferred.load(),gHostCaptureCheck.load(),gHostCaptureNode.load(),gHostMarkerIndex.load(),gHostMarkerKind.load(),gHostMarkerTypeRva.load()};}
}
