#include "aimp/MissionSync.h"
#include "aimp/Diagnostics.h"
#include "aimp/WorldEntityRegistry.h"
#include "aimp/WorldPublicActions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace aimp::mission_sync {
namespace {
struct Frontier final { WorldEntityId entity{}; std::uint32_t port{}; };
std::mutex gMutex;
std::unordered_map<std::uint64_t, std::vector<Frontier>> gFrontiers;
std::deque<Signal> gObserved;
std::atomic<WorldEntityRegistry*> gRegistry{};
WorldEntityId gMap{};
std::uint32_t gGeneration{};
std::uint64_t gSequence{}, gCommitted{}, gCaptured{}, gApplied{}, gUnsupported{}, gFailures{};
std::uint64_t gWithPayload{}, gWithDelay{}, gWithTail{};
// Engine-side observation coverage: distinguish authored-port mismatch from
// runtime identity mismatch and a rejected local OutputDefinition selector.
std::atomic<std::uint64_t> gRawOutputAttempts{}, gRawOutputLocalRoot{}, gRawOutputRemoteRoot{};
std::uint64_t gOutputCandidates{}, gOutputIdentityResolved{}, gOutputIdentityRejected{},
    gOutputKnownSource{}, gOutputKnownPort{}, gOutputPortOnOtherSource{},
    gOutputMatched{};
std::uint64_t gScopedLocalExact{}, gScopedRemoteExact{},
    gScopedLocalForeign{}, gScopedRemoteForeign{};
std::uint32_t gLastKnownSourcePort{}, gLastUnmatchedPort{};
std::uint64_t gLastUnmatchedRuntimeKey{};
struct ForeignPortEvidence final {
    std::uint32_t port{};
    std::uint64_t firstKey{}, lastKey{}, occurrences{}, observedKeys{};
    WorldEntityId lastMapped{};
};
std::array<ForeignPortEvidence, 16U> gForeignPorts{};
bool gEnabled{}, gFault{};

template<typename T> bool Read(std::ifstream& stream, T& value) {
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(value)));
}
std::uint64_t Key(const CathodeRuntimeKey& key) noexcept {
    return (static_cast<std::uint64_t>(key.entityA) << 32U) | key.entityB;
}
bool Current(WorldEntityRegistry& registry) noexcept {
    const auto context = registry.ReplicationContext();
    return context.manifestLoaded && context.levelGeneration == gGeneration && context.mapContentId == gMap;
}
const Frontier* Find(void* execution, const std::uint32_t port, WorldEntityRegistry& registry) noexcept {
    CathodeRuntimeKey key{};
    if (!registry.TryExtractRuntimeKey(execution, key)) return nullptr;
    const auto found = gFrontiers.find(Key(key));
    if (found == gFrontiers.end()) return nullptr;
    for (const auto& entry : found->second) if (entry.port == port) return &entry;
    return nullptr;
}
}

