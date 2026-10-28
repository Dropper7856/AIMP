#include "aimp/AISyncTransactionCapture.h"
#include "aimp/AISyncHostProducer.h"
#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>

// Static ABI provenance (no game execution): certified AI.exe SHA-256
// 00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364.
// Retained Ghidra instructions: aisync_semantic_audit_20261023/native-command-chain
// /0070a940.txt; aisync_phase2_20261023/native-{commands,authority}; and
// aisync_impl_p0p1_20261023/native-capture/{006c7550,0067d0d0,0040c850,006f8490}.txt.
// Move/Stop RET 1C/C, ECX-only low operations RET, stop-update RET 8;
// goal-update RET 4 has a branch in its prefix, so only its two exact CALLs
// are redirected. Unknown consumers/parameter variants remain uncovered.

namespace aimp::ai_sync::transaction_capture {
namespace {
using namespace transactions;
constexpr std::uint32_t kObservation = ObservationOnly | Incomplete;
bool Admission(const Operation op) noexcept {
    return op == Operation::Replace || op == Operation::Append || op == Operation::Front;
}
void CopySample(Record& out, const NativeSample& sample) noexcept {
    out.nativeKind = sample.kind;
    out.nativeStatus = sample.status;
    if (sample.hasGoal && std::isfinite(sample.goal[0]) &&
        std::isfinite(sample.goal[1]) && std::isfinite(sample.goal[2])) {
        out.goalX = sample.goal[0]; out.goalY = sample.goal[1]; out.goalZ = sample.goal[2];
        out.flags |= HasGoal;
    }
    if (sample.hasStopDistance && std::isfinite(sample.stopDistance) && sample.stopDistance >= 0) {
        out.stopDistance = sample.stopDistance; out.flags |= HasStopDistance;
    }
    if (sample.hasGait) { out.gait = sample.gait; out.flags |= HasGait; }
}
template<class T> void PutLE(TransportEnvelope& bytes, const std::size_t offset, T value) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value & 0xFFU);
        value >>= 8U;
    }
}
std::uint64_t AddLoss(const std::uint64_t a, const std::uint64_t b) noexcept {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
}

bool EncodeTransport(const Stream stream, const Observation& observation,
    const std::uint64_t lossCount, TransportEnvelope& output) noexcept {
    if (observation.rawSequence == 0 || observation.captureEpoch == 0 ||
        (stream != Stream::Oracle && stream != Stream::Finalized) ||
        observation.producer > Producer::Stop || observation.operation < Operation::Replace ||
        observation.operation > Operation::StopUpdate || observation.coverage > Coverage::UnsupportedParameters)
        return false;
    auto record = observation.record;
    record.flags |= kObservation;
    CanonicalRecord canonical{};
    if (Encode(record, canonical) != Result::Ok) return false;
    TransportEnvelope bytes{};
    PutLE(bytes, 0, kTransportMagic);
    PutLE(bytes, 4, kTransportVersion);
    PutLE(bytes, 6, static_cast<std::uint16_t>(kTransportEnvelopeSize));
    PutLE(bytes, 8, static_cast<std::uint32_t>(stream));
    PutLE(bytes, 12, static_cast<std::uint32_t>(observation.producer));
    PutLE(bytes, 16, static_cast<std::uint32_t>(observation.operation));
    PutLE(bytes, 20, static_cast<std::uint32_t>(observation.coverage));
    PutLE(bytes, 24, observation.rawSequence);
    PutLE(bytes, 32, observation.captureEpoch);
    PutLE(bytes, 40, observation.subrequestId);
    PutLE(bytes, 48, lossCount);
    std::copy(canonical.begin(), canonical.end(), bytes.begin() + 56);
    output = bytes;
    return true;
}

