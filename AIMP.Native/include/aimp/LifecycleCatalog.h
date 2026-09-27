#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace aimp {

enum class LifecycleEventKind : std::uint8_t {
    CharacterCreateBegin,
    CharacterCreateEnd,
    CharacterFinalizeBegin,
    CharacterFinalizeEnd,
    CharacterDestroyBegin,
    CharacterDestroyEnd,
    CharacterFullDestroyBegin,
    CharacterFullDestroyEnd,
    ParticipantAppendBegin,
    ParticipantAppendEnd,
    ChannelAllocated,
    ChannelReleased,
    ReplicationContextAllocated,
    ReplicationContextReleased,
};

struct LifecycleEvent {
    LifecycleEventKind kind{LifecycleEventKind::CharacterCreateBegin};
    std::uint64_t sequence{};
    std::uint64_t levelGeneration{};
    std::uint64_t performanceCounter{};
    std::uint32_t threadId{};
    std::uintptr_t owner{};
    std::uintptr_t first{};
    std::uintptr_t second{};
    std::uintptr_t result{};
};

class LifecycleCatalog final {
public:
    static constexpr std::size_t kHookSlots = 128;
    static constexpr std::size_t kHistoryCapacity = 256;

    bool CaptureFromHook(
        LifecycleEventKind kind,
        std::uint64_t levelGeneration,
        std::uintptr_t owner,
        std::uintptr_t first,
        std::uintptr_t second,
        std::uintptr_t result) noexcept;
    std::size_t Pump();
    void InvalidateGeneration(std::uint64_t generation) noexcept;
    [[nodiscard]] std::vector<LifecycleEvent> Snapshot() const;
    // Diagnostic history across generations. Callers must explicitly filter
    // before using an event as authorization for current-generation work.
    [[nodiscard]] std::vector<LifecycleEvent> SnapshotAll() const;
    [[nodiscard]] std::uint64_t Dropped() const noexcept;

private:
    struct Slot {
        std::atomic<std::uint8_t> state{0};
        LifecycleEvent event{};
    };

    std::array<Slot, kHookSlots> slots_{};
    std::atomic<std::uint64_t> nextSlot_{0};
    std::atomic<std::uint64_t> nextSequence_{1};
    std::atomic<std::uint64_t> dropped_{0};
    mutable std::mutex historyMutex_;
    std::deque<LifecycleEvent> history_;
    std::uint64_t currentGeneration_{};
};

[[nodiscard]] const char* ToString(LifecycleEventKind kind) noexcept;

} // namespace aimp
