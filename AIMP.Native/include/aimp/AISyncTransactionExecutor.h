#pragma once

#include "aimp/AISyncTransactions.h"

namespace aimp::ai_sync::transactions {

enum class Dependency : std::uint8_t { Ready, Waiting, Reject };

// Inject a real adapter on the qualified engine thread. No default engine
// implementation exists. Check must validate the typed payload, bindings and
// lifetimes for this exact operation; Ready confers no authority on other calls.
// Invoke must resolve resources by the full scope/owner/command incarnation,
// never by "current command". Both callbacks are synchronous and borrowed;
// context must outlive the executor. Callbacks may throw C++ exceptions; the
// driver contains them. Platform faults require the adapter's own crash guard.
struct TransactionAdapter {
    void* context{};
    Dependency (*checkDependencies)(void*, const Record&){};
    NativeOutcome (*invoke)(void*, const Record&){};
};

enum class ExecutionStatus : std::uint8_t {
    Refused, Metadata, Waiting, DependencyRejected, NativeEntered, NativeFinished,
    OutcomeUnknown, Superseded, Reentrant, Blocked, AdapterUnavailable,
    CallbackError
};

struct ExecutionDisposition {
    ExecutionStatus status{ExecutionStatus::Refused};
    Result result{Result::InvalidRecord};
    bool acceptedLogical{};
    bool nativeEntered{};
    bool hasNativeOutcome{};
    NativeOutcome nativeOutcome{NativeOutcome::Unknown};
    bool duplicate{};
};

enum class ExecutionBarrier : std::uint8_t {
    RequireReleased,
    // Explicit external proof: the caller has retired all affected native
    // resources/workers/callbacks, or destroyed their world, before this call.
    // This is not a request to perform cleanup, nor permission to skip it.
    ExternalQuiescence
};

// Single-threaded driver, disconnected from gameplay until an adapter is
// explicitly injected. All calls, reads and destruction belong to one thread;
// the executor/context must not be destroyed from a callback. No locks are
// held and no ledger mutation is in progress during either callback. Public
// mutation reentry is refused throughout callbacks, including configuration.
//
// Default strict mode: one exact Waiting operation is retained per owner; independent owners can
// progress in global admission order. Execute duplicates only report it.
// Retry is explicit. Later operations for that owner are blocked, except Cancel
// of that same command (which supersedes the waiting token after acceptance).
// A changed-scope Configure can abandon unentered waiting work. Same-scope
// Configure never resets evidence. Unknown is never retryable, and native
// ownership blocks replacement/default barriers until Release is admitted.
// Complete changes authority state only; it never calls the engine.
//
// nativePipeline=true preserves multiple native queue admissions and dependent
// Waiting revisions. Explicit Retry respects their causal order; a terminal
// operation supersedes unentered Submit/Update work without invoking it.
//
// Limits bound the aggregate array storage of driver AND Ledger; allocation
// happens only at construction. Counts include tombstones. An authenticated
// Checkpoint may retire a settled prefix while preserving live native commands,
// sequence watermarks and a monotone floor for retired command IDs. No implicit eviction.
// Retained duplicates remain reportable after capacity closes new admission;
// retrying Waiting remains prohibited until a changed-scope barrier.
class TransactionExecutor final {
public:
    explicit TransactionExecutor(TransactionAdapter adapter, Limits limits = {}, bool nativePipeline = false) noexcept;
    ~TransactionExecutor();
    TransactionExecutor(const TransactionExecutor&) = delete;
    TransactionExecutor& operator=(const TransactionExecutor&) = delete;

    [[nodiscard]] Result Configure(const Scope& scope,
        ExecutionBarrier barrier = ExecutionBarrier::RequireReleased) noexcept;
    [[nodiscard]] Result FenceOwner(const OwnerKey& owner,
        ExecutionBarrier barrier = ExecutionBarrier::RequireReleased) noexcept;
    [[nodiscard]] ExecutionDisposition Execute(const Record& operation) noexcept;
    [[nodiscard]] ExecutionDisposition Retry(const Record& exactOperation) noexcept;
    [[nodiscard]] bool GetDisposition(const Record& exactOperation,
        ExecutionDisposition& output) const noexcept;
    // Retained authenticated operation, including its executable extension.
    // Lookup never fabricates a request from a snapshot's partial reference.
    [[nodiscard]] bool ReadOperation(std::uint64_t globalSeq, Record&, ExecutionDisposition&) const noexcept;
    [[nodiscard]] Result OperationReadiness(const Record&) noexcept;
    [[nodiscard]] std::uint64_t HistoryFloor() const noexcept { return ledger_.HistoryFloor(); }
    [[nodiscard]] bool Find(const Record& identity, CommandState& output) const noexcept;
    [[nodiscard]] bool OwnerOutcomeUnknown(const OwnerKey&) const noexcept;
    [[nodiscard]] Result Checkpoint(const Scope&, std::uint64_t through) noexcept;
    [[nodiscard]] bool CoverageLost() const noexcept;
    [[nodiscard]] bool InvocationInFlight() const noexcept { return invoking_; }
    [[nodiscard]] bool HasWaiting() const noexcept { return WaitingCount() != 0; }
    [[nodiscard]] bool HasWaiting(const OwnerKey&) const noexcept;
    [[nodiscard]] std::size_t WaitingCount() const noexcept;
    [[nodiscard]] bool HasNativeOwnership(const OwnerKey& owner) const noexcept { return Unreleased(&owner); }
    [[nodiscard]] std::size_t RecordCount() const noexcept { return recordCount_; }
    [[nodiscard]] std::size_t AllocatedBytes() const noexcept { return allocatedBytes_; }

private:
    struct Operation;
    struct Command;
    static constexpr std::size_t noIndex = static_cast<std::size_t>(-1);
    [[nodiscard]] static Limits LedgerLimits(Limits limits) noexcept;
    [[nodiscard]] std::size_t FindOperation(const CanonicalRecord& bytes,
        std::uint64_t globalSeq) const noexcept;
    [[nodiscard]] std::size_t CurrentCommand(const OwnerKey& owner) const noexcept;
    [[nodiscard]] bool Unreleased(const OwnerKey* owner = nullptr) const noexcept;
    [[nodiscard]] ExecutionDisposition Dispatch(std::size_t index) noexcept;
    [[nodiscard]] ExecutionDisposition Remembered(std::size_t index) const noexcept;

    Ledger ledger_;
    const TransactionAdapter adapter_;
    Limits limits_{};
    std::unique_ptr<Operation[]> operations_;
    std::unique_ptr<Command[]> commands_;
    Scope scope_{};
    std::size_t recordCount_{};
    std::size_t commandCount_{};
    std::size_t allocatedBytes_{};
    bool configured_{};
    bool storageReady_{};
    bool busy_{};
    bool invoking_{};
    bool nativePipeline_{};
};

} // namespace aimp::ai_sync::transactions
