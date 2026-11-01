#include "aimp/AISyncOwnership.h"

namespace aimp::ai_sync::execution {
namespace {
struct Guard { bool& busy; explicit Guard(bool& value) noexcept : busy(value) { busy = true; } ~Guard() { busy = false; } };
bool SameActor(const transactions::OwnerKey& a, const transactions::OwnerKey& b) noexcept {
    return a.entityA == b.entityA && a.entityB == b.entityB && a.capability == b.capability;
}
bool ValidOwner(const transactions::OwnerKey& owner) noexcept {
    return (owner.entityA != 0 || owner.entityB != 0) && owner.birth != 0 && owner.actorClass != 0 &&
        owner.capability == transactions::Capability::Movement && owner.ownerEpoch != 0;
}
bool ValidScope(const transactions::Scope& scope) noexcept {
    bool session = false; for (auto byte : scope.session) session = session || byte != 0;
    return session && scope.authorityEpoch != 0 && scope.worldEpoch != 0 && scope.consumerEpoch != 0;
}
}
OwnershipCoordinator::Entry* OwnershipCoordinator::FindActor(const transactions::OwnerKey& owner) noexcept {
    for (std::size_t i = 0; i < count_; ++i) if (SameActor(entries_[i].token.owner, owner)) return &entries_[i];
    return nullptr;
}
const OwnershipCoordinator::Entry* OwnershipCoordinator::FindActor(const transactions::OwnerKey& owner) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) if (SameActor(entries_[i].token.owner, owner)) return &entries_[i];
    return nullptr;
}
OwnershipResult OwnershipCoordinator::CanConfigure(const transactions::Scope& scope, std::uint32_t generation) const noexcept {
    if (busy_) return OwnershipResult::Reentrant;
    if (!ValidScope(scope) || generation == 0) return OwnershipResult::Rejected;
    if (configured_ && scope == scope_ && generation == nativeGeneration_) return OwnershipResult::Ready;
    if (configured_ && scope == scope_) return OwnershipResult::WrongScope; // Reload needs a new consumer.
    if (configured_ && scope.session == scope_.session && (scope.authorityEpoch < scope_.authorityEpoch ||
        scope.worldEpoch < scope_.worldEpoch || scope.consumerEpoch <= scope_.consumerEpoch)) return OwnershipResult::WrongScope;
    for (std::size_t i = 0; i < count_; ++i)
        if (entries_[i].phase != OwnershipPhase::Released) return OwnershipResult::Busy;
    return OwnershipResult::Ready;
}
OwnershipResult OwnershipCoordinator::Configure(const transactions::Scope& scope, std::uint32_t generation) noexcept {
    const auto result = CanConfigure(scope, generation);
    if (result != OwnershipResult::Ready) return result;
    if (configured_ && scope == scope_) return result; // Preserve tombstones.
    scope_ = scope; nativeGeneration_ = generation; configured_ = true; count_ = 0;
    return result;
}
OwnershipResult OwnershipCoordinator::Finish(Entry& entry, OwnershipCheck check, OwnershipPhase ready) noexcept {
    if (entry.phase == OwnershipPhase::Fenced) return OwnershipResult::Unknown;
    if (check == OwnershipCheck::Ready) { entry.phase = ready; return OwnershipResult::Ready; }
    if (check == OwnershipCheck::Waiting) return OwnershipResult::Waiting;
    // Once a foreign drain/restore callback has run, even Reject cannot prove
    // that it left no partial effects. Never silently reopen the old writer.
    entry.phase = OwnershipPhase::Fenced;
    return OwnershipResult::Unknown;
}
OwnershipResult OwnershipCoordinator::Revalidate(Entry& entry) noexcept {
    if (entry.phase == OwnershipPhase::Fenced) return OwnershipResult::Unknown;
    ActorLifetime actor{};
    const auto check = adapter_.resolve(adapter_.context, scope_, entry.token.owner, actor);
    if (entry.phase == OwnershipPhase::Fenced) return OwnershipResult::Unknown;
    if (check == OwnershipCheck::Waiting) return OwnershipResult::Waiting;
    if (check != OwnershipCheck::Ready || actor != entry.token.actor) {
        entry.phase = OwnershipPhase::Fenced; return OwnershipResult::Unknown;
    }
    return OwnershipResult::Ready;
}
OwnershipResult OwnershipCoordinator::Drain(Entry& entry) noexcept {
    if (Revalidate(entry) != OwnershipResult::Ready)
        return entry.phase == OwnershipPhase::Fenced ? OwnershipResult::Unknown : OwnershipResult::Waiting;
    const auto check = adapter_.drain(adapter_.context, entry.token);
    const auto valid = Revalidate(entry); // Native callbacks may change incarnations.
    if (valid != OwnershipResult::Ready) {
        // A successful drain with temporarily unresolvable lifetime is not an
        // ownership grant. Re-draining cannot be assumed safe after this call.
        entry.phase = OwnershipPhase::Fenced; return OwnershipResult::Unknown;
    }
    return Finish(entry, check, OwnershipPhase::Owned);
}
OwnershipResult OwnershipCoordinator::Request(const transactions::OwnerKey& input) noexcept {
    if (busy_) return OwnershipResult::Reentrant;
    const Guard guard(busy_);
    const auto owner = input;
    if (!configured_) return OwnershipResult::WrongScope;
    if (!ValidOwner(owner) || !adapter_.resolve || !adapter_.drain || !adapter_.nativeReleased || !adapter_.restore)
        return OwnershipResult::Rejected;
    if (revocation_ == UINT64_MAX) return OwnershipResult::Unknown;
    const auto revocation = revocation_;
    auto* entry = FindActor(owner);
    if (entry && entry->token.owner == owner) {
        if (entry->phase == OwnershipPhase::Owned) return Revalidate(*entry);
        if (entry->phase == OwnershipPhase::Draining) return OwnershipResult::Waiting;
        return entry->phase == OwnershipPhase::Released ? OwnershipResult::StaleOwner : OwnershipResult::Busy;
    }
    if (entry) {
        if (entry->phase != OwnershipPhase::Released) return OwnershipResult::Busy;
        const auto& previous = entry->token.owner;
        if (owner.birth < previous.birth || owner.ownerEpoch <= previous.ownerEpoch ||
            (owner.birth == previous.birth && owner.actorClass != previous.actorClass)) return OwnershipResult::StaleOwner;
    }
    ActorLifetime actor{};
    const auto check = adapter_.resolve(adapter_.context, scope_, owner, actor);
    if (revocation != revocation_) return OwnershipResult::Unknown;
    if (check == OwnershipCheck::Waiting) return OwnershipResult::Waiting;
    if (check != OwnershipCheck::Ready || actor.character == 0 || actor.runtimeId == 0 || actor.nativeGeneration != nativeGeneration_)
        return OwnershipResult::Rejected;
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& other = entries_[i];
        if (&other != entry && other.phase != OwnershipPhase::Released && other.token.actor.character == actor.character &&
            other.token.actor.nativeGeneration == actor.nativeGeneration) return OwnershipResult::Busy;
    }
    if (adapter_.prepare) {
        const auto prepared = adapter_.prepare(adapter_.context, scope_, owner, actor);
        if (revocation != revocation_) return OwnershipResult::Unknown;
        if (prepared != OwnershipCheck::Ready) return prepared == OwnershipCheck::Waiting ? OwnershipResult::Waiting :
            prepared == OwnershipCheck::Unknown ? OwnershipResult::Unknown : OwnershipResult::Rejected;
        ActorLifetime after{};
        if (adapter_.resolve(adapter_.context, scope_, owner, after) != OwnershipCheck::Ready || after != actor || revocation != revocation_)
            return OwnershipResult::Unknown;
    }
    if (serial_ == UINT64_MAX || (!entry && count_ == kCapacity)) return OwnershipResult::Capacity;
    if (!entry) entry = &entries_[count_++];
    *entry = Entry{OwnershipToken{scope_, owner, actor, ++serial_}, OwnershipPhase::Draining};
    return Drain(*entry);
}
OwnershipResult OwnershipCoordinator::Retry(const transactions::OwnerKey& owner) noexcept {
    if (busy_) return OwnershipResult::Reentrant;
    const Guard guard(busy_);
    auto* entry = FindActor(owner);
    if (!entry || entry->token.owner != owner) return OwnershipResult::StaleOwner;
    if (entry->phase == OwnershipPhase::Owned) return Revalidate(*entry);
    if (entry->phase != OwnershipPhase::Draining) return OwnershipResult::Busy;
    return Drain(*entry);
}
OwnershipResult OwnershipCoordinator::Retire(const transactions::OwnerKey& owner) noexcept {
    if (busy_) return OwnershipResult::Reentrant;
    auto* entry = FindActor(owner);
    if (!entry || entry->token.owner != owner) return OwnershipResult::StaleOwner;
    if (entry->phase == OwnershipPhase::Fenced) return OwnershipResult::Unknown;
    if (entry->phase == OwnershipPhase::Released) return OwnershipResult::Ready;
    if (entry->phase != OwnershipPhase::Restoring) entry->phase = OwnershipPhase::Retiring;
    return OwnershipResult::Waiting;
}
void OwnershipCoordinator::RetireAll() noexcept {
    if (busy_) return;
    for (std::size_t i = 0; i < count_; ++i) static_cast<void>(Retire(entries_[i].token.owner));
}
bool OwnershipCoordinator::InvalidateActor(const ActorLifetime& actor) noexcept {
    // Invalidation may arrive during resolve/preflight before a token exists.
    // It must still prevent this in-flight request from granting that lifetime.
    if (revocation_ != UINT64_MAX) ++revocation_;
    bool changed = false;
    for (std::size_t i = 0; i < count_; ++i)
        if (entries_[i].phase != OwnershipPhase::Released && entries_[i].phase != OwnershipPhase::Fenced &&
            entries_[i].token.actor.character==actor.character&&entries_[i].token.actor.runtimeId==actor.runtimeId&&
            entries_[i].token.actor.nativeGeneration==actor.nativeGeneration&&
            (actor.creationSerial==0U||entries_[i].token.actor.creationSerial==actor.creationSerial))
            { entries_[i].phase = OwnershipPhase::Fenced; changed = true; }
    return changed;
}
OwnershipResult OwnershipCoordinator::Release(const transactions::OwnerKey& owner) noexcept {
    if (busy_) return OwnershipResult::Reentrant;
    const Guard guard(busy_);
    auto* entry = FindActor(owner);
    if (!entry || entry->token.owner != owner) return OwnershipResult::StaleOwner;
    if (entry->phase == OwnershipPhase::Released) return OwnershipResult::Ready;
    if(entry->destroyed) {
        // Revalidate/restore would dereference a dead Character and turn a
        // legitimate between-round teardown into a permanent Fenced owner.
        const auto released=adapter_.destroyedReleased?adapter_.destroyedReleased(adapter_.context,entry->token):OwnershipCheck::Unknown;
        if(released==OwnershipCheck::Ready){entry->phase=OwnershipPhase::Released;return OwnershipResult::Ready;}
        return released==OwnershipCheck::Waiting?OwnershipResult::Waiting:OwnershipResult::Unknown;
    }
    if (entry->phase == OwnershipPhase::Fenced) return OwnershipResult::Unknown;
    if (entry->phase != OwnershipPhase::Retiring && entry->phase != OwnershipPhase::Restoring) return OwnershipResult::Busy;
    const auto valid = Revalidate(*entry);
    if (valid != OwnershipResult::Ready) return valid;
    const auto native = adapter_.nativeReleased(adapter_.context, entry->token);
    if (native == OwnershipCheck::Waiting) return OwnershipResult::Waiting;
    if (native != OwnershipCheck::Ready) return Finish(*entry, native, OwnershipPhase::Released);
    if (Revalidate(*entry) != OwnershipResult::Ready) { entry->phase = OwnershipPhase::Fenced; return OwnershipResult::Unknown; }
    entry->phase = OwnershipPhase::Restoring;
    const auto restored = adapter_.restore(adapter_.context, entry->token);
    if (Revalidate(*entry) != OwnershipResult::Ready) { entry->phase = OwnershipPhase::Fenced; return OwnershipResult::Unknown; }
    return Finish(*entry, restored, OwnershipPhase::Released);
}
void OwnershipCoordinator::ConfirmDestroyed(const ActorLifetime& actor) noexcept {
    if(!actor.character||!actor.runtimeId||!actor.nativeGeneration||!actor.creationSerial)return;
    if(revocation_!=UINT64_MAX)++revocation_;
    for(std::size_t i=0;i<count_;++i)if(entries_[i].token.actor==actor&&entries_[i].phase!=OwnershipPhase::Released) {
        entries_[i].destroyed=true;entries_[i].phase=OwnershipPhase::Fenced;
    }
}
void OwnershipCoordinator::ForgetGeneration(const std::uint32_t generation) noexcept {
    if(busy_||generation==0)return;
    if (revocation_ != UINT64_MAX) ++revocation_;
    for(std::size_t i=0;i<count_;++i)if(entries_[i].phase!=OwnershipPhase::Released&&
        entries_[i].token.actor.nativeGeneration==generation)entries_[i].phase=OwnershipPhase::Released;
}
void OwnershipCoordinator::ReleaseDestroyed() noexcept {
    if(busy_)return;
    for(std::size_t i=0;i<count_;++i)if(entries_[i].destroyed&&entries_[i].phase!=OwnershipPhase::Released)
        static_cast<void>(Release(entries_[i].token.owner));
}
bool OwnershipCoordinator::ResolveOwned(const transactions::Scope& scope, const transactions::OwnerKey& owner,
    ActorLifetime& actor, bool cleanup) noexcept {
    return ResolveBinding(scope,owner,actor,cleanup) == OwnershipResult::Ready;
}
OwnershipResult OwnershipCoordinator::ResolveBinding(const transactions::Scope& scope, const transactions::OwnerKey& owner,
    ActorLifetime& actor, bool cleanup) noexcept {
    actor = {};
    if (busy_) return OwnershipResult::Reentrant;
    if (!configured_ || scope != scope_) return OwnershipResult::WrongScope;
    const Guard guard(busy_);
    auto* entry = FindActor(owner);
    if (!entry || entry->token.owner != owner) return OwnershipResult::StaleOwner;
    if (entry->phase == OwnershipPhase::Fenced) return OwnershipResult::Unknown;
    if (entry->phase != OwnershipPhase::Owned && !(cleanup && entry->phase == OwnershipPhase::Retiring)) return OwnershipResult::Busy;
    const auto result = Revalidate(*entry);
    if (result == OwnershipResult::Ready) actor = entry->token.actor;
    return result;
}
bool OwnershipCoordinator::AllowsLegacy(const ActorLifetime& actor) const noexcept {
    for (std::size_t i = 0; i < count_; ++i)
        if (entries_[i].phase != OwnershipPhase::Released && entries_[i].token.actor.character == actor.character &&
            entries_[i].token.actor.nativeGeneration == actor.nativeGeneration) return false;
    return true;
}
bool OwnershipCoordinator::Find(const transactions::OwnerKey& owner, OwnershipToken& token, OwnershipPhase& phase) const noexcept {
    const auto* entry = FindActor(owner);
    if (!entry || entry->token.owner != owner) return false;
    token = entry->token; phase = entry->phase; return true;
}
bool OwnershipCoordinator::Read(std::size_t index, OwnershipToken& token, OwnershipPhase& phase) const noexcept {
    if (index >= count_) return false;
    token = entries_[index].token; phase = entries_[index].phase; return true;
}
} // namespace aimp::ai_sync::execution
