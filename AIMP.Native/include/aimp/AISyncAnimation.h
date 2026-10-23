#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::ai_sync::animation {

// These callbacks also run on native animation workers. They must use the
// published lifetime bindings, not an Actor/mailbox or a blocking mutex.
using HumanPairClassifier = bool(*)(std::uintptr_t sourceCycle,
    std::uintptr_t victim) noexcept;
using ManagedSourcePredicate = bool(*)(std::uintptr_t source) noexcept;

inline constexpr std::uint32_t kScriptedValid = 1U;
// Known initializer flags without external spatial references or first-person
// camera requests. Unknown flag bits require their own execution contract.
inline constexpr std::uint32_t kScriptedNativeFlags = 0x0302BE7FU;

struct ScriptedState final {
    std::uint32_t flags{};
    std::uint32_t resourceToken{}, animationToken{};
    std::int32_t startFrame{}, endFrame{}, completionCount{};
    float playbackRate{}, rootBlendInSeconds{};
    std::uint32_t signalToken{};
    // +94 is forwarded to native graph-kind 0E +50 by 0068F740. Keep its
    // observed scalar meaning instead of assuming a blend-weight unit.
    float externalStopTimeSeconds{}, rootBlendOutSeconds{}, graphValue50{};
    std::uint32_t nativeFlags{};
};
static_assert(sizeof(ScriptedState) == 52U);

// Local inspection only. 009236A0 resolves these two-word CATHODE identities;
// no word is dereferenced as an address or copied into a portable State.
struct NativeReference final {
    std::uint32_t entity{}, resource{};
};
struct ScriptedContext final {
    std::array<NativeReference, 4U> references{};
    ScriptedState state{};
    float externalClockOrigin{};
    std::uint32_t reserved84{}, initialCountOffset{};
};

enum class Boundary : std::uint32_t {
    SourceUpdateEnter = 1U,
    SourceUpdateLeave = 2U,
    MarkerCallback = 3U,
    MarkerCallbackSkipped = 4U,
    MarkerDispatchEnter = 5U,
    MarkerDispatchLeave = 6U,
};

struct Observation final {
    Boundary boundary{};
    std::uintptr_t source{}, victim{}, cycle{}, graph{};
    std::uint32_t callerRva{}, marker{}, markerIndex{};
};
using Observer = void(*)(const Observation&) noexcept;

struct Callbacks final {
    HumanPairClassifier humanPair{};
    ManagedSourcePredicate managedSource{};
    Observer observer{};
    // Must require Client, stable generation, exact admitted source lifetime,
    // and engine thread. ApplyScripted never runs a decision producer.
    ManagedSourcePredicate canReplayScripted{};
};

struct Counters final {
    std::uint64_t sourceUpdates{}, coupledPlayerUpdates{}, humanAlignmentUpdates{};
    std::uint64_t markerCallbacks{}, markerCallbacksSkipped{}, markerDispatches{};
    std::uint64_t boundedListenerScans{};
    std::uint64_t scriptedCaptured{}, scriptedUnsupported{}, scriptedApplied{}, scriptedFailed{};
    std::uint32_t lastSourceUpdateCallerRva{}, lastMarker{}, lastMarkerCallerRva{};
};

// Correct only the certified 006556D0 participant classification. The native
// function performs all root motion/collision work. The other hooks observe
// existing updates and marker delivery; they never schedule a cycle or marker.
bool Install(std::uintptr_t base, HookManager& hooks,
    const Callbacks& callbacks, std::string& error);
bool ValidScripted(const ScriptedState& state) noexcept;
bool InspectScripted(std::uintptr_t character, ScriptedContext& context) noexcept;
// The supported subset has no spatial/timeline references and no consumed
// completion-count offset. Resource/animation tokens are resolved by the native
// initializer on the receiving machine, with local graphs/listeners.
bool CaptureScripted(std::uintptr_t character, ScriptedState& state) noexcept;
// Call once for a new authoritative execution revision. The native transition
// constructs and initializes synchronously; this function does not tick it.
bool ApplyScripted(std::uintptr_t character, const ScriptedState& state,
    std::string& error) noexcept;
Counters Snapshot() noexcept;

} // namespace aimp::ai_sync::animation
