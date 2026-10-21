#include "aimp/LoadingContextGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include <atomic>

namespace aimp::loading_context_guard {
namespace {
using Query = int(__thiscall*)(void*, std::uint32_t*);
Query gOriginal{};
std::atomic<bool> gInstalled{};
std::atomic<std::uint64_t> gNullContexts{};

// Certified caller F4D010 tests owner+24 but not owner+24->4. The Host
// crash recorded ECX=0 at F46ABA, reached through F4D056 -> F468B0.
// Returning zero follows the caller's existing "not ready" branch F4D05D.
int __fastcall QueryReady(void* context, void*, std::uint32_t* output) noexcept {
    if (context != nullptr) return gOriginal(context, output);
    if (output != nullptr) *output = 0U;
    gNullContexts.fetch_add(1U, std::memory_order_relaxed);
    return 0;
}
}

bool Install(const std::uintptr_t base, HookManager& hooks, std::string& error) {
    if (Installed()) return true;
    if (base == 0U || !hooks.MutationAllowed()) {
        error = "Loading context guard requires the certified executable";
        return false;
    }
    gOriginal = reinterpret_cast<Query>(base + 0x00B468B0U);
    if (!hooks.InstallCallRedirect({"LoadingContextGuard.NativeReadiness",
        reinterpret_cast<void*>(base + 0x00B4D056U),
        reinterpret_cast<void*>(base + 0x00B468B0U),
        reinterpret_cast<void*>(&QueryReady)}, error)) return false;
    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }
void LogDiagnostics(Diagnostics& diagnostics) {
    diagnostics.Log("info", "loading_context_guard",
        "A null native context follows the caller's existing not-ready branch; valid contexts execute retail unchanged.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"nullContextsDeferred", Diagnostics::Unsigned(gNullContexts.load(std::memory_order_relaxed)), true}});
}
}
