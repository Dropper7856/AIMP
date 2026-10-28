#include "aimp/AISyncRouteCodec.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace aimp::ai_sync::route {
namespace {
template<class T> void Put(std::span<std::uint8_t> out, std::size_t at, T value) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i) out[at+i] = static_cast<std::uint8_t>(value >> (i*8));
}
template<class T> T Get(std::span<const std::uint8_t> in, std::size_t at) noexcept {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(in[at+i]) << (i*8);
    return value;
}
bool Coordinate(float value) noexcept { return std::isfinite(value) && std::abs(value) <= 100000.0F; }
void Float(std::span<std::uint8_t> bytes, std::size_t at, float value) noexcept { Put(bytes,at,std::bit_cast<std::uint32_t>(value)); }
float Float(std::span<const std::uint8_t> bytes, std::size_t at) noexcept { return std::bit_cast<float>(Get<std::uint32_t>(bytes,at)); }
}
bool ValidKey(const Key& key) noexcept {
    transactions::Record record{};
    record.scope = key.scope; record.owner = key.owner; record.commandId = key.commandId;
    record.parameterRevision = key.parameterRevision; record.globalSeq = record.ownerSeq = 1;
    return key.routeRevision != 0 && key.part <= 1 && transactions::Validate(record) == transactions::Result::Ok;
}
Result Validate(const Graph& graph) noexcept {
    if (!ValidKey(graph.key)) return Result::InvalidKey;
    if(graph.failure!=Failure::None) {
        if(static_cast<std::uint32_t>(graph.failure)>static_cast<std::uint32_t>(Failure::NativeFault)||graph.resourceCount||graph.nodeCount)return Result::InvalidGraph;
        for(auto value:graph.auxiliary)if(std::bit_cast<std::uint32_t>(value)!=0)return Result::InvalidGraph;
        for(auto value:graph.goal)if(std::bit_cast<std::uint32_t>(value)!=0)return Result::InvalidGraph;
        for(auto value:graph.statuses)if(value)return Result::InvalidGraph;
        return Result::Ok;
    }
    if (graph.resourceCount == 0 || graph.resourceCount > kMaxResources || graph.nodeCount == 0 || graph.nodeCount > kMaxNodes)
        return Result::InvalidGraph;
    for (auto value : graph.auxiliary) if (!Coordinate(value)) return Result::InvalidGraph;
    for (auto value : graph.goal) if (!Coordinate(value)) return Result::InvalidGraph;
    // 00463420 checks each stage's status separately; 0x40 partial output is
    // deliberately outside the initial full-route domain. Preserve all words.
    for (auto status : graph.statuses) if (status != 0x40000000U) return Result::UnsupportedDomain;
    std::array<bool,kMaxResources> used{};
    std::size_t firstUse = 0;
    for (std::size_t i = 0; i < graph.resourceCount; ++i) {
        if (route_identity::Validate(graph.resources[i]) != route_identity::Result::Ok) return Result::InvalidGraph;
        for (std::size_t j = 0; j < i; ++j) if (graph.resources[i] == graph.resources[j]) return Result::InvalidGraph;
    }
    for (std::size_t i = 0; i < graph.nodeCount; ++i) {
        const auto& node = graph.nodes[i];
        if (node.type > 2) return Result::UnsupportedDomain;
        if(!ValidMarkers(node.markers))return Result::UnsupportedDomain;
        if (node.resourceIndex >= graph.resourceCount || node.ordinal >= (1U << 22)) return Result::InvalidGraph;
        for (auto value : node.vectors) if (!Coordinate(value)) return Result::InvalidGraph;
        if (!used[node.resourceIndex]) {
            // A unique canonical table in first encounter order, without ever
            // sorting/deduplicating the semantic sequence of nodes.
            if (node.resourceIndex != firstUse++) return Result::InvalidGraph;
            used[node.resourceIndex] = true;
        }
    }
    return firstUse == graph.resourceCount ? Result::Ok : Result::InvalidGraph;
}
Result Encode(const Graph& graph, std::vector<std::uint8_t>& output) noexcept {
    const auto valid = Validate(graph);
    if (valid != Result::Ok) return valid;
    try {
        std::vector<std::uint8_t> storage(kHeaderBytes + graph.resourceCount*kResourceBytes + graph.nodeCount*kNodeBytes);
        std::span<std::uint8_t> out(storage);
        Put(out,0,kMagic); Put(out,4,kVersion); Put(out,6,static_cast<std::uint16_t>(kHeaderBytes));
        Put(out,8,static_cast<std::uint32_t>(out.size()));
        Put(out,12,static_cast<std::uint32_t>(graph.resourceCount)); Put(out,16,static_cast<std::uint32_t>(graph.nodeCount));
        Put(out,20,graph.key.routeRevision); Put(out,24,graph.key.commandId);
        std::copy(graph.key.scope.session.begin(),graph.key.scope.session.end(),out.begin()+32);
        Put(out,48,graph.key.scope.authorityEpoch); Put(out,56,graph.key.scope.worldEpoch); Put(out,64,graph.key.scope.consumerEpoch);
        const auto& owner = graph.key.owner;
        Put(out,72,owner.ownerEpoch); Put(out,80,owner.entityA); Put(out,84,owner.entityB);
        Put(out,88,owner.birth); Put(out,92,owner.actorClass); Put(out,96,static_cast<std::uint32_t>(owner.capability));
        Put(out,100,graph.key.parameterRevision);
        for (std::size_t i = 0; i < 6; ++i) Float(out,104+i*4,graph.auxiliary[i]);
        for (std::size_t i = 0; i < 4; ++i) Put(out,128+i*4,graph.statuses[i]);
        if(graph.failure!=Failure::None)Put(out,128,0x80000000U|static_cast<std::uint32_t>(graph.failure));
        Put(out,144,graph.key.part);
        for (std::size_t i=0;i<3;++i) Float(out,148+i*4,graph.goal[i]);
        std::size_t at = kHeaderBytes;
        for (std::size_t i = 0; i < graph.resourceCount; ++i,at+=kResourceBytes) {
            const auto& resource = graph.resources[i];
            for (std::size_t j = 0; j < resource.level.size(); ++j) out[at+j] = static_cast<std::uint8_t>(resource.level[j]);
            Put(out,at+96,resource.state); Put(out,at+100,resource.formatVersion);
            std::copy(resource.immutableDigest.begin(),resource.immutableDigest.end(),out.begin()+at+104);
            for (std::size_t j = 0; j < 3; ++j) Put(out,at+136+j*4,resource.headerTuple[j]);
        }
        for (std::size_t i = 0; i < graph.nodeCount; ++i,at+=kNodeBytes) {
            const auto& node = graph.nodes[i];
            Put(out,at,node.type); Put(out,at+4,node.resourceIndex); Put(out,at+8,node.ordinal);
            for (std::size_t j = 0; j < 9; ++j) Float(out,at+12+j*4,node.vectors[j]);
        Put(out,at+48,node.markers);
        }
        output.swap(storage); return Result::Ok;
    } catch (...) { return Result::InvalidGraph; }
}
Result DecodeKey(std::span<const std::uint8_t> bytes, Key& output) noexcept {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes || Get<std::uint32_t>(bytes,0) != kMagic ||
        Get<std::uint16_t>(bytes,4) != kVersion || Get<std::uint16_t>(bytes,6) != kHeaderBytes ||
        Get<std::uint32_t>(bytes,8) != bytes.size()) return Result::InvalidEncoding;
    Key key{};
    const auto resourceCount=Get<std::uint32_t>(bytes,12),nodeCount=Get<std::uint32_t>(bytes,16);
    if ((resourceCount==0)!=(nodeCount==0) || resourceCount > kMaxResources || nodeCount > kMaxNodes ||
        bytes.size() != kHeaderBytes + resourceCount*kResourceBytes + nodeCount*kNodeBytes) return Result::InvalidEncoding;
    key.routeRevision = Get<std::uint32_t>(bytes,20); key.commandId = Get<std::uint64_t>(bytes,24);
    std::copy_n(bytes.begin()+32,16,key.scope.session.begin());
    key.scope.authorityEpoch = Get<std::uint64_t>(bytes,48); key.scope.worldEpoch = Get<std::uint64_t>(bytes,56);
    key.scope.consumerEpoch = Get<std::uint64_t>(bytes,64);
    auto& owner = key.owner;
    owner.ownerEpoch = Get<std::uint64_t>(bytes,72); owner.entityA = Get<std::uint32_t>(bytes,80); owner.entityB = Get<std::uint32_t>(bytes,84);
    owner.birth = Get<std::uint32_t>(bytes,88); owner.actorClass = Get<std::uint32_t>(bytes,92);
    owner.capability = static_cast<transactions::Capability>(Get<std::uint32_t>(bytes,96));
    key.parameterRevision = Get<std::uint32_t>(bytes,100);
    key.part = Get<std::uint32_t>(bytes,144);
    if (!ValidKey(key)) return Result::InvalidKey;
    output = key; return Result::Ok;
}
Result Decode(std::span<const std::uint8_t> bytes, const Key& expected, Graph& output) noexcept {
    if (!ValidKey(expected)) return Result::InvalidKey;
    Key key{}; const auto envelope = DecodeKey(bytes,key);
    if (envelope != Result::Ok) return envelope;
    if (key != expected) return Result::WrongKey;
    Graph graph{}; graph.key = key;
    for (std::size_t i=0;i<3;++i) graph.goal[i]=Float(bytes,148+i*4);
    graph.resourceCount = Get<std::uint32_t>(bytes,12); graph.nodeCount = Get<std::uint32_t>(bytes,16);
    for (std::size_t i = 0; i < 6; ++i) graph.auxiliary[i] = Float(bytes,104+i*4);
    for (std::size_t i = 0; i < 4; ++i) graph.statuses[i] = Get<std::uint32_t>(bytes,128+i*4);
    if(!graph.resourceCount&&!graph.nodeCount) {
        if((graph.statuses[0]&0x80000000U)==0)return Result::InvalidEncoding;
        graph.failure=static_cast<Failure>(graph.statuses[0]&0x7FFFFFFFU);graph.statuses[0]=0;
        if(graph.failure==Failure::None)return Result::InvalidEncoding;
    }
    std::size_t at = kHeaderBytes;
    for (std::size_t i = 0; i < graph.resourceCount; ++i,at+=kResourceBytes) {
        auto& resource = graph.resources[i];
        for (std::size_t j = 0; j < resource.level.size(); ++j) resource.level[j] = static_cast<char>(bytes[at+j]);
        resource.state = Get<std::uint32_t>(bytes,at+96); resource.formatVersion = Get<std::uint32_t>(bytes,at+100);
        std::copy_n(bytes.begin()+at+104,32,resource.immutableDigest.begin());
        for (std::size_t j = 0; j < 3; ++j) resource.headerTuple[j] = Get<std::uint32_t>(bytes,at+136+j*4);
    }
    for (std::size_t i = 0; i < graph.nodeCount; ++i,at+=kNodeBytes) {
        auto& node = graph.nodes[i];
        node.type = Get<std::uint32_t>(bytes,at); node.resourceIndex = Get<std::uint32_t>(bytes,at+4); node.ordinal = Get<std::uint32_t>(bytes,at+8);
        for (std::size_t j = 0; j < 9; ++j) node.vectors[j] = Float(bytes,at+12+j*4);
        node.markers=Get<std::uint32_t>(bytes,at+48);
    }
    const auto valid = Validate(graph);
    if (valid != Result::Ok) return valid;
    output = graph; return Result::Ok;
}
} // namespace aimp::ai_sync::route
