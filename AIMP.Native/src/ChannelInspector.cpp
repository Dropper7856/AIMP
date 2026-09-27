#include "aimp/ChannelInspector.h"

#include "aimp/CrashGuard.h"

#include <algorithm>
#include <utility>

namespace aimp {
namespace {

constexpr std::size_t kNetzBytes = 0x6A0;
constexpr std::size_t kChannelBase = 0x18;
constexpr std::size_t kChannelStride = 0x30;
constexpr std::size_t kLogicalIds = 0x618;
constexpr std::size_t kNextLogicalId = 0x698;
constexpr std::size_t kTransport = 0x69C;
constexpr std::uint32_t kMaximumObservedEntries = 512;
constexpr std::uint32_t kMaximumObservedCapacity = 4096;

template <typename T>
bool ReadAt(const std::uintptr_t base, const std::size_t offset, T& output) noexcept {
    const auto value = crash_guard::SafeRead<T>(
        reinterpret_cast<const void*>(base + offset));
    if (!value.has_value()) {
        return false;
    }
    output = *value;
    return true;
}

} // namespace

ChannelInspector::ChannelInspector(const DescriptorRegistry& registry) noexcept
    : registry_(&registry) {}

bool ChannelInspector::Inspect(
    const std::uintptr_t netzAddress,
    NetzSnapshot& output) const noexcept {
    try {
        output = {};
        output.address = netzAddress;
        output.descriptorDigest = 1469598103934665603ULL;
        if (netzAddress == 0 ||
            registry_ == nullptr ||
            !crash_guard::IsReadable(
                reinterpret_cast<const void*>(netzAddress),
                kNetzBytes)) {
            return false;
        }

        std::uint32_t activeMaskBefore = 0;
        std::uint32_t freeMaskBefore = 0;
        if (!ReadAt(netzAddress, 0x00, output.vtable) ||
            !ReadAt(netzAddress, 0x08, output.freeSummary) ||
            !ReadAt(netzAddress, 0x0C, freeMaskBefore) ||
            !ReadAt(netzAddress, 0x10, output.activeSummary) ||
            !ReadAt(netzAddress, 0x14, activeMaskBefore) ||
            !ReadAt(netzAddress, kNextLogicalId, output.nextLogicalId) ||
            !ReadAt(netzAddress, kTransport, output.transport)) {
            return false;
        }
        output.freeMask = freeMaskBefore;
        output.activeMask = activeMaskBefore;
        bool registryHeadersStable = true;

        for (std::uint32_t index = 0; index < output.channels.size(); ++index) {
            auto& channel = output.channels[index];
            channel.index = index;
            channel.active = ((activeMaskBefore >> index) & 1U) != 0;
            channel.free = ((freeMaskBefore >> index) & 1U) != 0;
            const auto address =
                netzAddress + kChannelBase + index * kChannelStride;

            std::uintptr_t entries = 0;
            if (!ReadAt(netzAddress, kLogicalIds + index * 4, channel.logicalId) ||
                !ReadAt(address, 0x1C, entries) ||
                !ReadAt(address, 0x20, channel.entryCount) ||
                !ReadAt(address, 0x24, channel.entryCapacity) ||
                !ReadAt(address, 0x2A, channel.streamOneState) ||
                !ReadAt(address, 0x2B, channel.streamZeroState) ||
                !ReadAt(address, 0x2C, channel.mode)) {
                return false;
            }

            channel.registryValid =
                channel.entryCount <= channel.entryCapacity &&
                channel.entryCount <= kMaximumObservedEntries &&
                channel.entryCapacity <= kMaximumObservedCapacity &&
                (channel.entryCount == 0 || entries != 0);

            std::vector<std::pair<std::uintptr_t, std::uintptr_t>>
                observedRegistryEntries;
            if (channel.registryValid) {
                observedRegistryEntries.reserve(channel.entryCount);
                channel.descriptors.reserve(channel.entryCount);
                for (std::uint32_t entryIndex = 0;
                     entryIndex < channel.entryCount;
                     ++entryIndex) {
                    const auto descriptorCore =
                        crash_guard::SafeRead<std::uintptr_t>(
                            reinterpret_cast<const void*>(
                                entries + entryIndex * 8));
                    const auto auxiliary =
                        crash_guard::SafeRead<std::uintptr_t>(
                            reinterpret_cast<const void*>(
                                entries + entryIndex * 8 + 4));
                    if (!descriptorCore.has_value() ||
                        !auxiliary.has_value()) {
                        channel.registryValid = false;
                        break;
                    }
                    observedRegistryEntries.emplace_back(
                        *descriptorCore,
                        *auxiliary);
                    if (*descriptorCore == 0) {
                        continue;
                    }
                    DescriptorSnapshot descriptor{};
                    if (!registry_->InspectCore(
                            *descriptorCore,
                            descriptor)) {
                        channel.registryValid = false;
                        break;
                    }
                    descriptor.auxiliaryAddress = *auxiliary;
                    channel.descriptors.push_back(std::move(descriptor));
                }
            }

            std::uintptr_t entriesAfter = 0;
            std::uint32_t countAfter = 0;
            std::uint32_t capacityAfter = 0;
            if (!ReadAt(address, 0x1C, entriesAfter) ||
                !ReadAt(address, 0x20, countAfter) ||
                !ReadAt(address, 0x24, capacityAfter)) {
                return false;
            }
            channel.registryStable =
                entriesAfter == entries &&
                countAfter == channel.entryCount &&
                capacityAfter == channel.entryCapacity;
            if (channel.registryStable &&
                channel.registryValid &&
                observedRegistryEntries.size() == channel.entryCount) {
                for (std::uint32_t entryIndex = 0;
                     entryIndex < channel.entryCount;
                     ++entryIndex) {
                    const auto descriptorCore =
                        crash_guard::SafeRead<std::uintptr_t>(
                            reinterpret_cast<const void*>(
                                entries + entryIndex * 8));
                    const auto auxiliary =
                        crash_guard::SafeRead<std::uintptr_t>(
                            reinterpret_cast<const void*>(
                                entries + entryIndex * 8 + 4));
                    if (!descriptorCore.has_value() ||
                        !auxiliary.has_value() ||
                        observedRegistryEntries[entryIndex] !=
                            std::pair{*descriptorCore, *auxiliary}) {
                        channel.registryStable = false;
                        break;
                    }
                }
            }
            if (!channel.registryStable) {
                registryHeadersStable = false;
                channel.registryValid = false;
                channel.descriptors.clear();
            }
            if (!channel.registryValid) {
                channel.descriptors.clear();
            }
            if (channel.registryValid) {
                for (const auto& descriptor : channel.descriptors) {
                    output.descriptorDigest =
                        DescriptorRegistry::AddToDigest(
                            output.descriptorDigest,
                            descriptor);
                }
            }
        }

        std::uint32_t activeMaskAfter = 0;
        std::uint32_t freeMaskAfter = 0;
        if (!ReadAt(netzAddress, 0x14, activeMaskAfter) ||
            !ReadAt(netzAddress, 0x0C, freeMaskAfter)) {
            return false;
        }
        output.consistent =
            activeMaskBefore == activeMaskAfter &&
            freeMaskBefore == freeMaskAfter &&
            registryHeadersStable;
        if (!output.consistent) {
            output = {};
            return false;
        }
        return true;
    } catch (...) {
        output.consistent = false;
        return false;
    }
}

} // namespace aimp
