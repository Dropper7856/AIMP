#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aimp {

using ReadyMessage8 = std::array<std::byte, 8>;

enum class ReadyReplayGateState : std::uint8_t {
    Idle,
    Armed,
    NaturalReadyAccepted,
    ReplayScheduled,
    ReplayPermitConsumed,
};

enum class ReadyReplayGateCode : std::uint8_t {
    Ok,
    InvalidGeneration,
    InvalidSpawnKey,
    AlreadyArmed,
    NotArmed,
    GenerationMismatch,
    SpawnKeyMismatch,
    MessageSpawnKeyMismatch,
    MessageMismatch,
    UnexpectedParticipantCount,
    UnexpectedReadyTransition,
    UnexpectedReadyCount,
    DuplicateNaturalReady,
    NaturalReadyMissing,
    DuplicateSchedule,
    ReplayNotScheduled,
    DuplicateExecution,
};

struct NaturalReadyObservation {
    std::uint64_t levelGeneration{};
    std::uint32_t spawnKey{};
    ReadyMessage8 message{};
    std::uint32_t participantCount{};
    std::uint32_t readyCountBefore{};
    std::uint32_t readyCountAfter{};
};

// The caller supplies a fresh, pointer-free view of the barrier immediately
// before scheduling and immediately before consuming the execution permit.
struct ReadyReplayCheck {
    std::uint64_t levelGeneration{};
    std::uint32_t spawnKey{};
    ReadyMessage8 message{};
    std::uint32_t participantCount{};
    std::uint32_t readyCount{};
};

struct ReadyReplayGateResult {
    ReadyReplayGateCode code{ReadyReplayGateCode::Ok};
    bool stateChanged{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return code == ReadyReplayGateCode::Ok;
    }
};

struct ReadyReplayGateSnapshot {
    ReadyReplayGateState state{ReadyReplayGateState::Idle};
    std::uint64_t levelGeneration{};
    std::uint32_t spawnKey{};
    ReadyMessage8 naturalMessage{};
};

// Pure value-state gate for the one extra readiness acknowledgement required
// by the two-participant Solo Dummy experiment. It neither stores engine
// pointers nor calls engine code. Its methods are intended to run serially on
// the validated game thread.
class ReadyReplayGate final {
public:
    [[nodiscard]] ReadyReplayGateResult Arm(
        std::uint64_t levelGeneration,
        std::uint32_t spawnKey) noexcept;

    // Accepts exactly one naturally observed Ready8 transition for the armed
    // spawn: participantCount == 2 and readyCount 0 -> 1.
    [[nodiscard]] ReadyReplayGateResult AcceptNaturalReady(
        const NaturalReadyObservation& observation) noexcept;

    // Reserves the sole replay only while the same exact Ready8 is still at
    // participantCount == 2 / readyCount == 1.
    [[nodiscard]] ReadyReplayGateResult ScheduleReplay(
        const ReadyReplayCheck& check) noexcept;

    // Consumes the sole execution permit after one final value revalidation.
    // A successful result authorizes the caller to perform one replay; this
    // class itself performs no callback.
    [[nodiscard]] ReadyReplayGateResult ConsumeExecutionPermit(
        const ReadyReplayCheck& check) noexcept;

    // Invalidates all values without dereferencing anything from the obsolete
    // level generation.
    void Reset() noexcept;

    [[nodiscard]] ReadyReplayGateSnapshot Snapshot() const noexcept;

private:
    [[nodiscard]] ReadyReplayGateCode ValidateCheck(
        const ReadyReplayCheck& check) const noexcept;

    ReadyReplayGateState state_{ReadyReplayGateState::Idle};
    std::uint64_t levelGeneration_{};
    std::uint32_t spawnKey_{};
    ReadyMessage8 naturalMessage_{};
};

[[nodiscard]] std::string_view ToString(
    ReadyReplayGateCode code) noexcept;

} // namespace aimp
