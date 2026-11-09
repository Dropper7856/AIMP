#pragma once
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

// AnimationSync: one generic replication contract for every scripted
// animation the local player performs (CMD_PlayAnimation on THE_PLAYER:
// buttons, generators, terminals, vents, levers, hacking...). Nothing here is
// per-animation: the alias token is CATHODE's own FNV-1a animation name, which
// the receiver resolves in its representation's clip database. The launcher's
// ANIMATION.PAK preparation gives every crew representation the player's
// first-person aliases, so the engine retargets FEMALEFP clips natively.
namespace aimp::animation_sync {
struct State final {
    std::uint64_t transition{};
    std::uint32_t animation{};
    // Age of this phase/transform observation, not time since clip entry.
    std::uint32_t elapsedMilliseconds{};
    // Native CMD_PlayAnimation parameters copied from the local cycle.
    std::int32_t startFrame{-1}, endFrame{-1}, loops{1};
    float speed{1.0F}, blendIn{0.2F};
    // Current owner transform and native graph clock. Progress refreshes do
    // not create a new transition, including during an indefinite loop.
    std::array<float, 3> position{};
    float yaw{};
    float phaseSeconds{}, durationSeconds{};
    std::array<std::uint32_t, 4> attachments{};
    bool operator==(const State&) const noexcept = default;
};
// Retain the proxy's standing height during small vertical movements made by
// the first-person character while leaning/crouching in an interaction. Large
// vertical travel (ladders, vents, level changes) is never suppressed.
[[nodiscard]] inline float PosturalHeightOffset(float baseline, float ownerStartY, float ownerY) noexcept {
    if (!std::isfinite(baseline) || !std::isfinite(ownerStartY) || !std::isfinite(ownerY)) return baseline;
    const auto delta = ownerStartY - ownerY;
    const auto distance = std::abs(delta);
    // Smoothly fade compensation; the old 25-cm switch jerked the visible
    // character during first-person locker leaning.
    if (distance <= 0.35F) return baseline + delta;
    if (distance >= 0.75F) return baseline;
    return baseline + delta * ((0.75F - distance) / 0.40F);
}
inline constexpr std::uint32_t kMaximumElapsedMilliseconds = 60000U;
inline bool Active(const State& value) noexcept { return value.transition != 0U; }
inline bool Valid(const State& value) noexcept {
    if (!Active(value)) return value == State{};
    if (value.animation == 0U || value.elapsedMilliseconds > kMaximumElapsedMilliseconds ||
        value.startFrame < -1 || value.startFrame > 1000000 || value.endFrame < -1 || value.endFrame > 1000000 ||
        value.loops < -1 || value.loops > 65535 ||
        !std::isfinite(value.speed) || value.speed <= 0.0F || value.speed > 64.0F ||
        !std::isfinite(value.blendIn) || value.blendIn < 0.0F || value.blendIn > 60.0F ||
        !std::isfinite(value.yaw) || std::abs(value.yaw) > 3.3F ||
        !std::isfinite(value.phaseSeconds) || value.phaseSeconds < 0.0F || value.phaseSeconds > 100000.0F ||
        !std::isfinite(value.durationSeconds) || value.durationSeconds <= 0.0F || value.durationSeconds > 100000.0F) return false;
    for (const auto coordinate : value.position)
        if (!std::isfinite(coordinate) || std::abs(coordinate) > 1000000.0F) return false;
    return true;
}
// "0" (no clip) or 18 comma-separated fields, matching AIMP.Protocol's
// PlayerAnimationCodec: transition,animation,elapsed,start,end,loops,speed,
// blendIn,x,y,z,yaw,phaseSeconds,durationSeconds,attachment0..3.
std::string Encode(const State& value);
inline bool Decode(std::string_view text, State& output) noexcept {
    if (text == "0") { output = {}; return true; }
    if (text.size() > 384U) return false;
    std::array<std::string_view, 18> fields{};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto comma = text.find(',');
        if ((i + 1 < fields.size()) != (comma != std::string_view::npos)) return false;
        fields[i] = text.substr(0, comma);
        if (comma != std::string_view::npos) text.remove_prefix(comma + 1);
    }
    const auto parse = [](std::string_view s, auto& value) noexcept {
        const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
        return !s.empty() && result.ec == std::errc{} && result.ptr == s.data() + s.size();
    };
    State value{};
    if (!parse(fields[0], value.transition) || !parse(fields[1], value.animation) ||
        !parse(fields[2], value.elapsedMilliseconds) || !parse(fields[3], value.startFrame) ||
        !parse(fields[4], value.endFrame) || !parse(fields[5], value.loops) ||
        !parse(fields[6], value.speed) || !parse(fields[7], value.blendIn) ||
        !parse(fields[8], value.position[0]) || !parse(fields[9], value.position[1]) ||
        !parse(fields[10], value.position[2]) || !parse(fields[11], value.yaw) ||
        !parse(fields[12], value.phaseSeconds) || !parse(fields[13], value.durationSeconds) ||
        !parse(fields[14], value.attachments[0]) || !parse(fields[15], value.attachments[1]) ||
        !parse(fields[16], value.attachments[2]) || !parse(fields[17], value.attachments[3]) ||
        value.transition == 0U || !Valid(value)) return false;
    output = value;
    return true;
}
}
