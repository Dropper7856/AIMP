#pragma once
#include "aimp/AISyncRouteCodec.h"
#include <cmath>
#include <bit>

namespace aimp::ai_sync::route {
[[nodiscard]] inline bool HybridResumeCurrent(std::uint64_t rejectedCommand,std::uint32_t rejectedRevision,
    std::uint64_t installedCommand,std::uint32_t installedRevision) noexcept {
    return installedCommand>rejectedCommand||(installedCommand==rejectedCommand&&installedRevision>=rejectedRevision);
}
// A route installer proves only an installed corridor, not locomotion.
// After the hybrid motor is released, this observer requires physical native
// progress or a settled Host pose before handing off permanently. No writes
// to Character, pathfinding or network protocol are performed here.
enum class NativeHandoffResult : std::uint8_t { Pending, Confirmed, Stalled };
struct NativeHandoffProbe {
    static constexpr std::uint64_t kTimeoutMs = 900U;
    static constexpr std::uint64_t kMinimumObservationMs = 80U;
    static constexpr std::uint64_t kMaximumSampleGapMs = 200U;
    static constexpr float kMinTravelSquared = 0.0144F; // 12 cm
    static constexpr float kMaxStepSquared = 4.0F;      // Absolute discontinuity guard
    static constexpr float kMaximumSpeed = 12.0F;       // Generous native locomotion bound (m/s)
    static constexpr float kPositionTolerance = 0.02F;  // Numerical/animation sampling tolerance
    static constexpr float kSettledSquared = 0.36F;     // 60 cm when Host stationary
    bool started{}, baseline{};
    std::uint64_t startMs{}, baselineMs{}, sampleMs{}, observedMs{};
    std::array<float,3> origin{}, previous{};
    [[nodiscard]] NativeHandoffResult Rebase(std::uint64_t now,
        const std::array<float,3>& local) noexcept {
        baseline=true;origin=previous=local;baselineMs=sampleMs=now;
        return NativeHandoffResult::Pending;
    }
    [[nodiscard]] NativeHandoffResult Observe(std::uint64_t now, bool valid,
        const std::array<float,3>& local, const std::array<float,3>& host, bool hostMoving,
        bool discontinuity=false) noexcept {
        if(!started) {started=true;startMs=observedMs=now;}
        if(now<observedMs||now-startMs>=kTimeoutMs)return NativeHandoffResult::Stalled;
        observedMs=now;
        // No displacement spanning a lost authority/sample interval can be
        // attributed to native locomotion. Keep the global deadline running.
        if(!valid||discontinuity) {baseline=false;return NativeHandoffResult::Pending;}
        for(std::size_t i=0;i<3;++i)if(!std::isfinite(local[i])||!std::isfinite(host[i]))
            {baseline=false;return NativeHandoffResult::Pending;}
        if(!baseline||now-sampleMs>kMaximumSampleGapMs)return Rebase(now,local);
        const auto delta=now-sampleMs;
        float spatialStep{},travel{},gap{};
        for(std::size_t i=0;i<3;++i) {
            const float error=local[i]-host[i];gap+=error*error;
            const float d=local[i]-previous[i];spatialStep+=d*d;
            // Vertical transport/vent motion is not evidence that a ground
            // navigation route has resumed its locomotion motor.
            if(i==1)continue;
            const float total=local[i]-origin[i];
            travel+=total*total;
        }
        // A teleport or external repositioning is not evidence of native
        // locomotion. Discard the previous baseline rather than accepting its
        // large accumulated displacement on the next stationary sample.
        const float allowed=kPositionTolerance+kMaximumSpeed*static_cast<float>(delta)*0.001F;
        if(spatialStep>kMaxStepSquared||spatialStep>allowed*allowed)return Rebase(now,local);
        previous=local;sampleMs=now;
        if(now-baselineMs>=kMinimumObservationMs &&
            (travel>=kMinTravelSquared || (!hostMoving && gap<=kSettledSquared)))
            return NativeHandoffResult::Confirmed;
        return NativeHandoffResult::Pending;
    }
};
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
// Endpoint-only Host revision of basis, applied to the installed range that
// a corridor join or trail bridge admitted from it. Both must still share the
// exact terminal node and goal; otherwise a fresh native admission is needed.
[[nodiscard]] inline bool InstalledEndpointRevision(const Graph& basis,const Graph& installed,
    const Graph& next,Graph& output) noexcept {
    if(!installed.nodeCount||installed.nodeCount>kMaxNodes||!basis.nodeCount||basis.nodeCount>kMaxNodes||
        !EndpointRevision(basis,next)||installed.goal!=basis.goal||
        installed.nodes[installed.nodeCount-1]!=basis.nodes[basis.nodeCount-1])return false;
    output=installed;output.key=next.key;output.goal=next.goal;
    output.nodes[output.nodeCount-1]=next.nodes[next.nodeCount-1];
    return true;
}
// Host corridor trail. During a chase the Host re-plans every few frames from
// ITS current pose, so a slightly late Client actor stands BEHIND the start of
// every new Host route (check 203) and was handed to the hybrid motor. The
// trail keeps the Host's own previously authored nodes for the same owner and
// splices each newer Host route at its first element. Every node remains
// Host-authored: no Client path, connector, nearest point or pose correction.
struct HostTrail {
    Graph graph;               // key/auxiliary/goal/statuses of the latest Host graph
    std::size_t latestBegin{}; // trail index standing for latest.nodes[0]
};
[[nodiscard]] inline bool SameTrailOwner(const Key& a,const Key& b) noexcept {
    return a.scope==b.scope&&a.owner==b.owner&&a.part==b.part;
}
// Same navigation element: equal resource and ordinal. Type1 special links
// use link ordinals and never stand for an area.
[[nodiscard]] inline bool SameElement(const Graph& a,const Node& x,const Graph& b,const Node& y) noexcept {
    return x.type!=1&&y.type!=1&&x.ordinal==y.ordinal&&x.resourceIndex<a.resourceCount&&
        y.resourceIndex<b.resourceCount&&a.resources[x.resourceIndex]==b.resources[y.resourceIndex];
}
// previous[0..j] + latest[1..], where previous[j] is the most recent node in
// latest's first element. Keeping previous[j] preserves the corridor node the
// Host actually traversed there; latest[0] is only the Host query start.
// Without an exact junction, same resource table or owner, the trail restarts.
inline void ExtendHostTrail(const HostTrail* previous,const Graph& latest,HostTrail& output) noexcept {
    output.graph=latest;output.latestBegin=0;
    if(!previous||latest.failure!=Failure::None||!latest.nodeCount||latest.nodeCount>kMaxNodes||latest.nodes[0].type==1)return;
    const auto& old=previous->graph;
    if(old.failure!=Failure::None||!old.nodeCount||old.nodeCount>kMaxNodes||
        !SameTrailOwner(old.key,latest.key)||!SameResources(old,latest))return;
    std::size_t junction=old.nodeCount;
    for(std::size_t k=old.nodeCount;k-->0;)if(SameElement(old,old.nodes[k],latest,latest.nodes[0])){junction=k;break;}
    if(junction==old.nodeCount||!junction)return;
    const std::size_t total=junction+latest.nodeCount;
    std::size_t start=total>kMaxNodes?total-kMaxNodes:0;
    while(start<junction&&old.nodes[start].type==1)++start;
    if(start>=junction)return;
    Graph merged=latest;merged.nodes={};merged.nodeCount=0;
    for(std::size_t k=start;k<=junction;++k)merged.nodes[merged.nodeCount++]=old.nodes[k];
    for(std::size_t k=1;k<latest.nodeCount;++k)merged.nodes[merged.nodeCount++]=latest.nodes[k];
    if(Validate(merged)!=Result::Ok)return;
    output.graph=merged;output.latestBegin=junction-start;
}
// The Host trail suffix from the most recent node, BEFORE the latest Host
// start, lying in the Client actor's own area. areaOf maps a trail node to the
// local area reference (0 when unmappable). The native StartsHere/admission
// proofs must still accept the result; this only selects Host-authored nodes.
template<class AreaOf>
[[nodiscard]] bool BridgeHostTrail(const HostTrail& trail,const Graph& latest,std::uint32_t area,
    AreaOf&& areaOf,Graph& output) noexcept {
    const auto& graph=trail.graph;
    if(!area||graph.key!=latest.key||!trail.latestBegin||trail.latestBegin>=graph.nodeCount||graph.nodeCount>kMaxNodes)return false;
    for(std::size_t i=trail.latestBegin;i-->0;) {
        if(graph.nodes[i].type==1||areaOf(graph,i)!=area)continue;
        Graph suffix=graph;suffix.nodes={};suffix.nodeCount=graph.nodeCount-i;
        for(std::size_t k=0;k<suffix.nodeCount;++k)suffix.nodes[k]=graph.nodes[i+k];
        if(Validate(suffix)!=Result::Ok)return false;
        output=suffix;return true;
    }
    return false;
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
