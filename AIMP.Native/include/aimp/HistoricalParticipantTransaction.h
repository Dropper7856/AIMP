#pragma once

#include "aimp/NativeLimits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace aimp::historical {

// Exact layout demonstrated for the 2015-02-17 reference profile.  This
// component is deliberately not connected to Bootstrap or LauncherIPC:
// callers must still prove the build, level generation and game thread before
// using it with engine memory.
inline constexpr std::size_t kParticipantRecordSize = 0x158;
inline constexpr std::size_t kParticipantTableSize =
    kNativeParticipantCapacity * kParticipantRecordSize;
static_assert(kParticipantTableSize == 0xAC0);
inline constexpr std::size_t kNameOffset = 0x00;
inline constexpr std::size_t kNameCapacity = 0x80;
inline constexpr std::size_t kIdentityOffset = 0x80;
inline constexpr std::size_t kFlagsOffset = 0xC8;
inline constexpr std::size_t kDefaultVtableOffset = 0xCC;
inline constexpr std::size_t kAssociatedObjectOffset = 0xD0;
inline constexpr std::size_t kAssociatedValueOffset = 0xD4;
inline constexpr std::size_t kDefaultScaleOffset = 0xE8;
inline constexpr std::size_t kBindingKeyOffset = 0xF4;
inline constexpr std::size_t kTrailingIndexOffset = 0x150;

inline constexpr std::uint32_t kRemoteUnboundFlags = 0x7;
inline constexpr std::uint32_t kRemoteParticipantBit = 0x2;
inline constexpr std::uint32_t kBoundParticipantBit = 0x8;

using ParticipantIdentity128 = std::array<std::byte, 16>;
using ParticipantRecord = std::array<std::byte, kParticipantRecordSize>;

struct DummyParticipantRecordSpec {
    ParticipantIdentity128 nativeIdentity{};
    std::uint32_t bindingKeyF4{};
    std::string_view displayName;
};

// The table bytes model owner+0x000..owner+0xABF. countAddress models
// owner+0xAC4 and selectedLocalIndexAddress models the single byte at
// wrapper+0x4.
struct ParticipantTableView {
    std::span<std::byte> records;
    std::uint32_t* countAddress{};
    std::uint8_t* selectedLocalIndexAddress{};
};

enum class ParticipantTransactionCode : std::uint8_t {
    Ok,
    AlreadyActive,
    NoActiveTransaction,
    InvalidGeneration,
    GenerationMismatch,
    InvalidView,
    EmptyRegistry,
    CapacityReached,
    InvalidLocalIndex,
    LocalParticipantInvariantFailed,
    EmptyDisplayName,
    DisplayNameTooLong,
    InvalidIdentity,
    DuplicateIdentity,
    InvalidBindingKey,
    DuplicateBindingKey,
    InvalidDefaultVtable,
    RegistryChanged,
    DummyNotLast,
    PostAppendVerificationFailed,
};

struct ParticipantTransactionResult {
    ParticipantTransactionCode code{ParticipantTransactionCode::Ok};
    bool mutated{};
    bool rollbackPerformed{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return code == ParticipantTransactionCode::Ok;
    }
};

class HistoricalParticipantTransaction final {
public:
    // Constructs the exact 0x158 temporary record copied by FUN_007941D0 /
    // FUN_00785540 on the reference build. Unknown fields are initialized to
    // the known default state produced by FUN_007A3990. The default
    // participant vtable is taken from the already-live selected local
    // participant, avoiding an absolute address. Bytes whose constructor
    // semantics are not demonstrated are zero rather than copied from another
    // participant.
    [[nodiscard]] ParticipantTransactionResult ConstructDummyRecord(
        const ParticipantTableView& table,
        const DummyParticipantRecordSpec& spec,
        ParticipantRecord& output) const noexcept;

    // Testable model of the demonstrated append contract: copy into
    // base+count*0x158 and increment count. It records enough state for a
    // guarded last-slot rollback. It is not wired to runtime engine memory.
    [[nodiscard]] ParticipantTransactionResult AppendSynthetic(
        const ParticipantTableView& table,
        const DummyParticipantRecordSpec& spec,
        std::uint64_t levelGeneration) noexcept;

    // Verifies stable table ownership plus portable-safe local discriminators.
    // D0/D4 and the bound flag may change after Character binding.
    [[nodiscard]] ParticipantTransactionResult Validate(
        const ParticipantTableView& table,
        std::uint64_t levelGeneration) const noexcept;

    // Removes only the still-last Dummy slot. The count is decremented before
    // restoring the inactive slot snapshot, matching the observed clear order.
    [[nodiscard]] ParticipantTransactionResult RollbackLastSlot(
        const ParticipantTableView& table,
        std::uint64_t levelGeneration) noexcept;

    // Used on a level transition. It never dereferences addresses captured
    // from the obsolete generation.
    void InvalidateGeneration() noexcept;

    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] std::uint32_t DummyIndex() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] const ParticipantRecord& Record() const noexcept;

private:
    struct StableIdentity {
        ParticipantIdentity128 identity{};
        std::uint32_t bindingKeyF4{};
    };

    [[nodiscard]] ParticipantTransactionCode ValidateTableAndSpec(
        const ParticipantTableView& table,
        const DummyParticipantRecordSpec* spec,
        std::uint32_t& count,
        std::uint8_t& selectedLocalIndex,
        std::uint32_t& defaultVtable) const noexcept;
    [[nodiscard]] ParticipantTransactionCode ValidateActiveTable(
        const ParticipantTableView& table,
        std::uint64_t levelGeneration,
        bool requireRemoteBit) const noexcept;
    void ClearState() noexcept;

    bool active_{};
    std::byte* tableAddress_{};
    std::size_t tableSize_{};
    std::uint32_t* countAddress_{};
    std::uint8_t* selectedLocalIndexAddress_{};
    std::uint32_t countBefore_{};
    std::uint8_t selectedLocalIndexBefore_{};
    std::uint64_t generation_{};
    ParticipantIdentity128 dummyIdentity_{};
    std::uint32_t dummyBindingKeyF4_{};
    ParticipantRecord record_{};
    ParticipantRecord replacedInactiveSlot_{};
    std::array<StableIdentity, kNativeParticipantCapacity>
        originalParticipants_{};
};

[[nodiscard]] std::string_view ToString(
    ParticipantTransactionCode code) noexcept;

} // namespace aimp::historical
