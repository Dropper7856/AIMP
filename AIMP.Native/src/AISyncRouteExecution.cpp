#include "aimp/AISyncRouteExecution.h"
#include "aimp/AISyncRouteRuntime.h"
#include "aimp/AISyncExecutionRuntime.h"
#include "aimp/AISyncMeshStorage.h"
#include "aimp/HookManager.h"
#include "aimp/NativeResourceLease.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <memory>

namespace aimp::ai_sync::route_execution {
namespace {
using namespace route;
constexpr std::size_t kCapacity=execution::NativeRequestAdapter::kCapacity;
std::uintptr_t gBase{};
EngineThread gEngine{};
Qualify gQualify{};
std::atomic<std::uint64_t> gSuppressed{},gPublished{},gInstalled{},gReplacements{},gRejected{},gRetired{};
std::atomic<std::uint64_t> gFailureCommand{},gLocalPlanning{},gLocalStartPlans{},gLocalReplans{};
HostPose gHostPose{};
// A Host route is a path FROM the Host character's position. It is only that
// command's path for a Client character standing at the same start.
constexpr float kRouteStartTolerance=0.75F;
std::atomic<std::uint32_t> gFailureSite{},gFailureResult{},gImportStage{},gContextCheck{};
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
    route_identity::Registry registry;
    NativeRouteResult writer;
    std::unique_ptr<Graph> desired;
    Protection protection{};
    bool skipped{},failed{},installing{},invalidating{},actorDestroyed{};
    bool localPlanned{}; // this request was planned natively from the Client's own position
};
struct Cached {std::unique_ptr<Graph> graph;};
struct State {
    std::array<std::unique_ptr<Slot>,kCapacity> slots;
    std::array<Cached,kCapacity> cache;
};
State* gState{}; // Unresolved native pins are never destroyed by DLL static teardown.
struct Frame {std::uintptr_t navigation{},block{};Frame* previous{};};
thread_local Frame* gFrame{};
thread_local Slot* gAdmission{};
struct FrameScope {
    Frame value;
    FrameScope(std::uintptr_t n,std::uintptr_t b):value{n,b,gFrame}{gFrame=&value;}
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
    if(!slot.skipped||slot.failed||!Current(slot))return false;
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
    return local==slot.local;
}
void Fail(Slot& slot,std::uint32_t site=__builtin_LINE(),Result result=Result::InvalidNativeState) noexcept {
    if(!slot.failed) {
        slot.failed=true;++gRejected;gFailureCommand=slot.binding.command.command;
        gFailureSite=site;gFailureResult=static_cast<std::uint32_t>(result);
        gImportStage=slot.writer.ImportStage();gContextCheck=slot.writer.ContextCheck();
        execution_runtime::Fail(slot.binding.command.scope,execution::Recovery::NativeUnavailable);
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
            key.part==binding.part&&key.parameterRevision<=binding.command.revision)return cached.graph.get();
    }
    return nullptr;
}
// True when the Client character stands where the Host route for this
// request starts (its first node, or the Host's live pose while that route
// is still in flight). Unknown references keep the Host route.
bool StartsHere(const execution::NavigationBinding& binding,const Graph* latest) noexcept {
    std::array<float,3> here{},start{};
    const auto c=binding.command.actor.character;
    if(!c||!Read(c+0x70U,here))return true;
    if(latest&&latest->key.parameterRevision==binding.command.revision&&latest->nodeCount)
        start={latest->nodes[0].vectors[0],latest->nodes[0].vectors[1],latest->nodes[0].vectors[2]};
    else if(!gHostPose||!gHostPose(binding.command.owner,start))return true;
    const float dx=here[0]-start[0],dy=here[1]-start[1],dz=here[2]-start[2];
    if(!(dx==dx)||!(dz==dz))return true;
    return dx*dx+dz*dz<=kRouteStartTolerance*kRouteStartTolerance&&dy*dy<4.0F;
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
bool Invalidate(Slot& slot) noexcept {
    if(slot.invalidating||slot.installing||!Current(slot)||!slot.desired)return false;
    if(!Guard(&slot,slot.desired->key,slot.local,{},GuardPhase::Read))return false;
    std::uintptr_t pending{};std::uint32_t state{};
    if(!Read(slot.local.navigation+0x5C,pending)||pending||!Read(slot.local.block+0x40,state)||state!=3)return false;
    if(!Endpoint(slot.local.block,slot.desired->goal))return false;
    slot.invalidating=true;
    // Certified no-worker owner: drops no queued solver ref. Native invalidation
    // marks blocks for a new native query and retains the old route until the
    // normal installer's replacement prefix, rather than erasing live lists.
    reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x4D200U)(reinterpret_cast<void*>(slot.local.navigation));
    slot.invalidating=false;++gReplacements;
    return Current(slot);
}
bool Populate(Slot& slot) noexcept {
    execution::NavigationBinding current{};
    if(slot.failed||!slot.result||!Current(slot,&current)||execution::MovementExecutionOwned(slot.binding.command.actor.character))return false;
    const auto* graph=Latest(current);
    if(!graph)return false;
    // A previous B may remain in the native list until its queued removal.
    // Waiting here is safe: no R node has been allocated/published yet.
    if(!Guard(&slot,graph->key,slot.local,{},GuardPhase::Read))return false;
    if(!slot.desired||slot.desired->key!=graph->key)slot.desired=std::make_unique<Graph>(*graph);
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
    if(slot.writer.Entered()) {
        if(slot.writer.Identity()==slot.desired->key)return false; // Native replan awaits a new Host route.
        if(!slot.writer.Published()||slot.writer.Rearm(slot.writer.Identity(),*slot.desired,slot.local,slot.protection)!=Result::Ok){Fail(slot);return false;}
    }
    const auto result=slot.writer.Populate(*slot.desired,slot.local,slot.protection,ImportPoint::ReplacementNoWorker);
    if(result!=Result::Ok){Fail(slot,__LINE__,result);return false;}
    ++gPublished;return true;
}
using Query=void(__thiscall*)(void*,void*,void*,void*,void*);
void __fastcall QueryFrame(void* n,void*,void* start,void* goal,void* b,void* previous) {
    if(!gEngine||!gEngine())return reinterpret_cast<Query>(gBase+0x61280U)(n,start,goal,b,previous);
    FrameScope frame(reinterpret_cast<std::uintptr_t>(n),reinterpret_cast<std::uintptr_t>(b));
    reinterpret_cast<Query>(gBase+0x61280U)(n,start,goal,b,previous);
}
void __fastcall Dispatch(void* scheduler,void*,void* parameters,void* reference) {
    using Native=void(__thiscall*)(void*,void*,void*);
    if(!gEngine||!gEngine()||!gFrame)return reinterpret_cast<Native>(gBase+0x570D0U)(scheduler,parameters,reference);
    execution::NavigationBinding binding{};
    const auto ownership=execution_runtime::IdentifyNavigation(gFrame->navigation,gFrame->block,binding);
    if(ownership==execution::NavigationOwnership::Unowned||binding.policy!=movement::Navigation::HostRoute)
        return reinterpret_cast<Native>(gBase+0x570D0U)(scheduler,parameters,reference);
    // The Host published this command without a capturable path: execute the
    // Host's command with native planning instead of waiting forever.
    {
        Key key{};key.scope=binding.command.scope;key.owner=binding.command.owner;key.commandId=binding.command.command;
        Failure reason{};
        if(route_runtime::CommandBlocked(key,reason)) {
            ++gLocalPlanning;
            return reinterpret_cast<Native>(gBase+0x570D0U)(scheduler,parameters,reference);
        }
    }
    {
        // The Client character is not where the Host route starts (it lags
        // or drifted), or it replans a Host route it already installed (no
        // new Host route will come for that). Execute the Host's command -
        // same request, same Host goal - with native planning from here.
        const auto* latest=Latest(binding);
        auto* existing=Find(gFrame->navigation,gFrame->block,&binding);
        const bool replan=existing&&latest&&latest->key.parameterRevision==binding.command.revision&&
            existing->writer.Entered()&&existing->writer.Identity()==latest->key;
        if(replan||!StartsHere(binding,latest)) {
            if(existing)existing->localPlanned=true;
            ++gLocalPlanning;++(replan?gLocalReplans:gLocalStartPlans);
            return reinterpret_cast<Native>(gBase+0x570D0U)(scheduler,parameters,reference);
        }
        if(existing)existing->localPlanned=false;
    }
    ++gSuppressed;
    if((ownership!=execution::NavigationOwnership::Ready&&ownership!=execution::NavigationOwnership::Admitting)||!Qualified(binding)) {
        ++gRejected;execution_runtime::Fail(binding.command.scope,execution::Recovery::NativeUnavailable);return;
    }
    // IdentifyNavigation has proved the CURRENT typed request. An older slot
    // at these same N/B addresses is retained storage, not its identity. Keep
    // its M/R/mesh pins until Quiet, and give the new incarnation its own slot.
    // ResultOwner still uses the independently strong-retained R, which cannot
    // be address-reused while an old slot owns it.
    Slot* slot=Find(gFrame->navigation,gFrame->block,&binding);
    if(!slot) {
        std::unique_ptr<Slot>* empty{};for(auto& item:gState->slots)if(!item){empty=&item;break;}
        if(!empty){execution_runtime::Fail(binding.command.scope,execution::Recovery::Capacity);return;}
        auto next=std::make_unique<Slot>();next->binding=binding;next->local.navigation=gFrame->navigation;next->local.block=gFrame->block;
        next->pin=std::make_shared<Pin>();next->pin->movement=std::make_unique<native_resource::StrongLease>(binding.command.actor.character+0x42A8,0x1F0);
        if(!*next->pin->movement||next->pin->movement->Object()+0xC4!=next->local.navigation||!Mesh(next->pin->mesh)||
            next->registry.BeginWorld(binding.command.scope.worldEpoch)!=route_identity::Result::Ok||!next->writer.Initialize(gBase,true,next->registry)) {
            execution_runtime::Fail(binding.command.scope,execution::Recovery::NativeUnavailable);return;
        }
        route_identity::BindingIncarnation mapped{};
        if(mesh_storage::BindOrdinary(next->registry,binding.command.scope.worldEpoch,next->pin->mesh,mapped)!=route_identity::Result::Ok) {
            execution_runtime::Fail(binding.command.scope,execution::Recovery::NativeUnavailable);return;
        }
        next->protection={next->pin,next->pin->mesh.mesh,0,&Guard,next.get()};
        slot=next.get();*empty=std::move(next);
    }
    std::array<std::uintptr_t,2> pair{};
    if(!Read(reinterpret_cast<std::uintptr_t>(reference),pair)||!pair[0]||!pair[1]){Fail(*slot);return;}
    // The wrapper already allocated R. Retain it instead of submitting a job.
    auto pin=std::make_unique<native_resource::StrongLease>(reinterpret_cast<std::uintptr_t>(reference),0x40);
    if(!*pin){Fail(*slot);return;}
    slot->local.result=pair[0];slot->local.resultControl=pair[1];
    slot->result=std::move(pin);slot->skipped=true;
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
    if(slot->failed||!slot->writer.Published()||!Current(*slot))return false;
    if(!Guard(slot,slot->writer.Identity(),slot->local,{},GuardPhase::Read))return false;
    std::uintptr_t r{};if(!Read(n+0x5C,r)||r!=slot->local.result)return false;
    if(gAdmission){Fail(*slot);return false;}
    slot->installing=true;gAdmission=slot;return true;
}
bool Owns(std::uintptr_t n) noexcept {return gEngine&&gEngine()&&Find(n)!=nullptr;}
void EndAdmission(std::uintptr_t n) noexcept {
    auto* slot=gAdmission;if(!slot||slot->local.navigation!=n)return;
    slot->installing=false;gAdmission=nullptr;
    std::uintptr_t pending{};std::uint32_t phase{};
    if(Read(n+0x5C,pending)&&!pending&&Read(slot->local.block+0x40,phase)&&phase==3)++gInstalled;
    else if(!Current(*slot))Fail(*slot);
}
bool PendingEndpoint(std::uintptr_t n,std::uintptr_t b) noexcept {
    execution::NavigationBinding binding{};
    const auto ownership=execution_runtime::IdentifyNavigation(n,b,binding);
    return ownership!=execution::NavigationOwnership::Unowned&&binding.policy==movement::Navigation::HostRoute;
}
bool Install(std::uintptr_t base,HookManager& hooks,EngineThread engine,Qualify qualify,std::string& error) {
    if(gState||!base||!engine||!qualify||!hooks.ExactBuildValidated())return false;
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
    for(auto& slot:gState->slots)if(slot&&slot->binding.command.actor==local)slot->actorDestroyed=true;
}
bool RetireDestroyedRoute(Slot& slot) noexcept {
    if(!slot.actorDestroyed||!slot.skipped||slot.installing||slot.invalidating||!slot.pin||
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
    reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x56BD0U)(reinterpret_cast<void*>(n));
    reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x341570U)(reinterpret_cast<void*>(n+0x10));
    return Quiet(slot);
}
void Pump() noexcept {
    if(!gState||!gEngine||!gEngine()||gFrame||gAdmission||gPumping)return;
    struct PumpGuard {PumpGuard(){gPumping=true;}~PumpGuard(){gPumping=false;}} pumping;
    for(auto& value:gState->slots)if(value) {
        auto& slot=*value;
        if(slot.actorDestroyed)static_cast<void>(RetireDestroyedRoute(slot));
        if(Quiet(slot,false)) {
            // Exact B removal, empty route, both raw-B lists and no-worker
            // provenance. Only history remains; native erase retains sentinel.
            if(!Quiet(slot))reinterpret_cast<void(__thiscall*)(void*)>(gBase+0x341570U)(reinterpret_cast<void*>(slot.local.navigation+0x10));
            if(Quiet(slot)&&(!slot.writer.Entered()||slot.writer.Retire(slot.writer.Identity())==Result::Ok)) {
                value.reset();++gRetired;continue;
            }
        }
        if(slot.failed||!Current(slot)||execution::MovementExecutionOwned(slot.binding.command.actor.character))continue;
        // Natively planned from the Client's position: its native result and
        // route belong to CATHODE. The next Host Update re-decides at Dispatch.
        if(slot.localPlanned)continue;
        std::uintptr_t r{};std::uint32_t ready{};
        if(!Read(slot.local.navigation+0x5C,r)) {Fail(slot);continue;}
        if(r) {
            if(r!=slot.local.result) {
                if(!ResultOwner(slot.local.navigation,r))Fail(slot);
                continue;
            }
            if(!Read(r+4,ready)){Fail(slot);continue;}
            if(!ready)static_cast<void>(Populate(slot));
            continue;
        }
        execution::NavigationBinding current{};if(!Current(slot,&current))continue;
        const auto* latest=Latest(current);if(!latest)continue;
        if(slot.desired&&latest->key==slot.desired->key&&slot.writer.Entered()&&slot.writer.Identity()==latest->key)continue;
        const auto preflight=slot.writer.Preflight(*latest,slot.local,slot.protection);
        if(preflight!=Result::Ok){Fail(slot,__LINE__,preflight);continue;}
        if(!slot.desired||slot.desired->key!=latest->key)slot.desired=std::make_unique<Graph>(*latest);
        // Invalidation may wait for a retiring compound B. Keep retrying the
        // desired graph until it was actually published, not merely selected.
        static_cast<void>(Invalidate(slot));
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
Counters Diagnostics() noexcept{return {gSuppressed.load(),gPublished.load(),gInstalled.load(),gReplacements.load(),gRejected.load(),gRetired.load(),gFailureCommand.load(),gFailureSite.load(),gFailureResult.load(),gImportStage.load(),gContextCheck.load()};}
}
namespace aimp::ai_sync::route_execution {
void SetHostPoseQuery(HostPose query) noexcept { gHostPose = query; }
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
        ++report.total;if(slot->localPlanned)++report.local;
    }
    return report;
}
}
namespace aimp::ai_sync::route_runtime {
std::uint64_t LocalPlanningFallbacks() noexcept { return route_execution::gLocalPlanning.load(std::memory_order_relaxed); }
std::uint64_t LocalStartPlans() noexcept { return route_execution::gLocalStartPlans.load(std::memory_order_relaxed); }
std::uint64_t LocalReplans() noexcept { return route_execution::gLocalReplans.load(std::memory_order_relaxed); }
}
