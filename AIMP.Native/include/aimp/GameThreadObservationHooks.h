#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aimp {

class GameThreadDispatcher;
class HookManager;
class LevelGenerationTracker;
class PlayerSyncObserver;
class SignatureResolver;

struct RuntimeHookSourceSnapshot {
    std::uint64_t calls{};
    std::uint32_t primaryThreadId{};
    std::uint32_t lastThreadId{};
    std::uint64_t firstPerformanceCounter{};
    std::uint64_t lastPerformanceCounter{};
    bool observedOnMultipleThreads{};
};

struct ReadinessRuntimeSnapshot {
    RuntimeHookSourceSnapshot producer;
    RuntimeHookSourceSnapshot publisher;
    RuntimeHookSourceSnapshot barrier;
    std::uint64_t lastProducerGeneration{};
    std::uint64_t lastPublisherGeneration{};
    std::uint64_t lastBarrierGeneration{};
    std::uint32_t lastProducerSpawnKey{};
    std::uintptr_t lastProducerManager{};
    std::uintptr_t lastProducerRecord{};
    std::uintptr_t lastPublisher{};
    std::uintptr_t lastBarrierSink{};
    std::array<std::byte, 8> lastPublisherMessage{};
    std::array<std::byte, 8> lastBarrierMessage{};
    std::uint32_t participantCountBeforeBarrier{};
    std::uint32_t participantCountAfterBarrier{};
    std::uintptr_t barrierRecord{};
    std::uint32_t readyCountBeforeBarrier{};
    std::uint32_t readyCountAfterBarrier{};
    std::uint8_t recordStateBeforeBarrier{};
    std::uint8_t recordStateAfterBarrier{};
    bool publisherMessageReadable{};
    bool barrierMessageReadable{};
    bool participantCountBeforeReadable{};
    bool participantCountAfterReadable{};
    bool barrierRecordReadable{};
    bool readyCountBeforeReadable{};
    bool readyCountAfterReadable{};
    bool recordStateBeforeReadable{};
    bool recordStateAfterReadable{};
};

struct GameThreadRuntimeSnapshot {
    std::uint64_t generation{};
    RuntimeHookSourceSnapshot mainThreadCharacterProcessing;
    RuntimeHookSourceSnapshot characterManagerUpdate;
    std::uintptr_t lastCharacterManager{};
    ReadinessRuntimeSnapshot readiness;
};

class GameThreadRuntimeEvidence final {
public:
    // These observations are diagnostics only. Engine-thread authorization is
    // owned exclusively by GameThreadDispatcher::BindEngineThread.
    void ResetGeneration(std::uint64_t generation) noexcept;
    void ObserveMainThreadCharacterProcessing(
        std::uint32_t threadId,
        std::uint64_t performanceCounter) noexcept;
    void ObserveCharacterManagerUpdate(
        std::uint32_t threadId,
        std::uint64_t performanceCounter,
        std::uintptr_t manager = 0) noexcept;
    void ObserveReadinessProducer(
        std::uint32_t threadId,
        std::uint64_t performanceCounter,
        std::uint64_t generation,
        std::uintptr_t manager,
        std::uint32_t spawnKey,
        std::uintptr_t record) noexcept;
    void ObserveReadinessPublisher(
        std::uint32_t threadId,
        std::uint64_t performanceCounter,
        std::uint64_t generation,
        std::uintptr_t publisher,
        const std::array<std::byte, 8>& message,
        bool messageReadable) noexcept;
    void ObserveReadinessBarrier(
        std::uint32_t threadId,
        std::uint64_t performanceCounter,
        std::uint64_t generation,
        std::uintptr_t sink,
        const std::array<std::byte, 8>& message,
        bool messageReadable,
        std::uint32_t participantCountBefore,
        bool participantCountBeforeReadable,
        std::uint32_t participantCountAfter,
        bool participantCountAfterReadable,
        std::uintptr_t barrierRecord = 0,
        std::uint32_t readyCountBefore = 0,
        bool readyCountBeforeReadable = false,
        std::uint32_t readyCountAfter = 0,
        bool readyCountAfterReadable = false,
        std::uint8_t recordStateBefore = 0,
        bool recordStateBeforeReadable = false,
        std::uint8_t recordStateAfter = 0,
        bool recordStateAfterReadable = false) noexcept;

    [[nodiscard]] GameThreadRuntimeSnapshot Snapshot() const noexcept;

private:
    struct AtomicSource {
        std::atomic<std::uint64_t> calls{0};
        std::atomic<std::uint32_t> primaryThreadId{0};
        std::atomic<std::uint32_t> lastThreadId{0};
        std::atomic<std::uint64_t> firstPerformanceCounter{0};
        std::atomic<std::uint64_t> lastPerformanceCounter{0};
        std::atomic<bool> observedOnMultipleThreads{false};

