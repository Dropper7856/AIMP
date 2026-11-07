#include "aimp/AISyncExecutionCodec.h"
#include "aimp/AISyncExecutionSession.h"
#include "aimp/AISyncExecutionRuntime.h"
#include <Windows.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace aimp::ai_sync::execution {
struct ExecutionSessionTestAccess {
    static bool Guidance(ExecutionSession& session,const transactions::Record& r) {
        return session.GuidanceCurrent(&session,{r.scope,r.owner,r.commandId,{},r.nativeKind,r.parameterRevision});
    }
    static void Adapter(ExecutionSession& session,transactions::TransactionAdapter adapter) {
        session.executor_.~TransactionExecutor();
        ::new (&session.executor_) transactions::TransactionExecutor(adapter,{},true);
    }
};
// Synthetic state only: exercise dependency decisions without invoking retail code.
struct NativeRequestAdapterTestAccess {
    static void Calls(NativeRequestAdapter& adapter,void* context,
        bool(*factory)(void*,std::uint32_t,std::uintptr_t,std::uintptr_t*)noexcept,
        bool(*admit)(void*,std::uintptr_t,std::uint32_t,bool&)noexcept,
        bool(*cancel)(void*,std::uintptr_t)noexcept,bool(*release)(void*,std::uintptr_t)noexcept) {
        adapter.movementCallsContext_=adapter.retirementCallsContext_=context;
        adapter.movementFactory_=factory;adapter.movementAdmission_=admit;
        adapter.retirementCancel_=cancel;adapter.retirementRelease_=release;
    }
    static void Seed(NativeRequestAdapter& adapter, const transactions::Record& r,
        LocalActorBinding actor, std::uintptr_t request, std::uintptr_t control) {
        auto& lease = adapter.leases_[0];
        lease = {}; lease.used = lease.admitted = true; lease.scope = r.scope;
        lease.owner = r.owner; lease.command = r.commandId; lease.actor = actor;
        lease.kind = r.nativeKind; lease.revision = r.parameterRevision;
        lease.ref = {request, control};
    }
};
}

namespace {
using namespace aimp::ai_sync;
using namespace execution;
using namespace transactions;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Scope TestScope() {
    Scope scope{}; scope.session[0] = 1;
    scope.authorityEpoch = 2; scope.worldEpoch = 3; scope.consumerEpoch = 4; return scope;
}
Record Command(Op op = Op::Begin) {
    Record r{}; r.scope = TestScope(); r.owner = {1, 2, 1, 1, Capability::Movement, 1};
    r.commandId = 1; r.globalSeq = r.ownerSeq = 1; r.opcode = op; r.parameterRevision = 1;
    r.nativeKind = 4; r.flags = HasGoal; r.mode = 3;
    r.goalX = 1.25F; r.goalY = -2.5F; r.goalZ = 3.75F; r.distance = 0.5F; return r;
}
void CodecGuards() {
    auto r = Command();
    Require(ValidateExecutable(r), "valid turn rejected");
    for (auto flag : {Incomplete, ObservationOnly, Accepted, HasGait, HasStopDistance}) {
        auto invalid = r; invalid.flags |= flag;
        Require(!ValidateExecutable(invalid), "observation or unsupported flag accepted");
    }
    for (auto kind : {0U,1U,2U,9U,UINT32_MAX}) {
        auto invalid = r; invalid.nativeKind = kind;
        Require(!ValidateExecutable(invalid), "uncertified kind accepted");
    }
    auto negativeZero = r; negativeZero.aux = -0.0F;
    Require(!ValidateExecutable(negativeZero), "unused negative zero accepted");
    auto heading = r; heading.flags = 0; heading.goalX = heading.goalY = heading.goalZ = 0; heading.aux = 6.5F;
    Require(!ValidateExecutable(heading), "heading outside native domain accepted");
    Batch batch{}; batch.scope = r.scope; batch.batchId = 17; batch.records[0] = r; batch.count = 1;
    std::vector<std::uint8_t> bytes;
    Require(EncodeBatch(batch, bytes), "batch encode failed");
    Batch decoded{};
    Require(DecodeBatch(bytes, r.scope, decoded) && decoded.batchId == 17 && decoded.count == 1, "batch decode failed");
    for (std::size_t i = 0; i < bytes.size(); ++i)
        Require(!DecodeBatch(std::span(bytes).first(i), r.scope, decoded), "truncated batch accepted");
    auto old = r.scope; ++old.consumerEpoch;
    Require(!DecodeBatch(bytes, old, decoded), "old consumer accepted");
    bytes[68] = 1; Require(!DecodeBatch(bytes, r.scope, decoded), "reserved byte accepted");
    batch.count = kMaxRecords;
    for (auto& record : batch.records) record = r;
    Require(EncodeBatch(batch, bytes) && DecodeBatch(bytes, r.scope, decoded) && decoded.count == kMaxRecords,
        "maximum bounded batch failed");
    batch.records.back().scope = old;
    Require(!EncodeBatch(batch, bytes), "late record scope mismatch accepted");
    Receipt receipt{r.scope, 17, r, Stage::Waiting, Result::Ok};
    std::array<std::uint8_t,kReceiptSize> ack{};
    Require(EncodeReceipt(receipt, ack), "receipt encode failed");
    Receipt copy{};
    Require(DecodeReceipt(ack, r.scope, copy) && copy.operation.commandId == r.commandId && copy.stage == Stage::Waiting,
        "receipt operation echo failed");
    for (std::size_t i = 0; i < ack.size(); ++i)
        Require(!DecodeReceipt(std::span(ack).first(i), r.scope, copy), "truncated receipt accepted");
    ack[224] = 99; Require(!DecodeReceipt(ack, r.scope, copy), "unknown receipt stage accepted");
    for (auto result : {0x100U, 0x110U, UINT32_MAX}) {
        Require(EncodeReceipt(receipt, ack), "receipt reset failed");
        std::memcpy(ack.data() + 228, &result, sizeof(result));
        Require(!DecodeReceipt(ack, r.scope, copy), "high result bits truncated before validation");
    }
}
void PlacementCodec() {
    Require(!PlacementDedupReady(),"placement integration gate enabled by default");
    Require(ProfileForKind(kPlacementKind)==kProfilePlacement&&kProfilePlacement==512U,"placement profile missing");
    for(const auto op:{Op::Begin,Op::Submit,Op::Update})for(const auto branch:{0U,1U})
        for(const auto admission:{0U,1U,2U})for(const auto retention:{0U,1U,2U,3U}) {
        auto r=Command(op);r.nativeKind=kPlacementKind;r.mode=branch;r.admission=admission;
        r.scalarRetention=retention;r.distance=0;r.aux=-1.25F;
        Batch batch{};batch.scope=r.scope;batch.batchId=1;batch.count=1;batch.records[0]=r;
        std::vector<std::uint8_t> bytes;Batch decoded{};
        Require(ValidateExecutable(r)&&EncodeBatch(batch,bytes)&&DecodeBatch(bytes,r.scope,decoded),"placement scalar roundtrip failed");
        const auto& copy=decoded.records[0];
        Require(copy.nativeKind==8&&copy.mode==branch&&copy.admission==admission&&copy.scalarRetention==retention&&
            copy.goalX==r.goalX&&copy.goalY==r.goalY&&copy.goalZ==r.goalZ&&copy.aux==r.aux,"placement payload changed");
        const auto stride=admission==0&&retention==3?transactions::kRecordSize:kExtendedRecordSize;
        Require(bytes.size()==kBatchHeaderSize+stride,"placement invented a new wire extension");
        Receipt receipt{r.scope,1,r,Stage::Waiting,Result::Ok},echo{};
        Require(EncodeReceipt(receipt,bytes)&&DecodeReceipt(bytes,r.scope,echo)&&echo.operation.mode==branch&&
            echo.operation.admission==admission&&echo.operation.scalarRetention==retention,"placement receipt lost exact operation");
        for(auto invalid:{0U,1U,2U,3U,4U,5U,6U,7U,8U,9U,10U}) {
            auto bad=r;
            switch(invalid) {
            case 0:bad.flags=0;break;case 1:bad.mode=2;break;case 2:bad.mode=255;break;
            case 3:bad.distance=-0.0F;break;case 4:bad.stopDistance=-0.0F;break;case 5:bad.gait=1;break;
            case 6:bad.goalX=100001.0F;break;case 7:bad.aux=6.5F;break;case 8:bad.distance=1;break;
            case 9:bad.goalY=std::bit_cast<float>(0x7F800000U);break;case 10:bad.aux=std::bit_cast<float>(0x7FC00001U);break;
            }
            Require(!ValidateExecutable(bad),"invalid placement payload accepted");
        }
    }
    for(const auto op:{Op::Cancel,Op::Stop,Op::Complete,Op::Release}) {
        auto r=Command(op);r.nativeKind=kPlacementKind;r.mode=r.flags=0;r.goalX=r.goalY=r.goalZ=r.distance=0;
        Require(ValidateExecutable(r),"placement terminal lifecycle rejected");
        r.scalarRetention=0;Require(!ValidateExecutable(r),"placement terminal accepted noncanonical retention");
    }
}
std::vector<std::uint8_t> HexFile(const char* path) {
    std::ifstream input(path);
    Require(static_cast<bool>(input), "shared execution fixture missing");
    std::string hex{std::istreambuf_iterator<char>{input}, {}};
    std::vector<std::uint8_t> bytes; int first = -1;
    for (unsigned char c : hex) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        const int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
            c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        Require(value >= 0, "invalid fixture hex");
        if (first < 0) first = value;
        else { bytes.push_back(static_cast<std::uint8_t>(first * 16 + value)); first = -1; }
    }
    Require(first < 0, "odd fixture hex"); return bytes;
}
void Fixture(const char* path) {
    const auto bytes = HexFile(path);
    Require(bytes.size() == 232, "execution fixture size mismatch");
    Scope expected{};
    std::copy(bytes.begin() + 16, bytes.begin() + 32, expected.session.begin());
    std::memcpy(&expected.authorityEpoch, bytes.data() + 32, 8);
    std::memcpy(&expected.worldEpoch, bytes.data() + 40, 8);
    std::memcpy(&expected.consumerEpoch, bytes.data() + 48, 8);
    Batch batch{};
    Require(DecodeBatch(bytes, expected, batch), "C# fixture decode failed");
    Require(batch.records[0].mode == 3 && batch.records[0].goalY == -2.5F && batch.records[0].distance == 0.5F,
        "C# fixture payload mismatch");
    std::vector<std::uint8_t> encoded;
    Require(EncodeBatch(batch, encoded) && encoded == bytes, "C++/C# execution fixture not byte-identical");
}
// Private non-executable data only. No AI.exe or retail function is run.
class FakeImage final {
public:
    FakeImage() {
        memory_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x00EC0000, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE));
        Require(memory_ != nullptr, "synthetic image allocation failed");
        const std::array<unsigned char,7> prefix{0x51,0x56,0x8B,0x74,0x24,0x0C,0x56};
        const std::array<unsigned char,3> ret{0xC2,4,0};
        for (auto rva : {0x00282450U,0x002823F0U,0x00282410U,0x002823B0U,0x002823D0U,0x00282390U,0x00282430U,0x00283530U}) {
            std::memcpy(memory_ + rva, prefix.data(), prefix.size());
            std::memcpy(memory_ + rva + 0x18, ret.data(), ret.size());
        }
        for (auto rva : {0x00EB19CCU,0x00EB19BCU}) {
            const auto callback = Base() + 0x002777E0;
            std::memcpy(memory_ + rva + 8, &callback, sizeof(callback));
        }
        const auto markerEligibility=Base()+0x277800U,markerStatus=Base()+0x282560U;
        std::memcpy(memory_+0xEB1A64,&markerEligibility,sizeof(markerEligibility));
        std::memcpy(memory_+0xEB1A68,&markerStatus,sizeof(markerStatus));
        const unsigned char markerEligible[]{0x33,0xC0,0x83,0xB9,0xB0,0,0,0,0xFF,0x0F,0x95,0xC0,0xC3};
        std::memcpy(memory_+0x277800,markerEligible,sizeof(markerEligible));
        const unsigned char markerInitialize[]{0xE8,0x62,0xF2,0xFF,0xFF};
        std::memcpy(memory_+0x2825A9,markerInitialize,sizeof(markerInitialize));
        // Kind5 (action) overrides the status method with 006799F0.
        const auto actionStatus = Base() + 0x002799F0;
        std::memcpy(memory_ + 0x00EB198C + 8, &actionStatus, sizeof(actionStatus));
        // Kind9 (directed) status method 006815A0.
        const auto directedStatus = Base() + 0x002815A0;
        std::memcpy(memory_ + 0x00EB19AC + 8, &directedStatus, sizeof(directedStatus));
        const auto controlStatus=Base()+scheduler_control::kStatusRva,eligibility=Base()+0xAA70F0U;
        std::memcpy(memory_+scheduler_control::kTypeRva+4,&eligibility,sizeof(eligibility));
        std::memcpy(memory_+scheduler_control::kTypeRva+8,&controlStatus,sizeof(controlStatus));
        const unsigned char statusRet[]{0xC2,0x0C,0};
        std::memcpy(memory_+scheduler_control::kStatusRva,statusRet,sizeof(statusRet));
        const auto tick=Base()+0x74D3F0U;
        std::memcpy(memory_+scheduler_control::kTypeRva+12,&tick,sizeof(tick));
        const unsigned char eligibleRet[]{0xB0,0x01,0xC3};
        std::memcpy(memory_+0xAA70F0U,eligibleRet,sizeof(eligibleRet));
        std::memcpy(memory_+0x74D3F0U,ret.data(),ret.size());
        const std::array<unsigned char,4> release{0x56,0x8B,0x71,4};
        std::memcpy(memory_ + 0xD770, release.data(), release.size());
    }
    ~FakeImage() { if (memory_ != nullptr) VirtualFree(memory_, 0, MEM_RELEASE); }
    std::uintptr_t Base() const { return reinterpret_cast<std::uintptr_t>(memory_); }
