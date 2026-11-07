#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace aimp::player_characters {
// Crew representations imported from BSPNOSTROMO_TWOTEAMS (mirror of the
// launcher's PlayerCharacterCatalog). Each PODLC_TwoTeams Character differs
// only by anim_set/reference_skeleton/display_model (= its name, FNV-1a 32)
// and its DisplayModel:<NAME> composite ShortGuid.
struct Identity final {
    std::string_view name;
    std::uint32_t nameHash{};
    std::uint32_t displayModelGuid{};
};

inline constexpr std::array<Identity, 5> kAll{{
    {"E_RIPLEY", 0x21F92DEAU, 0xD9A16D5BU},
    {"DALLAS", 0xE4CDDDE2U, 0x70BD7431U},
    {"PARKER", 0x1A576592U, 0xEAC1FB77U},
    {"LAMBERT", 0xB473FB3AU, 0x358D9B30U},
    {"ASH", 0x5BAD7BDFU, 0x127E16ABU},
}};

inline constexpr const Identity& kDefault = kAll[0];

[[nodiscard]] constexpr const Identity* Find(const std::string_view name) noexcept {
    for (const auto& identity : kAll) if (identity.name == name) return &identity;
    return nullptr;
}

// A representation's animation/clip database identity (clipDB+0x10).
[[nodiscard]] constexpr bool IsRepresentation(const std::uint32_t nameHash) noexcept {
    for (const auto& identity : kAll) if (identity.nameHash == nameHash) return true;
    return false;
}
}