        void Reset() noexcept;
        void Observe(
            std::uint32_t threadId,
            std::uint64_t performanceCounter) noexcept;
        [[nodiscard]] RuntimeHookSourceSnapshot Snapshot() const noexcept;
    };

    static std::uint64_t Pack(
        const std::array<std::byte, 8>& message) noexcept;
    static std::array<std::byte, 8> Unpack(std::uint64_t value) noexcept;

    std::atomic<std::uint64_t> generation_{0};
    AtomicSource mainThreadCharacterProcessing_;
    AtomicSource characterManagerUpdate_;
    std::atomic<std::uintptr_t> lastCharacterManager_{0};
    AtomicSource readinessProducer_;
    AtomicSource readinessPublisher_;
    AtomicSource readinessBarrier_;

    std::atomic<std::uint64_t> lastProducerGeneration_{0};
    std::atomic<std::uint64_t> lastPublisherGeneration_{0};
    std::atomic<std::uint64_t> lastBarrierGeneration_{0};
    std::atomic<std::uint32_t> lastProducerSpawnKey_{0};
    std::atomic<std::uintptr_t> lastProducerManager_{0};
    std::atomic<std::uintptr_t> lastProducerRecord_{0};
    std::atomic<std::uintptr_t> lastPublisher_{0};
    std::atomic<std::uintptr_t> lastBarrierSink_{0};
    std::atomic<std::uint64_t> lastPublisherMessage_{0};
    std::atomic<std::uint64_t> lastBarrierMessage_{0};
    std::atomic<std::uint32_t> participantCountBeforeBarrier_{0};
    std::atomic<std::uint32_t> participantCountAfterBarrier_{0};
    std::atomic<std::uintptr_t> barrierRecord_{0};
    std::atomic<std::uint32_t> readyCountBeforeBarrier_{0};
    std::atomic<std::uint32_t> readyCountAfterBarrier_{0};
    std::atomic<std::uint8_t> recordStateBeforeBarrier_{0};
    std::atomic<std::uint8_t> recordStateAfterBarrier_{0};
    std::atomic<bool> publisherMessageReadable_{false};
    std::atomic<bool> barrierMessageReadable_{false};
    std::atomic<bool> participantCountBeforeReadable_{false};
    std::atomic<bool> participantCountAfterReadable_{false};
    std::atomic<bool> barrierRecordReadable_{false};
    std::atomic<bool> readyCountBeforeReadable_{false};
    std::atomic<bool> readyCountAfterReadable_{false};
    std::atomic<bool> recordStateBeforeReadable_{false};
    std::atomic<bool> recordStateAfterReadable_{false};

};

class GameThreadObservationHooks final {
public:
    GameThreadObservationHooks(
        GameThreadRuntimeEvidence& evidence,
        GameThreadDispatcher& dispatcher,
        LevelGenerationTracker& generations,
        PlayerSyncObserver& playerSync) noexcept;
    ~GameThreadObservationHooks();

    bool InstallEngineThreadHooks(
        const SignatureResolver& resolver,
        HookManager& hooks,
        std::string& error);
    bool InstallOptionalReadinessHooks(
        const SignatureResolver& resolver,
        HookManager& hooks,
        std::string& error);
    void BeginLogicalShutdown() noexcept;

    [[nodiscard]] bool Installed() const noexcept;
    [[nodiscard]] bool MainThreadHookInstalled() const noexcept;
    [[nodiscard]] bool ManagerUpdateHookInstalled() const noexcept;
    [[nodiscard]] bool ReadinessProducerHookInstalled() const noexcept;
    [[nodiscard]] bool ReadinessHooksInstalled() const noexcept;
    [[nodiscard]] bool ProcessLifetimeRequired() const noexcept;
    [[nodiscard]] static std::uint32_t ActiveDetours() noexcept;

private:
    GameThreadRuntimeEvidence* evidence_{};
    GameThreadDispatcher* dispatcher_{};
    LevelGenerationTracker* generations_{};
    PlayerSyncObserver* playerSync_{};
    void* mainThreadTrampoline_{};
    void* managerUpdateTrampoline_{};
    void* readinessProducerTrampoline_{};
    void* readinessPublisherTrampoline_{};
    void* readinessBarrierTrampoline_{};
    bool mainThreadHookInstalled_{};
    bool managerUpdateHookInstalled_{};
    bool readinessProducerHookInstalled_{};
    bool readinessPublisherHookInstalled_{};
    bool readinessBarrierHookInstalled_{};
    bool processLifetimeRequired_{};
};

} // namespace aimp