void CaptureState::Configure(const ObservationSink sink, void* user) noexcept {
    sink_ = sink; sinkUser_ = user;
}
void CaptureState::Reset() noexcept {
    owners_ = {}; requests_ = {}; frames_ = {}; pending_ = {}; depth_ = 0; blockedDepth_ = 0;
    global_[0] = 0; global_[1] = 0;
    ++counters_.resets;
    if (counters_.captureEpoch != UINT64_MAX) ++counters_.captureEpoch;
    // command/token/raw IDs remain monotonic, even if a configuration-only
    // reset presents the same local context. A window is captureEpoch-scoped.
    counters_.coverageIncomplete = true;
}
CaptureState::Owner* CaptureState::FindOwner(const CaptureContext& context) noexcept {
    for (auto& owner : owners_) if (owner.used && owner.context == context) return &owner;
    for (auto& owner : owners_) if (!owner.used) {
        owner = {}; owner.context = context; owner.used = true; return &owner;
    }
    return nullptr;
}
void CaptureState::Emit(const Stream stream, Observation& observation) noexcept {
    const auto index = static_cast<std::size_t>(stream);
    auto* owner = FindOwner({observation.record.scope, observation.record.owner});
    if (owner == nullptr || global_[index] == UINT64_MAX || owner->seq[index] == UINT64_MAX) {
        ++counters_.overflow; return;
    }
    observation.record.globalSeq = ++global_[index];
    observation.record.ownerSeq = ++owner->seq[index];
    observation.record.flags |= kObservation;
    observation.captureEpoch = counters_.captureEpoch;
    if (stream == Stream::Oracle) ++counters_.oracle;
    else ++counters_.finalized;
    if (sink_ != nullptr) sink_(sinkUser_, stream, observation);
}
std::uint64_t CaptureState::Begin(const Producer producer,
    const CaptureContext& context, const std::uintptr_t state) noexcept {
    if (blockedDepth_ != 0 || depth_ == kDepth || token_ == UINT64_MAX || counters_.captureEpoch == UINT64_MAX) {
        ++blockedDepth_; ++counters_.overflow; return 0;
    }
    auto& frame = frames_[depth_]; frame = {};
    frame.context = context; frame.state = state; frame.producer = producer;
    frame.token = ++token_;
    frame.parent = depth_ == 0 ? 0 : frames_[depth_ - 1].primary;
    ++depth_; ++counters_.producers;
    return frame.token;
}
void CaptureState::Observe(const Operation operation, const CaptureContext& context,
    const std::uintptr_t request, const NativeSample& sample, const bool accepted) noexcept {
    if (rawSequence_ == UINT64_MAX) { ++counters_.overflow; return; }
    auto* frame = depth_ == 0 ? nullptr : &frames_[depth_ - 1];
    const bool scoped = blockedDepth_ == 0 && frame != nullptr && frame->context == context;
    Observation raw{};
    raw.record.scope = context.scope; raw.record.owner = context.owner;
    raw.record.opcode = Op::Oracle; raw.record.flags = kObservation | (accepted ? Accepted : 0U);
    raw.record.mode = static_cast<std::uint32_t>(operation);
    raw.rawSequence = ++rawSequence_; raw.operation = operation;
    raw.producer = scoped ? frame->producer : Producer::Unknown;
    raw.coverage = !scoped ? Coverage::OutsideProducer :
        (!sample.known ? Coverage::UnknownRequest : Coverage::Attributed);
    CopySample(raw.record, sample);
    // Independent oracle is emitted before transaction lookup/allocation and
    // before finalization. It survives missing maps, scopes and brief actions.
    Emit(Stream::Oracle, raw);
    if (!scoped) ++counters_.outsideProducer;
    if (!sample.known) ++counters_.unknown;

    Request* binding = nullptr;
    for (auto& item : requests_) if (item.used && item.address == request && item.context == context) {
        binding = &item; break;
    }
    if (Admission(operation) && accepted && request != 0) {
        if (binding == nullptr) for (auto& item : requests_) if (!item.used) { binding = &item; break; }
        if (binding != nullptr && command_ != UINT64_MAX) {
            // Every admission is a new incarnation, including allocator reuse
            // and identical endpoints between samples. Pending entries COPY IDs.
            *binding = {}; binding->used = true; binding->context = context;
            binding->address = request; binding->command = ++command_;
            binding->subrequest = raw.rawSequence;
            binding->parent = scoped ? (frame->primary != 0 ? frame->primary : frame->parent) : 0;
            if (scoped && frame->primary == 0) frame->primary = binding->command;
        } else { binding = nullptr; ++counters_.overflow; }
    }
    Observation final = raw;
    final.record.opcode = Op::CoverageLost;
    if (binding != nullptr && accepted) {
        final.record.commandId = binding->command;
        final.record.parentCommandId = binding->parent;
        final.subrequestId = binding->subrequest;
        if (!Admission(operation) && binding->revision != UINT32_MAX) ++binding->revision;
        final.record.parameterRevision = binding->revision;
        final.record.opcode = Admission(operation) ? Op::Begin :
            (operation == Operation::Cancel ? Op::Cancel : Op::Update);
    } else if (Admission(operation) && accepted) final.coverage = Coverage::Capacity;
    if (scoped && binding != nullptr) ++counters_.attributed;

    // An unscoped callback can still refer to an existing incarnation. Its
    // copied observation is useful, but OutsideProducer remains explicit.
    if (!scoped) {
        // Keep diagnostics for out-of-scope mutations but never turn an
        // admission with no producer into an apparently complete command.
        if (Admission(operation)) {
            final.record.opcode = Op::CoverageLost; final.record.commandId = 0;
            final.record.parentCommandId = 0; final.record.parameterRevision = 0;
        }
    }
    for (auto& pending : pending_) if (!pending.used) {
        pending.observation = final; pending.request = request;
        pending.frame = scoped ? frame->token : 0; pending.used = true; pending.ready = !scoped;
        FlushReady();
        return;
    }
    ++counters_.overflow;
}
void CaptureState::FlushReady() noexcept {
    for (;;) {
        Pending* first = nullptr;
        for (auto& pending : pending_) if (pending.used &&
            (first == nullptr || pending.observation.rawSequence < first->observation.rawSequence)) first = &pending;
        if (first == nullptr || !first->ready) return;
        auto observation = first->observation; first->used = false;
        Emit(Stream::Finalized, observation);
        if (observation.record.opcode == Op::Begin) {
            observation.record.opcode = Op::Submit;
            Emit(Stream::Finalized, observation);
        }
    }
}
void CaptureState::End(const std::uint64_t token, const FinalReader reader, void* user) noexcept {
    if (token == 0) { if (blockedDepth_ != 0) --blockedDepth_; return; }
    if (depth_ == 0 || frames_[depth_ - 1].token != token) {
        ++counters_.overflow; return;
    }
    const auto frame = frames_[--depth_];
    for (auto& pending : pending_) if (pending.used && pending.frame == token) {
        NativeSample sample{}; std::uint32_t role{};
        auto& observation = pending.observation;
        bool currentIncarnation = false;
        for (const auto& binding : requests_) if (binding.used && binding.address == pending.request &&
            binding.context == frame.context && binding.command == observation.record.commandId) {
            currentIncarnation = true; break;
        }
        // Cancel/flush can have released the object. Never read it afterward.
        if (pending.request == 0 || observation.operation == Operation::Cancel ||
            observation.operation == Operation::Flush) {
            observation.coverage = Coverage::UnsupportedParameters;
        } else if (!currentIncarnation || reader == nullptr ||
            !reader(user, frame.state, pending.request, sample, role)) {
            observation.coverage = Coverage::MissingLifetime; ++counters_.missingLifetime;
            // Do not label the pre-submit sample as finalized parameters.
            observation.record.flags &= ~(HasGoal | HasGait | HasStopDistance);
            observation.record.goalX = 0; observation.record.goalY = 0; observation.record.goalZ = 0;
            observation.record.gait = 0; observation.record.stopDistance = 0;
        } else {
            observation.record.flags &= ~(HasGoal | HasGait | HasStopDistance);
            CopySample(observation.record, sample);
            if (!sample.known) observation.coverage = Coverage::UnknownRequest;
            else if (!sample.complete) observation.coverage = Coverage::UnsupportedParameters;
        }
        // role is only a local lifetime-proof classification (1 main,2 companion).
        // subrequestId is a portable monotonically assigned incarnation, not role.
        pending.ready = true;
    }
    FlushReady();
}

