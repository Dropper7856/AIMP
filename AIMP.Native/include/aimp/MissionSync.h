#pragma once

#include "aimp/WorldReplicationTypes.h"
#include <cstdint>
#include <filesystem>
#include <string>

namespace aimp {
class Diagnostics;
class WorldEntityRegistry;
namespace mission_sync {
struct Signal final {
    std::uint32_t generation{};
    std::uint64_t sequence{};
    WorldEntityId entity{};
    std::uint32_t port{};
    std::uint8_t selector{};
};
// Configuration and replay execute on the engine thread. The catalogue is
// derived from this map's authored mission graph, never from remote pointers.
bool Configure(WorldEntityRegistry& registry, const std::filesystem::path& path,
    const std::string& role, std::string& error);
bool Candidate(void* execution, std::uint32_t port) noexcept;
bool Capture(void* execution, std::uint32_t port, std::uint8_t selector) noexcept;
void Unsupported() noexcept;
// Shape of a captured frontier call that the commit replay normalizes away:
// the frontier's targets are control inputs only (compiler-proven), so its data
// reference, delay and tail are not part of the mission decision.
void NoteCapturedShape(bool payload, bool delayed, bool tail) noexcept;
bool Apply(const Signal& signal, std::string& status, std::string& error) noexcept;
bool TryPeek(Signal& signal) noexcept;
void Pop() noexcept;
void LogDiagnostics(Diagnostics& diagnostics);
}
}
