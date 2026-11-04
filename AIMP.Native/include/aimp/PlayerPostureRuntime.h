#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace aimp::player_posture {

// Values are state/portable identities, never pointers from another process.
struct State final {
    std::uint32_t flags{}; // crouch, deep crouch, in vent, active traversal
    std::uint32_t ventIdA{}, ventIdB{};
    std::uint64_t traversalRevision{};
    std::uint16_t traversalIndex{};
    std::uint8_t traversalType{};
    std::uint32_t traversalOwner{};
};

[[nodiscard]] constexpr bool Valid(const State& state) noexcept {
    if ((state.flags & ~15U) != 0U) return false;
    if ((state.ventIdA == 0U) != (state.ventIdB == 0U) ||
        ((state.flags & 4U) != 0U && state.ventIdA == 0U)) return false;
    if (state.traversalRevision == 0U)
        return (state.flags & 8U) == 0U && state.traversalIndex == 0U &&
            state.traversalType == 0U && state.traversalOwner == 0U;
    return state.traversalIndex != 0U && state.traversalOwner != UINT32_MAX &&
        (state.traversalType == 4U || state.traversalType == 5U ||
         state.traversalType == 7U || state.traversalType == 8U);
}

// Fixed, bounded IPC representation shared with RemoteMovementCommandFactory.
[[nodiscard]] inline bool Decode(const std::string_view text, State& output) noexcept {
    if (text.empty() || text.size() > 128U) return false;
    std::array<std::uint64_t, 7> values{};
    std::size_t start{};
    for (std::size_t i = 0U; i < values.size(); ++i) {
        const auto end = text.find(',', start);
        const auto last = i + 1U == values.size();
        if (last != (end == std::string_view::npos)) return false;
        const auto part = text.substr(start, last ? text.size() - start : end - start);
        if (part.empty()) return false;
        const auto converted = std::from_chars(part.data(), part.data() + part.size(), values[i]);
        if (converted.ec != std::errc{} || converted.ptr != part.data() + part.size()) return false;
        start = end + 1U;
    }
    if (values[0] > 15U || values[1] > UINT32_MAX || values[2] > UINT32_MAX ||
        values[4] > UINT16_MAX || values[5] > UINT8_MAX || values[6] > UINT32_MAX) return false;
    const State candidate{static_cast<std::uint32_t>(values[0]),
        static_cast<std::uint32_t>(values[1]), static_cast<std::uint32_t>(values[2]), values[3],
        static_cast<std::uint16_t>(values[4]), static_cast<std::uint8_t>(values[5]),
        static_cast<std::uint32_t>(values[6])};
    if (!Valid(candidate)) return false;
    output = candidate;
    return true;
}

void Initialize(std::uintptr_t moduleBase) noexcept;
// IPC thread: bounded mailbox only. Character access belongs to PumpOnEngineThread.
bool Publish(std::uintptr_t character, const State& state, std::string& error) noexcept;
// Returns true while native traversal owns movement. No transform correction is
// allowed then; retail completion releases ownership on its own.
bool PumpOnEngineThread(std::uintptr_t character) noexcept;
// Reconcile persistent stance after movement setup and after the retail tick,
// including Characters without a locomotion mailbox. Never starts traversal.
void ReconcileAllOnEngineThread() noexcept;
// A lagging proxy approaches the authored entrance through normal locomotion;
// the traversal starter itself has no distance check.
bool ApproachTarget(std::uintptr_t character, float& x, float& y, float& z) noexcept;
void Forget(std::uintptr_t character) noexcept;
void ForgetAll() noexcept;

struct DiagnosticsSnapshot final {
    std::uint64_t postureApplications{}, traversalApplications{}, traversalRejections{};
    std::uint64_t activeRevision{}, pendingTraversals{};
    std::uint64_t postureFailures{}, crouchTransitions{};
    bool desiredCrouched{}, desiredDeepCrouched{}, appliedCrouched{}, appliedDeepCrouched{};
    const char* stage{"pending"};
};
DiagnosticsSnapshot Snapshot(std::uintptr_t character) noexcept;

} // namespace aimp::player_posture
