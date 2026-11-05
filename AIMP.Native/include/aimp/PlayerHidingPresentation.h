#pragma once
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace aimp::player_hiding_presentation {
struct State final {
    std::uint32_t phase{}, kind{}, entityShortGuid{}, hierarchyHash{};
    std::uint64_t transition{};
    std::uint32_t elapsedMilliseconds{};
    std::array<float, 3> position{};
    float yaw{};
    bool animate{};
};
inline bool Valid(const State& value) noexcept {
    if (value.phase == 0) {
        return value.kind == 0 && value.entityShortGuid == 0 && value.hierarchyHash == 0 &&
            value.transition == 0 && value.elapsedMilliseconds == 0 && value.position == std::array<float, 3>{}
            && value.yaw == 0 && !value.animate;
    }
    if (value.phase > 3 || value.kind < 1 || value.kind > 2 || value.hierarchyHash == 0 ||
        value.entityShortGuid != (value.kind == 1 ? 0x5DC33E3EU : 0x61B51D78U) ||
        value.transition == 0 || value.elapsedMilliseconds > 60000 ||
        !std::isfinite(value.yaw) || std::abs(value.yaw) > 3.3F) return false;
    for (const auto coordinate : value.position)
        if (!std::isfinite(coordinate) || std::abs(coordinate) > 1000000.F) return false;
    return true;
}
inline bool Decode(std::string_view text, State& output) noexcept {
    if (text == "0") { output = {}; return true; }
    if (text.size() > 224) return false;
    std::array<std::string_view, 11> fields{};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto comma = text.find(',');
        if ((i + 1 < fields.size()) != (comma != std::string_view::npos)) return false;
        fields[i] = text.substr(0, comma);
        if (comma != std::string_view::npos) text.remove_prefix(comma + 1);
    }
    const auto parse = [](std::string_view s, auto& value) noexcept {
        const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
        return result.ec == std::errc{} && result.ptr == s.data() + s.size();
    };
    State value{};
    if (!parse(fields[0], value.phase) || !parse(fields[1], value.kind) ||
        !parse(fields[2], value.entityShortGuid) || !parse(fields[3], value.hierarchyHash) ||
        !parse(fields[4], value.transition) || !parse(fields[5], value.elapsedMilliseconds) ||
        !parse(fields[6], value.position[0]) || !parse(fields[7], value.position[1]) ||
        !parse(fields[8], value.position[2]) || !parse(fields[9], value.yaw) ||
        (fields[10] != "0" && fields[10] != "1") || value.phase == 0) return false;
    value.animate = fields[10] == "1";
    if (!Valid(value)) return false;
    output = value; return true;
}
}
