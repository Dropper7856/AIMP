#pragma once

#include "aimp/DescriptorRegistry.h"
#include "aimp/NativeLimits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aimp {

struct ChannelSnapshot {
    std::uint32_t index{};
    std::uint32_t logicalId{};
    bool active{};
    bool free{};
    std::uint8_t streamOneState{};
    std::uint8_t streamZeroState{};
    std::uint8_t mode{};
    std::uint32_t entryCount{};
    std::uint32_t entryCapacity{};
    std::vector<DescriptorSnapshot> descriptors;
    bool registryStable{};
    bool registryValid{};
};

struct NetzSnapshot {
    std::uintptr_t address{};
    std::uintptr_t vtable{};
    std::uintptr_t transport{};
    std::uint32_t freeSummary{};
    std::uint32_t freeMask{};
    std::uint32_t activeSummary{};
    std::uint32_t activeMask{};
    std::uint32_t nextLogicalId{};
    std::array<ChannelSnapshot, kNativeChannelCapacity> channels{};
    std::uint64_t descriptorDigest{1469598103934665603ULL};
    bool consistent{};
};

class ChannelInspector final {
public:
    explicit ChannelInspector(const DescriptorRegistry& registry) noexcept;

    bool Inspect(
        std::uintptr_t netzAddress,
        NetzSnapshot& output) const noexcept;

private:
    const DescriptorRegistry* registry_{};
};

} // namespace aimp
