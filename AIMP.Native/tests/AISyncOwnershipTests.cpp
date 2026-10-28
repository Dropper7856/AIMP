#include "aimp/AISyncOwnership.h"
#include <cstdlib>
#include <iostream>
#include <type_traits>

using namespace aimp::ai_sync;
using namespace execution;
using namespace transactions;
namespace {
void Check(bool value, int line) {
    if (!value) { std::cerr << "ownership invariant at line " << line << '\n'; std::abort(); }
}
#define CHECK(value) Check((value), __LINE__)
Scope ScopeValue() { Scope s{}; s.session[0] = 11; s.authorityEpoch = 1; s.worldEpoch = 2; s.consumerEpoch = 3; return s; }
OwnerKey Owner(std::uint32_t id = 1) { return {id, 2, 1, 3, Capability::Movement, 1}; }
struct Harness {
    OwnershipCoordinator* coordinator{};
    OwnershipCheck resolve{OwnershipCheck::Ready}, drain{OwnershipCheck::Ready};
    OwnershipCheck released{OwnershipCheck::Ready}, restore{OwnershipCheck::Ready};
    OwnershipCheck prepared{OwnershipCheck::Ready};
    unsigned prepares{};
    bool mutatePrepare{}, invalidatePrepare{};
    unsigned drains{}, checks{}, restores{};
    std::uint32_t runtimeOffset{}, generation{7};
    std::uint64_t creationSerial{1U};
    bool alias{}, reenter{}, mutateDrain{}, mutateRelease{}, mutateRestore{}, invalidateDrain{}, invalidateRestore{};
    OwnerKey* mutateInput{};
    ActorLifetime Actor(const OwnerKey& key) const noexcept {
        return {alias ? 0x1100U : 0x1000U + key.entityA * 0x100U, key.entityA + 10U + runtimeOffset, generation,creationSerial};
    }
    void Reentry(const OwnershipToken& token) noexcept {
        if (!reenter) return;
        auto& c = *coordinator; ActorLifetime actor{};
        CHECK(!c.AllowsLegacy(token.actor)); // Fence is visible BEFORE callbacks.
        CHECK(c.Request(token.owner) == OwnershipResult::Reentrant);
        CHECK(c.Retry(token.owner) == OwnershipResult::Reentrant);
        CHECK(c.Retire(token.owner) == OwnershipResult::Reentrant);
        CHECK(c.Release(token.owner) == OwnershipResult::Reentrant);
        CHECK(c.Configure(token.scope, 7) == OwnershipResult::Reentrant);
        CHECK(!c.ResolveOwned(token.scope, token.owner, actor));
        OwnershipToken before{}, after{}; OwnershipPhase first{}, second{};
        CHECK(c.Find(token.owner, before, first)); c.RetireAll();
        CHECK(c.Find(token.owner, after, second) && before.serial == after.serial && first == second);
    }
    static OwnershipCheck Resolve(void* context, const Scope&, const OwnerKey& owner, ActorLifetime& actor) noexcept {
        auto& h = *static_cast<Harness*>(context); actor = h.Actor(owner);
        if (h.mutateInput) ++h.mutateInput->entityA;
        return h.resolve;
    }
    static OwnershipCheck Drain(void* context, const OwnershipToken& token) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.drains; h.Reentry(token);
        if (h.invalidateDrain) CHECK(h.coordinator->InvalidateActor(token.actor));
        if (h.mutateDrain) ++h.runtimeOffset;
        return h.drain;
    }
    static OwnershipCheck Released(void* context, const OwnershipToken& token) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.checks; h.Reentry(token);
        if (h.mutateRelease) ++h.runtimeOffset;
        return h.released;
    }
    static OwnershipCheck Restore(void* context, const OwnershipToken& token) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.restores; h.Reentry(token);
        if (h.invalidateRestore) CHECK(h.coordinator->InvalidateActor(token.actor));
        if (h.mutateRestore) ++h.runtimeOffset;
        return h.restore;
    }
    static OwnershipCheck Prepare(void* context,const Scope& scope,const OwnerKey& owner,const ActorLifetime& actor) noexcept {
        auto& h=*static_cast<Harness*>(context);++h.prepares;
        CHECK(h.coordinator->AllowsLegacy(actor));
        CHECK(h.coordinator->Request(owner)==OwnershipResult::Reentrant);
        CHECK(h.coordinator->Configure(scope,7)==OwnershipResult::Reentrant);
        if(h.mutatePrepare)++h.runtimeOffset;
        if(h.invalidatePrepare)CHECK(!h.coordinator->InvalidateActor(actor)); // No token yet; still revoke the admission.
        return h.prepared;
    }
    OwnershipAdapter Api() noexcept { return {this, &Resolve, &Drain, &Released, &Restore}; }
};
void DestructionReturnAndRoundRestart() {
    Harness h;
    auto api=h.Api();api.destroyedReleased=[](void* context,const OwnershipToken&) noexcept {
        return static_cast<Harness*>(context)->released;
    };
    OwnershipCoordinator c(api);h.coordinator=&c;
    const auto scope=ScopeValue();const auto owner=Owner();
    CHECK(c.Configure(scope,7)==OwnershipResult::Ready&&c.Request(owner)==OwnershipResult::Ready);
    const auto actor=h.Actor(owner);CHECK(c.InvalidateActor(actor));
    auto wrong=actor;++wrong.creationSerial;c.ConfirmDestroyed(wrong);
    c.ReleaseDestroyed();OwnershipToken token{};OwnershipPhase phase{};
    CHECK(c.Find(owner,token,phase)&&phase==OwnershipPhase::Fenced);
    wrong=actor;wrong.creationSerial=0;c.ConfirmDestroyed(wrong);c.ReleaseDestroyed();
    CHECK(c.Find(owner,token,phase)&&phase==OwnershipPhase::Fenced);
    c.ConfirmDestroyed(actor);h.resolve=OwnershipCheck::Unknown;h.released=OwnershipCheck::Waiting;
    c.ReleaseDestroyed();CHECK(c.Find(owner,token,phase)&&phase==OwnershipPhase::Fenced&&h.restores==0);
    auto next=scope;++next.worldEpoch;++next.consumerEpoch;
    CHECK(c.Configure(next,8)==OwnershipResult::Busy);
    h.released=OwnershipCheck::Ready;c.ReleaseDestroyed();
    CHECK(c.Find(owner,token,phase)&&phase==OwnershipPhase::Released&&h.restores==0&&h.checks==0);
    CHECK(c.Configure(next,8)==OwnershipResult::Ready);
    h.resolve=OwnershipCheck::Ready;h.generation=8;++h.creationSerial;
    auto newOwner=owner;++newOwner.birth;++newOwner.ownerEpoch;
    CHECK(c.Request(newOwner)==OwnershipResult::Ready);
    c.ConfirmDestroyed(actor);c.ReleaseDestroyed(); // Delayed old receipt/address reuse.
    CHECK(c.Find(newOwner,token,phase)&&phase==OwnershipPhase::Owned);
}
void Lifecycle() {
    Harness h; OwnershipCoordinator c(h.Api()); h.coordinator = &c; h.reenter = true;
    auto scope = ScopeValue(); auto owner = Owner(); ActorLifetime local{};
    CHECK(c.Request(owner) == OwnershipResult::WrongScope);
    CHECK(c.Configure(scope, 7) == OwnershipResult::Ready);
    CHECK(c.AllowsLegacy(h.Actor(owner)) && !c.ResolveOwned(scope, owner, local));
    CHECK(c.Request(owner) == OwnershipResult::Ready && h.drains == 1);
    CHECK(!c.AllowsLegacy(h.Actor(owner)) && c.ResolveOwned(scope, owner, local) && local == h.Actor(owner));
    auto wrongScope = scope; wrongScope.session[15] = 1;
    CHECK(!c.ResolveOwned(wrongScope, owner, local) && local.character == 0);
    CHECK(c.Request(owner) == OwnershipResult::Ready && h.drains == 1); // Duplicate cannot drain twice.
    CHECK(c.Release(owner) == OwnershipResult::Busy && h.restores == 0);
    auto nextScope = scope; ++nextScope.consumerEpoch;
    CHECK(c.Configure(nextScope, 7) == OwnershipResult::Busy);
    CHECK(c.Configure(scope, 8) == OwnershipResult::WrongScope);
    CHECK(c.Retire(owner) == OwnershipResult::Waiting);
    CHECK(!c.ResolveOwned(scope, owner, local) && c.ResolveOwned(scope, owner, local, true));
    h.released = OwnershipCheck::Waiting;
    CHECK(c.Release(owner) == OwnershipResult::Waiting && h.restores == 0 && !c.AllowsLegacy(h.Actor(owner)));
    h.released = OwnershipCheck::Ready; h.restore = OwnershipCheck::Waiting;
    CHECK(c.Release(owner) == OwnershipResult::Waiting && h.restores == 1);
    CHECK(!c.ResolveOwned(scope, owner, local, true));
    h.restore = OwnershipCheck::Ready;
    CHECK(c.Release(owner) == OwnershipResult::Ready && c.AllowsLegacy(h.Actor(owner)));
    CHECK(c.Release(owner) == OwnershipResult::Ready && h.restores == 2);
    CHECK(c.Request(owner) == OwnershipResult::StaleOwner);
    OwnershipToken previous{}; OwnershipPhase phase{};
    CHECK(c.Find(owner, previous, phase) && phase == OwnershipPhase::Released);
    ++owner.ownerEpoch; CHECK(c.Request(owner) == OwnershipResult::Ready);
    OwnershipToken fresh{};
    CHECK(c.Find(owner, fresh, phase) && fresh.serial > previous.serial);
    CHECK(c.Retire(previous.owner) == OwnershipResult::StaleOwner && c.Release(previous.owner) == OwnershipResult::StaleOwner);
    CHECK(!c.ResolveOwned(scope, previous.owner, local));
    c.RetireAll(); CHECK(c.Release(owner) == OwnershipResult::Ready);
    CHECK(c.Configure(nextScope, 7) == OwnershipResult::Ready && c.Count() == 0);
    CHECK(c.Configure(scope, 7) == OwnershipResult::WrongScope);
    CHECK(!c.ResolveOwned(scope, owner, local));
}
void WaitingAndIndependentActors() {
    Harness h; OwnershipCoordinator c(h.Api()); h.coordinator = &c;
    auto owner = Owner(); CHECK(c.Configure(ScopeValue(), 7) == OwnershipResult::Ready);
    h.resolve = OwnershipCheck::Waiting;
    CHECK(c.Request(owner) == OwnershipResult::Waiting && c.Count() == 0 && h.drains == 0);
    CHECK(c.AllowsLegacy(h.Actor(owner)));
    h.resolve = OwnershipCheck::Ready; h.drain = OwnershipCheck::Waiting;
    CHECK(c.Request(owner) == OwnershipResult::Waiting && c.Count() == 1 && h.drains == 1);
    CHECK(!c.AllowsLegacy(h.Actor(owner)));
    CHECK(c.Request(owner) == OwnershipResult::Waiting && h.drains == 1);
    h.drain = OwnershipCheck::Ready;
    const auto other = Owner(2); CHECK(c.Request(other) == OwnershipResult::Ready);
    CHECK(c.Retry(owner) == OwnershipResult::Ready && h.drains == 3);
    CHECK(c.Retire(owner) == OwnershipResult::Waiting);
    h.resolve = OwnershipCheck::Waiting;
    ActorLifetime waitingActor{};
    CHECK(c.ResolveBinding(ScopeValue(),owner,waitingActor,true) == OwnershipResult::Waiting && waitingActor.character == 0);
    h.resolve = OwnershipCheck::Ready;
    h.released = OwnershipCheck::Waiting;
    CHECK(c.Release(owner) == OwnershipResult::Waiting);
    ActorLifetime local{}; CHECK(c.ResolveOwned(ScopeValue(), other, local));
    CHECK(c.Retire(other) == OwnershipResult::Waiting);
    h.released = OwnershipCheck::Ready; CHECK(c.Release(other) == OwnershipResult::Ready);
    CHECK(!c.AllowsLegacy(h.Actor(owner)) && c.AllowsLegacy(h.Actor(other)));
    CHECK(c.Release(owner) == OwnershipResult::Ready);
    // Aborted draining still needs explicit native/legacy cleanup.
    ++owner.ownerEpoch; h.drain = OwnershipCheck::Waiting;
    CHECK(c.Request(owner) == OwnershipResult::Waiting);
    CHECK(c.Retire(owner) == OwnershipResult::Waiting && !c.AllowsLegacy(h.Actor(owner)));
    CHECK(c.Release(owner) == OwnershipResult::Ready);
}
void FaultsAndLifetimes() {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        Harness h; OwnershipCoordinator c(h.Api()); h.coordinator = &c;
        const auto owner = Owner(); CHECK(c.Configure(ScopeValue(), 7) == OwnershipResult::Ready);
        if (scenario == 0) h.drain = OwnershipCheck::Unknown;
        if (scenario == 1) h.drain = OwnershipCheck::Reject;
        if (scenario == 2) h.drain = static_cast<OwnershipCheck>(255);
        if (scenario == 3) h.mutateDrain = true;
        if (scenario < 4) CHECK(c.Request(owner) == OwnershipResult::Unknown);
        else {
            CHECK(c.Request(owner) == OwnershipResult::Ready);
            if (scenario == 4) ++h.runtimeOffset;
            if (scenario == 5) h.mutateRelease = true;
            if (scenario == 6) h.mutateRestore = true;
            if (scenario == 7) h.released = OwnershipCheck::Unknown;
            if (scenario == 4) { ActorLifetime local{}; CHECK(!c.ResolveOwned(ScopeValue(), owner, local)); }
            else { CHECK(c.Retire(owner) == OwnershipResult::Waiting); CHECK(c.Release(owner) == OwnershipResult::Unknown); }
        }
        OwnershipToken token{}; OwnershipPhase phase{};
        CHECK(c.Find(owner, token, phase) && phase == OwnershipPhase::Fenced);
        CHECK(!c.AllowsLegacy(token.actor));
        auto recycled = token.actor; ++recycled.runtimeId;
        CHECK(!c.AllowsLegacy(recycled)); // Pointer reuse cannot un-fence the writer.
        CHECK(c.Retry(owner) == OwnershipResult::Busy && c.Release(owner) == OwnershipResult::Unknown);
        auto fresh = ScopeValue(); ++fresh.consumerEpoch;
        CHECK(c.Configure(fresh, 7) == OwnershipResult::Busy);
        CHECK(c.Request(owner) == OwnershipResult::Busy);
        if (scenario == 5 || scenario == 7) CHECK(h.restores == 0);
    }
}
void ValidationAndCapacity() {
    OwnershipCoordinator empty;
    CHECK(empty.Configure(ScopeValue(), 7) == OwnershipResult::Ready && empty.Request(Owner()) == OwnershipResult::Rejected);
    Harness h; OwnershipCoordinator c(h.Api()); h.coordinator = &c;
    auto scope = ScopeValue(); auto invalid = scope; invalid.session = {};
    CHECK(c.Configure(invalid, 7) == OwnershipResult::Rejected && c.Configure(scope, 0) == OwnershipResult::Rejected);
    CHECK(c.Configure(scope, 7) == OwnershipResult::Ready);
    auto owner = Owner(); owner.ownerEpoch = 0;
    CHECK(c.Request(owner) == OwnershipResult::Rejected && h.drains == 0);
    h.generation = 8;
    CHECK(c.Request(Owner()) == OwnershipResult::Rejected); h.generation = 7;
    CHECK(c.Request(Owner()) == OwnershipResult::Ready);
    h.alias = true; CHECK(c.Request(Owner(2)) == OwnershipResult::Busy); h.alias = false;
    for (std::uint32_t id = 2; id <= OwnershipCoordinator::kCapacity; ++id)
        CHECK(c.Request(Owner(id)) == OwnershipResult::Ready);
    CHECK(c.Count() == OwnershipCoordinator::kCapacity && c.Request(Owner(129)) == OwnershipResult::Capacity);
    const auto tokenDrains = h.drains;
    CHECK(c.Request(Owner()) == OwnershipResult::Ready && h.drains == tokenDrains);
    c.RetireAll();
    for (std::uint32_t id = 1; id <= OwnershipCoordinator::kCapacity; ++id) CHECK(c.Release(Owner(id)) == OwnershipResult::Ready);
    CHECK(c.Request(Owner(129)) == OwnershipResult::Capacity); // Tombstones are not evicted.
    ++scope.consumerEpoch; CHECK(c.Configure(scope, 7) == OwnershipResult::Ready);
    CHECK(c.Request(Owner(129)) == OwnershipResult::Ready);
}
void DestructionCallbacks() {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        Harness h; OwnershipCoordinator c(h.Api()); h.coordinator = &c;
        const auto owner = Owner(); CHECK(c.Configure(ScopeValue(),7) == OwnershipResult::Ready);
        if (scenario == 0) {
            h.invalidateDrain = true; CHECK(c.Request(owner) == OwnershipResult::Unknown);
        } else {
            CHECK(c.Request(owner) == OwnershipResult::Ready);
            if (scenario == 1) {
                auto stale = h.Actor(owner); ++stale.runtimeId;
                CHECK(!c.InvalidateActor(stale));
                ActorLifetime actor{}; CHECK(c.ResolveOwned(ScopeValue(),owner,actor));
                CHECK(c.InvalidateActor(h.Actor(owner)));
            } else {
                CHECK(c.Retire(owner) == OwnershipResult::Waiting);
                h.invalidateRestore = true; CHECK(c.Release(owner) == OwnershipResult::Unknown);
            }
        }
        OwnershipToken token{}; OwnershipPhase phase{};
        CHECK(c.Find(owner,token,phase) && phase == OwnershipPhase::Fenced);
        CHECK(!c.AllowsLegacy(token.actor));
        CHECK(c.Release(owner) == OwnershipResult::Unknown && c.Request(owner) == OwnershipResult::Busy);
    }
}
void BorrowedInput() {
    Harness h; OwnershipCoordinator c(h.Api()); h.coordinator = &c;
    CHECK(c.Configure(ScopeValue(),7) == OwnershipResult::Ready);
    auto input = Owner(); const auto original = input; h.mutateInput = &input;
    CHECK(c.Request(input) == OwnershipResult::Ready && input.entityA != original.entityA);
    OwnershipToken token{}; OwnershipPhase phase{};
    CHECK(c.Find(original,token,phase) && phase == OwnershipPhase::Owned && token.owner == original);
    CHECK(!c.Find(input,token,phase));
}
void Generated() {
    for (std::uint32_t trace = 0; trace < 2000; ++trace) {
        Harness h; OwnershipCoordinator c(h.Api()); h.coordinator = &c;
        auto scope = ScopeValue(); CHECK(c.Configure(scope, 7) == OwnershipResult::Ready);
        auto owner = Owner(trace % 16 + 1);
        for (std::uint64_t epoch = 1; epoch <= 8; ++epoch) {
            owner.ownerEpoch = epoch; h.drain = OwnershipCheck::Waiting;
            CHECK(c.Request(owner) == OwnershipResult::Waiting && !c.AllowsLegacy(h.Actor(owner)));
            h.drain = OwnershipCheck::Ready; CHECK(c.Retry(owner) == OwnershipResult::Ready);
            CHECK(c.Retire(owner) == OwnershipResult::Waiting);
            h.released = OwnershipCheck::Waiting; CHECK(c.Release(owner) == OwnershipResult::Waiting);
            h.released = OwnershipCheck::Ready; CHECK(c.Release(owner) == OwnershipResult::Ready);
            CHECK(c.AllowsLegacy(h.Actor(owner)) && c.Request(owner) == OwnershipResult::StaleOwner);
        }
    }
}
void PreflightPreservesExistingExecutor() {
    Harness h;auto api=h.Api();api.prepare=&Harness::Prepare;
    OwnershipCoordinator c(api);h.coordinator=&c;
    auto owner=Owner();CHECK(c.Configure(ScopeValue(),7)==OwnershipResult::Ready);
    for(const auto result:{OwnershipCheck::Waiting,OwnershipCheck::Reject,OwnershipCheck::Unknown}) {
        h.prepared=result;
        const auto expected=result==OwnershipCheck::Waiting?OwnershipResult::Waiting:
            result==OwnershipCheck::Reject?OwnershipResult::Rejected:OwnershipResult::Unknown;
        CHECK(c.Request(owner)==expected&&c.Count()==0&&h.drains==0);
        CHECK(c.AllowsLegacy(h.Actor(owner)));
    }
    h.prepared=OwnershipCheck::Ready;h.mutatePrepare=true;
    CHECK(c.Request(owner)==OwnershipResult::Unknown&&c.Count()==0&&h.drains==0);
    CHECK(c.AllowsLegacy(h.Actor(owner)));
    h.mutatePrepare=false;h.invalidatePrepare=true;
    CHECK(c.Request(owner)==OwnershipResult::Unknown&&c.Count()==0&&h.drains==0);
    h.invalidatePrepare=false;h.drain=OwnershipCheck::Waiting;
    CHECK(c.Request(owner)==OwnershipResult::Waiting&&c.Count()==1&&h.drains==1);
    CHECK(!c.AllowsLegacy(h.Actor(owner))&&h.prepares==6U);
    h.drain=OwnershipCheck::Ready;
    CHECK(c.Retry(owner)==OwnershipResult::Ready&&h.prepares==6U&&h.drains==2U);
    CHECK(c.Request(owner)==OwnershipResult::Ready&&h.prepares==6U&&h.drains==2U);
}
void ConstructionReuse() {
    Harness h;OwnershipCoordinator c(h.Api());h.coordinator=&c;const auto owner=Owner();
    CHECK(c.Configure(ScopeValue(),7)==OwnershipResult::Ready&&c.Request(owner)==OwnershipResult::Ready);
    OwnershipToken original{};OwnershipPhase phase{};CHECK(c.Find(owner,original,phase));
    ++h.creationSerial;ActorLifetime current{};
    CHECK(!c.ResolveOwned(ScopeValue(),owner,current));
    CHECK(c.Find(owner,original,phase)&&phase==OwnershipPhase::Fenced);
    CHECK(!c.AllowsLegacy(h.Actor(owner))); // Reused address cannot bypass the old fence.
    auto replacement=owner;++replacement.birth;++replacement.ownerEpoch;
    CHECK(c.Request(replacement)==OwnershipResult::Busy);
    Harness other;OwnershipCoordinator d(other.Api());other.coordinator=&d;
    CHECK(d.Configure(ScopeValue(),7)==OwnershipResult::Ready&&d.Request(owner)==OwnershipResult::Ready);
    auto invalid=other.Actor(owner);++invalid.creationSerial;
    CHECK(!d.InvalidateActor(invalid));CHECK(d.Request(owner)==OwnershipResult::Ready);
    invalid.creationSerial=0U;CHECK(d.InvalidateActor(invalid));
    CHECK(d.Request(owner)==OwnershipResult::Busy);
    CHECK(!d.ResolveOwned(ScopeValue(),owner,current));
}
}
int main() {
    static_assert(!std::is_copy_constructible_v<OwnershipCoordinator>);
    Lifecycle(); WaitingAndIndependentActors(); FaultsAndLifetimes(); ValidationAndCapacity(); DestructionCallbacks(); BorrowedInput(); Generated();
    PreflightPreservesExistingExecutor();ConstructionReuse();DestructionReturnAndRoundRestart();
    std::cout << "AISyncOwnership: 9 groups and 2000 generated handoff traces PASS (offline only)\n";
}
