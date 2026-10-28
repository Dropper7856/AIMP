#pragma once
#include "aimp/AISyncRouteIdentity.h"
#include <memory>
#include <string>

namespace aimp { class HookManager; }
namespace aimp::ai_sync::mesh_storage {
// Loaded nav data is copied BEFORE initialization using the game's navigation
// allocator. The native W owns that copy (slot flag1), independently of the
// manager's reusable byte vector. A retained native W then pins its real blob.
struct Snapshot;
struct Lease {
    std::shared_ptr<const Snapshot> storage;
    route_identity::ResourceKey resource{};
    std::uintptr_t mesh{};
    std::uint64_t load{};
};
using EngineThread = bool(*)() noexcept;
[[nodiscard]] bool Install(std::uintptr_t imageBase, HookManager&, EngineThread, std::string& error);
// Engine thread only. The caller first proves the manager's live world/owner;
// this function then locks its retail CS before taking a real native W ref.
// Lease destruction also belongs to that qualified engine thread.
[[nodiscard]] bool Acquire(std::uintptr_t manager, std::uintptr_t mesh, Lease&) noexcept;
[[nodiscard]] bool Current(const Lease&) noexcept;
// Registers resource-qualified element ordinals, including special elements.
// Historical name retained for callers. This does NOT register a pair-key link
// map: NativeRoute resolves the exact special ordinal with004417C0 and verifies
// its borrowed record belongs to this owned blob before every publication.
[[nodiscard]] route_identity::Result BindOrdinary(route_identity::Registry&, std::uint64_t world,
    const Lease&, route_identity::BindingIncarnation&) noexcept;
// Thread-safe metadata retirement. External leases keep the owned blob
// alive and must be released on their qualified engine thread before unload.
[[nodiscard]] bool Retire() noexcept;
// Parsing is value-only; it never establishes a native storage lease.
[[nodiscard]] bool BlobLayout(std::span<const std::byte>, std::uint32_t& elements,
    std::array<std::uint32_t, 3>& header) noexcept;
struct Counters { std::uint64_t owned{}, rejected{}, capacity{}, allocationFailures{}; };
[[nodiscard]] Counters Diagnostics() noexcept;
}
