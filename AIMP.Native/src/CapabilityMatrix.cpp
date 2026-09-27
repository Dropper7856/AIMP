#include "aimp/CapabilityMatrix.h"

#include <utility>

namespace aimp {
namespace {

constexpr std::size_t Index(const Capability capability) noexcept {
    return static_cast<std::size_t>(capability);
}

} // namespace

CapabilityMatrix::CapabilityMatrix() {
    for (std::size_t index = 0; index < records_.size(); ++index) {
        records_[index].capability = static_cast<Capability>(index);
        records_[index].state = CapabilityState::Unresolved;
        records_[index].reason = "not_resolved";
    }
}

void CapabilityMatrix::Set(
    const Capability capability,
    const CapabilityState state,
    std::string reason) {
    if (capability == Capability::Count) {
        return;
    }
    std::lock_guard lock(mutex_);
    auto& record = records_[Index(capability)];
    record.state = state;
    record.reason = std::move(reason);
}

CapabilityRecord CapabilityMatrix::Get(const Capability capability) const {
    if (capability == Capability::Count) {
        return {capability, CapabilityState::Unresolved, "invalid_capability"};
    }
    std::lock_guard lock(mutex_);
    return records_[Index(capability)];
}

std::array<CapabilityRecord, static_cast<std::size_t>(Capability::Count)>
CapabilityMatrix::Snapshot() const {
    std::lock_guard lock(mutex_);
    return records_;
}

bool CapabilityMatrix::IsEnabled(const Capability capability) const {
    return Get(capability).state == CapabilityState::RuntimeValidated;
}

bool CapabilityMatrix::IsAtLeastResolved(const Capability capability) const {
    const auto state = Get(capability).state;
    return state == CapabilityState::Observed ||
        state == CapabilityState::StaticValidated ||
        state == CapabilityState::RuntimeValidated;
}

std::string_view ToString(const Capability capability) noexcept {
    switch (capability) {
    case Capability::ObserveNetZ:
        return "ObserveNetZ";
    case Capability::ObserveCharacterRequests:
        return "ObserveCharacterRequests";
    case Capability::ObserveCharacterLifecycle:
        return "ObserveCharacterLifecycle";
    case Capability::ObserveParticipants:
        return "ObserveParticipants";
    case Capability::ObserveChannels:
        return "ObserveChannels";
    case Capability::ObserveReplicationContexts:
        return "ObserveReplicationContexts";
    case Capability::ObserveLevelTransitions:
        return "ObserveLevelTransitions";
    case Capability::ScheduleGameThreadWork:
        return "ScheduleGameThreadWork";
    case Capability::CreateDummyParticipant:
        return "CreateDummyParticipant";
    case Capability::ReplayCharacterSpawn:
        return "ReplayCharacterSpawn";
    case Capability::BindDummyCharacter:
        return "BindDummyCharacter";
    case Capability::DestroyDummyCharacter:
        return "DestroyDummyCharacter";
    case Capability::ObserveOutboundDescriptors:
        return "ObserveOutboundDescriptors";
    case Capability::ApplyInboundDescriptors:
        return "ApplyInboundDescriptors";
    case Capability::Count:
        break;
    }
    return "UnknownCapability";
}

std::string_view ToString(const CapabilityState state) noexcept {
    switch (state) {
    case CapabilityState::Unresolved:
        return "Unresolved";
    case CapabilityState::Observed:
        return "Observed";
    case CapabilityState::StaticValidated:
        return "StaticValidated";
    case CapabilityState::RuntimeValidated:
        return "RuntimeValidated";
    case CapabilityState::Blocked:
        return "Blocked";
    case CapabilityState::Disabled:
        return "Disabled";
    }
    return "unknown";
}

void ApplyFailClosedMutationPolicy(CapabilityMatrix& matrix) {
    matrix.Set(
        Capability::ScheduleGameThreadWork,
        CapabilityState::Blocked,
        "engine_thread_hooks_not_installed");
    matrix.Set(
        Capability::CreateDummyParticipant,
        CapabilityState::Blocked,
        "participant_ctor_copy_append_and_F4_lookup_are_static_only_requires_validated_tick_coherent_registry_count_below_8_and_transactional_rollback");
    matrix.Set(
        Capability::ReplayCharacterSpawn,
        CapabilityState::Blocked,
        "requires_current_generation_F0_and_130_captures_validated_game_thread_and_exactly_once_second_readiness");
    matrix.Set(
        Capability::BindDummyCharacter,
        CapabilityState::Blocked,
        "remote_flags_F4_lookup_and_bind_consumer_are_static_only_dummy_binding_not_runtime_observed");
    matrix.Set(
        Capability::DestroyDummyCharacter,
        CapabilityState::Blocked,
        "full_destroy_record_destroy_channel_context_release_and_participant_pop_require_runtime_order_proof");
    matrix.Set(
        Capability::ApplyInboundDescriptors,
        CapabilityState::Blocked,
        "native_reader_sink_descriptor_context_and_tick_ownership_not_runtime_validated");
}

} // namespace aimp
