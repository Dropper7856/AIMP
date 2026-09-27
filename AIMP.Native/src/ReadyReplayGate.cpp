#include "aimp/ReadyReplayGate.h"

#include <cstring>

namespace aimp {
namespace {

[[nodiscard]] std::uint32_t MessageSpawnKey(
    const ReadyMessage8& message) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, message.data(), sizeof(value));
    return value;
}

} // namespace

ReadyReplayGateResult ReadyReplayGate::Arm(
    const std::uint64_t levelGeneration,
    const std::uint32_t spawnKey) noexcept {
    if (levelGeneration == 0) {
        return {ReadyReplayGateCode::InvalidGeneration, false};
    }
    if (spawnKey == 0) {
        return {ReadyReplayGateCode::InvalidSpawnKey, false};
    }
    if (state_ != ReadyReplayGateState::Idle) {
        return {ReadyReplayGateCode::AlreadyArmed, false};
    }

    levelGeneration_ = levelGeneration;
    spawnKey_ = spawnKey;
    naturalMessage_.fill(std::byte{0});
    state_ = ReadyReplayGateState::Armed;
    return {ReadyReplayGateCode::Ok, true};
}

ReadyReplayGateResult ReadyReplayGate::AcceptNaturalReady(
    const NaturalReadyObservation& observation) noexcept {
    if (state_ == ReadyReplayGateState::Idle) {
        return {ReadyReplayGateCode::NotArmed, false};
    }
    if (observation.levelGeneration != levelGeneration_) {
        return {ReadyReplayGateCode::GenerationMismatch, false};
    }
    if (observation.spawnKey != spawnKey_) {
        return {ReadyReplayGateCode::SpawnKeyMismatch, false};
    }
    if (MessageSpawnKey(observation.message) != spawnKey_) {
        return {
            ReadyReplayGateCode::MessageSpawnKeyMismatch,
            false};
    }
    if (observation.participantCount != 2) {
        return {
            ReadyReplayGateCode::UnexpectedParticipantCount,
            false};
    }
    if (observation.readyCountBefore != 0 ||
        observation.readyCountAfter != 1) {
        return {
            ReadyReplayGateCode::UnexpectedReadyTransition,
            false};
    }
    if (state_ != ReadyReplayGateState::Armed) {
        if (observation.message != naturalMessage_) {
            return {ReadyReplayGateCode::MessageMismatch, false};
        }
        return {
            ReadyReplayGateCode::DuplicateNaturalReady,
            false};
    }

    naturalMessage_ = observation.message;
    state_ = ReadyReplayGateState::NaturalReadyAccepted;
    return {ReadyReplayGateCode::Ok, true};
}

ReadyReplayGateResult ReadyReplayGate::ScheduleReplay(
    const ReadyReplayCheck& check) noexcept {
    if (state_ == ReadyReplayGateState::Idle) {
        return {ReadyReplayGateCode::NotArmed, false};
    }
    if (state_ == ReadyReplayGateState::Armed) {
        return {ReadyReplayGateCode::NaturalReadyMissing, false};
    }
    const auto validation = ValidateCheck(check);
    if (validation != ReadyReplayGateCode::Ok) {
        return {validation, false};
    }
    if (state_ == ReadyReplayGateState::ReplayScheduled ||
        state_ == ReadyReplayGateState::ReplayPermitConsumed) {
        return {ReadyReplayGateCode::DuplicateSchedule, false};
    }

    state_ = ReadyReplayGateState::ReplayScheduled;
    return {ReadyReplayGateCode::Ok, true};
}

ReadyReplayGateResult ReadyReplayGate::ConsumeExecutionPermit(
    const ReadyReplayCheck& check) noexcept {
    if (state_ == ReadyReplayGateState::Idle) {
        return {ReadyReplayGateCode::NotArmed, false};
    }
    if (state_ == ReadyReplayGateState::Armed ||
        state_ == ReadyReplayGateState::NaturalReadyAccepted) {
        return {ReadyReplayGateCode::ReplayNotScheduled, false};
    }
    const auto validation = ValidateCheck(check);
    if (validation != ReadyReplayGateCode::Ok) {
        return {validation, false};
    }
    if (state_ == ReadyReplayGateState::ReplayPermitConsumed) {
        return {ReadyReplayGateCode::DuplicateExecution, false};
    }

    state_ = ReadyReplayGateState::ReplayPermitConsumed;
    return {ReadyReplayGateCode::Ok, true};
}

void ReadyReplayGate::Reset() noexcept {
    state_ = ReadyReplayGateState::Idle;
    levelGeneration_ = 0;
    spawnKey_ = 0;
    naturalMessage_.fill(std::byte{0});
}

ReadyReplayGateSnapshot ReadyReplayGate::Snapshot() const noexcept {
    return {
        state_,
        levelGeneration_,
        spawnKey_,
        naturalMessage_};
}

ReadyReplayGateCode ReadyReplayGate::ValidateCheck(
    const ReadyReplayCheck& check) const noexcept {
    if (check.levelGeneration != levelGeneration_) {
        return ReadyReplayGateCode::GenerationMismatch;
    }
    if (check.spawnKey != spawnKey_) {
        return ReadyReplayGateCode::SpawnKeyMismatch;
    }
    if (MessageSpawnKey(check.message) != spawnKey_) {
        return ReadyReplayGateCode::MessageSpawnKeyMismatch;
    }
    if (check.message != naturalMessage_) {
        return ReadyReplayGateCode::MessageMismatch;
    }
    if (check.participantCount != 2) {
        return ReadyReplayGateCode::UnexpectedParticipantCount;
    }
    if (check.readyCount != 1) {
        return ReadyReplayGateCode::UnexpectedReadyCount;
    }
    return ReadyReplayGateCode::Ok;
}

std::string_view ToString(
    const ReadyReplayGateCode code) noexcept {
    switch (code) {
    case ReadyReplayGateCode::Ok:
        return "ok";
    case ReadyReplayGateCode::InvalidGeneration:
        return "invalid_generation";
    case ReadyReplayGateCode::InvalidSpawnKey:
        return "invalid_spawn_key";
    case ReadyReplayGateCode::AlreadyArmed:
        return "already_armed";
    case ReadyReplayGateCode::NotArmed:
        return "not_armed";
    case ReadyReplayGateCode::GenerationMismatch:
        return "generation_mismatch";
    case ReadyReplayGateCode::SpawnKeyMismatch:
        return "spawn_key_mismatch";
    case ReadyReplayGateCode::MessageSpawnKeyMismatch:
        return "message_spawn_key_mismatch";
    case ReadyReplayGateCode::MessageMismatch:
        return "message_mismatch";
    case ReadyReplayGateCode::UnexpectedParticipantCount:
        return "unexpected_participant_count";
    case ReadyReplayGateCode::UnexpectedReadyTransition:
        return "unexpected_ready_transition";
    case ReadyReplayGateCode::UnexpectedReadyCount:
        return "unexpected_ready_count";
    case ReadyReplayGateCode::DuplicateNaturalReady:
        return "duplicate_natural_ready";
    case ReadyReplayGateCode::NaturalReadyMissing:
        return "natural_ready_missing";
    case ReadyReplayGateCode::DuplicateSchedule:
        return "duplicate_schedule";
    case ReadyReplayGateCode::ReplayNotScheduled:
        return "replay_not_scheduled";
    case ReadyReplayGateCode::DuplicateExecution:
        return "duplicate_execution";
    }
    return "unknown";
}

} // namespace aimp
