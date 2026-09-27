#include "aimp/SignatureResolver.h"

#include "aimp/CrashGuard.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace aimp {
namespace {

struct ExecutableSection {
    std::uint32_t rva{};
    std::uint32_t size{};
    std::string name;
};

std::optional<ExecutableSection> FindLoadedTextSection(
    const std::byte* moduleBase,
    const std::size_t imageSize) {
    if (moduleBase == nullptr || imageSize < sizeof(IMAGE_DOS_HEADER)) {
        return std::nullopt;
    }
    IMAGE_DOS_HEADER dos{};
    if (!crash_guard::TryCopy(&dos, moduleBase, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0) {
        return std::nullopt;
    }
    const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
    if (ntOffset > imageSize ||
        imageSize - ntOffset < sizeof(IMAGE_NT_HEADERS32)) {
        return std::nullopt;
    }
    IMAGE_NT_HEADERS32 nt{};
    if (!crash_guard::TryCopy(
            &nt,
            moduleBase + ntOffset,
            sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 96) {
        return std::nullopt;
    }
    const auto sectionOffset =
        ntOffset + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    if (sectionOffset > imageSize ||
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) >
            (imageSize - sectionOffset) / sizeof(IMAGE_SECTION_HEADER)) {
        return std::nullopt;
    }
    for (std::uint16_t index = 0;
         index < nt.FileHeader.NumberOfSections;
         ++index) {
        IMAGE_SECTION_HEADER section{};
        if (!crash_guard::TryCopy(
                &section,
                moduleBase + sectionOffset +
                    static_cast<std::size_t>(index) *
                        sizeof(IMAGE_SECTION_HEADER),
                sizeof(section))) {
            return std::nullopt;
        }
        std::array<char, IMAGE_SIZEOF_SHORT_NAME + 1> name{};
        std::memcpy(name.data(), section.Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::string_view(name.data()) != ".text" ||
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            (section.Characteristics & IMAGE_SCN_CNT_CODE) == 0) {
            continue;
        }
        const auto size =
            (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (size == 0 ||
            section.VirtualAddress >= imageSize ||
            size > imageSize - section.VirtualAddress) {
            return std::nullopt;
        }
        return ExecutableSection{
            section.VirtualAddress,
            size,
            std::string(name.data())};
    }
    return std::nullopt;
}

} // namespace

SignatureResolver::SignatureResolver(
    const void* moduleBase,
    const std::size_t imageSize,
    const BuildProfile& profile,
    const bool exactBuildValidated) noexcept
    : moduleBase_(static_cast<const std::byte*>(moduleBase)),
      imageSize_(imageSize),
      profile_(&profile),
      exactBuildValidated_(exactBuildValidated) {}

const void* SignatureResolver::Resolve(const SymbolId id) const noexcept {
    return ResolveWithEvidence(id).address;
}

SymbolResolution SignatureResolver::ResolveWithEvidence(
    const SymbolId id) const noexcept {
    try {
        SymbolResolution result{};
        if (moduleBase_ == nullptr || profile_ == nullptr) {
            return result;
        }
        const auto* spec = profile_->Find(id);
        if (spec == nullptr) {
            return result;
        }
        result.fixedByteCount = static_cast<std::size_t>(std::count(
            spec->expected.mask.begin(),
            spec->expected.mask.end(),
            'x'));

        if (exactBuildValidated_) {
            if (spec->rva >= imageSize_) {
                return result;
            }
            const auto* address = moduleBase_ + spec->rva;
            if (spec->expected.bytes.empty()) {
                result.address = address;
                result.kind = ResolutionKind::ExactProfile;
                result.matchCount = 1;
                return result;
            }
            if (!spec->expected.IsValid() ||
                spec->expected.bytes.size() > imageSize_ - spec->rva) {
                return result;
            }

            std::vector<std::byte> actual(spec->expected.bytes.size());
            if (!crash_guard::TryCopy(actual.data(), address, actual.size()) ||
                !spec->expected.Matches(actual)) {
                return result;
            }
            result.address = address;
            result.kind = ResolutionKind::ExactProfile;
            result.matchCount = 1;
            return result;
        }

        // Generic discovery is deliberately stricter than exact-profile
        // lookup. Empty/data symbols and short/common prologues are not
        // portable evidence. This only establishes an observed candidate;
        // call-graph/ABI/runtime invariants must gate hooks and mutations.
        constexpr std::size_t kMinimumSignatureBytes = 12;
        constexpr std::size_t kMinimumFixedBytes = 8;
        if (!spec->expected.IsValid() ||
            spec->expected.bytes.size() < kMinimumSignatureBytes ||
            result.fixedByteCount < kMinimumFixedBytes) {
            return result;
        }

        const auto section = FindLoadedTextSection(moduleBase_, imageSize_);
        if (!section.has_value()) {
            return result;
        }
        result.sectionRva = section->rva;
        result.sectionSize = section->size;
        result.sectionName = section->name;

        std::vector<std::byte> text(section->size);
        const auto* textAddress = moduleBase_ + section->rva;
        if (!crash_guard::TryCopy(text.data(), textAddress, text.size())) {
            return result;
        }
        result.matchCount = CountMatches(text, spec->expected, 2);
        if (result.matchCount == 0) {
            result.kind = ResolutionKind::Missing;
            return result;
        }
        if (result.matchCount != 1) {
            result.kind = ResolutionKind::Ambiguous;
            return result;
        }
        const auto offset = FindUnique(text, spec->expected);
        if (!offset.has_value()) {
            result.kind = ResolutionKind::Ambiguous;
            return result;
        }
        result.address = textAddress + *offset;
        result.kind = ResolutionKind::GenericSignature;
        return result;
    } catch (...) {
        return {};
    }
}

void* SignatureResolver::ResolveMutable(const SymbolId id) const noexcept {
    return const_cast<void*>(Resolve(id));
}

bool SignatureResolver::ValidateAllCodeSymbols() const noexcept {
    if (!exactBuildValidated_ || profile_ == nullptr) {
        return false;
    }
    for (const auto& symbol : profile_->symbols) {
        if (!symbol.expected.bytes.empty() && Resolve(symbol.id) == nullptr) {
            return false;
        }
    }
    return true;
}

bool SignatureResolver::ExactBuildValidated() const noexcept {
    return exactBuildValidated_;
}

std::optional<std::size_t> SignatureResolver::FindUnique(
    const std::span<const std::byte> haystack,
    const MaskedBytes& pattern) noexcept {
    if (!pattern.IsValid() ||
        pattern.bytes.empty() ||
        pattern.bytes.size() > haystack.size()) {
        return std::nullopt;
    }

    std::optional<std::size_t> match;
    const auto last = haystack.size() - pattern.bytes.size();
    for (std::size_t offset = 0; offset <= last; ++offset) {
        if (!pattern.Matches(haystack.subspan(offset, pattern.bytes.size()))) {
            continue;
        }
        if (match.has_value()) {
            return std::nullopt;
        }
        match = offset;
    }
    return match;
}

std::size_t SignatureResolver::CountMatches(
    const std::span<const std::byte> haystack,
    const MaskedBytes& pattern,
    const std::size_t stopAfter) noexcept {
    if (!pattern.IsValid() ||
        pattern.bytes.empty() ||
        pattern.bytes.size() > haystack.size() ||
        stopAfter == 0) {
        return 0;
    }
    std::size_t matches = 0;
    const auto last = haystack.size() - pattern.bytes.size();
    for (std::size_t offset = 0; offset <= last; ++offset) {
        if (!pattern.Matches(haystack.subspan(offset, pattern.bytes.size()))) {
            continue;
        }
        ++matches;
        if (matches >= stopAfter) {
            return matches;
        }
    }
    return matches;
}

} // namespace aimp
