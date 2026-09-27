#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aimp {

struct DescriptorSnapshot {
    std::uintptr_t coreAddress{};
    std::uintptr_t nameAddress{};
    std::uintptr_t decoderAddress{};
    std::uintptr_t auxiliaryAddress{};
    std::uint32_t decoderRva{};
    std::uint16_t runtimeId{};
    std::string name;
    bool valid{};
};

class DescriptorRegistry final {
public:
    DescriptorRegistry(
        const void* moduleBase,
        std::size_t imageSize,
        std::uint32_t textRva,
        std::uint32_t textSize) noexcept;

    bool InspectCore(
        std::uintptr_t descriptorCore,
        DescriptorSnapshot& output) const noexcept;

    static std::uint64_t AddToDigest(
        std::uint64_t digest,
        const DescriptorSnapshot& descriptor) noexcept;

private:
    bool ReadBoundedString(std::uintptr_t address, std::string& output) const noexcept;

    std::uintptr_t moduleBase_{};
    std::size_t imageSize_{};
    std::uintptr_t textBegin_{};
    std::uintptr_t textEnd_{};
};

} // namespace aimp
