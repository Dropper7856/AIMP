#pragma once

#include "aimp/ItemSyncKey.h"
#include "aimp/WorldReplicationTypes.h"

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