namespace {
constexpr std::size_t kQueueCapacity = 1024;
struct Queued { Observation observation{}; Stream stream{}; };
std::array<Queued, kQueueCapacity> gQueue{};
// Bootstrap diagnostics and IPC snapshot requests may drain concurrently.
// Only consumers take this mutex; the engine producer remains non-blocking.
std::mutex gDiagnosticDrainMutex;
std::atomic<std::uint64_t> gWrite{}, gRead{}, gQueueDropped{}, gSkipped{}, gUnbound{};
// Separate value-only SPSC ring: diagnostics never consume IPC observations.
// Neither indices nor cumulative loss counters are reset by engine Reset.
std::array<TransportEnvelope, kQueueCapacity> gTransportQueue{};
std::atomic<std::uint64_t> gTransportWrite{}, gTransportRead{}, gTransportDropped{},
    gTransportDiscarded{}, gExportDropped{};
std::atomic<bool> gTransportEnabled{};
bool gTransportPeeked{}; // IPC consumer only, including lifecycle discards.
std::array<std::atomic<std::uint64_t>, 11> gCounts{};
std::atomic<bool> gEnabled{}, gInstalled{};
std::atomic<GuardCallback> gGuard{};
std::atomic<ContextCallback> gContext{};
std::uintptr_t gBase{};
CaptureState gState{};
bool gAttempted{};
// Published originals are NEVER cleared on Reset or rollback: a detour can
// still be returning through a retained trampoline. Owner controls quiescence.
std::array<std::atomic<void*>, 7> gOriginal{};
std::array<void*, 7> gTrampolines{};
thread_local std::uint32_t gProducerDepth{};

void Publish() noexcept {
    const auto s = gState.Snapshot();
    const std::array<std::uint64_t, 11> values{s.captureEpoch, s.producers, s.oracle,
        s.finalized, s.attributed, s.outsideProducer, s.unknown, s.missingLifetime,
        s.overflow, s.resets, 1};
    for (std::size_t i = 0; i < values.size(); ++i) gCounts[i].store(values[i], std::memory_order_release);
}
void Queue(void*, const Stream stream, const Observation& observation) noexcept {
    const auto write = gWrite.load(std::memory_order_relaxed);
    if (write - gRead.load(std::memory_order_acquire) >= kQueueCapacity) {
        gQueueDropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        gQueue[write % kQueueCapacity] = {observation, stream};
        gWrite.store(write + 1, std::memory_order_release);
    }
    if (!gTransportEnabled.load(std::memory_order_acquire)) return;
    const auto transportWrite = gTransportWrite.load(std::memory_order_relaxed);
    if (transportWrite - gTransportRead.load(std::memory_order_acquire) >= kQueueCapacity) {
        gTransportDropped.fetch_add(1, std::memory_order_relaxed); return;
    }
    // Codec failure is an explicit export loss, never a permanently stuck head.
    // Only copied values are inspected; no consumer needs a native lifetime.
    TransportEnvelope bytes{};
    if (!EncodeTransport(stream, observation, 0, bytes)) {
        gExportDropped.fetch_add(1, std::memory_order_relaxed); return;
    }
    gTransportQueue[transportWrite % kQueueCapacity] = bytes;
    gTransportWrite.store(transportWrite + 1, std::memory_order_release);
}
bool ExactOptIn(const char* name) noexcept {
    char enabled[4]{};
    return GetEnvironmentVariableA(name, enabled, sizeof(enabled)) == 1 && enabled[0] == '1';
}
bool Guarded() noexcept {
    if (!gEnabled.load(std::memory_order_acquire)) return false;
    const auto guard = gGuard.load(std::memory_order_acquire);
    if (guard == nullptr || !guard()) { gSkipped.fetch_add(1); return false; }
    return true;
}
template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    return address != 0 && crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(value));
}
bool Context(const std::uintptr_t character, CaptureContext& context) noexcept {
    const auto callback = gContext.load(std::memory_order_acquire);
    if (character == 0 || callback == nullptr || !callback(character, context)) {
        gUnbound.fetch_add(1); return false;
    }
    Record proof{}; proof.scope = context.scope; proof.owner = context.owner;
    proof.opcode = Op::Oracle; proof.flags = kObservation; proof.globalSeq = 1; proof.ownerSeq = 1;
    if (Validate(proof) != Result::Ok) { gUnbound.fetch_add(1); return false; }
    return true;
}
bool Sample(const std::uintptr_t request, NativeSample& sample, std::uintptr_t& character) noexcept {
    sample = {}; character = 0;
    std::uintptr_t scheduler{}, type{};
    if (!Read(request, type) || !Read(request + 0x10, sample.kind) ||
        !Read(request + 0xC, sample.status) || !Read(request + 0x14, scheduler) ||
        !Read(scheduler, character) || character == 0 || scheduler != character + 0x2CD0) return false;
    // Only point Move has typed fields here. Other admissions still reach the
    // oracle and are marked unknown rather than silently disappearing.
    if (sample.kind == 1 && type == gBase + 0xEB1A70) {
        std::uint8_t flags{};
        if (!Read(request + 0xB8, flags)) return true;
        const auto goal = request + ((flags & 0x40) != 0 ? 0x34 : 0x28);
        if (!crash_guard::TryCopy(sample.goal, reinterpret_cast<const void*>(goal), sizeof(sample.goal)) ||
            !Read(request + 0x54, sample.stopDistance)) return true;
        sample.known = true; sample.hasGoal = true; sample.hasStopDistance = true;
        // Gait is M+194. R+78 is explicitly NOT exported as gait or opaque int.
        // Callback-dependent variants and other R fields are not certified.
        sample.complete = false;
    }
    return true;
}
void ObserveNative(const Operation op, const std::uintptr_t request, const bool accepted) noexcept {
    if (!Guarded()) return;
    NativeSample sample{}; std::uintptr_t character{};
    if (!Sample(request, sample, character)) { gUnbound.fetch_add(1); return; }
    CaptureContext context{};
    if (!Context(character, context)) return;
    gState.Observe(op, context, request, sample, accepted); Publish();
}
bool ReadFinal(void* user, const std::uintptr_t state, const std::uintptr_t request,
    NativeSample& sample, std::uint32_t& role) noexcept {
    if (!Guarded() || state == 0) return false;
    // Static lifetime evidence: 0040C850 copies state+2C/+30, 006F8490
    // state+34/+38, retaining their controls. Read only while the producer's
    // caller still owns state and the exact request is held by that state.
    std::uintptr_t primary{}, primaryControl{}, companion{}, companionControl{};
    if (!Read(state + 0x2C, primary) || !Read(state + 0x30, primaryControl) ||
        !Read(state + 0x34, companion) || !Read(state + 0x38, companionControl)) return false;
    role = request == primary && primaryControl != 0 ? 1U :
        (request == companion && companionControl != 0 ? 2U : 0U);
    if (role == 0) return false;
    std::uintptr_t character{};
    CaptureContext now{};
    return Sample(request, sample, character) && user != nullptr &&
        Context(character, now) && now == *static_cast<const CaptureContext*>(user);
}
struct ProducerScope {
    CaptureContext context{};
    std::uintptr_t character{};
    std::uint64_t token{}, epoch{};
    bool attempted{}, bound{};
    ProducerScope(Producer producer, std::uintptr_t actor, std::uintptr_t state) noexcept : character(actor) {
        ++gProducerDepth;
        if (!Guarded()) return;
        bound = Context(actor, context);
        epoch = gState.Snapshot().captureEpoch;
        // An unbound reentrant producer masks its bound parent. Otherwise an
        // admission performed by the unbound producer could inherit the wrong
        // transaction solely because its callback refers to the parent actor.
        token = gState.Begin(bound ? producer : Producer::Unknown,
            bound ? context : CaptureContext{}, bound ? state : 0);
        attempted = true; Publish();
    }
    ~ProducerScope() noexcept {
        if (attempted && gState.Snapshot().captureEpoch == epoch) {
            CaptureContext now{};
            if (!bound) gState.End(token, nullptr, nullptr);
            else if (Guarded() && Context(character, now) && context == now) gState.End(token, &ReadFinal, &context);
            else gState.Reset();
            Publish();
        }
        --gProducerDepth;
    }
};

