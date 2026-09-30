#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aimp {
class HookManager;
class LevelGenerationTracker;
class GameThreadDispatcher;
class Diagnostics;

namespace ai_sync {
// Host decisions are NOT WorldMutation/PlayerSync messages. Version 1 deliberately
// supports a certified common movement adapter; unsupported actions leave a passive proxy.
enum class Role : std::uint32_t { Retail, Host, Client, FrozenClient };
#pragma pack(push, 1)
struct State final {
    std::uint32_t version{2U}; // execution policy added; byte layout stays 76 bytes
    std::uint32_t nativeGeneration{}; // informational: generations differ per process
    std::uint64_t sessionCookie{};
    std::uint32_t worldGeneration{};
    std::uint32_t entityA{}, entityB{}, characterClass{}, birth{}, revision{};
    std::uint32_t kind{}; // 0 passive/unsupported, 1 resolved common movement
    std::uint32_t movementSpeedType{};
    float destination[3]{};
    std::uint32_t locomotionStyle{};
    float stoppingDistance{}, tolerance{};
    std::uint32_t flags{}; // bits 0/8 helper inputs; 16/17 request+B8 bits 3/4; 18 request+B9 bit 4
};
struct Configuration final {
    std::uint32_t version{1U};
    Role role{Role::Retail};
    std::uint32_t nativeGeneration{}, worldGeneration{};
    std::uint64_t sessionCookie{};
};
#pragma pack(pop)
static_assert(sizeof(State) == 76U);
static_assert(sizeof(Configuration) == 24U);

bool Install(std::uintptr_t moduleBase, HookManager& hooks,
    LevelGenerationTracker& generations, GameThreadDispatcher& dispatcher,
    std::string& error);
bool Configure(std::span<const std::byte> payload) noexcept;
bool QueueRemote(std::span<const std::byte> payload) noexcept;
bool TryPopObserved(State& state) noexcept;
void PumpOnEngineThread() noexcept;
void ObserveOwnedCharacter(std::uintptr_t character) noexcept;
void ForgetCharacter(std::uintptr_t character) noexcept;
void ForgetSpawnKey(std::uint32_t spawnKey) noexcept;
void TransportLost() noexcept;
void BeginLogicalShutdown() noexcept;
// Also pins the AI target-domain gate while the experimental adapter is active.
bool OwnsAI() noexcept;
bool Installed() noexcept;
struct Counters final {
    std::uint64_t observed{}, applied{}, dropped{}, refused{}, suspended{}, faults{};
    std::uint64_t capturedMovement{}, startedMovement{}, completedMovement{}, missingIdentity{}, orphanedStates{};
    std::uint64_t managerUpdates{}, admitted{}, identityRejected{}, treeUnavailable{}, ownerMismatch{};
    std::uint64_t movementCalls{}, unsupportedMovement{}, remoteQueued{}, outsideFrameMovement{};
    std::uint64_t movementPublished{}, inactiveCaptured{};
    std::uint64_t policyCaptured{}, policyApplied{}, policyRejected{};
    std::uint32_t role{}, nativeGeneration{}, worldGeneration{}, lastMovementDefinitionRva{};
    std::uint32_t lastEntityA{}, lastEntityB{}, lastCharacterClass{};
    bool stable{};
};
Counters DiagnosticsSnapshot() noexcept;
// Call only from IPC/bootstrap workers, never from an engine detour.
void LogDiagnostics(Diagnostics& diagnostics);
} // namespace ai_sync
} // namespace aimp