bool Configure(WorldEntityRegistry& registry, const std::filesystem::path& path,
    const std::string& role, std::string& error) {
    if (role == "off") {
        std::scoped_lock lock(gMutex);
        gEnabled = false;
        gFrontiers.clear(); gObserved.clear();
        gRegistry.store(nullptr, std::memory_order_release);
        return true;
    }
    if (role != "host" && role != "client") { error = "Invalid MissionSync role."; return false; }
    if (!world_actions::MissionOutputReady()) { error = "Native CATHODE output hook is not ready."; return false; }
    std::ifstream file(path, std::ios::binary);
    std::uint32_t magic{}, count{};
    WorldEntityId map{};
    if (!Read(file, magic) || magic != 0x31534D41U || !Read(file, map.high) ||
        !Read(file, map.low) || !Read(file, count) || count > 4096U) {
        error = "Invalid MissionSync frontier catalogue."; return false;
    }
    const auto context = registry.ReplicationContext();
    if (!context.manifestLoaded || !(context.mapContentId == map)) {
        error = "MissionSync catalogue does not match the loaded map."; return false;
    }
    std::unordered_map<std::uint64_t, std::vector<Frontier>> entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        Frontier entry{};
        CathodeRuntimeKey key{};
        if (!Read(file, entry.entity.high) || !Read(file, entry.entity.low) ||
            !Read(file, key.entityA) || !Read(file, key.entityB) || !Read(file, entry.port) ||
            !entry.entity.Valid() || key.entityA == 0U || entry.port == 0U) {
            error = "Truncated or invalid mission frontier."; return false;
        }
        const auto known = registry.FindWorldEntityId(key);
        if (!known || !(*known == entry.entity)) { error = "Mission frontier identity is absent from the manifest."; return false; }
        auto& ports = entries[Key(key)];
        if (std::any_of(ports.begin(), ports.end(), [&](const Frontier& other) { return other.port == entry.port; })) {
            error = "Duplicate mission frontier."; return false;
        }
        ports.push_back(entry);
    }
    if (file.peek() != std::ifstream::traits_type::eof()) { error = "Unexpected mission catalogue tail."; return false; }
    std::scoped_lock lock(gMutex);
    // Preparation may revisit the same level. It must not rewind the ordered
    // journal or discard a captured event while peers are still loading.
    if (gEnabled && gGeneration == context.levelGeneration && gMap == map) return true;
    gFrontiers = std::move(entries); gObserved.clear();
    gMap = map; gGeneration = context.levelGeneration;
    gSequence = 0U; gCommitted = 0U;
    gCaptured = 0U; gApplied = 0U; gUnsupported = 0U; gFailures = 0U;
    gRawOutputAttempts.store(0U, std::memory_order_relaxed);
    gRawOutputLocalRoot.store(0U, std::memory_order_relaxed);
    gRawOutputRemoteRoot.store(0U, std::memory_order_relaxed);
    gOutputCandidates = gOutputIdentityResolved = gOutputIdentityRejected = 0U;
    gOutputKnownSource = gOutputKnownPort = gOutputPortOnOtherSource = gOutputMatched = 0U;
    gLastKnownSourcePort = gLastUnmatchedPort = 0U;
    gLastUnmatchedRuntimeKey = 0U;
    gForeignPorts = {};
    gScopedLocalExact = gScopedRemoteExact = gScopedLocalForeign = gScopedRemoteForeign = 0U;
    gFault = false; gEnabled = true;
    gRegistry.store(&registry, std::memory_order_release);
    return true;
}

