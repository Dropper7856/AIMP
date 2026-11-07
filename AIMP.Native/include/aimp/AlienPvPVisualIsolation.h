#pragma once
#include <cstdint>

namespace aimp::alien_pvp_visual {
// Engine-thread-only, fixed-capacity submodel visibility for the technical
// THE_PLAYER and the optional first-person Alien head. No stored submodel or
// native flags pointer survives between frames. An unsupported retail model
// fails closed: caller must not enable local possession controls.
bool HideTechnical(std::uintptr_t player,std::uint32_t generation) noexcept;
bool HideAlienHead(std::uintptr_t alien,std::uint32_t generation) noexcept;
void RestoreTechnical(std::uintptr_t player,std::uint32_t generation,bool provenLive) noexcept;
void RestoreAlienHead(std::uintptr_t alien,std::uint32_t generation,bool provenLive) noexcept;
void ForgetAll() noexcept;
} // namespace aimp::alien_pvp_visual
