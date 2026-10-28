#include "aimp/AISyncNativeOwnership.h"
#include <cstdlib>
#include <iostream>
#include <map>

using namespace aimp::ai_sync::execution;
namespace {
void Check(bool value,int line) {if(!value){std::cerr<<"native ownership invariant at "<<line<<'\n';std::abort();}}
#define CHECK(value) Check((value),__LINE__)
struct Memory {
    std::map<std::uintptr_t,std::uint8_t> bytes;
    std::uintptr_t failAt{},mutateAt{};
    bool mutated{};
    unsigned reads{};
    ActorLifetime actor{0x100000U,15U,7U};
    static constexpr std::uint32_t cycle=0x200000U,movement=0x300000U;
    template<class T> void Put(std::uintptr_t at,T value) {
        const auto* source=reinterpret_cast<const std::uint8_t*>(&value);
        for(std::size_t i=0;i<sizeof(T);++i)bytes[at+i]=source[i];
    }
    Memory() {
        Put(actor.character+0x50U,actor.runtimeId);Put(actor.character+0x2CD0U,static_cast<std::uint32_t>(actor.character));
        Put(actor.character+0x36CU,cycle);Put(cycle+4U,static_cast<std::uint32_t>(actor.character));
        Cycle(0xEABF80U,1U);Put(cycle+0x19U,std::uint8_t{7});
        Put(actor.character+0x297U,std::uint8_t{});Put(actor.character+0x32CU,std::uint32_t{1});
        Put(actor.character+0x2CD8U,std::uint32_t{});Put(actor.character+0x2CE4U,std::uint32_t{});
        Put(actor.character+0x42A8U,movement);Put(actor.character+0x42ACU,0x310000U);
        for(const auto offset:{4U,0x14U,0x2CU,0x38U,0x40U,0x44U,0x5CU})Put(movement+0xC4U+offset,std::uint32_t{});
    }
    void Cycle(std::uint32_t type,std::uint8_t state) {Put(cycle,0x400000U+type);Put(cycle+0x1AU,state);}
    static bool Read(void* context,std::uintptr_t at,void* target,std::size_t size) noexcept {
        auto& m=*static_cast<Memory*>(context);++m.reads;
        auto* data=static_cast<std::uint8_t*>(target);
        for(std::size_t i=0;i<size;++i){if(at+i==m.failAt)return false;const auto it=m.bytes.find(at+i);if(it==m.bytes.end())return false;data[i]=it->second;}
        if(at==m.mutateAt&&!m.mutated){m.mutated=true;m.Put(m.actor.character+0x50U,m.actor.runtimeId+1U);}
        return true;
    }
    OwnershipCheck Inspect(bool quiet=true) {return InspectMovementBoundary({0x400000U,this,&Read},actor,quiet);}
};
void NativeIdleAndCycles() {
    Memory m;CHECK(m.Inspect()==OwnershipCheck::Ready);CHECK(m.Inspect(false)==OwnershipCheck::Ready);
    m.Cycle(0xEABF80U,0x38U);CHECK(m.Inspect()==OwnershipCheck::Waiting);CHECK(m.Inspect(false)==OwnershipCheck::Ready);
    m.Cycle(0xEABEC0U,0x3BU);CHECK(m.Inspect()==OwnershipCheck::Ready);
    for(const auto type:{0xEAE5D0U,0xEAE690U,0xEAE1B8U,0x123456U})for(const auto state:{1U,0x38U,0x3BU,0x42U,0x4EU,0x4FU}) {
        m.Cycle(type,static_cast<std::uint8_t>(state));CHECK(m.Inspect(false)==OwnershipCheck::Waiting);
    }
    m.Cycle(0xEABF80U,1U);m.Put(m.actor.character+0x297U,std::uint8_t{1});CHECK(m.Inspect(false)==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x297U,std::uint8_t{});m.Put(m.actor.character+0x32CU,std::uint32_t{3});
    CHECK(m.Inspect(false)==OwnershipCheck::Waiting);
}
void SchedulerAndNavigation() {
    Memory m;
    // Regression from the v262 Client trace: neutral state 1, not locomotion
    // Idle. It may acquire ownership only with the same full native proof.
    m.Cycle(0xEAD858U,1U);
    CHECK(m.Inspect(false)==OwnershipCheck::Ready&&m.Inspect()==OwnershipCheck::Ready);
    for(const auto offset:{0x2CD8U,0x2CE4U}) {
        m.Put(m.actor.character+offset,std::uint32_t{1});CHECK(m.Inspect()==OwnershipCheck::Waiting);
        m.Put(m.actor.character+offset,std::uint32_t{});CHECK(m.Inspect()==OwnershipCheck::Ready);
    }
    for(const auto offset:{4U,0x14U,0x2CU,0x38U,0x40U,0x44U,0x5CU}) {
        m.Put(Memory::movement+0xC4U+offset,std::uint32_t{1});CHECK(m.Inspect()==OwnershipCheck::Waiting);
        m.Put(Memory::movement+0xC4U+offset,std::uint32_t{});CHECK(m.Inspect()==OwnershipCheck::Ready);
    }
    m.Put(m.actor.character+0x42ACU,std::uint32_t{});CHECK(m.Inspect()==OwnershipCheck::Unknown);
}
void FaultsAndLifetimeChanges() {
    Memory baseline;
    for(const auto& [address,value]:baseline.bytes) {
        static_cast<void>(value);Memory m;m.failAt=address;
        CHECK(m.Inspect()==OwnershipCheck::Unknown);
    }
    for(const auto at:{baseline.actor.character+0x32CU,Memory::movement+0xC4U+0x5CU}) {
        Memory m;m.mutateAt=at;CHECK(m.Inspect()==OwnershipCheck::Unknown&&m.mutated);
    }
    Memory stale;++stale.actor.runtimeId;CHECK(stale.Inspect()==OwnershipCheck::Unknown);
    Memory owner;owner.Put(Memory::cycle+4U,std::uint32_t{0x110000});CHECK(owner.Inspect()==OwnershipCheck::Unknown);
    Memory invalid;auto actor=invalid.actor;actor.nativeGeneration=0;
    CHECK(InspectMovementBoundary({0x400000U,&invalid,&Memory::Read},actor,true)==OwnershipCheck::Reject&&invalid.reads==0U);
}
void NativeInteractionReplacement() {
    Memory m;m.Cycle(0xEABE00U,0x37U);
    const std::uintptr_t table=0x400000U+0xEABE00U;
    m.Put(table,std::uint32_t{0x6577F0U});m.Put(table+0x60U,std::uint32_t{0x657870U});
    const auto inspect=[&] {return InspectInteractionBoundary({0x400000U,&m,&Memory::Read},m.actor);};
    CHECK(m.Inspect(false)==OwnershipCheck::Waiting); // Movement ownership remains stricter.
    CHECK(inspect()==OwnershipCheck::Ready); // The Host replaces this exact running cycle.
    const auto owned=[&] {return InspectAuthoritativeExecutionBoundary({0x400000U,&m,&Memory::Read},m.actor,true);};
    CHECK(owned()==OwnershipCheck::Ready);
    m.Put(Memory::movement+0xC4U+0x5CU,std::uint32_t{1});CHECK(owned()==OwnershipCheck::Waiting);
    m.Put(Memory::movement+0xC4U+0x5CU,std::uint32_t{});
    m.Put(m.actor.character+0x2CD8U,std::uint32_t{1});CHECK(owned()==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x2CD8U,std::uint32_t{});CHECK(owned()==OwnershipCheck::Ready);
    for(const auto state:{0x42U,0x4EU,0x4FU}) {
        m.Cycle(0xEABE00U,static_cast<std::uint8_t>(state));CHECK(inspect()==OwnershipCheck::Waiting);
    }
    m.Cycle(0xEABE00U,0x37U);m.Put(table,std::uint32_t{0xF0000000U});
    CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(table,std::uint32_t{0x6577F0U});m.failAt=table+0x60U;CHECK(inspect()==OwnershipCheck::Unknown);
    m.failAt=0;m.Put(m.actor.character+0x297U,std::uint8_t{1});CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x297U,std::uint8_t{});m.Put(m.actor.character+0x32CU,std::uint32_t{3});
    CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x32CU,std::uint32_t{1});m.mutateAt=table+0x60U;
    CHECK(inspect()==OwnershipCheck::Unknown);
}
void ExactNativeExecutionProvenance() {
    Memory m;m.actor.creationSerial=11;
    aimp::ai_sync::transactions::Scope scope{};scope.session[0]=1;scope.authorityEpoch=scope.worldEpoch=scope.consumerEpoch=1;
    const aimp::ai_sync::transactions::OwnerKey owner{1,2,3,4,aimp::ai_sync::transactions::Capability::Movement,5};
    NativeExecutionAnchor anchor{scope,owner,m.actor,Memory::cycle,7};
    const auto matches=[&] {return MatchesNativeExecution({0x400000U,&m,&Memory::Read},anchor,scope,owner,m.actor);};
    CHECK(matches());
    ++scope.consumerEpoch;CHECK(!matches());--scope.consumerEpoch;
    ++anchor.owner.birth;CHECK(!matches());--anchor.owner.birth;
    ++anchor.actor.creationSerial;CHECK(!matches());--anchor.actor.creationSerial;
    ++anchor.actor.nativeGeneration;CHECK(!matches());--anchor.actor.nativeGeneration;
    m.Put(Memory::cycle+0x19U,std::uint8_t{8});CHECK(!matches());m.Put(Memory::cycle+0x19U,std::uint8_t{7});
    m.Put(m.actor.character+0x50U,m.actor.runtimeId+1U);CHECK(!matches());m.Put(m.actor.character+0x50U,m.actor.runtimeId);
    m.failAt=Memory::cycle+0x19U;CHECK(!matches());
}
}
int main() {
    NativeIdleAndCycles();SchedulerAndNavigation();FaultsAndLifetimeChanges();NativeInteractionReplacement();ExactNativeExecutionProvenance();
    std::cout<<"Native movement boundary: typed Idle, scheduler/navigation retention and lifetime faults PASS (modeled reads only).\n";
}
