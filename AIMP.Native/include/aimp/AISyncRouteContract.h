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
// Diagnostic reason for PreserveEndpointPrefix, 0 when the suffix is exact.
// auxiliary[0..2] is R+10, the Host's query start copied into every capture.
// auxiliary[3..5] is re-read from live N+50 by every CaptureInstalled and
// moves with native progress/goal edits: comparing it refused every chase
// endpoint edit (Host check 113) and every Client replacement (site 374).
// The admitted graph keeps its own copy; it is not route identity.
[[nodiscard]] inline std::uint32_t EndpointPrefixMismatch(const Graph& complete,const Graph& remaining) noexcept {
    if(!SameRouteCommand(complete.key,remaining.key))return 1;
    if(complete.failure!=Failure::None||remaining.failure!=Failure::None)return 2;
    if(!complete.nodeCount||complete.nodeCount>kMaxNodes||!remaining.nodeCount||remaining.nodeCount>complete.nodeCount)return 3;
    if(!SameResources(complete,remaining))return 4;
    for(std::size_t i=0;i<3;++i)if(complete.auxiliary[i]!=remaining.auxiliary[i])return 5;
    if(complete.statuses!=remaining.statuses)return 6;
    // The corridor is its ordered native elements. A goal edit lets retail
    // re-smooth portal points (vectors) inside those SAME elements, and the
    // node at the cursor drops the markers it has already executed. Neither
    // changes the corridor the Client already follows: 7 = element sequence
    // changed (a real re-route, needs admission), 11 = later node's markers.
    const auto offset=complete.nodeCount-remaining.nodeCount;
    for(std::size_t i=0;i+1<remaining.nodeCount;++i) {
        const auto& before=complete.nodes[offset+i];const auto& now=remaining.nodes[i];
        if(before.type!=now.type||before.resourceIndex!=now.resourceIndex||before.ordinal!=now.ordinal)return 7;
        if(i&&before.markers!=now.markers)return 11;
    }
    const auto& previous=complete.nodes[complete.nodeCount-1];
    const auto& terminal=remaining.nodes[remaining.nodeCount-1];
    if(!SameNodeIdentity(previous,terminal))return 8;
    if(previous!=terminal||complete.goal!=remaining.goal) {
        if(terminal.type!=2)return 9;
        for(std::size_t i=0;i<9;++i)if(terminal.vectors[i]!=remaining.goal[i%3])return 10;
    }
    return 0;
}
// CaptureInstalled observes the remaining Host suffix. Endpoint-only edits
// must retain the first admitted prefix for a Client which has not consumed it.
// Exact0044D320's same-handle/stop<=0 branch changes ONLY the terminal vectors
// (all three equal the new goal) and B.goal. Any other mutation needs a fresh
// native admission; it cannot be described as an endpoint-only revision.
[[nodiscard]] inline bool PreserveEndpointPrefix(const Graph& complete,const Graph& remaining,Graph& output) noexcept {
    if(EndpointPrefixMismatch(complete,remaining))return false;
    Graph next=complete;next.key=remaining.key;next.goal=remaining.goal;
    next.nodes[next.nodeCount-1]=remaining.nodes[remaining.nodeCount-1];
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
