#pragma once
#include <cstdint>

namespace aimp::alien_pvp_input {
// A scoped input firewall, NOT a global USER32 detour. It validates the exact
// retail executable's imported API pointers and swaps only its own IAT slots.
// Our DLL's GetAsyncKeyState import remains untouched for Alien WASD polling.
// All observers transparently call the original functions outside local PvP.
bool EnsureInstalled(std::uintptr_t gameModule) noexcept;
bool Installed() noexcept;
void SetActive(bool enabled) noexcept;
void Uninstall() noexcept;
int TakeCameraWheel() noexcept;
void SetMapMode(bool enabled) noexcept;
struct MapPointer {float x{480},y{270};int wheel{};bool down{},clicked{};};
MapPointer TakeMapPointer() noexcept;
} // namespace aimp::alien_pvp_input
