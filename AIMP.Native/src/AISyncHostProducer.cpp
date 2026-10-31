#include "aimp/AISyncHostProducer.h"
#include <algorithm>

namespace aimp::ai_sync::host_producer {
namespace {
std::atomic<CommandTrace> gCommandTrace{};
using namespace transactions;
bool Parameters(const Command& p, Record& r) noexcept {
    r.nativeKind = p.kind; r.mode = p.mode;
    r.flags = p.point ? HasGoal : 0U;
    r.goalX = p.x; r.goalY = p.y; r.goalZ = p.z;
    r.aux = p.heading; r.distance = p.tolerance;
    if (p.kind == execution::kDirectedKind) { r.goalX = 0.0F; r.stopDistance = p.x; }
    r.movement = p.movement;
    r.intent = p.intent;
    r.gait = p.kind == execution::kActionKind ? p.actionFlags : 0U;
    r.admission=p.admission;r.scalarRetention=p.scalarRetention;
    return execution::ValidateExecutable(r);
}
Producer gSource;
std::mutex gBindingGate;
execution::Control gPending{};
std::uint32_t gPendingGeneration{};
std::uint64_t gRevision{}, gApplied{};
QualifyHost gQualify{};
bool gPumping{}; // Qualified engine-thread reentry barrier.
}
bool Producer::ControlLocked(execution::ControlKind kind, std::uint32_t value, std::uint64_t through) noexcept {
    if (count_ == kPackets || token_ == UINT64_MAX ||
        (kind != execution::ControlKind::Recovery && count_ >= kPackets - 1)) return false;
    std::array<std::uint8_t,execution::kHeaderSize> bytes{};
    if (!execution::EncodeControl({binding_.scope,kind,value,through},bytes)) return false;
    auto& packet = packets_[(read_ + count_) % kPackets]; packet = {};
    packet.control = true; packet.size = bytes.size(); packet.token = ++token_;
    std::copy(bytes.begin(),bytes.end(),packet.bytes.begin()); ++count_; return true;
}
void Producer::FailLocked(execution::Recovery reason, std::uint32_t site) noexcept {
    if (!active_.exchange(false,std::memory_order_acq_rel)) return;
    failure_.site=site;failure_.reason=static_cast<std::uint32_t>(reason);failure_.kind=emittingKind_;++failure_.count;
    // One output slot is always reserved for this terminal boundary.
    static_cast<void>(ControlLocked(execution::ControlKind::Recovery,static_cast<std::uint32_t>(reason)));
}
bool Producer::Configure(const execution::Control& control, std::uint32_t generation) noexcept {
    std::array<std::uint8_t,execution::kHeaderSize> bytes{};
    if (generation == 0 || control.kind != execution::ControlKind::Offer || control.scope.consumerEpoch != 1 ||
        !execution::EncodeControl(control,bytes)) return false;
    const std::lock_guard lock(gate_);
    if (generation_ != 0 && binding_.scope == control.scope) {
        if (generation != generation_ || binding_.value != control.value || !Bound()) return false;
        return ControlLocked(execution::ControlKind::Ready,control.value);
    }
    // No old packet may cross a new source checkpoint. The IPC consumer drains
    // the previous explicit boundary first. No mid-command restoration exists.
    if (count_ != 0 || Bound()) return false;
    if (generation_ != 0 && control.scope.session == binding_.scope.session &&
        (control.scope.authorityEpoch < binding_.scope.authorityEpoch ||
         control.scope.worldEpoch < binding_.scope.worldEpoch ||
         (control.scope.worldEpoch == binding_.scope.worldEpoch &&
          control.scope.authorityEpoch == binding_.scope.authorityEpoch))) return false;
    commands_ = {}; owners_ = {}; binding_ = control; generation_ = generation;
    command_ = global_ = batch_ = historyFloor_ = 0;
    active_.store(true,std::memory_order_release);
    if (!ControlLocked(execution::ControlKind::Ready,control.value)) { active_.store(false); return false; }
    return true;
}
void Producer::Retire() noexcept { const std::lock_guard lock(gate_); emittingKind_=0; FailLocked(execution::Recovery::TransportLost); }
void Producer::LoseCoverage(execution::Recovery reason) noexcept { const std::lock_guard lock(gate_); emittingKind_=0; FailLocked(reason); }
Producer::Entry* Producer::Find(std::uint64_t request) noexcept {
    if (request == 0) return nullptr;
    for (auto& item : commands_) if (item.used && !item.released && item.request == request) return &item;
    return nullptr;
}
Producer::Owner* Producer::FindOwner(const OwnerKey& key) noexcept {
    for (auto& owner : owners_) if (owner.used && owner.key == key) return &owner;
    for (auto& owner : owners_) if (!owner.used) { owner = {key,0,true}; return &owner; }
    return nullptr;
}
bool Producer::Emit(Entry& entry, std::span<const Op> operations, bool payload) noexcept {
    emittingKind_=entry.parameters.kind;
    if (operations.empty() || operations.size() > execution::kMaxRecords || count_ >= kPackets - 1 ||
        token_ == UINT64_MAX || batch_ == UINT64_MAX || global_ > UINT64_MAX - operations.size()) {
        FailLocked(execution::Recovery::Capacity); return false;
    }
    auto* owner = FindOwner(entry.owner);
    if (!owner || owner->sequence > UINT64_MAX - operations.size()) { FailLocked(execution::Recovery::Capacity); return false; }
    execution::Batch batch{}; batch.scope = binding_.scope; batch.batchId = batch_ + 1; batch.count = operations.size();
    for (std::size_t i = 0; i < operations.size(); ++i) {
        auto& record = batch.records[i]; record.scope = binding_.scope; record.owner = entry.owner;
        record.commandId = entry.command; record.parentCommandId = entry.parent;
        record.globalSeq = global_ + i + 1; record.ownerSeq = owner->sequence + i + 1;
        record.opcode = operations[i]; record.parameterRevision = entry.revision; record.nativeKind = entry.parameters.kind;
        record.admission=entry.parameters.admission;
        if (payload && !movement::Validate(entry.parameters.movement)) { FailLocked(execution::Recovery::InvalidStream); return false; }
        if (payload && !Parameters(entry.parameters,record)) { FailLocked(execution::Recovery::InvalidStream); return false; }
    }
    std::vector<std::uint8_t> bytes;
    if (!execution::EncodeBatch(batch,bytes)) { FailLocked(execution::Recovery::InvalidStream); return false; }
    auto& packet = packets_[(read_ + count_) % kPackets]; packet = {};
    packet.size = bytes.size(); packet.token = ++token_;
    std::copy(bytes.begin(),bytes.end(),packet.bytes.begin()); ++count_;
    global_ += operations.size(); entry.lastSequence=global_; owner->sequence += operations.size();
    entry.lastOwnerSequence=owner->sequence;entry.lastOperation=operations.back(); ++batch_;
    if(const auto trace=gCommandTrace.load(std::memory_order_acquire))
        for(std::size_t i=0;i<batch.count;++i)trace(batch.records[i]);
    return true;
}
SourceResult Producer::Admit(const OwnerKey& key, std::uint64_t request, const Command& parameters,
    std::uint64_t parent) noexcept {
    const std::lock_guard lock(gate_);
    if (!Bound()) return SourceResult::Unbound;
    if (!request || Find(request)) return SourceResult::Conflict;
    emittingKind_=parameters.kind;
    const auto profile = execution::ProfileForKind(parameters.kind);
    if (profile == 0) { FailLocked(execution::Recovery::NativeUnavailable); return SourceResult::Invalid; }
    if ((binding_.value & profile) == 0) { FailLocked(execution::Recovery::NativeUnavailable); return SourceResult::Invalid; }
    if(parameters.movement.navigation==movement::Navigation::HostRoute&&(binding_.value&8U)==0) {
        FailLocked(execution::Recovery::NativeUnavailable);return SourceResult::Invalid;
    }
    // A native Move companion belongs to its bundle, not another command.
    if (parent != 0) { FailLocked(execution::Recovery::NativeUnavailable); return SourceResult::Invalid; }
    Entry* entry{}; for (auto& candidate : commands_) if (!candidate.used) { entry = &candidate; break; }
    if (!entry || command_ == UINT64_MAX) { FailLocked(execution::Recovery::Capacity); return SourceResult::Capacity; }
    *entry = {}; entry->used = true; entry->owner = key; entry->parameters = parameters;
    entry->request = request; entry->command = ++command_; entry->parent = parent;
    constexpr std::array ops{Op::Begin,Op::Submit};
    return Emit(*entry,ops,true) ? SourceResult::Ok : SourceResult::CoverageLost;
}
SourceResult Producer::Update(std::uint64_t request, const Command& parameters) noexcept {
    const std::lock_guard lock(gate_);
    if (!Bound()) return SourceResult::Unbound;
    auto* entry = Find(request);
    if (!entry) { FailLocked(execution::Recovery::InvalidStream); return SourceResult::UnknownRequest; }
    if (entry->cancelled || entry->completed || entry->parameters.kind != parameters.kind || entry->revision == UINT32_MAX ||
        entry->parameters.admission!=parameters.admission || entry->parameters.movement.navigation!=parameters.movement.navigation) {
        FailLocked(execution::Recovery::InvalidStream); return SourceResult::Conflict;
    }
    if (entry->parameters == parameters) return SourceResult::Ok;
    entry->parameters = parameters; ++entry->revision;entry->routeRevisions={};
    constexpr std::array ops{Op::Update};
    return Emit(*entry,ops,true) ? SourceResult::Ok : SourceResult::CoverageLost;
}
SourceResult Producer::NativeEvent(const OwnerKey& owner,const Command& parameters) noexcept {
    const std::lock_guard lock(gate_);
    if(!Bound())return SourceResult::Unbound;
    const auto profile=execution::ProfileForKind(parameters.kind);
    if((profile!=execution::kProfileLifecycle&&parameters.kind!=native_intent::kKind)||
        parameters.admission!=1||(binding_.value&profile)==0)return SourceResult::Invalid;
    Entry* entry{};for(auto& candidate:commands_)if(!candidate.used){entry=&candidate;break;}
    if(!entry||command_==UINT64_MAX){FailLocked(execution::Recovery::Capacity);return SourceResult::Capacity;}
    *entry={};entry->used=true;entry->owner=owner;entry->parameters=parameters;entry->command=++command_;
    // Complete does not supersede an irreversible native event. Release has
    // no scheduler Ref to drop, but waits for that event's actual disposition.
    constexpr std::array ops{Op::Begin,Op::Submit};
    if(!Emit(*entry,ops,true))return SourceResult::CoverageLost;
    constexpr std::array terminal{Op::Complete,Op::Release};
    if(!Emit(*entry,terminal,false))return SourceResult::CoverageLost;
    entry->completed=entry->released=true;return SourceResult::Ok;
}
SourceResult Producer::Lifecycle(std::uint64_t request, Op operation) noexcept {
    const std::lock_guard lock(gate_);
    if (!Bound()) return SourceResult::Unbound;
    auto* entry = Find(request);
    if (!entry) { FailLocked(execution::Recovery::InvalidStream); return SourceResult::UnknownRequest; }
    if ((operation == Op::Cancel && (entry->cancelled || entry->completed)) ||
        (operation == Op::Complete && entry->completed)) return SourceResult::Ok;
    if ((operation == Op::Complete && entry->cancelled) ||
        (operation == Op::Release && !entry->completed && !entry->cancelled)) {
        FailLocked(execution::Recovery::InvalidStream); return SourceResult::Conflict;
    }
    const std::array ops{operation};
    if (!Emit(*entry,ops,false)) return SourceResult::CoverageLost;
    if (operation == Op::Cancel) entry->cancelled = true;
    if (operation == Op::Complete) entry->completed = true;
    if (operation == Op::Release) entry->released = true;
    return SourceResult::Ok;
}
SourceResult Producer::Cancel(std::uint64_t request) noexcept { return Lifecycle(request,Op::Cancel); }
void SetCommandTrace(CommandTrace trace) noexcept { gCommandTrace.store(trace,std::memory_order_release); }
SourceResult Producer::AppendState(const State& state) noexcept {
    const std::lock_guard lock(gate_);
    if(!Bound())return SourceResult::Unbound;
    if(count_>=kPackets-1||token_==UINT64_MAX){FailLocked(execution::Recovery::Capacity);return SourceResult::Capacity;}
    auto& packet=packets_[(read_+count_)%kPackets];packet={};packet.state=true;
    packet.size=causal_state::kBytes;packet.token=token_+1;
    if(!causal_state::Encode(std::span(packet.bytes).first(packet.size),binding_.scope,packet.token,global_,state)) {
        FailLocked(execution::Recovery::InvalidStream);return SourceResult::Invalid;
    }
    ++token_;++count_;return SourceResult::Ok;
}
bool Producer::NextRouteKey(const CommandIdentity& identity,std::uint32_t part,route::Key& output) const noexcept {
    const std::lock_guard lock(gate_);
    if(!Bound()||identity.scope!=binding_.scope)return false;
    for(const auto& entry:commands_)if(entry.used&&entry.command==identity.command&&entry.owner==identity.owner) {
        if(entry.routeBlocked||entry.cancelled||entry.completed||entry.released||entry.revision!=identity.parameterRevision||
            entry.parameters.kind!=identity.nativeKind||(identity.nativeKind!=1&&identity.nativeKind!=2&&identity.nativeKind!=3)||
            identity.navigation!=movement::Navigation::HostRoute||entry.parameters.movement.navigation!=identity.navigation||
            part>=entry.parameters.movement.parts||part>=entry.routeRevisions.size()||entry.routeRevisions[part]==UINT32_MAX)return false;
        output={binding_.scope,entry.owner,entry.command,entry.revision,entry.routeRevisions[part]+1,part};
        return true;
    }
    return false;
}
SourceResult Producer::AppendRoute(const route::Graph& graph) noexcept {
    std::vector<std::uint8_t> bytes;
    if (route::Encode(graph,bytes)!=route::Result::Ok) return SourceResult::Invalid;
    const std::lock_guard lock(gate_);
    if (!Bound() || graph.key.scope!=binding_.scope) return SourceResult::Unbound;
    Entry* entry{};
    for(auto& item:commands_)if(item.used&&item.command==graph.key.commandId&&item.owner==graph.key.owner) {entry=&item;break;}
    if(!entry||entry->routeBlocked||entry->cancelled||entry->completed||entry->released||entry->revision!=graph.key.parameterRevision||
        (entry->parameters.kind!=1&&entry->parameters.kind!=2&&entry->parameters.kind!=3)||graph.key.part>=entry->parameters.movement.parts||
        entry->parameters.movement.navigation!=movement::Navigation::HostRoute||
        entry->routeRevisions[graph.key.part]==UINT32_MAX||graph.key.routeRevision!=entry->routeRevisions[graph.key.part]+1) {
        return SourceResult::Conflict;
    }
    if(count_>=kPackets-1||token_==UINT64_MAX) {FailLocked(execution::Recovery::Capacity);return SourceResult::Capacity;}
    auto& packet=packets_[(read_+count_)%kPackets];packet={};packet.route=true;packet.size=bytes.size();packet.token=++token_;
    std::copy(bytes.begin(),bytes.end(),packet.bytes.begin());++count_;entry->routeRevisions[graph.key.part]=graph.key.routeRevision;
    if(graph.failure!=route::Failure::None)entry->routeBlocked=true;
    else {entry->routeAuxiliary[graph.key.part]=graph.auxiliary;entry->routeAuxiliaryKnown[graph.key.part]=true;}
    return SourceResult::Ok;
}
SourceResult Producer::BlockRoute(const CommandIdentity& identity,std::uint32_t part,route::Failure failure) noexcept {
    if(failure==route::Failure::None)return SourceResult::Invalid;
    route::Graph refusal{};refusal.failure=failure;
    if(!NextRouteKey(identity,part,refusal.key))return SourceResult::Conflict;
    return AppendRoute(refusal);
}
bool Producer::PreviousRouteAuxiliary(const CommandIdentity& identity,std::uint32_t part,std::array<float,6>& output) const noexcept {
    const std::lock_guard lock(gate_);
    if(!Bound()||identity.scope!=binding_.scope||part>=2)return false;
    for(const auto& entry:commands_)if(entry.used&&entry.command==identity.command&&entry.owner==identity.owner&&
        entry.revision==identity.parameterRevision&&entry.parameters.kind==identity.nativeKind&&
        entry.parameters.movement.navigation==movement::Navigation::HostRoute&&!entry.cancelled&&!entry.completed&&!entry.released&&entry.routeAuxiliaryKnown[part]) {
        output=entry.routeAuxiliary[part];return true;
    }
    return false;
}
SourceResult Producer::Complete(std::uint64_t request) noexcept { return Lifecycle(request,Op::Complete); }
SourceResult Producer::Release(std::uint64_t request) noexcept { return Lifecycle(request,Op::Release); }
bool Producer::Peek(HostPacket& packet) noexcept {
    const std::lock_guard lock(gate_); if (!count_) return false; packet = packets_[read_]; return true;
}
bool Producer::Commit(std::uint64_t token) noexcept {
    const std::lock_guard lock(gate_);
    if (!count_ || packets_[read_].token != token) return false;
    read_ = (read_ + 1) % kPackets; --count_; return true;
}
bool Producer::Current(const transactions::Scope& scope, std::uint32_t generation) const noexcept {
    const std::lock_guard lock(gate_); return Bound() && binding_.scope == scope && generation_ == generation;
}
bool Producer::Scope(transactions::Scope& scope) const noexcept {
    const std::lock_guard lock(gate_); if (!Bound()) return false; scope = binding_.scope; return true;
}
bool Producer::Identify(std::uint64_t request,CommandIdentity& output) const noexcept {
    const std::lock_guard lock(gate_);
    if(!request||!Bound())return false;
    for(const auto& entry:commands_)if(entry.used&&entry.request==request&&!entry.released&&!entry.cancelled&&!entry.completed) {
        output={binding_.scope,entry.owner,entry.command,entry.revision,entry.parameters.kind,entry.parameters.movement.navigation};return true;
    }
    return false;
}
bool Producer::ActionReference(std::uint64_t request,std::uint32_t revision,action_correlation::Reference& output) const noexcept {
    const std::lock_guard lock(gate_);
    if(!Bound()||!request)return false;
    for(const auto& entry:commands_)if(entry.used&&entry.request==request&&!entry.cancelled&&!entry.completed&&!entry.released) {
        if(entry.parameters.kind!=execution::kActionKind)return false;
        transactions::Record record{};record.scope=binding_.scope;record.owner=entry.owner;
        record.commandId=entry.command;record.globalSeq=entry.lastSequence;record.ownerSeq=entry.lastOwnerSequence;
        record.parameterRevision=entry.revision;record.opcode=entry.lastOperation;
        if(!Parameters(entry.parameters,record))return false;
        return action_correlation::Capture(record,revision,output);
    }
    return false;
}
std::uint32_t Producer::Profiles() const noexcept { const std::lock_guard lock(gate_); return Bound() ? binding_.value : 0; }
bool Producer::Checkpoint(const execution::Control& control,std::uint32_t generation) noexcept {
    std::array<std::uint8_t,execution::kHeaderSize> bytes{};
    if(!execution::EncodeControl(control,bytes))return false;
    const std::lock_guard lock(gate_);
    if(!Bound()||control.kind!=execution::ControlKind::Checkpoint||control.scope!=binding_.scope||
        generation!=generation_||!control.through||control.through<historyFloor_||control.through>global_)return false;
    if(!ControlLocked(execution::ControlKind::CheckpointAck,0,control.through))return false;
    for(auto& entry:commands_)if(entry.used&&entry.released&&entry.lastSequence<=control.through)entry={};
    historyFloor_=control.through;return true;
}
Producer::Failure Producer::LastFailure() const noexcept { const std::lock_guard lock(gate_); return failure_; }
HistoryDiagnostics Producer::History() const noexcept {
    const std::lock_guard lock(gate_);
    const auto commands=static_cast<std::size_t>(std::count_if(commands_.begin(),commands_.end(),[](const auto& e){return e.used;}));
    return {historyFloor_,commands,count_};
}
bool Initialize(QualifyHost qualify) noexcept {
    const std::lock_guard lock(gBindingGate); if (!qualify || gQualify) return false; gQualify = qualify; return true;
}
bool QueueBindingHost(std::span<const std::uint8_t> bytes, std::uint32_t generation) noexcept {
    execution::Control control{};
    if (!generation || !execution::DecodeControl(bytes,control) || control.kind == execution::ControlKind::Ready ||
        control.kind==execution::ControlKind::CheckpointAck ||
        control.scope.consumerEpoch != 1) return false;
    const std::lock_guard lock(gBindingGate);
    if (!gQualify || gRevision == UINT64_MAX) return false;
    if (control.kind != execution::ControlKind::Offer && control.scope != gPending.scope) return false;
    gPending = control; gPendingGeneration = generation; ++gRevision; return true;
}
void PumpHost() noexcept {
    if (gPumping) return;
    struct Guard { Guard() noexcept { gPumping = true; } ~Guard() { gPumping = false; } } pumping;
    execution::Control control{}; std::uint32_t generation{}; std::uint64_t revision{}; QualifyHost qualify{};
    {
        const std::lock_guard lock(gBindingGate);
        control = gPending; generation = gPendingGeneration; revision = gRevision; qualify = gQualify;
    }
    if (!qualify || revision == 0) return;
    if (control.kind != execution::ControlKind::Offer && control.kind!=execution::ControlKind::Checkpoint) {
        const std::lock_guard lock(gBindingGate);
        if (revision == gRevision) { gSource.Retire(); gApplied = revision; }
        return;
    }
    if (!qualify(control.scope,generation)) {
        const std::lock_guard lock(gBindingGate);
        if (revision == gRevision && gSource.Bound()) gSource.Retire();
        return;
    }
    // Qualify may reenter or a pipe worker may close/replace the binding while
    // it runs. A copied Offer must never resurrect a subsequently retired scope.
    const std::lock_guard lock(gBindingGate);
    if (revision != gRevision) return;
    if (gApplied == revision) return;
    if (control.kind==execution::ControlKind::Checkpoint ? gSource.Checkpoint(control,generation) : gSource.Configure(control,generation))
        gApplied = revision;
}
void RetireHost() noexcept {
    const std::lock_guard lock(gBindingGate);
    if (gRevision != UINT64_MAX) ++gRevision;
    gPending.kind = execution::ControlKind::Close; gSource.Retire();
}
bool PeekHost(HostPacket& packet) noexcept { return gSource.Peek(packet); }
bool CommitHost(std::uint64_t token) noexcept { return gSource.Commit(token); }
bool HostScope(transactions::Scope& scope) noexcept { return gSource.Scope(scope); }
Producer& EngineProducer() noexcept { return gSource; }
} // namespace aimp::ai_sync::host_producer
