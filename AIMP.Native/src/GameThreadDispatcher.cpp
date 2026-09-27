#include "aimp/GameThreadDispatcher.h"

#include <Windows.h>

#include <algorithm>
#include <utility>

namespace aimp {
namespace {

void Notify(
    const GameThreadCommand& command,
    const DispatchCompletion completion) noexcept {
    if (!command.completion) {
        return;
    }
    try {
        command.completion(completion);
    } catch (...) {
    }
}

} // namespace

GameThreadDispatcher::GameThreadDispatcher(const std::size_t capacity) noexcept
    : capacity_(capacity) {}

EngineThreadBindResult GameThreadDispatcher::BindEngineThread(
    const std::uint32_t threadId,
    const EngineThreadSource source,
    const std::uint64_t performanceCounter) noexcept {
    if (threadId == 0) {
        return EngineThreadBindResult::Invalid;
    }
    std::uint32_t expected = 0;
    if (engineThread_.compare_exchange_strong(
            expected,
            threadId,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        engineThreadSource_.store(source, std::memory_order_release);
        firstCallbackPerformanceCounter_.store(
            performanceCounter,
            std::memory_order_release);
        lastCallbackPerformanceCounter_.store(
            performanceCounter,
            std::memory_order_release);
        callbackCount_.store(1, std::memory_order_release);
        enabled_.store(true, std::memory_order_release);
        return EngineThreadBindResult::Bound;
    }
    if (expected != threadId) {
        std::uint32_t noMismatch = 0;
        mismatchedThread_.compare_exchange_strong(
            noMismatch,
            threadId,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
        mismatchCount_.fetch_add(1, std::memory_order_acq_rel);
        return EngineThreadBindResult::ThreadMismatch;
    }
    lastCallbackPerformanceCounter_.store(
        performanceCounter,
        std::memory_order_release);
    callbackCount_.fetch_add(1, std::memory_order_acq_rel);
    return EngineThreadBindResult::AlreadyBound;
}

void GameThreadDispatcher::SetHookAvailability(
    const bool mainThreadInstalled,
    const bool managerUpdateInstalled,
    const bool readinessProducerInstalled) noexcept {
    mainThreadHookInstalled_.store(
        mainThreadInstalled,
        std::memory_order_release);
    managerUpdateHookInstalled_.store(
        managerUpdateInstalled,
        std::memory_order_release);
    readinessProducerHookInstalled_.store(
        readinessProducerInstalled,
        std::memory_order_release);
}

void GameThreadDispatcher::SetGeneration(const std::uint64_t generation) {
    if (Generation() == generation) {
        return;
    }
    CancelForGeneration(generation);
}

DispatchEnqueueResult GameThreadDispatcher::Enqueue(
    GameThreadCommand command) {
    if (command.requestId == 0 || !command.execute) {
        return DispatchEnqueueResult::Invalid;
    }
    if (command.expiresAtTickMs != 0 &&
        command.expiresAtTickMs <= GetTickCount64()) {
        return DispatchEnqueueResult::Expired;
    }

    std::lock_guard lock(mutex_);
    if (command.levelGeneration != 0 &&
        command.levelGeneration != generation_) {
        return DispatchEnqueueResult::GenerationMismatch;
    }
    if (pendingIds_.contains(command.requestId) ||
        completedIds_.contains(command.requestId)) {
        return DispatchEnqueueResult::Duplicate;
    }
    if (pendingIds_.size() >= capacity_) {
        return DispatchEnqueueResult::CapacityReached;
    }
    if (command.kind == GameThreadCommandKind::CreateDummy &&
        (createPending_ || createExecuting_)) {
        return DispatchEnqueueResult::CreateAlreadyPending;
    }
    if (command.kind == GameThreadCommandKind::CreateDummy) {
        createPending_ = true;
    }
    pendingIds_.insert(command.requestId);
    tasks_.push_back(std::move(command));
    return DispatchEnqueueResult::Queued;
}

bool GameThreadDispatcher::Enqueue(std::function<void()> task) {
    GameThreadCommand command{};
    command.requestId =
        internalRequestId_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(mutex_);
        command.levelGeneration = generation_;
    }
    command.execute = std::move(task);
    return Enqueue(std::move(command)) == DispatchEnqueueResult::Queued;
}

std::size_t GameThreadDispatcher::DrainOnEngineThread(
    const std::size_t maximumTasks) {
    if (!enabled_.load(std::memory_order_acquire) ||
        GetCurrentThreadId() !=
            engineThread_.load(std::memory_order_acquire)) {
        return 0;
    }

    // A Cathode mutation can invoke another observation hook synchronously.
    // That hook must leave pending commands for this outer drain, not execute
    // another mutation inside the current engine callback.
    if (draining_.test_and_set(std::memory_order_acquire)) return 0;
    struct DrainScope final {
        std::atomic_flag& flag;
        ~DrainScope() { flag.clear(std::memory_order_release); }
    } drainScope{draining_};

    std::size_t completed = 0;
    while (completed < maximumTasks) {
        GameThreadCommand command;
        DispatchCompletion disposition = DispatchCompletion::Executed;
        {
            std::lock_guard lock(mutex_);
            if (tasks_.empty()) {
                break;
            }
            command = std::move(tasks_.front());
            tasks_.pop_front();
            if (command.kind == GameThreadCommandKind::CreateDummy) {
                createPending_ = false;
                createExecuting_ = true;
            }
            if (command.levelGeneration != 0 &&
                command.levelGeneration != generation_) {
                disposition = DispatchCompletion::GenerationInvalidated;
            } else if (command.expiresAtTickMs != 0 &&
                       command.expiresAtTickMs <= GetTickCount64()) {
                disposition = DispatchCompletion::Expired;
            }
        }

        if (disposition == DispatchCompletion::Executed && command.ready) {
            bool ready = false;
            try { ready = command.ready(); }
            catch (...) { disposition = DispatchCompletion::TaskFault; }
            if (!ready && disposition == DispatchCompletion::Executed) {
                std::lock_guard lock(mutex_);
                if (!enabled_.load(std::memory_order_acquire))
                    disposition = DispatchCompletion::Cleared;
                else if (command.levelGeneration != 0 && command.levelGeneration != generation_)
                    disposition = DispatchCompletion::GenerationInvalidated;
                else if (command.expiresAtTickMs != 0 && command.expiresAtTickMs <= GetTickCount64())
                    disposition = DispatchCompletion::Expired;
                else {
                    tasks_.push_front(std::move(command));
                    break; // Preserve authority order; never spin on an absent dependency.
                }
            }
        }
        {
            std::lock_guard lock(mutex_);
            pendingIds_.erase(command.requestId);
            RememberCompletedLocked(command.requestId);
        }

        if (disposition == DispatchCompletion::Executed) {
            try {
                command.execute();
            } catch (...) {
                disposition = DispatchCompletion::TaskFault;
            }
            lastExecutionTickMs_.store(
                GetTickCount64(),
                std::memory_order_release);
        }
        Notify(command, disposition);
        if (command.kind == GameThreadCommandKind::CreateDummy) {
            std::lock_guard lock(mutex_);
            createExecuting_ = false;
        }
        ++completed;
    }
    return completed;
}

std::size_t GameThreadDispatcher::ExpirePending() {
    std::vector<GameThreadCommand> expired;
    const auto now = GetTickCount64();
    {
        std::lock_guard lock(mutex_);
        auto retained = std::deque<GameThreadCommand>{};
        while (!tasks_.empty()) {
            auto command = std::move(tasks_.front());
            tasks_.pop_front();
            if (command.expiresAtTickMs != 0 &&
                command.expiresAtTickMs <= now) {
                pendingIds_.erase(command.requestId);
                RememberCompletedLocked(command.requestId);
                expired.push_back(std::move(command));
            } else {
                retained.push_back(std::move(command));
            }
        }
        tasks_.swap(retained);
        createPending_ = std::any_of(
            tasks_.begin(),
            tasks_.end(),
            [](const auto& command) {
                return command.kind == GameThreadCommandKind::CreateDummy;
            });
    }
    for (const auto& command : expired) {
        Notify(command, DispatchCompletion::Expired);
    }
    return expired.size();
}

std::size_t GameThreadDispatcher::CancelForGeneration(
    const std::uint64_t newGeneration) {
    std::vector<GameThreadCommand> cancelled;
    {
        std::lock_guard lock(mutex_);
        if (newGeneration == generation_) {
            return 0;
        }
        generation_ = newGeneration;
        auto retained = std::deque<GameThreadCommand>{};
        while (!tasks_.empty()) {
            auto command = std::move(tasks_.front());
            tasks_.pop_front();
            if (command.levelGeneration != 0 &&
                command.levelGeneration != newGeneration) {
                pendingIds_.erase(command.requestId);
                cancelled.push_back(std::move(command));
            } else {
                retained.push_back(std::move(command));
            }
        }
        tasks_.swap(retained);
        createPending_ = std::any_of(
            tasks_.begin(),
            tasks_.end(),
            [](const auto& command) {
                return command.kind == GameThreadCommandKind::CreateDummy;
            });
        for (const auto& command : cancelled) {
            RememberCompletedLocked(command.requestId);
        }
    }
    for (const auto& command : cancelled) {
        Notify(command, DispatchCompletion::GenerationInvalidated);
    }
    return cancelled.size();
}

void GameThreadDispatcher::Clear() noexcept {
    enabled_.store(false, std::memory_order_release);
    engineThread_.store(0, std::memory_order_release);
    engineThreadSource_.store(
        EngineThreadSource::Unknown,
        std::memory_order_release);
    firstCallbackPerformanceCounter_.store(0, std::memory_order_release);
    lastCallbackPerformanceCounter_.store(0, std::memory_order_release);
    callbackCount_.store(0, std::memory_order_release);
    mismatchedThread_.store(0, std::memory_order_release);
    mismatchCount_.store(0, std::memory_order_release);
    lastExecutionTickMs_.store(0, std::memory_order_release);
    mainThreadHookInstalled_.store(false, std::memory_order_release);
    managerUpdateHookInstalled_.store(false, std::memory_order_release);
    readinessProducerHookInstalled_.store(false, std::memory_order_release);
    std::vector<GameThreadCommand> cancelled;
    try {
        {
            std::lock_guard lock(mutex_);
            cancelled.assign(
                std::make_move_iterator(tasks_.begin()),
                std::make_move_iterator(tasks_.end()));
            tasks_.clear();
            pendingIds_.clear();
            createPending_ = false;
            createExecuting_ = false;
            for (const auto& command : cancelled) {
                RememberCompletedLocked(command.requestId);
            }
        }
        for (const auto& command : cancelled) {
            Notify(command, DispatchCompletion::Cleared);
        }
    } catch (...) {
    }
}

bool GameThreadDispatcher::IsEnabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

bool GameThreadDispatcher::IsCertifiedEngineThread(const std::uint32_t threadId) const noexcept {
    return threadId != 0U && enabled_.load(std::memory_order_acquire) &&
        engineThread_.load(std::memory_order_acquire) == threadId &&
        mismatchCount_.load(std::memory_order_acquire) == 0U;
}

std::uint32_t GameThreadDispatcher::EngineThreadId() const noexcept {
    return engineThread_.load(std::memory_order_acquire);
}

EngineThreadSource GameThreadDispatcher::EngineThreadBindingSource() const noexcept {
    return engineThreadSource_.load(std::memory_order_acquire);
}

GameThreadDispatcherSnapshot GameThreadDispatcher::Snapshot() const noexcept {
    GameThreadDispatcherSnapshot snapshot{};
    snapshot.engineThreadId = EngineThreadId();
    snapshot.source = EngineThreadBindingSource();
    snapshot.firstCallbackPerformanceCounter =
        firstCallbackPerformanceCounter_.load(std::memory_order_acquire);
    snapshot.lastCallbackPerformanceCounter =
        lastCallbackPerformanceCounter_.load(std::memory_order_acquire);
    snapshot.callbackCount = callbackCount_.load(std::memory_order_acquire);
    snapshot.mismatchedThreadId =
        mismatchedThread_.load(std::memory_order_acquire);
    snapshot.mismatchCount = mismatchCount_.load(std::memory_order_acquire);
    snapshot.lastExecutionTickMs =
        lastExecutionTickMs_.load(std::memory_order_acquire);
    snapshot.generation = Generation();
    snapshot.pendingCount = PendingCount();
    snapshot.mainThreadHookInstalled =
        mainThreadHookInstalled_.load(std::memory_order_acquire);
    snapshot.managerUpdateHookInstalled =
        managerUpdateHookInstalled_.load(std::memory_order_acquire);
    snapshot.readinessProducerHookInstalled =
        readinessProducerHookInstalled_.load(std::memory_order_acquire);
    snapshot.ready = IsEnabled();
    return snapshot;
}

std::uint64_t GameThreadDispatcher::Generation() const noexcept {
    try {
        std::lock_guard lock(mutex_);
        return generation_;
    } catch (...) {
        return 0;
    }
}

std::size_t GameThreadDispatcher::PendingCount() const noexcept {
    try {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    } catch (...) {
        return 0;
    }
}

void GameThreadDispatcher::RememberCompletedLocked(
    const std::uint64_t requestId) {
    constexpr std::size_t kCompletedCapacity = 256;
    completedIds_.insert(requestId);
    completedOrder_.push_back(requestId);
    while (completedOrder_.size() > kCompletedCapacity) {
        completedIds_.erase(completedOrder_.front());
        completedOrder_.pop_front();
    }
}

std::string_view ToString(const EngineThreadSource source) noexcept {
    switch (source) {
    case EngineThreadSource::MainThreadCharacterProcessing:
        return "MainThreadCharacterProcessing";
    case EngineThreadSource::CharacterManagerUpdate:
        return "CharacterManagerUpdate";
    case EngineThreadSource::CharacterReadinessProducer:
        return "CharacterReadinessProducer";
    case EngineThreadSource::Unknown:
        break;
    }
    return "Unknown";
}

} // namespace aimp
