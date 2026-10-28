#pragma once

#include "aimp/AISyncTransactions.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::ai_sync::transaction_capture {

// P1 local diagnostic identities only. A cookie plus a local namespace is NOT
// a full network session GUID. Every emitted record is ObservationOnly; these
// records must never be sent as P2 authoritative commands. The optional P2
// observation transport preserves this local identity for Launcher validation.
struct CaptureContext {
    transactions::Scope scope{};
    transactions::OwnerKey owner{};
    bool operator==(const CaptureContext&) const noexcept = default;
};
using ContextCallback = bool(*)(std::uintptr_t character, CaptureContext&) noexcept;
using GuardCallback = bool(*)() noexcept;

enum class Producer : std::uint32_t { Unknown, MoveToTarget, Stop };
enum class Operation : std::uint32_t {
    Replace = 1, Append, Front, Cancel, Flush, GoalUpdate, StopUpdate
};
enum class Coverage : std::uint32_t {
    Attributed, OutsideProducer, UnknownRequest, MissingLifetime,
    Capacity, Unbound, ScopeChanged, UnsupportedParameters
};
enum class Stream : std::uint32_t { Oracle, Finalized };

// All fields are copied values with certified meanings. No R+78, callbacks,
// Ref/control pointers, opaque object bytes or native object identity escape.
struct NativeSample {
    std::uint32_t kind{}, status{}, gait{};
    float goal[3]{}, stopDistance{};
    bool known{}, hasGoal{}, hasGait{}, hasStopDistance{};
    // Full semantic certification is deliberately separate from known fields.
    bool complete{};
};
struct Observation {
    transactions::Record record{};
    std::uint64_t rawSequence{}, captureEpoch{}, subrequestId{};
    Producer producer{};
    Operation operation{};
    Coverage coverage{};
};
using ObservationSink = void(*)(void*, Stream, const Observation&) noexcept;

inline constexpr std::uint32_t kTransportMagic = 0x4F544941U; // ASCII AITO
inline constexpr std::uint16_t kTransportVersion = 1U;
inline constexpr std::size_t kTransportEnvelopeSize = 216U;
static_assert(kTransportEnvelopeSize == 56U + transactions::kRecordSize);
using TransportEnvelope = std::array<std::uint8_t, kTransportEnvelopeSize>;
// Explicit LE envelope, never an object-layout copy. Canonical bytes [56,216)
// always carry ObservationOnly|Incomplete, even for a caller-supplied record.
[[nodiscard]] bool EncodeTransport(Stream, const Observation&, std::uint64_t lossCount,
    TransportEnvelope&) noexcept;
// Called at producer return. The reader must prove the request is STILL held
// by this producer state before reading it. Failed proof preserves the raw
// event and emits MissingLifetime, never a guessed finalized payload.
using FinalReader = bool(*)(void*, std::uintptr_t state, std::uintptr_t request,
    NativeSample&, std::uint32_t& subrequestRole) noexcept;

struct Counters {
    std::uint64_t captureEpoch{}, producers{}, oracle{}, finalized{}, attributed{};
    std::uint64_t outsideProducer{}, unknown{}, missingLifetime{}, overflow{};
    std::uint64_t skippedGuard{}, unbound{}, resets{}, queueDropped{};
    // Cumulative module-lifetime loss, including capture overflow and the
    // independent diagnostic/transport queues. Never cleared by Reset.
    std::uint64_t transportDropped{}, transportDiscarded{}, exportDropped{}, lossCount{};
    bool enabled{}, installed{}, transportEnabled{}, coverageIncomplete{true};
};

