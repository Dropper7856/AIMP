#pragma once

#include "aimp/AISyncTransactions.h"
#include "aimp/AISyncSchedulerControl.h"
#include <array>
#include <span>
#include <vector>

namespace aimp::ai_sync::execution {
inline constexpr std::uint32_t kMagic = 0x58454941U; // AIEX
inline constexpr std::uint16_t kVersion = 1U;
inline constexpr std::size_t kHeaderSize = 64, kBatchHeaderSize = 72, kMaxRecords = 16;
inline constexpr std::size_t kReceiptSize = 232;
inline constexpr std::uint16_t kMovementVersion = 2;
inline constexpr std::size_t kExtendedRecordSize = transactions::kRecordSize + 8 + movement::kBundleBytes;
inline constexpr std::uint16_t kNativeIntentVersion = 3;
inline constexpr std::size_t kNativeIntentRecordSize = kExtendedRecordSize + native_intent::kBytes;
inline constexpr std::size_t kMaximumReceiptBytes = kHeaderSize + kNativeIntentRecordSize + 8;
inline constexpr std::size_t kMaximumBatchBytes = kBatchHeaderSize + kMaxRecords * kNativeIntentRecordSize;
inline constexpr std::uint32_t kControlMagic = 0x43454941U;
enum class ControlKind : std::uint16_t { Offer = 1, Ready, Recovery, Close, Checkpoint, CheckpointAck };
enum class Recovery : std::uint32_t { RestartRequired = 1, Capacity, InvalidStream, NativeUnavailable, TransportLost };
struct Control {
    transactions::Scope scope{};
    ControlKind kind{ControlKind::Offer};
    std::uint32_t value{};
    std::uint64_t through{};
};
enum class Stage : std::uint32_t { Received = 1, Waiting, Entered, Admitted, Rejected, Unknown, Released, Superseded };
struct Batch {
    transactions::Scope scope{};
    std::uint64_t batchId{};
    std::array<transactions::Record, kMaxRecords> records{};
    std::size_t count{};
};
struct Receipt {
    transactions::Scope scope{};
    std::uint64_t batchId{};
    transactions::Record operation{};
    Stage stage{Stage::Received};
    transactions::Result result{transactions::Result::Ok};
};
// This is an explicit executable contract, NOT a promotion of a P1 observation.
// Profiles: native Move(1, optional Stop companion3), Stop(3), Turn(4), Stance(7),
// Action(5), Placement(8), typed native intent(0x200). HostRoute additionally requires bit8.
inline constexpr std::uint32_t kProfileTurn = 1U, kProfileStance = 2U, kProfileMove = 4U,
    kProfileRoutes = 8U, kProfileAction = 16U, kProfileDirected = 32U, kProfileLifecycle = 64U,
    kProfileNativeIntent = native_intent::kProfile, kProfileSchedulerControl = scheduler_control::kProfile,
    kProfilePlacement = 512U, kProfileMarker = 1024U, kKnownProfiles = 2047U;
inline constexpr std::uint32_t kMarkerKind = 2U;
// Kind8 is a native scheduler placement request, not an ordinary Move or a
// one-shot lifecycle call. goal=position+1C, aux=yaw+28, mode=branch byte+2C.
// Only canonical branch values0/1 are admitted; other native bytes are not
// silently normalized. Lifecycle children require parent-qualified dedup.
inline constexpr std::uint32_t kPlacementKind = 8U;
// Local integration proof, never network authority. Remains false until the
// embedding installs and qualifies both mode8 entry/update dedup boundaries.
void SetPlacementDedupReady(bool ready) noexcept;
[[nodiscard]] bool PlacementDedupReady() noexcept;
// Native kind5 (vtable EB198C) holds the scheduler while the Character's
// action cycle with state +1C runs; +20 interrupts that cycle on cancel.
inline constexpr std::uint32_t kActionKind = 5U, kActionNoState = 0x5DU;
// Native kind9 (vtable EB19AC): heading +1C, tolerance +20, distance +24.
// Carried as aux=heading, distance=tolerance, stopDistance=travel distance.
inline constexpr std::uint32_t kDirectedKind = 9U;
// Native Character lifecycle calls, not scheduler requests. Each is replayed as
// the exact CATHODE call the Host made, in order with every other command:
// Relocate = 007C6330(Character, position goal, yaw aux, mode); Backstage =
// 0078B140(Character, mode 0/1) (+ 007C0220 frontstage reset on exit).
inline constexpr std::uint32_t kRelocateKind = 0x100U, kBackstageKind = 0x101U, kFrontstageKind = 0x102U,
    kForcedRelocateKind = 0x103U, kDirectRelocateKind = 0x104U, kDirectForcedRelocateKind = 0x105U; // 007C63B0 -> 007C2710(...,true); no Client random spawn selection.
[[nodiscard]] constexpr std::uint32_t ProfileForKind(const std::uint32_t kind) noexcept {
    return kind == kMarkerKind ? kProfileMarker : kind == 1U || kind == 3U ? kProfileMove : kind == 4U ? kProfileTurn :
        kind == 7U ? kProfileStance : kind == kPlacementKind ? kProfilePlacement : kind == kActionKind ? kProfileAction :
        kind == kDirectedKind ? kProfileDirected : scheduler_control::Supported(kind) ? kProfileSchedulerControl :
        kind == native_intent::kKind ? kProfileNativeIntent :
        kind == kRelocateKind || kind == kBackstageKind || kind == kFrontstageKind || kind == kForcedRelocateKind || kind == kDirectRelocateKind || kind == kDirectForcedRelocateKind ? kProfileLifecycle : 0U;
}
[[nodiscard]] bool ValidateExecutable(const transactions::Record&) noexcept;
[[nodiscard]] bool EncodeBatch(const Batch&, std::vector<std::uint8_t>&) noexcept;
[[nodiscard]] bool DecodeBatch(std::span<const std::uint8_t>, const transactions::Scope& expected, Batch&) noexcept;
[[nodiscard]] bool DecodeBatchScope(std::span<const std::uint8_t>, transactions::Scope&) noexcept;
[[nodiscard]] bool EncodeReceipt(const Receipt&, std::array<std::uint8_t, kReceiptSize>&) noexcept;
[[nodiscard]] bool EncodeReceipt(const Receipt&, std::vector<std::uint8_t>&) noexcept;
[[nodiscard]] bool DecodeReceipt(std::span<const std::uint8_t>, const transactions::Scope& expected, Receipt&) noexcept;
[[nodiscard]] bool EncodeControl(const Control&, std::array<std::uint8_t, kHeaderSize>&) noexcept;
[[nodiscard]] bool DecodeControl(std::span<const std::uint8_t>, Control&) noexcept;
} // namespace aimp::ai_sync::execution
