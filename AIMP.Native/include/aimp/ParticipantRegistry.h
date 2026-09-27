#pragma once

#include "aimp/Common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aimp {

struct ParticipantSnapshot {
    std::uint32_t index{};
    std::array<std::byte, 16> identity128{};
    std::uint32_t flagsC8{};
    std::uintptr_t associatedObjectD0{};
    std::uint32_t associatedValueD4{};
    std::uint32_t keyF4{};
};

struct ParticipantTableSnapshot {
    std::uintptr_t owner{};
    std::uintptr_t table{};
    std::uint32_t count{};
    std::vector<ParticipantSnapshot> participants;
    bool consistent{};
    bool valid{};
    std::string rejectionReason;
};

class ParticipantRegistry final {
public:
    [[nodiscard]] static constexpr ModuleStatus Status() noexcept {
        return ModuleStatus::PartiallyImplemented;
    }

    // The owner address is not discovered statically. This read-only method is
    // available for a future observed owner pointer.
    bool Inspect(
        std::uintptr_t ownerAddress,
        ParticipantTableSnapshot& output) const noexcept;
};

} // namespace aimp
