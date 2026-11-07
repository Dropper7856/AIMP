#include "aimp/AISyncExecutionMailbox.h"
#include "aimp/AISyncCausalState.h"
#include <chrono>
#include "aimp/AISyncProfiling.h"
#include <algorithm>

namespace aimp::ai_sync::execution {
namespace {
struct PumpGuard {
    std::atomic_flag& flag;
    ~PumpGuard() { flag.clear(std::memory_order_release); }
};
}
void ExecutionMailbox::InvalidateLocked() noexcept {
    activeRevision_.store(0, std::memory_order_release);
    if (revision_ != UINT64_MAX) ++revision_;
    inRead_ = inCount_ = outRead_ = outCount_ = 0;
    bindingPending_ = false;lastCarrier_=0;
}
void ExecutionMailbox::Retire() noexcept {
    std::lock_guard lock(gate_); InvalidateLocked(); requested_ = {}; nativeGeneration_ = 0;
}
void ExecutionMailbox::RetireGeneration(const std::uint32_t generation) noexcept {
    std::lock_guard lock(gate_);
    if (generation == 0U || nativeGeneration_ != generation) return;
    InvalidateLocked(); requested_ = {}; nativeGeneration_ = 0;
}
bool ExecutionMailbox::Fence(Recovery reason) noexcept {
    std::lock_guard lock(gate_);
    const auto value = static_cast<std::uint32_t>(reason);
    if (activeRevision_.load(std::memory_order_acquire) == 0 || value < 1 || value > 5) return false;
    FailLocked(reason); return true;
}
bool ExecutionMailbox::Matches(const transactions::Scope& scope) noexcept {
    const std::lock_guard lock(gate_);
    return activeRevision_.load(std::memory_order_acquire) != 0 && requested_.scope == scope;
}
bool ExecutionMailbox::Fence(const transactions::Scope& scope, Recovery reason) noexcept {
    const std::lock_guard lock(gate_);
    const auto value = static_cast<std::uint32_t>(reason);
    if (activeRevision_.load(std::memory_order_acquire) == 0 || requested_.scope != scope || value < 1 || value > 5) return false;
    FailLocked(reason); return true;
}
bool ExecutionMailbox::PublishLocked(const Control& control) noexcept {
    std::array<std::uint8_t, kHeaderSize> bytes{};
    if (!EncodeControl(control, bytes) || outCount_ == kOutgoing || nextToken_ == UINT64_MAX ||
        (control.kind != ControlKind::Recovery && outCount_ >= kOutgoing - 1)) return false;
    auto& packet = outgoing_[(outRead_ + outCount_++) % kOutgoing]; packet = {};
    packet.size = bytes.size(); packet.token = ++nextToken_;
    std::copy(bytes.begin(), bytes.end(), packet.bytes.begin()); return true;
}
bool ExecutionMailbox::PublishLocked(const Receipt& receipt) noexcept {
    if (outCount_ >= kOutgoing - 1 || nextToken_ == UINT64_MAX) return false;
    auto& packet = outgoing_[(outRead_ + outCount_) % kOutgoing]; packet = {};
    std::vector<std::uint8_t> bytes;
    if (!EncodeReceipt(receipt, bytes)) return false;
    std::copy(bytes.begin(),bytes.end(),packet.bytes.begin());
    ++outCount_; packet.receipt = true; packet.size = bytes.size(); packet.token = ++nextToken_; return true;
}
MailboxFailure ExecutionMailbox::LastFailure() noexcept {
    const std::lock_guard lock(gate_);return failure_;
}
void ExecutionMailbox::FailLocked(Recovery reason,std::uint32_t site,std::uint32_t stage,const transactions::Record* operation) noexcept {
    ++failure_.count;failure_.site=site;failure_.reason=static_cast<std::uint32_t>(reason);
    failure_.stage=stage;failure_.sequence=operation?operation->globalSeq:0;
    failure_.kind=operation?operation->nativeKind:0;failure_.opcode=operation?static_cast<std::uint32_t>(operation->opcode):0;
    const auto scope = requested_.scope;
    activeRevision_.store(0, std::memory_order_release);
    if (revision_ != UINT64_MAX) ++revision_;
    inRead_ = inCount_ = 0;
    bindingPending_ = false;
    static_cast<void>(PublishLocked(Control{scope, ControlKind::Recovery, static_cast<std::uint32_t>(reason)}));
}
bool ExecutionMailbox::QueueBinding(const Control& control, std::uint32_t generation) noexcept {
    std::array<std::uint8_t, kHeaderSize> validated{};
    if (!EncodeControl(control, validated) || control.kind == ControlKind::Ready || control.kind==ControlKind::CheckpointAck) return false;
    std::lock_guard lock(gate_);
    if(control.kind==ControlKind::Checkpoint) {
        if(control.scope!=requested_.scope||generation!=nativeGeneration_||
            activeRevision_.load(std::memory_order_acquire)==0)return false;
        if(inCount_==kIncoming){FailLocked(Recovery::Capacity);return false;}
        auto& work=incoming_[(inRead_+inCount_++)%kIncoming];work={};
        work.binding=control;work.nativeGeneration=generation;work.revision=revision_;work.checkpoint=true;return true;
    }
    if (control.kind != ControlKind::Offer) {
        if (control.scope != requested_.scope) return false;
        InvalidateLocked(); requested_ = {}; nativeGeneration_ = 0; return true;
    }
    if (generation == 0 || revision_ == UINT64_MAX || nextToken_ == UINT64_MAX) return false;
    if (control.scope == requested_.scope) {
        if (control.value != requested_.value || generation != nativeGeneration_) return false;
        if (activeRevision_.load(std::memory_order_acquire) != 0) {
            if (PublishLocked(Control{control.scope, ControlKind::Ready, control.value})) return true;
            FailLocked(Recovery::Capacity); return false;
        }
        return bindingPending_; // Includes a bind temporarily executing on the engine thread.
    }
    std::size_t session = 0;
    for (; session < sessionCount_; ++session) if (sessions_[session].session == control.scope.session) break;
    if (session == sessionCount_) {
        if (sessionCount_ == sessions_.size()) return false;
        sessions_[sessionCount_++].session = control.scope.session;
    }
    auto& watermark = sessions_[session];
    if (control.scope.authorityEpoch < watermark.authority || control.scope.consumerEpoch <= watermark.consumer ||
        control.scope.worldEpoch < watermark.world) return false;
    watermark.authority = control.scope.authorityEpoch; watermark.consumer = control.scope.consumerEpoch;
    watermark.world = control.scope.worldEpoch;
    InvalidateLocked(); requested_ = control; nativeGeneration_ = generation;
    auto& work = incoming_[0]; work = {}; work.binding = control; work.nativeGeneration = generation;
    work.revision = revision_; work.bind = true; inCount_ = 1; bindingPending_ = true; return true;
}
bool ExecutionMailbox::QueueBatch(std::span<const std::uint8_t> bytes, bool retry) noexcept {
    std::lock_guard lock(gate_);
    if (activeRevision_.load(std::memory_order_acquire) == 0) return false;
    transactions::Scope scope{};
    if (!DecodeBatchScope(bytes,scope) || scope != requested_.scope) return false;
    Batch batch{};
    if (!DecodeBatch(bytes, requested_.scope, batch) || (retry && batch.count != 1)) { FailLocked(Recovery::InvalidStream); return false; }
    for (std::size_t i = 0; i < batch.count; ++i)
        if ((requested_.value & ProfileForKind(batch.records[i].nativeKind)) == 0 ||
            (movement::CarriesRoute(batch.records[i].movement.navigation)&&(requested_.value&8U)==0)) {
            FailLocked(Recovery::InvalidStream); return false;
        }
    if (inCount_ == kIncoming) { FailLocked(Recovery::Capacity); return false; }
    auto& work = incoming_[(inRead_ + inCount_++) % kIncoming]; work = {};
    work.retry = retry; work.queuedTick=GetTickCount64();
    work.binding = requested_; work.nativeGeneration = nativeGeneration_; work.revision = revision_; work.size = bytes.size();
    std::copy(bytes.begin(), bytes.end(), work.bytes.begin()); return true;
}
bool ExecutionMailbox::QueueCarrier(std::span<const std::uint8_t> bytes) noexcept {
    transactions::Scope scope{};std::uint64_t serial{},through{};State state{};
    if(!causal_state::Decode(bytes,scope,serial,through,state))return false;
    const std::lock_guard lock(gate_);
    if(activeRevision_.load(std::memory_order_acquire)==0||scope!=requested_.scope)return false;
    if(serial<=lastCarrier_)return true;
    if(inCount_==kIncoming){FailLocked(Recovery::Capacity);return false;}
    static_assert(causal_state::kBytes<=kMaximumBatchBytes);
    auto& work=incoming_[(inRead_+inCount_++)%kIncoming];work={};work.carrier=true;
    work.binding=requested_;work.nativeGeneration=nativeGeneration_;work.revision=revision_;work.size=bytes.size();
    std::copy(bytes.begin(),bytes.end(),work.bytes.begin());work.queuedTick=GetTickCount64();lastCarrier_=serial;return true;
}
bool ExecutionMailbox::QueueRoute(std::span<const std::uint8_t> bytes) noexcept {
    route::Key key{};route::Graph graph{};
    if(route::DecodeKey(bytes,key)!=route::Result::Ok||route::Decode(bytes,key,graph)!=route::Result::Ok)return false;
    const std::lock_guard lock(gate_);
    if(activeRevision_.load(std::memory_order_acquire)==0||key.scope!=requested_.scope)return false;
    if((requested_.value&kProfileRoutes)==0){FailLocked(Recovery::InvalidStream);return false;}
    if(inCount_==kIncoming){FailLocked(Recovery::Capacity);return false;}
    auto& work=incoming_[(inRead_+inCount_++)%kIncoming];work={};work.route=true;
    work.binding=requested_;work.nativeGeneration=nativeGeneration_;work.revision=revision_;work.size=bytes.size();
    std::copy(bytes.begin(),bytes.end(),work.bytes.begin());work.queuedTick=GetTickCount64();return true;
}
bool ExecutionMailbox::Current(const transactions::Scope& scope, std::uint32_t generation) const noexcept {
    return processingActive_ && scope == processing_.binding.scope && generation == processing_.nativeGeneration &&
        activeRevision_.load(std::memory_order_acquire) == processing_.revision;
}
bool ExecutionMailbox::Bound(const transactions::Scope& scope, std::uint32_t generation) noexcept {
    // Called by engine callbacks OUTSIDE gate_. Contention with an IPC read
    // is not a revoked incarnation. A spurious false here could permanently
    // fence a live actor during a native admission/lifetime revalidation.
    const std::lock_guard lock(gate_);
    return activeRevision_.load(std::memory_order_acquire) != 0 &&
        requested_.scope == scope && nativeGeneration_ == generation;
}
void ExecutionMailbox::Pump(const MailboxEngine& engine) noexcept {
    if (pumping_.test_and_set(std::memory_order_acquire)) return;
    const PumpGuard guard{pumping_};
    if (engine.bind == nullptr || engine.execute == nullptr) return;
    // A slow Client frame must not let the Host outpace this queue: always
    // take 4, then keep draining within a short wall-time slice.
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t budget = 0; budget < kIncoming; ++budget) {
        if (budget >= 4 && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(4)) return;
        {
            std::unique_lock lock(gate_, std::try_to_lock);
            if (!lock.owns_lock() || inCount_ == 0 || outCount_ > kOutgoing - kMaxRecords - 1) return;
            processing_ = incoming_[inRead_]; inRead_ = (inRead_ + 1) % kIncoming; --inCount_;
        }
        if(processing_.queuedTick) {
            const auto now=GetTickCount64();
            if(now>=processing_.queuedTick)profiling::Value(profiling::Section::MailboxQueueAge,(now-processing_.queuedTick)*1000U);
        }
        processingActive_ = true;
        std::array<Receipt, kMaxRecords> receipts{}; std::size_t count = 0;
        bool success{}, bindWaiting{};
        if (processing_.bind) {
            const auto result = engine.bind(engine.context, processing_.binding.scope, processing_.nativeGeneration);
            success = result == transactions::Result::Ok; bindWaiting = result == transactions::Result::OwnerBusy;
        }
        else if (Current(processing_.binding.scope, processing_.nativeGeneration)) {
            if(processing_.checkpoint) {
                success=engine.checkpoint&&engine.checkpoint(engine.context,processing_.binding.scope,
                    processing_.binding.through)==transactions::Result::Ok;
            } else if(processing_.route) {
                success=engine.route&&engine.route(engine.context,std::span(processing_.bytes).first(processing_.size),processing_.binding.scope);
            } else if(processing_.carrier) {
                success=engine.carrier&&engine.carrier(engine.context,std::span(processing_.bytes).first(processing_.size),processing_.binding.scope,processing_.queuedTick);
            } else if (processing_.retry) {
                Batch batch{};
                success = engine.retry != nullptr && DecodeBatch(std::span(processing_.bytes).first(processing_.size),
                    processing_.binding.scope, batch) && engine.retry(engine.context, batch.records[0], batch.batchId, batch.scope, receipts[0]);
                if (success) count = 1;
            } else success = engine.execute(engine.context, std::span(processing_.bytes).first(processing_.size),
                processing_.binding.scope, receipts, count);
        }
        processingActive_ = false;
        std::lock_guard lock(gate_);
        if (processing_.revision != revision_) continue; // Retired during a callback; no stale result publication.
        if (processing_.bind) {
            if (bindWaiting) {
                // The old scope is draining. Keep this exact bind pending;
                // no Ready and no retry loop inside the current engine tick.
                if (inCount_ == kIncoming) { FailLocked(Recovery::Capacity); return; }
                inRead_ = (inRead_ + kIncoming - 1) % kIncoming;
                incoming_[inRead_] = processing_; ++inCount_; return;
            }
            if (success) {
                bindingPending_ = false;
                activeRevision_.store(revision_, std::memory_order_release);
                success = PublishLocked(Control{requested_.scope, ControlKind::Ready, requested_.value});
            }
        } else if(processing_.carrier||processing_.route) {
            if(!success)FailLocked(Recovery::InvalidStream);
            continue;
        } else if(processing_.checkpoint) {
            if(success) {auto ack=processing_.binding;ack.kind=ControlKind::CheckpointAck;success=PublishLocked(ack);}
        } else {
            Batch batch{};
            if (!DecodeBatch(std::span(processing_.bytes).first(processing_.size), processing_.binding.scope, batch) || count > batch.count) {
                FailLocked(Recovery::NativeUnavailable,__LINE__,1);continue;
            }
            bool failed{};
            for (std::size_t i = 0; i < count; ++i) {
                transactions::CanonicalRecord expected{}, actual{};
                if (receipts[i].scope != batch.scope || receipts[i].batchId != batch.batchId ||
                    transactions::Encode(batch.records[i], expected) != transactions::Result::Ok ||
                    transactions::Encode(receipts[i].operation, actual) != transactions::Result::Ok || expected != actual ||
                    !movement::Same(batch.records[i].movement,receipts[i].operation.movement) ||
                    batch.records[i].admission!=receipts[i].operation.admission ||
                    batch.records[i].scalarRetention!=receipts[i].operation.scalarRetention) {
                    FailLocked(Recovery::NativeUnavailable,__LINE__,3,&batch.records[i]);failed=true;break;
                }
                if(!PublishLocked(receipts[i])) {
                    FailLocked(Recovery::NativeUnavailable,__LINE__,4,&batch.records[i]);failed=true;break;
                }
            }
            if(failed)continue;
            if(!success||count!=batch.count) {
                FailLocked(Recovery::NativeUnavailable,__LINE__,2,&batch.records[(std::min)(count,batch.count-1)]);continue;
            }
        }
        if (!success) FailLocked(Recovery::NativeUnavailable);
    }
}
bool ExecutionMailbox::Peek(MailboxPacket& packet) noexcept {
    std::lock_guard lock(gate_); if (outCount_ == 0) return false;
    packet = outgoing_[outRead_]; return true;
}
bool ExecutionMailbox::Commit(std::uint64_t token) noexcept {
    std::lock_guard lock(gate_); if (outCount_ == 0 || outgoing_[outRead_].token != token) return false;
    outRead_ = (outRead_ + 1) % kOutgoing; --outCount_; return true;
}
bool SessionTextMatches(const transactions::SessionId& session, std::string_view text) noexcept {
    if (text.size() != 36) return false;
    constexpr std::array<std::size_t,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    constexpr char hex[] = "0123456789abcdef";
    std::size_t at = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if ((i == 4 || i == 6 || i == 8 || i == 10) && text[at++] != '-') return false;
        for (const auto nibble : {static_cast<std::uint8_t>(session[order[i]] >> 4), static_cast<std::uint8_t>(session[order[i]] & 15)}) {
            auto value = text[at++]; if (value >= 'A' && value <= 'F') value = static_cast<char>(value - 'A' + 'a');
            if (value != hex[nibble]) return false;
        }
    }
    return true;
}
} // namespace aimp::ai_sync::execution
