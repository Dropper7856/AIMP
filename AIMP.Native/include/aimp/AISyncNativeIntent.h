#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aimp::ai_sync::native_intent {
inline constexpr std::uint32_t kKind = 0x200U, kProfile = 128U;
inline constexpr std::size_t kBytes = 144;
// 0043FC90 copies all three low flag bytes and only bits0/1 of byte+57.
// The remaining six bits are structure padding, not animation options.
inline constexpr std::uint32_t kScriptedFlags = 0x03FFFFFFU;
struct Payload {
    std::array<std::uint32_t, 36> words{};
    bool operator==(const Payload&) const = default;
};
using CanonicalPayload = std::array<std::uint8_t, kBytes>;
// 00401520 duration<=0 sets deadline -1; its virtual+60 (004015A0)
// deliberately ORs flags6 before hiding the native Character. This is a
// completed command, not a rejected initialization.
[[nodiscard]] inline bool CompletesAtConstruction(const Payload& p) noexcept {
    return p.words[0]==22U||(p.words[0]==23U&&
        std::isfinite(std::bit_cast<float>(p.words[2]))&&std::bit_cast<float>(p.words[2])<=0.0F);
}

[[nodiscard]] inline bool Empty(const Payload& value) noexcept {
    for (const auto word : value.words) if (word != 0) return false;
    return true;
}
[[nodiscard]] inline bool Validate(const Payload& value) noexcept {
    if (Empty(value)) return true;
    const auto& w = value.words;
    const auto zero = [&w](std::size_t begin, std::size_t end) noexcept {
        for (auto i = begin; i < end; ++i) if (w[i] != 0) return false;
        return true;
    };
    const auto real = [&w](std::size_t i, float low, float high) noexcept {
        const auto f = std::bit_cast<float>(w[i]);
        return std::isfinite(f) && f >= low && f <= high;
    };
    const auto integer = [&w](std::size_t i, std::int32_t high) noexcept {
        const auto n = std::bit_cast<std::int32_t>(w[i]);
        return n >= -1 && n <= high;
    };
    const auto id = w[0];
    if (id < 1 || id > 23 || id == 11 || id == 14 || id == 18 || w[35] != 0)
        return false;
    // 0064A8A0 (RET4) forwards a pointer to00401770, which copies only
    // XYZ through004013F0. The fourth vector lane is padding, never data.
    if (id == 21)
        return w[1] == 4 && zero(2, 8) && w[11] == 0 && zero(12, 35) &&
            std::isfinite(std::bit_cast<float>(w[8])) &&
            std::isfinite(std::bit_cast<float>(w[9])) &&
            std::isfinite(std::bit_cast<float>(w[10]));
    // 0064A9C0 takes no arguments; it supplies zero to the4D constructor.
    if (id == 22) return zero(1, 35);
    // 0064AA50 (RET4) supplies the caller's float duration to00401520.
    if (id == 23)
        return w[1] == 1 && std::isfinite(std::bit_cast<float>(w[2])) && zero(3, 35);
    if (id == 20) {
        // Four CATHODE RuntimeKeys {entityShortGuid,hierarchyHash}, resolved
        // locally through009236A0. Neither component is a native pointer.
        for (std::size_t i=12;i<20;i+=2) {
            if(w[i]==0 && w[i+1]!=0) return false;
            if((w[1]&(1U<<((i-12U)/2U)))!=0U && w[i]==0) return false;
        }
        // Resolution evidence, not native parameters: require only references
        // that the Host actually resolved. Retail permits absent optional refs.
        return (w[1]&~15U)==0U && zero(2, 12) && w[20] == 0 && w[21] != 0 && w[22] != 0 &&
            integer(23, 1000000) && integer(24, 1000000) && integer(25, 65535) &&
            real(26, 0.F, 64.F) && real(27, 0.F, 60.F) &&
            // +40: a timing scalar (Host observed 0.5), not a reserved zero.
            real(28, 0.F, 60.F) &&
            real(30, -1.F, 1000000.F) && real(31, 0.F, 60.F) &&
            real(32, -100000.F, 100000.F) && (w[33] & ~kScriptedFlags) == 0 && w[34] == 0;
    }
    if (!zero(12, 35)) return false;
    if (id <= 7) return zero(1, 12);
    if (id == 8 || id == 9 || id == 10 || id == 12 || id == 13)
        return w[1] == (id == 9 || id == 12 ? 0x11U : 1U) && (id == 9 || id == 12 || w[3] == 0) &&
            zero(4, 12) && (id != 13 || std::isfinite(std::bit_cast<float>(w[2])));
    if (id >= 15 && id <= 17)
        return zero(2, 4) && w[5] == 0 && zero(7, 12) &&
            ((id == 16 && w[1] == 5 && w[4] == 0 && w[6] == 0) ||
             (w[1] == 2 && (w[4] != 0 || w[6] != 0)));
    if (w[1] != 4 || !zero(2, 8)) return false;
    for (std::size_t i = 8; i < 12; ++i)
        if (!std::isfinite(std::bit_cast<float>(w[i]))) return false;
    return true;
}
[[nodiscard]] inline bool CaptureScripted(std::span<const std::uint32_t> parameters,
    std::uint32_t countOffset,Payload& output) noexcept {
    if(parameters.size()!=22U)return false;
    Payload p{};p.words[0]=20U;
    for(std::size_t i=0;i<22U;++i)p.words[12U+i]=parameters[i];
    p.words[33]&=kScriptedFlags;p.words[34]=countOffset;
    output=p;
    return Validate(output);
}
[[nodiscard]] inline bool Encode(const Payload& value, CanonicalPayload& output) noexcept {
    if (!Validate(value)) return false;
    for (std::size_t i = 0; i < value.words.size(); ++i)
        for (std::size_t b = 0; b < 4; ++b)
            output[i * 4 + b] = static_cast<std::uint8_t>(value.words[i] >> (b * 8));
    return true;
}
[[nodiscard]] inline bool Decode(std::span<const std::uint8_t> bytes, Payload& output) noexcept {
    if (bytes.size() != kBytes) return false;
    Payload value{};
    for (std::size_t i = 0; i < value.words.size(); ++i)
        for (std::size_t b = 0; b < 4; ++b)
            value.words[i] |= static_cast<std::uint32_t>(bytes[i * 4 + b]) << (b * 8);
    if (!Validate(value)) return false;
    output = value;
    return true;
}
} // namespace aimp::ai_sync::native_intent