void ObserveOutputAttempt(bool localRoot, bool remoteRoot) noexcept {
    if (gRegistry.load(std::memory_order_relaxed) == nullptr) return;
    gRawOutputAttempts.fetch_add(1U, std::memory_order_relaxed);
    if (localRoot) gRawOutputLocalRoot.fetch_add(1U, std::memory_order_relaxed);
    if (remoteRoot) gRawOutputRemoteRoot.fetch_add(1U, std::memory_order_relaxed);
}
void ProbeScopedOutput(void* execution, std::uint32_t port, bool localRoot) noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr) return;
    std::scoped_lock lock(gMutex);
    if (!gEnabled || !Current(*registry)) return;
    CathodeRuntimeKey key{};
    if (!registry->TryExtractRuntimeKey(execution, key)) return;
    bool exact = false, foreign = false;
    for (const auto& [source, entries] : gFrontiers) {
        for (const auto& entry : entries) {
            if (entry.port != port) continue;
            if (source == Key(key)) exact = true;
            else foreign = true;
        }
    }
    if (localRoot) {
        if (exact) ++gScopedLocalExact;
        else if (foreign) ++gScopedLocalForeign;
    } else {
        if (exact) ++gScopedRemoteExact;
        else if (foreign) ++gScopedRemoteForeign;
    }
}
bool Candidate(void* execution, const std::uint32_t port) noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr) return false;
    std::scoped_lock lock(gMutex);
    if (!gEnabled || !Current(*registry)) return false;
    ++gOutputCandidates;
    CathodeRuntimeKey key{};
    if (!registry->TryExtractRuntimeKey(execution, key)) {
        ++gOutputIdentityRejected;
        return false;
    }
    ++gOutputIdentityResolved;
    const auto found = gFrontiers.find(Key(key));
    if (found != gFrontiers.end()) {
        ++gOutputKnownSource;
        gLastKnownSourcePort = port;
        for (const auto& entry : found->second) {
            if (entry.port == port) {
                ++gOutputMatched;
                return true;
            }
        }
    }
    bool portInCatalog = false;
    for (const auto& [source, entries] : gFrontiers) {
        static_cast<void>(source);
        for (const auto& entry : entries)
            if (entry.port == port) { portInCatalog = true; break; }
        if (portInCatalog) break;
    }
    if (portInCatalog) {
        ++gOutputPortOnOtherSource;
        // Store bounded *observed* source identities for each compiled port.
        // These are diagnostics only: matching a port alone is NOT sufficient
        // proof to replay a CATHODE mission output on a different Character.
        auto* sample = static_cast<ForeignPortEvidence*>(nullptr);
        for (auto& slot : gForeignPorts) {
            if (slot.port == port) { sample = &slot; break; }
            if (slot.port == 0U && sample == nullptr) sample = &slot;
        }
        if (sample != nullptr) {
            const auto observedKey = Key(key);
            if (sample->port == 0U) {
                sample->port = port;
                sample->firstKey = observedKey;
            }
            ++sample->occurrences;
            if (sample->lastKey != observedKey) {
                ++sample->observedKeys;
                sample->lastKey = observedKey;
                sample->lastMapped = registry->FindWorldEntityId(key).value_or(WorldEntityId{});
            }
        }
        ++gOutputKnownPort;
    } else {
        gLastUnmatchedPort = port;
        gLastUnmatchedRuntimeKey = Key(key);
    }
    return false;
}
bool Capture(void* execution, const std::uint32_t port, const std::uint8_t selector) noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr) return false;
    std::scoped_lock lock(gMutex);
    if (!gEnabled || !Current(*registry) || (selector != 1U && selector != 2U)) return false;
    const auto* entry = Find(execution, port, *registry);
    if (entry == nullptr) return false;
    // Host and Client both wait for the same commit stream. Never let a full
    // observation queue silently advance only one participant's mission.
    if (gObserved.size() >= 256U) { gFault = true; ++gFailures; return true; }
    gObserved.push_back(Signal{gGeneration, ++gSequence, entry->entity, port, selector});
    ++gCaptured;
    return true;
}
void Unsupported() noexcept { std::scoped_lock lock(gMutex); ++gUnsupported; }
void NoteCapturedShape(const bool payload, const bool delayed, const bool tail) noexcept {
    std::scoped_lock lock(gMutex);
    gWithPayload += payload ? 1U : 0U; gWithDelay += delayed ? 1U : 0U; gWithTail += tail ? 1U : 0U;
}

bool Apply(const Signal& signal, std::string& status, std::string& error) noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr) { status = "MISSION_SYNC_NOT_READY"; return false; }
    {
        std::scoped_lock lock(gMutex);
        if (!gEnabled || !Current(*registry) || signal.generation != gGeneration) {
            status = "MISSION_SYNC_NOT_READY"; return false;
        }
        if (gFault) { status = "MISSION_SYNC_FAULT"; error = "Mission signal queue overflowed."; return false; }
        if (signal.sequence <= gCommitted) { status = "MISSION_SYNC_ALREADY_APPLIED"; return true; }
        bool allowed = false;
        for (const auto& [key, ports] : gFrontiers) {
            static_cast<void>(key);
            for (const auto& entry : ports) if (entry.entity == signal.entity && entry.port == signal.port) allowed = true;
        }
        if (!allowed || signal.sequence != gCommitted + 1U ||
            (signal.selector != 1U && signal.selector != 2U)) {
            status = "MISSION_SYNC_REJECTED"; error = "Unknown mission frontier or out-of-order commit."; return false;
        }
    }
    auto resolved = registry->Resolve(signal.entity);
    if (!resolved.lease) { status = "MISSION_SYNC_NOT_READY"; return false; }
    const auto live = registry->IdentifyRuntimeEntity(resolved.lease.Get());
    if (!live || !(*live == signal.entity)) { status = "MISSION_SYNC_REJECTED"; return false; }
    // The registry's lease pins the original native entity through all fan-out.
    const auto result = world_actions::InvokeMissionOutputNoCapture(resolved.lease, signal.port, signal.selector);
    std::scoped_lock lock(gMutex);
    if (result != world_actions::LocalCompositeInputInvokeStatus::Invoked) {
        ++gFailures;
        status = result == world_actions::LocalCompositeInputInvokeStatus::NativeFault
            ? "MISSION_SYNC_FAULT" : "MISSION_SYNC_NOT_READY";
        if (result == world_actions::LocalCompositeInputInvokeStatus::NativeFault) gFault = true;
        error = "The native mission fan-out could not execute.";
        return false;
    }
    gCommitted = signal.sequence; ++gApplied;
    status = "MISSION_SYNC_APPLIED";
    return true;
}

