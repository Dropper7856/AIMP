#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aimp {
class HookManager;
class LevelGenerationTracker;
class Diagnostics;
class GameThreadDispatcher;
namespace throwable_sync {
// Semantic release carrier, not a pickup/drop or an equipment-slot action.
// All pointers/slot references stay process-local. The Host owns lifetime.
#pragma pack(push, 1)
struct Configuration final {
    std::uint32_t version{1U}, enabled{}, nativeGeneration{}, worldGeneration{};
    std::uint64_t sessionCookie{}, localPeer{};
};
struct Event final {
    std::uint32_t version{1U}, nativeGeneration{}, worldGeneration{}, operation{1U};
    std::uint64_t sessionCookie{}, peer{}, sequence{};
    std::uint32_t placement{}, reserved{};
    float transform[12]{}, velocity[3]{}, timeLeft{};
    char path[160]{};
};
#pragma pack(pop)
static_assert(sizeof(Configuration) == 32U);
static_assert(sizeof(Event) == 272U);
bool Install(std::uintptr_t base, HookManager&, LevelGenerationTracker&, GameThreadDispatcher&, std::string& error);
bool Configure(std::span<const std::byte>) noexcept;
bool QueueRemote(std::span<const std::byte>) noexcept;
bool TryPeekObserved(Event&, std::uint64_t& ticket) noexcept;
void PopObserved(std::uint64_t ticket) noexcept;
void PumpOnEngineThread() noexcept;
void TransportLost() noexcept;
void Shutdown() noexcept;
bool Installed() noexcept;
void LogDiagnostics(Diagnostics&);
}
}
