#pragma once
#include "aimp/AISyncSchedulerControl.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include "aimp/AISyncMovement.h"
#include "aimp/AISyncNativeIntent.h"

namespace aimp::ai_sync::transactions {
// Queue controls and kind8 placement have effects even if they finish before the next snapshot.
// Like direct native calls, their admission/retention operations must not be
// discarded as an obsolete movement goal. This predicate grants no capability;
// executable payloads and profiles are still validated separately.
[[nodiscard]] constexpr bool PreserveNativeOperations(std::uint32_t kind) noexcept {
    return kind == 2U || kind == 5U || kind == 8U || kind >= 0x100U || scheduler_control::Supported(kind);
}

inline constexpr std::uint32_t kMagic = 0x58544941U;
inline constexpr std::uint16_t kVersion = 1U;
inline constexpr std::size_t kRecordSize = 160U;
using CanonicalRecord = std::array<std::uint8_t, kRecordSize>;
using SessionId = std::array<std::uint8_t, 16>;

enum class Op : std::uint32_t {
    Begin = 1, Update, Submit, Cancel, Stop, Complete, Release, Oracle, CoverageLost
};
enum class Capability : std::uint32_t { Movement = 1 };
enum Flag : std::uint32_t {
    HasGoal = 1, HasGait = 2, HasStopDistance = 4, Accepted = 8,
    Incomplete = 16, ObservationOnly = 32
};
inline constexpr std::uint32_t kKnownFlags = 63U;

struct Scope {
    SessionId session{};
    std::uint64_t authorityEpoch{};
    std::uint64_t worldEpoch{};
    std::uint64_t consumerEpoch{};
    bool operator==(const Scope&) const noexcept = default;
};

struct OwnerKey {
    std::uint32_t entityA{};
    std::uint32_t entityB{};
    std::uint32_t birth{};
    std::uint32_t actorClass{};
    Capability capability{Capability::Movement};
    std::uint64_t ownerEpoch{};
    bool operator==(const OwnerKey&) const noexcept = default;
};

// This is a logical record, never a packed/native structure. The codec alone
// defines the fixed LE layout. Native diagnostic integers confer no authority.
struct Record {
    Scope scope{};
    OwnerKey owner{};
    std::uint64_t commandId{};
    std::uint64_t parentCommandId{};
    std::uint64_t globalSeq{};
    std::uint64_t ownerSeq{};
    Op opcode{Op::Begin};
    std::uint32_t parameterRevision{};
    std::uint32_t flags{};
    std::uint32_t nativeKind{};
    std::uint32_t gait{};
    std::uint32_t mode{};
    std::uint32_t nativeStatus{};
    float goalX{};
    float goalY{};
    float goalZ{};
    float distance{};
    float stopDistance{};
    float aux{};
    // AIEX v2 extension. The historical AITX codec remains the 160-byte
    // logical carrier; executable evidence additionally binds these bytes.
    movement::Bundle movement{};
    // AIEX execution directive, outside the AITX logical carrier. Native queue
    // admission: Replace=0, Append=1, Front=2. Scalar retention bits encode
    // before/after admission, default3 preserves the original scalar profile.
    std::uint32_t admission{}, scalarRetention{3};
    native_intent::Payload intent{}; // AIEX v3 only; outside AITX160.
};

enum class Result : std::uint8_t {
    Ok, InvalidRecord, InvalidEncoding, WrongScope, StaleOwner,
    Duplicate, Conflict, GlobalGap, OwnerGap, MissingBegin,
    InvalidTransition, RevisionMismatch, OwnerBusy, ObservationRejected,
    CoverageLost, NativeAlreadyEntered, OutcomeUnknown
};

// Nonzero session/epochs/birth/class/sequences, nonempty entity pair; only
// Oracle/CoverageLost may have commandId=0 and both require ObservationOnly.
// Revision must be nonzero except Oracle/CoverageLost. All six floats finite;
// distance/stopDistance >=0. Diagnostic uint32 fields have no engine bounds.
[[nodiscard]] Result Validate(const Record& record) noexcept;
[[nodiscard]] Result Encode(const Record& record, CanonicalRecord& output) noexcept;
[[nodiscard]] Result Decode(std::span<const std::uint8_t> input, Record& output) noexcept;

struct Limits {
    std::size_t maxRecords{2048};
    std::size_t maxCommands{256};
    std::size_t maxOwners{128};
    std::size_t maxBytes{2U * 1024U * 1024U};
};

// Append-only capture: no application, native entry, coalescing, or implicit
// eviction. Reset explicitly ends a capture window. CoverageLost is sticky.
class Journal final {
public:
    explicit Journal(Limits limits = {}) noexcept;
    [[nodiscard]] Result Append(const Record& record) noexcept;
    [[nodiscard]] bool Read(std::size_t index, Record& output) const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }
    [[nodiscard]] std::size_t Bytes() const noexcept { return size_ * kRecordSize; }
    [[nodiscard]] bool CoverageLost() const noexcept { return coverageLost_; }
    void MarkCoverageLost() noexcept { coverageLost_ = true; }
    void Reset() noexcept;
private:
    std::unique_ptr<CanonicalRecord[]> records_;
    std::size_t capacity_{};
    std::size_t size_{};
    bool coverageLost_{};
};