private:
    std::uint8_t* memory_{};
};
struct Context { ExecutionSession* session{}; bool allowed{true}; std::size_t resolveCalls{}, permissionCalls{}; bool reenter{}; };
struct SyntheticActor {
    std::array<std::uint8_t, 0x4300> storage{};
    LocalActorBinding binding{reinterpret_cast<std::uintptr_t>(storage.data()), 19, true};
    SyntheticActor() {
        std::memcpy(storage.data() + 0x50, &binding.runtimeId, sizeof(binding.runtimeId));
        std::memcpy(storage.data() + 0x2CD0, &binding.character, sizeof(binding.character));
    }
    static bool Permit(void*, const Record&) noexcept { return true; }
    static bool Resolve(void* context, const OwnerKey&, LocalActorBinding& binding) noexcept {
        binding = static_cast<SyntheticActor*>(context)->binding; return true;
    }
};
void NativeReadiness() {
    static_assert(!std::is_copy_constructible_v<NativeRequestAdapter>);
    static_assert(!std::is_move_constructible_v<NativeRequestAdapter>);
    static_assert(!std::is_copy_assignable_v<NativeRequestAdapter>);
    static_assert(!std::is_move_assignable_v<NativeRequestAdapter>);
    FakeImage image; SyntheticActor actor; NativeRequestAdapter adapter;
    Require(adapter.Initialize(image.Base(), true, &SyntheticActor::Permit, &SyntheticActor::Resolve, &actor),
        "synthetic adapter initialization failed");
    auto r = Command(Op::Submit);
    Require(adapter.Check(r) == Readiness::Waiting && adapter.ActiveCount() == 0,
        "absent resource allowed factory entry");
    auto cancel = Command(Op::Cancel); cancel.flags = cancel.mode = 0;
    cancel.goalX = cancel.goalY = cancel.goalZ = cancel.distance = 0;
    Require(adapter.Invoke(cancel) == NativeOutcome::Admitted && adapter.ActiveCount() == 0,
        "proven empty invocation could not clean up");
    std::array<std::uintptr_t, 20> request{};
    request[0] = image.Base() + 0x00EB19CC;
    const auto scheduler = actor.binding.character + 0x2CD0;
    std::memcpy(reinterpret_cast<std::uint8_t*>(request.data()) + 0x14, &scheduler, sizeof(scheduler));
    auto* status = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(request.data()) + 0xC);
    *status = 1;
    std::array<LONG, 3> control{0, 1, 1}; std::array<std::uint8_t, 0x2C> resource{};
    const std::array<std::uintptr_t,2> pair{reinterpret_cast<std::uintptr_t>(resource.data()),
        reinterpret_cast<std::uintptr_t>(control.data())};
    std::memcpy(actor.storage.data() + 0x42A8, pair.data(), sizeof(pair));
    NativeRequestAdapterTestAccess::Seed(adapter, r, actor.binding,
        reinterpret_cast<std::uintptr_t>(request.data()), reinterpret_cast<std::uintptr_t>(control.data()));
    r = cancel; r.opcode = Op::Release;
    Require(adapter.Check(r) == Readiness::Ready, "authoritative Release cannot retire a running request via native Cancel");
    *status = 2;
    Require(adapter.Check(r) == Readiness::Ready, "authoritative Release cannot retire a queued request via native Cancel");
    *status = 3;
    Require(adapter.Check(r) == Readiness::Ready && control[1] == 1, "terminal request not ready or resource pin leaked");
    Require(adapter.ActorRequests(actor.binding.character)==0U && adapter.ActiveCount()==1U,
        "terminal request Ref blocked the next interaction or was released early");
    *status=1U;
    auto action=Command(Op::Submit);action.nativeKind=kActionKind;
    NativeRequestAdapterTestAccess::Seed(adapter,action,actor.binding,
        reinterpret_cast<std::uintptr_t>(request.data()),reinterpret_cast<std::uintptr_t>(control.data()));
    Require(adapter.ActorRequests(actor.binding.character)==1U&&adapter.ActorRequests(actor.binding.character,&action)==0U,
        "matching action reservation blocked its own start");
    auto foreign=action;++foreign.commandId;
    Require(adapter.ActorRequests(actor.binding.character,&foreign)==1U,"foreign action reservation was exempted");
    foreign=action;++foreign.parameterRevision;
    Require(adapter.ActorRequests(actor.binding.character,&foreign)==1U,"wrong reservation revision was exempted");
    NativeRequestAdapterTestAccess::Seed(adapter,Command(Op::Submit),actor.binding,
        reinterpret_cast<std::uintptr_t>(request.data()),reinterpret_cast<std::uintptr_t>(control.data()));
    Require(adapter.ActorRequests(actor.binding.character,&action)==1U,"unrelated native request was exempted");
}
struct IntentProbe {
    NativeRequestAdapter* adapter{};Record exact{};LONG* strong{};
    Readiness readiness{Readiness::Waiting};std::size_t checks{},calls{};bool exactPayload{},pinned{},reentryRefused{};
};
IntentProbe* gIntentProbe{};
Readiness CheckIntentProbe(const Record& record,const LocalActorBinding&) noexcept {
    auto& probe=*gIntentProbe;++probe.checks;
    probe.exactPayload=record.intent==probe.exact.intent;return probe.readiness;
}
NativeOutcome InvokeIntentProbe(const Record& record,const LocalActorBinding&) noexcept {
    auto& probe=*gIntentProbe;++probe.calls;
    probe.exactPayload=probe.exactPayload&&record.intent==probe.exact.intent;
    probe.pinned=*probe.strong>=2;
    probe.reentryRefused=probe.adapter->Check(record)==Readiness::Rejected;
    return NativeOutcome::Admitted;
}
void NativeIntentAdapter() {
    FakeImage image;SyntheticActor actor;NativeRequestAdapter adapter;
    Require(adapter.Initialize(image.Base(),true,&SyntheticActor::Permit,&SyntheticActor::Resolve,&actor),"intent adapter initialization");
    Record r=Command(Op::Submit);r.nativeKind=native_intent::kKind;r.admission=1;
    r.flags=r.mode=0;r.goalX=r.goalY=r.goalZ=r.distance=0;
    r.intent.words[0]=13;r.intent.words[1]=1;r.intent.words[2]=std::bit_cast<std::uint32_t>(-1.25F);
    std::array<LONG,3> control{0,1,1};std::array<std::uint8_t,0x2C> resource{};
    const std::array<std::uintptr_t,2> pair{reinterpret_cast<std::uintptr_t>(resource.data()),reinterpret_cast<std::uintptr_t>(control.data())};
    std::memcpy(actor.storage.data()+0x42A8,pair.data(),sizeof(pair));
    IntentProbe probe{&adapter,r,&control[1]};gIntentProbe=&probe;
    SetNativeIntentAdapter(&CheckIntentProbe,&InvokeIntentProbe);
    Require(adapter.Check(r)==Readiness::Waiting&&adapter.LastWaitReason()==9&&probe.calls==0,"waiting intent entered native callback");
    probe.readiness=Readiness::Ready;
    Require(adapter.Invoke(r)==NativeOutcome::Admitted&&probe.calls==1&&probe.exactPayload&&probe.pinned&&probe.reentryRefused,
        "native intent lost payload/resource pin/reentry barrier");
    Require(control[1]==1&&adapter.ActiveCount()==0,"one-shot adapter leaked a request or resource pin");
    r.opcode=Op::Release;r.intent={};
    Require(adapter.Invoke(r)==NativeOutcome::Admitted&&probe.calls==1,"logical intent release invoked native constructor again");
    SetNativeIntentAdapter(nullptr,nullptr);gIntentProbe=nullptr;
}
void SchedulerControlAdapter() {
    struct Calls {
        std::array<std::uint8_t,0x50> request{};
        std::array<LONG,3> refControl{0,1,1};std::uintptr_t base{};
        std::uint32_t admission{},retention{};unsigned creates{},admits{},cancels{},releases{};
        bool cleanTail=true;
        void Tail()noexcept {for(std::size_t i=0x1C;i<request.size();++i)cleanTail=cleanTail&&request[i]==0xCD;}
        static bool Factory(void* context,std::uint32_t kind,std::uintptr_t scheduler,std::uintptr_t* out)noexcept {
            auto& h=*static_cast<Calls*>(context);++h.creates;h.request.fill(0xCD);
            const std::uint32_t header[]{static_cast<std::uint32_t>(h.base+scheduler_control::kTypeRva),0,0,0,kind,static_cast<std::uint32_t>(scheduler),0};
            std::memcpy(h.request.data(),header,sizeof(header));
            out[0]=reinterpret_cast<std::uintptr_t>(h.request.data());out[1]=reinterpret_cast<std::uintptr_t>(h.refControl.data());return true;
        }
        static bool Admit(void* context,std::uintptr_t request,std::uint32_t admission,bool& accepted)noexcept {
            auto& h=*static_cast<Calls*>(context);++h.admits;h.admission=admission;h.retention=h.request[0x18];h.Tail();
            *reinterpret_cast<std::uint32_t*>(request+0xC)=2;accepted=true;return true;
        }
        static bool Cancel(void* context,std::uintptr_t request)noexcept {
            auto& h=*static_cast<Calls*>(context);++h.cancels;h.Tail();*reinterpret_cast<std::uint32_t*>(request+0xC)=5;return true;
        }
        static bool Release(void* context,std::uintptr_t)noexcept {
            auto& h=*static_cast<Calls*>(context);++h.releases;--h.refControl[1];return true;
        }
    };
    FakeImage image;
    for(const auto kind:{6U,11U})for(const auto admission:{0U,1U,2U})for(const auto retention:{0U,1U,2U,3U}) {
        Calls calls{};calls.base=image.Base();SyntheticActor actor;NativeRequestAdapter adapter;
        Require(adapter.Initialize(image.Base(),true,&SyntheticActor::Permit,&SyntheticActor::Resolve,&actor),"control adapter init");
        std::array<LONG,3> resourceControl{0,1,1};std::array<std::uint8_t,0x2C> resource{};
        const std::uintptr_t pair[]{reinterpret_cast<std::uintptr_t>(resource.data()),reinterpret_cast<std::uintptr_t>(resourceControl.data())};
        std::memcpy(actor.storage.data()+0x42A8,pair,sizeof(pair));
        NativeRequestAdapterTestAccess::Calls(adapter,&calls,&Calls::Factory,&Calls::Admit,&Calls::Cancel,&Calls::Release);
        Record r=Command(Op::Submit);r.nativeKind=kind;r.admission=admission;r.scalarRetention=retention;
        r.flags=r.mode=0;r.goalX=r.goalY=r.goalZ=r.distance=0;
        Require(adapter.Check(r)==Readiness::Ready&&adapter.Invoke(r)==NativeOutcome::Admitted,"control Submit failed");
        Require(calls.admission==admission&&calls.retention==(retention&1)&&calls.request[0x18]==(retention>>1),"control retention before/after lost");
        r.opcode=Op::Update;++r.parameterRevision;r.scalarRetention=3;
        Require(adapter.Invoke(r)==NativeOutcome::Admitted&&calls.request[0x18]==1,"control retention update failed");
        *reinterpret_cast<std::uint32_t*>(calls.request.data()+0xC)=3;
        r.opcode=Op::Release;r.scalarRetention=3;
        Require(adapter.Invoke(r)==NativeOutcome::Admitted&&adapter.ActiveCount()==0,"control Release failed");
        Require(calls.creates==1&&calls.admits==1&&calls.cancels==1&&calls.releases==1&&calls.cleanTail&&
            calls.refControl[1]==0&&resourceControl[1]==1,"control wrote beyond base header or leaked references");
    }
}
void PlacementAdapter() {
    struct Calls {
        std::array<std::uint8_t,0x34> request{};std::array<LONG,3> control{0,1,1};
        std::uintptr_t base{};std::uint32_t admission{},retention{};unsigned creates{},admits{},cancels{},releases{};
        static bool Factory(void* context,std::uint32_t kind,std::uintptr_t scheduler,std::uintptr_t* out)noexcept {
            auto& h=*static_cast<Calls*>(context);++h.creates;if(kind!=kPlacementKind)return false;h.request.fill(0xCD);
            const std::uint32_t header[]{static_cast<std::uint32_t>(h.base+0xEB199CU),0,0,0,kind,static_cast<std::uint32_t>(scheduler),0};
            std::memcpy(h.request.data(),header,sizeof(header));out[0]=reinterpret_cast<std::uintptr_t>(h.request.data());
            out[1]=reinterpret_cast<std::uintptr_t>(h.control.data());return true;
        }
        static bool Admit(void* context,std::uintptr_t request,std::uint32_t admission,bool& accepted)noexcept {
            auto& h=*static_cast<Calls*>(context);++h.admits;h.admission=admission;h.retention=h.request[0x18];
            *reinterpret_cast<std::uint32_t*>(request+0xC)=2;accepted=true;return true;
        }
        static bool Cancel(void* context,std::uintptr_t request)noexcept {
            ++static_cast<Calls*>(context)->cancels;*reinterpret_cast<std::uint32_t*>(request+0xC)=5;return true;
        }
        static bool Release(void* context,std::uintptr_t)noexcept {
            auto& h=*static_cast<Calls*>(context);++h.releases;--h.control[1];return true;
        }
    };
    FakeImage image;
    for(const auto branch:{0U,1U})for(const auto admission:{0U,1U,2U})for(const auto retention:{0U,1U,2U,3U}) {
        Calls calls{};calls.base=image.Base();SyntheticActor actor;NativeRequestAdapter adapter;
        Require(adapter.Initialize(image.Base(),true,&SyntheticActor::Permit,&SyntheticActor::Resolve,&actor),"placement adapter init");
        std::array<LONG,3> resourceControl{0,1,1};std::array<std::uint8_t,0x2C> resource{};
        const std::uintptr_t pair[]{reinterpret_cast<std::uintptr_t>(resource.data()),reinterpret_cast<std::uintptr_t>(resourceControl.data())};
        std::memcpy(actor.storage.data()+0x42A8,pair,sizeof(pair));
        NativeRequestAdapterTestAccess::Calls(adapter,&calls,&Calls::Factory,&Calls::Admit,&Calls::Cancel,&Calls::Release);
        auto r=Command(Op::Submit);r.nativeKind=kPlacementKind;r.mode=branch;r.admission=admission;r.scalarRetention=retention;r.distance=0;r.aux=-1.25F;
        Require(!PlacementDedupReady()&&adapter.Invoke(r)==NativeOutcome::Rejected&&calls.creates==0,"placement bypassed dedup integration gate");
        SetPlacementDedupReady(true);
        Require(adapter.Invoke(r)==NativeOutcome::Admitted&&calls.creates==1&&calls.admits==1,"placement Submit failed");
        std::array<std::uintptr_t,4> head{},node{};
        head[0]=reinterpret_cast<std::uintptr_t>(node.data());node[0]=reinterpret_cast<std::uintptr_t>(head.data());
        node[2]=reinterpret_cast<std::uintptr_t>(calls.request.data());node[3]=reinterpret_cast<std::uintptr_t>(calls.control.data());
        const auto list=reinterpret_cast<std::uintptr_t>(head.data()),scheduler=actor.binding.character+0x2CD0U;
        const std::uint32_t count=1;
        std::memcpy(actor.storage.data()+0x2CD4,&list,sizeof(list));std::memcpy(actor.storage.data()+0x2CD8,&count,sizeof(count));
        RetirementIdentity identity{};
        Require(adapter.IsOwnedPlacement(scheduler,&identity)&&identity.command==r.commandId&&identity.revision==r.parameterRevision&&
            identity.scope==r.scope&&identity.owner==r.owner,"placement typed current-head identity missing");
        ++actor.binding.creationSerial;Require(!adapter.IsOwnedPlacement(scheduler),"foreign actor incarnation acquired placement ownership");--actor.binding.creationSerial;
        ++node[2];Require(!adapter.IsOwnedPlacement(scheduler),"foreign scheduler head acquired placement ownership");--node[2];
        const auto payload=[&] {
            float values[4]{};std::memcpy(values,calls.request.data()+0x1C,sizeof(values));
            Require(values[0]==r.goalX&&values[1]==r.goalY&&values[2]==r.goalZ&&values[3]==r.aux&&calls.request[0x2C]==r.mode,"placement fields changed");
            for(std::size_t i=0x2D;i<calls.request.size();++i)Require(calls.request[i]==0xCD,"placement wrote padding or unused tail");
        };
        payload();Require(calls.admission==admission&&calls.retention==(retention&1)&&calls.request[0x18]==(retention>>1),"placement admission/retention lost");
        r.opcode=Op::Update;++r.parameterRevision;r.goalX=-4.5F;r.aux=2;r.mode=1-branch;
        // Same-vtable kind6 cannot impersonate our live kind8 request.
        calls.request[0x10]=6;Require(adapter.Check(r)==Readiness::Rejected,"same-vtable foreign kind accepted");calls.request[0x10]=8;
        Require(adapter.Invoke(r)==NativeOutcome::Admitted&&calls.creates==1&&calls.admits==1,"placement mutation recreated native request");payload();
        const auto oldIdentity=identity;
        Require(adapter.IsOwnedPlacement(scheduler,&identity)&&identity.revision==r.parameterRevision&&identity!=oldIdentity,"placement typed revision was not immutable");
        SetPlacementDedupReady(false);
        ++r.parameterRevision;Require(adapter.Invoke(r)==NativeOutcome::Rejected,"withdrawn dedup proof allowed placement update");
        r.opcode=Op::Release;r.flags=r.mode=0;r.goalX=r.goalY=r.goalZ=r.aux=0;r.scalarRetention=3;
        Require(adapter.Check(r)==Readiness::Waiting&&calls.cancels==0&&calls.releases==0&&adapter.ActiveCount()==1,
            "same-drain Release cancelled placement before native execution");
        // Native scheduler consumes the retained placement on its next frame.
        float target[3]{};std::memcpy(target,calls.request.data()+0x1C,sizeof(target));
        std::memcpy(actor.storage.data()+0x70,target,sizeof(target));
        *reinterpret_cast<std::uint32_t*>(calls.request.data()+0xC)=3;
        Require(adapter.Invoke(r)==NativeOutcome::Admitted&&adapter.ActiveCount()==0&&calls.cancels==1&&calls.releases==1&&
            calls.control[1]==0&&resourceControl[1]==1,"placement retirement leaked native references");
    }
    // Reproduce the actual failure: one engine/IPC drain sees the entire Host
    // lifecycle before CATHODE receives its next frame. The journal must retain
    // the Release, block its checkpoint, then settle it once retail finished.
    {
        Calls calls{};calls.base=image.Base();SyntheticActor actor;NativeRequestAdapter native;
        Require(native.Initialize(image.Base(),true,&SyntheticActor::Permit,&SyntheticActor::Resolve,&actor),"placement drain init");
        std::array<LONG,3> control{0,1,1};std::array<std::uint8_t,0x2C> resource{};
        const std::uintptr_t pin[]{reinterpret_cast<std::uintptr_t>(resource.data()),reinterpret_cast<std::uintptr_t>(control.data())};
        std::memcpy(actor.storage.data()+0x42A8,pin,sizeof(pin));
        NativeRequestAdapterTestAccess::Calls(native,&calls,&Calls::Factory,&Calls::Admit,&Calls::Cancel,&Calls::Release);
        SetPlacementDedupReady(true);
        TransactionExecutor journal({&native,[](void* p,const Record& r) {
            const auto state=static_cast<NativeRequestAdapter*>(p)->Check(r);
            return state==Readiness::Ready?Dependency::Ready:state==Readiness::Waiting?Dependency::Waiting:Dependency::Reject;
        },[](void* p,const Record& r){return static_cast<NativeRequestAdapter*>(p)->Invoke(r);}}, {},true);
        auto begin=Command();begin.nativeKind=kPlacementKind;begin.mode=0;begin.distance=0;
        Require(journal.Configure(begin.scope)==Result::Ok,"placement drain scope");
        auto submit=begin;submit.opcode=Op::Submit;submit.globalSeq=submit.ownerSeq=2;
        auto complete=submit;complete.opcode=Op::Complete;complete.globalSeq=complete.ownerSeq=3;
        complete.flags=0;complete.goalX=complete.goalY=complete.goalZ=complete.aux=0;complete.scalarRetention=3;
        auto release=complete;release.opcode=Op::Release;release.globalSeq=release.ownerSeq=4;
        Require(journal.Execute(begin).acceptedLogical&&journal.Execute(submit).nativeOutcome==NativeOutcome::Admitted&&
            journal.Execute(complete).acceptedLogical,"placement drain admission");
        Require(journal.Execute(release).status==ExecutionStatus::Waiting&&native.ActiveCount()==1&&calls.cancels==0,
            "placement lost its native frame in combined lifecycle drain");
        Require(journal.Checkpoint(begin.scope,4)==Result::OwnerBusy,"placement checkpoint advanced before native completion");
        *reinterpret_cast<std::uint32_t*>(calls.request.data()+0xC)=3;
        Require(journal.Retry(release).nativeOutcome==NativeOutcome::Admitted&&native.ActiveCount()==0&&calls.cancels==1&&calls.releases==1,
            "placement deferred Release failed or cancelled twice");
        Require(journal.Checkpoint(begin.scope,4)==Result::Ok,"placement checkpoint stayed blocked after release");
        SetPlacementDedupReady(false);
    }
}
bool PermissionCheck(void* opaque, const Record& record) noexcept {
    auto& c = *static_cast<Context*>(opaque); ++c.permissionCalls;
    if (c.reenter) {
        auto other = record.scope; ++other.consumerEpoch;
        if (c.session->Configure(other) != Result::OwnerBusy) std::terminate();
    }
    return c.allowed;
}
bool MissingActor(void* opaque, const OwnerKey&, LocalActorBinding&) noexcept {
    ++static_cast<Context*>(opaque)->resolveCalls; return false;
}
Readiness PairReservation(void*,const Record& record) noexcept {
    return record.opcode==Op::Submit||record.opcode==Op::Update?Readiness::Waiting:Readiness::Ready;
}
void SessionGuards() {
    ExecutionSession session; Context context{&session};
    Require(!session.Initialize(0, false, &PermissionCheck, &MissingActor, &context), "unvalidated image initialized");
    FakeImage image;
    Require(session.Initialize(image.Base(), true, &PermissionCheck, &MissingActor, &context,&PairReservation), "synthetic image contract failed");
    Require(session.Configure(TestScope()) == Result::Ok, "scope binding failed");
    Batch batch{}; batch.scope = TestScope(); batch.batchId = 1; batch.count = 1; batch.records[0] = Command();
    std::vector<std::uint8_t> bytes; Require(EncodeBatch(batch, bytes), "session test encode failed");
    std::array<Receipt,16> receipts{}; std::size_t count{};
    Require(!session.Handle(bytes, batch.scope, false, receipts, count) && count == 0, "unauthenticated entry accepted");
    auto old = batch.scope; ++old.worldEpoch;
    Require(!session.Handle(bytes, old, true, receipts, count), "stale authority accepted");
    auto invalid = bytes; invalid.push_back(0);
    Require(!session.Handle(invalid, batch.scope, true, receipts, count), "malformed batch entered ledger");
    context.reenter = true;
    Require(session.Handle(bytes, batch.scope, true, receipts, count) && count == 1 && receipts[0].stage == Stage::Received,
        "metadata admission failed");
    Require(context.resolveCalls == 0 && session.ActiveRequests() == 0, "Begin called native adapter");
    Require(session.Handle(bytes, batch.scope, true, receipts, count) && session.ActiveRequests() == 0,
        "metadata duplicate changed ownership");
    batch.records[0] = Command(Op::Submit); batch.records[0].globalSeq = batch.records[0].ownerSeq = 2;
    Require(EncodeBatch(batch, bytes) && session.Handle(bytes, batch.scope, true, receipts, count) &&
        receipts[0].stage == Stage::Waiting && session.ActiveRequests() == 0, "missing dependency invoked native factory");
    Require(context.resolveCalls==0,"reserved native command tried to resolve or drain its participants");
    Receipt retried{};
    Require(session.Retry(batch.records[0], 1, batch.scope, true, retried) && retried.stage == Stage::Waiting,
        "waiting retry did not retain exact record");
    auto terminal = Command(Op::Cancel);
    terminal.globalSeq = terminal.ownerSeq = 3; terminal.flags = terminal.mode = 0;
    terminal.goalX = terminal.goalY = terminal.goalZ = terminal.distance = 0;
    batch.records[0] = terminal;
    Require(EncodeBatch(batch, bytes) && session.Handle(bytes, batch.scope, true, receipts, count) &&
        receipts[0].stage == Stage::Admitted && session.ActiveRequests() == 0,
        "cancel of never-entered request required a nonexistent actor");
    terminal.opcode = Op::Release; terminal.globalSeq = terminal.ownerSeq = 4;
    batch.records[0] = terminal;
    Require(EncodeBatch(batch, bytes) && session.Handle(bytes, batch.scope, true, receipts, count) &&
        receipts[0].stage == Stage::Released && session.ActiveRequests() == 0, "unentered request could not release");
    auto replacement = Command(); replacement.commandId = 2; replacement.globalSeq = replacement.ownerSeq = 5;
    batch.records[0] = replacement;
    Require(EncodeBatch(batch, bytes) && session.Handle(bytes, batch.scope, true, receipts, count) &&
        receipts[0].stage == Stage::Received, "unentered cancellation blocked replacement");
    terminal.opcode = Op::Cancel; terminal.globalSeq = terminal.ownerSeq = 6;
    batch.records[0] = terminal;
    Require(EncodeBatch(batch, bytes) && session.Handle(bytes, batch.scope, true, receipts, count) &&
        receipts[0].stage == Stage::Rejected && session.ActiveRequests() == 0, "old cleanup mutated replacement");
    context.allowed = false;
    Require(!session.Handle(bytes, batch.scope, true, receipts, count) && count == 0, "revoked permission accepted duplicate");
    Require(session.ActiveRequests() == 0, "guard test executed synthetic image code");
}
void ActionCoordinationGuards() {
    using Action=aimp::ai_sync::action_correlation::Readiness;
    namespace correlation=aimp::ai_sync::action_correlation;
    FakeImage image;ExecutionSession session;Context context{&session};
    Require(session.Initialize(image.Base(),true,&PermissionCheck,&MissingActor,&context),"action session initialization failed");
    auto operation=Command();operation.nativeKind=kActionKind;operation.flags=0U;operation.mode=9U;
    operation.goalX=operation.goalY=operation.goalZ=operation.distance=operation.stopDistance=operation.aux=0;
    operation.scope.consumerEpoch=42U;
    Require(session.Configure(operation.scope)==Result::Ok,"action scope binding failed");
    auto submit=operation;submit.opcode=Op::Submit;submit.globalSeq=submit.ownerSeq=2U;
    auto source=submit;source.scope.consumerEpoch=1U;
    correlation::Reference reference{};
    Require(correlation::Capture(source,1U,reference),"action reference capture failed");
    Require(session.ActionReadiness(reference,submit.owner)==Action::Waiting,"early snapshot didn't wait for the journal");
    Batch batch{};batch.scope=operation.scope;batch.batchId=1U;batch.count=2;
    batch.records[0]=operation;batch.records[1]=submit;
    std::vector<std::uint8_t> bytes;std::array<Receipt,16> receipts{};std::size_t count{};
    Require(EncodeBatch(batch,bytes)&&session.Handle(bytes,batch.scope,true,receipts,count)&&count==2U&&
        receipts[1].stage==Stage::Waiting,"action reservation did not remain a native dependency wait");
    Require(session.ActionReadiness(reference,submit.owner)==Action::Ready&&session.ActiveRequests()==0U,
        "action start circularly required native kind5 admission");
    auto wrong=reference;++wrong.ownerSequence;
    Require(session.ActionReadiness(wrong,submit.owner)==Action::Rejected,"wrong action sequences accepted");
    wrong=reference;wrong.source.session[8]=1U;
    Require(session.ActionReadiness(wrong,submit.owner)==Action::Rejected,"foreign full session accepted");
    auto cancel=submit;cancel.opcode=Op::Cancel;cancel.mode=0U;cancel.globalSeq=cancel.ownerSeq=3U;
    batch.records[0]=cancel;batch.count=1;
    Require(EncodeBatch(batch,bytes)&&session.Handle(bytes,batch.scope,true,receipts,count),"action cancellation failed");
    Require(session.ActionReadiness(reference,submit.owner)==Action::Superseded,"late cancelled action could start");
    auto release=cancel;release.opcode=Op::Release;release.globalSeq=release.ownerSeq=4U;
    batch.records[0]=release;
    Require(EncodeBatch(batch,bytes)&&session.Handle(bytes,batch.scope,true,receipts,count)&&
        session.Checkpoint(batch.scope,4U)==Result::OwnerBusy,"checkpoint erased an unexecuted Host action");
    Require(session.ActionReadiness(reference,submit.owner)==Action::Superseded,"retired action waited or resurrected");
    Require(session.ActiveRequests()==0U,"action query entered native code");

    ExecutionSession ordered;Context orderedContext{&ordered};
    Require(ordered.Initialize(image.Base(),true,&PermissionCheck,&MissingActor,&orderedContext)&&
        ordered.Configure(operation.scope)==Result::Ok,"ordered action session failed");
    auto predecessor=Command();predecessor.scope=operation.scope;
    auto predecessorSubmit=predecessor;predecessorSubmit.opcode=Op::Submit;
    predecessorSubmit.globalSeq=predecessorSubmit.ownerSeq=2U;
    // Appended (admission 1): it queues behind the predecessor natively. A
    // replacing Submit (admission 0) would instead supersede that predecessor.
    auto next=operation;next.commandId=2U;next.globalSeq=next.ownerSeq=3U;next.admission=1U;
    auto nextSubmit=next;nextSubmit.opcode=Op::Submit;nextSubmit.globalSeq=nextSubmit.ownerSeq=4U;
    batch.count=4;batch.records[0]=predecessor;batch.records[1]=predecessorSubmit;batch.records[2]=next;batch.records[3]=nextSubmit;
    Require(EncodeBatch(batch,bytes)&&ordered.Handle(bytes,batch.scope,true,receipts,count),"ordered action batch failed");
    source=nextSubmit;source.scope.consumerEpoch=1U;
    Require(correlation::Capture(source,2U,reference)&&ordered.ActionReadiness(reference,next.owner)==Action::Waiting,
        "action bypassed a waiting native predecessor");
    cancel=predecessorSubmit;cancel.opcode=Op::Cancel;cancel.globalSeq=cancel.ownerSeq=5U;
    cancel.flags=cancel.mode=0U;cancel.goalX=cancel.goalY=cancel.goalZ=cancel.distance=0;
    batch.count=1;batch.records[0]=cancel;
    Require(EncodeBatch(batch,bytes)&&ordered.Handle(bytes,batch.scope,true,receipts,count)&&
        ordered.ActionReadiness(reference,next.owner)==Action::Ready,"Host cancellation failed to unblock the dependent action");
}
void DelayedInteractionReservation() {
    using Action=action_correlation::Readiness;
    struct Probe {
        Dependency dependency{Dependency::Waiting};NativeOutcome outcome{NativeOutcome::Admitted};unsigned calls{};
        static Dependency Check(void* p,const Record&) {return static_cast<Probe*>(p)->dependency;}
        static NativeOutcome Invoke(void* p,const Record&) {auto& probe=*static_cast<Probe*>(p);++probe.calls;return probe.outcome;}
    };
    for(const auto outcome:{NativeOutcome::Admitted,NativeOutcome::Rejected,NativeOutcome::Unknown}) {
        FakeImage image;ExecutionSession session;Context context{&session};Probe probe{};probe.outcome=outcome;
        Require(session.Initialize(image.Base(),true,&PermissionCheck,&MissingActor,&context),"delayed pair init");
        ExecutionSessionTestAccess::Adapter(session,{&probe,&Probe::Check,&Probe::Invoke});
        auto begin=Command();begin.nativeKind=kActionKind;begin.flags=0;begin.mode=0x4E;
        begin.goalX=begin.goalY=begin.goalZ=begin.distance=0;
        Require(session.Configure(begin.scope)==Result::Ok,"delayed pair scope");
        auto submit=begin;submit.opcode=Op::Submit;submit.globalSeq=submit.ownerSeq=2;
        auto source=submit;source.scope.consumerEpoch=1;action_correlation::Reference reference{};
        Require(action_correlation::Capture(source,9,reference),"delayed pair reference");
        auto cancel=submit;cancel.opcode=Op::Cancel;cancel.mode=0;cancel.globalSeq=cancel.ownerSeq=3;
        auto release=cancel;release.opcode=Op::Release;release.globalSeq=release.ownerSeq=4;
        Batch batch{};batch.scope=begin.scope;batch.batchId=1;batch.count=4;
        batch.records[0]=begin;batch.records[1]=submit;batch.records[2]=cancel;batch.records[3]=release;
        std::vector<std::uint8_t> bytes;std::array<Receipt,16> receipts{};std::size_t count{};
        Require(EncodeBatch(batch,bytes)&&session.Handle(bytes,batch.scope,true,receipts,count)&&count==4,
            "delayed pair sequence");
        Require(probe.calls==0&&receipts[1].stage==Stage::Waiting&&receipts[2].stage==Stage::Waiting&&
            receipts[3].stage==Stage::Waiting,"terminal overtook or erased the Host action");
        Require(session.ActionReadiness(reference,submit.owner)==Action::Superseded,"current action resurrected");
        // The pending Host event may prepare the pair before its reservation
        // enters, as in the live path; no native invocation happens in this query.
        Require(session.ActionReadiness(reference,submit.owner,true)==Action::Ready&&probe.calls==0,
            "ordered pair cannot authorize its preserved start");
        probe.dependency=Dependency::Ready;Receipt result{};
        Require(session.Retry(submit,2,begin.scope,true,result)&&probe.calls==1,"preserved action retry");
        Require(session.ActionReadiness(reference,submit.owner,true)==
            (outcome==NativeOutcome::Admitted?Action::Ready:Action::Rejected),"failed/unknown admission authorized a pair");
        if(outcome!=NativeOutcome::Admitted)continue;
        Require(session.Retry(cancel,3,begin.scope,true,result)&&session.Retry(release,4,begin.scope,true,result)&&probe.calls==3,
            "preserved cleanup did not drain in order");
        Require(session.ActionReadiness(reference,submit.owner,true)==Action::Ready&&
            session.ActionReadiness(reference,submit.owner)==Action::Superseded&&probe.calls==3,
            "late interaction lost admitted evidence or reentered native code");
        auto wrong=reference;++wrong.parameterRevision;
        Require(session.ActionReadiness(wrong,submit.owner,true)==Action::Rejected,"history accepted a different revision");
        auto replacement=begin;replacement.commandId=2;replacement.globalSeq=replacement.ownerSeq=5;
        auto replacing=replacement;replacing.opcode=Op::Submit;replacing.globalSeq=replacing.ownerSeq=6;
        probe.outcome=NativeOutcome::Unknown;batch.batchId=5;batch.count=2;batch.records[0]=replacement;batch.records[1]=replacing;
        Require(EncodeBatch(batch,bytes)&&session.Handle(bytes,batch.scope,true,receipts,count),"unknown replacement sequence");
        Require(session.ActionReadiness(reference,submit.owner,true)==Action::Rejected,"old history bypassed owner quarantine");
    }
}
void GuidedAuthorityStopsAtComplete() {
    struct Probe {
        static Dependency Check(void*,const Record&) {return Dependency::Ready;}
        static NativeOutcome Invoke(void*,const Record&) {return NativeOutcome::Admitted;}
    };
    FakeImage image;ExecutionSession session;Context context{&session};
    Require(session.Initialize(image.Base(),true,&PermissionCheck,&MissingActor,&context),"guidance session init");
    ExecutionSessionTestAccess::Adapter(session,{nullptr,&Probe::Check,&Probe::Invoke});
    auto begin=Command();begin.nativeKind=1;begin.flags=begin.mode=0;
    begin.goalX=begin.goalY=begin.goalZ=begin.distance=0;
    begin.movement.version=begin.movement.parts=1;begin.movement.navigation=movement::Navigation::HostGuided;
    Require(session.Configure(begin.scope)==Result::Ok,"guidance session scope");
    Batch batch{};batch.scope=begin.scope;batch.batchId=1;batch.count=2;batch.records[0]=begin;
    auto submit=begin;submit.opcode=Op::Submit;submit.globalSeq=submit.ownerSeq=2;batch.records[1]=submit;
    std::vector<std::uint8_t> bytes;std::array<Receipt,16> receipts{};std::size_t count{};
    Require(EncodeBatch(batch,bytes)&&session.Handle(bytes,begin.scope,true,receipts,count),"guidance sequence");
    Require(ExecutionSessionTestAccess::Guidance(session,submit),"admitted Host Move cannot guide");
    auto stale=submit;++stale.parameterRevision;
    Require(!ExecutionSessionTestAccess::Guidance(session,stale),"foreign revision can guide");
    stale=submit;++stale.scope.authorityEpoch;
    Require(!ExecutionSessionTestAccess::Guidance(session,stale),"foreign authority can guide");
    auto complete=submit;complete.opcode=Op::Complete;complete.movement={};complete.globalSeq=complete.ownerSeq=3;
    batch.batchId=2;batch.count=1;batch.records[0]=complete;
    Require(EncodeBatch(batch,bytes)&&session.Handle(bytes,begin.scope,true,receipts,count),"guidance Complete");
    Require(!ExecutionSessionTestAccess::Guidance(session,submit),"Complete resurrected a retained Move");
}
// Encoded Host sequences through the real session; missing actors keep factories out.
void ActionSequenceScenarios() {
    using Action=action_correlation::Readiness;
    for(const auto terminal:{Op::Cancel,Op::Stop,Op::Complete}) {
        FakeImage image;ExecutionSession session;Context context{&session};
        Require(session.Initialize(image.Base(),true,&PermissionCheck,&MissingActor,&context),"sequence initialize failed");
        const auto scope=TestScope();Require(session.Configure(scope)==Result::Ok,"sequence configure failed");
        std::array<Receipt,16> receipts{};std::size_t count{};std::uint64_t batchId{};
        const auto send=[&](std::initializer_list<Record> records) {
            Batch batch{};batch.scope=scope;batch.batchId=++batchId;batch.count=records.size();
            std::copy(records.begin(),records.end(),batch.records.begin());std::vector<std::uint8_t> bytes;
            Require(EncodeBatch(batch,bytes)&&session.Handle(bytes,scope,true,receipts,count)&&count==batch.count,
                "encoded action sequence rejected");
            for(std::size_t i=0;i<count;++i)Require(receipts[i].stage!=Stage::Rejected,"sequence operation rejected");
        };
        const auto reference=[](Record operation) {
            operation.scope.consumerEpoch=1U;action_correlation::Reference ref{};
            Require(action_correlation::Capture(operation,7U,ref),"sequence correlation failed");return ref;
        };
        const auto end=[](Record operation,Op op,std::uint64_t global,std::uint64_t owner) {
            operation.opcode=op;operation.globalSeq=global;operation.ownerSeq=owner;
            operation.flags=operation.mode=operation.gait=0U;
            operation.goalX=operation.goalY=operation.goalZ=operation.distance=operation.stopDistance=operation.aux=0;
            operation.movement={};return operation;
        };
        auto move=Command();move.nativeKind=1U;move.flags=move.mode=0U;
        move.goalX=move.goalY=move.goalZ=move.distance=0;
        auto& b=move.movement;b.version=1;b.parts=1;b.gait=3;b.navigation=movement::Navigation::HostRoute;
        b.before={1,2,{1.25F,-2.5F,3.75F},-1,-1,{0,0,0,0,50,.25F,-.5F,8,1.15F},0x21,0x10};
        b.after=b.before;b.after.flags90=1;b.after.flags91=0x11;b.after.stopDistance=.5F;
        auto moving=move;moving.opcode=Op::Submit;moving.globalSeq=moving.ownerSeq=2U;
        auto action=end(Command(),Op::Begin,3U,3U);action.nativeKind=kActionKind;action.mode=0x4EU;action.commandId=2U;action.admission=1U; // Appended: a replacing action would supersede the Move.
        auto submit=action;submit.opcode=Op::Submit;submit.globalSeq=submit.ownerSeq=4U;
        const auto ref=reference(submit);
        Require(session.ActionReadiness(ref,submit.owner)==Action::Waiting,"early action did not wait");
        send({move,moving,action,submit});
        Require(session.ActionReadiness(ref,submit.owner)==Action::Waiting,"action bypassed pending Host-route movement");
        auto other=action;other.owner.entityA=99U;other.globalSeq=5U;other.ownerSeq=1U;
        auto otherSubmit=other;otherSubmit.opcode=Op::Submit;otherSubmit.globalSeq=6U;otherSubmit.ownerSeq=2U;
        const auto otherRef=reference(otherSubmit);send({other,otherSubmit});
        Require(session.ActionReadiness(otherRef,other.owner)==Action::Ready,"one actor's wait stalled another actor");
        send({end(moving,Op::Cancel,7U,5U)});
        Require(session.ActionReadiness(ref,submit.owner)==Action::Ready,"Host movement cancellation did not permit action");
        send({submit});Require(session.ActionReadiness(ref,submit.owner)==Action::Ready,"duplicate changed action readiness");
        send({end(submit,terminal,8U,6U)});
        Require(session.ActionReadiness(ref,submit.owner)==Action::Superseded,"terminal action resurrected from delayed State");
        Require(session.ActionReadiness(otherRef,other.owner)==Action::Ready,"terminal action fenced unrelated actor");
        send({end(submit,Op::Release,9U,7U)});
        auto replacement=action;replacement.commandId=3U;replacement.globalSeq=10U;replacement.ownerSeq=8U;
        auto replacementSubmit=replacement;replacementSubmit.opcode=Op::Submit;replacementSubmit.globalSeq=11U;replacementSubmit.ownerSeq=9U;
        send({replacement,replacementSubmit});
        const auto replacementRef=reference(replacementSubmit);
        Require(session.ActionReadiness(replacementRef,replacement.owner)==Action::Waiting&&
            session.ActionReadiness(ref,submit.owner)==Action::Superseded,"replacement revived old command or stalled");
        auto next=scope;++next.worldEpoch;++next.consumerEpoch;
        Require(session.Configure(next)==Result::OwnerBusy,"scope discarded outstanding native cleanup");
        send({end(moving,Op::Release,12U,10U),end(otherSubmit,Op::Cancel,13U,3U),
            end(otherSubmit,Op::Release,14U,4U),end(replacementSubmit,Op::Cancel,15U,11U),
            end(replacementSubmit,Op::Release,16U,12U)});
        Require(session.Configure(next)==Result::Ok,"unentered action scope could not retire");
        Require(session.ActionReadiness(replacementRef,replacement.owner)==Action::Rejected,"old-world action crossed binding");
        Require(!session.Retry(replacementSubmit,99U,scope,true,receipts[0]),"old-world retry crossed binding");
        Require(session.ActiveRequests()==0U,"sequence query unexpectedly entered native factory");
    }
}
Scope gRuntimeExpectedScope = TestScope();
bool gRuntimeFrozen{};
struct RuntimeOwnershipHarness {
    SyntheticActor actor;
    unsigned drains{}, releases{}, restores{};
    bool quiescent{true};
    OwnershipCheck drain{OwnershipCheck::Ready};
    static OwnershipCheck Resolve(void* context, const Scope& scope, const OwnerKey& owner, ActorLifetime& output) noexcept {
        auto& h = *static_cast<RuntimeOwnershipHarness*>(context);
        if (scope.session != gRuntimeExpectedScope.session || scope.authorityEpoch != gRuntimeExpectedScope.authorityEpoch ||
            scope.worldEpoch != gRuntimeExpectedScope.worldEpoch || owner.entityA != 1 || owner.entityB != 2 || owner.birth != 1 || owner.actorClass != 1)
            return OwnershipCheck::Reject;
        output = {h.actor.binding.character, h.actor.binding.runtimeId, 7}; return OwnershipCheck::Ready;
    }
    static OwnershipCheck Drain(void* context, const OwnershipToken& token) noexcept {
        auto& h = *static_cast<RuntimeOwnershipHarness*>(context); ++h.drains;
        if (execution_runtime::AllowsLegacy(token.actor) ||
            execution_runtime::RequestOwnership(token.owner) != OwnershipResult::Busy ||
            execution_runtime::RetireOwnership(token.owner) != OwnershipResult::Busy) std::terminate();
        return h.drain;
    }
    static OwnershipCheck Released(void* context, const OwnershipToken&) noexcept {
        auto& h = *static_cast<RuntimeOwnershipHarness*>(context); ++h.releases;
        return h.quiescent ? OwnershipCheck::Ready : OwnershipCheck::Waiting;
    }
    static OwnershipCheck Restore(void* context, const OwnershipToken& token) noexcept {
        auto& h = *static_cast<RuntimeOwnershipHarness*>(context); ++h.restores;
        if (execution_runtime::AllowsLegacy(token.actor)) std::terminate();
        return OwnershipCheck::Ready;
    }
    OwnershipAdapter Api() noexcept { return {this, &Resolve, &Drain, &Released, &Restore}; }
};
void RuntimeOwnership(bool commandAdmission) {
    // Synthetic image is READWRITE, not executable. Movement resources are
    // absent, so neither factory nor Submit/Cancel/RefRelease retail can run.
    FakeImage image; RuntimeOwnershipHarness h;
    const auto qualified = [](const Scope& scope, std::uint32_t generation) noexcept {
        return !gRuntimeFrozen && scope == gRuntimeExpectedScope && generation == 7;
    };
    const auto retirementQualified = [](const Scope& scope, std::uint32_t generation) noexcept {
        return scope.session == gRuntimeExpectedScope.session && scope.authorityEpoch == gRuntimeExpectedScope.authorityEpoch &&
            scope.worldEpoch == gRuntimeExpectedScope.worldEpoch && generation == 7;
    };
    auto partial = h.Api(); partial.restore = nullptr;
    Require(!execution_runtime::Initialize(image.Base(),true,qualified,partial), "partial local hooks initialized");
    Require(!execution_runtime::Initialize(image.Base(),true,qualified,h.Api()), "ownership without retirement qualifier initialized");
    Require(execution_runtime::Initialize(image.Base(),true,qualified,h.Api(),retirementQualified,commandAdmission), "qualified runtime init failed");
    // Production emits OFF/old-scope retirement before its first Offer. This
    // request has no native generation to drain yet; it must not retire the
    // fresh scope after advertising Ready on the next frame.
    execution_runtime::Retire();execution_runtime::Pump();
    auto record = Command();
    Require(execution_runtime::RequestOwnership(record.owner) == OwnershipResult::Busy && h.drains == 0,
        "ownership acquired before binding");
    std::array<std::uint8_t,kHeaderSize> control{};
    Require(EncodeControl({record.scope,ControlKind::Offer,3},control) && execution_runtime::QueueBinding(control,7), "runtime bind queue failed");
    execution_runtime::Pump(); MailboxPacket packet{};
    Require(execution_runtime::Peek(packet) && !packet.receipt && execution_runtime::Commit(packet.token), "runtime Ready missing");
    execution_runtime::Pump();
    Require(!execution_runtime::RetirementSnapshot().retired,"pre-bind retirement killed the fresh Ready scope");
    Scope boundScope{};
    Require(execution_runtime::CurrentScope(boundScope)&&boundScope==record.scope,
        "bound scope unavailable between mailbox callbacks");
    const ActorLifetime local{h.actor.binding.character,h.actor.binding.runtimeId,7};
    const auto send = [&](const Record& operation, Stage stage, bool retry = false) {
        Batch batch{}; batch.scope = operation.scope; batch.batchId = operation.globalSeq; batch.count = 1; batch.records[0] = operation;
        std::vector<std::uint8_t> bytes;
        Require(EncodeBatch(batch,bytes) && execution_runtime::QueueBatch(bytes,retry), "runtime queue failed");
        execution_runtime::Pump(); Receipt receipt{};
        Require(execution_runtime::Peek(packet) && packet.receipt &&
            DecodeReceipt(std::span(packet.bytes).first(packet.size),operation.scope,receipt) && receipt.stage == stage &&
            execution_runtime::Commit(packet.token), "runtime disposition mismatch");
    };
    if (commandAdmission) {
        auto gap = record; gap.globalSeq = gap.ownerSeq = 9;
        send(gap,Stage::Rejected);
        Require(h.drains == 0 && execution_runtime::AllowsLegacy(local), "rejected Begin drained a local owner");
    }
    send(record,Stage::Received);
    Require(h.drains == 0 && execution_runtime::AllowsLegacy(local), "Begin drained a local owner");
    record.opcode = Op::Submit; record.globalSeq = record.ownerSeq = 2;
    if (commandAdmission) h.drain = OwnershipCheck::Waiting;
    send(record,Stage::Waiting);
    if (commandAdmission) {
        Require(h.drains == 1 && !execution_runtime::AllowsLegacy(local), "accepted Submit did not begin a qualified drain");
        send(record,Stage::Waiting);
        Require(h.drains == 1, "duplicate progressed a waiting drain");
        auto changed = record; changed.goalX += 1;
        send(changed,Stage::Rejected,true);
        Require(h.drains == 1, "changed Retry progressed a waiting drain");
        h.drain = OwnershipCheck::Ready;
        send(record,Stage::Waiting,true);
        Require(h.drains == 2, "exact Retry did not resume the waiting drain");
    } else {
        Require(h.drains == 0 && execution_runtime::AllowsLegacy(local), "network Submit granted ownership");
        Require(execution_runtime::RequestOwnership(record.owner) == OwnershipResult::Ready && h.drains == 1,
            "explicit local grant failed");
    }
    Require(!execution_runtime::AllowsLegacy(local), "legacy writer still permitted");
    send(record,Stage::Waiting,true); // Granted actor, but no live movement resources.
    Require(execution_runtime::RetireOwnership(record.owner) == OwnershipResult::Waiting, "local retirement failed");
    Require(execution_runtime::ReleaseOwnership(record.owner) == OwnershipResult::Waiting && h.releases == 0,
        "pending logical request bypassed release barrier");
    record.opcode = Op::Cancel; record.globalSeq = record.ownerSeq = 3; record.flags = record.mode = 0;
    record.goalX = record.goalY = record.goalZ = record.distance = 0;
    send(record,Stage::Admitted);
    Require(execution_runtime::ReleaseOwnership(record.owner) == OwnershipResult::Waiting && h.releases == 0,
        "Cancel claimed Ref release");
    record.opcode = Op::Release; record.globalSeq = record.ownerSeq = 4; send(record,Stage::Released);
    Require(execution_runtime::ReleaseOwnership(record.owner) == OwnershipResult::Ready && h.releases == 1 && h.restores == 1 &&
        execution_runtime::AllowsLegacy(local), "released command did not restore legacy capability");
    Require(execution_runtime::RequestOwnership(record.owner) == OwnershipResult::StaleOwner, "released grant resurrected");
    ++record.owner.ownerEpoch;
    const auto priorDrains = h.drains;
    Require(execution_runtime::RequestOwnership(record.owner) == OwnershipResult::Ready && h.drains == priorDrains + 1,
        "new owner incarnation did not fence old ledger owner");
    execution_runtime::Retire(); // Worker-safe revocation, no native callbacks on caller.
    gRuntimeFrozen = true;
    Require(!execution_runtime::AllowsLegacy(local) && h.restores == 1, "disconnect silently restored old writer");
    Require(execution_runtime::RequestOwnership(record.owner) == OwnershipResult::Busy, "retired scope granted ownership");
    execution_runtime::Pump();
    Require(execution_runtime::ReleaseOwnership(record.owner) == OwnershipResult::Ready && execution_runtime::AllowsLegacy(local),
        "explicit quiescent retirement failed after transport loss");
    Require(execution_runtime::ContextRetirementComplete(), "frozen cleanup did not release context barrier");
    // A new binding may start only after the old grant was released. Lifetime
    // loss then produces recovery immediately, with no next Host command.
    gRuntimeFrozen = false; ++gRuntimeExpectedScope.consumerEpoch;
    Require(EncodeControl({gRuntimeExpectedScope,ControlKind::Offer,3},control) && execution_runtime::QueueBinding(control,7),
        "new consumer bind failed");
    execution_runtime::Pump();
    Require(execution_runtime::Peek(packet) && !packet.receipt && execution_runtime::Commit(packet.token), "new consumer Ready missing");
    Require(execution_runtime::RequestOwnership(record.owner) == OwnershipResult::Ready, "new consumer grant failed");
    // A new offer waits for old native/external quiescence rather than failing
    // or advertising Ready while a retiring owner is still blocked.
    h.quiescent = false; execution_runtime::Retire(); execution_runtime::Pump();
    Require(!execution_runtime::ContextRetirementComplete(), "external worker bypassed context barrier");
    ++gRuntimeExpectedScope.consumerEpoch;
    Require(EncodeControl({gRuntimeExpectedScope,ControlKind::Offer,3},control) && execution_runtime::QueueBinding(control,7),
        "pending retirement offer rejected");
    execution_runtime::Pump();
    Require(!execution_runtime::Peek(packet) && !execution_runtime::ContextRetirementComplete(), "pending offer claimed readiness");
    h.quiescent = true; execution_runtime::Pump();
    Require(execution_runtime::Peek(packet) && !packet.receipt && execution_runtime::Commit(packet.token), "retired offer never became Ready");
    Require(execution_runtime::RequestOwnership(record.owner) == OwnershipResult::Ready, "post-retirement grant failed");
    auto staleLifetime = local; ++staleLifetime.runtimeId;
    execution_runtime::InvalidateActor(staleLifetime);
    Require(!execution_runtime::Peek(packet), "old destruction event fenced a new local incarnation");
    execution_runtime::InvalidateActor(local);
    Control recovery{};
    Require(execution_runtime::Peek(packet) && !packet.receipt &&
        DecodeControl(std::span(packet.bytes).first(packet.size),recovery) && recovery.scope == gRuntimeExpectedScope &&
        recovery.kind == ControlKind::Recovery && recovery.value == static_cast<std::uint32_t>(Recovery::NativeUnavailable) &&
        execution_runtime::Commit(packet.token), "lifetime loss did not immediately report recovery");
    execution_runtime::Pump();
    Require(!execution_runtime::AllowsLegacy(local) && execution_runtime::ReleaseOwnership(record.owner) == OwnershipResult::Unknown &&
        execution_runtime::RequestOwnership(record.owner) == OwnershipResult::Busy, "destroyed grant silently recovered");
    Require(!execution_runtime::ContextRetirementComplete(), "destroyed owner released context barrier");
    const auto status = execution_runtime::RetirementSnapshot();
    Require(status.retired && status.referencesReleased && !status.contextReleased && status.references == 0 && status.fencedOwners == 1,
        "retirement diagnostics confused Ref release with owner/context release");
}
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--command-admission"), "expected shared execution fixture path");
        CodecGuards(); PlacementCodec(); Fixture(argv[1]); SessionGuards(); ActionCoordinationGuards(); DelayedInteractionReservation(); GuidedAuthorityStopsAtComplete(); ActionSequenceScenarios(); NativeReadiness(); NativeIntentAdapter(); SchedulerControlAdapter(); PlacementAdapter(); RuntimeOwnership(argc == 3);
        std::cout << "PASS AIEX codec, canonical C++/C# fixture and qualified session guards (no game execution)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
