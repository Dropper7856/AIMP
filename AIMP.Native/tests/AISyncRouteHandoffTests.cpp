#include "aimp/AISyncRouteContract.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <limits>
using namespace aimp::ai_sync::route;
namespace {
void Check(bool condition) {if(!condition){std::cerr<<"Compound Host tail contract failed\n";std::exit(1);}}
void HandoffContinuity() {
    const std::array<float,3> origin{},host{3,0,0};
    NativeHandoffProbe delayed{};
    Check(delayed.Observe(1000,false,origin,host,true)==NativeHandoffResult::Pending);
    Check(delayed.Observe(1800,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(delayed.Observe(1801,true,{0.13F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(delayed.Observe(1881,true,{0.30F,0,0},host,true)==NativeHandoffResult::Confirmed);
    NativeHandoffProbe interrupted{};
    Check(interrupted.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(interrupted.Observe(1080,false,{0.5F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(interrupted.Observe(1100,true,{0.5F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(interrupted.Observe(1180,true,{0.5F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(interrupted.Observe(1220,true,{0.66F,0,0},host,true)==NativeHandoffResult::Confirmed);
    NativeHandoffProbe teleport{};
    Check(teleport.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(teleport.Observe(1100,true,{1.5F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(teleport.Observe(1180,true,{1.5F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(teleport.Observe(1220,true,{1.7F,0,0},host,true)==NativeHandoffResult::Confirmed);
    NativeHandoffProbe gap{};
    Check(gap.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(gap.Observe(1250,true,{0.3F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(gap.Observe(1330,true,{0.5F,0,0},host,true)==NativeHandoffResult::Confirmed);
    NativeHandoffProbe duplicateTime{};
    Check(duplicateTime.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(duplicateTime.Observe(1000,true,{0.13F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(duplicateTime.Observe(1080,true,{0.13F,0,0},host,true)==NativeHandoffResult::Pending);
    NativeHandoffProbe lifecycle{};
    Check(lifecycle.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(lifecycle.Observe(1100,true,{0.5F,0,0},host,true,true)==NativeHandoffResult::Pending);
    Check(lifecycle.Observe(1120,true,{0.5F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(lifecycle.Observe(1200,true,{0.7F,0,0},host,true)==NativeHandoffResult::Confirmed);
    NativeHandoffProbe nonfinite{};
    Check(nonfinite.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(nonfinite.Observe(1080,true,{std::numeric_limits<float>::quiet_NaN(),0,0},host,true)==NativeHandoffResult::Pending);
    Check(nonfinite.Observe(1100,true,{0.5F,0,0},host,true)==NativeHandoffResult::Pending);
    Check(nonfinite.Observe(1180,true,{0.5F,0,0},host,true)==NativeHandoffResult::Pending);
    NativeHandoffProbe rewind{};
    Check(rewind.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(rewind.Observe(1100,false,origin,host,true)==NativeHandoffResult::Pending);
    Check(rewind.Observe(1099,false,origin,host,true)==NativeHandoffResult::Stalled);
    NativeHandoffProbe deadline{};
    Check(deadline.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    for(std::uint64_t now=1100;now<1900;now+=100)
        Check(deadline.Observe(now,false,origin,host,true)==NativeHandoffResult::Pending);
    Check(deadline.Observe(1900,true,{0.5F,0,0},host,true)==NativeHandoffResult::Stalled);
}
}
int main() {
    HandoffContinuity();
    Check(!HybridResumeCurrent(8,4,7,100));
    Check(!HybridResumeCurrent(8,4,8,3));
    Check(HybridResumeCurrent(8,4,8,4));
    Check(HybridResumeCurrent(8,4,9,1));
    // Native installation alone cannot clear a hybrid recovery ticket.
    // Probe starts only after AISync relinquishes its own motor lease.
    NativeHandoffProbe moving{};
    const std::array<float,3> origin{0.0F,0.0F,0.0F}, host{3.0F,0.0F,0.0F};
    Check(moving.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(moving.Observe(1050,true,{0.04F,0.0F,0.0F},host,true)==NativeHandoffResult::Pending);
    Check(moving.Observe(1120,true,{0.18F,0.0F,0.0F},host,true)==NativeHandoffResult::Confirmed);
    NativeHandoffProbe blocked{};
    Check(blocked.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(blocked.Observe(1450,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(blocked.Observe(1900,true,origin,host,true)==NativeHandoffResult::Stalled);
    NativeHandoffProbe settled{};
    Check(settled.Observe(1000,true,origin,origin,false)==NativeHandoffResult::Pending);
    Check(settled.Observe(1100,true,origin,origin,false)==NativeHandoffResult::Confirmed);
    NativeHandoffProbe distantIdle{};
    Check(distantIdle.Observe(1000,true,origin,host,false)==NativeHandoffResult::Pending);
    Check(distantIdle.Observe(1900,true,origin,host,false)==NativeHandoffResult::Stalled);
    NativeHandoffProbe teleport{};
    Check(teleport.Observe(1000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(teleport.Observe(1100,true,{4.0F,0.0F,0.0F},host,true)==NativeHandoffResult::Pending);
    Check(teleport.Observe(1900,false,origin,host,true)==NativeHandoffResult::Stalled);
    NativeHandoffProbe missing{};
    // Samples taken while the hybrid motor still owns the Character are invalid
    // even if that motor moved the Alien by several metres.
    Check(missing.Observe(1000,false,origin,host,true)==NativeHandoffResult::Pending);
    Check(missing.Observe(1100,false,{0.9F,0.0F,0.0F},host,true)==NativeHandoffResult::Pending);
    Check(missing.Observe(1900,false,origin,host,true)==NativeHandoffResult::Stalled);
    NativeHandoffProbe delayed{};
    Check(delayed.Observe(1000,false,origin,host,true)==NativeHandoffResult::Pending);
    Check(delayed.Observe(1060,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(delayed.Observe(1160,true,{0.20F,0.0F,0.0F},host,true)==NativeHandoffResult::Confirmed);
    NativeHandoffProbe rewind{};
    Check(rewind.Observe(2000,true,origin,host,true)==NativeHandoffResult::Pending);
    Check(rewind.Observe(1999,true,origin,host,true)==NativeHandoffResult::Stalled);
        auto previous=std::make_unique<Graph>();auto next=std::make_unique<Graph>();
    previous->nodeCount=2;previous->resourceCount=2;
    previous->resources[1].state=7;previous->resources[1].immutableDigest[0]=123;
    auto& tail=previous->nodes[1];tail.resourceIndex=1;tail.type=2;tail.ordinal=15;
    tail.markers=11;tail.vectors={1,2,3,4,5,6,7,8,9};
    next->resourceCount=1;next->resources[0]=previous->resources[1];next->nodeCount=2;
    next->nodes[0]=tail;next->nodes[0].resourceIndex=0;
    Check(AppendSharesTail(*previous,*next));
    // Resource indices are process/graph local; actual resource identity is not.
    ++next->resources[0].state;Check(!AppendSharesTail(*previous,*next));--next->resources[0].state;
    ++next->nodes[0].ordinal;Check(!AppendSharesTail(*previous,*next));--next->nodes[0].ordinal;
    next->nodes[0].vectors[8]+=0.001F;Check(!AppendSharesTail(*previous,*next));next->nodes[0].vectors=tail.vectors;
    tail.vectors[0]=0.0F;next->nodes[0].vectors[0]=-0.0F;Check(!AppendSharesTail(*previous,*next));
    next->nodes[0].vectors=tail.vectors;
    next->nodes[0].markers=0x0B09;Check(!AppendSharesTail(*previous,*next));
    next->nodeCount=1;next->nodes[0].markers=0x0B0B;Check(AppendSharesTail(*previous,*next));
    next->nodes[0].markers=11;Check(!AppendSharesTail(*previous,*next));
    tail.markers=0x0B090807;next->nodes[0].markers=tail.markers;Check(!AppendSharesTail(*previous,*next));
    next->nodeCount=2;Check(AppendSharesTail(*previous,*next));
    next->nodes[0].resourceIndex=1;Check(!AppendSharesTail(*previous,*next));
    next->nodeCount=0;Check(!AppendSharesTail(*previous,*next));
    std::cout<<"Compound Host tail identity, geometry, ordered markers PASS; native execution UNPROVEN\n";
}
