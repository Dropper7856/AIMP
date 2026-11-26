#pragma once

#include <cstdint>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::ui_heap_guard {
// Exact retail GFx heap mutators. Use their existing critical sections even
// when the heap descriptor opted out, because AS3 Socket workers share them.
bool Install(std::uintptr_t base, HookManager& hooks, std::string& error);
bool Installed() noexcept;
// AvS 2026-11-25 round 5 crash (new match after an Alien win): retail
// PERSISTENT_ACT_GUN_LAYER update 006742B0 read [held item + 0x50] at 00674921
// with no held item (a remote survivor replica whose weapon was still being
// re-provisioned). Its only caller (00674A16) is redirected to a wrapper that
// skips that frame's update on exactly this fault. Engine thread.
bool InstallGunLayerGuard(std::uintptr_t base, HookManager& hooks, std::string& error);
bool GunLayerGuardInstalled() noexcept;
std::uint64_t GunLayerFaultsSkipped() noexcept;
}
