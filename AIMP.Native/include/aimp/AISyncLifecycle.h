#pragma once

#include "aimp/AISyncInteraction.h"

namespace aimp::ai_sync::lifecycle {
// Backstage/frontstage are the certified Character presentation/collision
// transitions, not a claim that generic spawn/activation/conduits are solved.
enum class Operation : std::uint8_t {
    SetBackstage = 1, SetFrontstage, Spawn, Activate, Deactivate, Destroy, EnterConduit, ExitConduit
};
struct Command {
    interactions::Key key{};
    interactions::Participant actor{};
    Operation operation{Operation::SetFrontstage};
    std::uint32_t revision{}, resourceA{}, resourceB{};
    bool operator==(const Command&) const noexcept = default;
};
struct Binding {
    interactions::LocalParticipant actor{};
    // Local proof that conflicting movement/pair/physics owners have drained.
    bool executionQuiescent{};
    bool operator==(const Binding&) const noexcept = default;
};
enum class Result : std::uint8_t { Applied, Released, Duplicate, Waiting, Rejected, Unsupported, Conflict, Unknown, Reentrant };
struct Receipt {
    interactions::Key key{};
    bool nativeEntered{}, applied{}, unknown{}, resourcesReleased{};
};
using Permission = interactions::Check(*)(void*, const Command&, const Binding&) noexcept;
using ReleaseProof = interactions::Check(*)(void*, const Command&, const Binding&, const Receipt&) noexcept;

// Engine-thread adapter to 0078B140 (Character, one byte stack slot, RET4)
// and 007C0220 (AnimatedCharacter, no stack arguments, RET). Caller supplies
// exact build/resource/role/world checks. This never places a Character or
// reconstructs a spawn request from a snapshot. Unsupported operations are
// rejected before entry, even if the caller supplies a permissive callback.
class NativeCharacterAdapter final {
public:
    NativeCharacterAdapter() noexcept = default;
    NativeCharacterAdapter(const NativeCharacterAdapter&) = delete;
    NativeCharacterAdapter& operator=(const NativeCharacterAdapter&) = delete;
    NativeCharacterAdapter(NativeCharacterAdapter&&) = delete;
    NativeCharacterAdapter& operator=(NativeCharacterAdapter&&) = delete;
    [[nodiscard]] bool Initialize(std::uintptr_t imageBase, bool exactBuildValidated, Permission, void*) noexcept;
    [[nodiscard]] Result Apply(const Command&, const Binding&, Receipt&) noexcept;
    [[nodiscard]] Result Release(ReleaseProof, void*, Receipt&) noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] bool Find(Receipt&) const noexcept;
private:
    friend struct NativeCharacterAdapterTestAccess;
    [[nodiscard]] interactions::Check Check(const Command&, const Binding&) noexcept;
    [[nodiscard]] bool Enter(const Binding&, bool backstage) noexcept;
    [[nodiscard]] bool Inspect(const Binding&, bool& backstage) noexcept;
    std::uintptr_t base_{};
    Permission permission_{};
    void* context_{};
    Command command_{};
    Binding binding_{};
    Receipt receipt_{};
    std::uint64_t revocation_{};
    bool used_{}, busy_{};
    void* primitivesContext_{};
    interactions::Check (*checkPrimitive_)(void*, const Binding&) noexcept{};
    bool (*enterPrimitive_)(void*, const Binding&, bool) noexcept{};
    bool (*inspectPrimitive_)(void*, const Binding&, bool&) noexcept{};
};
} // namespace aimp::ai_sync::lifecycle
