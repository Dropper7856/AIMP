#pragma once

#include "aimp/AISyncRouteCodec.h"
#include <memory>

namespace aimp::ai_sync::route {
// 004541D0's default -1 and an explicit zero both disable the positive
// stop-distance trim in 004659D0. ReplacementNoWorker imports already trimmed
// installed Host graphs and requires the executor to skip the second trim.
[[nodiscard]] constexpr bool StopTrimDisabled(std::uint32_t bits) noexcept {
    return bits==0U||bits==0xBF800000U;
}
struct LocalContext {
    std::uintptr_t navigation{}, block{}, result{}, resultControl{};
    bool operator==(const LocalContext&) const noexcept = default;
};
enum class GuardPhase : std::uint8_t { Read, Create, Publish, Retire, Preflight, Rebind };
enum class ImportPoint : std::uint8_t { QueryDispatch, DeferredNoWorker, ReplacementNoWorker };
// 0044D200 resets every non-retired B, not just the triggering request.
// Read-only proof required before using it for one part's replacement.
[[nodiscard]] bool ReplacementAffectsOnlyBlock(std::uintptr_t navigation,std::uintptr_t block) noexcept;
// Footprint only: caller must also certify every supplied block's Host graph.
[[nodiscard]] bool ReplacementAffectsOnlyBlocks(std::uintptr_t navigation,
    std::span<const std::uintptr_t> explicitlyOwnedBlocks) noexcept;
enum class AdmissionMode : std::uint8_t { Replace, AppendEmpty, Append };
// Engine-thread local proof; pointers never leave the process. Populate
// independently repeats this proof at every allocation/publication boundary.
struct AdmissionPlan {
    AdmissionMode mode{AdmissionMode::Replace};
    std::uint32_t existingNodeCount{},prefixCount{},incomingOffset{},incomingCount{},cursorIndex{UINT32_MAX};
    std::uint32_t tailMarkers{},tailMarkerDelta{};
    std::uintptr_t sentinel{},first{},tail{},cursor{},previousBlock{};
    bool operator==(const AdmissionPlan&) const noexcept = default;
};
using Guard = bool(*)(void*, const Key&, const LocalContext&,
    std::span<const route_identity::ElementMapping>, GuardPhase) noexcept;

// LOCAL ONLY. storage must OWN protection of the exact backing blob, not a
// cached readiness bool. For retail flags0 W, its strong Ref does not pin the
// manager+20 vector. AISyncMeshStorage can instead supply an owned flags1 clone
// plus native W ref; its Current check is mandatory at every guard boundary.
// Revoke mappings before reload and quiesce consumers before dropping a pin.
// Guard additionally proves qualified owner thread, full key/N/B/R binding,
// new owner with no prior worker, exact directed adjacencies and compatible
// start; local invalidity must report ExecutionBlocked to Host, never solve.
struct Protection {
    std::shared_ptr<const void> storage;
    std::uintptr_t mesh{};
    std::uint32_t localWorldRevision{};
    Guard guard{};
    void* context{};
};

// Capture reads a restricted worker result. Production CaptureInstalled reads
// the unconsumed portion of the exact B range AFTER004659D0, including its Host-authored prefix
// and ordered parameterless tags2/3/4/7/8/9/11. It never drops N to transmit R alone.
// Populate replaces ONLY CALL004607E3->004570D0 for a qualified strict owner;
// the enclosing00460760 still allocates/releases its own R Ref normally.
// Four postprocess CALLs00465AAD/ABB/AC9/AD7 must be bypassed for that exact
// imported R/N/B only; ordinary004659D0 then retains its full prefix/suffix,
// validation, distances, callbacks, collision and locomotion flow. Imported
// nodes already own their exact tags; the two 9/11 attachment calls are skipped
// only for vectors proven to belong to the independently pinned imported R.
// These primitives neither install those hooks nor claim B was admitted.
class NativeRouteResult final {
public:
    NativeRouteResult() noexcept = default;
    // As with native request leases, the caller must keep this object alive
    // until Retire succeeds or external world teardown proves all native
    // consumers destroyed. Destruction is not cancellation/quiescence.
    ~NativeRouteResult() = default;
    NativeRouteResult(const NativeRouteResult&) = delete;
    NativeRouteResult& operator=(const NativeRouteResult&) = delete;
    [[nodiscard]] bool Initialize(std::uintptr_t imageBase, bool exactBuildValidated,
        route_identity::Registry&) noexcept;
    [[nodiscard]] Result Capture(const Key&, const LocalContext&, const Protection&,
        std::span<const route_identity::BindingIncarnation>, Graph&) noexcept;
    // Remaining Host B range from max(N.cursor,B.begin), with all admitted tags.
    // requireComplete refuses a first export after any of B's prefix was consumed.
    [[nodiscard]] Result CaptureInstalled(const Key&,const LocalContext&,const Protection&,
        const route_identity::BindingIncarnation&,const std::array<float,6>& previousAuxiliary,Graph&,
        bool requireComplete = false) noexcept;
    // One native entry per instance, including faults. An uncertain partial
    // append/release is fenced, never retried. Protection is retained after
    // entry until Retire's external native-quiescence guard succeeds.
    [[nodiscard]] Result Populate(const Graph&, const LocalContext&, const Protection&,
        ImportPoint = ImportPoint::QueryDispatch) noexcept;
    // Read-only remapping/adjacency validation before retiring an active route.
    [[nodiscard]] Result Preflight(const Graph&, const LocalContext&, const Protection&) noexcept;
    [[nodiscard]] Result CertifyAdmission(const Graph&,const LocalContext&,const Protection&,AdmissionPlan&,
        ImportPoint = ImportPoint::ReplacementNoWorker) noexcept;
    // Continue the SAME protected N/B/mesh lifetime using a fresh worker-free R.
    // Old native nodes may remain in N.history: storage protection is kept,
    // never declared quiescent. Caller proves old R detached via Rebind guard.
    [[nodiscard]] Result Rearm(const Key& previous,const Graph&,const LocalContext&,const Protection&) noexcept;
    // Before compound invalidation: retain an exact remaining native suffix
    // of the SAME Host graph. Auxiliary/wire identity are never rewritten.
    [[nodiscard]] Result RetainContinuation(const Graph& authoritative,const LocalContext& installed,
        const Protection&,Graph& remaining) noexcept;
    [[nodiscard]] Result RearmContinuation(const Graph& remaining,const LocalContext& fresh,
        const Protection&) noexcept;
    [[nodiscard]] Result Retire(const Key&) noexcept;
    // Read-only admission check: the imported node must still resolve to the
    // exact local element/link, never a pair-map or nearest-position match.
    [[nodiscard]] bool MatchesNode(const Protection&, const Node&,
        const route_identity::ElementMapping&, std::uintptr_t borrowedLink) noexcept;
    [[nodiscard]] bool ReadMarkers(std::uintptr_t node,std::uint32_t&) noexcept;
    [[nodiscard]] bool Entered() const noexcept { return entered_; }
    [[nodiscard]] bool Published() const noexcept { return published_; }
    [[nodiscard]] std::uint32_t ImportStage() const noexcept { return importStage_; }
    [[nodiscard]] std::uint32_t ImportNode() const noexcept { return importNode_; }
    [[nodiscard]] std::uint32_t ContextCheck() const noexcept { return contextCheck_; }
    // Value-only first refusal detail; no native pointers leave the engine thread.
    [[nodiscard]] std::uint32_t CaptureNode() const noexcept { return captureNode_; }
    [[nodiscard]] std::uint32_t MarkerIndex() const noexcept { return markerIndex_; }
    [[nodiscard]] std::uint32_t MarkerKind() const noexcept { return markerKind_; }
    [[nodiscard]] std::uint32_t MarkerTypeRva() const noexcept { return markerTypeRva_; }
    [[nodiscard]] const Key& Identity() const noexcept { return key_; }
private:
    friend struct NativeRouteResultTestAccess;
    struct Ref { std::uintptr_t object{}, control{}; };
    [[nodiscard]] bool Image() const noexcept;
    [[nodiscard]] bool Context(const LocalContext&, bool capturing, std::size_t count = 0,
        ImportPoint = ImportPoint::QueryDispatch,bool allowAppend = false) const noexcept;
    [[nodiscard]] Result PlanAdmission(const Graph&,const LocalContext&,const Protection&,AdmissionPlan&,
        ImportPoint,std::size_t resultCount) noexcept;
    [[nodiscard]] bool Protected(const Key&, const LocalContext&, const Protection&,
        std::span<const route_identity::ElementMapping>, GuardPhase) const noexcept;
    [[nodiscard]] bool ResolveOrdinary(const Protection&, std::uint32_t reference) noexcept;
    [[nodiscard]] bool ResolveSpecial(const Protection&, std::uint32_t reference, std::uintptr_t& link) noexcept;
    [[nodiscard]] bool Adjacent(const Protection&, std::uint32_t from, std::uint32_t to) noexcept;
    [[nodiscard]] bool Create(Ref&) noexcept;
    [[nodiscard]] bool Append(std::uintptr_t result, const Ref&) noexcept;
    [[nodiscard]] bool Release(Ref&) noexcept;
    [[nodiscard]] bool NodeValues(const Ref&, const Node&, std::uint32_t reference, std::uintptr_t link) noexcept;
    [[nodiscard]] bool Marker(std::uint32_t kind,Ref&) noexcept;
    [[nodiscard]] bool AttachMarker(std::uintptr_t node,const Ref&) noexcept;
    [[nodiscard]] bool Publish(const Graph&, const Protection&, const LocalContext&) noexcept;
    std::uintptr_t base_{};
    route_identity::Registry* registry_{};
    bool busy_{}, entered_{}, published_{}, retired_{};
    std::uint32_t importStage_{},importNode_{UINT32_MAX};
    mutable std::uint32_t contextCheck_{};
    std::uint32_t captureNode_{UINT32_MAX},markerIndex_{UINT32_MAX},markerKind_{UINT32_MAX},markerTypeRva_{UINT32_MAX};
    Key key_{};
    LocalContext local_{};
    Protection protection_{};
    ImportPoint importPoint_{};
    Ref pending_{},pendingMarker_{};
    std::unique_ptr<Graph> continuation_;
    // Private injected calls exercise the actual memory/ordering code in
    // tests. Production always calls the exact checked native ABI.
    void* callsContext_{};
    bool (*create_)(void*, std::uintptr_t&, std::uintptr_t&) noexcept{};
    bool (*append_)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t) noexcept{};
    bool (*release_)(void*, std::uintptr_t&, std::uintptr_t&) noexcept{};
    bool (*resolve_)(void*, std::uintptr_t, std::uint32_t) noexcept{};
    bool (*adjacent_)(void*, std::uint32_t, std::uint32_t) noexcept{};
    std::uintptr_t (*special_)(void*, std::uintptr_t, std::uint32_t) noexcept{};
    bool (*marker_)(void*,std::uint32_t,std::uintptr_t&,std::uintptr_t&) noexcept{};
    bool (*attachMarker_)(void*,std::uintptr_t,std::uintptr_t,std::uintptr_t) noexcept{};
};
} // namespace aimp::ai_sync::route
