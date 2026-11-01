#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::ai_sync::marker_identity {

inline constexpr std::uint32_t kCoverVersion = 7;
inline constexpr std::size_t kMaxLevelBytes = 96;
inline constexpr std::size_t kMaxContentBytes = 16U * 1024U * 1024U;
using Digest = std::array<std::uint8_t, 32>;

// Portable value schema, NOT a packed wire/native structure. Cover ordinal is
// ONE based (native marker+18); marker ordinal within cover is ZERO based.
// SHA256(domain || stateLE32 || level[96] || SHA256(entire original cvr2 stream)).
// State/level are folded into the digest: wire schema is exactly 32+4+4 bytes.
struct Key {
    Digest contentSha256{};
    std::uint32_t coverOrdinal{};
    std::uint32_t markerOrdinal{};
    bool operator==(const Key&) const noexcept = default;
};

enum class Result : std::uint8_t {
    Ok, InvalidKey, InvalidContent, UnsupportedVersion, ContentTooLarge,
    InvalidOrdinal, ContentMismatch, HashUnavailable,
    NativeLoaderBindingRequired, WrongThread, WrongWorld, MissingResource,
    StaleBinding, NativeMismatch, InvalidGeneration, CapacityExceeded, NativeUseRejected
};

[[nodiscard]] Result Validate(const Key& key) noexcept;
[[nodiscard]] Result HashImmutableContent(std::span<const std::byte> bytes,
    Digest& output) noexcept;

// Offline helpers only. Bytes must be the unmodified original stream; these
// routines do NOT prove that any native world loaded it. Never hash runtime
// records or reread a path and assert it is the stream the engine consumed.
// Unsupported/truncated/overlong streams and inconsistent totals are rejected.
[[nodiscard]] Result DescribeSerializedMarker(std::span<const std::byte> bytes,
    const std::array<char, kMaxLevelBytes>& level, std::uint32_t state,
    std::uint32_t coverOrdinal, std::uint32_t markerOrdinal, Key& output) noexcept;
[[nodiscard]] Result FindSerializedMarker(std::span<const std::byte> bytes,
    const std::array<char, kMaxLevelBytes>& level, std::uint32_t state,
    const Key& key, std::uint32_t& flatOrdinal) noexcept;

using EngineThread = bool(*)() noexcept;
// Install BEFORE the cover load. Exact-build HookManager qualification required.
// Observes actual original stream; never opens files. A late install cannot
// recover an already loaded resource. Retire before uninstalling these hooks;
// HookManager/trampolines must outlive all detours. Observation runs on the
// loader's own thread, without additional CATHODE calls or native mutation.
[[nodiscard]] bool Install(std::uintptr_t imageBase, HookManager&, EngineThread,
    std::string& error);
void Retire() noexcept;
// Engine thread only. world==0 selects the actual current singleton world.
// generation is caller-owned, nonzero and monotonically increasing on world
// replacement (same-world state reload uses a new internal load incarnation).
// Returned marker is borrowed for immediate engine-thread use; no native Ref
// exists for these world-owned arrays. Never retain just the raw marker.
[[nodiscard]] Result Capture(std::uintptr_t imageBase, std::uintptr_t world,
    std::uint64_t generation, std::int32_t localMarker, Key& output) noexcept;
[[nodiscard]] Result Resolve(std::uintptr_t imageBase, std::uintptr_t world,
    std::uint64_t generation, const Key& key, std::int32_t& localMarker) noexcept;

struct Mapping {
    Key key{};
    std::uintptr_t world{};
    std::uint64_t generation{}, load{};
    std::int32_t marker{-1};
};
[[nodiscard]] Result Resolve(std::uintptr_t imageBase, std::uintptr_t world,
    std::uint64_t generation, const Key& key, Mapping& output) noexcept;
[[nodiscard]] Result Revalidate(std::uintptr_t imageBase, const Mapping&) noexcept;
// Holds the same gate as pre-free revocation for the whole synchronous use.
// Callback must not unload/reload the world or retain the borrowed mapping.
// Parent owns request/world retirement after admission; this is not a lease
// extending native storage lifetime beyond the callback.
using ResolvedUse = bool(*)(const Mapping&, void*) noexcept;
[[nodiscard]] Result UseResolved(std::uintptr_t imageBase, std::uintptr_t world,
    std::uint64_t generation, const Key&, ResolvedUse, void* context) noexcept;

#ifdef AIMP_MARKER_IDENTITY_TESTING
namespace testing {
void Configure(std::uintptr_t imageBase, EngineThread) noexcept;
// Exercises the same snapshot/commit/revoke code as the detours, using local
// synthetic native tables. Not compiled/exported in production.
[[nodiscard]] bool Load(std::uintptr_t world, std::uint32_t state,
    const std::array<char, kMaxLevelBytes>& level, std::span<const std::byte> bytes,
    std::uint32_t consumedBytes = UINT32_MAX, std::uint32_t streamError = 0) noexcept;
void Unload(std::uintptr_t world) noexcept;
}
#endif

} // namespace aimp::ai_sync::marker_identity