// Bounded, engine-thread-only model. Tests drive this with fake operations and
// fake lifetime proofs, without loading or executing CATHODE. Sink must not
// reenter this model. Native invocation is always outside capture state work.
class CaptureState final {
public:
    static constexpr std::size_t kDepth = 16, kRequests = 256, kPending = 128, kOwners = 128;
    void Configure(ObservationSink sink, void* user) noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::uint64_t Begin(Producer, const CaptureContext&, std::uintptr_t state) noexcept;
    void Observe(Operation, const CaptureContext&, std::uintptr_t request,
        const NativeSample&, bool accepted = true) noexcept;
    void End(std::uint64_t token, FinalReader, void* user) noexcept;
    [[nodiscard]] Counters Snapshot() const noexcept { return counters_; }
private:
    struct Owner {
        CaptureContext context{};
        std::uint64_t seq[2]{};
        bool used{};
    };
    struct Request {
        CaptureContext context{};
        std::uintptr_t address{};
        std::uint64_t command{}, parent{}, subrequest{};
        std::uint32_t revision{1};
        bool used{};
    };
    struct Frame {
        CaptureContext context{};
        std::uintptr_t state{};
        std::uint64_t token{}, primary{}, parent{};
        Producer producer{};
    };
    struct Pending {
        Observation observation{};
        std::uintptr_t request{};
        std::uint64_t frame{};
        bool used{}, ready{};
    };
    void Emit(Stream, Observation&) noexcept;
    void FlushReady() noexcept;
    Owner* FindOwner(const CaptureContext&) noexcept;
    std::array<Owner, kOwners> owners_{};
    std::array<Request, kRequests> requests_{};
    std::array<Frame, kDepth> frames_{};
    std::array<Pending, kPending> pending_{};
    std::size_t depth_{};
    std::size_t blockedDepth_{};
    std::uint64_t token_{}, command_{}, rawSequence_{}, global_[2]{};
    Counters counters_{};
    ObservationSink sink_{};
    void* sinkUser_{};
};

// Install AFTER mandatory AISync hooks as an optional suffix group. The main
// persistent HookManager may be shared; mandatory hooks precede its checkpoint.
// Default OFF; only exact environment value "1" enables installation. Failure
// rolls back ONLY hooks installed by this call. Trampolines stay published and
// retained until caller proves native detour quiescence, including rollback.
bool Install(std::uintptr_t imageBase, HookManager& shadowHooks,
    ContextCallback context, GuardCallback guard, std::string& error);
// Existing AISync 00683CE0 hook calls this after original, before kind/accepted
// early returns. No second hook of 00683CE0 is installed by this module.
void ObserveSubmit(std::uintptr_t request, bool accepted) noexcept;
// Bootstrap-only: the retained shadow suffix already owns Append/Front and
// forwards their admission facts to the typed producer without promoting any
// observation record. The same HookManager lifetime must cover both modules.
[[nodiscard]] bool HasAdmissionHooks(std::uintptr_t imageBase) noexcept;
// Engine thread on every role/session/world/config/lifetime transition. Reset
// invalidates pending scopes/maps without dereferencing old native objects.
void Reset() noexcept;
Counters DiagnosticsSnapshot() noexcept;
// One serialized consumer, off engine thread. At most 128 records per call;
// each line is an independent JSON object prefixed by no logger decorations.
// Includes stream/rawSequence/captureEpoch/coverage and canonical 160-byte hex.
std::string DrainJSONL(std::size_t maxRecords = 64);

// Independent bounded SPSC queue, enabled at Install only when BOTH exact
// AIMP_AISYNC_TRANSACTION_{SHADOW,TRANSPORT_SHADOW}=1 opt-ins are present.
// One IPC-worker consumer for the entire module lifetime. Peek does not remove
// the head; Commit only after IPC Queue accepts it. Backpressure retains it.
// Reset never rewinds indices or relabels copied old-epoch/session values.
// Loss at offset 48 is a cumulative export-time snapshot; retries may see an
// increased loss count. Neither call reads native objects or invokes hooks.
[[nodiscard]] bool PeekTransport(TransportEnvelope& output) noexcept;
void CommitTransport() noexcept;
// IPC worker only, at connection/session/authority barriers. Discards to an
// acquire snapshot of the producer index and counts every discarded value as
// loss. Does not touch engine capture state or the diagnostic queue.
void DiscardTransport() noexcept;

} // namespace aimp::ai_sync::transaction_capture
