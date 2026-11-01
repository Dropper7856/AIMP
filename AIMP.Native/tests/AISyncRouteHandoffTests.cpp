#include "aimp/AISyncRouteContract.h"
#include <cstdlib>
#include <iostream>
#include <memory>
using namespace aimp::ai_sync::route;
namespace {
void Check(bool condition) {if(!condition){std::cerr<<"Compound Host tail contract failed\n";std::exit(1);}}
}
int main() {
    Check(!HybridResumeCurrent(8,4,7,100));
    Check(!HybridResumeCurrent(8,4,8,3));
    Check(HybridResumeCurrent(8,4,8,4));
    Check(HybridResumeCurrent(8,4,9,1));
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
