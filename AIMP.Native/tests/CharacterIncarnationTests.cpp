#include "aimp/CharacterIncarnation.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iterator>
#include <cctype>
#include <string>
#include <thread>
#include <vector>

namespace aimp::character_incarnations {
struct RegistryTestAccess {static void Exhaust(Registry& registry) noexcept{registry.serial_=UINT64_MAX;}};
}
using namespace aimp::character_incarnations;
namespace {
void Check(bool value,int line){if(!value){std::cerr<<"character incarnation invariant at "<<line<<'\n';std::abort();}}
#define CHECK(value) Check((value),__LINE__)
void CreationAndReuse() {
    Registry registry;Ticket first{},next{},found{};
    CHECK(registry.Created(7,0x100000,15,first)==Result::Ok&&first.origin==Origin::Created);
    CHECK(registry.Current(first)&&registry.Find(7,0x100000,found)==Result::Ok&&found==first);
    CHECK(registry.PublishLocal(first)==Result::Ok&&registry.Local()==first);
    CHECK(registry.Retire(first)==Result::Ok&&!registry.Current(first));
    CHECK(registry.Local()==Ticket{}&&registry.PublishLocal(first)==Result::Missing);
    CHECK(registry.BindExisting(7,0x100000,15,found)==Result::Missing);
    CHECK(registry.Created(7,0x100000,15,next)==Result::Ok&&next.serial>first.serial);
    CHECK(!registry.Current(first)&&registry.Current(next));
    CHECK(registry.PublishLocal(next)==Result::Ok);
    CHECK(registry.Retire(first)==Result::Missing&&registry.Current(next));
    CHECK(registry.Created(7,0x100000,15,found)==Result::Ok&&found.serial>next.serial);
    CHECK(!registry.Current(next)&&registry.Current(found));
    CHECK(registry.Local()==Ticket{}&&registry.PublishLocal(found)==Result::Ok);
    auto altered=found;altered.runtimeId=99;CHECK(registry.Retire(altered)==Result::Missing&&registry.Current(found));
    CHECK(registry.Retire(found)==Result::Ok&&registry.Retire(found)==Result::Duplicate);
    CHECK(registry.Local()==Ticket{});
}
void BaselineAndWorlds() {
    Registry registry;Ticket baseline{},same{},created{};
    CHECK(registry.BindExisting(7,0x100000,15,baseline)==Result::Ok&&baseline.origin==Origin::ExistingAtBinding);
    CHECK(registry.BindExisting(7,0x100000,15,same)==Result::Duplicate&&same==baseline);
    CHECK(registry.Created(7,0x100000,15,created)==Result::Ok&&created.origin==Origin::Created);
    CHECK(created.serial>baseline.serial&&!registry.Current(baseline));
    CHECK(registry.BindExisting(7,0x100000,15,same)==Result::Duplicate&&same==created);
    CHECK(registry.AdvanceWorld(8)==Result::Ok&&!registry.Current(created));
    CHECK(registry.Created(8,0x100000,15,same)==Result::Ok&&same.serial>created.serial);
    CHECK(registry.Retire(created)==Result::WrongGeneration&&registry.Current(same));
    CHECK(registry.AdvanceWorld(7)==Result::WrongGeneration&&registry.Current(same));
    CHECK(registry.Created(7,0x200000,16,created)==Result::WrongGeneration&&created==Ticket{});
    CHECK(registry.Created(0,0x100000,15,created)==Result::Invalid);
    CHECK(registry.Created(8,0xFFF,15,created)==Result::Invalid);
    CHECK(registry.Created(8,0x100000,0,created)==Result::Invalid);
    CHECK(registry.BindExisting(8,0x100000,16,created)==Result::Uncertain);
    CHECK(!registry.Current(same)); // A missed lifetime change closes coverage.
    CHECK(registry.AdvanceWorld(8)==Result::Uncertain);
    CHECK(registry.AdvanceWorld(9)==Result::Ok);
}
void BoundedHistoryAndExhaustion() {
    Registry registry;Ticket first{},ticket{};
    for(std::size_t i=0;i<Registry::kCapacity;++i) {
        CHECK(registry.Created(7,0x100000+i*0x100,15,ticket)==Result::Ok);
        if(!i)first=ticket;
        CHECK(registry.Retire(ticket)==Result::Ok);
    }
    CHECK(registry.BindExisting(7,first.character,first.runtimeId,ticket)==Result::Missing);
    CHECK(registry.Created(7,first.character,first.runtimeId,ticket)==Result::Ok&&ticket.serial>first.serial);
    const auto current=ticket;
    CHECK(registry.Created(7,0x400000,16,ticket)==Result::Capacity&&ticket==Ticket{});
    CHECK(!registry.Current(current)&&registry.AdvanceWorld(7)==Result::Uncertain);
    CHECK(registry.AdvanceWorld(8)==Result::Ok);
    CHECK(registry.Created(8,0x400000,16,ticket)==Result::Ok&&ticket.serial>current.serial);
    Registry exhausted;RegistryTestAccess::Exhaust(exhausted);
    CHECK(exhausted.Created(7,0x100000,15,ticket)==Result::Uncertain);
    CHECK(exhausted.AdvanceWorld(8)==Result::Ok);
    CHECK(exhausted.Created(8,0x100000,15,ticket)==Result::Uncertain); // Never wrap serials.
}
void ConcurrentObservations() {
    Registry registry;std::array<std::array<std::uint64_t,2000>,4> serials{};
    std::array<std::thread,4> workers;
    for(std::size_t worker=0;worker<workers.size();++worker)workers[worker]=std::thread([&,worker] {
        for(auto& serial:serials[worker]) {
            Ticket ticket{};CHECK(registry.Created(7,0x100000+worker*0x100,15,ticket)==Result::Ok);
            CHECK(registry.Current(ticket));serial=ticket.serial;CHECK(registry.Retire(ticket)==Result::Ok);
        }
    });
    for(auto& worker:workers)worker.join();
    std::vector<std::uint64_t> all;
    for(const auto& row:serials)all.insert(all.end(),row.begin(),row.end());
    std::sort(all.begin(),all.end());
    CHECK(all.front()==1U&&all.back()==all.size()&&std::adjacent_find(all.begin(),all.end())==all.end());
}
void LocalCodec(const char* path) {
    std::ifstream input(path);std::string hex{std::istreambuf_iterator<char>{input},{}};
    hex.erase(std::remove_if(hex.begin(),hex.end(),[](unsigned char c){return std::isspace(c)!=0;}),hex.end());
    CHECK(hex.size()==80U);std::array<std::uint8_t,kLocalStateBytes> fixture{};
    for(std::size_t i=0;i<fixture.size();++i)fixture[i]=static_cast<std::uint8_t>(std::stoul(hex.substr(i*2,2),nullptr,16));
    const Ticket value{7U,0x0102030405060708ULL,0x11223344U,0x55667788U,Origin::ExistingAtBinding};
    Ticket decoded{};CHECK(DecodeLocalState(fixture,decoded)&&decoded==value);
    std::array<std::uint8_t,kLocalStateBytes> encoded{};CHECK(EncodeLocalState(value,encoded)&&encoded==fixture);
    for(std::size_t i=0;i<fixture.size();++i)CHECK(!DecodeLocalState(std::span(fixture).first(i),decoded)&&decoded==Ticket{});
    std::array<std::uint8_t,kLocalStateBytes+1> trailing{};
    CHECK(!DecodeLocalState(trailing,decoded)&&!EncodeLocalState(value,trailing));
    for(const auto offset:{0U,4U,33U,36U}) {auto bad=fixture;bad[offset]^=0x80U;CHECK(!DecodeLocalState(bad,decoded));}
    CHECK(EncodeLocalState({},encoded)&&DecodeLocalState(encoded,decoded)&&decoded==Ticket{});
    encoded[32]=1U;CHECK(!DecodeLocalState(encoded,decoded));
    auto bad=value;bad.serial=0;CHECK(!EncodeLocalState(bad,encoded));
    bad=value;bad.origin=static_cast<Origin>(2);CHECK(!EncodeLocalState(bad,encoded));
    CHECK(!aimp::character_incarnations::NativeRegistry().Current(value)); // Decoding grants no native identity.
}
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    CreationAndReuse();BaselineAndWorlds();BoundedHistoryAndExhaustion();ConcurrentObservations();
    LocalCodec(argv[1]);
    std::cout<<"Character incarnation tickets: 5 groups and 8000 concurrent construction/retirement observations PASS (value layer only).\n";
}
