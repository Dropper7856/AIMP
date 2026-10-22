#include "aimp/LoadingContextGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include <Windows.h>
#include <atomic>

namespace aimp::loading_context_guard {
namespace {
using Query = int(__thiscall*)(void*, std::uint32_t*);
Query gOriginal{};
std::atomic<bool> gInstalled{};
std::atomic<std::uint64_t> gNullContexts{}, gUnreadyContexts{};

bool Executable(const std::uintptr_t address) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
    constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & kExecute) != 0U && (info.Protect & PAGE_GUARD) == 0U;
}

// F46A90 -> F46C00 calls the optional frame callback at context+6AC. A second
// Host crash jumped to EIP=0x48 through it while the CRI player was still being
// constructed (allocation fill 0xCDCDCDCD in the same object). A set callback
// must point to executable code; otherwise the player is not ready yet.
bool ContextReady(void* context) noexcept {
    __try {
        const auto callback = *reinterpret_cast<const std::uintptr_t*>(static_cast<const std::byte*>(context) + 0x6ACU);
        return callback == 0U || Executable(callback);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Certified caller F4D010 tests owner+24 but not owner+24->4. The Host
// crash recorded ECX=0 at F46ABA, reached through F4D056 -> F468B0.
// Returning zero follows the caller's existing "not ready" branch F4D05D.
int __fastcall QueryReady(void* context, void*, std::uint32_t* output) noexcept {
    if (context != nullptr && ContextReady(context)) return gOriginal(context, output);
    if (output != nullptr) *output = 0U;
    (context == nullptr ? gNullContexts : gUnreadyContexts).fetch_add(1U, std::memory_order_relaxed);
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
         {"nullContextsDeferred", Diagnostics::Unsigned(gNullContexts.load(std::memory_order_relaxed)), true},
         {"unreadyContextsDeferred", Diagnostics::Unsigned(gUnreadyContexts.load(std::memory_order_relaxed)), true}});
}
}
