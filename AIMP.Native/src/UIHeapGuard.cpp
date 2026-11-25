#include "aimp/UIHeapGuard.h"
#include "aimp/HookManager.h"

#include <array>
#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace aimp::ui_heap_guard {
namespace {
std::atomic<bool> gInstalled{};
struct Site { std::uintptr_t rva; std::uint8_t distance; };
// CMP byte [ESI+64],0 / JE unlocked. Keeping the fall-through preserves both
// the native lock order and its matching unlock (including Free's auto-release
// AFTER unlocking). Do not wrap Free while holding an extra heap lock.
constexpr std::array<Site, 10U> kSites{{
    {0x009078FDU, 0x30U}, {0x0090795DU, 0x34U},
    {0x00907EB2U, 0x2FU}, {0x00907F32U, 0x42U},
    {0x00907FD2U, 0x33U}, {0x0090805AU, 0x46U},
    {0x0090810AU, 0x1FU}, {0x00908173U, 0x3CU},
    {0x00908219U, 0x71U}, {0x00908321U, 0x4DU}
}};
}

bool Install(const std::uintptr_t base, HookManager& hooks, std::string& error) {
    if (Installed()) return true;
    if (base == 0U || !hooks.MutationAllowed()) {
        error = "UI heap synchronization requires the certified executable";
        return false;
    }
    std::array<BytePatchRequest, kSites.size()> patches{};
    for (std::size_t i = 0U; i < kSites.size(); ++i) {
        const auto site = kSites[i];
        auto& patch = patches[i];
        patch.target = reinterpret_cast<void*>(base + site.rva - 4U);
        patch.expected = {std::byte{0x80}, std::byte{0x7E}, std::byte{0x64},
            std::byte{0x00}, std::byte{0x74}, static_cast<std::byte>(site.distance)};
        patch.replacement = patch.expected;
        patch.replacement[4U] = patch.replacement[5U] = std::byte{0x90};
    }
    if (!hooks.InstallPatchBatch("UIHeapGuard.NativeGFxLocks", patches, error)) return false;
    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }

namespace {
constexpr std::uintptr_t kGunLayerUpdateRva = 0x002742B0U;
constexpr std::uintptr_t kGunLayerCallRva = 0x00274A16U;
constexpr std::uintptr_t kGunLayerFaultRva = 0x00274921U; // mov ecx,[eax+50]
std::atomic<bool> gGunLayerInstalled{};
std::atomic<std::uintptr_t> gGunLayerBase{};
std::atomic<std::uint64_t> gGunLayerSkipped{};
int GunLayerFilter(const EXCEPTION_POINTERS* info) noexcept {
    const auto base = gGunLayerBase.load(std::memory_order_relaxed);
    const auto* record = info ? info->ExceptionRecord : nullptr;
    // Only the known null held-item read; any other fault stays a crash.
    return record && base && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        reinterpret_cast<std::uintptr_t>(record->ExceptionAddress) == base + kGunLayerFaultRva &&
        record->NumberParameters >= 2U && record->ExceptionInformation[0] == 0U &&
        record->ExceptionInformation[1] < 0x1000U ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
void __fastcall GunLayerUpdateGuarded(void* layer, void*) noexcept {
    const auto base = gGunLayerBase.load(std::memory_order_relaxed);
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + kGunLayerUpdateRva)(layer);
    } __except (GunLayerFilter(GetExceptionInformation())) {
        gGunLayerSkipped.fetch_add(1U, std::memory_order_relaxed);
    }
}
}

bool InstallGunLayerGuard(const std::uintptr_t base, HookManager& hooks, std::string& error) {
    if (GunLayerGuardInstalled()) return true;
    if (base == 0U || !hooks.MutationAllowed()) {
        error = "gun layer guard requires the certified executable";
        return false;
    }
    gGunLayerBase.store(base, std::memory_order_relaxed);
    CallRedirectRequest call;
    call.name = "GunLayerGuard.HeldItemNull";
    call.callSite = reinterpret_cast<void*>(base + kGunLayerCallRva);
    call.expectedTarget = reinterpret_cast<void*>(base + kGunLayerUpdateRva);
    call.detour = reinterpret_cast<void*>(&GunLayerUpdateGuarded);
    if (!hooks.InstallCallRedirect(call, error)) return false;
    gGunLayerInstalled.store(true, std::memory_order_release);
    return true;
}
bool GunLayerGuardInstalled() noexcept { return gGunLayerInstalled.load(std::memory_order_acquire); }
std::uint64_t GunLayerFaultsSkipped() noexcept { return gGunLayerSkipped.load(std::memory_order_relaxed); }
}
