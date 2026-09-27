#include "aimp/DummyPlayerController.h"

#include <algorithm>

namespace aimp {
namespace {

constexpr DummyNativeStep kCreateOrder[]{
    DummyNativeStep::Participant,
    DummyNativeStep::Channel,
    DummyNativeStep::Descriptors,
    DummyNativeStep::Character,
    DummyNativeStep::ReplicationContext,
    DummyNativeStep::Binding,
};

// This is not a generic reverse-allocation order. The demonstrated lifecycle
// requires detaching the participant binding before Character destruction,
// then returning the 0x440 context and channel-owned descriptor state.
constexpr DummyNativeStep kCleanupOrder[]{
    DummyNativeStep::Binding,
    DummyNativeStep::Character,
    DummyNativeStep::ReplicationContext,
    DummyNativeStep::Descriptors,
    DummyNativeStep::Channel,
    DummyNativeStep::Participant,
};

DummyAchievement AchievementFor(
    const std::vector<DummyNativeStep>& acquired) noexcept {
    const auto has = [&](const DummyNativeStep step) {
        return std::find(acquired.begin(), acquired.end(), step) !=
            acquired.end();
    };
    if (has(DummyNativeStep::Binding) &&
        has(DummyNativeStep::ReplicationContext) &&
        has(DummyNativeStep::Descriptors) &&
        has(DummyNativeStep::Channel) &&
        has(DummyNativeStep::Participant) &&
        has(DummyNativeStep::Character)) {
        return DummyAchievement::C;
    }
    if (has(DummyNativeStep::Character)) {
        return DummyAchievement::A;
    }
    return DummyAchievement::None;
}

} // namespace

DummyOperationResult DummyPlayerController::Create(
    const DummyRequestIdentity& identity,
    const std::uint64_t generation,
    const DummySpawnOffset& spawnOffset,
    IDummyNativeExecutor& executor) {
    std::lock_guard lock(mutex_);
    if (state_ == DummyState::Creating ||
        state_ == DummyState::Removing ||
        state_ == DummyState::Active ||
        state_ == DummyState::Failed) {
        return {
            state_,
            achievement_,
            "DUMMY_ALREADY_EXISTS",
            state_ == DummyState::Failed
                ? "A prior native attempt failed; wait for explicit level-generation invalidation before retrying."
                : "A single Dummy Player is already active or in progress.",
            false,
            {},
            {state_ == DummyState::Failed
                 ? "level_generation_reset_after_failure"
                 : "single_dummy_limit"}};
    }
    if (generation == 0 ||
        identity.portableDummyId.empty() ||
        identity.peerId == 0 ||
        identity.portableBindingId.empty()) {
        return {
            DummyState::Failed,
            DummyAchievement::None,
            "INVALID_DUMMY_REQUEST",
            "Portable identities and a non-zero level generation are required.",
            false,
            {},
            {"valid_portable_identity", "stable_level_generation"}};
    }

    state_ = DummyState::Creating;
    achievement_ = DummyAchievement::None;
    generation_ = generation;
    identity_ = identity;
    acquired_.clear();
    std::string error;
    if (!executor.BeginCreate(
            identity,
            generation,
            spawnOffset,
            error)) {
        state_ = DummyState::Failed;
        identity_ = {};
        return {
            state_,
            DummyAchievement::None,
            "DUMMY_CREATE_BLOCKED",
            error.empty()
                ? "native_create_context_rejected"
                : error,
            false,
            {},
            {"validated_native_spawn_pipeline"}};
    }
    for (const auto step : kCreateOrder) {
        if (!executor.Acquire(step, error)) {
            return RollbackLocked(
                executor,
                error.empty() ? "native_step_failed" : error,
                acquired_);
        }
        acquired_.push_back(step);
    }
    if (!executor.Verify(error)) {
        return RollbackLocked(
            executor,
            error.empty() ? "post_spawn_verification_failed" : error,
            acquired_);
    }
    achievement_ = AchievementFor(acquired_);
    state_ = DummyState::Active;

    DummyOperationResult result{};
    result.state = state_;
    result.achieved = achievement_;
    result.statusCode = "DUMMY_ACTIVE";
    result.message =
        "Participant, Character, channel, descriptors, context and binding were verified.";
    for (const auto step : acquired_) {
        result.completedSteps.emplace_back(ToString(step));
    }
    return result;
}

DummyOperationResult DummyPlayerController::Create(
    const DummyRequestIdentity& identity,
    const std::uint64_t generation,
    IDummyNativeExecutor& executor) {
    return Create(identity, generation, {}, executor);
}

DummyOperationResult DummyPlayerController::Remove(
    const std::uint64_t generation,
    IDummyNativeExecutor& executor) {
    std::lock_guard lock(mutex_);
    if (state_ == DummyState::Absent) {
        return {
            DummyState::Absent,
            DummyAchievement::None,
            "DUMMY_ABSENT",
            "No Dummy Player exists; remove is idempotent.",
            false};
    }
    if (generation != generation_) {
        return {
            state_,
            achievement_,
            "LEVEL_GENERATION_MISMATCH",
            "The Dummy mapping belongs to an obsolete level generation.",
            false,
            {},
            {"current_level_generation"}};
    }

    state_ = DummyState::Removing;
    DummyOperationResult result{};
    result.rollbackPerformed = true;
    std::string firstError;
    for (const auto step : kCleanupOrder) {
        if (std::find(acquired_.begin(), acquired_.end(), step) ==
            acquired_.end()) {
            continue;
        }
        std::string error;
        if (executor.Release(step, error)) {
            result.completedSteps.emplace_back(ToString(step));
        } else if (firstError.empty()) {
            firstError = error.empty() ? "native_release_failed" : error;
        }
    }
    acquired_.clear();
    identity_ = {};
    achievement_ = DummyAchievement::None;
    if (firstError.empty()) {
        state_ = DummyState::Absent;
        generation_ = 0;
        result.state = state_;
        result.achieved = achievement_;
        result.statusCode = "DUMMY_REMOVED";
        result.message =
            "Dummy Player resources were released in the validated lifecycle cleanup order.";
    } else {
        state_ = DummyState::Failed;
        result.state = state_;
        result.achieved = achievement_;
        result.statusCode = "DUMMY_REMOVE_PARTIAL";
        result.message = firstError;
        result.blockedGates.emplace_back("native_cleanup_verification");
    }
    return result;
}

DummyOperationResult DummyPlayerController::Move(
    const std::uint64_t generation,
    const std::string_view command,
    const float amount,
    const std::uint32_t durationMilliseconds,
    IDummyNativeExecutor& executor) {
    std::lock_guard lock(mutex_);
    if (state_ != DummyState::Active || generation != generation_) {
        return {
            state_,
            achievement_,
            "DUMMY_NOT_ACTIVE",
            "Movement requires an active Dummy in the current generation.",
            false,
            {},
            {"active_dummy_binding"}};
    }
    std::string method;
    std::string error;
    if (!executor.Move(
            command,
            amount,
            durationMilliseconds,
            method,
            error)) {
        return {
            state_,
            achievement_,
            "MOVEMENT_BLOCKED",
            error.empty() ? "No validated movement path is available." : error,
            false,
            {},
            {"validated_native_movement_path"}};
    }
    achievement_ = DummyAchievement::D;
    return {
        state_,
        achievement_,
        "MOVEMENT_APPLIED",
        method,
        false,
        {"movement"}};
}

void DummyPlayerController::InvalidateGeneration(
    const std::uint64_t newGeneration) noexcept {
    try {
        std::lock_guard lock(mutex_);
        // Pointers from the old generation are deliberately not dereferenced.
        acquired_.clear();
        identity_ = {};
        achievement_ = DummyAchievement::None;
        state_ = DummyState::Absent;
        generation_ = newGeneration;
    } catch (...) {
    }
}

DummyState DummyPlayerController::State() const noexcept {
    try {
        std::lock_guard lock(mutex_);
        return state_;
    } catch (...) {
        return DummyState::Failed;
    }
}

DummyAchievement DummyPlayerController::Achievement() const noexcept {
    try {
        std::lock_guard lock(mutex_);
        return achievement_;
    } catch (...) {
        return DummyAchievement::None;
    }
}

std::uint64_t DummyPlayerController::Generation() const noexcept {
    try {
        std::lock_guard lock(mutex_);
        return generation_;
    } catch (...) {
        return 0;
    }
}

std::string DummyPlayerController::PortableDummyId() const {
    std::lock_guard lock(mutex_);
    return identity_.portableDummyId;
}

DummyOperationResult DummyPlayerController::RollbackLocked(
    IDummyNativeExecutor& executor,
    const std::string_view failure,
    std::vector<DummyNativeStep>& acquired) {
    DummyOperationResult result{};
    result.rollbackPerformed = !acquired.empty();
    std::string rollbackError;
    for (const auto step : kCleanupOrder) {
        if (std::find(acquired.begin(), acquired.end(), step) ==
            acquired.end()) {
            continue;
        }
        std::string error;
        if (executor.Release(step, error)) {
            result.completedSteps.emplace_back(ToString(step));
        } else if (rollbackError.empty()) {
            rollbackError =
                error.empty() ? "rollback_release_failed" : error;
        }
    }
    acquired.clear();
    identity_ = {};
    achievement_ = DummyAchievement::None;
    state_ = DummyState::Failed;
    result.state = state_;
    result.achieved = achievement_;
    result.statusCode =
        rollbackError.empty() ? "DUMMY_CREATE_BLOCKED" : "ROLLBACK_PARTIAL";
    result.message = std::string(failure);
    if (!rollbackError.empty()) {
        result.message += "; ";
        result.message += rollbackError;
    }
    result.blockedGates.emplace_back("validated_native_spawn_pipeline");
    return result;
}

bool BlockedDummyExecutor::BeginCreate(
    const DummyRequestIdentity&,
    const std::uint64_t,
    const DummySpawnOffset&,
    std::string& error) {
    error =
        "Runtime mutation is blocked: game-thread correlation, natural capture replay, participant cleanup and binding verification are not jointly demonstrated.";
    return false;
}

bool BlockedDummyExecutor::Acquire(
    const DummyNativeStep,
    std::string& error) {
    error =
        "Runtime mutation is blocked: game-thread correlation, natural capture replay, participant cleanup and binding verification are not jointly demonstrated.";
    return false;
}

bool BlockedDummyExecutor::Release(
    const DummyNativeStep,
    std::string&) noexcept {
    return true;
}

bool BlockedDummyExecutor::Verify(std::string& error) {
    error = "No production Dummy binding can be verified in observation-only mode.";
    return false;
}

bool BlockedDummyExecutor::Move(
    const std::string_view,
    const float,
    const std::uint32_t,
    std::string& method,
    std::string& error) {
    method = "NotApplied";
    error =
        "No native descriptor, locomotion command or controlled transform path is runtime-validated.";
    return false;
}

std::string_view ToString(const DummyState state) noexcept {
    switch (state) {
    case DummyState::Absent:
        return "Absent";
    case DummyState::Creating:
        return "Creating";
    case DummyState::Active:
        return "Active";
    case DummyState::Removing:
        return "Removing";
    case DummyState::Failed:
        return "Failed";
    }
    return "Failed";
}

std::string_view ToString(const DummyAchievement level) noexcept {
    switch (level) {
    case DummyAchievement::None:
        return "None";
    case DummyAchievement::A:
        return "A";
    case DummyAchievement::B:
        return "B";
    case DummyAchievement::C:
        return "C";
    case DummyAchievement::D:
        return "D";
    }
    return "None";
}

std::string_view ToString(const DummyNativeStep step) noexcept {
    switch (step) {
    case DummyNativeStep::Participant:
        return "participant";
    case DummyNativeStep::Channel:
        return "channel";
    case DummyNativeStep::Descriptors:
        return "descriptors";
    case DummyNativeStep::Character:
        return "character";
    case DummyNativeStep::ReplicationContext:
        return "replication_context";
    case DummyNativeStep::Binding:
        return "binding";
    }
    return "unknown";
}

} // namespace aimp
