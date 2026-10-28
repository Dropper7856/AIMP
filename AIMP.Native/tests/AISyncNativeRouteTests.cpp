#include "aimp/AISyncNativeRoute.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

namespace aimp::ai_sync::route {
struct NativeRouteResultTestAccess {
    static void Prepare(NativeRouteResult& result, route_identity::Registry& registry, void* context,
        bool(*create)(void*,std::uintptr_t&,std::uintptr_t&) noexcept,
        bool(*append)(void*,std::uintptr_t,std::uintptr_t,std::uintptr_t) noexcept,
        bool(*release)(void*,std::uintptr_t&,std::uintptr_t&) noexcept,
        bool(*resolve)(void*,std::uintptr_t,std::uint32_t) noexcept,
        bool(*adjacent)(void*,std::uint32_t,std::uint32_t) noexcept) {
        result.base_ = 0x400000; result.registry_ = &registry; result.callsContext_ = context;
        result.create_ = create; result.append_ = append; result.release_ = release;
        result.resolve_ = resolve; result.adjacent_ = adjacent;
    }
    static void Special(NativeRouteResult& result, std::uintptr_t(*call)(void*,std::uintptr_t,std::uint32_t) noexcept) {result.special_=call;}
    static void Markers(NativeRouteResult& result,bool(*create)(void*,std::uint32_t,std::uintptr_t&,std::uintptr_t&) noexcept,
        bool(*attach)(void*,std::uintptr_t,std::uintptr_t,std::uintptr_t) noexcept) {result.marker_=create;result.attachMarker_=attach;}
};
}
using namespace aimp::ai_sync;
using namespace route;
namespace {
void Require(bool ok, int line) { if (!ok) { std::cerr << "native route invariant " << line << '\n'; std::abort(); } }
#define CHECK(value) Require((value),__LINE__)
// Native B includes flags at +90/+91; keep those bytes inside the fixture.
using Bytes = std::array<std::uint8_t,0xA0>;
std::uintptr_t Address(Bytes& value) { return reinterpret_cast<std::uintptr_t>(value.data()); }
template<class T> void Put(std::uintptr_t address, T value) { std::memcpy(reinterpret_cast<void*>(address),&value,sizeof(value)); }
template<class T> T Get(std::uintptr_t address) { T value{}; std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value)); return value; }
struct Harness {
    NativeRouteResult native;
    std::unique_ptr<route_identity::Registry> registry{std::make_unique<route_identity::Registry>()};
    route_identity::BindingIncarnation binding{};
    Graph graph{}; LocalContext local{}; Protection protection{};
    Bytes navigation{}, block{}, result{}, navSentinel{}, blockSentinel{}, blockLink{}, sentinel{}, mesh{};
    std::array<std::uint8_t,0x100> slots{};
    std::array<std::uint8_t,4096> blob{};
    std::uintptr_t specialLink{};bool wrongSpecial{}, invalidateSpecialOnAppend{};
    std::array<Bytes,12> nodes{}, links{};
    std::array<Bytes,48> markerObjects{};
    std::array<std::array<std::uintptr_t,8>,12> markerPairs{};
    unsigned markersCreated{};bool failMarkerCreate{},failMarkerAttach{},revokeMarkerAttach{};
    unsigned creates{}, appends{}, releases{};
    bool allowed{true}, quiet{}, adjacent{true}, resolve{true}, failCreate{}, failAppend{}, failRelease{}, reenter{};
    bool revokeAppend{}, guardAfterAppend{true};
    bool detached{true};
    Harness() {
        CHECK(registry->BeginWorld(3) == route_identity::Result::Ok);
        graph.key.scope.session[0] = 7; graph.key.scope.authorityEpoch = 2;
        graph.key.scope.worldEpoch = 3; graph.key.scope.consumerEpoch = 4;
        graph.key.owner = {1,2,3,4,transactions::Capability::Movement,5};
        graph.key.commandId = graph.key.parameterRevision = graph.key.routeRevision = 1;
        graph.resourceCount = 1; graph.nodeCount = 2;
        std::copy_n("TECH_COMMS",10,graph.resources[0].level.begin());
        graph.resources[0].immutableDigest.fill(5); graph.resources[0].headerTuple = {10,20,30};
        graph.statuses.fill(0x40000000); graph.auxiliary = {1,2,3,4,5,6};
        graph.nodes[0].ordinal = 7; graph.nodes[1].ordinal = 9;
        for (std::size_t i = 0; i < 18; ++i) graph.nodes[i/9].vectors[i%9] = static_cast<float>(i);
        route_identity::CallerVerifiedBinding b{}; b.resource = graph.resources[0];
        b.owner = {3,1,Address(mesh),1}; b.layout = {8,2,22,4,256}; b.localSlot = 1; b.localSalt = 2;
        b.elementCount = 100; b.resourceAndLocalMeshVerified = b.storageLifetimeVerified = true;
        CHECK(registry->BindCallerVerified(b,{},binding) == route_identity::Result::Ok);
        local = {Address(navigation),Address(block),Address(result)+0xC,Address(result)};
        Put(local.resultControl+4,1U); Put(local.result,0x400000U+0xDC7288U);
        Put(local.navigation,Address(navSentinel)); Put(Address(navSentinel),Address(navSentinel)); Put(Address(navSentinel)+4,Address(navSentinel));
        Put(local.navigation+0x28,Address(blockSentinel)); Put(local.navigation+0x2C,1U);
        Put(Address(blockSentinel),Address(blockLink)); Put(Address(blockSentinel)+4,Address(blockLink));
        Put(Address(blockLink),Address(blockSentinel)); Put(Address(blockLink)+4,Address(blockSentinel)); Put(Address(blockLink)+8,local.block);
        Put(local.block+0x40,1U);
        Put(local.result+0x38,Address(sentinel)); Put(Address(sentinel),Address(sentinel)); Put(Address(sentinel)+4,Address(sentinel));
        protection = {std::make_shared<int>(1),Address(mesh),19,&Guard,this};
        NativeRouteResultTestAccess::Prepare(native,*registry,this,&Create,&Append,&Release,&Resolve,&Adjacent);
        NativeRouteResultTestAccess::Markers(native,&CreateMarker,&AttachMarker);
    }
    void EnableSpecial() {
        graph.nodes[1].type=1;
        const auto w=Address(mesh),table=reinterpret_cast<std::uintptr_t>(slots.data());
        const auto start=reinterpret_cast<std::uintptr_t>(blob.data()),slot=table+0x40;
        specialLink=start+0x64+100*0x24;
        Put(w+0x48,22U);Put(w+0x4C,2U);Put(w+0x50,8U);Put(w+0x30,4U);Put(w+0x18,256U);Put(w+0x34,1U);Put(w+0x44,table);
        Put(slot,2U);Put(slot+8,start);Put(slot+0xC,start+0x64);Put(slot+0x2C,specialLink);
        Put(slot+0x30,start);Put(slot+0x34,0x64U+100U*0x24U+0x3CU);Put(slot+0x38,1U);
        Put(start+0x18,100U);Put(start+0x34,1U);Put(start+0x38,9U);
        Put(start+0x64+9*0x24+0x20,1U);Put(specialLink+0x1C,static_cast<std::uint16_t>(9));
        route_identity::ElementMapping mapping{};
        CHECK(registry->MapElement({graph.resources[0],9},mapping)==route_identity::Result::Ok);
        Put(specialLink+0x20,mapping.localReference);
        Put(specialLink+0x28,0x1234U);Put(specialLink+0x2C,0x5678U);
        NativeRouteResultTestAccess::Special(native,&Special);
    }
    static std::uintptr_t Special(void* context,std::uintptr_t,std::uint32_t) noexcept {
        auto& h=*static_cast<Harness*>(context);return h.specialLink+(h.wrongSpecial?0x3C:0);
    }
    static bool Guard(void* context, const Key&, const LocalContext&, std::span<const route_identity::ElementMapping>, GuardPhase phase) noexcept {
        auto& h = *static_cast<Harness*>(context);
        if (h.reenter) CHECK(h.native.Populate(h.graph,h.local,h.protection) == Result::Reentrant);
        return h.allowed && (phase != GuardPhase::Retire || h.quiet) && (phase != GuardPhase::Rebind || h.detached) && (h.guardAfterAppend || h.appends == 0);
    }
    static bool Resolve(void* context, std::uintptr_t, std::uint32_t reference) noexcept { auto& h=*static_cast<Harness*>(context);return h.resolve && !(h.specialLink && (reference&255U)==9U); }
    static bool Adjacent(void* context, std::uint32_t, std::uint32_t) noexcept { return static_cast<Harness*>(context)->adjacent; }
    static bool Create(void* context, std::uintptr_t& object, std::uintptr_t& control) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.creates;
        if (h.failCreate || h.creates > h.nodes.size()) return false;
        control = Address(h.nodes[h.creates-1]); object = control+0xC;
        Put(control+4,1U); Put(object,0x400000U+0xDC730CU); Put(object+4,3U); return true;
    }
    static bool Append(void* context, std::uintptr_t resultAt, std::uintptr_t object, std::uintptr_t control) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.appends;
        if (h.failAppend) return false;
        const auto link = Address(h.links[h.appends-1]); const auto sentinelAt = Get<std::uintptr_t>(resultAt+0x38);
        const auto previous = Get<std::uintptr_t>(sentinelAt+4);
        Put(link,sentinelAt); Put(link+4,previous); Put(link+8,object); Put(link+0xC,control);
        Put(previous,link); Put(sentinelAt+4,link); Put(resultAt+0x3C,Get<std::uint32_t>(resultAt+0x3C)+1);
        Put(control+4,Get<std::uint32_t>(control+4)+1);
        if(h.invalidateSpecialOnAppend&&h.appends==2)Put(h.specialLink+0x20,0U);
        if (h.revokeAppend) CHECK(h.registry->BeginRetire(h.binding) == route_identity::Result::Ok);
        return true;
    }
    static bool Release(void* context, std::uintptr_t& object, std::uintptr_t& control) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.releases;
        if (h.failRelease) return false;
        Put(control+4,Get<std::uint32_t>(control+4)-1); object = control = 0; return true;
    }
    static bool CreateMarker(void* context,std::uint32_t kind,std::uintptr_t& object,std::uintptr_t& control) noexcept {
        auto& h=*static_cast<Harness*>(context);
        if(h.failMarkerCreate||h.markersCreated>=h.markerObjects.size())return false;
        control=Address(h.markerObjects[h.markersCreated++]);object=control+12;
        Put(control+4,1U);Put(object,0x400000U+0xDC7224U);Put(object+0xC,kind);return true;
    }
    static bool AttachMarker(void* context,std::uintptr_t node,std::uintptr_t object,std::uintptr_t control) noexcept {
        auto& h=*static_cast<Harness*>(context);if(h.failMarkerAttach)return false;
        for(std::size_t i=0;i<h.nodes.size();++i)if(Address(h.nodes[i])+12==node) {
            const auto begin=reinterpret_cast<std::uintptr_t>(h.markerPairs[i].data());
            const auto old=Get<std::uintptr_t>(node+0x38),end=old?old:begin;
            if(end-begin>=32)return false;
            Put(end,object);Put(end+4,control);Put(node+0x34,begin);Put(node+0x38,end+8);
            Put(control+4,Get<std::uint32_t>(control+4)+1);
            if(h.revokeMarkerAttach)static_cast<void>(h.registry->BeginRetire(h.binding));
            return true;
        }
        return false;
    }
    Result Populate() { return native.Populate(graph,local,protection); }
    void Install() {
        Put(local.block+0x40,2U); Put(local.navigation+0x40,local.block);
        Put(local.navigation+0x5C,local.result); Put(local.navigation+0x60,local.resultControl);
    }
};
void Roundtrip() {
    auto h = std::make_unique<Harness>(); h->reenter = true;
    CHECK(h->Populate() == Result::Ok && h->native.Entered() && h->native.Published());
    CHECK(h->creates == 2 && h->appends == 2 && h->releases == 2);
    CHECK(Get<std::uint32_t>(h->local.result+4) == 1 && Get<std::uint32_t>(h->local.result+8) == 19);
    for (const auto& node : h->nodes) if (Get<std::uint32_t>(reinterpret_cast<std::uintptr_t>(node.data())+4) != 0)
        CHECK(Get<std::uint32_t>(reinterpret_cast<std::uintptr_t>(node.data())+4) == 1);
    CHECK(h->Populate() == Result::AlreadyEntered);
    h->Install(); Graph captured{}; NativeRouteResult reader;
    NativeRouteResultTestAccess::Prepare(reader,*h->registry,h.get(),&Harness::Create,&Harness::Append,&Harness::Release,&Harness::Resolve,&Harness::Adjacent);
    h->reenter = false;
    const std::array bindings{h->binding};
    CHECK(reader.Capture(h->graph.key,h->local,h->protection,bindings,captured) == Result::Ok && captured == h->graph);
    auto wrong = h->graph.key; ++wrong.routeRevision; CHECK(h->native.Retire(wrong) == Result::WrongKey);
    CHECK(h->native.Retire(h->graph.key) == Result::LifetimeLost);
    std::weak_ptr<const void> weak = h->protection.storage; h->protection.storage.reset(); CHECK(!weak.expired());
    h->quiet = true; CHECK(h->native.Retire(h->graph.key) == Result::Ok && weak.expired());
    CHECK(h->native.Retire(h->graph.key) == Result::Ok);
}
void RetailDefaultCapture() {
    // v263 Host sample: six solved nodes, B.state1 and stopDistance=-1.
    // The resource hash buckets and element capacity are tested separately by
    // the real loader adapter test; neither value is a native route decision.
    auto host=std::make_unique<Harness>();host->graph.nodeCount=6;
    for(std::size_t i=0;i<6;++i)host->graph.nodes[i].ordinal=static_cast<std::uint32_t>(i+1);
    Put(host->local.block+0x30,0xBF800000U);
    CHECK(host->Populate()==Result::Ok);host->Install();Put(host->local.block+0x40,1U);
    const std::array bindings{host->binding};Graph captured{};
    CHECK(host->native.Capture(host->graph.key,host->local,host->protection,bindings,captured)==Result::Ok&&captured==host->graph);
    auto client=std::make_unique<Harness>();client->graph=captured;client->Install();Put(client->local.block+0x30,0xBF800000U);
    CHECK(client->native.Populate(captured,client->local,client->protection,ImportPoint::DeferredNoWorker)==Result::Ok);
    CHECK(client->creates==6&&client->appends==6&&client->releases==6);
    for(const auto bad:{0xBF800001U,0x7FC00000U,0x7F800000U,0x3F800000U}) {
        auto invalid=std::make_unique<Harness>();Put(invalid->local.block+0x30,bad);
        CHECK(invalid->Populate()==Result::InvalidNativeState&&invalid->creates==0);
    }
}
void GuardsAndFaults() {
    {auto h=std::make_unique<Harness>();Graph refusal{};refusal.key=h->graph.key;refusal.failure=Failure::Unsupported;
     CHECK(h->native.Populate(refusal,h->local,h->protection)==Result::UnsupportedDomain&&!h->native.Entered()&&h->creates==0);
     CHECK(h->native.Preflight(refusal,h->local,h->protection)==Result::UnsupportedDomain);}
    { auto h = std::make_unique<Harness>(); h->Install();
      CHECK(h->Populate() == Result::InvalidNativeState && !h->native.Entered());
      CHECK(h->native.Populate(h->graph,h->local,h->protection,ImportPoint::DeferredNoWorker) == Result::Ok);
      CHECK(h->creates == 2 && Get<std::uint32_t>(h->local.result+4) == 1); }
    { auto h = std::make_unique<Harness>(); h->Install(); Put(h->local.navigation+0x60,0U);
      CHECK(h->native.Populate(h->graph,h->local,h->protection,ImportPoint::DeferredNoWorker) == Result::InvalidNativeState); }
    { auto h = std::make_unique<Harness>(); h->Install(); Put(h->local.result+4,1U);
      CHECK(h->native.Populate(h->graph,h->local,h->protection,ImportPoint::DeferredNoWorker) == Result::InvalidNativeState); }
    { auto h = std::make_unique<Harness>(); h->protection.storage.reset(); CHECK(h->Populate() == Result::LifetimeLost && h->creates == 0); }
    { auto h = std::make_unique<Harness>(); h->adjacent = false; CHECK(h->Populate() == Result::LocalInvalidity && !h->native.Entered()); }
    { auto h = std::make_unique<Harness>(); CHECK(h->registry->BeginRetire(h->binding) == route_identity::Result::Ok);
      CHECK(h->Populate() == Result::MissingResource && h->creates == 0); }
    { auto h = std::make_unique<Harness>(); Put(h->local.block+0x30,1U); CHECK(h->Populate() == Result::InvalidNativeState && h->creates == 0); }
    for (unsigned fault = 0; fault < 5; ++fault) {
        auto h = std::make_unique<Harness>(); h->failCreate = fault == 0; h->failAppend = fault == 1;
        h->failRelease = fault == 2; h->revokeAppend = fault == 3; h->guardAfterAppend = fault != 4;
        CHECK(h->Populate() != Result::Ok && h->native.Entered() && !h->native.Published());
        CHECK(Get<std::uint32_t>(h->local.result+4) == 0);
        const auto calls = h->creates+h->appends+h->releases;
        CHECK(h->Populate() == Result::AlreadyEntered && h->creates+h->appends+h->releases == calls);
        h->guardAfterAppend = true; h->quiet = true;
        CHECK(h->native.Retire(h->graph.key) == (fault == 0 ? Result::Ok : Result::NativeFault));
    }
}
void ReplacementContinuation() {
    for(unsigned fault=0;fault<2;++fault) {
        auto h=std::make_unique<Harness>();CHECK(h->Populate()==Result::Ok);h->Install();
        const auto oldKey=h->graph.key;auto graph=h->graph;++graph.key.routeRevision;
        graph.goal={10,20,30};graph.nodes[0].vectors[0]=42;
        h->adjacent=false;CHECK(h->native.Preflight(graph,h->local,h->protection)==Result::LocalInvalidity&&h->creates==2);
        h->adjacent=true;CHECK(h->native.Preflight(graph,h->local,h->protection)==Result::Ok&&h->creates==2);
        Bytes fresh{},head{};auto local=h->local;local.resultControl=Address(fresh);local.result=Address(fresh)+12;
        Put(local.resultControl+4,1U);Put(local.result,0x400000U+0xDC7288U);
        Put(local.result+0x38,Address(head));Put(Address(head),Address(head));Put(Address(head)+4,Address(head));
        Put(local.navigation+0x5C,local.result);Put(local.navigation+0x60,local.resultControl);
        auto wrong=graph;++wrong.key.commandId;
        CHECK(h->native.Rearm(oldKey,wrong,local,h->protection)==Result::WrongKey);
        wrong=graph;wrong.key.routeRevision=oldKey.routeRevision;CHECK(h->native.Rearm(oldKey,wrong,local,h->protection)==Result::WrongKey);
        h->detached=false;CHECK(h->native.Rearm(oldKey,graph,local,h->protection)==Result::LifetimeLost&&h->native.Published());h->detached=true;
        auto changed=h->protection;changed.storage=std::make_shared<int>(2);
        CHECK(h->native.Rearm(oldKey,graph,local,changed)==Result::LifetimeLost);
        changed=h->protection;++changed.mesh;
        CHECK(h->native.Rearm(oldKey,graph,local,changed)==Result::LifetimeLost);
        ++h->protection.localWorldRevision; // Same protected mesh, freshly validated local eligibility token.
        std::weak_ptr<const void> storage=h->protection.storage;
        CHECK(h->native.Rearm(oldKey,graph,local,h->protection)==Result::Ok&&!h->native.Entered()&&!storage.expired());
        h->failCreate=fault==1;
        const auto result=h->native.Populate(graph,local,h->protection,ImportPoint::DeferredNoWorker);
        CHECK(result==(fault?Result::NativeFault:Result::Ok)&&h->native.Entered());
        CHECK(h->native.Populate(graph,local,h->protection,ImportPoint::DeferredNoWorker)==Result::AlreadyEntered);
        CHECK(Get<std::uint32_t>(local.result+4)==(fault?0U:1U));
        if(!fault)CHECK(Get<std::uint32_t>(local.result+8)==h->protection.localWorldRevision);
        CHECK(Get<std::uint32_t>(h->local.result+4)==1U); // Old R is distinct and untouched.
        CHECK(!storage.expired());
        if(!fault) {
            h->local=local;
            for(std::size_t i=0;i<3;++i)Put(local.block+i*4,graph.goal[i]);
            NativeRouteResult reader;NativeRouteResultTestAccess::Prepare(reader,*h->registry,h.get(),&Harness::Create,&Harness::Append,&Harness::Release,&Harness::Resolve,&Harness::Adjacent);
            Graph captured{};const std::array bindings{h->binding};
            CHECK(reader.Capture(graph.key,local,h->protection,bindings,captured)==Result::Ok&&captured==graph);
            CHECK(h->creates==4&&h->appends==4&&h->releases==4);
        }
    }
}
void SpecialRoutes() {
    auto h=std::make_unique<Harness>();h->EnableSpecial();
    h->graph.nodeCount=3;h->graph.nodes[2]=h->graph.nodes[0];
    CHECK(h->native.Preflight(h->graph,h->local,h->protection)==Result::Ok&&h->creates==0);
    CHECK(h->Populate()==Result::Ok);
    CHECK(Get<std::uint32_t>(Address(h->nodes[1])+12+4)==1U);
    CHECK(Get<std::uintptr_t>(Address(h->nodes[1])+12+0x30)==h->specialLink);
    CHECK(Get<std::uintptr_t>(Address(h->nodes[0])+12+0x30)==0);
    h->Install();NativeRouteResult reader;
    NativeRouteResultTestAccess::Prepare(reader,*h->registry,h.get(),&Harness::Create,&Harness::Append,&Harness::Release,&Harness::Resolve,&Harness::Adjacent);
    NativeRouteResultTestAccess::Special(reader,&Harness::Special);
    const std::array bindings{h->binding};Graph captured{};
    CHECK(reader.Capture(h->graph.key,h->local,h->protection,bindings,captured)==Result::Ok&&captured==h->graph);
    // Same portable identity, different process-local link address.
    auto client=std::make_unique<Harness>();client->EnableSpecial();client->graph=h->graph;
    CHECK(client->specialLink!=h->specialLink&&client->Populate()==Result::Ok);
    CHECK(Get<std::uintptr_t>(Address(client->nodes[1])+12+0x30)==client->specialLink);
    for(unsigned fault=0;fault<12;++fault) {
        auto bad=std::make_unique<Harness>();bad->EnableSpecial();
        const auto w=Address(bad->mesh),start=reinterpret_cast<std::uintptr_t>(bad->blob.data());
        const auto slot=reinterpret_cast<std::uintptr_t>(bad->slots.data())+0x40;
        switch(fault) {
            case 0:bad->wrongSpecial=true;break;
            case 1:Put(bad->specialLink+0x1C,static_cast<std::uint16_t>(8));break;
            case 2:Put(bad->specialLink+0x20,0U);break;
            case 3:Put(start+0x34,UINT32_MAX);break;
            case 4:Put(start+0x38,10U);break;
            case 5:Put(slot+0x2C,start+4096-8);break;
            case 6:Put(slot+0x38,0U);break;
            case 7:Put(slot,3U);break;
            case 8:Put(start+0x64+9*0x24+0x20,0U);break;
            case 9:Put(w+0x50,32U);break;
            case 10:Put(slot+0x2C,bad->specialLink-4);break;
            case 11:Put(start+0x1C,1U);break;
        }
        CHECK(bad->Populate()==Result::LocalInvalidity&&!bad->native.Entered()&&bad->creates==0);
    }
    auto wrongKind=std::make_unique<Harness>();wrongKind->EnableSpecial();wrongKind->graph.nodes[1].type=2;
    CHECK(wrongKind->Populate()==Result::LocalInvalidity&&wrongKind->creates==0);
    auto ordinaryAsSpecial=std::make_unique<Harness>();ordinaryAsSpecial->EnableSpecial();ordinaryAsSpecial->graph.nodes[0].type=1;
    CHECK(ordinaryAsSpecial->Populate()==Result::LocalInvalidity&&ordinaryAsSpecial->creates==0);
    auto drift=std::make_unique<Harness>();drift->EnableSpecial();drift->invalidateSpecialOnAppend=true;
    CHECK(drift->Populate()==Result::LocalInvalidity&&drift->native.Entered()&&!drift->native.Published());
    CHECK(Get<std::uint32_t>(drift->local.result+4)==0&&drift->Populate()==Result::AlreadyEntered);
    auto single=std::make_unique<Harness>();single->EnableSpecial();
    const auto singleTable=reinterpret_cast<std::uintptr_t>(single->slots.data());
    std::memmove(single->slots.data(),single->slots.data()+0x40,0x40);
    Put(Address(single->mesh)+0x44,singleTable);Put(Address(single->mesh)+0x30,1U);
    Put(Address(single->mesh)+0x4C,0U);Put(Address(single->mesh)+0x48,24U);
    // Same portable key is rebound to a different local salt/slot layout.
    auto one=std::make_unique<route_identity::Registry>();CHECK(one->BeginWorld(3)==route_identity::Result::Ok);
    route_identity::CallerVerifiedBinding b{};b.resource=single->graph.resources[0];b.owner={3,1,Address(single->mesh),1};
    b.layout={8,0,24,1,256};b.elementCount=100;b.localSalt=2;b.resourceAndLocalMeshVerified=b.storageLifetimeVerified=true;
    CHECK(one->BindCallerVerified(b,{},single->binding)==route_identity::Result::Ok);single->registry=std::move(one);
    NativeRouteResultTestAccess::Prepare(single->native,*single->registry,single.get(),&Harness::Create,&Harness::Append,&Harness::Release,&Harness::Resolve,&Harness::Adjacent);
    Put(single->specialLink+0x20,(2U<<8)|9U);
    CHECK(single->Populate()==Result::Ok);
    // Captured link mismatch leaves output unchanged and never guesses another.
    Put(Address(h->nodes[1])+12+0x30,client->specialLink);
    const auto retained=captured;
    CHECK(reader.Capture(h->graph.key,h->local,h->protection,bindings,captured)==Result::LocalInvalidity&&captured==retained);
}
void InstalledHostCapture() {
    auto h=std::make_unique<Harness>();CHECK(h->Populate()==Result::Ok);
    const auto n=h->local.navigation,b=h->local.block,head=Address(h->navSentinel);
    Put(n+4,2U);Put(head,Address(h->links[0]));Put(head+4,Address(h->links[1]));
    Put(Address(h->links[0])+4,head);Put(Address(h->links[1]),head);
    Put(n+0xC,Address(h->links[0]));Put(b+0x38,Address(h->links[0]));Put(b+0x3C,Address(h->links[1]));Put(b+0x40,3U);
    for(std::size_t i=0;i<3;++i)Put(n+0x50+i*4,h->graph.auxiliary[i+3]);
    std::array<Bytes,2> tags{},pairs{};
    for(std::size_t i=0;i<2;++i) {
        const auto control=Address(tags[i]),object=control+12,at=Address(pairs[i]);
        Put(control+4,1U);Put(object,0x400000U+0xDC7224U);Put(object+0xC,i==0?9U:11U);
        Put(at,object);Put(at+4,control);
        Put(Address(h->nodes[i])+12+0x34,at);Put(Address(h->nodes[i])+12+0x38,at+8);
    }
    auto key=h->graph.key;++key.routeRevision;
    LocalContext local{n,b,0,0};Graph captured{};
    Put(b+0x30,0xBF800000U); // Native default must also survive Host goal edits.
    CHECK(h->native.CaptureInstalled(key,local,h->protection,h->binding,h->graph.auxiliary,captured)==Result::Ok);
    auto expected=h->graph;expected.key=key;expected.nodes[0].markers=9;expected.nodes[1].markers=11;CHECK(captured==expected);
    // Native progress is not a solver lease. Export preserves the route and
    // does not clear that progress word or touch the Host's navigation.
    Put(n+0x24,0x12345678U);
    CHECK(h->native.CaptureInstalled(key,local,h->protection,h->binding,h->graph.auxiliary,captured)==Result::Ok&&captured==expected);
    CHECK(Get<std::uint32_t>(n+0x24)==0x12345678U);
    const auto beforeDeferred=captured;
    Put(n+0x48,std::uint8_t{1});
    CHECK(h->native.CaptureInstalled(key,local,h->protection,h->binding,h->graph.auxiliary,captured)==Result::Deferred&&captured==beforeDeferred);
    Put(n+0x48,std::uint8_t{});Put(b+0x90,std::uint8_t{1});
    CHECK(h->native.CaptureInstalled(key,local,h->protection,h->binding,h->graph.auxiliary,captured)==Result::Deferred&&captured==beforeDeferred);
    Put(b+0x90,std::uint8_t{});
    Put(n+0x40,b);
    CHECK(h->native.CaptureInstalled(key,local,h->protection,h->binding,h->graph.auxiliary,captured)==Result::Deferred&&captured==beforeDeferred);
    Put(n+0x40,std::uintptr_t{});
    Put(b+0x3C,Address(h->navSentinel));
    CHECK(h->native.CaptureInstalled(key,local,h->protection,h->binding,h->graph.auxiliary,captured)==Result::Deferred&&captured==beforeDeferred);
    Put(b+0x3C,Address(h->links[1]));
    Put(n+0xC,Address(h->links[1]));
    CHECK(h->native.CaptureInstalled(key,local,h->protection,h->binding,h->graph.auxiliary,captured)==Result::Ok&&
        captured.nodeCount==1&&captured.nodes[0]==expected.nodes[1]);
    const auto retained=captured;
    Put(Address(tags[1])+12+0xC,12U);
    CHECK(h->native.CaptureInstalled(key,local,h->protection,h->binding,h->graph.auxiliary,captured)==Result::UnsupportedDomain&&captured==retained);
    CHECK(h->creates==2&&h->appends==2&&h->releases==2);
}
void OrderedNativeMarkers() {
    for(const auto kind:{2U,3U,4U}) {
        auto boundary=std::make_unique<Harness>();boundary->graph.nodes[0].type=0;boundary->graph.nodes[0].markers=kind;
        CHECK(boundary->Populate()==Result::Ok&&boundary->markersCreated==1);
        std::uint32_t marker{};CHECK(boundary->native.ReadMarkers(Address(boundary->nodes[0])+12,marker)&&marker==kind);
    }
    auto h=std::make_unique<Harness>();h->graph.nodes[0].markers=0x090807U;h->graph.nodes[1].markers=11;
    CHECK(h->Populate()==Result::Ok&&h->markersCreated==4&&h->releases==6);
    for(std::size_t i=0;i<2;++i) {
        std::uint32_t markers{};CHECK(h->native.ReadMarkers(Address(h->nodes[i])+12,markers)&&markers==h->graph.nodes[i].markers);
    }
    for(std::size_t i=0;i<4;++i)CHECK(Get<std::uint32_t>(Address(h->markerObjects[i])+4)==1); // vector owns the remaining ref
    for(unsigned fault=0;fault<3;++fault) {
        auto bad=std::make_unique<Harness>();bad->graph.nodes[0].markers=7;
        bad->failMarkerCreate=fault==0;bad->failMarkerAttach=fault==1;bad->revokeMarkerAttach=fault==2;
        CHECK(bad->Populate()!=Result::Ok&&bad->native.Entered()&&!bad->native.Published());
        CHECK(Get<std::uint32_t>(bad->local.result+4)==0&&bad->Populate()==Result::AlreadyEntered);
    }
}
void CompleteInstalledRoute() {
    auto host=std::make_unique<Harness>();host->graph.nodeCount=11;
    for(std::size_t i=0;i<11;++i) {
        host->graph.nodes[i].ordinal=static_cast<std::uint32_t>(i+1);
        for(std::size_t j=0;j<9;++j)host->graph.nodes[i].vectors[j]=static_cast<float>(i*10+j);
    }
    host->graph.nodes[0].type=0;host->graph.nodes[0].markers=0x0703U;host->graph.nodes[1].markers=8;
    host->graph.nodes[2].markers=9;host->graph.nodes[10].markers=11;
    host->graph.nodes[3].markers=0x0402U;
    Put(host->local.block+0x30,0xBF800000U);CHECK(host->Populate()==Result::Ok);
    const auto n=host->local.navigation,b=host->local.block,head=Address(host->navSentinel);
    Put(n+4,11U);Put(head,Address(host->links[0]));Put(head+4,Address(host->links[10]));
    Put(Address(host->links[0])+4,head);Put(Address(host->links[10]),head);Put(n+0xC,Address(host->links[0]));Put(b+0x38,Address(host->links[0]));Put(b+0x3C,Address(host->links[10]));Put(b+0x40,3U);
    for(std::size_t i=0;i<3;++i)Put(n+0x50+i*4,host->graph.auxiliary[i+3]);
    Graph captured{};LocalContext installed{n,b,0,0};
    CHECK(host->native.CaptureInstalled(host->graph.key,installed,host->protection,host->binding,host->graph.auxiliary,captured)==Result::Ok);
    CHECK(captured==host->graph&&captured.nodeCount==11); // Both prefix nodes, not just the nine query nodes.
    auto client=std::make_unique<Harness>();client->graph=captured;CHECK(client->Populate()==Result::Ok&&client->creates==11&&client->markersCreated==7);
    for(std::size_t i=0;i<11;++i) {
        std::uint32_t tags{};CHECK(client->native.ReadMarkers(Address(client->nodes[i])+12,tags)&&tags==captured.nodes[i].markers);
        CHECK(Get<std::uint32_t>(Address(client->nodes[i])+12+8)==Get<std::uint32_t>(Address(host->nodes[i])+12+8));
        for(std::size_t j=0;j<9;++j)CHECK(Get<float>(Address(client->nodes[i])+12+0xC+j*4)==captured.nodes[i].vectors[j]);
    }
    // A compound Move shares N, but each B owns a specific iterator range.
    // Do not send the next part's suffix as this part's route, or replay the
    // already consumed prefix. All nine portal/position scalars remain exact.
    Bytes secondBlock{},secondLink{};
    const auto blockHead=Address(host->blockSentinel),firstBlock=Address(host->blockLink),other=Address(secondLink);
    Put(firstBlock,other);Put(other+4,firstBlock);Put(other,blockHead);Put(other+8,Address(secondBlock));Put(blockHead+4,other);Put(n+0x2C,2U);
    Put(b+0x38,Address(host->links[2]));Put(b+0x3C,Address(host->links[8]));Put(n+0xC,Address(host->links[4]));
    Put(b+0x30,2.0F); // Host has already performed the positive stop trim.
    Graph portion{};
    CHECK(host->native.CaptureInstalled(host->graph.key,installed,host->protection,host->binding,host->graph.auxiliary,portion)==Result::Ok);
    CHECK(portion.nodeCount==5);
    for(std::size_t i=0;i<5;++i)CHECK(portion.nodes[i]==host->graph.nodes[i+4]);
}
void PendingReplacementPreservesLiveRoute() {
    for(unsigned variant=0;variant<6;++variant) {
        const bool replacing=variant>=1&&variant<=3;
        auto h=std::make_unique<Harness>();Bytes first{},last{},previousBlock{},previousLink{};
        const auto n=h->local.navigation,head=Address(h->navSentinel),a=Address(first),b=Address(last);
        Put(n+4,2U);Put(n+0xC,a);Put(head,a);Put(head+4,b);
        Put(a+4,head);Put(a,b);Put(b+4,a);Put(b,head);
        Put(n+0x48,variant==1?std::uint8_t{1}:std::uint8_t{});
        Put(h->local.block+0x90,variant==2?std::uint8_t{1}:std::uint8_t{});
        if(variant>=3) {
            const auto blockHead=Address(h->blockSentinel),link=Address(h->blockLink),other=Address(previousLink);
            Put(blockHead,other);Put(other+4,blockHead);Put(other,link);Put(other+8,Address(previousBlock));Put(link+4,other);
            Put(n+0x2C,2U);Put(n+0x44,Address(previousBlock));
            Put(Address(previousBlock)+0x90,variant==4?std::uint8_t{}:std::uint8_t{0x80});
            if(variant==5)Put(n+0x44,Address(first)); // Unregistered pointer cannot authorize erase.
        }
        h->Install();Put(h->local.block+0x30,2.0F);
        const auto result=h->native.Populate(h->graph,h->local,h->protection,ImportPoint::ReplacementNoWorker);
        if(result!=(replacing?Result::Ok:Result::InvalidNativeState))
            std::cerr<<"replacement variant "<<variant<<" result "<<static_cast<unsigned>(result)<<" context "<<h->native.ContextCheck()<<" stage "<<h->native.ImportStage()<<'\n';
        CHECK(result==(replacing?Result::Ok:Result::InvalidNativeState));
        CHECK(h->creates==(replacing?2U:0U));
        CHECK(Get<std::uintptr_t>(head)==a&&Get<std::uintptr_t>(head+4)==b&&Get<std::uint32_t>(n+4)==2);
        CHECK(Get<std::uintptr_t>(n+0xC)==a); // Installer, not importer, owns N replacement.
        CHECK(Get<std::uint32_t>(h->local.result+4)==(replacing?1U:0U));
    }
}
}
int main() {
    static_assert(sizeof(std::uintptr_t) == 4);
    Roundtrip(); RetailDefaultCapture(); GuardsAndFaults(); ReplacementContinuation(); InstalledHostCapture(); SpecialRoutes();OrderedNativeMarkers();CompleteInstalledRoute();PendingReplacementPreservesLiveRoute();
    std::cout << "Native route: real memory validation/publication and injected calls PASS; native admission/game UNPROVEN\n";
}
