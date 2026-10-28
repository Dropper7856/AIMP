#pragma once
#include "aimp/AISyncExecutionCodec.h"

namespace aimp::ai_sync::action_correlation {
enum class Readiness : std::uint8_t { Waiting, Ready, Superseded, Rejected };
// Value-only reference to an observed native kind5 reservation, NOT proof
// that the reservation created the cycle, or permission to replay anything.
// Scope is the Host SOURCE scope (consumer=1). Peer projection requires the
// current authenticated execution binding, never a value from the snapshot.
#pragma pack(push,1)
struct Reference {
    transactions::Scope source{};
    std::uint64_t ownerEpoch{},command{},globalSequence{},ownerSequence{};
    std::uint32_t parameterRevision{},nativeKind{},executionRevision{},cycleState{};
    bool operator==(const Reference&) const noexcept = default;
};
#pragma pack(pop)
static_assert(sizeof(Reference)==88);
[[nodiscard]] inline bool Empty(const Reference& value) noexcept {return value==Reference{};}
[[nodiscard]] inline bool Valid(const Reference& value,const transactions::OwnerKey& owner) noexcept {
    if(Empty(value))return true;
    if(value.source.consumerEpoch!=1||value.nativeKind!=execution::kActionKind||!value.executionRevision||
        value.cycleState>0xFF||value.cycleState==execution::kActionNoState||value.ownerEpoch!=owner.ownerEpoch)return false;
    transactions::Record record{};record.scope=value.source;record.owner=owner;record.commandId=value.command;
    record.globalSeq=value.globalSequence;record.ownerSeq=value.ownerSequence;record.parameterRevision=value.parameterRevision;
    record.nativeKind=value.nativeKind;record.mode=value.cycleState;record.opcode=transactions::Op::Submit;
    return execution::ValidateExecutable(record);
}
[[nodiscard]] inline bool Capture(const transactions::Record& operation,std::uint32_t revision,Reference& output) noexcept {
    if(operation.nativeKind!=execution::kActionKind||operation.scope.consumerEpoch!=1||
        (operation.opcode!=transactions::Op::Submit&&operation.opcode!=transactions::Op::Update)||!execution::ValidateExecutable(operation))return false;
    Reference next{operation.scope,operation.owner.ownerEpoch,operation.commandId,operation.globalSeq,operation.ownerSeq,
        operation.parameterRevision,operation.nativeKind,revision,operation.mode};
    if(!Valid(next,operation.owner))return false;
    output=next;return true;
}
// Read-only correlation against an already authenticated/validated AIEX
// operation. Does not inspect native status, acquire ownership or grant entry.
[[nodiscard]] inline bool Matches(const Reference& source,const transactions::OwnerKey& actor,
    const transactions::Record& operation,const transactions::Scope& consumer) noexcept {
    return !Empty(source)&&Valid(source,actor)&&execution::ValidateExecutable(operation)&&
        (operation.opcode==transactions::Op::Submit||operation.opcode==transactions::Op::Update)&&
        consumer.consumerEpoch!=0&&operation.scope==consumer&&source.source.session==consumer.session&&
        source.source.authorityEpoch==consumer.authorityEpoch&&source.source.worldEpoch==consumer.worldEpoch&&
        operation.owner==actor&&source.command==operation.commandId&&source.parameterRevision==operation.parameterRevision&&
        source.nativeKind==operation.nativeKind&&source.globalSequence==operation.globalSeq&&source.ownerSequence==operation.ownerSeq&&
        source.cycleState==operation.mode;
}
} // namespace aimp::ai_sync::action_correlation
