#pragma once

#include "aimp/Common.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace aimp {

enum class GameThreadCommandKind : std::uint8_t {
    Observation,
    CreateDummy,
    RemoveDummy,
    MoveDummy,
    ExecuteGameCommand,
    WorldMutation,
};

enum class DispatchEnqueueResult : std::uint8_t {
    Queued,
    Invalid,
    Duplicate,
    CapacityReached,
    GenerationMismatch,
    CreateAlreadyPending,
    Expired,
};

enum class DispatchCompletion : std::uint8_t {
    Executed,
    TaskFault,
    Expired,
    GenerationInvalidated,
    Cleared,
};

enum class EngineThreadSource : std::uint8_t {
    Unknown,
    MainThreadCharacterProcessing,
    CharacterManagerUpdate,
    CharacterReadinessProducer,
};

enum class EngineThreadBindResult : std::uint8_t {
    Bound,
    AlreadyBound,
    ThreadMismatch,
    Invalid,
};

struct GameThreadDispatcherSnapshot {
    std::uint32_t engineThreadId{};
    EngineThreadSource source{EngineThreadSource::Unknown};
    std::uint64_t firstCallbackPerformanceCounter{};
    std::uint64_t lastCallbackPerformanceCounter{};
    std::uint64_t callbackCount{};
    std::uint32_t mismatchedThreadId{};
    std::uint64_t mismatchCount{};
    std::uint64_t lastExecutionTickMs{};
    std::uint64_t generation{};
    std::size_t pendingCount{};
    bool mainThreadHookInstalled{};
    bool managerUpdateHookInstalled{};
    bool readinessProducerHookInstalled{};
    bool ready{};
};

struct GameThreadCommand {
    std::uint64_t requestId{};
    std::uint64_t levelGeneration{};
    std::uint64_t expiresAtTickMs{};
    GameThreadCommandKind kind{GameThreadCommandKind::Observation};
    std::function<void()> execute;
    std::function<void(DispatchCompletion)> completion;
    // Optional dependency preflight on the engine thread. False keeps the same
    // request/order/deadline pending; execute has not begun and may not be retried.
    std::function<bool()> ready;
};

class GameThreadDispatcher final {
public:
    explicit GameThreadDispatcher(std::size_t capacity = 128) noexcept;

    [[nodiscard]] static constexpr ModuleStatus Status() noexcept {
        return ModuleStatus::PartiallyImplemented;
    }

    // The first valid engine callback wins. Later callbacks are idempotent;
    // a different thread is recorded for diagnostics but never blocks work.
    EngineThreadBindResult BindEngineThread(
        std::uint32_t threadId,
        EngineThreadSource source,
        std::uint64_t performanceCounter) noexcept;
    void SetHookAvailability(
        bool mainThreadInstalled,
        bool managerUpdateInstalled,
        bool readinessProducerInstalled) noexcept;

    void SetGeneration(std::uint64_t generation);
    DispatchEnqueueResult Enqueue(GameThreadCommand command);
    bool Enqueue(std::function<void()> task);
    std::size_t DrainOnEngineThread(std::size_t maximumTasks);
    std::size_t ExpirePending();
    std::size_t CancelForGeneration(std::uint64_t newGeneration);
    void Clear() noexcept;

    [[nodiscard]] bool IsEnabled() const noexcept;
    // Narrow hot-path predicate. Unlike Snapshot(), this never locks the
    // command queue. It retains the same enabled/thread/mismatch policy.
    [[nodiscard]] bool IsCertifiedEngineThread(std::uint32_t threadId) const noexcept;
    [[nodiscard]] std::uint32_t EngineThreadId() const noexcept;
    [[nodiscard]] EngineThreadSource EngineThreadBindingSource() const noexcept;
    [[nodiscard]] GameThreadDispatcherSnapshot Snapshot() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] std::size_t PendingCount() const noexcept;

private:
    void RememberCompletedLocked(std::uint64_t requestId);

    const std::size_t capacity_;
    std::atomic<std::uint32_t> engineThread_{0};
    std::atomic<EngineThreadSource> engineThreadSource_{
        EngineThreadSource::Unknown};
    std::atomic<std::uint64_t> firstCallbackPerformanceCounter_{0};
    std::atomic<std::uint64_t> lastCallbackPerformanceCounter_{0};
    std::atomic<std::uint64_t> callbackCount_{0};
    std::atomic<std::uint32_t> mismatchedThread_{0};
    std::atomic<std::uint64_t> mismatchCount_{0};
    std::atomic<std::uint64_t> lastExecutionTickMs_{0};
    std::atomic<bool> mainThreadHookInstalled_{false};
    std::atomic<bool> managerUpdateHookInstalled_{false};
    std::atomic<bool> readinessProducerHookInstalled_{false};
    std::atomic<bool> enabled_{false};
    std::atomic_flag draining_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> internalRequestId_{1};
    mutable std::mutex mutex_;
    std::deque<GameThreadCommand> tasks_;
    std::unordered_set<std::uint64_t> pendingIds_;
    std::unordered_set<std::uint64_t> completedIds_;
    std::deque<std::uint64_t> completedOrder_;
    std::uint64_t generation_{};
    bool createPending_{};
    bool createExecuting_{};
};

[[nodiscard]] std::string_view ToString(EngineThreadSource source) noexcept;

} // namespace aimp