// 0070A940: ECX=action; seven DWORD stack slots; every return RET 1C.
// Use DWORD carriers for ALL original stack bits, including byte/float slots.
using MoveFn = std::uint32_t(__thiscall*)(void*, std::uintptr_t, std::uintptr_t,
    std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
std::uint32_t __fastcall MoveDetour(void* action, void*, std::uintptr_t character,
    std::uintptr_t state, std::uint32_t p4, std::uint32_t p5, std::uint32_t p6,
    std::uint32_t p7, std::uint32_t p8) noexcept {
    const auto original = reinterpret_cast<MoveFn>(gOriginal[0].load(std::memory_order_acquire));
    if (original == nullptr) return 0;
    ProducerScope scope(Producer::MoveToTarget, character, state);
    host_producer::BeginNativeMove(character,state);
    const auto result=original(action, character, state, p4, p5, p6, p7, p8);
    host_producer::EndNativeMove();return result;
}
// 0070B140: three STACK args, RET C; ECX/EDX are not inputs.
using StopFn = void(__stdcall*)(std::uintptr_t, std::uintptr_t, std::uint32_t);
void __stdcall StopDetour(std::uintptr_t character, std::uintptr_t state, std::uint32_t reason) noexcept {
    const auto original = reinterpret_cast<StopFn>(gOriginal[1].load(std::memory_order_acquire));
    if (original == nullptr) return;
    ProducerScope scope(Producer::Stop, character, state);
    original(character, state, reason);
}
using AdmissionFn = std::uint32_t(__thiscall*)(void*);
std::uint32_t __fastcall AppendDetour(void* request, void*) noexcept {
    const auto original = reinterpret_cast<AdmissionFn>(gOriginal[2].load(std::memory_order_acquire));
    if (original == nullptr) return 0;
    host_producer::BeforeNativeAdmission(reinterpret_cast<std::uintptr_t>(request),true);
    const auto result = original(request);
    ObserveNative(Operation::Append, reinterpret_cast<std::uintptr_t>(request), (result & 0xFF) != 0);
    host_producer::ObserveNativeAdmission(reinterpret_cast<std::uintptr_t>(request),(result & 0xFF) != 0);
    return result;
}
std::uint32_t __fastcall FrontDetour(void* request, void*) noexcept {
    const auto original = reinterpret_cast<AdmissionFn>(gOriginal[3].load(std::memory_order_acquire));
    if (original == nullptr) return 0;
    host_producer::BeforeNativeAdmission(reinterpret_cast<std::uintptr_t>(request),2);
    const auto result = original(request);
    ObserveNative(Operation::Front, reinterpret_cast<std::uintptr_t>(request), (result & 0xFF) != 0);
    host_producer::ObserveNativeAdmission(reinterpret_cast<std::uintptr_t>(request),(result & 0xFF) != 0);
    return result;
}
using VoidFn = void(__thiscall*)(void*);
void __fastcall CancelDetour(void* request, void*) noexcept {
    const auto original = reinterpret_cast<VoidFn>(gOriginal[4].load(std::memory_order_acquire));
    if (original == nullptr) return;
    ObserveNative(Operation::Cancel, reinterpret_cast<std::uintptr_t>(request), true);
    original(request);
}
void __fastcall FlushDetour(void* scheduler, void*) noexcept {
    const auto original = reinterpret_cast<VoidFn>(gOriginal[5].load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (Guarded()) {
        std::uintptr_t character{}; CaptureContext context{};
        if (Read(reinterpret_cast<std::uintptr_t>(scheduler), character) &&
            reinterpret_cast<std::uintptr_t>(scheduler) == character + 0x2CD0 && Context(character, context)) {
            gState.Observe(Operation::Flush, context, 0, {}, true); Publish();
        }
    }
    original(scheduler);
}
using StopUpdateFn = void(__thiscall*)(void*, std::uint32_t, std::uint32_t);
void __fastcall StopUpdateDetour(void* request, void*, std::uint32_t distance, std::uint32_t mode) noexcept {
    const auto original = reinterpret_cast<StopUpdateFn>(gOriginal[6].load(std::memory_order_acquire));
    if (original == nullptr) return;
    host_producer::BeginNativeMutation();
    original(request, distance, mode);
    host_producer::EndNativeMutation();
    host_producer::ObserveNativeMutation(reinterpret_cast<std::uintptr_t>(request));
    ObserveNative(Operation::StopUpdate, reinterpret_cast<std::uintptr_t>(request), true);
}
using GoalUpdateFn = void(__thiscall*)(void*, const void*);
void __fastcall GoalUpdateDetour(void* request, void*, const void* goal) noexcept {
    // Certified direct CALL redirection, so the unmodified target is original.
    const auto original = reinterpret_cast<GoalUpdateFn>(gBase + 0x2C7550);
    host_producer::BeginNativeMutation();
    original(request, goal);
    host_producer::EndNativeMutation();
    host_producer::ObserveNativeMutation(reinterpret_cast<std::uintptr_t>(request));
    ObserveNative(Operation::GoalUpdate, reinterpret_cast<std::uintptr_t>(request), true);
}
}

bool Install(const std::uintptr_t base, HookManager& hooks,
    const ContextCallback context, const GuardCallback guard, std::string& error) {
    if (!ExactOptIn("AIMP_AISYNC_TRANSACTION_SHADOW")) return true;
    if (gAttempted) { error = "transaction shadow install already attempted; retained trampolines require quiescence"; return false; }
    if (base == 0 || context == nullptr || guard == nullptr || !hooks.MutationAllowed()) {
        error = "transaction shadow requires exact-build optional hook manager and context/guard"; return false;
    }
    gAttempted = true; gBase = base;
    gContext.store(context, std::memory_order_release); gGuard.store(guard, std::memory_order_release);
    gState.Configure(&Queue, nullptr); gState.Reset(); Publish();
    const auto before = hooks.InstalledCount();
    const auto hook = [&](const char* name, std::uintptr_t rva, void* detour,
        std::initializer_list<std::uint8_t> prefix, std::size_t index) {
        HookRequest request{}; request.name = name;
        request.target = reinterpret_cast<void*>(base + rva); request.detour = detour;
        for (const auto b : prefix) { request.expected.bytes.push_back(b); request.expected.mask.push_back('x'); }
        // PUSH absolute SEH handler in Move/Flush is relocated by the PE loader.
        if (index == 0 || index == 5) {
            const auto handler = static_cast<std::uint32_t>(base + (index == 0 ? 0xD29798 : 0xD29758));
            std::memcpy(request.expected.bytes.data() + 3, &handler, sizeof(handler));
        }
        request.patchLength = request.expected.bytes.size(); request.relocationSafePrefix = true;
        request.trampolineOut = &gTrampolines[index]; request.publishedTrampolineOut = &gOriginal[index];
        return hooks.Install(request, error);
    };
    bool ok = false;
    try {
        ok = hook("AISync transaction Move producer", 0x30A940, reinterpret_cast<void*>(&MoveDetour), {0x6A,0xFF,0x68,0,0,0,0}, 0) &&
            hook("AISync transaction Stop producer", 0x30B140, reinterpret_cast<void*>(&StopDetour), {0x55,0x8B,0xEC,0x83,0xE4,0xF8}, 1) &&
            hook("AISync transaction Append oracle", 0x283C10, reinterpret_cast<void*>(&AppendDetour), {0x64,0xA1,0,0,0,0}, 2) &&
            hook("AISync transaction Front oracle", 0x283550, reinterpret_cast<void*>(&FrontDetour), {0x64,0xA1,0,0,0,0}, 3) &&
            hook("AISync transaction Cancel oracle", 0x283DB0, reinterpret_cast<void*>(&CancelDetour), {0x64,0xA1,0,0,0,0}, 4) &&
            hook("AISync transaction Flush oracle", 0x283A90, reinterpret_cast<void*>(&FlushDetour), {0x6A,0xFF,0x68,0,0,0,0}, 5) &&
            hook("AISync transaction Stop update", 0x27D0D0, reinterpret_cast<void*>(&StopUpdateDetour), {0xF3,0x0F,0x10,0x49,0x58}, 6);
        if (ok) for (const auto site : {0x30AAEAU, 0x362417U}) {
            CallRedirectRequest call{"AISync transaction Goal update", reinterpret_cast<void*>(base + site),
                reinterpret_cast<void*>(base + 0x2C7550), reinterpret_cast<void*>(&GoalUpdateDetour)};
            if (!hooks.InstallCallRedirect(call, error)) { ok = false; break; }
        }
    } catch (...) { ok = false; error = "transaction shadow optional installation threw"; }
    if (!ok) {
        std::string rollback;
        if (!hooks.UninstallFrom(before, rollback)) error += "; shadow rollback failed: " + rollback;
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    gTransportEnabled.store(ExactOptIn("AIMP_AISYNC_TRANSACTION_TRANSPORT_SHADOW"), std::memory_order_release);
    gEnabled.store(true, std::memory_order_release);
    return true;
}
void ObserveSubmit(const std::uintptr_t request, const bool accepted) noexcept {
    ObserveNative(Operation::Replace, request, accepted);
}
bool HasAdmissionHooks(std::uintptr_t base) noexcept {
    return gInstalled.load(std::memory_order_acquire) && gBase == base;
}
void Reset() noexcept {
    if (!gEnabled.load(std::memory_order_acquire)) return;
    gState.Reset(); Publish();
}
Counters DiagnosticsSnapshot() noexcept {
    Counters s{};
    s.captureEpoch = gCounts[0].load(); s.producers = gCounts[1].load(); s.oracle = gCounts[2].load();
    s.finalized = gCounts[3].load(); s.attributed = gCounts[4].load(); s.outsideProducer = gCounts[5].load();
    s.unknown = gCounts[6].load(); s.missingLifetime = gCounts[7].load(); s.overflow = gCounts[8].load();
    s.resets = gCounts[9].load(); s.skippedGuard = gSkipped.load(); s.unbound = gUnbound.load();
    s.queueDropped = gQueueDropped.load(); s.enabled = gEnabled.load(); s.installed = gInstalled.load();
    s.transportDropped = gTransportDropped.load(); s.transportDiscarded = gTransportDiscarded.load();
    s.exportDropped = gExportDropped.load(); s.transportEnabled = gTransportEnabled.load();
    s.lossCount = AddLoss(AddLoss(AddLoss(AddLoss(s.overflow, s.queueDropped),
        s.transportDropped), s.transportDiscarded), s.exportDropped);
    s.coverageIncomplete = true;
    return s;
}
bool PeekTransport(TransportEnvelope& output) noexcept {
    const auto read = gTransportRead.load(std::memory_order_relaxed);
    if (read == gTransportWrite.load(std::memory_order_acquire)) {
        gTransportPeeked = false; return false;
    }
    output = gTransportQueue[read % kQueueCapacity];
    PutLE(output, 48, DiagnosticsSnapshot().lossCount);
    gTransportPeeked = true;
    return true;
}
void CommitTransport() noexcept {
    if (!gTransportPeeked) return;
    const auto read = gTransportRead.load(std::memory_order_relaxed);
    gTransportRead.store(read + 1, std::memory_order_release);
    gTransportPeeked = false;
}
void DiscardTransport() noexcept {
    const auto read = gTransportRead.load(std::memory_order_relaxed);
    const auto write = gTransportWrite.load(std::memory_order_acquire);
    gTransportDiscarded.fetch_add(write - read, std::memory_order_relaxed);
    gTransportRead.store(write, std::memory_order_release);
    gTransportPeeked = false;
}
std::string DrainJSONL(const std::size_t requested) {
    const std::lock_guard drainLock(gDiagnosticDrainMutex);
    const auto limit = (std::min)(requested, std::size_t{128});
    std::ostringstream output;
    auto read = gRead.load(std::memory_order_relaxed);
    const auto end = gWrite.load(std::memory_order_acquire);
    std::size_t emitted{};
    constexpr char hex[] = "0123456789abcdef";
    const auto lossFields = [&]() {
        const auto loss = DiagnosticsSnapshot();
        output << ",\"captureOverflow\":" << loss.overflow
            << ",\"queueDropped\":" << loss.queueDropped
            << ",\"transportDropped\":" << loss.transportDropped
            << ",\"transportDiscarded\":" << loss.transportDiscarded
            << ",\"exportDropped\":" << loss.exportDropped
            << ",\"lossCount\":" << loss.lossCount;
    };
    while (read != end && emitted < limit) {
        const auto item = gQueue[read % kQueueCapacity];
        gRead.store(++read, std::memory_order_release);
        CanonicalRecord bytes{};
        if (Encode(item.observation.record, bytes) != Result::Ok) {
            // Invalid records must be visible, just like bounded queue loss.
            gQueueDropped.fetch_add(1, std::memory_order_relaxed);
            output << "{\"type\":\"aisync_transaction_shadow\",\"coverageIncomplete\":true,"
                "\"reason\":\"canonical_encoding_failed\",\"captureEpoch\":"
                << item.observation.captureEpoch << ",\"rawSequence\":"
                << item.observation.rawSequence;
            lossFields(); output << "}\n";
            ++emitted; continue;
        }
        const auto& observation = item.observation;
        output << "{\"type\":\"aisync_transaction_shadow\",\"stream\":\""
            << (item.stream == Stream::Oracle ? "oracle" : "finalized") << "\",\"captureEpoch\":"
            << observation.captureEpoch << ",\"rawSequence\":" << observation.rawSequence
            << ",\"subrequestId\":" << observation.subrequestId
            << ",\"producer\":" << static_cast<std::uint32_t>(observation.producer)
            << ",\"operation\":" << static_cast<std::uint32_t>(observation.operation)
            << ",\"coverage\":" << static_cast<std::uint32_t>(observation.coverage)
            << ",\"coverageIncomplete\":true,\"canonical\":\"";
        for (const auto byte : bytes) output << hex[byte >> 4] << hex[byte & 15];
        output << '"'; lossFields(); output << "}\n"; ++emitted;
    }
    return output.str();
}
} // namespace aimp::ai_sync::transaction_capture
