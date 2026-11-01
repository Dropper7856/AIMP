#include "aimp/AISyncExecutionCodec.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>

namespace aimp::ai_sync::execution {
namespace {
std::atomic<bool> gPlacementDedupReady{};
using namespace transactions;
template<class T> void Put(std::span<std::uint8_t> bytes, std::size_t at, T value) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}
template<class T> T Get(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(bytes[at + i]) << (i * 8U);
    return value;
}
bool ValidScope(const Scope& scope) noexcept {
    return std::any_of(scope.session.begin(), scope.session.end(), [](auto b) { return b != 0; }) &&
        scope.authorityEpoch != 0 && scope.worldEpoch != 0 && scope.consumerEpoch != 0;
}
bool Zero(float value) noexcept { return std::bit_cast<std::uint32_t>(value) == 0U; }
bool ZeroFloats(const Record& r) noexcept {
    return Zero(r.goalX) && Zero(r.goalY) && Zero(r.goalZ) && Zero(r.distance) && Zero(r.stopDistance) && Zero(r.aux);
}
void Header(std::span<std::uint8_t> bytes, const Scope& scope, std::uint64_t id, std::uint16_t kind,
    std::uint16_t version = kVersion) noexcept {
    Put(bytes, 0, kMagic); Put(bytes, 4, version); Put(bytes, 6, kind);
    Put(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    std::copy(scope.session.begin(), scope.session.end(), bytes.begin() + 16);
    Put(bytes, 32, scope.authorityEpoch); Put(bytes, 40, scope.worldEpoch);
    Put(bytes, 48, scope.consumerEpoch); Put(bytes, 56, id);
}
bool ReadHeader(std::span<const std::uint8_t> bytes, const Scope& expected,
    std::uint16_t kind, std::uint64_t& id) noexcept {
    if (bytes.size() < kHeaderSize || !ValidScope(expected) || Get<std::uint32_t>(bytes, 0) != kMagic ||
        (Get<std::uint16_t>(bytes, 4) != kVersion && Get<std::uint16_t>(bytes,4) != kMovementVersion &&
         Get<std::uint16_t>(bytes,4) != kNativeIntentVersion) || Get<std::uint16_t>(bytes, 6) != kind ||
        Get<std::uint32_t>(bytes, 8) != bytes.size() || Get<std::uint32_t>(bytes, 12) != 0 ||
        !std::equal(expected.session.begin(), expected.session.end(), bytes.begin() + 16) ||
        Get<std::uint64_t>(bytes, 32) != expected.authorityEpoch ||
        Get<std::uint64_t>(bytes, 40) != expected.worldEpoch ||
        Get<std::uint64_t>(bytes, 48) != expected.consumerEpoch) return false;
    id = Get<std::uint64_t>(bytes, 56);
    return id != 0;
}
bool ValidReceipt(const Receipt& r) noexcept {
    return ValidScope(r.scope) && r.batchId != 0 && r.operation.scope == r.scope &&
        ValidateExecutable(r.operation) && r.stage >= Stage::Received && r.stage <= Stage::Superseded &&
        r.result >= Result::Ok && r.result <= Result::OutcomeUnknown;
}
std::size_t Stride(std::span<const std::uint8_t> bytes) noexcept {
    return Get<std::uint16_t>(bytes,4)==kNativeIntentVersion ? kNativeIntentRecordSize :
        Get<std::uint16_t>(bytes,4)==kMovementVersion ? kExtendedRecordSize : transactions::kRecordSize;
}
bool Intent(const Record& r) noexcept { return r.nativeKind == kMarkerKind || r.nativeKind == native_intent::kKind || !native_intent::Empty(r.intent); }
bool Extended(const Record& r) noexcept {return r.nativeKind==kMarkerKind||r.nativeKind==1||r.nativeKind==3||r.admission!=0||r.scalarRetention!=3;}
bool WriteOperation(std::span<std::uint8_t> bytes, const Record& record) noexcept {
    CanonicalRecord base{};
    if (transactions::Encode(record,base)!=Result::Ok) return false;
    std::copy(base.begin(),base.end(),bytes.begin());
    if (bytes.size()==kExtendedRecordSize || bytes.size()==kNativeIntentRecordSize) {
        Put(bytes,160,record.admission);Put(bytes,164,record.scalarRetention);
        movement::CanonicalBundle extension{};
        if (!movement::Encode(record.movement,extension))return false;
        std::copy(extension.begin(),extension.end(),bytes.begin()+transactions::kRecordSize+8);
    } else if(Extended(record) || !movement::Empty(record.movement)) return false;
    if (bytes.size()==kNativeIntentRecordSize) {
        native_intent::CanonicalPayload intent{};
        if (!native_intent::Encode(record.intent,intent)) return false;
        std::copy(intent.begin(),intent.end(),bytes.begin()+kExtendedRecordSize);
    } else if (Intent(record)) return false;
    return true;
}
bool ReadOperation(std::span<const std::uint8_t> bytes, Record& record) noexcept {
    if(transactions::Decode(bytes.first(transactions::kRecordSize),record)!=Result::Ok)return false;
    if(bytes.size()==kExtendedRecordSize || bytes.size()==kNativeIntentRecordSize) {
        record.admission=Get<std::uint32_t>(bytes,160);record.scalarRetention=Get<std::uint32_t>(bytes,164);
        if(!movement::Decode(bytes.subspan(transactions::kRecordSize+8,movement::kBundleBytes),record.movement))return false;
    } else if((record.nativeKind==kMarkerKind || record.nativeKind==1 || record.nativeKind==3))return false;
    if (bytes.size()==kNativeIntentRecordSize) {
        if (!native_intent::Decode(bytes.subspan(kExtendedRecordSize,native_intent::kBytes),record.intent)) return false;
    } else if (record.nativeKind==kMarkerKind || record.nativeKind==native_intent::kKind) return false;
    return ValidateExecutable(record);
}
}
void SetPlacementDedupReady(bool ready) noexcept { gPlacementDedupReady.store(ready,std::memory_order_release); }
bool PlacementDedupReady() noexcept { return gPlacementDedupReady.load(std::memory_order_acquire); }
bool ValidateExecutable(const transactions::Record& r) noexcept {
    using namespace transactions;
    if (Validate(r) != Result::Ok || (r.flags & (Incomplete | ObservationOnly | Accepted)) != 0 ||
        r.opcode == Op::Oracle || r.opcode == Op::CoverageLost || r.nativeStatus != 0 ||
        ProfileForKind(r.nativeKind) == 0 || (r.gait != 0 && r.nativeKind != kActionKind)) return false;
    if (r.nativeKind == kMarkerKind) {
        if (r.flags != 0 || r.mode != 0 || r.gait != 0 || !ZeroFloats(r) || r.scalarRetention != 3)
            return false;
        const bool payload = r.opcode == Op::Begin || r.opcode == Op::Submit || r.opcode == Op::Update;
        if (!payload) return native_intent::Empty(r.intent) && movement::Empty(r.movement);
        if (r.intent.words[0] != native_intent::kMarkerContract || !native_intent::Validate(r.intent) ||
            movement::Empty(r.movement) || !movement::Validate(r.movement) || r.movement.version != 1 ||
            r.movement.parts != 1 || r.movement.navigation != movement::Navigation::HostRoute ||
            r.movement.primaryFlags != 0) return false;
        // BEFORE is authored before0044D190; AFTER may change retained only.
        auto after = r.movement.after;
        after.retained = r.movement.before.retained;
        return movement::Same(r.movement.before, after);
    }
    if (r.nativeKind == native_intent::kKind) {
        if (r.intent.words[0] == native_intent::kMarkerContract) return false;
        if (r.admission != 1 || r.scalarRetention != 3 || r.flags != 0 || r.mode != 0 || r.gait != 0 ||
            !ZeroFloats(r) || !movement::Empty(r.movement)) return false;
        if (r.opcode == Op::Begin || r.opcode == Op::Submit)
            return !native_intent::Empty(r.intent) && native_intent::Validate(r.intent);
        return (r.opcode == Op::Complete || r.opcode == Op::Release) && native_intent::Empty(r.intent);
    }
    if (!native_intent::Empty(r.intent)) return false;
    const bool payload = r.opcode == Op::Begin || r.opcode == Op::Submit || r.opcode == Op::Update;
    if (!payload) return r.flags == 0 && r.mode == 0 && r.gait == 0 && ZeroFloats(r) && movement::Empty(r.movement) && r.scalarRetention==3;
    if (r.nativeKind==1 || r.nativeKind==3) return r.flags==0 && r.mode==0 && ZeroFloats(r) &&
        !movement::Empty(r.movement) && movement::Validate(r.movement) && r.scalarRetention==3 &&
        (r.nativeKind==1 ? r.movement.primaryFlags==0 : r.movement.parts==1);
    if (!movement::Empty(r.movement)) return false;
    if (scheduler_control::Supported(r.nativeKind))
        return r.flags == 0 && r.mode == 0 && r.gait == 0 && ZeroFloats(r);
    if (r.nativeKind == 7) return r.flags == 0 && r.mode <= 2 && ZeroFloats(r);
    if (r.nativeKind == kPlacementKind)
        return r.flags == HasGoal && r.mode <= 1U && std::abs(r.goalX) <= 100000.0F &&
            std::abs(r.goalY) <= 100000.0F && std::abs(r.goalZ) <= 100000.0F &&
            std::abs(r.aux) <= 6.4F && Zero(r.distance) && Zero(r.stopDistance);
    // Action: +1C cycle state, +20/+21/+22 booleans packed into gait bits0..2.
    if (r.nativeKind == kActionKind)
        return r.flags == 0 && r.mode <= 0xFFU && r.mode != kActionNoState && r.gait <= 7U && ZeroFloats(r);
    if (r.nativeKind == kRelocateKind || r.nativeKind == kForcedRelocateKind || r.nativeKind == kDirectRelocateKind || r.nativeKind == kDirectForcedRelocateKind)
        return r.opcode!=Op::Update && r.admission==1 && r.flags == HasGoal && r.mode <= 15U && std::abs(r.goalX) <= 100000.0F && std::abs(r.goalY) <= 100000.0F &&
            std::abs(r.goalZ) <= 100000.0F && std::abs(r.aux) <= 6.4F && Zero(r.distance) && Zero(r.stopDistance);
    // Backstage/frontstage: HasGoal carries the Host Character pose at that
    // native transition (goal=position, aux=yaw).
    if (r.nativeKind == kBackstageKind || r.nativeKind == kFrontstageKind)
        return r.opcode!=Op::Update && r.admission==1 && r.mode <= (r.nativeKind == kBackstageKind ? 1U : 0U) &&
            ((r.flags == 0 && ZeroFloats(r)) ||
             (r.flags == HasGoal && std::abs(r.goalX) <= 100000.0F && std::abs(r.goalY) <= 100000.0F &&
              std::abs(r.goalZ) <= 100000.0F && std::abs(r.aux) <= 6.4F && Zero(r.distance) && Zero(r.stopDistance)));
    if (r.nativeKind == kDirectedKind)
        return r.flags == 0 && r.mode == 0 && Zero(r.goalX) && Zero(r.goalY) && Zero(r.goalZ) &&
            std::abs(r.aux) <= 6.4F && r.distance <= 6.4F && r.stopDistance <= 1000.0F;
    if ((r.flags & ~HasGoal) != 0 || r.mode > 3 || r.distance > 6.4F || !Zero(r.stopDistance)) return false;
    if ((r.flags & HasGoal) != 0)
        return std::abs(r.goalX) <= 100000.0F && std::abs(r.goalY) <= 100000.0F &&
            std::abs(r.goalZ) <= 100000.0F && Zero(r.aux);
    return Zero(r.goalX) && Zero(r.goalY) && Zero(r.goalZ) && std::abs(r.aux) <= 6.4F;
}
bool EncodeBatch(const Batch& batch, std::vector<std::uint8_t>& output) noexcept {
    if (!ValidScope(batch.scope) || batch.batchId == 0 || batch.count == 0 || batch.count > kMaxRecords) return false;
    for (std::size_t i = 0; i < batch.count; ++i)
        if (batch.records[i].scope != batch.scope || !ValidateExecutable(batch.records[i])) return false;
    try {
        const bool extended=std::any_of(batch.records.begin(),batch.records.begin()+batch.count,Extended);
        const bool intent=std::any_of(batch.records.begin(),batch.records.begin()+batch.count,Intent);
        const auto stride=intent ? kNativeIntentRecordSize : extended ? kExtendedRecordSize : transactions::kRecordSize;
        std::vector<std::uint8_t> bytes(kBatchHeaderSize + batch.count * stride);
        Header(bytes, batch.scope, batch.batchId, 1,intent ? kNativeIntentVersion : extended ? kMovementVersion : kVersion);
        Put(std::span(bytes), 64, static_cast<std::uint32_t>(batch.count));
        for (std::size_t i = 0; i < batch.count; ++i) {
            if(!WriteOperation(std::span(bytes).subspan(kBatchHeaderSize+i*stride,stride),batch.records[i]))return false;
        }
        output.swap(bytes); return true;
    } catch (...) { return false; }
}
bool DecodeBatchScope(std::span<const std::uint8_t> bytes, transactions::Scope& output) noexcept {
    if (bytes.size() < kBatchHeaderSize || bytes.size() > kMaximumBatchBytes) return false;
    Scope scope{};
    std::copy(bytes.begin() + 16, bytes.begin() + 32, scope.session.begin());
    scope.authorityEpoch = Get<std::uint64_t>(bytes,32); scope.worldEpoch = Get<std::uint64_t>(bytes,40);
    scope.consumerEpoch = Get<std::uint64_t>(bytes,48);
    std::uint64_t id{};
    const auto count = Get<std::uint32_t>(bytes,64);
    if (!ReadHeader(bytes,scope,1,id) || Get<std::uint32_t>(bytes,68) != 0 || count == 0 || count > kMaxRecords ||
        bytes.size() != kBatchHeaderSize + count * Stride(bytes)) return false;
    output = scope; return true;
}
bool DecodeBatch(std::span<const std::uint8_t> bytes, const transactions::Scope& expected, Batch& output) noexcept {
    Scope scope{};
    if (!DecodeBatchScope(bytes,scope) || scope != expected) return false;
    Batch batch{}; batch.scope = expected;
    if (!ReadHeader(bytes, expected, 1, batch.batchId) || Get<std::uint32_t>(bytes, 68) != 0) return false;
    batch.count = Get<std::uint32_t>(bytes, 64);
    if (batch.count == 0 || batch.count > kMaxRecords ||
        bytes.size() != kBatchHeaderSize + batch.count * Stride(bytes)) return false;
    for (std::size_t i = 0; i < batch.count; ++i) {
        if (!ReadOperation(bytes.subspan(kBatchHeaderSize + i * Stride(bytes),Stride(bytes)),batch.records[i]) ||
            batch.records[i].scope != expected || !ValidateExecutable(batch.records[i])) return false;
    }
    output = batch; return true;
}
bool EncodeReceipt(const Receipt& r, std::array<std::uint8_t, kReceiptSize>& output) noexcept {
    if (!ValidReceipt(r) || Extended(r.operation) || Intent(r.operation)) return false;
    std::array<std::uint8_t, kReceiptSize> bytes{};
    Header(bytes, r.scope, r.batchId, 2);
    transactions::CanonicalRecord record{};
    if (transactions::Encode(r.operation, record) != transactions::Result::Ok) return false;
    std::copy(record.begin(), record.end(), bytes.begin() + kHeaderSize);
    Put(std::span(bytes), 224, static_cast<std::uint32_t>(r.stage));
    Put(std::span(bytes), 228, static_cast<std::uint32_t>(r.result)); output = bytes; return true;
}
bool EncodeReceipt(const Receipt& r, std::vector<std::uint8_t>& output) noexcept {
    if(!ValidReceipt(r))return false;
    try {
        const bool extended=Extended(r.operation);
        const bool intent=Intent(r.operation);
        const auto stride=intent ? kNativeIntentRecordSize : extended ? kExtendedRecordSize : transactions::kRecordSize;
        std::vector<std::uint8_t> bytes(kHeaderSize+stride+8);
        Header(bytes,r.scope,r.batchId,2,intent ? kNativeIntentVersion : extended ? kMovementVersion : kVersion);
        if(!WriteOperation(std::span(bytes).subspan(kHeaderSize,stride),r.operation))return false;
        Put(std::span(bytes),kHeaderSize+stride,static_cast<std::uint32_t>(r.stage));
        Put(std::span(bytes),kHeaderSize+stride+4,static_cast<std::uint32_t>(r.result));output.swap(bytes);return true;
    } catch(...) {return false;}
}
bool DecodeReceipt(std::span<const std::uint8_t> bytes, const transactions::Scope& expected, Receipt& output) noexcept {
    if (bytes.size()!=kReceiptSize && bytes.size()!=kHeaderSize+kExtendedRecordSize+8 && bytes.size()!=kMaximumReceiptBytes) return false;
    Receipt receipt{}; receipt.scope = expected;
    if (!ReadHeader(bytes, expected, 2, receipt.batchId) || bytes.size()!=kHeaderSize+Stride(bytes)+8 ||
        !ReadOperation(bytes.subspan(kHeaderSize,Stride(bytes)),receipt.operation))
        return false;
    receipt.stage = static_cast<Stage>(Get<std::uint32_t>(bytes, kHeaderSize+Stride(bytes)));
    const auto rawResult = Get<std::uint32_t>(bytes, kHeaderSize+Stride(bytes)+4);
    if (rawResult > static_cast<std::uint32_t>(transactions::Result::OutcomeUnknown)) return false;
    receipt.result = static_cast<transactions::Result>(rawResult);
    if (!ValidReceipt(receipt)) return false;
    output = receipt; return true;
}
bool EncodeControl(const Control& control, std::array<std::uint8_t, kHeaderSize>& output) noexcept {
    if (!ValidScope(control.scope)) return false;
    const bool checkpoint=control.kind==ControlKind::Checkpoint||control.kind==ControlKind::CheckpointAck;
    if(checkpoint ? (!control.through||control.value!=0) : control.through!=0)return false;
    if (control.kind == ControlKind::Offer || control.kind == ControlKind::Ready) {
        if (control.value == 0 || (control.value & ~kKnownProfiles) != 0 ||
            ((control.value&kProfileRoutes)!=0&&(control.value&(kProfileMove|kProfileMarker))==0)) return false;
    } else if (control.kind == ControlKind::Recovery) {
        if (control.value < 1 || control.value > 5) return false;
    } else if (!checkpoint && (control.kind != ControlKind::Close || control.value != 0)) return false;
    std::array<std::uint8_t, kHeaderSize> bytes{};
    Put(std::span(bytes), 0, kControlMagic); Put(std::span(bytes), 4, kVersion);
    Put(std::span(bytes), 6, static_cast<std::uint16_t>(control.kind));
    Put(std::span(bytes), 8, static_cast<std::uint32_t>(kHeaderSize)); Put(std::span(bytes), 12, control.value);
    std::copy(control.scope.session.begin(), control.scope.session.end(), bytes.begin() + 16);
    Put(std::span(bytes), 32, control.scope.authorityEpoch); Put(std::span(bytes), 40, control.scope.worldEpoch);
    Put(std::span(bytes), 48, control.scope.consumerEpoch); Put(std::span(bytes),56,control.through); output = bytes; return true;
}
bool DecodeControl(std::span<const std::uint8_t> bytes, Control& output) noexcept {
    if (bytes.size() != kHeaderSize || Get<std::uint32_t>(bytes, 0) != kControlMagic ||
        Get<std::uint16_t>(bytes, 4) != kVersion || Get<std::uint32_t>(bytes, 8) != kHeaderSize) return false;
    Control control{};
    std::copy(bytes.begin() + 16, bytes.begin() + 32, control.scope.session.begin());
    control.scope.authorityEpoch = Get<std::uint64_t>(bytes, 32);
    control.scope.worldEpoch = Get<std::uint64_t>(bytes, 40);
    control.scope.consumerEpoch = Get<std::uint64_t>(bytes, 48);
    control.kind = static_cast<ControlKind>(Get<std::uint16_t>(bytes, 6)); control.value = Get<std::uint32_t>(bytes, 12);
    control.through=Get<std::uint64_t>(bytes,56);
    std::array<std::uint8_t, kHeaderSize> canonical{};
    if (!EncodeControl(control, canonical) || !std::equal(bytes.begin(), bytes.end(), canonical.begin())) return false;
    output = control; return true;
}
} // namespace aimp::ai_sync::execution
