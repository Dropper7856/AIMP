#pragma once
#include "aimp/AISyncRouteCodec.h"
#include <cmath>
#include <limits>
#include <algorithm>

namespace aimp::ai_sync::movement {
// Value-only cursor. The Host chooses the corridor; retail plans each local
// segment. This never writes a pose, infers an action or accelerates the gait.
struct GuidanceCursor {
    std::uint32_t revision{},routeRevision{};
    std::size_t index{};
    std::uint32_t ordinal{},resource{};
    route_identity::ResourceKey resourceKey{};
    std::uint32_t type{};
    std::array<float,3> target{};
    bool initialized{};
};
struct GuidedGoal {
    std::array<float,3> position{};
    bool intermediate{},paced{},holding{},anticipating{};
};
struct ObservedGoal {
    std::array<float,3> position{};
    float lead{};
    bool valid{},comparable{};
    // The newest physical sample is not a corner still owed by the Client.
    // Only at this tip may a known Host corridor provide forward lookahead.
    bool atTip{};
};
struct HostProgress {
    std::array<float,3> position{},velocity{};
    std::uint32_t gait{};
    bool protectedMotion{};
    ObservedGoal observed{};
};
struct ProgressClock {
    std::uint64_t hostTick{},arrival{};
    // Repeated heartbeats containing an old physical observation must not
    // renew permission. Host ticks compare only with Host ticks; age uses
    // Client intake time, including time spent behind a causal barrier.
    bool Observe(std::uint64_t tick,std::uint64_t received) noexcept {
        if(!tick||!received||tick<=hostTick)return false;
        hostTick=tick;arrival=received;return true;
    }
    [[nodiscard]] bool Fresh(std::uint64_t now) const noexcept {
        return arrival&&now>=arrival&&now-arrival<=600U;
    }
};
struct PacingCursor {
    std::array<float,3> direction{},anchor{};
    bool directionKnown{},holding{};
};
struct PathProgress {
    float along{},error{std::numeric_limits<float>::max()};
    bool valid{};
    std::size_t segment{};
    std::array<float,3> position{};
    bool ambiguous{};
};
inline float DistanceSquared(const std::array<float,3>& a,const std::array<float,3>& b) noexcept {
    float value{};for(std::size_t i=0;i<3;++i){const auto d=a[i]-b[i];value+=d*d;}return value;
}
inline std::array<float,3> NodePosition(const route::Node& n) noexcept {
    return {n.vectors[0],n.vectors[1],n.vectors[2]};
}
inline bool FinitePoint(const std::array<float,3>& point) noexcept {
    return std::all_of(point.begin(),point.end(),[](float value){return std::isfinite(value)&&std::abs(value)<=100000.0F;});
}
// A parameter-only Update can precede the next route publication. Its older
// Host geometry remains a hint for this exact command while the authored goal
// is unchanged. This never satisfies native route import or request authority.
[[nodiscard]] inline bool RetainGuidanceGeometry(const route::Graph& graph,
    const route::Key& current,const std::array<float,3>& authoredGoal) noexcept {
    return graph.failure==route::Failure::None&&graph.key.scope==current.scope&&
        graph.key.owner==current.owner&&graph.key.commandId==current.commandId&&
        graph.key.part==current.part&&graph.key.parameterRevision<current.parameterRevision&&
        graph.nodeCount>=2U&&graph.nodeCount<=route::kMaxNodes&&FinitePoint(authoredGoal)&&
        graph.goal==authoredGoal;
}
// Host PHYSICAL observations, independent of request parameter revisions. A
// moving endpoint must not discard a bend the lagging Client has yet to walk.
// This is a bounded source of native goals; it never writes a Character pose.
class ObservedTrail {
    static constexpr std::size_t kPoints=256;
    std::array<std::array<float,3>,kPoints> points_{};
    std::array<float,3> tip_{};
    std::uint64_t first_{},next_{},cursor_{},selected_{},tick_{};
    bool joined_{};
    [[nodiscard]] const std::array<float,3>& Point(std::uint64_t i) const noexcept {
        return i==next_?tip_:points_[i%kPoints];
    }
public:
    void Clear() noexcept {first_=next_=cursor_=selected_=tick_=0;joined_=false;}
    void Observe(const std::array<float,3>& position,std::uint64_t tick,bool ordinary) noexcept {
        if(!ordinary||!FinitePoint(position)||!tick){Clear();return;}
        if(tick_&&tick<=tick_)return;
        const auto maxStep=tick_?std::max(1.0F,static_cast<float>(tick-tick_)*0.012F+0.1F):1.0F;
        if(tick_&&(tick-tick_>600U||std::abs(position[1]-tip_[1])>1.5F||
            DistanceSquared(position,tip_)>maxStep*maxStep))Clear();
        tick_=tick;tip_=position;
        if(next_==first_||DistanceSquared(points_[(next_-1U)%kPoints],position)>=0.04F) {
            points_[next_%kPoints]=position;++next_;
            if(next_-first_>kPoints){++first_;if(cursor_<first_){cursor_=first_;joined_=false;}}
        }
    }
    [[nodiscard]] std::size_t Size() const noexcept {return static_cast<std::size_t>(next_-first_);}
    [[nodiscard]] ObservedGoal Goal(const std::array<float,3>& local) noexcept {
        if(!tick_||!FinitePoint(local)||first_==next_)return {};
        // Join the closest segment once. Thereafter the cursor only advances:
        // a return through the same corridor cannot rewind to its earlier leg.
        if(!joined_) {
            float best=std::numeric_limits<float>::max();cursor_=first_;
            for(auto i=first_;i<next_;++i) {
                const auto& a=Point(i);const auto& b=Point(i+1U);
                const auto length=DistanceSquared(a,b);float dot{};
                for(std::size_t k=0;k<3;++k)dot+=(local[k]-a[k])*(b[k]-a[k]);
                const auto t=length>0.000001F?std::clamp(dot/length,0.0F,1.0F):0.0F;
                std::array<float,3> p{};for(std::size_t k=0;k<3;++k)p[k]=a[k]+t*(b[k]-a[k]);
                if(std::abs(p[1]-local[1])>0.75F)continue;
                const auto d=DistanceSquared(p,local);
                if(d<best){best=d;cursor_=i;}
            }
            joined_=true;
        }
        while(cursor_<next_&&std::abs(Point(cursor_)[1]-local[1])<=0.75F&&
            DistanceSquared(Point(cursor_),local)<=0.5625F)++cursor_;
        // Through-goals and native strides need not hit every sample sphere.
        // Prove passage on the current straight leg instead of chasing an old
        // point backwards. Bound the scan to 4m and stop AT its first bend;
        // proximity to a later leg/crossing cannot consume that bend.
        const auto bend=[&](std::uint64_t i) noexcept {
            if(i<=first_||i>=next_)return false;
            const auto& a=Point(i-1U);const auto& b=Point(i);const auto& c=Point(i+1U);
            float dot{},aa{},bb{};
            for(std::size_t k=0;k<3;++k){const auto u=b[k]-a[k],v=c[k]-b[k];dot+=u*v;aa+=u*u;bb+=v*v;}
            return aa>0.000001F&&bb>0.000001F&&dot/std::sqrt(aa*bb)<0.9F;
        };
        auto passed=cursor_;float scanned{};
        for(auto i=cursor_;i<next_&&scanned<=4.0F;++i) {
            if(bend(i))break;
            const auto& a=Point(i);const auto& b=Point(i+1U);
            const auto length=DistanceSquared(a,b);const auto segmentLength=std::sqrt(length);
            if(scanned+segmentLength>4.0F)break;
            scanned+=segmentLength;
            if(length<0.000001F)continue;
            float dot{};for(std::size_t k=0;k<3;++k)dot+=(local[k]-a[k])*(b[k]-a[k]);
            if(dot<0.0F)continue;
            const auto t=std::min(dot/length,1.0F);
            std::array<float,3> projected{};
            for(std::size_t k=0;k<3;++k)projected[k]=a[k]+t*(b[k]-a[k]);
            if(std::abs(projected[1]-local[1])<=0.75F&&DistanceSquared(projected,local)<=0.5625F)passed=i+1U;
        }
        cursor_=passed;
        auto goal=Point(cursor_);auto goalIndex=cursor_;
        // Pass small samples on a straight leg, but retain every substantial
        // bend. Lookahead consists exclusively of positions already observed.
        for(auto i=cursor_;i<next_&&DistanceSquared(goal,local)<4.0F;++i) {
            const auto& a=i>first_?Point(i-1U):local;
            const auto& b=Point(i);const auto& d=Point(i+1U);
            float dot{},aa{},bb{};
            for(std::size_t k=0;k<3;++k){const auto u=b[k]-a[k],v=d[k]-b[k];dot+=u*v;aa+=u*u;bb+=v*v;}
            if(aa>0.000001F&&bb>0.000001F&&dot/std::sqrt(aa*bb)<0.9F)break;
            goal=d;goalIndex=i+1U;
        }
        // Keep selected goals ordered as well as consumed samples. Otherwise
        // body recoil/lateral motion shrinks the local-distance lookahead and
        // sends a previously issued target backwards on the SAME Host leg.
        // This watermark does not consume a corner or certify body progress.
        // Pin the live tip to its last stored sample: next_ is a virtual point
        // and must not reserve a future sample before the Host has visited it.
        selected_=std::max(std::max(selected_,first_),std::min(goalIndex,next_-1U));
        if(goalIndex<selected_){goalIndex=selected_;goal=Point(goalIndex);}
        // Beyond the newest observation, the final physical segment can prove
        // advance. Behind it, path order wins over the Host's current heading.
        float lead{};bool comparable{};
        if(next_>first_+1U&&cursor_>=next_-1U) {
            const auto& a=Point(next_-2U);const auto& b=tip_;
            const auto dx=b[0]-a[0],dz=b[2]-a[2],length=std::hypot(dx,dz);
            if(length>0.01F&&std::abs(local[1]-b[1])<=0.75F) {
                lead=((local[0]-b[0])*dx+(local[2]-b[2])*dz)/length;
                comparable=std::abs(((local[0]-b[0])*dz-(local[2]-b[2])*dx)/length)<=2.0F;
            }
        } else if(cursor_<next_) {
            lead=-std::sqrt(DistanceSquared(local,Point(cursor_)));
            for(auto i=cursor_;i<next_;++i)lead-=std::sqrt(DistanceSquared(Point(i),Point(i+1U)));
            comparable=true;
        }
        return {goal,lead,true,comparable,goalIndex==next_};
    }
};
// Read-only projection onto the Host's ordered polyline. A different floor or
// a point far outside that corridor cannot certify progress along it.
[[nodiscard]] inline PathProgress ProjectProgress(const route::Graph* graph,const std::array<float,3>& point) noexcept {
    PathProgress best{};
    if(!graph||graph->failure!=route::Failure::None||graph->nodeCount<2||graph->nodeCount>route::kMaxNodes||!FinitePoint(point))return best;
    std::array<float,route::kMaxNodes> closeProgress{};std::size_t closeCount{};
    float along{};
    for(std::size_t i=1;i<graph->nodeCount;++i) {
        const auto a=NodePosition(graph->nodes[i-1]),b=NodePosition(graph->nodes[i]);
        if(!FinitePoint(a)||!FinitePoint(b))return {};
        const auto length=std::sqrt(DistanceSquared(a,b));
        if(length<0.001F)continue;
        float dot{};for(std::size_t k=0;k<3;++k)dot+=(point[k]-a[k])*(b[k]-a[k]);
        const auto fraction=std::clamp(dot/(length*length),0.0F,1.0F);
        std::array<float,3> projected{};for(std::size_t k=0;k<3;++k)projected[k]=a[k]+fraction*(b[k]-a[k]);
        const auto error=DistanceSquared(projected,point);
        // A link/vertical transition remains owned by the native action. Do
        // not interpret its entry/exit segment as ordinary walking progress.
        if(graph->nodes[i-1].type!=1&&graph->nodes[i].type!=1&&
            std::abs(projected[1]-point[1])<=0.75F&&error<=4.0F) {
            const auto progress=along+fraction*length;
            if(error<best.error)best={progress,error,true,i,projected,false};
            if(error<=0.5625F)closeProgress[closeCount++]=progress;
        }
        along+=length;
    }
    // Near a crossing/return, centimetres of body displacement can change the
    // nearest leg by metres of path time. Equality of projection errors is
    // insufficient: every leg within the 0.75m join tube is a possible one.
    if(best.valid&&best.error<=0.5625F)for(std::size_t i=0;i<closeCount;++i)
        if(std::abs(closeProgress[i]-best.along)>1.0F)best.ambiguous=true;
    return best;
}
// A native Move needs a geometric target beyond its current body. Use only
// the supplied Host corridor for that lookahead, never velocity extrapolation.
// The 2m horizon is a controller policy, not a discovered CATHODE constant or
// a claim that the physical body can be bounded by an instantaneous stop.
// Stop at the first bend/link: this function cannot select a different route.
[[nodiscard]] inline bool CorridorLookahead(const route::Graph* graph,const PathProgress& host,
    const std::array<float,3>& velocity,std::array<float,3>& target) noexcept {
    if(!graph||graph->failure!=route::Failure::None||graph->nodeCount>route::kMaxNodes||
        !host.valid||host.ambiguous||host.error>0.5625F||
        !host.segment||host.segment>=graph->nodeCount)return false;
    const auto speed=std::hypot(velocity[0],velocity[2]);
    if(speed<=0.2F)return false;
    auto from=host.position;float remaining=2.0F;bool advanced{};
    std::array<float,3> firstDirection{};
    for(auto i=host.segment;i<graph->nodeCount;++i) {
        if(graph->nodes[i-1U].type==1U||graph->nodes[i].type==1U)break;
        const auto point=NodePosition(graph->nodes[i]);
        if(!FinitePoint(point))return false;
        const auto length=std::sqrt(DistanceSquared(from,point));
        if(length>0.001F) {
            // A stale/crossing corridor must not turn a live Host observation
            // into a target in the opposite direction of its current leg.
            const auto horizontal=std::hypot(point[0]-from[0],point[2]-from[2]);
            if(!advanced&&horizontal>0.01F&&
                ((point[0]-from[0])*velocity[0]+(point[2]-from[2])*velocity[2])<0.25F*horizontal*speed)
                return false;
            if(!advanced)for(std::size_t k=0;k<3;++k)firstDirection[k]=(point[k]-from[k])/length;
            const auto fraction=std::min(1.0F,remaining/length);
            for(std::size_t k=0;k<3;++k)target[k]=from[k]+fraction*(point[k]-from[k]);
            advanced=true;remaining-=length;
            if(remaining<=0.0F)return true;
        }
        if(graph->nodes[i].type==1U||i+1U==graph->nodeCount)break;
        const auto after=NodePosition(graph->nodes[i+1U]);
        if(!FinitePoint(after))return false;
        float dot{},aa{},bb{},accumulatedDot{};
        const auto before=NodePosition(graph->nodes[i-1U]);
        for(std::size_t k=0;k<3;++k) {
            const auto u=point[k]-before[k],v=after[k]-point[k];
            dot+=u*v;aa+=u*u;bb+=v*v;accumulatedDot+=firstDirection[k]*v;
        }
        // A projected Host already at the corner may follow the next leg;
        // otherwise keep the corner as the next native target.
        if(advanced&&aa>0.000001F&&bb>0.000001F&&dot/std::sqrt(aa*bb)<0.9F)break;
        // Several individually small turns still form a bend. Bound their
        // cumulative change instead of aiming through an entire curved leg.
        if(advanced&&bb>0.000001F&&accumulatedDot/std::sqrt(bb)<0.9F)break;
        from=point;
    }
    return advanced;
}
// Geometry and permission are distinct. Physical samples preserve the walked
// bends and detect an advance; a short Host-authored corridor horizon keeps
// native navigation from repeatedly finishing at its own current position.
// Missing observations still hold; a missing corridor never invents a path.
[[nodiscard]] inline GuidedGoal LimitGuidedProgress(const route::Graph* graph,
    const std::array<float,3>& local,const HostProgress* host,GuidedGoal selected,
    PacingCursor& pacing,float& lead) noexcept {
    lead=0.0F;
    const auto hold=[&]() noexcept {
        if(!pacing.holding)pacing.anchor=local;
        pacing.holding=true;
        return GuidedGoal{pacing.anchor,false,true,true};
    };
    if(!FinitePoint(local))return selected;
    if(!host||!FinitePoint(host->position)||!FinitePoint(host->velocity))return hold();
    const auto speed=std::hypot(host->velocity[0],host->velocity[2]);
    if(speed>0.2F) {
        pacing.direction={host->velocity[0]/speed,0.0F,host->velocity[2]/speed};pacing.directionKnown=true;
    }
    const auto h=ProjectProgress(graph,host->position),c=ProjectProgress(graph,local);
    bool comparable=h.valid&&c.valid&&!h.ambiguous&&!c.ambiguous;
    if(host->observed.valid) {
        if(!FinitePoint(host->observed.position)||!std::isfinite(host->observed.lead))return hold();
    }
    if(host->observed.valid&&host->observed.comparable) {
        lead=host->observed.lead;comparable=true;
    } else if(comparable)lead=c.along-h.along;
    else if((!host->observed.valid||host->observed.atTip)&&pacing.directionKnown&&
        std::abs(local[1]-host->position[1])<=1.0F) {
        const auto dx=local[0]-host->position[0],dz=local[2]-host->position[2];
        lead=dx*pacing.direction[0]+dz*pacing.direction[2];
        const auto lateral=dx*pacing.direction[2]-dz*pacing.direction[0];
        comparable=std::abs(lateral)<=2.0F;
    }
    if(comparable&&lead>(pacing.holding?0.15F:0.6F))return hold();
    pacing.holding=false;
    // Never jump an unconsumed physical bend to reach the Host's future leg.
    // Close to the tip, local and Host projections must both identify the
    // same unambiguous corridor; a different floor/crossing cannot grant it.
    std::array<float,3> lookahead{};
    if((!host->observed.valid||host->observed.atTip)&&h.valid&&c.valid&&
        !h.ambiguous&&!c.ambiguous&&c.error<=0.5625F&&DistanceSquared(local,host->position)<=4.0F&&
        CorridorLookahead(graph,h,host->velocity,lookahead)&&DistanceSquared(local,lookahead)>0.1F) {
        const auto target=ProjectProgress(graph,lookahead);
        if(target.valid&&!target.ambiguous&&target.along>c.along+0.05F)
            return {lookahead,true,true,false,true};
    }
    // Losing the corridor after a short advance must not send the body back
    // to the previous Host sample. A native hold waits for physical catch-up;
    // this fallback never consumes an earlier bend or extrapolates a path.
    if((!host->observed.valid||host->observed.atTip)&&comparable&&lead>0.15F)return hold();
    if(host->observed.valid)return {host->observed.position,false,true,false};
    const auto target=ProjectProgress(graph,selected.position);
    if(h.valid&&target.valid&&target.along<=h.along+0.1F)return selected;
    // A missing graph/revision cannot reopen the distant final destination.
    // This remains a native navigation goal, not a Character position write.
    if(DistanceSquared(selected.position,host->position)<=0.0025F)return selected;
    return {host->position,false,true,false};
}
// Keep the target through endpoint edits instead of rewinding to the nearest
// node every frame. Re-routes without that element rebase from the live body.
// Do not skip bends or special links just because the final goal is nearby.
[[nodiscard]] inline GuidedGoal SelectGuidedGoal(const route::Graph* graph,
    const std::array<float,3>& local,const std::array<float,3>& destination,
    GuidanceCursor& cursor,float reach=1.25F) noexcept {
    if(!FinitePoint(local)||!graph||graph->failure!=route::Failure::None||!graph->nodeCount||graph->nodeCount>route::kMaxNodes)
        return {destination,false};
    if(!cursor.initialized||cursor.revision!=graph->key.parameterRevision||cursor.routeRevision!=graph->key.routeRevision) {
        std::size_t selected=graph->nodeCount;float best=std::numeric_limits<float>::max();
        // Preserve the previous target on the SAME resource/element when a
        // parameter edit keeps the corridor, including repeated elements.
        if(cursor.initialized)for(std::size_t i=0;i<graph->nodeCount;++i) {
            const auto& n=graph->nodes[i];
            if(n.type!=cursor.type||n.ordinal!=cursor.ordinal||n.resourceIndex>=graph->resourceCount||
                graph->resources[n.resourceIndex]!=cursor.resourceKey)continue;
            const auto d=DistanceSquared(NodePosition(n),cursor.target);
            if(d<best){selected=i;best=d;}
        }
        if(selected==graph->nodeCount) {
            best=std::numeric_limits<float>::max();
            for(std::size_t i=0;i<graph->nodeCount;++i) {
                const auto point=NodePosition(graph->nodes[i]);
                // A nearby XZ point on another floor is not a join.
                if(std::abs(point[1]-local[1])>2.0F)continue;
                const auto d=DistanceSquared(point,local);
                if(d<best){best=d;selected=i;}
            }
        }
        if(selected==graph->nodeCount)return {destination,false};
        cursor.index=selected;cursor.revision=graph->key.parameterRevision;
        cursor.routeRevision=graph->key.routeRevision;cursor.initialized=true;
    }
    if(cursor.index>=graph->nodeCount)cursor.index=graph->nodeCount-1;
    while(cursor.index+1<graph->nodeCount) {
        const auto point=NodePosition(graph->nodes[cursor.index]);
        if(std::abs(point[1]-local[1])>0.75F||DistanceSquared(point,local)>reach*reach)break;
        ++cursor.index;
    }
    const auto& node=graph->nodes[cursor.index];
    cursor.ordinal=node.ordinal;cursor.resource=node.resourceIndex;cursor.type=node.type;
    if(node.resourceIndex<graph->resourceCount)cursor.resourceKey=graph->resources[node.resourceIndex];
    cursor.target=NodePosition(node);
    if(cursor.index+1==graph->nodeCount)return {destination,false};
    return {cursor.target,true};
}
} // namespace aimp::ai_sync::movement
