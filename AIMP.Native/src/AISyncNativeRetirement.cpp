#include "aimp/AISyncNativeRequestAdapter.h"
#include "aimp/AISyncExecutionCodec.h"
#include "aimp/CharacterResourceAdmission.h"
#include <Windows.h>
#include <algorithm>

namespace aimp::ai_sync::execution {
namespace {
constexpr std::uintptr_t kScheduler = 0x2CD0, kCancel = 0x00283DB0, kRelease = 0xD770;
template<class T> bool Read(std::uintptr_t at, T& value) noexcept {
    __try { value = *reinterpret_cast<const T*>(at); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct BusyGuard {
    bool& busy;
    explicit BusyGuard(bool& value) noexcept : busy(value) { busy = true; }
    ~BusyGuard() { busy = false; }
};
}
bool NativeRequestAdapter::BeginRetirement(const transactions::Scope& scope) noexcept {
    if (busy_ || base_ == 0) return false;
    if (retiring_) return scope == retirementScope_;
    std::array<std::uint8_t,kHeaderSize> bytes{};
    if (!EncodeControl({scope,ControlKind::Offer,3},bytes)) return false;
    retirementScope_ = scope; retiring_ = true; retirementCursor_ = 0; return true;
}
bool NativeRequestAdapter::EndRetirement(const transactions::Scope& next) noexcept {
    if (busy_ || !retiring_ || next == retirementScope_ || ActiveCount() != 0) return false;
    retiring_ = false; return true;
}
RetirementProgress NativeRequestAdapter::RetirementState() const noexcept {
    RetirementProgress progress{}; progress.started = retiring_;
    for (const auto& lease : leases_) {
        if (!lease.used) continue;
        ++progress.remaining;
        if (lease.unknown || (retiring_ && lease.scope != retirementScope_)) ++progress.unknown;
        else ++progress.waiting;
    }
    progress.complete = retiring_ && progress.remaining == 0 && !busy_;
    return progress;
}
bool NativeRequestAdapter::RetirementLive(const Lease& lease) const noexcept {
    std::uint32_t runtime{},status{}; std::uintptr_t owner{}; LONG strong{};
    return (lease.kind == 1 || lease.kind == 2 || lease.kind == 3 || lease.kind == 4 || lease.kind == 5 || lease.kind == 7 || lease.kind == 9 ||
        scheduler_control::Supported(lease.kind)) && lease.actor.exclusiveExecution &&
        Read(lease.ref.control + 4, strong) && strong > 0 &&
        Read(lease.ref.object + 0xC,status) && status <= 6U &&
        Read(lease.actor.character + 0x50, runtime) && runtime == lease.actor.runtimeId &&
        Read(lease.actor.character + kScheduler, owner) && owner == lease.actor.character && Live(lease,lease.actor);
}
bool NativeRequestAdapter::CancelRetiring(std::uintptr_t request) noexcept {
    __try {
        if (retirementCancel_) return retirementCancel_(retirementCallsContext_,request);
        reinterpret_cast<void(__thiscall*)(void*)>(base_ + kCancel)(reinterpret_cast<void*>(request)); return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeRequestAdapter::ReleaseRetiring(Ref& ref) noexcept {
    __try {
        if (retirementRelease_) return retirementRelease_(retirementCallsContext_,reinterpret_cast<std::uintptr_t>(&ref));
        reinterpret_cast<void(__thiscall*)(Ref*)>(base_ + kRelease)(&ref); return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeRequestAdapter::ReleaseLeaseRef(Lease& lease) noexcept {
    if(lease.releaseEntered)return false;
    // This bit survives an exceptional/partial decrement. A destruction proof
    // must never turn an uncertain RefRelease into a second decrement.
    lease.releaseEntered=true;
    if(!ReleaseRetiring(lease.ref))return false;
    lease.refCertified=false;return true;
}
void NativeRequestAdapter::ConfirmActorDestroyed(const LocalActorBinding& actor) noexcept {
    if(busy_||!actor.character||!actor.runtimeId||!actor.exclusiveExecution||!actor.creationSerial)return;
    for(auto& lease:leases_)if(lease.used&&lease.actor==actor)lease.actorDestroyed=true;
}
void NativeRequestAdapter::DrainDestroyedLease(Lease& lease) noexcept {
    if(!lease.refCertified||lease.releaseEntered)return;
    std::uintptr_t type{};std::uint32_t status{};LONG strong{};
    const auto expected=lease.kind==1?0xEB1A70U:lease.kind==2?0xEB1A60U:lease.kind==3?0xEB1A50U:lease.kind==4?0xEB19CCU:
        lease.kind==5?0xEB198CU:lease.kind==7?0xEB19BCU:lease.kind==9?0xEB19ACU:
        scheduler_control::Supported(lease.kind)?scheduler_control::kTypeRva:0U;
    const auto destructor=lease.kind==1?0x282B10U:(lease.kind==2||lease.kind==3)?0x282B50U:0x27F830U;
    std::uintptr_t actualDestructor{};
    // Only the strongly retained request is read here. The Character and its
    // scheduler are gone. Native 00688410/00686C80 drained both scheduler lists;
    // 0067F830/006824C0/00682A70 destroy only request-owned storage/weak Refs.
    if(!expected||(!retirementRelease_&&(!Read(base_+expected,actualDestructor)||actualDestructor!=base_+destructor))||
        !Read(lease.ref.control+4,strong)||strong<=0||!Read(lease.ref.object,type)||type!=base_+expected||
        !Read(lease.ref.object+0xC,status)||status>6U||status==1U||status==2U) {
        lease.unknown=true;return;
    }
    lease.unknown=true;
    if(!ReleaseLeaseRef(lease))return;
    lease={};
}
void NativeRequestAdapter::DrainLease(Lease& lease, RetirementGuard guard, void* context) noexcept {
    if(lease.actorDestroyed){DrainDestroyedLease(lease);return;}
    if (lease.unknown || !guard) return;
    const RetirementIdentity identity{lease.scope,lease.owner,lease.command,lease.actor,lease.kind,lease.revision};
    auto permitted = guard(context,identity);
    if (permitted == Readiness::Waiting) return;
    if (permitted != Readiness::Ready || !RetirementLive(lease)) { lease.unknown = true; return; }
    const character_direct_step_runtime::MovementResourceLease resources(lease.actor.character);
    if (!resources) return;
    permitted = guard(context,identity);
    if (permitted == Readiness::Waiting) return;
    if (permitted != Readiness::Ready || !RetirementLive(lease)) { lease.unknown = true; return; }
    if (!lease.cancelled) {
        // Publish uncertainty before the callback-capable native operation.
        // A fault/lost lifetime cannot turn into an automatic second Cancel.
        lease.unknown = true;
        if (!CancelRetiring(lease.ref.object) || guard(context,identity) != Readiness::Ready || !RetirementLive(lease)) return;
        lease.cancelled = true; lease.unknown = false;
    }
    lease.unknown = true;
    if (!ReleaseLeaseRef(lease)) return; // A partial decrement is never repeated.
    // Do not dereference Character/request after RefRelease: its destructor may
    // already have invalidated them. The independent reference was consumed;
    // ownership/world fences remain the coordinator's responsibility.
    lease = {};
}
RetirementProgress NativeRequestAdapter::DrainRetirement(RetirementGuard guard, void* context, std::size_t budget) noexcept {
    if (busy_) { auto state = RetirementState(); state.reentrant = true; state.complete = false; return state; }
    if (!retiring_) return RetirementState();
    const BusyGuard callbackGuard(busy_);
    std::size_t visited = 0, released = 0;
    for (std::size_t scan = 0; scan < kCapacity && visited < std::min<std::size_t>(budget,4); ++scan) {
        auto& lease = leases_[retirementCursor_]; retirementCursor_ = (retirementCursor_ + 1) % kCapacity;
        if (!lease.used || lease.scope != retirementScope_) continue;
        ++visited; DrainLease(lease,guard,context);
        if (!lease.used) ++released;
    }
    auto state = RetirementState(); state.visited = visited; state.released = released;
    state.complete = state.started && state.remaining == 0; return state;
}
RetirementProgress NativeRequestAdapter::DrainActorRetirement(const LocalActorBinding& actor,
    RetirementGuard guard,void* context) noexcept {
    if(busy_){auto state=RetirementState();state.reentrant=true;state.complete=false;return state;}
    if(!retiring_||!actor.character||!actor.runtimeId||!actor.exclusiveExecution)return RetirementState();
    const BusyGuard callbackGuard(busy_);
    std::size_t visited{},released{};
    // Companions are retired first, matching ordinary authoritative Release.
    for(const bool secondary:{true,false})for(auto& lease:leases_) {
        if(!lease.used||lease.scope!=retirementScope_||lease.actor.character!=actor.character||
            lease.actor.runtimeId!=actor.runtimeId||lease.actor.creationSerial!=actor.creationSerial||lease.secondary!=secondary)continue;
        ++visited;DrainLease(lease,guard,context);if(!lease.used)++released;
    }
    auto state=RetirementState();state.visited=visited;state.released=released;
    state.complete=state.started&&state.remaining==0;return state;
}
} // namespace aimp::ai_sync::execution
