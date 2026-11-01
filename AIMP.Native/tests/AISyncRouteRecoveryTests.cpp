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
}
int Run() {
    scope.session[0]=1;scope.authorityEpoch=scope.worldEpoch=scope.consumerEpoch=1;
    gEngine=&Engine;gQualify=&QualifyScope;gState=new State;
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
