#include "aimp/TeardownReferenceGuard.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include <Windows.h>
#include <atomic>
#include <cstddef>

namespace aimp::teardown_reference_guard {
namespace {
// 0063EEB0 (thiscall, two stack arguments, RET 8) resolves the entity held by
// the reference at +0x2C through its object (+0x0C) and vtable[0x8C], then
// copies that entity's transform. It never validates the reference. A Client
// crashed there during RETURN_TO_FRONTEND (EDX=0, read 0x8C): the reference
// still pointed at an entity freed by the level unload. When the referenced
// entity no longer has a live retail vtable, take the existing retail
// "no entity" exit (FUN_00A17990 == 0) instead of faulting; live references
// run retail unchanged.
using Update = void(__thiscall*)(void* self, void* first, void* second);

constexpr std::uintptr_t kUpdateRva = 0x0023EEB0U;
constexpr std::uintptr_t kReferenceOffset = 0x2CU;
constexpr std::uintptr_t kObjectOffset = 0x0CU;
constexpr std::uintptr_t kResolveSlot = 0x8CU;

std::uintptr_t gBase{}, gImageEnd{};
void* gTrampoline{};
std::atomic<void*> gOriginal{};
std::atomic<bool> gInstalled{};
std::atomic<std::uint64_t> gStaleReferences{}, gReportedStaleReferences{};

bool InImage(const std::uintptr_t value) noexcept { return value >= gBase && value < gImageEnd; }

bool ReferenceLive(void* self) noexcept {
    using crash_guard::SafeRead;
    const auto reference = SafeRead<std::uintptr_t>(static_cast<std::byte*>(self) + kReferenceOffset);
    if (!reference || *reference == 0U) return false;
    const auto object = SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(*reference + kObjectOffset));
    if (!object || *object < 0x10000U || (*object & 3U) != 0U) return false;
    const auto table = SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(*object));
    if (!table || !InImage(*table)) return false;
    const auto resolve = SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(*table + kResolveSlot));
    return resolve && InImage(*resolve);
}

void __fastcall UpdateDetour(void* self, void*, void* first, void* second) noexcept {
    const auto original = reinterpret_cast<Update>(gOriginal.load(std::memory_order_acquire));
    if (self != nullptr && ReferenceLive(self)) {
        original(self, first, second);
        return;
    }
    gStaleReferences.fetch_add(1U, std::memory_order_relaxed);
}
} // namespace

bool Install(const std::uintptr_t base, HookManager& hooks, std::string& error) {
    if (Installed()) return true;
    if (base == 0U || !hooks.MutationAllowed()) {
        error = "Teardown reference guard requires the certified executable";
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
    gBase = base;
    gImageEnd = base + nt->OptionalHeader.SizeOfImage;
    HookRequest request;
    request.name = "TeardownReferenceGuard.EntityTransform";
    request.target = reinterpret_cast<void*>(base + kUpdateRva);
    request.detour = reinterpret_cast<void*>(&UpdateDetour);
    // PUSH EBP; MOV EBP,ESP; AND ESP,-16: position-independent.
    request.expected.bytes = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    request.expected.mask = "xxxxxx";
    request.patchLength = request.expected.bytes.size();
    request.relocationSafePrefix = true;
    request.trampolineOut = &gTrampoline;
    request.publishedTrampolineOut = &gOriginal;
    if (!hooks.Install(request, error)) return false;
    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }

void LogDiagnostics(Diagnostics& diagnostics) {
    const auto stale = gStaleReferences.load(std::memory_order_relaxed);
    if (stale == gReportedStaleReferences.exchange(stale, std::memory_order_relaxed)) return;
    diagnostics.Log("warning", "teardown_reference_guard",
        "An entity transform update referenced an entity that no longer exists; the update was skipped.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"staleReferences", Diagnostics::Unsigned(stale), true}});
}
} // namespace aimp::teardown_reference_guard
