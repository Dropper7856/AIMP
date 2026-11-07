// Exercise the actual slot-failure path, including its recovery latch, without
// native code execution. Linking Core supplies the normal runtime; it is not
// initialized, so the cancellation queue deliberately refuses the first try.
#include "../src/AISyncRouteExecution.cpp"
#include <cstdlib>
#include <iostream>

namespace aimp::ai_sync::route_execution {
namespace {
bool engine=true;
transactions::Scope scope{};
bool Engine() noexcept {return engine;}
bool QualifyScope(const transactions::Scope& value) noexcept {return value==scope;}
void Check(bool value,int line) {
    if(!value){std::cerr<<"route recovery regression at "<<line<<'\n';std::exit(1);}
}
#define CHECK(value) Check((value),__LINE__)
execution::NavigationBinding Binding(std::uintptr_t character,std::uint64_t command,std::uint32_t revision=1) {
    execution::NavigationBinding result{};
    result.command.scope=scope;
    result.command.owner={1,2,1,1,transactions::Capability::Movement,1};
    result.command.actor={character,3,true,4};
    result.command.command=command;result.command.revision=revision;
    result.policy=movement::Navigation::HostRoute;
    return result;
}
unsigned erases{};bool eraseFails{};
bool Erase(std::uintptr_t n,bool route) noexcept {
    ++erases;CHECK(route);
    if(eraseFails)return false;
    auto* words=reinterpret_cast<std::uintptr_t*>(n);
    auto* head=reinterpret_cast<std::uintptr_t*>(words[0]);
    head[0]=head[1]=words[0];words[1]=0;words[3]=words[0];
    return true;
}
void SameWorldRetirement() {
    for(const unsigned scenario:{0U,1U,2U,3U,4U,5U}) {
        std::array<std::uintptr_t,256> movement{};
        std::array<std::uintptr_t,3> control{0,1,1};
        std::array<std::uintptr_t,2> reference{reinterpret_cast<std::uintptr_t>(movement.data()),reinterpret_cast<std::uintptr_t>(control.data())};
        std::array<std::array<std::uintptr_t,4>,4> heads{};
        std::array<std::array<std::uintptr_t,4>,20> nodes{};
        const auto n=reference[0]+0xC4U;
        const auto write=[&](unsigned offset,std::uintptr_t value){*reinterpret_cast<std::uintptr_t*>(n+offset)=value;};
        for(unsigned i=0;i<4;++i) {
            const auto head=reinterpret_cast<std::uintptr_t>(heads[i].data());heads[i][0]=heads[i][1]=head;
            write(std::array<unsigned,4>{0,0x10,0x28,0x34}[i],head);
        }
        const auto routeHead=reinterpret_cast<std::uintptr_t>(heads[0].data());
        heads[0][0]=reinterpret_cast<std::uintptr_t>(nodes.front().data());heads[0][1]=reinterpret_cast<std::uintptr_t>(nodes.back().data());
        for(unsigned i=0;i<nodes.size();++i)nodes[i]={
            i+1<nodes.size()?reinterpret_cast<std::uintptr_t>(nodes[i+1].data()):routeHead,
            i?reinterpret_cast<std::uintptr_t>(nodes[i-1].data()):routeHead,reference[0],reference[1]};
        write(4,20);write(0xC,heads[0][0]);
        auto pin=std::make_shared<Pin>();
        pin->movement=std::make_unique<native_resource::StrongLease>(reinterpret_cast<std::uintptr_t>(reference.data()),sizeof(movement));
        CHECK(*pin->movement&&control[1]==2);
        for(unsigned i=0;i<3;++i) {
            auto slot=std::make_unique<Slot>();slot->binding=Binding(0x1000,10+i);slot->skipped=true;
            slot->local.navigation=n;slot->pin=pin;gState->slots[i]=std::move(slot);
        }
        auto& slot=*gState->slots[0];erases=0;eraseFails=scenario==5U;
        CHECK(!RetireDestroyedRoute(slot,&Erase)&&erases==0); // Active scopes never drain.
        RetireScope(scope);
        if(scenario==1U)write(0x5C,1); // Result/worker still owns route.
        if(scenario==2U)write(0x2C,1); // A native B remains.
        if(scenario==3U)gState->slots[1]->scopeRetired=false; // Another live owner shares M.
        if(scenario==4U)nodes[10][1]=0; // Malformed route ring.
        const bool drained=RetireDestroyedRoute(slot,&Erase);
        if(scenario==0U) {
            CHECK(drained&&Quiet(slot)&&erases==1);
            CHECK(RetireDestroyedRoute(*gState->slots[1],&Erase)&&Quiet(*gState->slots[1]));
        } else {
            CHECK(!drained&&erases==(scenario==5U?1U:0U));
            if(scenario==5U)CHECK(!RetireDestroyedRoute(slot,&Erase)&&erases==1&&slot.retirementFault);
        }
        for(unsigned i=0;i<3;++i)gState->slots[i].reset();
        pin.reset();CHECK(control[1]==1); // Independent M pin released exactly once.
    }
    for(auto& entry:gState->hybrid)entry={};
}
}
int Run() {
    scope.session[0]=1;scope.authorityEpoch=scope.worldEpoch=scope.consumerEpoch=1;
    gEngine=&Engine;gQualify=&QualifyScope;gState=new State;
    SameWorldRetirement();
    // An old strict route's retained storage on N must neither block a newer
    // HostGuided B nor grant an obsolete/unknown B a native solver.
    using Ownership=execution::NavigationOwnership;
    for(const auto policy:{movement::Navigation::LocalNative,movement::Navigation::HostGuided}) {
        CHECK(PlanQuery(Ownership::Ready,policy,true,true)==QueryPlan::Retail);
        CHECK(PlanQuery(Ownership::Admitting,policy,true,true)==QueryPlan::Retail);
        CHECK(PlanQuery(Ownership::Ready,policy,true,false)==QueryPlan::Refuse);
        CHECK(PlanQuery(Ownership::Rejected,policy,false,true)==QueryPlan::Refuse);
        CHECK(PlanQuery(Ownership::Waiting,policy,false,true)==QueryPlan::Refuse);
    }
    CHECK(PlanQuery(Ownership::Unowned,movement::Navigation{},true,true)==QueryPlan::Imported);
    CHECK(PlanQuery(Ownership::Unowned,movement::Navigation{},false,false)==QueryPlan::Retail);
    CHECK(PlanQuery(Ownership::Ready,movement::Navigation::HostRoute,true,true)==QueryPlan::Imported);
    execution::NavigationBinding actual{};
    {
        auto slot=std::make_unique<Slot>();slot->binding=Binding(0x1000,7);
        const auto rejected=gRejected.load();
        Fail(*slot,484,Result::LocalInvalidity);
        CHECK(slot->failed&&!slot->rejectionQueued); // Runtime not bound yet.
        CHECK(gRejected.load()==rejected+1);
        CHECK(HybridBinding(0x1000,actual)&&actual.command.command==7);
        Fail(*slot,557,Result::UnsupportedDomain);
        CHECK(gRejected.load()==rejected+1); // Retrying cancellation is idempotent.
    }
    // Native slot retirement cannot erase value-only fallback permission.
    CHECK(HybridBinding(0x1000,actual)&&actual.command.command==7);
    {
        auto next=std::make_unique<Slot>();next->binding=Binding(0x1000,9,2);
        Fail(*next,484,Result::LocalInvalidity);
        auto delayed=std::make_unique<Slot>();delayed->binding=Binding(0x1000,8,100);
        Fail(*delayed,557,Result::UnsupportedDomain);
        CHECK(HybridBinding(0x1000,actual)&&actual.command.command==9&&actual.command.revision==2);
        auto oldRevision=std::make_unique<Slot>();oldRevision->binding=Binding(0x1000,9,1);
        Fail(*oldRevision,484,Result::LocalInvalidity);
        CHECK(HybridBinding(0x1000,actual)&&actual.command.revision==2);
    }
    // Refusals before a Slot exists use the same recovery/cancellation path.
    auto second=Binding(0x2000,10);second.command.owner.entityA=9;
    CHECK(!Reject(second));
    CHECK(HybridBinding(0x2000,actual)&&actual.command.command==10);
    CHECK(!HybridBinding(0x3000,actual));
    engine=false;CHECK(!HybridBinding(0x1000,actual));engine=true;
    ConfirmActorDestroyed({0x1000,3,1,4});
    CHECK(!HybridBinding(0x1000,actual));
    CHECK(HybridBinding(0x2000,actual));
    // Scope can change without a new world/name/cookie. The accepted barrier
    // revokes the old consumer's permission before any new pose is consumed.
    auto nextScope=scope;++nextScope.consumerEpoch;
    BeginScope(nextScope);
    CHECK(!HybridBinding(0x2000,actual));
    CHECK(!Reject(second));CHECK(HybridBinding(0x2000,actual));
    ++scope.worldEpoch;
    CHECK(!HybridBinding(0x2000,actual)); // No permission survives a world change.
    delete gState;gState=nullptr;
    std::cout<<"Actual route-slot failures activate recovery before cancellation; lifetime and scope fences PASS\n";
    return 0;
}
}
int main(){return aimp::ai_sync::route_execution::Run();}
