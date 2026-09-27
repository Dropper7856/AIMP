#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace aimp {

enum class Capability : std::uint8_t {
    ObserveNetZ,
    ObserveCharacterRequests,
    ObserveCharacterLifecycle,
    ObserveParticipants,
    ObserveChannels,
    ObserveReplicationContexts,
    ObserveLevelTransitions,
    ScheduleGameThreadWork,
    CreateDummyParticipant,
    ReplayCharacterSpawn,
    BindDummyCharacter,
    DestroyDummyCharacter,
    ObserveOutboundDescriptors,
    ApplyInboundDescriptors,
    Count,
};

enum class CapabilityState : std::uint8_t {
    Unresolved,
    Observed,
    StaticValidated,
    RuntimeValidated,
    Blocked,
    Disabled,
};

struct CapabilityRecord {
    Capability capability{};
    CapabilityState state{CapabilityState::Unresolved};
    std::string reason;
};

class CapabilityMatrix final {
public:
    CapabilityMatrix();

    void Set(
        Capability capability,
        CapabilityState state,
        std::string reason);
    [[nodiscard]] CapabilityRecord Get(Capability capability) const;
    [[nodiscard]] std::array<CapabilityRecord, static_cast<std::size_t>(
        Capability::Count)> Snapshot() const;
    [[nodiscard]] bool IsEnabled(Capability capability) const;
    [[nodiscard]] bool IsAtLeastResolved(Capability capability) const;

private:
    mutable std::mutex mutex_;
    std::array<CapabilityRecord, static_cast<std::size_t>(
        Capability::Count)> records_{};
};

[[nodiscard]] std::string_view ToString(Capability capability) noexcept;
[[nodiscard]] std::string_view ToString(CapabilityState state) noexcept;
void ApplyFailClosedMutationPolicy(CapabilityMatrix& matrix);

} // namespace aimp
