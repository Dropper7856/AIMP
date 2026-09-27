#include "aimp/CaptureCatalog.h"

#include "aimp/CrashGuard.h"

#include <Windows.h>

#include <algorithm>

namespace aimp {

bool CaptureCatalog::CaptureFromHook(
    const CharacterRequestKind kind,
    const void* sink,
    const void* request,
    const std::size_t bytes,
    const std::uint64_t levelGeneration,
    const std::uint32_t sourceRva) noexcept {
    const std::size_t expected =
        kind == CharacterRequestKind::SpawnF0 ? 0xF0U : 0x130U;
    if (request == nullptr || bytes != expected) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

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

    CharacterRequestCapture capture{};
    capture.kind = kind;
    capture.sequence =
        nextSequence_.fetch_add(1, std::memory_order_relaxed);
    capture.levelGeneration = levelGeneration;
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    capture.performanceCounter =
        static_cast<std::uint64_t>(counter.QuadPart);
    capture.threadId = GetCurrentThreadId();
    capture.sourceRva = sourceRva;
    capture.sink = reinterpret_cast<std::uintptr_t>(sink);
    capture.size = static_cast<std::uint16_t>(bytes);
    if (!crash_guard::TryCopy(capture.bytes.data(), request, bytes)) {
        slot.state.store(0, std::memory_order_release);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    slot.capture = capture;
    slot.state.store(2, std::memory_order_release);
    accepted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::size_t CaptureCatalog::Pump() {
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
        const auto capture = slot.capture;
        {
            std::lock_guard lock(historyMutex_);
            if (capture.levelGeneration == currentGeneration_) {
                lastThreadId_ = capture.threadId;
            }
            if (history_.size() >= kHistoryCapacity) {
                history_.pop_front();
            }
            history_.push_back(capture);
        }
        slot.state.store(0, std::memory_order_release);
        ++count;
    }
    return count;
}

void CaptureCatalog::InvalidateGeneration(const std::uint64_t generation) {
    std::lock_guard lock(historyMutex_);
    currentGeneration_ = generation;
    lastThreadId_ = 0;
}

CaptureCatalogSummary CaptureCatalog::Summary() const noexcept {
    CaptureCatalogSummary summary{};
    summary.accepted = accepted_.load(std::memory_order_relaxed);
    summary.dropped = dropped_.load(std::memory_order_relaxed);
    try {
        std::lock_guard lock(historyMutex_);
        summary.currentGeneration = currentGeneration_;
        summary.lastThreadId = lastThreadId_;
        for (const auto& capture : history_) {
            if (capture.levelGeneration != currentGeneration_) {
                continue;
            }
            ++summary.currentCaptureCount;
            summary.hasF0 =
                summary.hasF0 ||
                capture.kind == CharacterRequestKind::SpawnF0;
            summary.has130 =
                summary.has130 ||
                capture.kind == CharacterRequestKind::Spawn130;
        }
    } catch (...) {
    }
    return summary;
}

std::optional<CharacterRequestCapture> CaptureCatalog::Latest(
    const CharacterRequestKind kind,
    const std::uint64_t generation) const {
    std::lock_guard lock(historyMutex_);
    const auto found = std::find_if(
        history_.rbegin(),
        history_.rend(),
        [&](const CharacterRequestCapture& capture) {
            return capture.kind == kind &&
                capture.levelGeneration == generation;
        });
    if (found == history_.rend()) {
        return std::nullopt;
    }
    return *found;
}

std::vector<CharacterRequestCapture> CaptureCatalog::Snapshot() const {
    std::lock_guard lock(historyMutex_);
    return {history_.begin(), history_.end()};
}

std::string CharacterRequestHex(
    const CharacterRequestCapture& capture) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto bytes = (std::min<std::size_t>)(
        capture.size,
        capture.bytes.size());
    std::string output(bytes * 2, '0');
    for (std::size_t index = 0; index < bytes; ++index) {
        const auto value =
            std::to_integer<std::uint8_t>(capture.bytes[index]);
        output[index * 2] = kHex[(value >> 4U) & 0x0FU];
        output[index * 2 + 1] = kHex[value & 0x0FU];
    }
    return output;
}

} // namespace aimp
