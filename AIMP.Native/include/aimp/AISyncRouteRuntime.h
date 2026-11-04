#pragma once
#include "aimp/AISyncRouteCodec.h"
#include <string>

namespace aimp { class HookManager; }
namespace aimp::ai_sync::route_runtime {
using EngineThread = bool(*)() noexcept;
using Qualify = bool(*)(const transactions::Scope&) noexcept;
struct Packet {
    std::array<std::uint8_t,route::kMaxBytes> bytes{};
    std::size_t size{};
    std::uint64_t token{};
};
// Host native corridor capture, bounded wire queue and native Client execution.
[[nodiscard]] bool Install(std::uintptr_t,HookManager&,EngineThread,Qualify host,Qualify client,std::string&);
[[nodiscard]] bool Available() noexcept;
void Disable() noexcept; // Stops path capture and value admission.
[[nodiscard]] bool QueueClient(std::span<const std::uint8_t>) noexcept;
void Pump() noexcept; // Engine thread only, no IPC locks held.
// Producer's immediate callback after complete Submit/Update finalization.
// Caller retains and revalidates the exact request/M owner for this N/B.
// Consumes only metadata observed at the native post-install capture seam.
void ObserveHostNavigation(std::uintptr_t navigation,std::uintptr_t block) noexcept;
// Copied diagnostic, any thread. Command fence covers both parts and Updates;
// no native ownership or completion is inferred from this value.
[[nodiscard]] bool CommandBlocked(const route::Key&,route::Failure&) noexcept;
// Commands the Host published without a capturable path (command-local refusal).
[[nodiscard]] std::uint64_t HostRoutesUnavailable() noexcept;
[[nodiscard]] std::uint64_t LocalPlanningFallbacks() noexcept;
// Host-route requests planned natively because the Client was off the route start / replanned it.
[[nodiscard]] std::uint64_t LocalStartPlans() noexcept;
[[nodiscard]] std::uint64_t LocalReplans() noexcept;
// Includes native M/R/mesh pins held by the route executor.
[[nodiscard]] bool RetirementComplete(const transactions::Scope&) noexcept;
// True while an owned native result waits for its authoritative graph.
[[nodiscard]] bool AwaitingHostRoute(std::uintptr_t navigation) noexcept;
struct Counters {
    std::uint64_t captured{},imported{},blocked{},pending{},retained{};
    std::uint64_t invalidations{},replanSuppressed{},goalDeferred{},awaitingHost{};
    std::uint64_t replacements{},replacementAdmissions{},superseded{};
    std::uint64_t commandFaults{},discarded{},globalFailures{};
    std::uint32_t hostFailureSite{},hostFailureResult{},hostNavigationNodes{},hostResultNodes{};
    std::uint32_t hostBlockState{},hostBlockMode{},hostStopBits{},hostNodeType{},hostMetadataBytes{};
    std::uint64_t queued{},queueRejected{},engineQueries{},workerQueries{},strictQueries{},unownedQueries{},requeues{};
    std::uint32_t queuedPackets{},lastQueueRejection{},lastQueryOwnership{},lastQueryPolicy{},lastQueryKind{};
    std::uint64_t hostCaptureDeferred{};
    std::uint32_t hostCaptureCheck{};
    std::uint32_t hostCaptureNode{UINT32_MAX},hostMarkerIndex{UINT32_MAX},hostMarkerKind{UINT32_MAX},hostMarkerTypeRva{UINT32_MAX};
    std::uint64_t hostEndpointEditsSkipped{};
};
[[nodiscard]] Counters Diagnostics() noexcept;
// Bounded value-only failure records, copied on the engine thread and drained
// by diagnostics. No native addresses or reads on the diagnostics thread.
struct ClientFailure {
    std::uint64_t tick{},command{},receivedCommand{};
    std::uint32_t site{},result{},currentCheck{},ownership{},revision{},routeRevision{},part{};
    std::uint32_t currentRevision{},receivedRevision{},receivedRoute{},receivedPart{};
    std::uint32_t worldToken{},nativeToken{},resourceDifference{},readMask{};
    std::uint32_t importStage{},importNode{UINT32_MAX},bindingDifference{},contextCheck{};
    // N route/history/active/retired counts, previous-B presence, current-B/R
    // match, B state/mode/stop, R ready/status/node count.
    std::array<std::uint32_t,13> native{};
};
[[nodiscard]] bool PopClientFailure(ClientFailure&) noexcept;
[[nodiscard]] std::uint64_t ClientFailuresDropped() noexcept;
}
