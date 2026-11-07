#include "aimp/AISyncRouteContract.h"
#include "aimp/AISyncGuidedMovement.h"
#include <cstdlib>
#include <initializer_list>
#include <iostream>
using namespace aimp::ai_sync::route;
namespace {
void Check(bool condition,const char* what) {if(!condition){std::cerr<<"Host trail contract failed: "<<what<<"\n";std::exit(1);}}
// Ordinal == local area in these fixtures; 0 is unmappable.
std::uint32_t Area(const Graph& graph,std::size_t index) noexcept {return graph.nodes[index].ordinal;}
Graph Route(std::uint64_t command,std::uint32_t revision,std::initializer_list<std::uint32_t> areas,std::uint32_t firstType=0) {
    Graph graph{};
    graph.key.scope.session={0x33,0x22,0x11,0,0x55,0x44,0x77,0x66,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    graph.key.scope.authorityEpoch=2;graph.key.scope.worldEpoch=3;graph.key.scope.consumerEpoch=1;
    graph.key.owner={0x1234,0x5678,5,6,aimp::ai_sync::transactions::Capability::Movement,4};
    graph.key.commandId=command;graph.key.parameterRevision=revision;graph.key.routeRevision=1;
    graph.auxiliary={1,2,3,4,5,6};graph.statuses.fill(0x40000000);graph.goal={9,0,9};
    constexpr char level[]="CHALLENGEMAP16";
    std::copy(std::begin(level),std::end(level),graph.resources[0].level.begin());
    for(std::size_t i=0;i<32;++i)graph.resources[0].immutableDigest[i]=static_cast<std::uint8_t>(i+1);
    graph.resourceCount=1;
    for(const auto area:areas) {
        auto& node=graph.nodes[graph.nodeCount];
        node.type=graph.nodeCount?2U:firstType;node.ordinal=area;
        for(auto& value:node.vectors)value=static_cast<float>(area)+0.25F*static_cast<float>(graph.nodeCount);
        ++graph.nodeCount;
    }
    Check(Validate(graph)==Result::Ok,"fixture");
    return graph;
}
bool Areas(const Graph& graph,std::initializer_list<std::uint32_t> areas) {
    if(graph.nodeCount!=areas.size())return false;
    std::size_t i{};for(const auto area:areas)if(graph.nodes[i++].ordinal!=area)return false;
    return true;
}
void ChaseBehindHostStart() {
    // Host walks 10->11->12->13; then re-plans from 12 toward 15.
    HostTrail first{};ExtendHostTrail(nullptr,Route(10,1,{10,11,12,13}),first);
    Check(first.latestBegin==0&&Areas(first.graph,{10,11,12,13}),"first trail is the first Host route");
    const auto replanned=Route(10,2,{12,14,15});
    HostTrail second{};ExtendHostTrail(&first,replanned,second);
    // The Host's corridor node in area 12 is kept, the Host query start dropped.
    Check(Areas(second.graph,{10,11,12,14,15})&&second.latestBegin==2,"junction keeps the traversed corridor");
    Check(second.graph.key==replanned.key&&second.graph.goal==replanned.goal&&
        second.graph.auxiliary==replanned.auxiliary,"trail carries latest Host identity");
    Check(second.graph.nodes[2]==first.graph.nodes[2]&&second.graph.nodes[3]==replanned.nodes[1],"nodes are Host-authored copies");
    // Client actor still in area 11: bridge the Host trail, not a Client path.
    Graph bridged{};
    Check(BridgeHostTrail(second,replanned,11,Area,bridged)&&Areas(bridged,{11,12,14,15}),"bridge from lagging area");
    Check(bridged.key==replanned.key&&Validate(bridged)==Result::Ok,"bridge keeps exact Host key");
    // Already at the Host start or ahead: JoinCorridor/StartsHere own that case.
    Check(!BridgeHostTrail(second,replanned,12,Area,bridged),"start area is not bridged");
    Check(!BridgeHostTrail(second,replanned,14,Area,bridged),"ahead is not bridged");
    // Off the Host trail: refused, the hybrid recovery remains responsible.
    Check(!BridgeHostTrail(second,replanned,42,Area,bridged),"off-trail actor refused");
    Check(!BridgeHostTrail(second,replanned,0,Area,bridged),"unknown area refused");
}
void RepeatedReplansAccumulate() {
    HostTrail trail{};ExtendHostTrail(nullptr,Route(10,1,{1,2,3}),trail);
    for(std::uint32_t revision=2;revision<=6;++revision) {
        HostTrail next{};
        ExtendHostTrail(&trail,Route(10,revision,{revision,revision+1,revision+2}),next);
        trail=next;
    }
    Check(Areas(trail.graph,{1,2,3,4,5,6,7,8})&&trail.latestBegin==5,"successive Host re-plans accumulate");
    Graph bridged{};
    Check(BridgeHostTrail(trail,Route(10,6,{6,7,8}),2,Area,bridged)&&Areas(bridged,{2,3,4,5,6,7,8}),"bridge spans many re-plans");
    // A trail built for another revision cannot be presented as this graph.
    Check(!BridgeHostTrail(trail,Route(10,5,{5,6,7}),2,Area,bridged),"exact key required");
}
void NewCommandContinuesTrail() {
    HostTrail chase{};ExtendHostTrail(nullptr,Route(10,4,{20,21,22,23}),chase);
    HostTrail next{};ExtendHostTrail(&chase,Route(11,1,{22,30}),next);
    Check(Areas(next.graph,{20,21,22,30})&&next.graph.key.commandId==11,"same owner new command joins");
}
void RestartsWithoutExactJunction() {
    HostTrail trail{};ExtendHostTrail(nullptr,Route(10,1,{1,2,3}),trail);
    HostTrail next{};ExtendHostTrail(&trail,Route(10,2,{7,8}),next);
    Check(Areas(next.graph,{7,8})&&next.latestBegin==0,"teleport or new corridor restarts the trail");
    Graph bridged{};Check(!BridgeHostTrail(next,Route(10,2,{7,8}),2,Area,bridged),"no stale bridge after restart");
    // Another owner never inherits this actor's corridor.
    auto other=Route(10,2,{3,4});other.key.owner.entityA=0x9999;
    Check(Validate(other)==Result::Ok,"other owner fixture");
    ExtendHostTrail(&trail,other,next);
    Check(Areas(next.graph,{3,4})&&!next.latestBegin,"other owner restarts");
    // Another resource table is a different navigation mesh.
    auto mesh=Route(10,2,{3,4});mesh.resources[0].headerTuple={1,2,3};
    ExtendHostTrail(&trail,mesh,next);
    Check(Areas(next.graph,{3,4})&&!next.latestBegin,"other mesh restarts");
    // Junction at the very first trail node has no history to keep.
    ExtendHostTrail(&trail,Route(10,2,{1,9}),next);
    Check(Areas(next.graph,{1,9})&&!next.latestBegin,"junction at trail start");
}
void SpecialLinksAndCapacity() {
    // A type1 link can never become the first trail/bridge node.
    auto linked=Route(10,1,{1,2,3,4});linked.nodes[1].type=1;
    Check(Validate(linked)==Result::Ok,"link fixture");
    HostTrail trail{};ExtendHostTrail(nullptr,linked,trail);
    HostTrail next{};ExtendHostTrail(&trail,Route(10,2,{4,5}),next);
    Check(Areas(next.graph,{1,2,3,4,5}),"link retained inside trail");
    Graph bridged{};
    Check(!BridgeHostTrail(next,Route(10,2,{4,5}),2,Area,bridged),"link node is not an area");
    Check(BridgeHostTrail(next,Route(10,2,{4,5}),1,Area,bridged)&&Areas(bridged,{1,2,3,4,5}),"bridge across a Host link");
    // A latest route starting on a link cannot be spliced.
    auto startsOnLink=Route(10,3,{3,6});startsOnLink.nodes[0].type=1;
    ExtendHostTrail(&next,startsOnLink,trail);
    Check(Areas(trail.graph,{3,6})&&!trail.latestBegin,"link start restarts");
    // Capacity drops the oldest Host nodes, never the latest Host route.
    HostTrail longTrail{};
    {
        Graph route=Route(10,1,{1});route.nodeCount=0;
        for(std::uint32_t i=1;i<=kMaxNodes;++i){route.nodes[route.nodeCount].type=route.nodeCount?2U:0U;route.nodes[route.nodeCount++].ordinal=i;}
        Check(Validate(route)==Result::Ok,"long fixture");
        ExtendHostTrail(nullptr,route,longTrail);
    }
    const auto tail=Route(10,2,{kMaxNodes,kMaxNodes+1,kMaxNodes+2});
    ExtendHostTrail(&longTrail,tail,next);
    Check(next.graph.nodeCount==kMaxNodes&&next.graph.nodes[kMaxNodes-1].ordinal==kMaxNodes+2&&
        next.graph.nodes[next.latestBegin].ordinal==kMaxNodes&&next.graph.nodes[0].ordinal==3,"capacity trims oldest");
}
void EndpointEditKeepsBridgedRange() {
    // The chase fast path: the Host only moves its goal. A bridged install
    // must take that edit in place instead of a full native re-admission.
    HostTrail first{};ExtendHostTrail(nullptr,Route(10,1,{10,11,12,13}),first);
    const auto latest=Route(10,2,{12,14,15});
    HostTrail trail{};ExtendHostTrail(&first,latest,trail);
    Graph bridged{};Check(BridgeHostTrail(trail,latest,11,Area,bridged),"bridge fixture");
    auto moved=latest;moved.key.parameterRevision=3;moved.goal={20,0,21};
    for(std::size_t i=0;i<9;++i)moved.nodes[moved.nodeCount-1].vectors[i]=moved.goal[i%3];
    Graph applied{};
    Check(InstalledEndpointRevision(latest,bridged,moved,applied),"endpoint edit on bridged range");
    Check(Areas(applied,{11,12,14,15})&&applied.key==moved.key&&applied.goal==moved.goal&&
        applied.nodes[3]==moved.nodes[2]&&applied.nodes[0]==bridged.nodes[0],"only key, goal and terminal change");
    // Unjoined install: identical to the previous whole-graph behaviour.
    Check(InstalledEndpointRevision(latest,latest,moved,applied)&&applied==moved,"plain install unchanged");
    // A real re-route is not an endpoint edit.
    auto rerouted=moved;rerouted.nodes[1].ordinal=40;
    Check(!InstalledEndpointRevision(latest,bridged,rerouted,applied),"re-route needs admission");
    // The installed range must still end on the Host terminal.
    auto truncated=bridged;--truncated.nodeCount;
    Check(!InstalledEndpointRevision(latest,truncated,moved,applied),"terminal mismatch refused");
}
void NativeGuidanceKeepsHostBends() {
    using namespace aimp::ai_sync::movement;
    auto graph=Route(50,1,{1,2,3,4});
    const std::array<std::array<float,3>,4> points{{{0,0,0},{4,0,0},{4,0,4},{8,0,4}}};
    for(std::size_t i=0;i<points.size();++i)for(std::size_t k=0;k<9;++k)graph.nodes[i].vectors[k]=points[i][k%3];
    graph.goal=points.back();GuidanceCursor cursor{};
    auto goal=SelectGuidedGoal(&graph,points[0],graph.goal,cursor);
    Check(goal.intermediate&&goal.position==points[1],"first corner, not final destination");
    goal=SelectGuidedGoal(&graph,{3.0F,0,0},graph.goal,cursor);
    Check(goal.intermediate&&goal.position==points[2],"advance before stopping at first corner");
    // An endpoint edit keeps the current waypoint even if another branch is
    // now spatially closer. It must not rewind to the old route start.
    auto edit=graph;edit.key.parameterRevision=2;edit.goal={9,0,4};
    goal=SelectGuidedGoal(&edit,{3.1F,0,0},edit.goal,cursor);
    Check(goal.position==points[2]&&cursor.index==2,"endpoint edit preserves progress");
    goal=SelectGuidedGoal(&edit,points[2],edit.goal,cursor);
    Check(!goal.intermediate&&goal.position==edit.goal,"final goal and Host arrival policy restored");
    // Coincident XZ on another floor does not mean that waypoint was reached.
    cursor={};goal=SelectGuidedGoal(&graph,{4,5,0},graph.goal,cursor);
    Check(!cursor.initialized&&!goal.intermediate,"no false join from another floor");
    cursor={};goal=SelectGuidedGoal(&graph,{0,0,0},graph.goal,cursor);
    goal=SelectGuidedGoal(&graph,{4,2,0},graph.goal,cursor);
    Check(cursor.index==1,"vertical separation cannot consume a point");
    auto refused=graph;refused.failure=Failure::Unavailable;
    goal=SelectGuidedGoal(&refused,{0,0,0},graph.goal,cursor);
    Check(!goal.intermediate&&goal.position==graph.goal,"route refusal keeps authenticated destination");
    goal=SelectGuidedGoal(nullptr,{0,0,0},edit.goal,cursor);
    Check(!goal.intermediate&&goal.position==edit.goal,"missing guidance cannot deadlock native admission");
    // A large stride cannot skip the unvisited bend just because the end is
    // near. No predicted progress or transform is used to consume it.
    cursor={};static_cast<void>(SelectGuidedGoal(&graph,{0,0,0},graph.goal,cursor));
    goal=SelectGuidedGoal(&graph,{7,0,4},graph.goal,cursor);
    Check(cursor.index==1&&goal.position==points[1],"unvisited point never skipped by time or final proximity");
    // Special-link entry remains an ordered target; native CATHODE owns the
    // traversal and the synchronized action owns its animation.
    auto linked=graph;linked.nodes[1].type=1;cursor={};
    goal=SelectGuidedGoal(&linked,{0,0,0},linked.goal,cursor);
    Check(cursor.index==1&&goal.position==points[1],"special-link entry retained");
}
void ObservedProgressFencesFutureRoute() {
    using namespace aimp::ai_sync::movement;
    auto graph=Route(60,1,{1,2,3,4});
    const std::array<std::array<float,3>,4> points{{{0,0,0},{4,0,0},{4,0,4},{8,0,4}}};
    for(std::size_t i=0;i<points.size();++i)for(std::size_t k=0;k<9;++k)graph.nodes[i].vectors[k]=points[i][k%3];
    graph.goal=points.back();HostProgress host{{2,0,0},{1,0,0},1,false};
    PacingCursor pacing{};float lead{};
    auto goal=LimitGuidedProgress(&graph,{1,0,0},&host,{points[1],true},pacing,lead);
    Check(goal.paced&&!goal.holding&&goal.anticipating&&goal.position==points[1],"Host corridor provides a geometric target without claiming physical progress");
    goal=LimitGuidedProgress(&graph,{3,0,0},&host,{points[1],true},pacing,lead);
    Check(goal.holding&&goal.position==std::array<float,3>{3,0,0}&&lead>0.9F,"ahead waits at local anchor");
    host.position={2.6F,0,0};goal=LimitGuidedProgress(&graph,{3,0,0},&host,{points[1],true},pacing,lead);
    Check(goal.holding&&goal.position==std::array<float,3>{3,0,0},"hysteresis avoids stop/resume oscillation");
    host.position={3.1F,0,0};goal=LimitGuidedProgress(&graph,{3,0,0},&host,{points[1],true},pacing,lead);
    Check(!goal.holding&&goal.anticipating&&goal.position==points[1],"Host catching up releases the hold toward its known corner");
    host.position={4,0,3};goal=LimitGuidedProgress(&graph,{3,0,0},&host,{points[1],true},pacing,lead);
    Check(!goal.paced&&goal.position==points[1],"already traversed corner remains native waypoint");
    // An upcoming return near the same XZ must not be mistaken for advance
    // on the current leg. Host progress uses path length across the bend.
    host.position={4,0,1};goal=LimitGuidedProgress(&graph,{4,0,3},&host,{points[3],false},pacing,lead);
    Check(goal.holding&&lead>1.9F,"advance measured along ordered Host path");
    pacing={};host.position={2,5,0};
    goal=LimitGuidedProgress(&graph,{1,0,0},&host,{points[3],false},pacing,lead);
    Check(goal.paced&&!goal.holding&&goal.position==host.position,"other floor cannot certify future-route progress");
    pacing={};host.position={2,0,0};
    goal=LimitGuidedProgress(nullptr,{1,0,0},&host,{points[3],false},pacing,lead);
    Check(goal.position==host.position&&goal.paced,"missing exact graph does not reopen final goal");
    goal=LimitGuidedProgress(nullptr,{3,0,0},&host,{points[3],false},pacing,lead);
    Check(goal.holding&&goal.position[0]==3.0F,"no graph: measured Host direction detects advance");
    goal=LimitGuidedProgress(nullptr,{3.2F,0,0},nullptr,{points[3],false},pacing,lead);
    Check(goal.holding&&goal.position[0]==3.0F,"missing sample retains stable hold, no target drift");
    pacing={};host.position[0]=std::numeric_limits<float>::quiet_NaN();
    goal=LimitGuidedProgress(nullptr,{1,0,0},&host,{points[3],false},pacing,lead);
    Check(goal.holding&&goal.position[0]==1.0F,"invalid physical sample never grants advance");
    // Recorded 418 test, 18:45:49 UTC: Client was 3.4m ahead and reached
    // almost exactly the Host's position 1182ms later (8cm matching error).
    pacing={};host={{-26.031130F,19.253653F,-7.055638F},{-2,0,-2},1,false};
    const std::array<float,3> recorded{-28.292160F,19.255964F,-9.594171F};
    goal=LimitGuidedProgress(nullptr,recorded,&host,{{-40,19,-20},false},pacing,lead);
    Check(goal.holding&&goal.position==recorded&&lead>3.3F,"recorded lead brakes locally without reverse/teleport");
    ProgressClock clock{};
    Check(clock.Observe(100,1000)&&clock.Fresh(1600)&&!clock.Fresh(1601),"physical intake expiration");
    Check(!clock.Observe(100,1500)&&!clock.Fresh(1601),"old physical sample heartbeat cannot renew authority");
    Check(!clock.Observe(99,1610)&&clock.Observe(101,1620),"physical ticks are monotonic per scope");
    Check(!clock.Fresh(1619)&&clock.Fresh(1620),"local future arrival is not fresh");
    clock={};Check(!clock.Fresh(1700)&&clock.Observe(5,1701),"new scope needs its own physical observation");
}
void PhysicalTrailSurvivesIntentRevisions() {
    using namespace aimp::ai_sync::movement;
    ObservedTrail trail;
    // Host has rounded a corner while Client is still on the first leg. A
    // missing exact-revision graph must never send it diagonally to the tip.
    trail.Observe({0,0,0},1000,true);trail.Observe({4,0,0},1500,true);
    trail.Observe({4,0,4},2000,true);trail.Observe({0,0,4},2500,true);
    auto selected=trail.Goal({0,0,0});
    Check(selected.valid&&selected.position==std::array<float,3>{4,0,0}&&selected.lead<0,"physical trail retains first bend");
    HostProgress host{{0,0,4},{-1,0,0},2,false,selected};PacingCursor pacing{};float lead{};
    auto goal=LimitGuidedProgress(nullptr,{0,0,0},&host,{{40,0,40},false},pacing,lead);
    Check(goal.position==selected.position&&!goal.holding,"Host heading cannot falsely hold a Client on an earlier leg");
    selected=trail.Goal({4,0,0});
    Check(selected.position==std::array<float,3>{4,0,4},"next physical bend stays ordered across commands");
    selected=trail.Goal({4,0,4});
    Check(selected.position==host.position,"Host tip after traversing both bends");
    // A return through an old location must not rewind the shared actor cursor.
    trail.Observe({0,0,0},3000,true);
    selected=trail.Goal({0,0,4});
    Check(selected.position==std::array<float,3>{0,0,0},"return leg follows time order");
    selected=trail.Goal({0,0,-1});
    Check(selected.comparable&&selected.lead>0.9F,"advance past physical tip can be proven");
    // Protected native actions/spatial jumps do not create a walking bridge.
    trail.Observe({100,0,100},3100,false);Check(!trail.Goal({0,0,0}).valid,"capture/vent retires ordinary trail");
    trail.Observe({100,0,100},3200,true);Check(trail.Size()==1,"native action resumes without old intentions");
    trail.Observe({200,0,100},3250,true);Check(trail.Size()==1,"physical discontinuity clears breadcrumb bridge");
    trail.Observe({200.1F,0,100},4000,true);Check(trail.Size()==1,"long physical gap starts fresh");
    trail.Observe({201,0,100},3900,true);Check(trail.Size()==1,"old tick cannot append a past point");
    trail.Clear();
    for(unsigned i=0;i<1000;++i)trail.Observe({static_cast<float>(i)*0.25F,0,0},5000+i*50U,true);
    Check(trail.Size()==256,"physical history memory is bounded");
    selected=trail.Goal({248,0,0});
    Check(selected.valid&&selected.position[0]<=249.75F&&selected.position[0]>248,"lookahead never predicts past Host tip");
    trail.Clear();
    trail.Observe({0,0,0},1,true);trail.Observe({0.05F,0,0},51,true);trail.Observe({0.1F,0,0},101,true);
    Check(trail.Size()==1&&trail.Goal({0,0,0}).position[0]==0.1F,"subthreshold motion updates physical tip");
}
void PhysicalLookaheadDoesNotRewindSkippedSamples() {
    using namespace aimp::ai_sync::movement;
    ObservedTrail trail;
    for(unsigned i=0;i<=20;++i)trail.Observe({static_cast<float>(i)*0.25F,0,0},1000+i*50U,true);
    auto goal=trail.Goal({0,0,0});
    Check(goal.position[0]>=2.0F,"fixture issues a forward native goal");
    // Native locomotion can pass alongside small physical samples without
    // ever entering the .75m sphere around the current one.
    goal=trail.Goal({3.0F,0,0.7F});
    Check(goal.position[0]>3.0F,"projection consumes passed samples on the issued straight leg");
    // Only previously issued portions can prove progress. An unvisited bend
    // must not be consumed by proximity to a later return leg.
    trail.Clear();trail.Observe({0,0,0},1000,true);trail.Observe({4,0,0},1500,true);
    trail.Observe({4,0,4},2000,true);trail.Observe({0,0,4},2500,true);
    goal=trail.Goal({0,0,0});Check(goal.position==std::array<float,3>{4,0,0},"bend issued");
    goal=trail.Goal({0,0,4});Check(goal.position==std::array<float,3>{4,0,0},"later leg cannot skip issued bend");
    goal=trail.Goal({4,2,0});Check(goal.position==std::array<float,3>{4,0,0},"other floor cannot consume bend");
}
void SelectedPhysicalGoalDoesNotRewind() {
    using namespace aimp::ai_sync::movement;
    ObservedTrail trail;
    for(unsigned i=0;i<=40U;++i)trail.Observe({static_cast<float>(i)*0.25F,0,0},1000U+i*50U,true);
    auto goal=trail.Goal({0,0,0});const auto initial=goal.position;
    Check(initial[0]==2.0F,"fixture selects the two-metre lookahead");
    goal=trail.Goal({-1,0,0});
    Check(goal.position==initial,"body recoil cannot retract a selected physical target");
    goal=trail.Goal({0,0,2});
    Check(goal.position==initial,"lateral displacement cannot retract a selected physical target");
    goal=trail.Goal({2,0,0});
    Check(goal.position[0]>initial[0],"reaching the target still advances the consumed cursor");
    const auto advanced=goal.position;goal=trail.Goal({0,0,0});
    Check(goal.position==advanced,"selection remains ordered across repeated local replans");
    // A watermark is an observation index, not a monotonic world coordinate.
    trail.Clear();trail.Observe({0,0,0},1000,true);trail.Observe({4,0,0},1500,true);
    trail.Observe({4,0,4},2000,true);trail.Observe({0,0,4},2500,true);
    Check(trail.Goal({0,0,0}).position==std::array<float,3>{4,0,0},"first corner is owed");
    Check(trail.Goal({0,0,4}).position==std::array<float,3>{4,0,0},"watermark does not consume an unvisited corner");
    Check(trail.Goal({4,2,0}).position==std::array<float,3>{4,0,0},"different floor cannot consume selected corner");
    Check(trail.Goal({4,0,0}).position==std::array<float,3>{4,0,4},"reached corner advances to next leg");
    Check(trail.Goal({4,0,4}).position==std::array<float,3>{0,0,4},"real Host return may decrease world X");
}
void SelectedPhysicalGoalKeepsTipAndResetSemantics() {
    using namespace aimp::ai_sync::movement;
    ObservedTrail trail;
    trail.Observe({0,0,0},1000,true);trail.Observe({2,0,0},1500,true);
    Check(trail.Goal({0,0,0}).position==std::array<float,3>{2,0,0},"initial live tip selected");
    trail.Observe({2,0,1},1600,true);
    Check(trail.Goal({0,0,0}).position==std::array<float,3>{2,0,0},"old virtual tip must not reserve a future point past a new corner");
    Check(trail.Goal({2,0,0}).position==std::array<float,3>{2,0,1},"new leg opens after reaching the tip corner");
    trail.Clear();
    for(unsigned i=0;i<1000U;++i) {
        trail.Observe({static_cast<float>(i)*0.25F,0,0},1000U+i*50U,true);
        if(i==20U)(void)trail.Goal({0,0,0});
    }
    const auto bounded=trail.Goal({248,0,0});
    Check(trail.Size()==256U&&bounded.valid&&bounded.position[0]>248.0F&&bounded.position[0]<=249.75F,
        "eviction never dereferences an expired selected sample");
    trail.Clear();trail.Observe({-5,0,0},1,true);
    Check(trail.Goal({-5,0,0}).position==std::array<float,3>{-5,0,0},"reset discards previous selection epoch");
    trail.Observe({-5,0,0},51,false);
    Check(!trail.Goal({-5,0,0}).valid,"protected motion clears selected guidance");
}
void GeometricLookaheadDoesNotBecomePhysicalAuthority() {
    using namespace aimp::ai_sync::movement;
    auto graph=Route(70,1,{1,2,3,4});
    const std::array<std::array<float,3>,4> points{{{0,0,0},{4,0,0},{4,0,4},{8,0,4}}};
    for(std::size_t i=0;i<points.size();++i)
        for(std::size_t k=0;k<9;++k)graph.nodes[i].vectors[k]=points[i][k%3];
    graph.goal=points.back();
    ObservedTrail trail;trail.Observe(points[0],1000,true);
    HostProgress host{points[0],{1,0,0},1,false,trail.Goal(points[0])};
    PacingCursor pacing{};float lead{};
    auto goal=LimitGuidedProgress(&graph,points[0],&host,{points[1],true},pacing,lead);
    Check(host.observed.atTip&&goal.anticipating&&goal.position==std::array<float,3>{2,0,0},
        "synchronized moving startup has a real native target on the Host corridor");
    Check(lead==0.0F,"lookahead does not pretend that Host already traversed the corridor");
    // Body advance remains separately fenced even though the path is known.
    goal=LimitGuidedProgress(&graph,{1,0,0},&host,{points[1],true},pacing,lead);
    Check(goal.holding&&!goal.anticipating&&lead==1.0F,"single physical observation cannot authorize an ahead Client");
    // Missing or obsolete route information cannot be replaced by extrapolating
    // the velocity vector toward a fabricated point.
    pacing={};goal=LimitGuidedProgress(nullptr,points[0],&host,{points[3],false},pacing,lead);
    Check(!goal.anticipating&&goal.position==points[0],"no corridor keeps the observed target for the initial-wait policy");
    goal=LimitGuidedProgress(nullptr,{0.4F,0,0},&host,{points[3],false},pacing,lead);
    Check(goal.holding&&goal.position==std::array<float,3>{0.4F,0,0}&&lead>0.39F,
        "losing a corridor after anticipation holds instead of walking back to the first Host sample");
    pacing={};host.position={0.2F,0,0};host.observed={host.position,0.4F,true,true,true};
    goal=LimitGuidedProgress(nullptr,{0.6F,0,0},&host,{points[3],false},pacing,lead);
    Check(goal.holding&&goal.position==std::array<float,3>{0.6F,0,0},
        "a small proven advance without a corridor cannot reopen a backwards target");
    auto endpoint=graph;endpoint.nodeCount=2;
    pacing={};host.position={3.8F,0,0};host.observed={host.position,0.55F,true,true,true};
    goal=LimitGuidedProgress(&endpoint,{4.35F,0,0},&host,{points[1],false},pacing,lead);
    Check(goal.holding&&!goal.anticipating&&goal.position==std::array<float,3>{4.35F,0,0},
        "corridor endpoint behind the body is a hold, not a forward lookahead");
    host.position=points[0];host.observed={points[0],0,true,false,true};
    pacing={};host.velocity={};
    goal=LimitGuidedProgress(&graph,points[0],&host,{points[1],true},pacing,lead);
    Check(!goal.anticipating&&goal.position==points[0],"stationary Host grants no future physical travel");
    host.velocity={1,0,0};
    goal=LimitGuidedProgress(&graph,points[0],nullptr,{points[1],true},pacing,lead);
    Check(goal.holding&&!goal.anticipating,"stale carrier closes even a known corridor");
    // At the end of a leg, lookahead stops at the corner, not two metres
    // diagonally toward the following leg.
    pacing={};host.position={3.5F,0,0};host.observed={};
    goal=LimitGuidedProgress(&graph,host.position,&host,{points[1],true},pacing,lead);
    Check(goal.anticipating&&goal.position==points[1],"lookahead ends at first bend");
    host.position=points[1];host.velocity={0,0,1};
    goal=LimitGuidedProgress(&graph,host.position,&host,{points[2],true},pacing,lead);
    Check(goal.anticipating&&goal.position==std::array<float,3>{4,0,2},"a reached corner permits the next Host leg");
    // An earlier unconsumed physical bend wins over a geometrically tempting
    // future route, including when it happens to be very close to the body.
    host.position={4,0,1};host.observed={points[1],-1.5F,true,true,false};
    goal=LimitGuidedProgress(&graph,{3.5F,0,0},&host,{points[2],true},pacing,lead);
    Check(!goal.anticipating&&goal.position==points[1],"an owed physical corner cannot be skipped");
    auto linked=graph;linked.nodes[2].type=1;
    host.position=points[1];host.observed={};
    goal=LimitGuidedProgress(&linked,points[1],&host,{points[2],true},pacing,lead);
    Check(!goal.anticipating,"lookahead cannot cross a special-link node");
    // A retraced corridor has two possible path times at x=2. It cannot be
    // used to grant forward travel before physical order resolves the tie.
    auto crossing=graph;crossing.nodeCount=3;
    const std::array<std::array<float,3>,3> returnPoints{{{0,0,0},{4,0,0},{0,0,0}}};
    for(std::size_t i=0;i<returnPoints.size();++i)
        for(std::size_t k=0;k<9;++k)crossing.nodes[i].vectors[k]=returnPoints[i][k%3];
    Check(ProjectProgress(&crossing,{2,0,0}).ambiguous,"ambiguous path time is explicit at a return crossing");
    host.position={2,0,0};host.velocity={1,0,0};host.observed={};pacing={};
    goal=LimitGuidedProgress(&crossing,host.position,&host,{points[1],true},pacing,lead);
    Check(!goal.anticipating,"ambiguous projection cannot grant corridor anticipation");
    // Slightly unequal projection errors at a crossing are still ambiguous.
    // A four-centimetre separation must not become a ten-metre path advance.
    crossing.nodeCount=4;
    const std::array<std::array<float,3>,4> crossed{{{-2,0,-2},{2,0,2},{-2,0,2},{2,0,-2}}};
    for(std::size_t i=0;i<crossed.size();++i)
        for(std::size_t k=0;k<9;++k)crossing.nodes[i].vectors[k]=crossed[i][k%3];
    host.position={-0.02F,0,-0.02F};host.velocity={1,0,1};host.observed={host.position,0,true,false,true};pacing={};
    const std::array<float,3> nearCrossing{0.02F,0,-0.02F};
    Check(ProjectProgress(&crossing,host.position).ambiguous&&ProjectProgress(&crossing,nearCrossing).ambiguous,
        "the join tube detects different plausible path times near a crossing");
    goal=LimitGuidedProgress(&crossing,nearCrossing,&host,{crossed.back(),false},pacing,lead);
    Check(!goal.anticipating&&!goal.holding&&std::abs(lead)<0.05F,
        "nearby bodies at a crossing cannot trigger a false multi-metre hold");
    auto curved=graph;curved.nodeCount=7;
    std::array<float,3> point{};
    for(std::size_t i=0;i<curved.nodeCount;++i) {
        if(i) {
            const auto angle=static_cast<float>(i-1U)*0.34906585F;
            point[0]+=0.35F*std::cos(angle);point[2]+=0.35F*std::sin(angle);
        }
        for(std::size_t k=0;k<9;++k)curved.nodes[i].vectors[k]=point[k%3];
    }
    host.position={0,0,0};host.velocity={1,0,0};host.observed={host.position,0,true,false,true};pacing={};
    goal=LimitGuidedProgress(&curved,host.position,&host,{curved.goal,false},pacing,lead);
    Check(goal.anticipating&&DistanceSquared(goal.position,NodePosition(curved.nodes[2]))<0.000001F,
        "successive small turns stop at their cumulative bend");
}
void ParameterUpdatesKeepOnlyCompatibleGeometry() {
    using namespace aimp::ai_sync::movement;
    const auto graph=Route(35,4,{1,2,3});
    auto current=graph.key;++current.parameterRevision;
    Check(RetainGuidanceGeometry(graph,current,graph.goal),
        "parameter-only update keeps the last Host corridor as a hint");
    auto changedGoal=graph.goal;changedGoal[0]+=0.01F;
    Check(!RetainGuidanceGeometry(graph,current,changedGoal),"a changed authored goal requires its own route");
    auto changed=current;++changed.commandId;
    Check(!RetainGuidanceGeometry(graph,changed,graph.goal),"another command cannot reuse the hint");
    changed=current;++changed.owner.birth;
    Check(!RetainGuidanceGeometry(graph,changed,graph.goal),"another incarnation cannot reuse the hint");
    changed=current;++changed.scope.consumerEpoch;
    Check(!RetainGuidanceGeometry(graph,changed,graph.goal),"another consumer scope cannot reuse the hint");
    changed=current;++changed.part;
    Check(!RetainGuidanceGeometry(graph,changed,graph.goal),"compound parts cannot exchange hints");
    changed=current;changed.parameterRevision=graph.key.parameterRevision-1U;
    Check(!RetainGuidanceGeometry(graph,changed,graph.goal),"future route cannot guide an older intention");
    Check(!RetainGuidanceGeometry(graph,graph.key,graph.goal),"exact revisions use the exact cache path");
}
} // namespace
int main() {
    ChaseBehindHostStart();
    RepeatedReplansAccumulate();
    NewCommandContinuesTrail();
    RestartsWithoutExactJunction();
    SpecialLinksAndCapacity();
    EndpointEditKeepsBridgedRange();
    NativeGuidanceKeepsHostBends();
    ObservedProgressFencesFutureRoute();
    PhysicalTrailSurvivesIntentRevisions();
    PhysicalLookaheadDoesNotRewindSkippedSamples();
    SelectedPhysicalGoalDoesNotRewind();
    SelectedPhysicalGoalKeepsTipAndResetSemantics();
    GeometricLookaheadDoesNotBecomePhysicalAuthority();
    ParameterUpdatesKeepOnlyCompatibleGeometry();
    std::cout<<"AISync Host trail bridge tests passed\n";
    return 0;
}
