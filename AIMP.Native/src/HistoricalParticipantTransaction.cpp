#include "aimp/HistoricalParticipantTransaction.h"

#include <algorithm>
#include <cstring>

namespace aimp::historical {
namespace {

template <typename T>
[[nodiscard]] T ReadValue(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

template <typename T>
void WriteValue(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const T value) noexcept {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

[[nodiscard]] std::span<std::byte> RecordAt(
    const ParticipantTableView& table,
    const std::uint32_t index) noexcept {
    return table.records.subspan(
        static_cast<std::size_t>(index) * kParticipantRecordSize,
        kParticipantRecordSize);
}

[[nodiscard]] std::span<const std::byte> ConstRecordAt(
    const ParticipantTableView& table,
    const std::uint32_t index) noexcept {
    return std::span<const std::byte>(
        table.records.data() +
            static_cast<std::size_t>(index) * kParticipantRecordSize,
        kParticipantRecordSize);
}

[[nodiscard]] ParticipantIdentity128 ReadIdentity(
    const std::span<const std::byte> record) noexcept {
    ParticipantIdentity128 identity{};
    std::memcpy(
        identity.data(),
        record.data() + kIdentityOffset,
        identity.size());
    return identity;
}

[[nodiscard]] bool IsZeroIdentity(
    const ParticipantIdentity128& identity) noexcept {
    return std::all_of(
        identity.begin(),
        identity.end(),
        [](const std::byte value) {
            return value == std::byte{0};
        });
}

[[nodiscard]] bool HasEmbeddedNull(
    const std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

} // namespace

ParticipantTransactionResult
HistoricalParticipantTransaction::ConstructDummyRecord(
    const ParticipantTableView& table,
    const DummyParticipantRecordSpec& spec,
    ParticipantRecord& output) const noexcept {
    std::uint32_t count = 0;
    std::uint8_t selectedLocalIndex = 0;
    std::uint32_t defaultVtable = 0;
    const auto code = ValidateTableAndSpec(
        table,
        &spec,
        count,
        selectedLocalIndex,
        defaultVtable);
    if (code != ParticipantTransactionCode::Ok) {
        output.fill(std::byte{0});
        return {code, false, false};
    }

    output.fill(std::byte{0});
    std::memcpy(
        output.data() + kNameOffset,
        spec.displayName.data(),
        spec.displayName.size());
    std::memcpy(
        output.data() + kIdentityOffset,
        spec.nativeIdentity.data(),
        spec.nativeIdentity.size());
    const auto record = std::span<std::byte>(output);
    WriteValue(record, kFlagsOffset, kRemoteUnboundFlags);
    WriteValue(record, kDefaultVtableOffset, defaultVtable);
    WriteValue<std::uint32_t>(record, kAssociatedObjectOffset, 0);
    WriteValue<std::uint32_t>(record, kAssociatedValueOffset, 0);
    WriteValue<std::uint32_t>(
        record,
        kDefaultScaleOffset,
        0x3F800000U);
    WriteValue(record, kBindingKeyOffset, spec.bindingKeyF4);
    WriteValue<std::uint32_t>(
        record,
        kTrailingIndexOffset,
        0xFFFFFFFFU);
    return {};
}

ParticipantTransactionResult
HistoricalParticipantTransaction::AppendSynthetic(
    const ParticipantTableView& table,
    const DummyParticipantRecordSpec& spec,
    const std::uint64_t levelGeneration) noexcept {
    if (active_) {
        return {
            ParticipantTransactionCode::AlreadyActive,
            false,
            false};
    }
    if (levelGeneration == 0) {
        return {
            ParticipantTransactionCode::InvalidGeneration,
            false,
            false};
    }

    ParticipantRecord prepared{};
    const auto preparedResult =
        ConstructDummyRecord(table, spec, prepared);
    if (!preparedResult.Succeeded()) {
        return preparedResult;
    }

    std::uint32_t count = 0;
    std::uint8_t selectedLocalIndex = 0;
    std::uint32_t defaultVtable = 0;
    const auto revalidation = ValidateTableAndSpec(
        table,
        &spec,
        count,
        selectedLocalIndex,
        defaultVtable);
    if (revalidation != ParticipantTransactionCode::Ok) {
        return {revalidation, false, false};
    }
    if (ReadValue<std::uint32_t>(
            std::span<const std::byte>(prepared),
            kDefaultVtableOffset) != defaultVtable) {
        return {
            ParticipantTransactionCode::RegistryChanged,
            false,
            false};
    }
    tableAddress_ = table.records.data();
    tableSize_ = table.records.size();
    countAddress_ = table.countAddress;
    selectedLocalIndexAddress_ =
        table.selectedLocalIndexAddress;
    countBefore_ = count;
    selectedLocalIndexBefore_ = selectedLocalIndex;
    generation_ = levelGeneration;
    dummyIdentity_ = spec.nativeIdentity;
    dummyBindingKeyF4_ = spec.bindingKeyF4;
    record_ = prepared;
    std::memcpy(
        replacedInactiveSlot_.data(),
        RecordAt(table, count).data(),
        replacedInactiveSlot_.size());
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto existing = ConstRecordAt(table, index);
        originalParticipants_[index].identity =
            ReadIdentity(existing);
        originalParticipants_[index].bindingKeyF4 =
            ReadValue<std::uint32_t>(
                existing,
                kBindingKeyOffset);
    }
    active_ = true;

    // FUN_007941D0 reserves the next index by incrementing the count and then
    // copies the complete record with FUN_00785540. Remote flags (bit 1 set)
    // deliberately leave wrapper+4 unchanged.
    *table.countAddress = count + 1;
    std::memcpy(
        RecordAt(table, count).data(),
        prepared.data(),
        prepared.size());

    auto verification =
        ValidateActiveTable(table, levelGeneration, true);
    if (verification == ParticipantTransactionCode::Ok &&
        RecordAt(table, count).size() == prepared.size() &&
        std::memcmp(
            RecordAt(table, count).data(),
            prepared.data(),
            prepared.size()) == 0) {
        return {ParticipantTransactionCode::Ok, true, false};
    }

    const auto rollback =
        RollbackLastSlot(table, levelGeneration);
    return {
        ParticipantTransactionCode::PostAppendVerificationFailed,
        true,
        rollback.Succeeded()};
}

ParticipantTransactionResult
HistoricalParticipantTransaction::Validate(
    const ParticipantTableView& table,
    const std::uint64_t levelGeneration) const noexcept {
    return {
        ValidateActiveTable(table, levelGeneration, true),
        false,
        false};
}

ParticipantTransactionResult
HistoricalParticipantTransaction::RollbackLastSlot(
    const ParticipantTableView& table,
    const std::uint64_t levelGeneration) noexcept {
    const auto validation =
        ValidateActiveTable(table, levelGeneration, false);
    if (validation != ParticipantTransactionCode::Ok) {
        return {validation, false, false};
    }

    const auto dummyRecord = ConstRecordAt(table, countBefore_);
    if (ReadIdentity(dummyRecord) != dummyIdentity_ ||
        ReadValue<std::uint32_t>(
            dummyRecord,
            kBindingKeyOffset) != dummyBindingKeyF4_) {
        return {
            ParticipantTransactionCode::DummyNotLast,
            false,
            false};
    }

    // FUN_007A4FD0 decrements the active count before resetting the released
    // slot. Restoring the full pre-append snapshot also preserves any
    // constructor sentinel that occupied the inactive slot.
    *table.countAddress = countBefore_;
    std::memcpy(
        RecordAt(table, countBefore_).data(),
        replacedInactiveSlot_.data(),
        replacedInactiveSlot_.size());
    ClearState();
    return {ParticipantTransactionCode::Ok, true, true};
}

void HistoricalParticipantTransaction::InvalidateGeneration() noexcept {
    ClearState();
}

bool HistoricalParticipantTransaction::Active() const noexcept {
    return active_;
}

std::uint32_t HistoricalParticipantTransaction::DummyIndex() const noexcept {
    return active_ ? countBefore_ : 0;
}

std::uint64_t HistoricalParticipantTransaction::Generation() const noexcept {
    return generation_;
}

const ParticipantRecord&
HistoricalParticipantTransaction::Record() const noexcept {
    return record_;
}

ParticipantTransactionCode
HistoricalParticipantTransaction::ValidateTableAndSpec(
    const ParticipantTableView& table,
    const DummyParticipantRecordSpec* const spec,
    std::uint32_t& count,
    std::uint8_t& selectedLocalIndex,
    std::uint32_t& defaultVtable) const noexcept {
    if (table.records.data() == nullptr ||
        table.records.size() != kParticipantTableSize ||
        table.countAddress == nullptr ||
        table.selectedLocalIndexAddress == nullptr) {
        return ParticipantTransactionCode::InvalidView;
    }
    count = *table.countAddress;
    selectedLocalIndex = *table.selectedLocalIndexAddress;
    if (count == 0) {
        return ParticipantTransactionCode::EmptyRegistry;
    }
    if (!ParticipantSlotAvailable(count)) {
        return ParticipantTransactionCode::CapacityReached;
    }
    if (selectedLocalIndex >= count) {
        return ParticipantTransactionCode::InvalidLocalIndex;
    }

    const auto local = ConstRecordAt(table, selectedLocalIndex);
    const auto localFlags =
        ReadValue<std::uint32_t>(local, kFlagsOffset);
    if ((localFlags & 0x1U) == 0 ||
        (localFlags & kRemoteParticipantBit) != 0) {
        return
            ParticipantTransactionCode::LocalParticipantInvariantFailed;
    }
    defaultVtable = ReadValue<std::uint32_t>(
        local,
        kDefaultVtableOffset);
    if (defaultVtable == 0) {
        return ParticipantTransactionCode::InvalidDefaultVtable;
    }

    if (spec == nullptr) {
        return ParticipantTransactionCode::Ok;
    }
    if (spec->displayName.empty() ||
        HasEmbeddedNull(spec->displayName)) {
        return ParticipantTransactionCode::EmptyDisplayName;
    }
    if (spec->displayName.size() >= kNameCapacity) {
        return ParticipantTransactionCode::DisplayNameTooLong;
    }
    if (IsZeroIdentity(spec->nativeIdentity)) {
        return ParticipantTransactionCode::InvalidIdentity;
    }
    if (spec->bindingKeyF4 == 0) {
        return ParticipantTransactionCode::InvalidBindingKey;
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        const auto existing = ConstRecordAt(table, index);
        if (ReadIdentity(existing) == spec->nativeIdentity) {
            return ParticipantTransactionCode::DuplicateIdentity;
        }
        if (ReadValue<std::uint32_t>(
                existing,
                kBindingKeyOffset) == spec->bindingKeyF4) {
            return ParticipantTransactionCode::DuplicateBindingKey;
        }
    }
    return ParticipantTransactionCode::Ok;
}

ParticipantTransactionCode
HistoricalParticipantTransaction::ValidateActiveTable(
    const ParticipantTableView& table,
    const std::uint64_t levelGeneration,
    const bool requireRemoteBit) const noexcept {
    if (!active_) {
        return ParticipantTransactionCode::NoActiveTransaction;
    }
    if (levelGeneration != generation_) {
        return ParticipantTransactionCode::GenerationMismatch;
    }
    if (table.records.data() != tableAddress_ ||
        table.records.size() != tableSize_ ||
        table.countAddress != countAddress_ ||
        table.selectedLocalIndexAddress !=
            selectedLocalIndexAddress_) {
        return ParticipantTransactionCode::InvalidView;
    }
    if (*table.countAddress != countBefore_ + 1 ||
        *table.selectedLocalIndexAddress !=
            selectedLocalIndexBefore_) {
        return ParticipantTransactionCode::RegistryChanged;
    }
    for (std::uint32_t index = 0; index < countBefore_; ++index) {
        const auto existing = ConstRecordAt(table, index);
        if (ReadIdentity(existing) !=
                originalParticipants_[index].identity ||
            ReadValue<std::uint32_t>(
                existing,
                kBindingKeyOffset) !=
                originalParticipants_[index].bindingKeyF4) {
            return ParticipantTransactionCode::RegistryChanged;
        }
    }

    const auto dummy = ConstRecordAt(table, countBefore_);
    if (ReadIdentity(dummy) != dummyIdentity_ ||
        ReadValue<std::uint32_t>(
            dummy,
            kBindingKeyOffset) != dummyBindingKeyF4_) {
        return ParticipantTransactionCode::DummyNotLast;
    }
    if (requireRemoteBit &&
        (ReadValue<std::uint32_t>(
             dummy,
             kFlagsOffset) &
         kRemoteParticipantBit) == 0) {
        return ParticipantTransactionCode::DummyNotLast;
    }
    return ParticipantTransactionCode::Ok;
}

void HistoricalParticipantTransaction::ClearState() noexcept {
    active_ = false;
    tableAddress_ = nullptr;
    tableSize_ = 0;
    countAddress_ = nullptr;
    selectedLocalIndexAddress_ = nullptr;
    countBefore_ = 0;
    selectedLocalIndexBefore_ = 0;
    generation_ = 0;
    dummyIdentity_.fill(std::byte{0});
    dummyBindingKeyF4_ = 0;
    record_.fill(std::byte{0});
    replacedInactiveSlot_.fill(std::byte{0});
    originalParticipants_.fill({});
}

std::string_view ToString(
    const ParticipantTransactionCode code) noexcept {
    switch (code) {
    case ParticipantTransactionCode::Ok:
        return "ok";
    case ParticipantTransactionCode::AlreadyActive:
        return "already_active";
    case ParticipantTransactionCode::NoActiveTransaction:
        return "no_active_transaction";
    case ParticipantTransactionCode::InvalidGeneration:
        return "invalid_generation";
    case ParticipantTransactionCode::GenerationMismatch:
        return "generation_mismatch";
    case ParticipantTransactionCode::InvalidView:
        return "invalid_view";
    case ParticipantTransactionCode::EmptyRegistry:
        return "empty_registry";
    case ParticipantTransactionCode::CapacityReached:
        return "capacity_reached";
    case ParticipantTransactionCode::InvalidLocalIndex:
        return "invalid_local_index";
    case ParticipantTransactionCode::LocalParticipantInvariantFailed:
        return "local_participant_invariant_failed";
    case ParticipantTransactionCode::EmptyDisplayName:
        return "empty_display_name";
    case ParticipantTransactionCode::DisplayNameTooLong:
        return "display_name_too_long";
    case ParticipantTransactionCode::InvalidIdentity:
        return "invalid_identity";
    case ParticipantTransactionCode::DuplicateIdentity:
        return "duplicate_identity";
    case ParticipantTransactionCode::InvalidBindingKey:
        return "invalid_binding_key";
    case ParticipantTransactionCode::DuplicateBindingKey:
        return "duplicate_binding_key";
    case ParticipantTransactionCode::InvalidDefaultVtable:
        return "invalid_default_vtable";
    case ParticipantTransactionCode::RegistryChanged:
        return "registry_changed";
    case ParticipantTransactionCode::DummyNotLast:
        return "dummy_not_last";
    case ParticipantTransactionCode::PostAppendVerificationFailed:
        return "post_append_verification_failed";
    }
    return "unknown";
}

} // namespace aimp::historical
