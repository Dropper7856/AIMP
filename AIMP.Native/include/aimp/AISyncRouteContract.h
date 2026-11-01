#pragma once
#include "aimp/AISyncRouteCodec.h"
#include <cmath>
#include <bit>

namespace aimp::ai_sync::route {
[[nodiscard]] inline bool HybridResumeCurrent(std::uint64_t rejectedCommand,std::uint32_t rejectedRevision,
    std::uint64_t installedCommand,std::uint32_t installedRevision) noexcept {
    return installedCommand>rejectedCommand||(installedCommand==rejectedCommand&&installedRevision>=rejectedRevision);
}
// Local-only contract checks. No new wire fields or inferred bridge nodes.
[[nodiscard]] inline bool SameRouteCommand(const Key& a,const Key& b) noexcept {
    return a.scope==b.scope&&a.owner==b.owner&&a.commandId==b.commandId&&a.part==b.part;
}
[[nodiscard]] inline bool NewerRoute(const Key& previous,const Key& next) noexcept {
    return SameRouteCommand(previous,next)&&
        (next.parameterRevision>previous.parameterRevision||
         (next.parameterRevision==previous.parameterRevision&&next.routeRevision>previous.routeRevision));
}
[[nodiscard]] inline bool SameNodeIdentity(const Node& a,const Node& b) noexcept {
    return a.type==b.type&&a.resourceIndex==b.resourceIndex&&a.ordinal==b.ordinal&&a.markers==b.markers;
}
[[nodiscard]] inline bool SameResources(const Graph& a,const Graph& b) noexcept {
    if(a.resourceCount!=b.resourceCount||a.resourceCount>kMaxResources)return false;
    for(std::size_t i=0;i<a.resourceCount;++i)if(a.resources[i]!=b.resources[i])return false;
    return true;
}
// Pre-reset compound proof. A later native append shares the preceding B's
// terminal node; positional proximity cannot substitute for that identity.
[[nodiscard]] inline bool AppendSharesTail(const Graph& previous,const Graph& next) noexcept {
    if(!previous.nodeCount||previous.nodeCount>kMaxNodes||!next.nodeCount||next.nodeCount>kMaxNodes)return false;
    const auto& tail=previous.nodes[previous.nodeCount-1];const auto& first=next.nodes[0];
    if(tail.resourceIndex>=previous.resourceCount||first.resourceIndex>=next.resourceCount||
        previous.resourceCount>kMaxResources||next.resourceCount>kMaxResources||
        previous.resources[tail.resourceIndex]!=next.resources[first.resourceIndex]||
        tail.type!=first.type||tail.ordinal!=first.ordinal)return false;
    for(std::size_t i=0;i<9;++i)if(std::bit_cast<std::uint32_t>(tail.vectors[i])!=std::bit_cast<std::uint32_t>(first.vectors[i]))return false;
    auto markers=tail.markers;
    if(next.nodeCount==1) {
        unsigned shift{};for(auto tags=markers;tags;tags>>=8)shift+=8;
        if(shift>=32)return false;markers|=11U<<shift;
    }
    return markers==first.markers;
}
// CaptureInstalled observes the remaining Host suffix. Endpoint-only edits
// must retain the first admitted prefix for a Client which has not consumed it.
// Exact0044D320's same-handle/stop<=0 branch changes ONLY the terminal vectors
// (all three equal the new goal) and B.goal. Any other mutation needs a fresh
// native admission; it cannot be described as an endpoint-only revision.
[[nodiscard]] inline bool PreserveEndpointPrefix(const Graph& complete,const Graph& remaining,Graph& output) noexcept {
    if(!SameRouteCommand(complete.key,remaining.key)||complete.failure!=Failure::None||remaining.failure!=Failure::None||
        !complete.nodeCount||complete.nodeCount>kMaxNodes||!remaining.nodeCount||remaining.nodeCount>complete.nodeCount||
        !SameResources(complete,remaining)||complete.auxiliary!=remaining.auxiliary||complete.statuses!=remaining.statuses)return false;
    const auto offset=complete.nodeCount-remaining.nodeCount;
    for(std::size_t i=0;i+1<remaining.nodeCount;++i)if(complete.nodes[offset+i]!=remaining.nodes[i])return false;
    const auto& previous=complete.nodes[complete.nodeCount-1];
    const auto& terminal=remaining.nodes[remaining.nodeCount-1];
    if(!SameNodeIdentity(previous,terminal))return false;
    if(previous!=terminal||complete.goal!=remaining.goal) {
        if(terminal.type!=2)return false;
        for(std::size_t i=0;i<9;++i)if(terminal.vectors[i]!=remaining.goal[i%3])return false;
    }
    Graph next=complete;next.key=remaining.key;next.goal=remaining.goal;
    next.nodes[next.nodeCount-1]=terminal;
    output=next;return true;
}
[[nodiscard]] inline bool EndpointRevision(const Graph& previous,const Graph& next) noexcept {
    if(previous.nodeCount!=next.nodeCount)return false;
    Graph preserved{};
    return PreserveEndpointPrefix(previous,next,preserved)&&preserved==next;
}
// Native0043D330 clips one FIXED segment to one mapped area. Reaching its
// end is insufficient when its start was outside (positive entry fraction).
[[nodiscard]] inline bool FullAreaSegment(std::uint32_t intersects,float entry,float exit) noexcept {
    return (intersects&255U)==1U&&std::isfinite(entry)&&std::isfinite(exit)&&entry<=0.0F&&exit>=1.0F;
}
// Native00443330 must confirm that fixed segment without crossing an area.
// Truncation, partial reach and even a successful detour are all rejected.
[[nodiscard]] inline bool SameAreaTrace(std::uint32_t status,float reach,std::uint32_t area,
    std::span<const std::uint32_t> references) noexcept {
    return status==0x40000000U&&reach==1.0F&&area&&references.size()==2&&references[0]==area&&references[1]==area;
}
} // namespace aimp::ai_sync::route
