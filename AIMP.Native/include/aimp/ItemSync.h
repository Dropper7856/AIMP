#pragma once

#include "aimp/ItemSyncKey.h"
#include "aimp/WorldReplicationTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace aimp {
class Diagnostics;
class HookManager;
class LevelGenerationTracker;

namespace item_sync {

// ItemSync: world pickups collected by one participant disappear for everyone.
// Detection hooks the pickup manager's collection request (FUN_007A2B30) only
// when it comes from the "pickup_collected" output callback; removal on the
// other peers calls the manager's own removal (FUN_007A3200), which destroys
// the spawned pickup without giving the item.
struct PickupEvent final {
    std::uint64_t sequence{};
    WorldEntityId entity{};
};

// Spawn authority. Owner: a GCIP_WorldPickup spawn (random item, difficulty
// skip) proposed by the peer whose owner ran, decided once by the Host.
// Drop: a pickup the local player dropped, replicated with a synthetic owner.
enum class SpawnKind : std::uint8_t {
    Owner = 1,
    Drop = 2,
};

inline constexpr std::size_t kSpawnPathCapacity = 96U; // NUL included

struct SpawnData final {
    SpawnKind kind{SpawnKind::Owner};
    // Owner CATHODE runtime key packed as (ShortGuid << 32) | instance id.
    std::uint64_t ownerKey{};
    bool spawn{};
    char path[kSpawnPathCapacity]{}; // item definition composite path, "Pickups\\..."
    std::int32_t quantity{};
    float transform[12]{};           // 3x4 row-major, translation at [3], [7], [11]
};

struct SpawnEvent final {
    std::uint64_t sequence{};
    SpawnData data{};
};

// Synthetic drop owners never match a CATHODE entity: the pickup manager's
// owner messages to them are dropped by the retail resolver.
inline constexpr std::uint32_t kDropOwnerInstanceTag = 0xA1D00000U;
inline constexpr std::uint32_t kDropOwnerInstanceMask = 0xFFF00000U;

enum class ApplyStatus : std::uint8_t {
    Applied,
    AlreadyApplied,
    Deferred,
    Unavailable,
    Rejected,
    Fault,
};

bool Install(std::uintptr_t moduleBase, HookManager& hooks,
    LevelGenerationTracker& generations, std::string& error);
[[nodiscard]] bool Installed() noexcept;
// Logical shutdown: the detour forwards to retail and nothing is applied.
void Shutdown() noexcept;

// IPC worker: local collections, oldest first. Peek/Pop keeps an event at the
// head until the Launcher frame is queued.
[[nodiscard]] bool TryPeekObservedPickup(PickupEvent& event) noexcept;
void PopObservedPickup() noexcept;
// IPC worker: local spawn proposals and drops, same Peek/Pop contract.
[[nodiscard]] bool TryPeekObservedSpawn(SpawnEvent& event) noexcept;
void PopObservedSpawn() noexcept;

// Engine thread. Owner: stores the Host decision and spawns it once the local
// owner has run. Drop: spawns the remote player's dropped pickup now.
[[nodiscard]] ApplyStatus ApplySpawnDecision(const SpawnData& decision) noexcept;

// Engine thread only. Removes the pickup now, or keeps it pending until the
// pickup is spawned locally (streaming, late spawn). Idempotent per level.
[[nodiscard]] ApplyStatus ApplyRemoteCollected(const WorldEntityId& entity) noexcept;
// Engine thread: retries pending removals (self-throttled).
void PumpOnEngineThread() noexcept;

[[nodiscard]] const char* ApplyStatusCode(ApplyStatus status) noexcept;
[[nodiscard]] const char* ApplyStatusMessage(ApplyStatus status) noexcept;
[[nodiscard]] std::string EntityIdHex(const WorldEntityId& entity);
void LogDiagnostics(Diagnostics& diagnostics);

} // namespace item_sync
} // namespace aimp
