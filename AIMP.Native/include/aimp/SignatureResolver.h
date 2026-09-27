#pragma once

#include "aimp/BuildProfile.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace aimp {

enum class ResolutionKind : std::uint8_t {
    Missing,
    Ambiguous,
    ExactProfile,
    GenericSignature,
};

struct SymbolResolution {
    const void* address{};
    ResolutionKind kind{ResolutionKind::Missing};
    std::size_t matchCount{};
    std::size_t fixedByteCount{};
    std::uint32_t sectionRva{};
    std::uint32_t sectionSize{};
    std::string sectionName;
};

class SignatureResolver final {
public:
    SignatureResolver(
        const void* moduleBase,
        std::size_t imageSize,
        const BuildProfile& profile,
        bool exactBuildValidated) noexcept;

    [[nodiscard]] const void* Resolve(SymbolId id) const noexcept;
    [[nodiscard]] SymbolResolution ResolveWithEvidence(
        SymbolId id) const noexcept;
    [[nodiscard]] void* ResolveMutable(SymbolId id) const noexcept;
    [[nodiscard]] bool ValidateAllCodeSymbols() const noexcept;
    [[nodiscard]] bool ExactBuildValidated() const noexcept;

    static std::optional<std::size_t> FindUnique(
        std::span<const std::byte> haystack,
        const MaskedBytes& pattern) noexcept;
    static std::size_t CountMatches(
        std::span<const std::byte> haystack,
        const MaskedBytes& pattern,
        std::size_t stopAfter = 2) noexcept;

private:
    const std::byte* moduleBase_{};
    std::size_t imageSize_{};
    const BuildProfile* profile_{};
    bool exactBuildValidated_{};
};

} // namespace aimp
