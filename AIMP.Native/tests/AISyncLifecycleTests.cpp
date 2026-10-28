#include "aimp/AISyncLifecycle.h"
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace aimp::ai_sync::lifecycle {
struct NativeCharacterAdapterTestAccess {
    static void Prepare(NativeCharacterAdapter& adapter, void* context,
        interactions::Check(*check)(void*,const Binding&) noexcept,
        bool(*enter)(void*,const Binding&,bool) noexcept,
        bool(*inspect)(void*,const Binding&,bool&) noexcept) {
        adapter.primitivesContext_ = context; adapter.checkPrimitive_ = check;
        adapter.enterPrimitive_ = enter; adapter.inspectPrimitive_ = inspect;
    }
};
}
using namespace aimp::ai_sync;
using namespace lifecycle;
using Check = interactions::Check;
namespace {
void Require(bool value, int line) { if (!value) { std::cerr << "lifecycle invariant " << line << '\n'; std::abort(); } }
#define CHECK(value) Require((value),__LINE__)
struct Harness {
    NativeCharacterAdapter adapter;
    Command command{}; Binding binding{};
    Check permission{Check::Ready}, ready{Check::Ready}, release{Check::Ready};
    bool backstage{}, enterOk{true}, inspectOk{true}, revokeCheck{}, revokeEnter{}, revokeRelease{}, reenter{};
    unsigned entries{}, inspections{}, releases{};
    Harness() {
        command.key.scope.session[0] = 7; command.key.scope.authorityEpoch = 1;
        command.key.scope.worldEpoch = 2; command.key.scope.consumerEpoch = 3;
        command.key.command = 1; command.actor = {0,1,2,3,4}; command.revision = 1;
        binding.actor = {0x100000,17,1,2,interactions::Perspective::Npc}; binding.executionQuiescent = true;
        CHECK(adapter.Initialize(0x400000,true,&Permission,this));
        NativeCharacterAdapterTestAccess::Prepare(adapter,this,&LocalCheck,&Enter,&Inspect);
    }
    static Check Permission(void* context, const Command&, const Binding&) noexcept {
        auto& h = *static_cast<Harness*>(context);
        if (h.revokeCheck) h.adapter.Invalidate();
        if (h.reenter) { Receipt r{}; CHECK(h.adapter.Apply(h.command,h.binding,r) == Result::Reentrant); }
        return h.permission;
    }
    static Check LocalCheck(void* context, const Binding&) noexcept { return static_cast<Harness*>(context)->ready; }
    static bool Enter(void* context, const Binding&, bool backstage) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.entries; h.backstage = backstage;
        if (h.revokeEnter) h.adapter.Invalidate();
        return h.enterOk;
    }
    static bool Inspect(void* context, const Binding&, bool& backstage) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.inspections; backstage = h.backstage; return h.inspectOk;
    }
    static Check Release(void* context, const Command&, const Binding&, const Receipt&) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.releases;
        if (h.revokeRelease) h.adapter.Invalidate();
        return h.release;
    }
    Result Apply(Receipt& r) { return adapter.Apply(command,binding,r); }
};
void Lifecycle() {
    Harness h; Receipt r{};
    h.command.operation = Operation::SetBackstage;
    CHECK(h.Apply(r) == Result::Applied && r.nativeEntered && r.applied && !r.resourcesReleased && h.backstage);
    CHECK(h.Apply(r) == Result::Duplicate && h.entries == 1);
    ++h.command.revision; CHECK(h.Apply(r) == Result::Conflict && h.entries == 1); --h.command.revision;
    auto next = h.command; ++next.key.command; next.operation = Operation::SetFrontstage;
    CHECK(h.adapter.Apply(next,h.binding,r) == Result::Waiting);
    h.release = Check::Waiting; CHECK(h.adapter.Release(&Harness::Release,&h,r) == Result::Waiting && !r.resourcesReleased);
    h.release = Check::Ready; CHECK(h.adapter.Release(&Harness::Release,&h,r) == Result::Released && r.resourcesReleased);
    CHECK(h.adapter.Release(&Harness::Release,&h,r) == Result::Duplicate && h.releases == 2);
    CHECK(h.adapter.Apply(next,h.binding,r) == Result::Applied && !h.backstage && h.entries == 2);
    CHECK(h.adapter.Release(&Harness::Release,&h,r) == Result::Released);
    CHECK(h.Apply(r) == Result::Rejected && h.entries == 2);
}
void Refusals() {
    Harness h; Receipt r{};
    for (auto operation : {Operation::Spawn,Operation::Activate,Operation::Deactivate,Operation::Destroy,
        Operation::EnterConduit,Operation::ExitConduit,static_cast<Operation>(255)}) {
        h.command.operation = operation; CHECK(h.Apply(r) == Result::Unsupported && h.entries == 0 && !r.nativeEntered);
    }
    h.command.operation = Operation::SetFrontstage;
    h.permission = Check::Waiting; CHECK(h.Apply(r) == Result::Waiting && h.entries == 0);
    h.permission = Check::Ready; h.binding.executionQuiescent = false;
    CHECK(h.Apply(r) == Result::Rejected && h.entries == 0); h.binding.executionQuiescent = true;
    h.command.resourceA = 1; CHECK(h.Apply(r) == Result::Rejected); h.command.resourceA = 0;
    h.command.actor.birth = 0; CHECK(h.Apply(r) == Result::Rejected && h.entries == 0);
}
void Fences() {
    Receipt r{};
    { Harness h; h.revokeCheck = true; CHECK(h.Apply(r) == Result::Unknown && h.entries == 0); }
    { Harness h; h.revokeEnter = true; CHECK(h.Apply(r) == Result::Unknown && r.nativeEntered && r.unknown);
      CHECK(h.Apply(r) == Result::Duplicate && h.entries == 1); }
    { Harness h; h.enterOk = false; CHECK(h.Apply(r) == Result::Unknown);
      CHECK(h.Apply(r) == Result::Duplicate && h.entries == 1 && h.inspections == 0); }
    { Harness h; h.inspectOk = false; CHECK(h.Apply(r) == Result::Unknown && h.entries == 1); }
    { Harness h; h.reenter = true; CHECK(h.Apply(r) == Result::Applied && h.entries == 1);
      h.revokeRelease = true; CHECK(h.adapter.Release(&Harness::Release,&h,r) == Result::Unknown && !r.resourcesReleased); }
}
}
int main() {
    static_assert(!std::is_copy_constructible_v<NativeCharacterAdapter> && !std::is_move_constructible_v<NativeCharacterAdapter>);
    Lifecycle(); Refusals(); Fences();
    std::cout << "Native lifecycle: 3 groups PASS (injected primitives; no game execution)\n";
}
