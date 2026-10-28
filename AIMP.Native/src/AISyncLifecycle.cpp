#include "aimp/AISyncLifecycle.h"
#include <algorithm>
#if defined(_WIN32)
#include <Windows.h>
#endif

namespace aimp::ai_sync::lifecycle {
namespace {
using interactions::Check;
struct Busy final { bool& flag; explicit Busy(bool& value) noexcept : flag(value) { flag = true; } ~Busy() { flag = false; } };
bool Valid(const Command& command) noexcept {
    const auto& scope = command.key.scope;
    return command.key.command != 0U && command.revision != 0U && command.actor.birth != 0U &&
        command.actor.peer == 0U && command.actor.actorClass != 0U &&
        (command.actor.entityA != 0U || command.actor.entityB != 0U) &&
        scope.authorityEpoch != 0U && scope.worldEpoch != 0U && scope.consumerEpoch != 0U &&
        std::any_of(scope.session.begin(),scope.session.end(),[](auto value) { return value != 0U; });
}
}
bool NativeCharacterAdapter::Initialize(const std::uintptr_t base, const bool exactBuildValidated,
    const Permission permission, void* context) noexcept {
    if (base_ != 0U || base < 0x10000U || base > 0xFFFFFFFFULL - 0x012F0C88U ||
        !exactBuildValidated || permission == nullptr) return false;
    base_ = base; permission_ = permission; context_ = context; return true;
}
interactions::Check NativeCharacterAdapter::Check(const Command& command, const Binding& binding) noexcept {
    if (base_ == 0U || !binding.executionQuiescent || binding.actor.character == 0U || binding.actor.runtimeId == 0U)
        return interactions::Check::Rejected;
    const auto permission = permission_(context_,command,binding);
    if (permission != interactions::Check::Ready) return permission;
    if (checkPrimitive_ != nullptr) return checkPrimitive_(primitivesContext_,binding);
#if defined(_WIN32) && defined(_M_IX86)
    __try {
        return *reinterpret_cast<const std::uint32_t*>(binding.actor.character + 0x50U) == binding.actor.runtimeId
            ? interactions::Check::Ready : interactions::Check::Rejected;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return interactions::Check::Unknown; }
#else
    return interactions::Check::Rejected;
#endif
}
bool NativeCharacterAdapter::Enter(const Binding& binding, const bool backstage) noexcept {
    if (enterPrimitive_ != nullptr) return enterPrimitive_(primitivesContext_,binding,backstage);
#if defined(_WIN32) && defined(_M_IX86)
    __try {
        using Backstage = void(__thiscall*)(void*,std::uint8_t);
        reinterpret_cast<Backstage>(base_ + 0x0038B140U)(reinterpret_cast<void*>(binding.actor.character),backstage ? 1U : 0U);
        if (!backstage) {
            // The first call may synchronously destroy/rebind a Character.
            // Revalidate before the native animated-view/frontstage call.
            if (Check(command_,binding) != interactions::Check::Ready || receipt_.unknown) return false;
            const auto table = *reinterpret_cast<const std::uintptr_t*>(binding.actor.character);
            const auto getter = *reinterpret_cast<const std::uintptr_t*>(table + 0x1CU);
            if (getter < base_ + 0x1000U || getter >= base_ + 0x00D9F000U) return false;
            const auto animated = reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(reinterpret_cast<void*>(binding.actor.character));
            if (animated == 0U || Check(command_,binding) != interactions::Check::Ready || receipt_.unknown) return false;
            reinterpret_cast<void(__thiscall*)(void*)>(base_ + 0x003C0220U)(reinterpret_cast<void*>(animated));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
bool NativeCharacterAdapter::Inspect(const Binding& binding, bool& backstage) noexcept {
    if (inspectPrimitive_ != nullptr) return inspectPrimitive_(primitivesContext_,binding,backstage);
#if defined(_WIN32) && defined(_M_IX86)
    __try {
        backstage = *reinterpret_cast<const std::uint8_t*>(binding.actor.character + 0x2DAU) != 0U;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
Result NativeCharacterAdapter::Apply(const Command& command, const Binding& binding, Receipt& output) noexcept {
    output = {};
    if (busy_) return Result::Reentrant;
    if (command.operation != Operation::SetBackstage && command.operation != Operation::SetFrontstage) return Result::Unsupported;
    if (!Valid(command) || command.resourceA != 0U || command.resourceB != 0U) return Result::Rejected;
    if (used_) {
        output = receipt_;
        if (command.key == command_.key) return command == command_ && binding == binding_ ? Result::Duplicate : Result::Conflict;
        if (!receipt_.resourcesReleased) return Result::Waiting;
        if (command.key.scope == command_.key.scope && command.key.command <= command_.key.command) return Result::Rejected;
    }
    Busy guard(busy_);
    const auto revision = revocation_;
    const auto checked = Check(command,binding);
    if (revision != revocation_) return Result::Unknown;
    if (checked != interactions::Check::Ready) return checked == interactions::Check::Waiting ? Result::Waiting :
        checked == interactions::Check::Unknown ? Result::Unknown : Result::Rejected;
    command_ = command; binding_ = binding; used_ = true; receipt_ = {}; receipt_.key = command.key;
    receipt_.nativeEntered = true; // At-most-once includes partial native failure.
    const bool backstage = command.operation == Operation::SetBackstage;
    bool actual{};
    if (!Enter(binding,backstage) || revision != revocation_ ||
        Check(command,binding) != interactions::Check::Ready || !Inspect(binding,actual) || revision != revocation_) {
        receipt_.unknown = true; output = receipt_; return Result::Unknown;
    }
    receipt_.applied = actual == backstage;
    // Returning from the setter is an application receipt, not proof that all
    // native listeners/physics/callbacks have released their resources.
    if (!receipt_.applied) receipt_.unknown = true;
    output = receipt_;
    return output.applied ? Result::Applied : Result::Unknown;
}
Result NativeCharacterAdapter::Release(ReleaseProof proof, void* context, Receipt& output) noexcept {
    output = receipt_;
    if (busy_) return Result::Reentrant;
    if (!used_ || proof == nullptr) return Result::Rejected;
    if (receipt_.resourcesReleased) return Result::Duplicate;
    Busy guard(busy_);
    const auto revision = revocation_;
    const auto released = proof(context,command_,binding_,receipt_);
    if (revision != revocation_ || released == interactions::Check::Unknown) {
        receipt_.unknown = true; output = receipt_; return Result::Unknown;
    }
    if (released != interactions::Check::Ready) return released == interactions::Check::Waiting ? Result::Waiting : Result::Rejected;
    receipt_.resourcesReleased = true; output = receipt_; return Result::Released;
}
void NativeCharacterAdapter::Invalidate() noexcept { ++revocation_; if (used_ && !receipt_.resourcesReleased) receipt_.unknown = true; }
bool NativeCharacterAdapter::Find(Receipt& output) const noexcept { output = receipt_; return used_; }
} // namespace aimp::ai_sync::lifecycle
