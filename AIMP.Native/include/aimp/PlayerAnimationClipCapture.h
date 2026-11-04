#pragma once
#include <cstdint>
#include <string>
namespace aimp { class HookManager; class Diagnostics; }
namespace aimp::player_animation_clip_capture {
// Passive probes of the two retail clip resolvers. They preserve every
// argument, result and resource reference; no replacement animation is played.
bool Install(std::uintptr_t moduleBase, HookManager& hooks, std::string& error);
void Clear() noexcept;
void Export(Diagnostics& diagnostics);
}
