#include "aimp/AISyncHostProducer.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <fstream>
#include <sstream>
#include <algorithm>

using namespace aimp::ai_sync;
using namespace host_producer;
namespace {
void Check(bool value, int line) { if (!value) { std::cerr << "host producer invariant " << line << '\n'; std::abort(); } }
#define CHECK(value) Check((value),__LINE__)
execution::Control Binding() {
    execution::Control c{}; c.scope.session[0] = 7; c.scope.session[15] = 31;
    c.scope.authorityEpoch = 2; c.scope.worldEpoch = 3; c.scope.consumerEpoch = 1; c.value = 3; return c;
}
transactions::OwnerKey Owner(std::uint32_t a = 1) { return {a,2,3,4,transactions::Capability::Movement,5}; }
void Ready(Producer& source, const execution::Control& control) {
    CHECK(source.Configure(control,7)); HostPacket packet{}; CHECK(source.Peek(packet) && packet.control);
    execution::Control response{}; CHECK(execution::DecodeControl(std::span(packet.bytes).first(packet.size),response));
    CHECK(response.kind == execution::ControlKind::Ready && response.scope == control.scope && response.value == control.value);
    CHECK(source.Commit(packet.token)); CHECK(!source.Commit(packet.token));
}
void Drain(Producer& source, transactions::Ledger& ledger, const transactions::Scope& scope, std::uint64_t& sequence) {
    HostPacket packet{};
    while (source.Peek(packet)) {
        CHECK(!packet.control); execution::Batch batch{};
        CHECK(execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch));
        HostPacket same{}; CHECK(source.Peek(same) && same.token == packet.token && same.bytes == packet.bytes);
        CHECK(!source.Commit(packet.token + 1));
        for (std::size_t i = 0; i < batch.count; ++i) {
            CHECK(batch.records[i].globalSeq == ++sequence);
            CHECK(ledger.Apply(batch.records[i]) == transactions::Result::Ok);
        }
        CHECK(source.Commit(packet.token));
    }
}
void Lifecycle() {
    auto p = std::make_unique<Producer>(); const auto binding = Binding(); Ready(*p,binding);
    transactions::Ledger ledger; CHECK(ledger.SetScope(binding.scope) == transactions::Result::Ok);
    std::uint64_t seq{};
    Command turn{4,2,true,10,20,30,0,0.5F};
    CommandIdentity identity{};
    CHECK(!p->Identify(100,identity));
    CHECK(p->Admit(Owner(),100,turn) == SourceResult::Ok); Drain(*p,ledger,binding.scope,seq); CHECK(seq == 2);
    CHECK(p->Identify(100,identity)&&identity.scope==binding.scope&&identity.owner==Owner()&&
        identity.command==1&&identity.parameterRevision==1&&identity.nativeKind==4);
    turn.x = 11; CHECK(p->Update(100,turn) == SourceResult::Ok);
    CHECK(p->Update(100,turn) == SourceResult::Ok); // Identical payload is not a new native mutation.
    Drain(*p,ledger,binding.scope,seq); CHECK(seq == 3);
    CHECK(p->Identify(100,identity)&&identity.command==1&&identity.parameterRevision==2);
    CHECK(p->Complete(100) == SourceResult::Ok); CHECK(p->Complete(100) == SourceResult::Ok);
    const auto completed=identity;CHECK(!p->Identify(100,identity)&&identity==completed);
    CHECK(p->Release(100) == SourceResult::Ok); Drain(*p,ledger,binding.scope,seq); CHECK(seq == 5);
    // Same allocator address gets a new command ID, with continuing ownerSeq.
    CHECK(p->Admit(Owner(),100,Command{7,1}) == SourceResult::Ok);
    CHECK(p->Identify(100,identity)&&identity.command==2&&identity.parameterRevision==1&&identity.nativeKind==7);
    CHECK(p->Cancel(100) == SourceResult::Ok); CHECK(p->Cancel(100) == SourceResult::Ok);
    CHECK(!p->Identify(100,identity));
    CHECK(p->Release(100) == SourceResult::Ok); Drain(*p,ledger,binding.scope,seq); CHECK(seq == 9);
    CHECK(p->Admit(Owner(2),200,Command{7,2}) == SourceResult::Ok);
    CHECK(p->Complete(200) == SourceResult::Ok); CHECK(p->Release(200) == SourceResult::Ok);
    Drain(*p,ledger,binding.scope,seq); CHECK(seq == 13);
    CHECK(p->Current(binding.scope,7)); auto other = binding.scope; other.session[15] ^= 1;
    CHECK(!p->Current(other,7)); CHECK(!p->Current(binding.scope,8));
}
void ForcedPlacement() {
    auto p=std::make_unique<Producer>();auto binding=Binding();binding.value=execution::kProfileLifecycle;
    Ready(*p,binding);
    Command placement{};placement.kind=execution::kForcedRelocateKind;placement.admission=1;
    placement.point=true;placement.x=-33.439754F;placement.y=19.28F;placement.z=-35.993092F;placement.heading=1.25F;
    CHECK(p->NativeEvent(Owner(),placement)==SourceResult::Ok);
    HostPacket packet{};CHECK(p->Peek(packet));execution::Batch batch{};
    CHECK(execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch));
    CHECK(batch.count==2U&&batch.records[1].nativeKind==execution::kForcedRelocateKind);
    CHECK(batch.records[1].goalX==placement.x&&batch.records[1].goalZ==placement.z&&batch.records[1].aux==placement.heading);
    CHECK(execution::ProfileForKind(batch.records[1].nativeKind)==execution::kProfileLifecycle);
    auto invalid=batch.records[1];invalid.opcode=transactions::Op::Update;
    CHECK(!execution::ValidateExecutable(invalid));
    transactions::Ledger ledger;CHECK(ledger.SetScope(binding.scope)==transactions::Result::Ok);
    std::uint64_t sequence{};Drain(*p,ledger,binding.scope,sequence);CHECK(sequence==4U);
}
void SchedulerPlacement() {
    auto p=std::make_unique<Producer>();auto binding=Binding();binding.value=execution::kProfilePlacement;
    Ready(*p,binding);
    Command placement{};placement.kind=execution::kPlacementKind;placement.point=true;
    placement.x=1.25F;placement.y=-2.5F;placement.z=3.75F;placement.heading=-1.25F;
    placement.mode=1;placement.admission=2;placement.scalarRetention=2;
    CHECK(p->Admit(Owner(),100,placement)==SourceResult::Ok);
    CommandIdentity original{},current{};CHECK(p->Identify(100,original)&&original.nativeKind==execution::kPlacementKind);
    placement.x=4.5F;CHECK(p->Update(100,placement)==SourceResult::Ok&&p->Identify(100,current)&&
        original.command==current.command&&original.parameterRevision+1==current.parameterRevision);
    CHECK(p->Complete(100)==SourceResult::Ok&&p->Release(100)==SourceResult::Ok);
    HostPacket packet{};std::uint64_t sequence{};std::vector<transactions::Op> observed;
    while(p->Peek(packet)) {
        execution::Batch batch{};CHECK(!packet.control&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch));
        for(std::size_t i=0;i<batch.count;++i) {
            const auto& r=batch.records[i];CHECK(r.globalSeq==++sequence&&r.ownerSeq==sequence&&r.nativeKind==8&&r.admission==2);
            observed.push_back(r.opcode);
            if(r.opcode==transactions::Op::Begin||r.opcode==transactions::Op::Submit||r.opcode==transactions::Op::Update)
                CHECK(r.flags==transactions::HasGoal&&r.mode==1&&r.aux==-1.25F&&r.scalarRetention==2&&
                    r.goalX==(r.opcode==transactions::Op::Update?4.5F:1.25F));
        }
        CHECK(p->Commit(packet.token));
    }
    CHECK(observed==std::vector<transactions::Op>({transactions::Op::Begin,transactions::Op::Submit,
        transactions::Op::Update,transactions::Op::Complete,transactions::Op::Release}));
}
void NativeIntentOrdering() {
    auto p=std::make_unique<Producer>();auto binding=Binding();binding.value=execution::kProfileNativeIntent|execution::kProfileAction;
    Ready(*p,binding);
    Command call{};call.kind=native_intent::kKind;call.admission=1;call.intent.words[0]=13U;
    call.intent.words[1]=1U;call.intent.words[2]=std::bit_cast<std::uint32_t>(1.25F);
    CHECK(p->NativeEvent(Owner(),call)==SourceResult::Ok);
    // Repeating the same native call is a distinct operation, even when its
    // destination cycle/state and all parameter bits are identical.
    CHECK(p->NativeEvent(Owner(),call)==SourceResult::Ok);
    Command hold{};hold.kind=execution::kActionKind;hold.mode=0x57U;hold.admission=1;
    CHECK(p->Admit(Owner(),100U,hold)==SourceResult::Ok);
    HostPacket packet{};std::uint64_t sequence{},command{};std::size_t count{};
    while(p->Peek(packet)) {
        execution::Batch batch{};CHECK(!packet.control&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch));
        for(std::size_t i=0;i<batch.count;++i) {
            const auto& r=batch.records[i];CHECK(r.globalSeq==++sequence&&r.ownerSeq==sequence);
            if(sequence<=8U) {
                CHECK(r.nativeKind==native_intent::kKind&&r.commandId==(sequence-1U)/4U+1U);
                if(r.opcode==transactions::Op::Begin||r.opcode==transactions::Op::Submit)CHECK(r.intent==call.intent);
                else CHECK(native_intent::Empty(r.intent));
            } else {CHECK(r.nativeKind==execution::kActionKind&&r.commandId==3U);command=r.commandId;}
            ++count;
        }
        CHECK(p->Commit(packet.token));
    }
    CHECK(count==10U&&command==3U);
    // Unsupported fields cannot become a scalar fallback or silently lose the
    // payload; the producer reports loss of executable coverage.
    call.intent.words[35]=1U;CHECK(p->NativeEvent(Owner(),call)==SourceResult::CoverageLost);
    CHECK(!p->Bound());
}
void MovementBundle() {
    auto p=std::make_unique<Producer>();auto binding=Binding();binding.value=7;Ready(*p,binding);
    Command move{1};auto& b=move.movement;b.version=1;b.parts=2;b.navigation=movement::Navigation::LocalNative;
    b.before.retained=b.after.retained=1;b.before.style=b.after.style=2;
    b.before.goal=b.after.goal={10,20,30};b.before.query[5]=.25F;b.after=b.before;
    b.before.flags90=0x21;b.after.flags90=1;b.companionFlags=2;b.companionRetainedAfter=1;
    CHECK(p->Admit(Owner(),100,move)==SourceResult::Ok);
    HostPacket packet{};CHECK(p->Peek(packet));execution::Batch batch{};
    CHECK(execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch)&&batch.count==2);
    CHECK(movement::Same(batch.records[0].movement,b)&&movement::Same(batch.records[1].movement,b));
    CHECK(p->Commit(packet.token));
    move.movement.after.query[5]=.5F;CHECK(p->Update(100,move)==SourceResult::Ok);
    CHECK(p->Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch));
    CHECK(batch.records[0].parameterRevision==2&&movement::Same(batch.records[0].movement,move.movement));
    CHECK(p->Commit(packet.token));CHECK(p->Cancel(100)==SourceResult::Ok&&p->Release(100)==SourceResult::Ok);
    while(p->Peek(packet)) {
        CHECK(execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch));
        CHECK(batch.records[0].nativeKind==1&&movement::Empty(batch.records[0].movement));CHECK(p->Commit(packet.token));
    }
}
void RouteOrdering() {
    auto source=std::make_unique<Producer>();auto binding=Binding();binding.value=15;Ready(*source,binding);
    Command move{1};move.movement.version=1;move.movement.parts=1;move.movement.navigation=movement::Navigation::HostRoute;
    CHECK(source->Admit(Owner(),100,move)==SourceResult::Ok);
    route::Graph graph{};graph.key={binding.scope,Owner(),1,1,1,0};graph.resourceCount=graph.nodeCount=1;
    std::copy_n("TECH_COMMS",10,graph.resources[0].level.begin());graph.resources[0].immutableDigest.fill(5);
    graph.statuses.fill(0x40000000U);
    CHECK(source->AppendRoute(graph)==SourceResult::Ok);
    CHECK(source->Complete(100)==SourceResult::Ok&&source->Release(100)==SourceResult::Ok);
    HostPacket packet{};std::uint64_t token{};
    CHECK(source->Peek(packet)&&!packet.route&&!packet.control);token=packet.token;
    execution::Batch batch{};CHECK(execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch)&&batch.count==2&&
        batch.records[1].opcode==transactions::Op::Submit&&source->Commit(token));
    CHECK(source->Peek(packet)&&packet.route&&!packet.control&&packet.token>token);token=packet.token;
    route::Graph decoded{};CHECK(route::Decode(std::span(packet.bytes).first(packet.size),graph.key,decoded)==route::Result::Ok&&
        decoded==graph&&source->Commit(token));
    for(const auto op:{transactions::Op::Complete,transactions::Op::Release}) {
        CHECK(source->Peek(packet)&&!packet.route&&!packet.control&&packet.token>token);token=packet.token;
        CHECK(execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch)&&
            batch.records[0].opcode==op&&source->Commit(token));
    }
    CHECK(source->AppendRoute(graph)==SourceResult::Conflict&&source->Bound());
    CHECK(!source->Peek(packet));
    // Companion and primary have independent route revisions. Duplicate and
    // missing revision are not silently reinterpreted as another native query.
    source=std::make_unique<Producer>();Ready(*source,binding);
    move.movement.parts=2;CHECK(source->Admit(Owner(),100,move)==SourceResult::Ok);
    graph.key.part=1;CHECK(source->AppendRoute(graph)==SourceResult::Ok);
    graph.key.part=0;CHECK(source->AppendRoute(graph)==SourceResult::Ok);
    CHECK(source->AppendRoute(graph)==SourceResult::Conflict&&source->Bound());
}
void RecalculatedRoutes() {
    auto source=std::make_unique<Producer>();auto binding=Binding();binding.value=15;Ready(*source,binding);
    Command move{1};move.movement.version=1;move.movement.parts=2;move.movement.navigation=movement::Navigation::HostRoute;
    CHECK(source->Admit(Owner(),100,move)==SourceResult::Ok);
    HostPacket packet{};CHECK(source->Peek(packet)&&source->Commit(packet.token));
    CommandIdentity identity{};CHECK(source->Identify(100,identity));
    std::array<float,6> auxiliary{};CHECK(!source->PreviousRouteAuxiliary(identity,0,auxiliary));
    route::Graph graph{};graph.resourceCount=graph.nodeCount=1;graph.statuses.fill(0x40000000U);
    std::copy_n("TECH_COMMS",10,graph.resources[0].level.begin());graph.resources[0].immutableDigest.fill(5);
    // Same Move, unchanged parameters: every published native result receives
    // a fresh route revision. Reading a candidate never burns that revision.
    for(std::uint32_t revision=1;revision<=40;++revision) {
        CHECK(source->NextRouteKey(identity,0,graph.key)&&graph.key.routeRevision==revision);
        route::Key again{};CHECK(source->NextRouteKey(identity,0,again)&&again==graph.key);
        graph.nodes[0].vectors[0]=static_cast<float>(revision);
        CHECK(source->AppendRoute(graph)==SourceResult::Ok&&source->Bound());
        CHECK(source->PreviousRouteAuxiliary(identity,0,auxiliary)&&auxiliary==graph.auxiliary);
        CHECK(source->Peek(packet)&&packet.route);
        route::Graph decoded{};CHECK(route::Decode(std::span(packet.bytes).first(packet.size),graph.key,decoded)==route::Result::Ok&&decoded==graph);
        CHECK(source->Commit(packet.token));
    }
    CHECK(source->NextRouteKey(identity,1,graph.key)&&graph.key.routeRevision==1);
    CHECK(source->AppendRoute(graph)==SourceResult::Ok);
    CHECK(source->Peek(packet)&&source->Commit(packet.token));
    route::Key unchanged=graph.key;
    auto wrong=identity;++wrong.scope.consumerEpoch;
    CHECK(!source->NextRouteKey(wrong,0,graph.key)&&graph.key==unchanged);
    wrong=identity;++wrong.owner.birth;
    CHECK(!source->NextRouteKey(wrong,0,graph.key)&&graph.key==unchanged);
    CHECK(!source->NextRouteKey(identity,2,graph.key)&&graph.key==unchanged);
    // An actual parameter update starts a separate route revision series;
    // the old snapshot cannot allocate keys for the new parameter revision.
    move.movement.after.goal[0]=1;
    CHECK(source->Update(100,move)==SourceResult::Ok);
    CHECK(!source->NextRouteKey(identity,0,graph.key)&&graph.key==unchanged);
    CHECK(source->Identify(100,identity)&&identity.parameterRevision==2);
    CHECK(source->PreviousRouteAuxiliary(identity,0,auxiliary)&&auxiliary==graph.auxiliary);
    CHECK(source->NextRouteKey(identity,0,graph.key)&&graph.key.parameterRevision==2&&graph.key.routeRevision==1);
    CHECK(source->AppendRoute(graph)==SourceResult::Ok);
    CHECK(source->Cancel(100)==SourceResult::Ok);
    CHECK(!source->PreviousRouteAuxiliary(identity,0,auxiliary));
    CHECK(!source->NextRouteKey(identity,0,graph.key));
    CHECK(source->Release(100)==SourceResult::Ok);
    CHECK(source->Admit(Owner(),100,move)==SourceResult::Ok);
    CHECK(!source->NextRouteKey(identity,0,graph.key));
    CHECK(source->Identify(100,identity)&&identity.command==2);
    CHECK(source->NextRouteKey(identity,0,graph.key)&&graph.key.routeRevision==1);
}
void RouteFailureIsolation() {
    auto source=std::make_unique<Producer>();auto binding=Binding();binding.value=15;Ready(*source,binding);
    Command move{1};move.movement.version=1;move.movement.parts=2;move.movement.navigation=movement::Navigation::HostRoute;
    auto other=Owner();++other.entityA;
    CHECK(source->Admit(Owner(),100,move)==SourceResult::Ok&&source->Admit(other,200,move)==SourceResult::Ok);
    CommandIdentity failed{},healthy{};CHECK(source->Identify(100,failed)&&source->Identify(200,healthy));
    CHECK(source->BlockRoute(failed,1,route::Failure::Unsupported)==SourceResult::Ok&&source->Bound());
    route::Key key{};CHECK(!source->NextRouteKey(failed,0,key)&&!source->NextRouteKey(failed,1,key));
    CHECK(source->BlockRoute(failed,0,route::Failure::Unavailable)==SourceResult::Conflict&&source->Bound());
    CHECK(source->NextRouteKey(healthy,0,key));
    route::Graph graph{};graph.key=key;graph.resourceCount=graph.nodeCount=1;graph.statuses.fill(0x40000000);
    std::copy_n("TECH_COMMS",10,graph.resources[0].level.begin());graph.resources[0].immutableDigest.fill(5);
    CHECK(source->AppendRoute(graph)==SourceResult::Ok&&source->Bound());
    CHECK(source->Complete(100)==SourceResult::Ok&&source->Release(100)==SourceResult::Ok);
    CHECK(source->Admit(Owner(),100,move)==SourceResult::Ok); // New command, same native address.
    CommandIdentity replacement{};CHECK(source->Identify(100,replacement)&&replacement.command!=failed.command);
    CHECK(source->NextRouteKey(replacement,0,key));
    HostPacket packet{};unsigned index{},faults{},routes{};
    while(source->Peek(packet)) {
        CHECK(!packet.control);++index;
        if(packet.route) {
            route::Key routeKey{};route::Graph decoded{};const auto bytes=std::span(packet.bytes).first(packet.size);
            CHECK(route::DecodeKey(bytes,routeKey)==route::Result::Ok&&route::Decode(bytes,routeKey,decoded)==route::Result::Ok);
            if(decoded.failure!=route::Failure::None) {CHECK(index==3&&decoded.key.commandId==failed.command&&decoded.failure==route::Failure::Unsupported);++faults;}
            else {CHECK(index==4&&decoded.key.commandId==healthy.command);++routes;}
        }
        CHECK(source->Commit(packet.token));
    }
    CHECK(faults==1&&routes==1&&source->Bound());
}
void Refusals() {
    auto p = std::make_unique<Producer>(); auto binding = Binding();
    auto wrong = binding; wrong.scope.consumerEpoch = 2; CHECK(!p->Configure(wrong,7));
    Ready(*p,binding);
    CHECK(p->Admit(Owner(),100,Command{1}) == SourceResult::Invalid); CHECK(!p->Bound());
    HostPacket packet{}; CHECK(p->Peek(packet) && packet.control);
    execution::Control recovery{}; CHECK(execution::DecodeControl(std::span(packet.bytes).first(packet.size),recovery));
    CHECK(recovery.kind == execution::ControlKind::Recovery && recovery.value == 4);
    CHECK(p->Commit(packet.token)); CHECK(!p->Configure(binding,7)); // Closed source cannot resurrect.
    ++binding.scope.worldEpoch; Ready(*p,binding);
    CHECK(p->Admit(Owner(),100,Command{7,0}) == SourceResult::Ok);
    CHECK(p->Release(100) == SourceResult::Conflict); CHECK(!p->Bound());
    p = std::make_unique<Producer>(); Ready(*p,binding);
    CHECK(p->Update(1234,Command{7,1}) == SourceResult::UnknownRequest); CHECK(!p->Bound());
    p = std::make_unique<Producer>(); Ready(*p,binding);
    CHECK(p->Admit(Owner(),100,Command{4,0,false,0,0,0,1,0.2F},99) == SourceResult::Invalid);
}
void Backpressure() {
    auto p = std::make_unique<Producer>(); Ready(*p,Binding());
    CHECK(p->Admit(Owner(),100,Command{7,0}) == SourceResult::Ok);
    for (std::size_t i = 1; i < Producer::kPackets - 1; ++i)
        CHECK(p->Update(100,Command{7,static_cast<std::uint32_t>(i % 3)}) == SourceResult::Ok);
    CHECK(p->Update(100,Command{7,0}) == SourceResult::CoverageLost); CHECK(!p->Bound());
    HostPacket packet{}; std::size_t count{}, controls{};
    while (p->Peek(packet)) { ++count; controls += packet.control ? 1U : 0U; CHECK(p->Commit(packet.token)); }
    CHECK(count == Producer::kPackets && controls == 1);
}
bool gRetireInQualifier{}, gReplaceInQualifier{};
execution::Control gReplacement{};
bool Qualifier(const transactions::Scope&, std::uint32_t) noexcept {
    if (gRetireInQualifier) RetireHost();
    if (gReplaceInQualifier) {
        gReplaceInQualifier = false;
        std::array<std::uint8_t,execution::kHeaderSize> bytes{};
        CHECK(execution::EncodeControl(gReplacement,bytes) && QueueBindingHost(bytes,7));
    }
    return true;
}
void BindingRevocation() {
    CHECK(Initialize(&Qualifier));
    auto control = Binding(); std::array<std::uint8_t,execution::kHeaderSize> bytes{};
    CHECK(execution::EncodeControl(control,bytes) && QueueBindingHost(bytes,7));
    gRetireInQualifier = true; PumpHost(); gRetireInQualifier = false;
    CHECK(!EngineProducer().Bound()); HostPacket packet{}; CHECK(!PeekHost(packet));
    CHECK(execution::EncodeControl(control,bytes) && QueueBindingHost(bytes,7));
    gReplacement = control; ++gReplacement.scope.authorityEpoch;
    gReplaceInQualifier = true; PumpHost(); CHECK(!EngineProducer().Bound());
    PumpHost(); CHECK(EngineProducer().Current(gReplacement.scope,7));
    CHECK(PeekHost(packet) && packet.control && CommitHost(packet.token));
    RetireHost(); PumpHost(); CHECK(!EngineProducer().Bound());
    CHECK(PeekHost(packet) && packet.control && CommitHost(packet.token));
}
void ActionReferences(const char* fixture) {
    auto p=std::make_unique<Producer>();auto binding=Binding();binding.value=19;Ready(*p,binding);
    Command action{5};action.mode=0x3B;action.actionFlags=1;
    CHECK(p->Admit(Owner(),100,action)==SourceResult::Ok);
    action_correlation::Reference reference{};
    CHECK(p->ActionReference(100,9,reference)&&reference.command==1&&reference.globalSequence==2&&reference.ownerSequence==2);
    CHECK(reference.parameterRevision==1&&reference.executionRevision==9&&reference.cycleState==0x3B);
    std::ifstream file(fixture);CHECK(file.good());std::string hex,part;while(file>>part)hex+=part;
    CHECK(hex.size()==sizeof(reference)*2);
    const auto* raw=reinterpret_cast<const std::uint8_t*>(&reference);
    for(std::size_t i=0;i<sizeof(reference);++i)CHECK(raw[i]==static_cast<std::uint8_t>(std::stoul(hex.substr(i*2,2),nullptr,16)));
    const auto frozen=reference;HostPacket packet{};CHECK(p->Peek(packet));execution::Batch batch{};
    CHECK(execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch));
    auto consumer=binding.scope;consumer.consumerEpoch=42;auto operation=batch.records[1];operation.scope=consumer;
    CHECK(action_correlation::Matches(reference,Owner(),operation,consumer));
    auto wrong=operation;++wrong.owner.birth;CHECK(!action_correlation::Matches(reference,Owner(),wrong,consumer));
    wrong=operation;++wrong.owner.ownerEpoch;CHECK(!action_correlation::Matches(reference,Owner(),wrong,consumer));
    wrong=operation;++wrong.commandId;CHECK(!action_correlation::Matches(reference,Owner(),wrong,consumer));
    wrong=operation;++wrong.ownerSeq;CHECK(!action_correlation::Matches(reference,Owner(),wrong,consumer));
    wrong=operation;++wrong.globalSeq;CHECK(!action_correlation::Matches(reference,Owner(),wrong,consumer));
    wrong=operation;++wrong.parameterRevision;CHECK(!action_correlation::Matches(reference,Owner(),wrong,consumer));
    wrong=operation;wrong.mode=0x42;CHECK(!action_correlation::Matches(reference,Owner(),wrong,consumer));
    wrong=operation;wrong.opcode=transactions::Op::Complete;CHECK(!action_correlation::Matches(reference,Owner(),wrong,consumer));
    auto collision=consumer;collision.session[15]^=1;wrong=operation;wrong.scope=collision;
    CHECK(!action_correlation::Matches(reference,Owner(),wrong,collision));
    // Intervening commands cannot replace this command's actual owner/global sequence.
    CHECK(p->Admit(Owner(),200,Command{7})==SourceResult::Ok&&p->ActionReference(100,9,reference)&&reference==frozen);
    action.actionFlags=3;CHECK(p->Update(100,action)==SourceResult::Ok&&p->ActionReference(100,9,reference));
    CHECK(reference.command==frozen.command&&reference.parameterRevision==2&&reference.globalSequence==5&&reference.ownerSequence==5);
    CHECK(p->Complete(100)==SourceResult::Ok&&!p->ActionReference(100,9,reference));
    CHECK(p->Release(100)==SourceResult::Ok&&p->Admit(Owner(),100,action)==SourceResult::Ok&&p->ActionReference(100,10,reference));
    CHECK(reference.command!=frozen.command&&reference.executionRevision==10);
    const auto unchanged=reference;CHECK(!p->ActionReference(100,0,reference)&&reference==unchanged);
    CHECK(!p->ActionReference(200,9,reference)&&reference==unchanged);
}
void CheckpointHistory() {
    auto p=std::make_unique<Producer>();const auto binding=Binding();Ready(*p,binding);
    execution::Control checkpoint{binding.scope,execution::ControlKind::Checkpoint,0,1};
    CHECK(!p->Checkpoint(checkpoint,7));
    std::uint64_t sequence{},lastCommand{};
    for(std::uint64_t i=0;i<10000;++i) {
        CHECK(p->Admit(Owner(),100,Command{7,0})==SourceResult::Ok);
        CHECK(p->Complete(100)==SourceResult::Ok&&p->Release(100)==SourceResult::Ok);
        HostPacket packet{};
        while(p->Peek(packet)) {
            execution::Batch batch{};
            CHECK(!packet.control&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),binding.scope,batch));
            for(std::size_t j=0;j<batch.count;++j) {
                CHECK(batch.records[j].globalSeq==++sequence&&batch.records[j].ownerSeq==sequence);
                CHECK(batch.records[j].commandId==i+1);lastCommand=batch.records[j].commandId;
            }
            CHECK(p->Commit(packet.token));
        }
        checkpoint.through=sequence;
        CHECK(!p->Checkpoint(checkpoint,8));
        CHECK(p->Checkpoint(checkpoint,7));
        execution::Control ack{};
        CHECK(p->Peek(packet)&&packet.control&&execution::DecodeControl(std::span(packet.bytes).first(packet.size),ack));
        CHECK(ack.kind==execution::ControlKind::CheckpointAck&&ack.through==sequence&&p->Commit(packet.token));
        CHECK(p->Bound());
    }
    CHECK(sequence==40000&&lastCommand==10000);
}
}
int main(int argc,char** argv) { CHECK(argc==2);ActionReferences(argv[1]); Lifecycle(); ForcedPlacement(); SchedulerPlacement(); NativeIntentOrdering(); MovementBundle(); RouteOrdering(); RecalculatedRoutes(); RouteFailureIsolation(); Refusals(); Backpressure(); BindingRevocation(); CheckpointHistory(); std::cout << "Host producer: lifecycle, movement bundle, route ordering/recalculations, full scope, allocator reuse, refusal, backpressure, binding revocation and 10000 checkpointed commands passed\n"; }
