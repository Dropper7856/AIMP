#include "aimp/AISyncExecutionMailbox.h"
#include "aimp/AISyncExecutionRuntime.h"
#include "aimp/AISyncCausalState.h"
#include "aimp/AISyncHybridMotion.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include <Windows.h>

namespace {
using namespace aimp::ai_sync;
using namespace execution;
using namespace transactions;
void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
Scope ScopeValue() {
    Scope scope{}; scope.session = {0x33,0x22,0x11,0,0x55,0x44,0x77,0x66,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    scope.authorityEpoch = 1; scope.worldEpoch = 2; scope.consumerEpoch = 3; return scope;
}
Batch BatchValue(std::size_t count = 1) {
    Batch batch{}; batch.scope = ScopeValue(); batch.batchId = 1; batch.count = count;
    for (std::size_t i = 0; i < count; ++i) {
        auto& r = batch.records[i]; r.scope = batch.scope; r.owner = {static_cast<std::uint32_t>(i + 1),0,1,1,Capability::Movement,1};
        r.commandId = i + 1; r.globalSeq = i + 1; r.ownerSeq = 1; r.parameterRevision = 1;
        r.nativeKind = 4; r.goalX = 12.5F; r.flags = HasGoal;
    }
    return batch;
}
struct Engine {
    ExecutionMailbox* mailbox{}; unsigned binds{}, calls{}, retries{}; bool retire{}, wrongReceipt{}, partial{};
    bool bindBusy{}, duplicateDuringBind{};
    bool wrongMovement{};
    unsigned checkpoints{};bool checkpointFail{};
    static Result Bind(void* context, const Scope& scope, std::uint32_t generation) noexcept {
        auto& e = *static_cast<Engine*>(context); ++e.binds;
        e.mailbox->Pump(e.Api()); // Mutation callback reentry cannot consume another work item.
        if (e.duplicateDuringBind && !e.mailbox->QueueBinding({scope,ControlKind::Offer,3},generation)) std::terminate();
        if (e.bindBusy) return Result::OwnerBusy;
        return generation == 7 ? Result::Ok : Result::WrongScope;
    }
    static bool Execute(void* context, std::span<const std::uint8_t> bytes, const Scope& scope,
        std::span<Receipt> output, std::size_t& count) noexcept {
        auto& e = *static_cast<Engine*>(context); ++e.calls; count = 0;
        if (!e.mailbox->Current(scope, 7) || e.mailbox->Current(scope, 8)) return false;
        Batch batch{}; if (!DecodeBatch(bytes, scope, batch)) return false;
        for (std::size_t i = 0; i < batch.count; ++i) {
            output[count++] = Receipt{scope,batch.batchId,batch.records[i],Stage::Received,Result::Ok};
            if (e.wrongReceipt && i == 1) ++output[i].operation.commandId;
            if (e.wrongMovement) output[i].operation.movement.after.query[5] += .25F;
        }
        if (e.retire) { e.mailbox->Retire(); if (e.mailbox->Current(scope, 7)) std::terminate(); }
        return !e.partial;
    }
    static bool Retry(void* context, const Record& record, std::uint64_t id, const Scope& scope, Receipt& receipt) noexcept {
        auto& e = *static_cast<Engine*>(context); ++e.retries;
        receipt = Receipt{scope,id,record,Stage::Admitted,Result::Ok}; return e.mailbox->Current(scope, 7);
    }
    static Result Checkpoint(void* context,const Scope& scope,std::uint64_t through) noexcept {
        auto& e=*static_cast<Engine*>(context);++e.checkpoints;
        e.mailbox->Pump(e.Api());
        return e.mailbox->Current(scope,7)&&through==128&&!e.checkpointFail ? Result::Ok : Result::OwnerBusy;
    }
    MailboxEngine Api() noexcept { return {this, &Bind, &Execute, &Retry, &Checkpoint}; }
};
void Bind(ExecutionMailbox& mailbox, Engine& engine) {
    Check(!mailbox.Bound(ScopeValue(),7), "unbound mailbox exposed an active grant");
    Check(mailbox.QueueBinding({ScopeValue(),ControlKind::Offer,3},7), "offer rejected");
    mailbox.Pump(engine.Api()); MailboxPacket ack{}; Control ready{};
    Check(mailbox.Peek(ack) && !ack.receipt && DecodeControl(std::span(ack.bytes).first(ack.size), ready) &&
        ready.kind == ControlKind::Ready && ready.scope == ScopeValue(), "binding acknowledged incorrectly");
    Check(!mailbox.Commit(ack.token + 1) && mailbox.Commit(ack.token), "peek/commit lost or mismatched head");
    Check(mailbox.Bound(ScopeValue(),7) && !mailbox.Bound(ScopeValue(),8), "binding lifetime qualification failed");
}
void Guards() {
    Check(!execution_runtime::Initialize(0,false,[](const Scope&,std::uint32_t) noexcept { return true; }) &&
        !execution_runtime::Available(), "unvalidated image activated runtime endpoint");
    auto mailbox = std::make_unique<ExecutionMailbox>(); Engine engine{mailbox.get()};
    auto batch = BatchValue(); std::vector<std::uint8_t> bytes; Check(EncodeBatch(batch, bytes), "encode failed");
    Check(!mailbox->QueueBatch(bytes), "unbound packet accepted"); Bind(*mailbox, engine);
    auto foreign = batch; foreign.scope.session.back() ^= 1; foreign.records[0].scope = foreign.scope;
    std::vector<std::uint8_t> bad; Check(EncodeBatch(foreign, bad) && !mailbox->QueueBatch(bad), "cookie collision accepted");
    Check(mailbox->QueueBatch(bytes), "bound batch rejected"); std::fill(bytes.begin(),bytes.end(),std::uint8_t{0});
    mailbox->Pump(engine.Api()); MailboxPacket packet{}; Receipt receipt{};
    Check(mailbox->Peek(packet) && packet.receipt && DecodeReceipt(std::span(packet.bytes).first(packet.size), batch.scope, receipt) &&
        receipt.operation.goalX == 12.5F, "borrowed input changed queued payload");
    const auto oldToken = packet.token; mailbox->Retire();
    Check(!mailbox->Bound(batch.scope,7), "retired binding still authorized ownership grants");
    Check(!mailbox->Commit(oldToken) && !mailbox->QueueBinding({batch.scope,ControlKind::Offer,3},7), "retired binding resurrected");
    auto fresh = batch.scope; ++fresh.consumerEpoch;
    Check(mailbox->QueueBinding({fresh,ControlKind::Offer,3},7), "fresh consumer unavailable");
    mailbox->Pump(engine.Api()); Check(engine.binds == 2 && engine.calls == 1, "callback reentry or duplicate bind");
}
void WorldRollbackAndDelayedWork() {
    auto box=std::make_unique<ExecutionMailbox>();Engine engine{box.get()};Bind(*box,engine);
    auto old=BatchValue();std::vector<std::uint8_t> bytes;
    Check(EncodeBatch(old,bytes)&&box->QueueBatch(bytes),"old world enqueue failed");
    auto next=ScopeValue();++next.worldEpoch;++next.consumerEpoch;
    Check(box->QueueBinding({next,ControlKind::Offer,3},7),"next world offer failed");
    box->Pump(engine.Api());
    Check(engine.calls==0&&box->Bound(next,7),"old queued work crossed world binding");
    auto rollback=next;--rollback.worldEpoch;++rollback.consumerEpoch;++rollback.authorityEpoch;
    Check(!box->QueueBinding({rollback,ControlKind::Offer,3},7)&&box->Bound(next,7),
        "old world offer invalidated current binding with larger consumer/authority epochs");
    Check(!box->QueueBatch(bytes)&&!box->QueueBinding({old.scope,ControlKind::Close,0},7)&&box->Bound(next,7),
        "delayed batch/close poisoned new world");
    box->Retire();
    Check(!box->QueueBinding({rollback,ControlKind::Offer,3},7),"retirement forgot world watermark");
    ++next.consumerEpoch;
    Check(box->QueueBinding({next,ControlKind::Offer,3},7),"safe fresh consumer could not reconnect");
    box->Pump(engine.Api());Check(box->Bound(next,7),"reconnection did not bind");
}
void PendingRetirementBinding() {
    auto mailbox = std::make_unique<ExecutionMailbox>(); Engine engine{mailbox.get()};
    engine.bindBusy = engine.duplicateDuringBind = true;
    Check(mailbox->QueueBinding({ScopeValue(),ControlKind::Offer,3},7), "pending offer rejected");
    MailboxPacket packet{};
    mailbox->Pump(engine.Api());
    Check(engine.binds == 1 && !mailbox->Peek(packet) && !mailbox->Bound(ScopeValue(),7), "busy bind claimed Ready or looped");
    Check(mailbox->QueueBinding({ScopeValue(),ControlKind::Offer,3},7), "pending duplicate lost");
    mailbox->Pump(engine.Api()); Check(engine.binds == 2 && !mailbox->Peek(packet), "pending bind failed or duplicated");
    engine.bindBusy = false; mailbox->Pump(engine.Api()); Control ready{};
    Check(engine.binds == 3 && mailbox->Peek(packet) && DecodeControl(std::span(packet.bytes).first(packet.size),ready) &&
        ready.kind == ControlKind::Ready && mailbox->Bound(ScopeValue(),7), "drained bind did not become Ready");
    auto retired = std::make_unique<ExecutionMailbox>(); Engine old{retired.get()}; old.bindBusy = true;
    Check(retired->QueueBinding({ScopeValue(),ControlKind::Offer,3},7), "old pending offer rejected");
    retired->Pump(old.Api()); retired->Retire(); old.bindBusy = false; retired->Pump(old.Api());
    Check(old.binds == 1 && !retired->Peek(packet), "retired pending bind resurrected");
}
void FailureAndRetry() {
    auto mailbox = std::make_unique<ExecutionMailbox>(); Engine engine{mailbox.get()}; Bind(*mailbox, engine);
    auto batch = BatchValue(2); std::vector<std::uint8_t> bytes; Check(EncodeBatch(batch,bytes), "encode failed");
    engine.wrongReceipt = true; Check(mailbox->QueueBatch(bytes), "queue failed"); mailbox->Pump(engine.Api());
    MailboxPacket packet{}; Control recovery{};
    Check(mailbox->Peek(packet) && packet.receipt, "valid partial receipt discarded"); Check(mailbox->Commit(packet.token), "commit failed");
    Check(mailbox->Peek(packet) && !packet.receipt && DecodeControl(std::span(packet.bytes).first(packet.size),recovery) &&
        recovery.kind == ControlKind::Recovery, "invalid callback receipt did not fence");
    Check(!mailbox->QueueBatch(bytes), "fenced stream reentered");
    const auto failed=mailbox->LastFailure();
    Check(failed.count==1&&failed.site!=0&&failed.reason==static_cast<std::uint32_t>(Recovery::NativeUnavailable),
        "native callback failure lost diagnostic provenance");
    Check(failed.stage==3&&failed.sequence==batch.records[1].globalSeq&&failed.kind==batch.records[1].nativeKind,
        "receipt echo failure lost the exact operation");
    auto partialBox=std::make_unique<ExecutionMailbox>();Engine partialEngine{partialBox.get()};Bind(*partialBox,partialEngine);
    partialEngine.partial=true;Check(partialBox->QueueBatch(bytes),"partial callback batch refused");partialBox->Pump(partialEngine.Api());
    for(unsigned i=0;i<2;++i)Check(partialBox->Peek(packet)&&packet.receipt&&partialBox->Commit(packet.token),
        "callback failure discarded an already produced exact receipt");
    Check(partialBox->Peek(packet)&&!packet.receipt&&partialBox->LastFailure().stage==2,
        "callback failure lost its ordered terminal boundary");
    auto retryBox = std::make_unique<ExecutionMailbox>(); Engine retryEngine{retryBox.get()}; Bind(*retryBox,retryEngine);
    auto invalidRetry = std::make_unique<ExecutionMailbox>(); Engine invalidEngine{invalidRetry.get()}; Bind(*invalidRetry,invalidEngine);
    Check(!invalidRetry->QueueBatch(bytes,true), "multi-record retry accepted");
    Check(invalidRetry->LastFailure().reason==static_cast<std::uint32_t>(Recovery::InvalidStream)&&
        invalidRetry->LastFailure().site!=failed.site,"malformed retry conflated with native callback failure");
    batch = BatchValue(); batch.records[0].opcode = Op::Submit;
    Check(EncodeBatch(batch,bytes) && retryBox->QueueBatch(bytes,true), "exact retry queue failed");
    retryBox->Pump(retryEngine.Api()); Check(retryEngine.retries == 1 && retryEngine.calls == 0, "retry promoted to Execute");
    auto revokeBox = std::make_unique<ExecutionMailbox>(); Engine revokeEngine{revokeBox.get()}; Bind(*revokeBox,revokeEngine);
    revokeEngine.retire = true; Check(revokeBox->QueueBatch(bytes), "queue failed"); revokeBox->Pump(revokeEngine.Api());
    Check(!revokeBox->Peek(packet) && !revokeBox->QueueBatch(bytes), "callback retirement published stale results");
    auto fenced = std::make_unique<ExecutionMailbox>(); Engine fencedEngine{fenced.get()}; Bind(*fenced,fencedEngine);
    Check(fenced->QueueBatch(bytes), "pre-fence queue failed"); fenced->Pump(fencedEngine.Api());
    Check(fenced->Fence(Recovery::NativeUnavailable) && !fenced->Bound(batch.scope,7), "local fence retained grant permission");
    Check(fenced->Peek(packet) && packet.receipt && fenced->Commit(packet.token), "local fence discarded earlier receipt");
    Check(fenced->Peek(packet) && !packet.receipt && DecodeControl(std::span(packet.bytes).first(packet.size),recovery) &&
        recovery.kind == ControlKind::Recovery, "local fence recovery missing");
    Check(!fenced->Fence(Recovery::NativeUnavailable), "local fence appended duplicate recovery");
}
void Bounds() {
    auto scoped=std::make_unique<ExecutionMailbox>();Engine scopedEngine{scoped.get()};Bind(*scoped,scopedEngine);
    auto stale=ScopeValue();++stale.consumerEpoch;
    Check(scoped->Matches(ScopeValue())&&!scoped->Matches(stale),"wrong copied scope matched mailbox");
    Check(!scoped->Fence(stale,Recovery::NativeUnavailable)&&scoped->Matches(ScopeValue()),"old route fenced newer mailbox");
    Check(scoped->Fence(ScopeValue(),Recovery::NativeUnavailable)&&!scoped->Matches(ScopeValue()),"current route could not fence mailbox");
    auto mailbox = std::make_unique<ExecutionMailbox>(); Engine engine{mailbox.get()}; Bind(*mailbox, engine);
    auto batch = BatchValue(); std::vector<std::uint8_t> bytes; Check(EncodeBatch(batch,bytes), "encode failed");
    for (std::size_t i = 0; i < ExecutionMailbox::kIncoming; ++i) Check(mailbox->QueueBatch(bytes), "early incoming capacity");
    Check(!mailbox->QueueBatch(bytes), "incoming bound exceeded"); mailbox->Pump(engine.Api());
    Check(engine.calls == 0, "overflow silently executed retained work");
    auto output = std::make_unique<ExecutionMailbox>(); Engine outputEngine{output.get()}; Bind(*output,outputEngine);
    batch = BatchValue(kMaxRecords); Check(EncodeBatch(batch,bytes), "encode failed");
    for (int i = 0; i < 16; ++i) { Check(output->QueueBatch(bytes), "queue full unexpectedly"); output->Pump(outputEngine.Api()); }
    Check(outputEngine.calls == 15, "outbox saturation allowed native entry without receipt capacity");
    MailboxPacket packet{};
    for (std::size_t i = 0; i < kMaxRecords; ++i) Check(output->Peek(packet) && output->Commit(packet.token), "output drain failed");
    output->Pump(outputEngine.Api()); Check(outputEngine.calls == 16, "reserved outbox work was lost");
    auto corruptBox = std::make_unique<ExecutionMailbox>(); Engine corruptEngine{corruptBox.get()}; Bind(*corruptBox,corruptEngine);
    batch = BatchValue(); Check(EncodeBatch(batch,bytes), "encode failed"); bytes[kBatchHeaderSize + 120] = 9;
    Check(!corruptBox->QueueBatch(bytes), "uncertified kind entered mailbox");
    Control failure{};
    Check(corruptBox->Peek(packet) && DecodeControl(std::span(packet.bytes).first(packet.size),failure) &&
        failure.value == static_cast<std::uint32_t>(Recovery::InvalidStream), "current malformed body was silently dropped");
}
void MovementEcho() {
    for(const bool corrupt:{false,true}) {
        auto box=std::make_unique<ExecutionMailbox>();Engine engine{box.get()};
        Check(box->QueueBinding({ScopeValue(),ControlKind::Offer,4},7),"Move offer refused");box->Pump(engine.Api());
        MailboxPacket packet{};Check(box->Peek(packet)&&box->Commit(packet.token),"Move Ready missing");
        auto batch=BatchValue();auto& r=batch.records[0];r.nativeKind=1;r.flags=0;r.goalX=0;
        r.movement.version=r.movement.parts=1;r.movement.navigation=movement::Navigation::LocalNative;
        std::vector<std::uint8_t> bytes;Check(EncodeBatch(batch,bytes)&&box->QueueBatch(bytes),"Move queue failed");
        engine.wrongMovement=corrupt;box->Pump(engine.Api());Check(box->Peek(packet),"Move response missing");
        if(corrupt) {
            Control control{};Check(!packet.receipt&&DecodeControl(std::span(packet.bytes).first(packet.size),control)&&
                control.kind==ControlKind::Recovery&&!box->Bound(ScopeValue(),7),"modified Move echo escaped mailbox");
        } else {
            Receipt receipt{};Check(packet.receipt&&packet.size==kHeaderSize+kExtendedRecordSize+8U&&
                DecodeReceipt(std::span(packet.bytes).first(packet.size),ScopeValue(),receipt)&&
                movement::Same(r.movement,receipt.operation.movement),"Move receipt was truncated");
        }
    }
}
void ControlFixture(const char* path) {
    std::ifstream input(path); Check(static_cast<bool>(input), "control fixture missing");
    const std::string text{std::istreambuf_iterator<char>{input},{}}; std::vector<std::uint8_t> bytes; int high=-1;
    for (const auto c : text) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const int value = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1;
        Check(value >= 0, "invalid control fixture");
        if (high < 0) high=value; else { bytes.push_back(static_cast<std::uint8_t>(high*16+value)); high=-1; }
    }
    Control decoded{}; std::array<std::uint8_t,kHeaderSize> encoded{};
    Check(high == -1 && DecodeControl(bytes,decoded) && decoded.scope == ScopeValue() && decoded.value == 3 &&
        EncodeControl(decoded,encoded) && std::equal(bytes.begin(),bytes.end(),encoded.begin()), "shared control fixture mismatch");
    for (std::size_t i = 0; i < bytes.size(); ++i) Check(!DecodeControl(std::span(bytes).first(i),decoded), "control truncation accepted");
    bytes[56] = 1; Check(!DecodeControl(bytes,decoded), "nonzero control reserved accepted");
    Check(SessionTextMatches(ScopeValue().session,"00112233-4455-6677-8899-AABBCCDDEEFF"), "GUID byte order mismatch");
    Check(!SessionTextMatches(ScopeValue().session,"00112233-4455-6677-8899-aabbccddeefe"), "full GUID ignored");
}
void Checkpoint() {
    for(const bool fail:{false,true}) {
        auto box=std::make_unique<ExecutionMailbox>();Engine engine{box.get()};Bind(*box,engine);
        Control control{ScopeValue(),ControlKind::Checkpoint,0,128};
        std::array<std::uint8_t,kHeaderSize> bytes{};Control decoded{};
        Check(EncodeControl(control,bytes)&&DecodeControl(bytes,decoded)&&decoded.through==128,"checkpoint codec lost prefix");
        Check(!box->QueueBinding({ScopeValue(),ControlKind::CheckpointAck,0,128},7),"worker forged checkpoint ACK");
        Check(!box->QueueBinding(control,8),"checkpoint borrowed old native generation");
        Check(box->QueueBinding(control,7)&&engine.checkpoints==0,"checkpoint ran on worker");
        engine.checkpointFail=fail;box->Pump(engine.Api());
        MailboxPacket packet{};
        Check(engine.checkpoints==1&&box->Peek(packet)&&!packet.receipt&&DecodeControl(std::span(packet.bytes).first(packet.size),decoded),"checkpoint response absent");
        Check(decoded.kind==(fail?ControlKind::Recovery:ControlKind::CheckpointAck),"unsettled checkpoint acknowledged");
        Check(fail||decoded.through==128,"checkpoint ACK changed prefix");
    }
}
void RouteNegotiation() {
    for(const bool strict:{false,true}) {
        auto box=std::make_unique<ExecutionMailbox>();Engine engine{box.get()};
        Check(box->QueueBinding({ScopeValue(),ControlKind::Offer,strict?15U:7U},7),"route offer refused");
        box->Pump(engine.Api());MailboxPacket packet{};Check(box->Peek(packet)&&box->Commit(packet.token),"route Ready missing");
        auto batch=BatchValue();auto& r=batch.records[0];r.nativeKind=1;r.flags=0;r.goalX=0;
        r.movement.version=r.movement.parts=1;r.movement.navigation=movement::Navigation::HostRoute;
        std::vector<std::uint8_t> bytes;Check(EncodeBatch(batch,bytes),"route batch encode failed");
        Check(box->QueueBatch(bytes)==strict,"HostRoute silently used a local-navigation contract");
        box->Pump(engine.Api());Check(engine.calls==(strict?1U:0U),"unnegotiated strict command reached engine");
    }
}
void ConcurrentBindingReads() {
    auto box=std::make_unique<ExecutionMailbox>();Engine engine{box.get()};Bind(*box,engine);
    std::atomic<bool> start{};
    std::thread worker([&]{while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        for(unsigned i=0;i<50000;++i)Check(box->Matches(ScopeValue()),"current binding changed during read-only contention");});
    start.store(true,std::memory_order_release);
    bool allBound=true;
    for(unsigned i=0;i<50000;++i)if(!box->Bound(ScopeValue(),7))allBound=false;
    worker.join();
    Check(allBound,"IPC read contention revoked a live engine binding");
    box->Retire();Check(!box->Bound(ScopeValue(),7),"retirement retained the binding");
}
void CarrierArrivalLease() {
    auto box=std::make_unique<ExecutionMailbox>();Engine engine{box.get()};Bind(*box,engine);
    struct Arrival {std::uint64_t tick{};unsigned calls{};} arrival;
    auto api=engine.Api();api.context=&arrival;
    api.carrier=[](void* context,std::span<const std::uint8_t>,const Scope&,std::uint64_t tick) noexcept {
        auto& observed=*static_cast<Arrival*>(context);observed.tick=tick;++observed.calls;return true;
    };
    State state{};state.sessionCookie=0x6677445500112233ULL;state.worldGeneration=2;
    std::array<std::uint8_t,causal_state::kBytes> bytes{};
    Check(causal_state::Encode(bytes,ScopeValue(),1,0,state),"carrier fixture encode");
    const auto before=GetTickCount64();Check(box->QueueCarrier(bytes),"carrier queue");
    const auto after=GetTickCount64();Sleep(20);box->Pump(api);
    Check(arrival.calls==1&&arrival.tick>=before&&arrival.tick<=after&&arrival.tick<GetTickCount64(),
        "carrier rebased immutable intake time at engine apply");
    hybrid_motion::Timeline timeline;
    hybrid_motion::Sample sample{1000,{0,0,0},{1,0,0},0,hybrid_motion::Valid};
    Check(timeline.Push(sample,arrival.tick),"delayed carrier timeline");
    hybrid_motion::Target target{};
    Check(timeline.SampleAt(arrival.tick+600,target)&&target.stale&&target.velocity[0]==0,
        "delayed first carrier renewed motion authority");
    sample.tick=1100;
    Check(timeline.Push(sample,arrival.tick+100)&&timeline.SampleAt(arrival.tick+700,target)&&target.stale,
        "delayed established carrier renewed motion authority");
}
}
int main(int argc, char** argv) {
    try { Check(argc == 2,"expected control fixture"); ControlFixture(argv[1]); Guards(); WorldRollbackAndDelayedWork(); PendingRetirementBinding(); FailureAndRetry(); Bounds(); MovementEcho(); Checkpoint(); RouteNegotiation(); ConcurrentBindingReads(); CarrierArrivalLease();
        std::cout << "PASS execution mailbox binding, retirement, receipts, retry and bounds (offline only)\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
