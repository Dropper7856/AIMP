#include "aimp/AISyncRouteCodec.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>

using namespace aimp::ai_sync::route;
namespace {
std::size_t checks{};
void Check(bool ok, int line) { ++checks; if (!ok) { std::cerr << "route codec invariant " << line << '\n'; std::abort(); } }
#define CHECK(value) Check((value),__LINE__)
Graph Fixture() {
    Graph graph{};
    graph.key.scope.session = {0x33,0x22,0x11,0,0x55,0x44,0x77,0x66,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    graph.key.scope.authorityEpoch = 2; graph.key.scope.worldEpoch = 3; graph.key.scope.consumerEpoch = 1;
    graph.key.owner = {0x1234,0x5678,5,6,aimp::ai_sync::transactions::Capability::Movement,4};
    graph.key.commandId = 7; graph.key.parameterRevision = 2; graph.key.routeRevision = 3;
    graph.auxiliary = {-0.0F,1,2,3,4,5}; graph.statuses.fill(0x40000000);
    graph.goal={12.5F,-6,3};
    graph.resourceCount = 1; graph.nodeCount = 2;
    constexpr char level[] = "TECH_COMMS";
    std::copy(std::begin(level),std::end(level),graph.resources[0].level.begin());
    for (std::size_t i = 0; i < 32; ++i) graph.resources[0].immutableDigest[i] = static_cast<std::uint8_t>(i+1);
    graph.resources[0].headerTuple = {10,20,30};
    graph.nodes[0].ordinal = 7; graph.nodes[1].ordinal = 9;
    for (std::size_t i = 0; i < 18; ++i) graph.nodes[i/9].vectors[i%9] = static_cast<float>(i);
    return graph;
}
std::vector<std::uint8_t> Hex(const char* path) {
    std::ifstream file(path); CHECK(file.good());
    std::string hex, line;
    while (std::getline(file,line)) hex += line;
    hex.erase(std::remove_if(hex.begin(),hex.end(),[](unsigned char ch) { return std::isspace(ch) != 0; }),hex.end());
    CHECK(hex.size()%2 == 0);
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < hex.size(); i+=2) bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i,2),nullptr,16)));
    return bytes;
}
void Codec(const char* path) {
    auto graph = Fixture(); CHECK(Validate(graph) == Result::Ok);
    std::vector<std::uint8_t> bytes; CHECK(Encode(graph,bytes) == Result::Ok);
    CHECK(bytes.size() == 412 && bytes == Hex(path));
    auto marked=graph;marked.nodes[0].markers=0x090807U;marked.nodes[1].markers=11;
    std::vector<std::uint8_t> markedBytes;CHECK(Encode(marked,markedBytes)==Result::Ok);
    const auto markedPath=(std::filesystem::path(path).parent_path()/"aisync-route-markers-v4.hex").string();
    CHECK(markedBytes==Hex(markedPath.c_str()));Graph markedDecoded{};
    CHECK(Decode(markedBytes,marked.key,markedDecoded)==Result::Ok&&markedDecoded==marked);
    for(auto invalid:{1U,6U,10U,12U,0x00090007U,0xFFU}) {auto bad=marked;bad.nodes[0].markers=invalid;CHECK(Validate(bad)!=Result::Ok);}
    auto special=graph;special.nodeCount=3;special.nodes[1].type=1;special.nodes[2]=special.nodes[0];
    std::vector<std::uint8_t> specialBytes;CHECK(Encode(special,specialBytes)==Result::Ok);
    const auto specialPath=(std::filesystem::path(path).parent_path()/"aisync-route-special-v4.hex").string();
    CHECK(specialBytes==Hex(specialPath.c_str()));Graph specialDecoded{};
    CHECK(Decode(specialBytes,special.key,specialDecoded)==Result::Ok&&specialDecoded==special);
    Graph decoded{}; CHECK(Decode(bytes,graph.key,decoded) == Result::Ok);
    CHECK(decoded == graph && std::bit_cast<std::uint32_t>(decoded.auxiliary[0]) == 0x80000000U);
    const Graph sentinel = decoded;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        CHECK(Decode(std::span(bytes).first(i),graph.key,decoded) != Result::Ok); CHECK(decoded == sentinel);
    }
    auto stale = graph.key; ++stale.owner.birth;
    CHECK(Decode(bytes,stale,decoded) == Result::WrongKey && decoded == sentinel);
    auto invalidGoal=bytes;invalidGoal[150]=0xC0;invalidGoal[151]=0x7F;
    CHECK(Decode(invalidGoal,graph.key,decoded)==Result::InvalidGraph);
    auto oldVersion=bytes;oldVersion[4]=2;CHECK(Decode(oldVersion,graph.key,decoded)==Result::InvalidEncoding);
    ++bytes[144]; CHECK(Decode(bytes,graph.key,decoded) == Result::WrongKey); --bytes[144];
    auto companion = graph; companion.key.part = 1;
    std::vector<std::uint8_t> companionBytes;
    CHECK(Encode(companion,companionBytes) == Result::Ok && Decode(companionBytes,companion.key,decoded) == Result::Ok);
    CHECK(Decode(companionBytes,graph.key,decoded) == Result::WrongKey);
    companion.key.part = 2; CHECK(Validate(companion) == Result::InvalidKey);
    bytes.push_back(0); CHECK(Decode(bytes,graph.key,decoded) == Result::InvalidEncoding); bytes.pop_back();
    graph.nodes[1].ordinal = graph.nodes[0].ordinal;
    CHECK(Encode(graph,bytes) == Result::Ok && Decode(bytes,graph.key,decoded) == Result::Ok);
    CHECK(decoded.nodeCount == 2 && decoded.nodes[0].ordinal == decoded.nodes[1].ordinal);
    graph.statuses[3] |= 0x40; CHECK(Validate(graph) == Result::UnsupportedDomain);
    graph = Fixture(); graph.nodes[0].type = 1;
    CHECK(Encode(graph,bytes)==Result::Ok && Decode(bytes,graph.key,decoded)==Result::Ok && decoded==graph);
    graph.nodes[0].type=0; CHECK(Validate(graph)==Result::Ok);
    graph.nodes[0].type=3; CHECK(Validate(graph)==Result::UnsupportedDomain);
    graph = Fixture(); graph.nodes[0].vectors[0] = (std::numeric_limits<float>::infinity)(); CHECK(Validate(graph) == Result::InvalidGraph);
    graph = Fixture(); graph.resources[0].immutableDigest.fill(0); CHECK(Validate(graph) == Result::InvalidGraph);
    graph = Fixture(); graph.nodeCount = 0; CHECK(Validate(graph) == Result::InvalidGraph);
    graph = Fixture(); graph.resources[1] = graph.resources[0]; graph.resourceCount = 2; CHECK(Validate(graph) == Result::InvalidGraph);
}
void Refusals(const char* path) {
    Graph graph{};graph.key=Fixture().key;graph.failure=Failure::Unsupported;
    std::vector<std::uint8_t> bytes;CHECK(Encode(graph,bytes)==Result::Ok&&bytes.size()==kHeaderBytes);
    const auto fixture=(std::filesystem::path(path).parent_path()/"aisync-route-failure-v4.hex").string();CHECK(bytes==Hex(fixture.c_str()));
    Graph decoded{};CHECK(Decode(bytes,graph.key,decoded)==Result::Ok&&decoded==graph);
    const auto retained=decoded;
    for(std::size_t n=0;n<bytes.size();++n)CHECK(Decode(std::span(bytes).first(n),graph.key,decoded)!=Result::Ok&&decoded==retained);
    for(auto offset:{128U,132U,104U,148U,12U}) {
        auto invalid=bytes;invalid[offset]^=0x80;CHECK(Decode(invalid,graph.key,decoded)!=Result::Ok&&decoded==retained);
    }
    graph.goal[0]=-0.0F;CHECK(Validate(graph)==Result::InvalidGraph);graph.goal[0]=0;
    graph.failure=static_cast<Failure>(256);CHECK(Validate(graph)==Result::InvalidGraph);
    for(std::uint32_t reason=1;reason<=5;++reason) {
        graph.failure=static_cast<Failure>(reason);CHECK(Encode(graph,bytes)==Result::Ok&&Decode(bytes,graph.key,decoded)==Result::Ok&&decoded==graph);
    }
}
void Generated() {
    std::uint32_t random = 0x253260;
    for (std::uint32_t iteration = 0; iteration < 2000; ++iteration) {
        auto graph = Fixture(); graph.key.commandId += iteration;
        graph.nodes = {}; // Unused fixed-array slots are outside the wire graph.
        graph.nodeCount = 1 + iteration%kMaxNodes;
        for (std::size_t i = 0; i < graph.nodeCount; ++i) {
            auto& node = graph.nodes[i]; random = random*1664525U + 1013904223U;
            node.ordinal = random % 3000; node.type=1+(random%2);
            for (auto& coordinate : node.vectors) {
                random = random*1664525U + 1013904223U;
                coordinate = static_cast<float>(static_cast<int>(random%200000)-100000)/10.0F;
            }
        }
        std::vector<std::uint8_t> bytes; CHECK(Encode(graph,bytes) == Result::Ok);
        Graph decoded{}; CHECK(Decode(bytes,graph.key,decoded) == Result::Ok && decoded == graph);
    }
    auto graph = Fixture(); graph.resourceCount = kMaxResources; graph.nodeCount = kMaxNodes;
    for (std::size_t i = 1; i < kMaxResources; ++i) { graph.resources[i] = graph.resources[0]; graph.resources[i].state = static_cast<std::uint32_t>(i); }
    for (std::size_t i = 0; i < kMaxNodes; ++i) { graph.nodes[i].resourceIndex = static_cast<std::uint32_t>(i%kMaxResources); graph.nodes[i].ordinal = static_cast<std::uint32_t>(i); }
    std::vector<std::uint8_t> bytes; CHECK(Encode(graph,bytes) == Result::Ok && bytes.size() == kMaxBytes && kMaxBytes == 14656);
    Graph decoded{}; CHECK(Decode(bytes,graph.key,decoded) == Result::Ok && decoded == graph);
}
}
int main(int argc, char** argv) {
    CHECK(argc == 2); Codec(argv[1]); Refusals(argv[1]); Generated();
    std::cout << "AISync route codec: " << checks << " checks passed\n";
}
