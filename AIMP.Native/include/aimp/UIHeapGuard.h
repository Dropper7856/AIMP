#pragma once

#include <cstdint>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::ui_heap_guard {
// Exact retail GFx heap mutators. Use their existing critical sections even
// when the heap descriptor opted out, because AS3 Socket workers share them.
bool Install(std::uintptr_t base, HookManager& hooks, std::string& error);
bool Installed() noexcept;
}