enum class Phase : std::uint8_t { Begun, Submitted, Cancelled, Stopped, Completed, Released };
enum class NativeOutcome : std::uint8_t { Admitted, Rejected, Unknown };
enum class NativeState : std::uint8_t { NotEntered, Entered, Admitted, Rejected, Unknown, Superseded };
struct CommandState {
    Phase phase{Phase::Begun};
    std::uint32_t parameterRevision{};
    std::uint64_t commandId{};
    std::uint64_t parentCommandId{};
    OwnerKey owner{};
    // Logical Submit is not proof that the native executor admitted a request.
    bool nativeAdmitted{};
    bool nativeReleased{};
    // Replaced by a newer Host intent. Native admission/release evidence stays
    // independent: supersession never releases an already entered request.
    bool superseded{};
    std::uint32_t nativeKind{}; // Retained across checkpoints with native ownership.
};

// Single-threaded portable model, intentionally disconnected from the engine.
// Counts include tombstones and duplicate evidence. Capacity closes coverage.
// Only an explicit settled-prefix Checkpoint or changed-scope barrier can
// retire evidence. Active command state and sequence watermarks survive a checkpoint.
class Ledger final {
public:
    explicit Ledger(Limits limits = {}, bool nativePipeline = false) noexcept;
    ~Ledger();
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;
    [[nodiscard]] Result SetScope(const Scope& scope) noexcept;
    [[nodiscard]] Result FenceOwner(const OwnerKey& owner) noexcept;
    [[nodiscard]] Result Apply(const Record& record) noexcept;
    // EnterNative marks invocation BEFORE the caller makes any native call.
    // The entire accepted record is the token: delayed old-scope tokens fail.
    [[nodiscard]] Result EnterNative(const Record& operation) noexcept;
    // Pipeline eligibility: OwnerBusy is a retained causal wait; Duplicate
    // means explicitly superseded by an authoritative terminal operation.
    [[nodiscard]] Result NativeReadiness(const Record& operation) noexcept;
    [[nodiscard]] Result FinishNative(const Record& operation, NativeOutcome outcome) noexcept;
    // Authenticated settled prefix, not implicit eviction. Live commands and
    // owner/global sequence watermarks survive; discarded tokens cannot enter.
    [[nodiscard]] Result Checkpoint(const Scope&, std::uint64_t through) noexcept;
    [[nodiscard]] std::uint64_t HistoryFloor() const noexcept { return historyFloor_; }
    [[nodiscard]] bool Find(const Record& identity, CommandState& output) const noexcept;
    [[nodiscard]] bool Superseded(std::uint64_t globalSeq) const noexcept;
    [[nodiscard]] bool CoverageLost() const noexcept { return coverageLost_; }
    [[nodiscard]] std::size_t RecordCount() const noexcept { return recordCount_; }
    [[nodiscard]] std::size_t CommandCount() const noexcept { return commandCount_; }
    [[nodiscard]] std::size_t AllocatedBytes() const noexcept { return allocatedBytes_; }
private:
    struct Operation;
    struct Owner;
    [[nodiscard]] Result LoseCoverage() noexcept;
    [[nodiscard]] Operation* FindOperation(const Record& record) noexcept;
    std::unique_ptr<Operation[]> operations_;
    std::unique_ptr<CommandState[]> commands_;
    std::unique_ptr<Owner[]> owners_;
    Limits limits_{};
    Scope scope_{};
    std::size_t recordCount_{};
    std::size_t commandCount_{};
    std::size_t ownerCount_{};
    std::size_t allocatedBytes_{};
    std::uint64_t lastGlobal_{};
    std::uint64_t historyFloor_{};
    bool hasScope_{};
    bool coverageLost_{};
    bool storageReady_{};
    bool nativePipeline_{};
};

} // namespace aimp::ai_sync::transactions
