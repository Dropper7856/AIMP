#include "aimp/LifecycleCatalog.h"

#include <Windows.h>

namespace aimp {

bool LifecycleCatalog::CaptureFromHook(
    const LifecycleEventKind kind,
    const std::uint64_t levelGeneration,
    const std::uintptr_t owner,
    const std::uintptr_t first,
    const std::uintptr_t second,
    const std::uintptr_t result) noexcept {
    auto& slot =
        slots_[nextSlot_.fetch_add(1, std::memory_order_relaxed) %
               slots_.size()];
    std::uint8_t empty = 0;
    if (!slot.state.compare_exchange_strong(
            empty,
            1,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    slot.event = {
        kind,
        nextSequence_.fetch_add(1, std::memory_order_relaxed),
        levelGeneration,
        static_cast<std::uint64_t>(counter.QuadPart),
        GetCurrentThreadId(),
        owner,
        first,
        second,
        result};
    slot.state.store(2, std::memory_order_release);
    return true;
}

std::size_t LifecycleCatalog::Pump() {
    std::size_t count = 0;
    for (auto& slot : slots_) {
        std::uint8_t ready = 2;
        if (!slot.state.compare_exchange_strong(
                ready,
                3,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            continue;
        }
        const auto event = slot.event;
        {
            std::lock_guard lock(historyMutex_);
            if (history_.size() >= kHistoryCapacity) {
                history_.pop_front();
            }
            history_.push_back(event);
        }
        slot.state.store(0, std::memory_order_release);
        ++count;
    }
    return count;
}

void LifecycleCatalog::InvalidateGeneration(
    const std::uint64_t generation) noexcept {
    try {
        std::lock_guard lock(historyMutex_);
        currentGeneration_ = generation;
    } catch (...) {
    }
}

std::vector<LifecycleEvent> LifecycleCatalog::Snapshot() const {
    std::lock_guard lock(historyMutex_);
    std::vector<LifecycleEvent> result;
    result.reserve(history_.size());
    for (const auto& event : history_) {
        if (event.levelGeneration == currentGeneration_) {
            result.push_back(event);
        }
    }
    return result;
}

std::vector<LifecycleEvent> LifecycleCatalog::SnapshotAll() const {
    std::lock_guard lock(historyMutex_);
    return {history_.begin(), history_.end()};
}

std::uint64_t LifecycleCatalog::Dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
}

const char* ToString(const LifecycleEventKind kind) noexcept {
    switch (kind) {
    case LifecycleEventKind::CharacterCreateBegin:
        return "character_create_begin";
    case LifecycleEventKind::CharacterCreateEnd:
        return "character_create_end";
    case LifecycleEventKind::CharacterFinalizeBegin:
        return "character_finalize_begin";
    case LifecycleEventKind::CharacterFinalizeEnd:
        return "character_finalize_end";
    case LifecycleEventKind::CharacterDestroyBegin:
        return "character_destroy_begin";
    case LifecycleEventKind::CharacterDestroyEnd:
        return "character_destroy_end";
    case LifecycleEventKind::CharacterFullDestroyBegin:
        return "character_full_destroy_begin";
    case LifecycleEventKind::CharacterFullDestroyEnd:
        return "character_full_destroy_end";
    case LifecycleEventKind::ParticipantAppendBegin:
        return "participant_append_begin";
    case LifecycleEventKind::ParticipantAppendEnd:
        return "participant_append_end";
    case LifecycleEventKind::ChannelAllocated:
        return "channel_allocated";
    case LifecycleEventKind::ChannelReleased:
        return "channel_released";
    case LifecycleEventKind::ReplicationContextAllocated:
        return "replication_context_allocated";
    case LifecycleEventKind::ReplicationContextReleased:
        return "replication_context_released";
    }
    return "unknown_lifecycle_event";
}

} // namespace aimp
