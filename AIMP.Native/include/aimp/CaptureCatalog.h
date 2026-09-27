#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aimp {

enum class CharacterRequestKind : std::uint8_t {
    SpawnF0,
    Spawn130,
};

struct CharacterRequestCapture {
    CharacterRequestKind kind{CharacterRequestKind::SpawnF0};
    std::uint64_t sequence{};
    std::uint64_t levelGeneration{};
    std::uint64_t performanceCounter{};
    std::uint32_t threadId{};
    std::uint32_t sourceRva{};
    std::uintptr_t sink{};
    std::uint16_t size{};
    std::array<std::byte, 0x130> bytes{};
};

struct CaptureCatalogSummary {
    std::uint64_t accepted{};
    std::uint64_t dropped{};
    std::uint64_t currentGeneration{};
    bool hasF0{};
    bool has130{};
    std::uint32_t lastThreadId{};
    std::uint32_t currentCaptureCount{};
};

class CaptureCatalog final {
public:
    static constexpr std::size_t kHookSlots = 64;
    static constexpr std::size_t kHistoryCapacity = 64;

    bool CaptureFromHook(
        CharacterRequestKind kind,
        const void* sink,
        const void* request,
        std::size_t bytes,
        std::uint64_t levelGeneration,
        std::uint32_t sourceRva) noexcept;

    std::size_t Pump();
    void InvalidateGeneration(std::uint64_t generation);
    [[nodiscard]] CaptureCatalogSummary Summary() const noexcept;
    [[nodiscard]] std::optional<CharacterRequestCapture> Latest(
        CharacterRequestKind kind,
        std::uint64_t generation) const;
    [[nodiscard]] std::vector<CharacterRequestCapture> Snapshot() const;

private:
    struct Slot {
        std::atomic<std::uint8_t> state{0};
        CharacterRequestCapture capture{};
    };

    std::array<Slot, kHookSlots> slots_{};
    std::atomic<std::uint64_t> nextSlot_{0};
    std::atomic<std::uint64_t> nextSequence_{1};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> dropped_{0};
    mutable std::mutex historyMutex_;
    std::deque<CharacterRequestCapture> history_;
    std::uint64_t currentGeneration_{};
    std::uint32_t lastThreadId_{};
};

[[nodiscard]] std::string CharacterRequestHex(
    const CharacterRequestCapture& capture);

} // namespace aimp
