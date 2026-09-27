#include "aimp/DescriptorRegistry.h"

#include "aimp/CrashGuard.h"

#include <limits>

namespace aimp {

DescriptorRegistry::DescriptorRegistry(
    const void* moduleBase,
    const std::size_t imageSize,
    const std::uint32_t textRva,
    const std::uint32_t textSize) noexcept
    : moduleBase_(reinterpret_cast<std::uintptr_t>(moduleBase)),
      imageSize_(imageSize),
      textBegin_(moduleBase_ + textRva),
      textEnd_(textBegin_ + textSize) {}

bool DescriptorRegistry::InspectCore(
    const std::uintptr_t descriptorCore,
    DescriptorSnapshot& output) const noexcept {
    try {
        output = {};
        output.coreAddress = descriptorCore;
        if (descriptorCore == 0 ||
            !crash_guard::IsInRange(
                reinterpret_cast<const void*>(descriptorCore),
                10,
                reinterpret_cast<const void*>(moduleBase_),
                imageSize_)) {
            return false;
        }

        const auto nameAddress = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(descriptorCore));
        const auto decoderAddress = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(descriptorCore + 4));
        const auto runtimeId = crash_guard::SafeRead<std::uint16_t>(
            reinterpret_cast<const void*>(descriptorCore + 8));
        if (!nameAddress.has_value() ||
            !decoderAddress.has_value() ||
            !runtimeId.has_value() ||
            *decoderAddress < textBegin_ ||
            *decoderAddress >= textEnd_ ||
            !ReadBoundedString(*nameAddress, output.name)) {
            return false;
        }

        output.nameAddress = *nameAddress;
        output.decoderAddress = *decoderAddress;
        output.decoderRva =
            static_cast<std::uint32_t>(*decoderAddress - moduleBase_);
        output.runtimeId = *runtimeId;
        output.valid = true;
        return true;
    } catch (...) {
        output.valid = false;
        return false;
    }
}

std::uint64_t DescriptorRegistry::AddToDigest(
    std::uint64_t digest,
    const DescriptorSnapshot& descriptor) noexcept {
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (const auto character : descriptor.name) {
        digest ^= static_cast<std::uint8_t>(character);
        digest *= kPrime;
    }
    digest ^= descriptor.runtimeId & 0xFFU;
    digest *= kPrime;
    digest ^= (descriptor.runtimeId >> 8U) & 0xFFU;
    digest *= kPrime;
    const auto decoder = descriptor.decoderRva;
    for (std::size_t index = 0; index < 4; ++index) {
        digest ^= (decoder >> (index * 8U)) & 0xFFU;
        digest *= kPrime;
    }
    return digest;
}

bool DescriptorRegistry::ReadBoundedString(
    const std::uintptr_t address,
    std::string& output) const noexcept {
    try {
        constexpr std::size_t kMaximumName = 96;
        if (address < moduleBase_ ||
            address >= moduleBase_ + imageSize_) {
            return false;
        }
        const auto available = moduleBase_ + imageSize_ - address;
        output.clear();
        output.reserve(48);
        for (std::size_t index = 0;
             index < kMaximumName && index < available;
             ++index) {
            const auto character = crash_guard::SafeRead<char>(
                reinterpret_cast<const void*>(address + index));
            if (!character.has_value()) {
                return false;
            }
            if (*character == '\0') {
                return !output.empty();
            }
            const auto byte = static_cast<unsigned char>(*character);
            if (byte < 0x20U || byte > 0x7EU) {
                return false;
            }
            output.push_back(*character);
        }
        return false;
    } catch (...) {
        return false;
    }
}

} // namespace aimp
