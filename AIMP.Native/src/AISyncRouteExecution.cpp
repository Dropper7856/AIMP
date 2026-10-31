#include "aimp/AISyncRouteExecution.h"
#include "aimp/AISyncRouteRuntime.h"
#include "aimp/AISyncRouteContract.h"
#include "aimp/AISyncExecutionRuntime.h"
#include "aimp/AISyncMeshStorage.h"
#include "aimp/HookManager.h"
#include "aimp/NativeResourceLease.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>

namespace aimp::ai_sync::route_execution {
namespace {
using namespace route;
constexpr std::size_t kCapacity=execution::NativeRequestAdapter::kCapacity;
std::uintptr_t gBase{};
EngineThread gEngine{};
Qualify gQualify{};
std::atomic<std::uint64_t> gSuppressed{},gPublished{},gInstalled{},gReplacements{},gRejected{},gRetired{};
std::atomic<std::uint64_t> gReplacementRequests{},gReplacementCommits{},gReplacementWaits{},gReplacementCommand{};
std::atomic<std::uint32_t> gReplacementReason{},gReplacementArea{},gReplacementExpected{},gReplacementParameters{},gReplacementRevision{};
std::atomic<std::uint64_t> gFailureCommand{};
constexpr ULONGLONG kRouteWaitMilliseconds=2000;
std::atomic<std::uint32_t> gFailureSite{},gFailureResult{},gImportStage{},gContextCheck{};
std::atomic<std::uint64_t> gAppendPublished{},gContinuationPublished{},gCompoundRequests{};
std::atomic<std::uint32_t> gLastAdmissionMode{},gLastIncomingCount{},gLastCompoundParts{};
bool gPumping{};
template<class T> bool Read(std::uintptr_t at,T& value) noexcept {
    return crash_guard::TryCopy(&value,reinterpret_cast<const void*>(at),sizeof(value));
}
bool SameCommand(const Key& a,const Key& b) noexcept {
    return a.scope==b.scope&&a.owner==b.owner&&a.commandId==b.commandId&&a.part==b.part;
}
struct Pin {
    mesh_storage::Lease mesh;
    std::unique_ptr<native_resource::StrongLease> movement;
};
struct Slot {
    execution::NavigationBinding binding{};
    LocalContext local{};
    std::shared_ptr<Pin> pin;
    std::unique_ptr<native_resource::StrongLease> result;
    std::unique_ptr<native_resource::StrongLease> previousResult;
    route_identity::Registry registry;
    route_identity::BindingIncarnation meshBinding{};
    NativeRouteResult writer;
    std::unique_ptr<Graph> desired;
    // Exact graphs staged for the whole native reset footprint. Continuations
    // retain the same Host identity; they are never new route decisions.
    std::unique_ptr<Graph> planned;
    bool continuation{};
    Protection protection{};
    bool skipped{},failed{},installing{},actorDestroyed{};
    bool installed{},startValid{},retiring{},retirementFault{},rejectionQueued{},replacementRequested{};
    std::array<float,3> queryStart{};
    std::array<std::uint32_t,43> queryOptions{};
    std::uint32_t startCheck{};
    ULONGLONG waitingSince{};
};
struct Cached {std::unique_ptr<Graph> graph;};
struct Hybrid {bool used{};execution::NavigationBinding binding{};};
struct State {
    std::array<std::unique_ptr<Slot>,kCapacity> slots;
    std::array<Cached,kCapacity> cache;
    std::array<Hybrid,kCapacity> hybrid;
};
State* gState{}; // Unresolved native pins are never destroyed by DLL static teardown.
struct Frame {std::uintptr_t navigation{},block{};std::array<float,3> start{};bool startValid{};Frame* previous{};};
thread_local Frame* gFrame{};
thread_local Slot* gAdmission{};
struct FrameScope {
    Frame value;
    FrameScope(std::uintptr_t n,std::uintptr_t b,std::uintptr_t start):value{} {
        value.navigation=n;value.block=b;value.startValid=Read(start,value.start);value.previous=gFrame;gFrame=&value;
    }
    ~FrameScope(){gFrame=value.previous;}
};
bool Qualified(const execution::NavigationBinding& binding) noexcept {
    return gEngine&&gEngine()&&gQualify&&gQualify(binding.command.scope)&&
        binding.policy==movement::Navigation::HostRoute;
}
bool Current(Slot& slot,execution::NavigationBinding* output=nullptr) noexcept {
    execution::NavigationBinding actual{};
    if(!Qualified(slot.binding))return false;
    const auto state=execution_runtime::IdentifyNavigation(slot.local.navigation,slot.local.block,actual);
    if((state!=execution::NavigationOwnership::Ready&&state!=execution::NavigationOwnership::Admitting)||
        actual.command.scope!=slot.binding.command.scope||actual.command.owner!=slot.binding.command.owner||
        actual.command.command!=slot.binding.command.command||actual.command.actor!=slot.binding.command.actor||
        actual.part!=slot.binding.part||actual.policy!=movement::Navigation::HostRoute)return false;
    if(output)*output=actual;
    return slot.pin&&slot.pin->movement&&*slot.pin->movement&&
        slot.pin->movement->Object()+0xC4==slot.local.navigation&&mesh_storage::Current(slot.pin->mesh);
}
bool EmptyRing(std::uintptr_t n,std::uint32_t offset) noexcept {
    std::uintptr_t h{},first{},last{};std::uint32_t count{};
    return Read(n+offset,h)&&h&&Read(n+offset+4,count)&&!count&&Read(h,first)&&first==h&&Read(h+4,last)&&last==h;
}
bool HistoryRing(std::uintptr_t n,std::uint32_t offset=0x10) noexcept {
    std::uintptr_t head{},at{},previous{},last{};std::uint32_t count{};
    if(!Read(n+offset,head)||!head||!Read(head,at)||!Read(head+4,last)||!Read(n+offset+4,count)||count>kMaxNodes*2+20)return false;
    previous=head;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t prior{},next{},object{},control{};LONG strong{};
        if(!at||at==head||!Read(at+4,prior)||prior!=previous||!Read(at,next)||
            !Read(at+8,object)||!object||!Read(at+12,control)||!control||!Read(control+4,strong)||strong<=0)return false;
        previous=at;at=next;
    }
    return at==head&&previous==last;
}
bool Quiet(Slot& slot,bool history=true) noexcept {
    if(!slot.skipped||!slot.pin||!slot.pin->movement||!*slot.pin->movement||
        slot.pin->movement->Object()+0xC4!=slot.local.navigation)return false;
    const auto n=slot.local.navigation;
    std::uintptr_t head{},cursor{};
    if(!EmptyRing(n,0)||!EmptyRing(n,0x28)||!EmptyRing(n,0x34)||
        !Read(n,head)||!Read(n+0xC,cursor)||head!=cursor)return false;
    for(const auto offset:{0x24U,0x40U,0x44U,0x5CU,0x60U}) {
        std::uintptr_t value{};if(!Read(n+offset,value)||value)return false;
    }
    return history?EmptyRing(n,0x10):HistoryRing(n);
}
bool Guard(void* context,const Key& key,const LocalContext& local,
    std::span<const route_identity::ElementMapping>,GuardPhase phase) noexcept {
    auto& slot=*static_cast<Slot*>(context);
    if(!gEngine||!gEngine()||!slot.pin||!slot.pin->movement||!*slot.pin->movement||
        local.navigation!=slot.pin->movement->Object()+0xC4||
        key.scope!=slot.binding.command.scope||key.owner!=slot.binding.command.owner||
        key.commandId!=slot.binding.command.command||key.part!=slot.binding.part)return false;
    if(phase==GuardPhase::Retire)return Quiet(slot);
    if(!slot.skipped||slot.failed||slot.retiring||slot.retirementFault||!Current(slot))return false;
    // Remapping is read-only and owns no native B mutation. A neighbouring B
    // may still be retiring; requiring its execution lease here turned that
    // ordinary boundary into a terminal stream failure. Admission/invalidation
    // below still qualify EVERY registered B before CATHODE can visit them.
    if(phase==GuardPhase::Preflight)return local==slot.local&&mesh_storage::Current(slot.pin->mesh);
    // The native installer can visit every registered B. Qualify the entire
    // compound request, not just the particular part whose route arrived.
    std::uintptr_t head{},at{};std::uint32_t count{};
    if(!Read(local.navigation+0x28,head)||!head||!Read(head,at)||
        !Read(local.navigation+0x2C,count)||!count||count>128)return false;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t block{},next{};execution::NavigationBinding owned{};
        if(!at||at==head||!Read(at+8,block)||!Read(at,next))return false;
        const auto state=execution_runtime::IdentifyNavigation(local.navigation,block,owned);
        if((state!=execution::NavigationOwnership::Ready&&state!=execution::NavigationOwnership::Admitting)||
            owned.policy!=movement::Navigation::HostRoute||owned.command.scope!=key.scope||owned.command.owner!=key.owner||
            owned.command.actor!=slot.binding.command.actor)return false;
        at=next;
    }
    if(at!=head)return false;
    if(phase==GuardPhase::Rebind)return local.navigation==slot.local.navigation&&local.block==slot.local.block;
    return local==slot.local||(phase==GuardPhase::Read&&slot.installed&&
        local.navigation==slot.local.navigation&&local.block==slot.local.block&&!local.result&&!local.resultControl);
}
bool Reject(const execution::NavigationBinding& binding) noexcept {
    // A route may fail after native cancellation already invalidated its
    // executable binding. Remember the Host-authorized failure independently
    // of the temporary route/lease qualification: otherwise the failed request
    // is retired before the client fallback can ever observe it.
    // BeginScope/actor destruction revoke the remembered identity.
    if(gState&&binding.policy==movement::Navigation::HostRoute&&
        binding.command.command!=0U&&binding.command.actor.character!=0U&&
        binding.command.actor.creationSerial!=0U) {
        Hybrid* available{};bool found{};
        for(auto& entry:gState->hybrid) {
            if(entry.used&&!gQualify(entry.binding.command.scope))entry={};
            if(entry.used&&entry.binding.command.scope==binding.command.scope&&
                entry.binding.command.owner==binding.command.owner&&entry.binding.command.actor==binding.command.actor) {
                if(HybridResumeCurrent(entry.binding.command.command,entry.binding.command.revision,
                    binding.command.command,binding.command.revision))entry.binding=binding;
                found=true;break;
            }
            if(!entry.used&&!available)available=&entry;
        }
        if(!found&&available)*available={true,binding};
    }
    // This queues exact-command retirement; it must not close other actors.
    return execution_runtime::RejectNavigation(binding);
}
void Fail(Slot& slot,std::uint32_t site=__builtin_LINE(),Result result=Result::InvalidNativeState) noexcept {
    if(!slot.failed) {
        slot.failed=true;++gRejected;gFailureCommand=slot.binding.command.command;
        gFailureSite=site;gFailureResult=static_cast<std::uint32_t>(result);
        gImportStage=slot.writer.ImportStage();
        gContextCheck=result==Result::LocalInvalidity&&slot.startCheck?slot.startCheck:slot.writer.ContextCheck();
    }
    if(!slot.rejectionQueued) {
        execution::NavigationBinding current{};
        // An Update advances the lease revision without replacing its B.
        if(Current(slot,&current))slot.binding=current;
        // Every refusal, including a populated slot's preflight/start/timeout
        // failure, must publish recovery permission BEFORE cancellation can
        // remove the request. Calling the runtime directly bypassed recovery.
        slot.rejectionQueued=Reject(slot.binding);
    }
}
bool Mesh(mesh_storage::Lease& lease) noexcept {
    std::uintptr_t root{},manager{},mesh{};
    return Read(gBase+0x12F0C88U,root)&&root&&Read(root+0x24,manager)&&manager&&
        Read(manager+0x18,mesh)&&mesh&&mesh_storage::Acquire(manager,mesh,lease);
}
bool SameRequest(const execution::NavigationBinding& a,const execution::NavigationBinding& b) noexcept {
    // B is embedded in a ref-counted request and its address can be recycled
    // before N's route/history becomes quiet. Revision is deliberately absent:
    // an Update advances the same command rather than constructing another B.
    return a.command.scope==b.command.scope&&a.command.owner==b.command.owner&&
        a.command.command==b.command.command&&a.command.actor==b.command.actor&&a.part==b.part&&a.policy==b.policy;
}
Slot* Find(std::uintptr_t n,std::uintptr_t b=0,const execution::NavigationBinding* binding=nullptr) noexcept {
    if(gState)for(auto& slot:gState->slots)if(slot&&slot->local.navigation==n&&(!b||slot->local.block==b)&&
        (!binding||SameRequest(slot->binding,*binding)))return slot.get();
    return nullptr;
}
Slot* ResultOwner(std::uintptr_t n,std::uintptr_t result) noexcept {
    if(gState)for(auto& slot:gState->slots)if(slot&&slot->local.navigation==n&&slot->local.result==result)return slot.get();
    return nullptr;
}
const Graph* Latest(const execution::NavigationBinding& binding) noexcept {
    if(gState)for(const auto& cached:gState->cache)if(cached.graph) {
        const auto& key=cached.graph->key;
        if(key.scope==binding.command.scope&&key.owner==binding.command.owner&&key.commandId==binding.command.command&&
            key.part==binding.part&&key.parameterRevision==binding.command.revision)return cached.graph.get();
    }
    return nullptr;
}
bool AreaSegment(std::uintptr_t mesh,std::uint32_t reference,const std::array<float,3>& from,
    const std::array<float,3>& to,const std::array<std::uint32_t,43>& options) noexcept {
    // Exact00443330 builds this same bounded ordinary polygon before clipping
    // and tracing. Reject entry from outside before invoking its ray walk.
    __try {
        std::uintptr_t slot{},element{},header{},vertices{};std::uint32_t vertexCount{},flags{};std::uint8_t count{};
        using Resolve=std::uint32_t(__thiscall*)(void*,std::uint32_t,std::uintptr_t*,std::uintptr_t*,void*);
        if(reinterpret_cast<Resolve>(gBase+0x3DC70U)(reinterpret_cast<void*>(mesh),reference,&slot,&element,nullptr)!=0x40000000U||
            !slot||!element||!Read(element+0x20,flags)||(flags&1U)||!Read(element+0x1C,count)||count<3||count>6||
            !Read(slot+8,header)||!header||!Read(header+0x1C,vertexCount)||!Read(slot+0x14,vertices)||!vertices)return false;
        std::array<float,18> polygon{};
        for(std::size_t i=0;i<count;++i) {
            std::uint16_t index{};std::array<float,3> point{};
            if(!Read(element+4+i*2,index)||index>=vertexCount||!Read(vertices+index*12,point))return false;
            for(std::size_t j=0;j<3;++j) {
                if(!std::isfinite(point[j])||!std::isfinite(from[j])||!std::isfinite(to[j]))return false;
                polygon[i*3+j]=point[j];
            }
        }
        float entry{},exit{};int entryEdge{},exitEdge{};
        using Clip=std::uint32_t(__cdecl*)(const float*,const float*,const float*,int,float*,float*,int*,int*);
        const auto clipped=reinterpret_cast<Clip>(gBase+0x3D330U)(from.data(),to.data(),polygon.data(),count,&entry,&exit,&entryEdge,&exitEdge);
        if(!FullAreaSegment(clipped,entry,exit))return false;
        std::array<std::uint32_t,2> references{};float reach{};int size{};
        using Trace=std::uint32_t(__cdecl*)(void*,std::uint32_t,std::uint32_t,const float*,const float*,const void*,float*,float*,std::uint32_t*,int*,int,float*);
        const auto status=reinterpret_cast<Trace>(gBase+0x43330U)(reinterpret_cast<void*>(mesh),reference,reference,
            from.data(),to.data(),options.data(),&reach,nullptr,references.data(),&size,2,nullptr);
        return size==2&&SameAreaTrace(status,reach,reference,references);
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool StartsHere(Slot& slot,const Graph& graph,bool continuation=false) noexcept {
    slot.startCheck=201;
    if(!slot.startValid||!graph.nodeCount||graph.nodes[0].type==1)return false;
    route_identity::ElementMapping first{};
    slot.startCheck=202;
    if(slot.registry.MapElement({graph.resources[graph.nodes[0].resourceIndex],graph.nodes[0].ordinal},first)!=route_identity::Result::Ok)return false;
    slot.startCheck=203;if(slot.queryOptions[0x94/4]!=first.localReference)return false;
    slot.startCheck=204;if(slot.registry.Revalidate(first)!=route_identity::Result::Ok)return false;
    std::array<float,3> hostStart{},firstPosition{},original{},projected{},envelope{};
    for(std::size_t i=0;i<3;++i) {
        firstPosition[i]=graph.nodes[0].vectors[i];
        hostStart[i]=continuation?firstPosition[i]:graph.auxiliary[i];
        original[i]=std::bit_cast<float>(slot.queryOptions[0x74/4+i]);
        projected[i]=std::bit_cast<float>(slot.queryOptions[0x5C/4+i]);
        envelope[i]=std::bit_cast<float>(slot.queryOptions[0x9C/4+i]);
        slot.startCheck=205;
        if(!std::isfinite(envelope[i])||envelope[i]<0||!std::isfinite(projected[i])||original[i]!=slot.queryStart[i])return false;
    }
    // O+74 is retained pre-projection query start (00449750); R+10 carries
    // that Host value (00463420), not necessarily the first installed node.
    // Vertical admissibility uses the native query envelope, not a new radius.
    slot.startCheck=206;
    if(std::fabs(original[1]-projected[1])>envelope[1]||
        std::fabs(hostStart[1]-projected[1])>envelope[1]||std::fabs(firstPosition[1]-projected[1])>envelope[1])return false;
    slot.startCheck=207;if(!AreaSegment(slot.protection.mesh,first.localReference,original,hostStart,slot.queryOptions))return false;
    slot.startCheck=208;if(!AreaSegment(slot.protection.mesh,first.localReference,hostStart,firstPosition,slot.queryOptions))return false;
    slot.startCheck=209;
    if(slot.registry.Revalidate(first)!=route_identity::Result::Ok||!mesh_storage::Current(slot.pin->mesh))return false;
    slot.startCheck=0;return true;
}
// These writes consume the Host's endpoint command, not a selected Client
// waypoint. 0044D320 would otherwise choose a local end polygon and edit N.
bool Endpoint(std::uintptr_t b,const std::array<float,3>& goal) noexcept {
    __try {
        for(std::size_t i=0;i<3;++i)*reinterpret_cast<float*>(b+i*4)=goal[i];
        *reinterpret_cast<std::uint8_t*>(b+0x90)&=0xBFU;
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool WriteEndpoint(std::uintptr_t block,std::uintptr_t node,const Graph& graph) noexcept {
    __try {
        for(std::size_t i=0;i<9;++i)*reinterpret_cast<float*>(node+0xC+i*4)=graph.nodes[graph.nodeCount-1].vectors[i];
        for(std::size_t i=0;i<3;++i)*reinterpret_cast<float*>(block+i*4)=graph.goal[i];
        *reinterpret_cast<std::uint8_t*>(block+0x90)&=0xBFU;
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ApplyEndpoint(Slot& slot,const Graph& graph) noexcept {
    if(!slot.installed||!slot.desired||!EndpointRevision(*slot.desired,graph))return false;
    std::uint32_t stop{};
    if(!Read(slot.local.block+0x30,stop)||!StopTrimDisabled(stop))return false;
    // Read the Client's actual remaining native B interval. It must still be
    // an exact suffix of the previously imported Host contract. This is native
    // progress verification, not a nearest waypoint/path selection.
    Graph remaining{},verified{};
    const LocalContext local{slot.local.navigation,slot.local.block,0,0};
    const auto captured=slot.writer.CaptureInstalled(slot.desired->key,local,slot.protection,slot.meshBinding,slot.desired->auxiliary,remaining);
    if(captured!=Result::Ok)return false;
    remaining.statuses=slot.desired->statuses;
    if(!PreserveEndpointPrefix(*slot.desired,remaining,verified)||verified!=*slot.desired)return false;
    if(slot.writer.Preflight(graph,slot.local,slot.protection)!=Result::Ok||
        !Guard(&slot,graph.key,slot.local,{},GuardPhase::Read))return false;
    std::uintptr_t terminal{},node{};
    if(!Read(slot.local.block+0x3C,terminal)||!terminal||!Read(terminal+8,node)||!node)return false;
    // Exact0044D320 same-handle branch: mutate terminal vectors and B.goal.
    // Preserve B distances, N cursor, history, world token and locomotion.
    if(!WriteEndpoint(slot.local.block,node,graph)){Fail(slot,__LINE__,Result::NativeFault);return false;}
    slot.desired=std::make_unique<Graph>(graph);++gReplacements;return true;
}
bool RemainingHostRange(Slot& slot,Graph& remaining) noexcept {
    if(!slot.installed||!slot.desired)return false;
    const LocalContext local{slot.local.navigation,slot.local.block,0,0};
    if(slot.writer.CaptureInstalled(slot.desired->key,local,slot.protection,slot.meshBinding,
        slot.desired->auxiliary,remaining)!=Result::Ok)return false;
    remaining.statuses=slot.desired->statuses;
    Graph verified{};
    return PreserveEndpointPrefix(*slot.desired,remaining,verified)&&verified==*slot.desired;
}
bool NativeInvalidate(std::uintptr_t navigation) noexcept {
    // Exact0044D200: ECX=N, no stack arguments, RET at0044D314.
    // CATHODE keeps its existing nodes until004659D0 replaces the range.
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x4D200U)(reinterpret_cast<void*>(navigation));
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool RequestReplacement(Slot& slot,const Graph& graph) noexcept {
    if(slot.replacementRequested||slot.planned||!slot.installed||!slot.desired||
        !NewerRoute(slot.desired->key,graph.key)||!Current(slot)||
        !Guard(&slot,graph.key,slot.local,{},GuardPhase::Read))return false;
    const auto preflight=slot.writer.Preflight(graph,slot.local,slot.protection);
    if(preflight!=Result::Ok){Fail(slot,__LINE__,preflight);return false;}
    Graph remaining{};
    if(!RemainingHostRange(slot,remaining)){Fail(slot,__LINE__,Result::LocalInvalidity);return false;}
    route_identity::ElementMapping first{};std::uint32_t currentArea{},phase{},reason{};
    std::uintptr_t pending{},active{},previous{};
    if(!graph.nodeCount||graph.nodes[0].type==1||
        slot.registry.MapElement({graph.resources[graph.nodes[0].resourceIndex],graph.nodes[0].ordinal},first)!=route_identity::Result::Ok||
        !Read(slot.pin->movement->Object()+0x1C4,currentArea)||!Read(slot.local.block+0x40,phase)||
        !Read(slot.local.block+0x44,reason)||!Read(slot.local.navigation+0x5C,pending)||
        !Read(slot.local.navigation+0x40,active)||!Read(slot.local.navigation+0x44,previous)) {
        Fail(slot,__LINE__,Result::LocalInvalidity);return false;
    }
    // If behind, keep executing the already admitted Host corridor. Do not
    // invent a connecting path or reset to the first node of the new route.
    // A matching area alone grants no entry: the fresh native query below
    // still proves both fixed segments with StartsHere before publication.
    gReplacementCommand=graph.key.commandId;gReplacementParameters=graph.key.parameterRevision;
    gReplacementRevision=graph.key.routeRevision;gReplacementArea=currentArea;gReplacementExpected=first.localReference;
    if(phase!=3||reason==3||pending||active||previous) {gReplacementReason=2U;++gReplacementWaits;return false;}
    // 0044D200 visits the entire B ring. Retain every unchanged part's exact
    // remaining Host suffix BEFORE that call, rather than replaying its prefix
    // or assigning a fabricated route revision to make Rearm accept it.
    struct Planned {Slot* slot{};std::unique_ptr<Graph> graph;bool continuation{};};
    std::array<Planned,kCapacity> plans{};
    std::array<std::uintptr_t,kCapacity> blocks{};
    std::uintptr_t head{},at{};std::uint32_t count{};std::size_t live{};
    if(!Read(slot.local.navigation+0x28,head)||!head||!Read(head,at)||
        !Read(slot.local.navigation+0x2C,count)||count>plans.size())return false;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t b{},next{};std::uint32_t retired{};
        if(!at||at==head||!Read(at+8,b)||!Read(at,next)||!Read(b+0x44,retired))return false;
        at=next;if(retired==3)continue;
        execution::NavigationBinding binding{};
        const auto ownership=execution_runtime::IdentifyNavigation(slot.local.navigation,b,binding);
        auto* other=Find(slot.local.navigation,b,&binding);
        if((ownership!=execution::NavigationOwnership::Ready&&ownership!=execution::NavigationOwnership::Admitting)||
            !other||!other->installed||!other->desired||other->failed||other->replacementRequested||other->planned||
            binding.command.scope!=slot.binding.command.scope||binding.command.owner!=slot.binding.command.owner||
            binding.command.actor!=slot.binding.command.actor||!Current(*other)) {
            gReplacementReason=3U;++gReplacementWaits;return false;
        }
        auto& plan=plans[live];plan.slot=other;blocks[live++]=b;
        const auto* latest=Latest(binding);
        if(other==&slot)latest=&graph;
        if(latest&&NewerRoute(other->desired->key,latest->key)) {
            if(other->writer.Preflight(*latest,other->local,other->protection)!=Result::Ok)return false;
            Graph retained{};if(!RemainingHostRange(*other,retained))return false;
            plan.graph=std::make_unique<Graph>(*latest);
        } else {
            plan.graph=std::make_unique<Graph>();plan.continuation=true;
            const LocalContext installed{other->local.navigation,other->local.block,0,0};
            if(other->writer.RetainContinuation(*other->desired,installed,other->protection,*plan.graph)!=Result::Ok) {
                gReplacementReason=4U;++gReplacementWaits;return false;
            }
        }
    }
    if(at!=head||!live||!ReplacementAffectsOnlyBlocks(slot.local.navigation,std::span(blocks).first(live))) {
        gReplacementReason=3U;++gReplacementWaits;return false;
    }
    for(std::size_t i=1;i<live;++i) {
        std::uint8_t flags{},priorFlags{};
        if(!Read(plans[i].slot->local.block+0x90,flags)||!Read(plans[i-1].slot->local.block+0x90,priorFlags))return false;
        if(!(flags&1U)&&!(priorFlags&0x80U)&&!AppendSharesTail(*plans[i-1].graph,*plans[i].graph)) {
            // Wait for a compatible Host graph before invalidating anything.
            gReplacementReason=5U;++gReplacementWaits;return false;
        }
    }
    // The first live part is restarted from the native cursor, later parts
    // start at the preceding native tail. Only the first needs the actor's
    // current area; every fresh R separately proves its actual query segment.
    auto& leading=plans[0];route_identity::ElementMapping leadingArea{};
    const auto& leadingNode=leading.graph->nodes[0];
    if(leadingNode.type==1||leading.slot->registry.MapElement(
        {leading.graph->resources[leadingNode.resourceIndex],leadingNode.ordinal},leadingArea)!=route_identity::Result::Ok||
        currentArea!=leadingArea.localReference) {gReplacementReason=1U;++gReplacementWaits;return false;}
    if(slot.registry.Revalidate(first)!=route_identity::Result::Ok||!Current(slot))return false;
    gReplacementReason=0U;++gReplacementRequests;
    gLastCompoundParts=static_cast<std::uint32_t>(live);if(live>1)++gCompoundRequests;
    for(std::size_t i=0;i<live;++i) {
        auto& plan=plans[i];plan.slot->planned=std::move(plan.graph);
        plan.slot->continuation=plan.continuation;plan.slot->replacementRequested=true;
    }
    // Fence the complete group before the first native callback can reenter.
    for(std::size_t i=0;i<live;++i)if(!Endpoint(plans[i].slot->local.block,plans[i].slot->planned->goal)) {
        for(std::size_t j=0;j<live;++j)Fail(*plans[j].slot,__LINE__,Result::NativeFault);
        return false;
    }
    if(!NativeInvalidate(slot.local.navigation)) {
        for(std::size_t i=0;i<live;++i)Fail(*plans[i].slot,__LINE__,Result::NativeFault);
        return false;
    }
    std::uint16_t reset{};
    if(!Current(slot)||!Read(slot.local.navigation+0x48,reset)||reset!=0x101U||
        !Read(slot.local.block+0x40,phase)||!Read(slot.local.block+0x44,reason)) {
        for(std::size_t i=0;i<live;++i)Fail(*plans[i].slot,__LINE__,Result::LocalInvalidity);
        return false;
    }
    for(std::size_t i=0;i<live;++i) {
        auto& part=*plans[i].slot;
        if(!Current(part)||!Read(part.local.block+0x40,phase)||!Read(part.local.block+0x44,reason)||
            (reason!=3&&(phase!=1||reason))) {
            for(std::size_t j=0;j<live;++j)Fail(*plans[j].slot,__LINE__,Result::LocalInvalidity);
            return false;
        }
        // Retail invalidation may finish an already reached part. Let its
        // native removal proceed; never force it back into state1.
        if(reason==3){part.planned.reset();part.continuation=false;part.replacementRequested=false;}
    }
    return true;
}
bool Populate(Slot& slot) noexcept {
    execution::NavigationBinding current{};
    if(slot.failed||!slot.result||!Current(slot,&current)||execution::MovementExecutionOwned(slot.binding.command.actor.character))return false;
    const auto* graph=slot.planned?slot.planned.get():Latest(current);
    if(!graph)return false;
    // A previous B may remain in the native list until its queued removal.
    // Waiting here is bounded: R is retained but no route node was published.
    if(!Guard(&slot,graph->key,slot.local,{},GuardPhase::Read))return false;
    // A new result is not permission to rewind the installed graph. It needs
    // a genuinely newer Host admission and the native query's actual start.
    const bool replacing=slot.writer.Entered();
    if(replacing&&(!slot.desired||(!slot.continuation&&!NewerRoute(slot.desired->key,graph->key))))return false;
    std::uintptr_t r{};std::uint32_t ready{};
    if(!Read(slot.local.navigation+0x5C,r)||r!=slot.local.result||!Read(r+4,ready)||ready)return false;
    slot.protection.localWorldRevision=0;
    std::uintptr_t root{},scheduler{},manager{};
    if(!Read(gBase+0x12F0C88U,root)||!root||!Read(root+0x2C,scheduler)||!scheduler||
        !Read(scheduler+0x38,manager)||!manager) {Fail(slot);return false;}
    // Exact 00455350 token source: scheduler+38 points at the manager CS;
    // its +3C counter is copied to R+08 before ready is published.
    EnterCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(manager));
    const bool token=Read(manager+0x3C,slot.protection.localWorldRevision);
    LeaveCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(manager));
    if(!token||!Current(slot)){Fail(slot);return false;}
    // R+08 is the mesh manager's invalidation token, not a transported epoch.
    const auto checked=slot.writer.Preflight(*graph,slot.local,slot.protection);
    if(checked!=Result::Ok){Fail(slot,__LINE__,checked);return false;}
    AdmissionPlan admission{};
    slot.startCheck=0;
    const auto certified=slot.writer.CertifyAdmission(*graph,slot.local,slot.protection,admission);
    if(certified!=Result::Ok){Fail(slot,__LINE__,certified);return false;}
    // A natural append shares exactly the old native tail. CertifyAdmission
    // proves that identity and leaves the earlier range/cursor untouched.
    // For a retained continuation, the first point is the certified cursor
    // suffix, not the original Host query start (which is now behind us).
    if(!StartsHere(slot,*graph,slot.continuation)) {Fail(slot,__LINE__,Result::LocalInvalidity);return false;}
    if(replacing) {
        const auto armed=slot.continuation?slot.writer.RearmContinuation(*graph,slot.local,slot.protection):
            slot.writer.Rearm(slot.writer.Identity(),*graph,slot.local,slot.protection);
        if(armed!=Result::Ok){Fail(slot,__LINE__,armed);return false;}
    }
    if(!slot.continuation&&(!slot.desired||slot.desired->key!=graph->key))slot.desired=std::make_unique<Graph>(*graph);
    // Native admission sees exactly this graph's parameter revision and goal.
    if(!Endpoint(slot.local.block,graph->goal)){Fail(slot,__LINE__,Result::NativeFault);return false;}
    const auto result=slot.writer.Populate(*graph,slot.local,slot.protection,ImportPoint::ReplacementNoWorker);
    if(result!=Result::Ok){Fail(slot,__LINE__,result);return false;}
    gLastAdmissionMode=static_cast<std::uint32_t>(admission.mode);gLastIncomingCount=admission.incomingCount;
    if(admission.mode==AdmissionMode::Append)++gAppendPublished;
    if(slot.continuation)++gContinuationPublished;
    ++gPublished;return true;
}
using Query=void(__thiscall*)(void*,void*,void*,void*,void*);
void __fastcall QueryFrame(void* n,void*,void* start,void* goal,void* b,void* previous) {
    if(!gEngine||!gEngine())return reinterpret_cast<Query>(gBase+0x61280U)(n,start,goal,b,previous);
    FrameScope frame(reinterpret_cast<std::uintptr_t>(n),reinterpret_cast<std::uintptr_t>(b),reinterpret_cast<std::uintptr_t>(start));
    reinterpret_cast<Query>(gBase+0x61280U)(n,start,goal,b,previous);
}
void __fastcall Dispatch(void* scheduler,void*,void* parameters,void* reference) {
    using Native=void(__thiscall*)(void*,void*,void*);
    if(!gEngine||!gEngine()||!gFrame)return reinterpret_cast<Native>(gBase+0x570D0U)(scheduler,parameters,reference);
    execution::NavigationBinding binding{};
    const auto ownership=execution_runtime::IdentifyNavigation(gFrame->navigation,gFrame->block,binding);
    auto* retained=Find(gFrame->navigation);
    if(!retained&&(ownership==execution::NavigationOwnership::Unowned||binding.policy!=movement::Navigation::HostRoute))
        return reinterpret_cast<Native>(gBase+0x570D0U)(scheduler,parameters,reference);
    ++gSuppressed;
    if((ownership!=execution::NavigationOwnership::Ready&&ownership!=execution::NavigationOwnership::Admitting)||!Qualified(binding)) {
        if(retained)Fail(*retained);else {++gRejected;Reject(binding);}return;
    }
    // IdentifyNavigation has proved the CURRENT typed request. An older slot
    // at these same N/B addresses is retained storage, not its identity. Keep
    // its M/R/mesh pins until Quiet, and give the new incarnation its own slot.
    // ResultOwner still uses the independently strong-retained R, which cannot
    // be address-reused while an old slot owns it.
    Slot* slot=Find(gFrame->navigation,gFrame->block,&binding);
    if(!slot) {
        std::unique_ptr<Slot>* empty{};for(auto& item:gState->slots)if(!item){empty=&item;break;}
        if(!empty){++gRejected;Reject(binding);return;}
        auto next=std::make_unique<Slot>();next->binding=binding;next->local.navigation=gFrame->navigation;next->local.block=gFrame->block;
        next->pin=std::make_shared<Pin>();next->pin->movement=std::make_unique<native_resource::StrongLease>(binding.command.actor.character+0x42A8,0x1F0);
        if(!*next->pin->movement||next->pin->movement->Object()+0xC4!=next->local.navigation||!Mesh(next->pin->mesh)||
            next->registry.BeginWorld(binding.command.scope.worldEpoch)!=route_identity::Result::Ok||!next->writer.Initialize(gBase,true,next->registry)) {
            ++gRejected;Reject(binding);return;
        }
        if(mesh_storage::BindOrdinary(next->registry,binding.command.scope.worldEpoch,next->pin->mesh,next->meshBinding)!=route_identity::Result::Ok) {
            ++gRejected;Reject(binding);return;
        }
        next->protection={next->pin,next->pin->mesh.mesh,0,&Guard,next.get()};
        slot=next.get();*empty=std::move(next);
    }
    if(slot->retiring||slot->retirementFault) {
        slot->retirementFault=true;Fail(*slot);return;
    }
    std::array<std::uintptr_t,2> pair{};
    if(!Read(reinterpret_cast<std::uintptr_t>(reference),pair)||!pair[0]||!pair[1]){Fail(*slot);return;}
    // The wrapper already allocated R. Retain it instead of submitting a job.
    auto pin=std::make_unique<native_resource::StrongLease>(reinterpret_cast<std::uintptr_t>(reference),0x40);
    if(!*pin){Fail(*slot);return;}
    if(slot->previousResult){Fail(*slot);return;}
    slot->previousResult=std::move(slot->result);
    slot->local.result=pair[0];slot->local.resultControl=pair[1];
    slot->result=std::move(pin);slot->skipped=true;
    slot->queryStart=gFrame->start;slot->startValid=gFrame->startValid;
    slot->startValid=Read(reinterpret_cast<std::uintptr_t>(parameters),slot->queryOptions)&&slot->startValid;
    slot->waitingSince=GetTickCount64();slot->installed=false;
    Failure reason{};
    Key key{};key.scope=binding.command.scope;key.owner=binding.command.owner;key.commandId=binding.command.command;
    if(slot->failed||slot->retiring||slot->retirementFault||route_runtime::CommandBlocked(key,reason)) {
        Fail(*slot,__LINE__,Result::UnsupportedDomain);return;
    }
    // Retain a fresh no-worker result until the newer Host graph arrives.
    // Populate validates its start and Rearm retains the old mesh lifetime.
}
bool Imported(Slot& slot,std::uintptr_t list) noexcept {
    std::uintptr_t r{};
    // Even a callback revoking permission must not turn an already imported
    // result into permission to perform the four local route transformations.
    // Existing native refs keep these nodes/mesh alive while this call unwinds.
    return slot.installing&&slot.writer.Published()&&
        Read(slot.local.navigation+0x5C,r)&&r==slot.local.result&&list==r+0x38;
}
template<std::uintptr_t Target> void __fastcall Postprocess(void* n,void*,void* list) {
    auto* slot=gAdmission;
    if(slot&&slot->local.navigation==reinterpret_cast<std::uintptr_t>(n)&&Imported(*slot,reinterpret_cast<std::uintptr_t>(list)))return;
    reinterpret_cast<void(__thiscall*)(void*,void*)>(gBase+Target)(n,list);
}
void __fastcall StopTrim(void* n,void*,void* b) {
    auto* slot=gAdmission;
    // The transmitted graph is captured AFTER Host stop-distance trimming.
    // The Client still keeps native distances/arrival, but must not trim twice.
    if(slot&&slot->local.navigation==reinterpret_cast<std::uintptr_t>(n)&&
        slot->local.block==reinterpret_cast<std::uintptr_t>(b)&&Imported(*slot,slot->local.result+0x38))return;
    reinterpret_cast<void(__thiscall*)(void*,void*)>(gBase+0x58620U)(n,b);
}
bool ImportedMarkerVector(std::uintptr_t vector) noexcept {
    auto* slot=gAdmission;
    if(!slot||!Imported(*slot,slot->local.result+0x38))return false;
    std::uintptr_t head{},at{};std::uint32_t count{};
    if(!Read(slot->local.result+0x38,head)||!Read(slot->local.result+0x3C,count)||count>kMaxNodes||!Read(head,at))return false;
    for(std::uint32_t i=0;i<count&&at!=head;++i) {
        std::uintptr_t node{},next{};if(!Read(at+8,node)||!Read(at,next))return false;
        if(node+0x34==vector)return true;at=next;
    }
    return false;
}
void __fastcall AttachMarker(void* vector,void*,void* ref) {
    if(ImportedMarkerVector(reinterpret_cast<std::uintptr_t>(vector)))return;
    reinterpret_cast<void(__thiscall*)(void*,void*)>(gBase+0x617D0U)(vector,ref);
}
}
// Called only around the normal 004659D0 boundary, not from an IPC callback.
bool BeginAdmission(std::uintptr_t n) noexcept {
    std::uintptr_t pending{};if(!Read(n+0x5C,pending)||!pending)return false;
    auto* slot=ResultOwner(n,pending);if(!slot)return false;
    if(slot->failed||slot->retiring||slot->retirementFault||!slot->writer.Published()||!Current(*slot))return false;
    if(!Guard(slot,slot->writer.Identity(),slot->local,{},GuardPhase::Read))return false;
    std::uintptr_t r{};if(!Read(n+0x5C,r)||r!=slot->local.result)return false;
    if(gAdmission){Fail(*slot);return false;}
    slot->installing=true;gAdmission=slot;return true;
}
bool Owns(std::uintptr_t n) noexcept {
    std::uintptr_t pending{};
    return gEngine&&gEngine()&&Read(n+0x5C,pending)&&pending&&ResultOwner(n,pending)!=nullptr;
}
void EndAdmission(std::uintptr_t n) noexcept {
    auto* slot=gAdmission;if(!slot||slot->local.navigation!=n)return;
    slot->installing=false;gAdmission=nullptr;
    std::uintptr_t pending{};std::uint32_t phase{};
    if(Current(*slot)&&Read(n+0x5C,pending)&&!pending&&Read(slot->local.block+0x40,phase)&&phase==3) {
        if(slot->replacementRequested){++gReplacementCommits;++gReplacements;gReplacementReason=0U;}
        slot->installed=true;slot->waitingSince=0;slot->replacementRequested=false;
        // Only an ACTUALLY installed current/newer native route hands motion
        // back. A received graph or a Submit receipt is insufficient.
        for(auto& entry:gState->hybrid)if(entry.used&&
            entry.binding.command.scope==slot->binding.command.scope&&entry.binding.command.owner==slot->binding.command.owner&&
            entry.binding.command.actor==slot->binding.command.actor&&
            HybridResumeCurrent(entry.binding.command.command,entry.binding.command.revision,
                slot->binding.command.command,slot->binding.command.revision))entry={};
        slot->planned.reset();slot->continuation=false;
        // The native installer no longer references the old R. Route/history
        // nodes retain their own Refs and the slot retains the backing mesh.
        slot->previousResult.reset();++gInstalled;
    }
    else if(!Current(*slot))Fail(*slot);
}
bool PendingEndpoint(std::uintptr_t n,std::uintptr_t b) noexcept {
    execution::NavigationBinding binding{};
    const auto ownership=execution_runtime::IdentifyNavigation(n,b,binding);
    return ownership!=execution::NavigationOwnership::Unowned&&binding.policy==movement::Navigation::HostRoute;
}
bool Install(std::uintptr_t base,HookManager& hooks,EngineThread engine,Qualify qualify,std::string& error) {
    if(gState||!base||!engine||!qualify||!hooks.ExactBuildValidated())return false;
    std::array<std::uint8_t,8> invalidate{};std::uint8_t returns{};
    if(!Read(base+0x4D200U,invalidate)||invalidate!=std::array<std::uint8_t,8>{0x83,0xEC,0x08,0x53,0x55,0x56,0x57,0x8B}||
        !Read(base+0x4D314U,returns)||returns!=0xC3U) {error="AISync route invalidation ABI mismatch";return false;}
    gBase=base;gEngine=engine;gQualify=qualify;gState=std::make_unique<State>().release();
    const auto before=hooks.InstalledCount();
    const auto call=[&](const char* name,std::uintptr_t site,std::uintptr_t target,void* detour) {
        return hooks.InstallCallRedirect({name,reinterpret_cast<void*>(base+site),reinterpret_cast<void*>(base+target),detour},error);
    };
    if(!call("AISync native route query ownership",0x659A1U,0x61280U,reinterpret_cast<void*>(&QueryFrame))||
        !call("AISync authoritative route result",0x607E3U,0x570D0U,reinterpret_cast<void*>(&Dispatch))||
        !call("AISync preserve Host route prefix",0x65AADU,0x58AC0U,reinterpret_cast<void*>(&Postprocess<0x58AC0U>))||
        !call("AISync preserve Host route start",0x65ABBU,0x58A30U,reinterpret_cast<void*>(&Postprocess<0x58A30U>))||
        !call("AISync preserve Host route turn",0x65AC9U,0x64710U,reinterpret_cast<void*>(&Postprocess<0x64710U>))||
        !call("AISync preserve Host corridor",0x65AD7U,0x64200U,reinterpret_cast<void*>(&Postprocess<0x64200U>))||
        !call("AISync preserve Host arrival boundary",0x65CCAU,0x58620U,reinterpret_cast<void*>(&StopTrim))||
        !call("AISync preserve Host entry marker",0x65B00U,0x617D0U,reinterpret_cast<void*>(&AttachMarker))||
        !call("AISync preserve Host exit marker",0x65C46U,0x617D0U,reinterpret_cast<void*>(&AttachMarker))) {
        std::string rollback;const auto undone=hooks.UninstallFrom(before,rollback);
        if(!rollback.empty())error+="; "+rollback;if(undone){delete gState;gState=nullptr;}return false;
    }
    return true;
}
bool Offer(const Graph& graph) noexcept {
    if(!gState||!gEngine||!gEngine()||!gQualify(graph.key.scope)||Validate(graph)!=Result::Ok||graph.failure!=Failure::None)return false;
    Cached* empty{};
    for(auto& entry:gState->cache) {
        if(!entry.graph||!execution_runtime::Matches(entry.graph->key.scope)){if(!empty)empty=&entry;continue;}
        if(!SameCommand(entry.graph->key,graph.key))continue;
        if(graph.key.parameterRevision<entry.graph->key.parameterRevision||
            (graph.key.parameterRevision==entry.graph->key.parameterRevision&&graph.key.routeRevision<entry.graph->key.routeRevision))return true;
        if(graph.key==entry.graph->key)return graph==*entry.graph;
        entry.graph=std::make_unique<Graph>(graph);return true;
    }
    if(!empty)for(auto& entry:gState->cache)if(entry.graph&&entry.graph->key.scope==graph.key.scope&&
        entry.graph->key.owner==graph.key.owner&&entry.graph->key.commandId<graph.key.commandId) {
        bool retained{};
        for(const auto& slot:gState->slots)if(slot&&slot->binding.command.scope==entry.graph->key.scope&&
            slot->binding.command.owner==entry.graph->key.owner&&slot->binding.command.command==entry.graph->key.commandId)retained=true;
        if(!retained){empty=&entry;break;}
    }
    if(!empty)return false;
    empty->graph=std::make_unique<Graph>(graph);return true;
}
void ConfirmActorDestroyed(const execution::ActorLifetime& actor) noexcept {
    if(!gState||!gEngine||!gEngine()||!actor.creationSerial)return;
    const execution::LocalActorBinding local{actor.character,actor.runtimeId,true,actor.creationSerial};
    for(auto& entry:gState->hybrid)if(entry.used&&entry.binding.command.actor==local)entry={};
    for(auto& slot:gState->slots)if(slot&&slot->binding.command.actor==local)slot->actorDestroyed=true;
}
bool DrainNative(std::uintptr_t n,bool route) noexcept {
    __try {
        if(route)reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x56BD0U)(reinterpret_cast<void*>(n));
        reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x341570U)(reinterpret_cast<void*>(n+0x10));
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Drain(Slot& slot,bool route=false) noexcept {
    if(slot.retiring||slot.retirementFault)return false;
    std::uintptr_t history{},sentinel{};
    if(!Read(slot.local.navigation+0x10,history)||!history||!Read(slot.local.navigation,sentinel)||!sentinel)return false;
    // Native Ref destructors may reenter. Close this slot before the ONE erase
    // attempt; a fault or changed lifetime remains pinned and is never retried.
    slot.retiring=true;
    const auto drained=DrainNative(slot.local.navigation,route);
    std::uintptr_t afterHistory{},afterRoute{};
    const bool valid=drained&&!slot.retirementFault&&
        Read(slot.local.navigation+0x10,afterHistory)&&afterHistory==history&&
        Read(slot.local.navigation,afterRoute)&&afterRoute==sentinel&&Quiet(slot);
    slot.retiring=false;
    if(!valid){slot.retirementFault=true;Fail(slot);}
    return valid;
}
bool RetireDestroyedRoute(Slot& slot) noexcept {
    if(!slot.actorDestroyed||!slot.skipped||slot.installing||slot.retiring||slot.retirementFault||!slot.pin||
        !slot.pin->movement||!*slot.pin->movement||slot.pin->movement->Object()+0xC4!=slot.local.navigation)return false;
    const auto n=slot.local.navigation;
    // Never reset a navigation object now shared by a different live owner.
    for(const auto& other:gState->slots)if(other&&other.get()!=&slot&&other->local.navigation==n&&!other->actorDestroyed)return false;
    if(!EmptyRing(n,0x28)||!EmptyRing(n,0x34)||!HistoryRing(n,0)||!HistoryRing(n))return false;
    for(const auto offset:{0x40U,0x44U,0x5CU,0x60U}) {
        std::uintptr_t value{};if(!Read(n+offset,value)||value)return false;
    }
    // No B, result or solver worker remains, and the Character destructor has
    // returned. M is still strongly pinned here. Clear leftover route/history
    // through retail list/Ref destructors; do not wait for a dead actor to tick.
    return Drain(slot,true);
}
void Pump() noexcept {
    if(!gState||!gEngine||!gEngine()||gFrame||gAdmission||gPumping)return;
    struct PumpGuard {PumpGuard(){gPumping=true;}~PumpGuard(){gPumping=false;}} pumping;
    for(auto& value:gState->slots)if(value) {
        auto& slot=*value;
        if(slot.actorDestroyed)static_cast<void>(RetireDestroyedRoute(slot));
        if(!slot.retirementFault&&!slot.retiring&&Quiet(slot,false)) {
            // Exact B removal, empty route, both raw-B lists and no-worker
            // provenance. Only history remains; native erase retains sentinel.
            if(!Quiet(slot))static_cast<void>(Drain(slot));
            if(!slot.retirementFault&&Quiet(slot)&&(!slot.writer.Entered()||slot.writer.Retire(slot.writer.Identity())==Result::Ok)) {
                value.reset();++gRetired;continue;
            }
        }
        if(slot.failed){Fail(slot);continue;}
        if(!Current(slot)||execution::MovementExecutionOwned(slot.binding.command.actor.character))continue;
        Key command{};command.scope=slot.binding.command.scope;command.owner=slot.binding.command.owner;command.commandId=slot.binding.command.command;
        Failure reason{};
        if(route_runtime::CommandBlocked(command,reason)){Fail(slot,__LINE__,Result::UnsupportedDomain);continue;}
        std::uintptr_t r{};std::uint32_t ready{};
        if(!Read(slot.local.navigation+0x5C,r)) {Fail(slot);continue;}
        if(r) {
            if(r!=slot.local.result) {
                if(!ResultOwner(slot.local.navigation,r))Fail(slot);
                continue;
            }
            if(!Read(r+4,ready)){Fail(slot);continue;}
            if(!ready&&slot.waitingSince&&GetTickCount64()-slot.waitingSince>=kRouteWaitMilliseconds) {
                Fail(slot,__LINE__,Result::Deferred);continue;
            }
            if(!ready) {
                static_cast<void>(Populate(slot));
            }
            continue;
        }
        execution::NavigationBinding current{};if(!Current(slot,&current))continue;
        const auto* latest=Latest(current);if(!latest)continue;
        if(slot.desired&&latest->key==slot.desired->key&&slot.installed)continue;
        if(ApplyEndpoint(slot,*latest))continue;
        // Native re-admission starts at the actual current area. Until that
        // area matches, retain the previous Host range and its progress.
        static_cast<void>(RequestReplacement(slot,*latest));
    }
}
bool Awaiting(std::uintptr_t n) noexcept {
    std::uintptr_t r{};std::uint32_t ready{};
    if(!Read(n+0x5C,r)||!r)return false;
    const auto* slot=ResultOwner(n,r);
    return slot&&!slot->failed&&Read(r+4,ready)&&!ready;
}
bool Retired(const transactions::Scope& scope) noexcept {
    if(gState)for(const auto& slot:gState->slots)if(slot&&slot->binding.command.scope==scope)return false;
    return true;
}
bool HybridBinding(std::uintptr_t character,execution::NavigationBinding& output) noexcept {
    if(!gState||!gEngine||!gEngine())return false;
    for(auto& entry:gState->hybrid)if(entry.used) {
        // A temporarily unqualified native request is precisely the situation
        // for which recovery is needed. Do not destroy its copied identity
        // here; the real scope/actor lifetime boundaries revoke it.
        if(!Qualified(entry.binding))continue;
        if(entry.binding.command.actor.character==character){output=entry.binding;return true;}
    }
    return false;
}
void BeginScope(const transactions::Scope& scope) noexcept {
    if(!gState||!gEngine||!gEngine())return;
    for(auto& entry:gState->hybrid)if(entry.used&&entry.binding.command.scope!=scope)entry={};
}
bool CommandRetired(const transactions::Scope& scope,const transactions::OwnerKey& owner,std::uint64_t command) noexcept {
    if(!gState||!gEngine||!gEngine())return false;
    for(const auto& slot:gState->slots)if(slot&&slot->binding.command.scope==scope&&
        slot->binding.command.owner==owner&&slot->binding.command.command==command)return false;
    return true;
}
Counters Diagnostics() noexcept{return {gSuppressed.load(),gPublished.load(),gInstalled.load(),gReplacements.load(),gRejected.load(),gRetired.load(),gFailureCommand.load(),gFailureSite.load(),gFailureResult.load(),gImportStage.load(),gContextCheck.load(),gAppendPublished.load(),gContinuationPublished.load(),gCompoundRequests.load(),gLastAdmissionMode.load(),gLastIncomingCount.load(),gLastCompoundParts.load()};}
ReplacementReport ReplacementDiagnostics() noexcept {
    return {gReplacementRequests.load(),gReplacementCommits.load(),gReplacementWaits.load(),gReplacementCommand.load(),
        gReplacementReason.load(),gReplacementArea.load(),gReplacementExpected.load(),gReplacementParameters.load(),gReplacementRevision.load()};
}
}
namespace aimp::ai_sync::route_execution {
void SetHostPoseQuery(HostPose) noexcept {}
SlotReport LiveSlots() noexcept {
    SlotReport report{};
    if(!gState)return report;
    for(const auto& slot:gState->slots)if(slot) {
        if(!report.total) {
            const auto n=slot->local.navigation;std::uintptr_t head{},cursor{};
            if(EmptyRing(n,0))report.quietMask|=1U;
            if(EmptyRing(n,0x28))report.quietMask|=2U;
            if(EmptyRing(n,0x34))report.quietMask|=4U;
            if(Read(n,head)&&Read(n+0xC,cursor)&&head==cursor)report.quietMask|=8U;
            if(EmptyRing(n,0x10))report.quietMask|=16U;
            bool clear=true;for(const auto offset:{0x24U,0x40U,0x44U,0x5CU,0x60U}){std::uintptr_t v{};if(!Read(n+offset,v)||v)clear=false;}
            if(clear)report.quietMask|=32U;
        }
        ++report.total;
    }
    return report;
}
}
namespace aimp::ai_sync::route_runtime {
std::uint64_t LocalPlanningFallbacks() noexcept { return 0; }
std::uint64_t LocalStartPlans() noexcept { return 0; }
std::uint64_t LocalReplans() noexcept { return 0; }
}
