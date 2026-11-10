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
    std::uint64_t occurrence{};
};
// Configuration and replay execute on the engine thread. The catalogue is
// derived from this map's authored mission graph, never from remote pointers.
bool Configure(WorldEntityRegistry& registry, const std::filesystem::path& path,
    const std::string& role, std::string& error);
// Cheap raw output-hook counter to distinguish missing engine output observations
// from unmapped runtime keys or an authored signal that never fires.
void ObserveOutputAttempt(bool localRoot, bool remoteRoot) noexcept;
void ProbeScopedOutput(void* execution, std::uint32_t port, bool localRoot) noexcept;
// True only while a validated MissionSync commit runs native CATHODE fan-out.
// WorldSync RemoteRoot must not suppress this authoritative mission replay.
bool ReplayActive() noexcept;
// payloadTolerant: the matched frontier is a Composite/Proxy interface pin whose
// targets are control inputs only; its runtime data reference is not replayed.
bool Candidate(void* execution, std::uint32_t port, bool& payloadTolerant, std::uint8_t inputKind = 0U) noexcept;
bool CampaignActive() noexcept;
bool HostRole() noexcept;
bool Capture(void* execution, std::uint32_t port, std::uint8_t selector, bool playerInitiated = false) noexcept;
void Unsupported() noexcept;
// Shape of a captured frontier call that the commit replay normalizes away:
// the frontier's targets are control inputs only (compiler-proven), so its data
// reference, delay and tail are not part of the mission decision.
void NoteCapturedShape(bool payload, bool delayed, bool tail) noexcept;
// A raw CATHODE data reference cannot be discarded and then declared applied.
// Record rejected shapes; leave the retail invocation untouched.
void NoteUnsafeShape(bool payload, bool delayed, bool tail) noexcept;
bool Apply(const Signal& signal, std::string& status, std::string& error) noexcept;
bool TryPeek(Signal& signal) noexcept;
void Pop() noexcept;
void LogDiagnostics(Diagnostics& diagnostics);
}
}