bool TryPeek(Signal& signal) noexcept {
    std::scoped_lock lock(gMutex);
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr || !Current(*registry)) { gObserved.clear(); return false; }
    if (gObserved.empty()) return false;
    signal = gObserved.front(); return true;
}
void Pop() noexcept { std::scoped_lock lock(gMutex); if (!gObserved.empty()) gObserved.pop_front(); }
void LogDiagnostics(Diagnostics& diagnostics) {
    std::scoped_lock lock(gMutex);
    diagnostics.Log("info", "native_missionsync_snapshot", "MissionSync native graph frontier status.",
        {{"generation", Diagnostics::Unsigned(gGeneration), true},
         {"sources", Diagnostics::Unsigned(gFrontiers.size()), true},
         {"captured", Diagnostics::Unsigned(gCaptured), true},
         {"rawOutputHookCalls", Diagnostics::Unsigned(gRawOutputAttempts.load(std::memory_order_relaxed)), true},
         {"rawOutputLocalRoot", Diagnostics::Unsigned(gRawOutputLocalRoot.load(std::memory_order_relaxed)), true},
         {"rawOutputRemoteRoot", Diagnostics::Unsigned(gRawOutputRemoteRoot.load(std::memory_order_relaxed)), true},
         {"observedOutputs", Diagnostics::Unsigned(gOutputCandidates), true},
         {"outputRuntimeIdentities", Diagnostics::Unsigned(gOutputIdentityResolved), true},
         {"outputIdentityRejected", Diagnostics::Unsigned(gOutputIdentityRejected), true},
         {"outputOnCatalogSources", Diagnostics::Unsigned(gOutputKnownSource), true},
         {"outputCatalogPortOnOtherSource", Diagnostics::Unsigned(gOutputPortOnOtherSource), true},
         {"outputMatched", Diagnostics::Unsigned(gOutputMatched), true},
         {"scopedLocalExact", Diagnostics::Unsigned(gScopedLocalExact), true},
         {"scopedRemoteExact", Diagnostics::Unsigned(gScopedRemoteExact), true},
         {"scopedLocalForeign", Diagnostics::Unsigned(gScopedLocalForeign), true},
         {"scopedRemoteForeign", Diagnostics::Unsigned(gScopedRemoteForeign), true},
         {"lastCatalogSourcePort", Diagnostics::Unsigned(gLastKnownSourcePort), true},
         {"lastUnmatchedPort", Diagnostics::Unsigned(gLastUnmatchedPort), true},
         {"lastUnmatchedRuntimeKey", Diagnostics::Unsigned(gLastUnmatchedRuntimeKey), true},
         {"applied", Diagnostics::Unsigned(gApplied), true},
         {"unsupportedContext", Diagnostics::Unsigned(gUnsupported), true},
         {"capturedWithPayload", Diagnostics::Unsigned(gWithPayload), true},
         {"capturedWithDelay", Diagnostics::Unsigned(gWithDelay), true},
         {"capturedWithTail", Diagnostics::Unsigned(gWithTail), true},
         {"failures", Diagnostics::Unsigned(gFailures), true},
         {"fault", gFault ? "true" : "false", true}});
    for (const auto& sample : gForeignPorts) {
        if (sample.port == 0U) continue;
        diagnostics.Log("info", "native_missionsync_foreign_output",
            "Observed an authored mission-output port on a different native source. "
            "Port equality alone is never replay authority.",
            {{"port", Diagnostics::Unsigned(sample.port), true},
             {"firstRuntimeKey", Diagnostics::Unsigned(sample.firstKey), true},
             {"lastRuntimeKey", Diagnostics::Unsigned(sample.lastKey), true},
             {"lastMappedEntityHigh", Diagnostics::Unsigned(sample.lastMapped.high), true},
             {"lastMappedEntityLow", Diagnostics::Unsigned(sample.lastMapped.low), true},
             {"occurrences", Diagnostics::Unsigned(sample.occurrences), true},
             {"runtimeKeyChanges", Diagnostics::Unsigned(sample.observedKeys), true}});
    }
}
}
