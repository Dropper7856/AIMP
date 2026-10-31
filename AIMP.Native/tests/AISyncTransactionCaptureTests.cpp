#include "aimp/AISyncTransactionCapture.h"
#include "aimp/AISyncHostProducer.h"
#include "aimp/AISyncNativeRoute.h"
#include "aimp/AISyncMeshStorage.h"
#include "aimp/HookManager.h"
#include <Windows.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
#include <thread>
#include <set>

namespace {
using namespace aimp::ai_sync;
using namespace transaction_capture;
using namespace transactions;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
CaptureContext ContextValue() {
    CaptureContext context{};
    context.scope.session[0] = 7; context.scope.session[15] = 42;
    context.scope.authorityEpoch = 1; context.scope.worldEpoch = 2; context.scope.consumerEpoch = 1;
    context.owner.entityA = 3; context.owner.entityB = 4; context.owner.birth = 5;
    context.owner.actorClass = 1; context.owner.ownerEpoch = 6;
    return context;
}
NativeSample Point(float x) {
    NativeSample sample{}; sample.kind = 1; sample.status = 1;
    sample.goal[0] = x; sample.goal[1] = 2; sample.goal[2] = 3;
    sample.stopDistance = 0.25F; sample.known = true; sample.hasGoal = true;
    sample.hasStopDistance = true; sample.complete = true;
    return sample;
}
std::uint64_t LE(std::span<const std::uint8_t> bytes, const std::size_t offset,
    const std::size_t count) {
    Require(count <= 8 && offset + count <= bytes.size(), "invalid LE test read");
    std::uint64_t value{};
    for (std::size_t i = 0; i < count; ++i) value |= std::uint64_t{bytes[offset + i]} << (i * 8);
    return value;
}
Record TransportRecord(const TransportEnvelope& bytes) {
    Require(LE(bytes, 0, 4) == kTransportMagic && LE(bytes, 4, 2) == 1 &&
        LE(bytes, 6, 2) == 216, "wrong transport envelope header");
    Record record{};
    Require(Decode(std::span(bytes).subspan(56), record) == Result::Ok, "invalid transport canonical bytes");
    Require((record.flags & (ObservationOnly | Incomplete)) == (ObservationOnly | Incomplete),
        "transport exported complete or authoritative record");
    return record;
}
void TestTransportEncoding() {
    static_assert(kTransportEnvelopeSize == 216 && sizeof(TransportEnvelope) == 216);
    Observation observation{};
    const auto context = ContextValue();
    observation.record.scope = context.scope; observation.record.owner = context.owner;
    observation.record.globalSeq = 10; observation.record.ownerSeq = 11;
    observation.record.commandId = 12; observation.record.parameterRevision = 13;
    observation.record.flags = HasGoal; observation.record.goalX = 1.25F;
    observation.rawSequence = 0x0102030405060708ULL;
    observation.captureEpoch = 0x1112131415161718ULL;
    observation.subrequestId = 0x2122232425262728ULL;
    observation.producer = Producer::Stop; observation.operation = Operation::StopUpdate;
    observation.coverage = Coverage::UnsupportedParameters;
    constexpr std::uint64_t loss = 0x3132333435363738ULL;
    TransportEnvelope bytes{};
    Require(EncodeTransport(Stream::Finalized, observation, loss, bytes), "transport encoding failed");
    const std::array<std::uint8_t, 8> header{'A','I','T','O',1,0,216,0};
    Require(std::equal(header.begin(), header.end(), bytes.begin()), "transport header is not explicit LE");
    Require(LE(bytes, 8, 4) == 1 && LE(bytes, 12, 4) == 2 && LE(bytes, 16, 4) == 7 &&
        LE(bytes, 20, 4) == 7 && LE(bytes, 24, 8) == observation.rawSequence &&
        LE(bytes, 32, 8) == observation.captureEpoch && LE(bytes, 40, 8) == observation.subrequestId &&
        LE(bytes, 48, 8) == loss, "transport observation offsets changed");
    auto expected = observation.record; expected.flags |= ObservationOnly | Incomplete;
    CanonicalRecord canonical{};
    Require(Encode(expected, canonical) == Result::Ok &&
        std::equal(canonical.begin(), canonical.end(), bytes.begin() + 56),
        "transport did not use canonical codec");
    Require(TransportRecord(bytes).goalX == 1.25F && observation.record.flags == HasGoal,
        "transport modified source observation");
    const auto valid = bytes;
    observation.record.scope = {};
    Require(!EncodeTransport(Stream::Oracle, observation, 0, bytes) && bytes == valid,
        "invalid export changed output or bypassed canonical validation");
}
void TestTransportDefaultOff() {
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_SHADOW", nullptr);
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_TRANSPORT_SHADOW", "1");
    aimp::HookManager hooks(true, true); std::string error;
    Require(Install(0, hooks, nullptr, nullptr, error) && hooks.InstalledCount() == 0,
        "transport-only opt-in attempted native installation");
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_SHADOW", "01");
    Require(Install(0, hooks, nullptr, nullptr, error), "inexact P1 opt-in attempted installation");
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_SHADOW", nullptr);
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_TRANSPORT_SHADOW", nullptr);
    const auto before = transaction_capture::DiagnosticsSnapshot();
    ObserveSubmit(1, true); // Disabled guard rejects before any native read.
    TransportEnvelope bytes{};
    Require(!transaction_capture::DiagnosticsSnapshot().transportEnabled && !PeekTransport(bytes) &&
        transaction_capture::DiagnosticsSnapshot().oracle == before.oracle, "transport was not default off");
    CommitTransport(); DiscardTransport();
}
struct Output {
    // No heap allocation or exceptions through the native-style sink boundary.
    std::array<Observation, 4096> raw{}, final{};
    std::size_t rawCount{}, finalCount{};
    static void Sink(void* user, Stream stream, const Observation& observation) noexcept {
        auto& out = *static_cast<Output*>(user);
        if (stream == Stream::Oracle) {
            if (out.rawCount < out.raw.size()) out.raw[out.rawCount++] = observation;
        } else if (out.finalCount < out.final.size()) out.final[out.finalCount++] = observation;
    }
    void ValidateAll() {
        const auto check = [](const Observation& observation) {
            CanonicalRecord bytes{}; Record decoded{};
            Require(Encode(observation.record, bytes) == Result::Ok, "capture produced invalid canonical record");
            Require(Decode(bytes, decoded) == Result::Ok, "capture canonical round trip failed");
            Require((decoded.flags & (ObservationOnly | Incomplete)) == (ObservationOnly | Incomplete),
                "capture claimed authoritative or complete coverage");
        };
        for (std::size_t i = 0; i < rawCount; ++i) check(raw[i]);
        for (std::size_t i = 0; i < finalCount; ++i) check(final[i]);
    }
};
struct Reader {
    NativeSample sample{Point(99)};
    bool alive{true};
    std::size_t calls{};
    static bool Read(void* user, std::uintptr_t state, std::uintptr_t request,
        NativeSample& sample, std::uint32_t& role) noexcept {
        auto& reader = *static_cast<Reader*>(user); ++reader.calls;
        if (!reader.alive || state == 0 || request == 0) return false;
        sample = reader.sample; role = request == 0x100 ? 1U : 2U; return true;
    }
};
void TestFinalizationAndCompanion() {
    auto output = std::make_unique<Output>(); auto state = std::make_unique<CaptureState>();
    state->Configure(&Output::Sink, output.get()); state->Reset();
    const auto context = ContextValue(); Reader reader{};
    const auto token = state->Begin(Producer::MoveToTarget, context, 0x200);
    state->Observe(Operation::Replace, context, 0x100, Point(1));
    state->Observe(Operation::Append, context, 0x101, {});
    Require(output->rawCount == 2 && output->finalCount == 0, "published before post-Submit producer writes");
    state->End(token, &Reader::Read, &reader);
    Require(output->finalCount == 4, "principal and companion admissions were lost");
    Require(output->raw[0].record.goalX == 1 && output->final[0].record.goalX == 99,
        "raw oracle was not independent of finalized payload");
    Require(output->final[0].record.opcode == Op::Begin && output->final[1].record.opcode == Op::Submit,
        "Begin/admission phase distinction missing");
    const auto main = output->final[0].record.commandId;
    const auto companion = output->final[2].record.commandId;
    Require(main != companion && output->final[2].record.parentCommandId == main,
        "companion incarnation/parentage missing");
    Require(output->final[0].subrequestId != output->final[2].subrequestId,
        "principal and companion share an incarnation");
    output->ValidateAll();
}
void TestNestedOrderAndAddressReuse() {
    auto output = std::make_unique<Output>(); auto state = std::make_unique<CaptureState>();
    state->Configure(&Output::Sink, output.get()); state->Reset();
    const auto context = ContextValue(); Reader reader{};
    const auto outer = state->Begin(Producer::MoveToTarget, context, 0x200);
    state->Observe(Operation::Replace, context, 0x100, Point(1));
    const auto inner = state->Begin(Producer::Stop, context, 0x201);
    state->Observe(Operation::Replace, context, 0x100, Point(1));
    state->End(inner, &Reader::Read, &reader);
    Require(output->finalCount == 0, "nested publication overtook an unfinished earlier operation");
    state->End(outer, &Reader::Read, &reader);
    Require(output->finalCount == 4, "nested actions were collapsed");
    Require(output->final[0].rawSequence < output->final[2].rawSequence,
        "observed operation order changed during nested finalization");
    Require(output->final[0].record.commandId != output->final[2].record.commandId,
        "same request address reused an old incarnation");
    Require(output->final[0].coverage == Coverage::MissingLifetime &&
        (output->final[0].record.flags & HasGoal) == 0 && reader.calls == 1,
        "old incarnation finalized from the replacement object at the same address");
    Require(output->final[2].record.parentCommandId == output->final[0].record.commandId,
        "reentrant producer lost parentage");
    output->ValidateAll();
}
void TestBriefActionsMutationsAndUnknowns() {
    auto output = std::make_unique<Output>(); auto state = std::make_unique<CaptureState>();
    state->Configure(&Output::Sink, output.get()); state->Reset();
    const auto context = ContextValue(); Reader reader{};
    auto token = state->Begin(Producer::MoveToTarget, context, 0x200);
    state->Observe(Operation::Replace, context, 0x100, Point(1));
    state->Observe(Operation::GoalUpdate, context, 0x100, Point(2));
    state->Observe(Operation::StopUpdate, context, 0x100, Point(3));
    state->Observe(Operation::Cancel, context, 0x100, Point(4));
    reader.alive = false;
    state->End(token, &Reader::Read, &reader);
    Require(output->rawCount == 4 && output->finalCount == 5, "brief action or mutation without Submit disappeared");
    Require(output->final[0].coverage == Coverage::MissingLifetime &&
        (output->final[0].record.flags & HasGoal) == 0, "borrowed request read after its lifetime ended");
    Require(reader.calls == 3, "cancelled object was read after native cancellation");
    Require(output->final[4].record.opcode == Op::Cancel, "status was used in place of explicit cancellation cause");
    const auto command = output->final[0].record.commandId;
    Require(output->final[2].record.commandId == command && output->final[3].record.commandId == command,
        "mutation without Submit invented a command incarnation");
    state->Observe(Operation::Front, context, 0x300, {});
    Require(output->raw[4].coverage == Coverage::OutsideProducer, "out-of-scope admission silently covered");
    token = state->Begin(Producer::Stop, context, 0x201);
    state->Observe(Operation::Flush, context, 0, {});
    state->End(token, &Reader::Read, &reader);
    Require(output->final[output->finalCount - 1].record.opcode == Op::CoverageLost,
        "flush invented implicit cancel-all commands");
    output->ValidateAll();
}
void TestResetAndBounds() {
    auto output = std::make_unique<Output>(); auto state = std::make_unique<CaptureState>();
    state->Configure(&Output::Sink, output.get()); state->Reset();
    const auto context = ContextValue(); Reader reader{};
    const auto token = state->Begin(Producer::MoveToTarget, context, 0x200);
    state->Observe(Operation::Replace, context, 0x100, Point(1));
    const auto oldEpoch = state->Snapshot().captureEpoch;
    state->Reset(); state->End(token, &Reader::Read, &reader);
    Require(reader.calls == 0 && output->finalCount == 0 && state->Snapshot().captureEpoch > oldEpoch,
        "reset dereferenced or finalized old native state");
    auto current = state->Begin(Producer::MoveToTarget, context, 0x200);
    for (std::size_t i = 0; i < CaptureState::kPending + 10; ++i)
        state->Observe(Operation::Replace, context, 0x1000 + i, Point(1));
    Require(state->Snapshot().oracle == CaptureState::kPending + 11 && state->Snapshot().overflow != 0,
        "bounded transaction overflow silently lost oracle evidence");
    state->End(current, &Reader::Read, &reader);
    state->Reset();
    std::array<std::uint64_t, CaptureState::kDepth + 1> tokens{};
    for (auto& value : tokens) value = state->Begin(Producer::MoveToTarget, context, 0x200);
    state->Observe(Operation::Replace, context, 0x100, Point(1));
    Require(output->raw[output->rawCount - 1].coverage == Coverage::OutsideProducer,
        "overflowed producer attributed to its parent");
    for (auto i = tokens.size(); i != 0; --i) state->End(tokens[i - 1], &Reader::Read, &reader);
    output->ValidateAll();
}

template<class T> T Read(std::span<const std::byte> bytes, std::size_t offset) {
    Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "truncated PE");
    T value{}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
class DataImage {
public:
    explicit DataImage(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<char> file{std::istreambuf_iterator<char>{input}, {}};
        const auto raw = std::as_bytes(std::span(file));
        const auto dos = Read<IMAGE_DOS_HEADER>(raw, 0);
        Require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0, "invalid DOS header");
        const auto pe = static_cast<std::size_t>(dos.e_lfanew);
        const auto nt = Read<IMAGE_NT_HEADERS32>(raw, pe);
        Require(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
            nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
            nt.OptionalHeader.SizeOfImage != 0 && nt.OptionalHeader.SizeOfImage < 0x10000000,
            "expected bounded x86 PE32");
        std::vector<std::byte> mapped(nt.OptionalHeader.SizeOfImage);
        Require(nt.OptionalHeader.SizeOfHeaders <= raw.size() && nt.OptionalHeader.SizeOfHeaders <= mapped.size(),
            "invalid PE header size");
        std::memcpy(mapped.data(), raw.data(), nt.OptionalHeader.SizeOfHeaders);
        const auto table = pe + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        for (std::size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            const auto section = Read<IMAGE_SECTION_HEADER>(raw, table + i * sizeof(IMAGE_SECTION_HEADER));
            Require(section.PointerToRawData <= raw.size() && section.SizeOfRawData <= raw.size() - section.PointerToRawData &&
                section.VirtualAddress <= mapped.size() && section.SizeOfRawData <= mapped.size() - section.VirtualAddress,
                "invalid PE section bounds");
            if (section.SizeOfRawData != 0) std::memcpy(mapped.data() + section.VirtualAddress,
                raw.data() + section.PointerToRawData, section.SizeOfRawData);
        }
        allocation_ = VirtualAlloc(nullptr, mapped.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        Require(allocation_ != nullptr, "unable to map private PE data");
        try {
            const auto delta = static_cast<DWORD>(Base()) - nt.OptionalHeader.ImageBase;
            Require(delta != 0, "test requires a rebased data image");
            const auto relocation = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            Require(relocation.VirtualAddress != 0 && relocation.VirtualAddress < mapped.size() &&
                relocation.Size <= mapped.size() - relocation.VirtualAddress, "invalid relocation directory");
            std::size_t cursor = relocation.VirtualAddress; const auto end = cursor + relocation.Size;
            while (cursor < end) {
                const auto block = Read<IMAGE_BASE_RELOCATION>(mapped, cursor);
                Require(block.SizeOfBlock >= sizeof(block) && block.SizeOfBlock <= end - cursor &&
                    (block.SizeOfBlock - sizeof(block)) % sizeof(WORD) == 0, "invalid relocation block");
                for (std::size_t j = sizeof(block); j < block.SizeOfBlock; j += sizeof(WORD)) {
                    const auto entry = Read<WORD>(mapped, cursor + j);
                    if ((entry >> 12) == IMAGE_REL_BASED_ABSOLUTE) continue;
                    Require((entry >> 12) == IMAGE_REL_BASED_HIGHLOW, "unsupported PE relocation");
                    const auto target = static_cast<std::size_t>(block.VirtualAddress) + (entry & 0xFFFU);
                    const auto value = Read<DWORD>(mapped, target) + delta;
                    std::memcpy(mapped.data() + target, &value, sizeof(value));
                }
                cursor += block.SizeOfBlock;
            }
            std::memcpy(allocation_, mapped.data(), mapped.size());
        } catch (...) { VirtualFree(allocation_, 0, MEM_RELEASE); allocation_ = nullptr; throw; }
    }
    ~DataImage() { if (allocation_ != nullptr) VirtualFree(allocation_, 0, MEM_RELEASE); }
    std::uintptr_t Base() const noexcept { return reinterpret_cast<std::uintptr_t>(allocation_); }
    DataImage(const DataImage&) = delete;
    DataImage& operator=(const DataImage&) = delete;
private:
    void* allocation_{};
};
struct TestRef { std::uintptr_t object{},control{}; };
unsigned gNativeStrong{},gNativeRelease{};
TestRef* __fastcall TestStrong(void* weak,void*,TestRef* out) noexcept {
    *out=*static_cast<TestRef*>(weak);if(out->control)InterlockedIncrement(reinterpret_cast<LONG*>(out->control+4));
    ++gNativeStrong;return out;
}
void __fastcall TestRelease(TestRef* ref,void*) noexcept {
    if(ref->control)InterlockedDecrement(reinterpret_cast<LONG*>(ref->control+4));*ref={};++gNativeRelease;
}
void TestThunk(std::uintptr_t address,std::uintptr_t target) {
    DWORD old{};Require(VirtualProtect(reinterpret_cast<void*>(address),5,PAGE_EXECUTE_READWRITE,&old)!=FALSE,"thunk protection failed");
    *reinterpret_cast<std::uint8_t*>(address)=0xE9;
    *reinterpret_cast<std::uint32_t*>(address+1)=static_cast<std::uint32_t>(target-address-5);
    Require(FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(address),5)!=FALSE,"thunk cache failed");
}
struct NavTestMesh {std::array<std::uint32_t,24> value{};std::array<std::uint32_t,16> slot{};};
std::array<NavTestMesh,4> gNavMeshes{};
std::array<std::uint8_t,0x100> gNavManager{};
std::array<std::uint8_t,148> gNavBlob{};
std::uintptr_t gNavBase{};
std::size_t gNavNext{};
unsigned gNavAllocations{},gNavFrees{},gNavInitializations{};
bool gNavFailAllocation{},gNavEngine{true};
bool NavEngine() noexcept{return gNavEngine;}
void* __cdecl NavAllocate(std::uint32_t bytes,std::uint32_t) {if(gNavFailAllocation)return nullptr;++gNavAllocations;return std::malloc(bytes);}
void __cdecl NavFree(void* ptr) {if(ptr){++gNavFrees;std::free(ptr);}}
void __fastcall NavDestroyBody(void* object,void*) {
    auto* mesh=static_cast<std::uint32_t*>(object);
    auto* slot=reinterpret_cast<std::uint32_t*>(mesh[0x44/4]);
    if(slot&&(slot[0x38/4]&1)){NavFree(reinterpret_cast<void*>(slot[0x30/4]));slot[0x30/4]=0;}
}
void __fastcall NavDestroy(void* control,void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(gNavBase+0x3DAA0U)(static_cast<std::uint8_t*>(control)+12);
}
void __fastcall NavDelete(void*,void*) {}
void __fastcall NavPath(void*,void*,char* output,std::uint32_t size) {
    constexpr char text[]="data\\env\\production\\TECH_COMMS\\WORLD";
    Require(size>=sizeof(text),"short path test buffer");std::memcpy(output,text,sizeof(text));
}
std::uint32_t __fastcall NavInitialize(void* object,void*,const void* data,std::uint32_t bytes,std::uint32_t flags) {
    ++gNavInitializations;auto* mesh=static_cast<std::uint32_t*>(object);
    auto* slot=reinterpret_cast<std::uint32_t*>(mesh[0x44/4]);
    slot[0x30/4]=reinterpret_cast<std::uint32_t>(data);slot[0x34/4]=bytes;slot[0x38/4]=flags;
    slot[0]=1;slot[2]=reinterpret_cast<std::uint32_t>(data);
    mesh[0x30/4]=1;
    // Real layout: W+34 is hash bucket count, W+18 is element capacity.
    // Different values catch the old single-element fake's accidental alias.
    mesh[0x18/4]=256;mesh[0x34/4]=1;mesh[0x48/4]=24;mesh[0x4C/4]=0;mesh[0x50/4]=8;
    if(flags==1)static_cast<std::uint8_t*>(const_cast<void*>(data))[0x64]=0xAB; // Native initialization mutates its own data.
    return 0x40000000U;
}
void __fastcall NavLoad(void* manager,void*,std::uint32_t state) {
    auto* ref=reinterpret_cast<TestRef*>(static_cast<std::uint8_t*>(manager)+0x18);
    if(ref->control&&InterlockedDecrement(reinterpret_cast<LONG*>(ref->control+4))==0)NavDestroy(reinterpret_cast<void*>(ref->control),nullptr);
    Require(gNavNext<gNavMeshes.size(),"nav test mesh pool exhausted");
    auto& item=gNavMeshes[gNavNext++];item={};item.value[0]=static_cast<std::uint32_t>(gNavBase+0xDC711CU);
    item.value[1]=item.value[2]=1;
    ref->control=reinterpret_cast<std::uintptr_t>(item.value.data());ref->object=ref->control+12;
    item.value[3+0x44/4]=reinterpret_cast<std::uint32_t>(item.slot.data());
    const auto target=[](std::uintptr_t site){return site+5+*reinterpret_cast<const std::int32_t*>(site+1);};
    std::array<char,260> path{};
    reinterpret_cast<void(__thiscall*)(void*,char*,std::uint32_t)>(target(gNavBase+0x58D5FU))(nullptr,path.data(),260);
    const auto result=reinterpret_cast<std::uint32_t(__thiscall*)(void*,const void*,std::uint32_t,std::uint32_t)>(
        target(gNavBase+0x58E4FU))(reinterpret_cast<void*>(ref->object),gNavBlob.data(),static_cast<std::uint32_t>(gNavBlob.size()),0);
    Require(result==0x40000000U&&state<100,"native initialization result changed");
}
__declspec(naked) void NavLoadBridge() {__asm {add esp,8} __asm {jmp NavLoad}}
__declspec(naked) void NavDestroyBridge() {__asm {pop esi} __asm {pop ebp} __asm {pop ebx} __asm {jmp NavDestroyBody}}
void TestMeshStorage(const char* path) {
    using namespace mesh_storage;
    DataImage image(path);gNavBase=image.Base();std::string error;
    gNavBlob={};const auto put=[](std::size_t at,std::uint32_t value){std::memcpy(gNavBlob.data()+at,&value,4);};
    put(0,0x444E4156);put(4,7);put(8,10);put(12,20);put(16,30);put(0x18,1);put(0x20,1);
    std::uint32_t elements{};std::array<std::uint32_t,3> header{};
    Require(BlobLayout(std::as_bytes(std::span(gNavBlob)),elements,header)&&elements==1&&header[2]==30,"nav blob layout mismatch");
    for(std::size_t n=0;n<gNavBlob.size();++n)Require(!BlobLayout(std::as_bytes(std::span(gNavBlob)).first(n),elements,header),"truncated nav blob accepted");
    put(0x18,UINT32_MAX);Require(!BlobLayout(std::as_bytes(std::span(gNavBlob)),elements,header),"overflow nav blob accepted");put(0x18,1);
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(gNavManager.data()));
    aimp::HookManager hooks(true,true);std::array<std::uintptr_t,4> sites{0x58CA0U,0x58D5FU,0x58E4FU,0x3DAA0U};
    std::array<std::array<std::uint8_t,7>,4> originals{};
    for(std::size_t i=0;i<sites.size();++i)std::memcpy(originals[i].data(),reinterpret_cast<void*>(gNavBase+sites[i]),7);
    *reinterpret_cast<std::uint8_t*>(gNavBase+sites[2])=0x90;
    Require(!Install(gNavBase,hooks,&NavEngine,error)&&hooks.InstalledCount()==0,"partial nav storage install did not roll back");
    *reinterpret_cast<std::uint8_t*>(gNavBase+sites[2])=originals[2][0];error.clear();
    Require(Install(gNavBase,hooks,&NavEngine,error)&&hooks.InstalledCount()==4,error.c_str());
    // Only test thunks plus the seven-byte PUSH/PUSH stack prefix execute.
    // No CATHODE allocator, loader, initialization or destructor body executes.
    TestThunk(gNavBase+0x58CA7U,reinterpret_cast<std::uintptr_t>(&NavLoadBridge));
    TestThunk(gNavBase+0x3DAA5U,reinterpret_cast<std::uintptr_t>(&NavDestroyBridge));
    TestThunk(gNavBase+0x38BEB0U,reinterpret_cast<std::uintptr_t>(&NavPath));
    TestThunk(gNavBase+0x4B690U,reinterpret_cast<std::uintptr_t>(&NavInitialize));
    TestThunk(gNavBase+0x441B0U,reinterpret_cast<std::uintptr_t>(&NavAllocate));
    TestThunk(gNavBase+0x2C6100U,reinterpret_cast<std::uintptr_t>(&NavFree));
    *reinterpret_cast<std::uintptr_t*>(gNavBase+0x1236788U)=gNavBase+0x441B0U;
    *reinterpret_cast<std::uintptr_t*>(gNavBase+0x123678CU)=gNavBase+0x2C6100U;
    *reinterpret_cast<std::uintptr_t*>(gNavBase+0xDC711CU)=reinterpret_cast<std::uintptr_t>(&NavDestroy);
    *reinterpret_cast<std::uintptr_t*>(gNavBase+0xDC7120U)=reinterpret_cast<std::uintptr_t>(&NavDelete);
    const auto load=reinterpret_cast<void(__thiscall*)(void*,std::uint32_t)>(gNavBase+0x58CA0U);
    load(gNavManager.data(),0);
    const auto ref=*reinterpret_cast<const TestRef*>(gNavManager.data()+0x18);
    Lease lease;Require(Acquire(reinterpret_cast<std::uintptr_t>(gNavManager.data()),ref.object,lease)&&Current(lease),"owned mesh lease missing");
    Require(lease.resource.level[0]=='T'&&lease.resource.state==0&&lease.resource.headerTuple[1]==20,"loader metadata changed");
    constexpr std::array<std::uint8_t,32> digest{0xBB,0x31,0x63,0x16,0x3A,0xE8,0x43,0x5F,0x1F,0xBC,0x08,0x6B,0x86,0xF3,0x6B,0x2E,
        0x4D,0xE9,0xBC,0xC8,0x2E,0xF2,0x9B,0x87,0x99,0x4B,0x4E,0x50,0x45,0xA3,0xCD,0x55};
    Require(lease.resource.immutableDigest==digest,"resource digest included native initialization mutations");
    route_identity::Registry registry;route_identity::BindingIncarnation binding{};route_identity::ElementMapping mapping{};
    Require(registry.BeginWorld(1)==route_identity::Result::Ok&&BindOrdinary(registry,1,lease,binding)==route_identity::Result::Ok&&
        registry.MapElement({lease.resource,0},mapping)==route_identity::Result::Ok&&mapping.localReference==256,"native mesh did not bind to portable ordinary ordinal");
    const auto firstBinding=binding;
    for(unsigned repeat=0;repeat<3;++repeat)
        Require(BindOrdinary(registry,1,lease,binding)==route_identity::Result::Ok&&binding==firstBinding&&
            registry.Revalidate(mapping)==route_identity::Result::Ok,"repeated route capture rejected the same live mesh binding");
    const auto blob=reinterpret_cast<const std::uint8_t*>(gNavMeshes[0].slot[0x30/4]);
    Require(blob!=gNavBlob.data()&&blob[0x64]==0xAB&&gNavBlob[0x64]==0,"native mesh still borrowed manager buffer");
    gNavBlob[0x64]=0x42;load(gNavManager.data(),1);
    Require(!Current(lease)&&gNavFrees==0,"reload freed pinned mesh blob or retained stale mapping");
    Require(BindOrdinary(registry,1,lease,binding)==route_identity::Result::StaleBinding,"old storage rebound after reload");
    lease={};load(gNavManager.data(),2);Require(gNavFrees==2,"retired owned blobs did not release through native destructor");
    gNavFailAllocation=true;load(gNavManager.data(),3);
    const auto borrowed=*reinterpret_cast<const TestRef*>(gNavManager.data()+0x18);
    Require(!Acquire(reinterpret_cast<std::uintptr_t>(gNavManager.data()),borrowed.object,lease),"allocation failure claimed an owned storage lease");
    Require(gNavInitializations==4&&gNavAllocations==3,"native initialization duplicated or allocation accounting changed");
    Require(Retire()&&gNavFrees==3,"owned storage failed to retire after all leases released");
    Require(hooks.UninstallAll(error)&&hooks.ReleaseRetiredTrampolines(true,error),"nav hook cleanup failed");
    for(std::size_t i=0;i<sites.size();++i)Require(std::memcmp(originals[i].data(),reinterpret_cast<void*>(gNavBase+sites[i]),i==0?7:5)==0,"nav storage hooks not restored");
    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(gNavManager.data()));
}
void TestTypedMovement(const char* path) {
    // Only these two five-byte TEST thunks execute in the private mapping.
    // They jump to the test-owned functions above; no retail code executes.
    DataImage image(path);const auto base=image.Base();
    TestThunk(base+0x6D8F0,reinterpret_cast<std::uintptr_t>(&TestStrong));
    TestThunk(base+0xD770,reinterpret_cast<std::uintptr_t>(&TestRelease));
    std::vector<std::uint8_t> actor(0x42B0);std::array<std::uint32_t,16> state{};std::array<std::uint32_t,0x198/4> movement{};
    std::array<std::array<std::uint32_t,64>,2> requests{};
    std::array<std::array<LONG,3>,2> controls{{{0,1,1},{0,1,1}}};
    std::array<LONG,3> movementControl{0,1,1};
    const auto character=reinterpret_cast<std::uintptr_t>(actor.data());
    const auto stateAddress=reinterpret_cast<std::uintptr_t>(state.data());
    // Unique runtime ID lets the context attest our sole synthetic actor.
    *reinterpret_cast<std::uint32_t*>(actor.data()+0x50)=19;
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x2CD0)=character;
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x42A8)=reinterpret_cast<std::uintptr_t>(movement.data());movement[0x194/4]=3;
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x42AC)=reinterpret_cast<std::uintptr_t>(movementControl.data());
    aimp::HookManager hooks(true,true);std::string error;
    const auto nativeContext=[](std::uintptr_t at,OwnerKey& owner) noexcept {
        std::uint32_t id{};
        __try{id=*reinterpret_cast<std::uint32_t*>(at+0x50);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
        if(id!=19)return false;owner=ContextValue().owner;return true;
    };
    Require(host_producer::InstallNative(base,hooks,nativeContext,[]()noexcept{return true;},
        [](const Scope&,const OwnerKey&,std::uintptr_t,std::uint32_t)noexcept{return true;},error),"Move source install failed");
    auto& producer=host_producer::EngineProducer();auto scope=ContextValue().scope;
    Require(producer.Configure({scope,execution::ControlKind::Offer,23U|execution::kProfileRoutes},1),"Move source bind failed");
    host_producer::HostPacket packet{};Require(producer.Peek(packet)&&producer.Commit(packet.token),"Move source Ready missing");
    for(std::size_t i=0;i<2;++i) {
        auto& r=requests[i];const auto address=reinterpret_cast<std::uintptr_t>(r.data());
        r[0]=static_cast<std::uint32_t>(base+(i==0?0xEB1A70U:0xEB1A50U));r[1]=address;
        r[2]=reinterpret_cast<std::uintptr_t>(controls[i].data());r[3]=1;r[4]=i==0?1U:3U;r[5]=character+0x2CD0;
        r[6]=i==0?1U:0U;const auto block=i==0?0x28U:0x1CU;
        auto* bytes=reinterpret_cast<std::uint8_t*>(r.data());
        const std::array<float,3> goal{10,20,30};std::memcpy(bytes+block,goal.data(),12);
        *reinterpret_cast<float*>(bytes+block+0x2C)=-1;*reinterpret_cast<float*>(bytes+block+0x30)=-1;
        bytes[block+0x90]=0x21;*reinterpret_cast<std::uint32_t*>(bytes+block+0x50)=2;
    }
    const auto primary=reinterpret_cast<std::uintptr_t>(requests[0].data());
    const auto companion=reinterpret_cast<std::uintptr_t>(requests[1].data());
    host_producer::BeginNativeMove(character,stateAddress);
    host_producer::BeforeNativeAdmission(primary);host_producer::ObserveNativeAdmission(primary,true);
    // An unbound nested producer must mask its parent and not finalize it.
    host_producer::BeginNativeMove(character+1,stateAddress);host_producer::EndNativeMove();
    auto* mainBytes=reinterpret_cast<std::uint8_t*>(requests[0].data());
    mainBytes[0xB8]=5;mainBytes[0xB9]=0x41;*reinterpret_cast<float*>(mainBytes+0x58)=.5F;
    // The native kind3 constructor preserves unrelated upper bits of B8.
    // Only its three authored subclass flags belong to this wire contract.
    reinterpret_cast<std::uint8_t*>(requests[1].data())[0xB8]=0xFA;
    host_producer::BeforeNativeAdmission(companion,true);host_producer::ObserveNativeAdmission(companion,true);
    requests[1][6]=1;state[0x2C/4]=primary;state[0x30/4]=reinterpret_cast<std::uintptr_t>(controls[0].data());
    state[0x34/4]=companion;state[0x38/4]=reinterpret_cast<std::uintptr_t>(controls[1].data());
    host_producer::EndNativeMove();
    execution::Batch batch{};
    Require(producer.Peek(packet)&&!packet.control&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&batch.count==2,
        "bracketed Move did not emit Begin/Submit");
    const auto& b=batch.records[1].movement;
    Require(b.navigation==movement::Navigation::HostRoute&&b.parts==2&&b.before.flags90==0x21&&b.after.flags90==5&&b.after.stopDistance==.5F&&
        b.companion.retained==0&&b.companionRetainedAfter==1&&b.companionFlags==2&&b.gait==3,"Move submission phases were flattened");
    Require(gNativeStrong==2&&controls[0][1]==2&&controls[1][1]==2&&producer.Commit(packet.token),"Move source refs not pinned");
    host_producer::BeginNativeMove(character,stateAddress);
    *reinterpret_cast<std::uint32_t*>(mainBytes+0x68)=3;mainBytes[0xB8]|=0x40;
    const std::array<float,3> pending{40,50,60};std::memcpy(mainBytes+0x34,pending.data(),12);
    host_producer::EndNativeMove();
    Require(producer.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&
        batch.count==1&&batch.records[0].opcode==Op::Update&&batch.records[0].movement.after.goal==pending&&
        producer.Commit(packet.token),"producer pending-goal update lost");
    requests[0][3]=3;host_producer::PumpNative();Require(!producer.Peek(packet),"primary finished before companion boundary");
    requests[1][3]=3;host_producer::PumpNative();
    Require(producer.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&
        batch.records[0].opcode==Op::Complete&&producer.Commit(packet.token),"Move Complete missing");
    Require(producer.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&
        batch.records[0].opcode==Op::Release&&producer.Commit(packet.token),"Move Release missing");
    Require(gNativeRelease==2&&controls[0][1]==1&&controls[1][1]==1&&host_producer::NativeIdle(),"Move observer refs leaked");
    // A different retail producer may author a standalone Move through the
    // same native admission. No 0070A940 frame or species-specific path needed.
    requests[0][3]=1;mainBytes[0xB8]=0x21;mainBytes[0xB9]=0;requests[0][0x68/4]=0;
    host_producer::BeforeNativeAdmission(primary);host_producer::PumpNative();
    Require(!producer.Peek(packet)&&gNativeRelease==2,"callback tick finalized an admission still in progress");
    host_producer::ObserveNativeAdmission(primary,true);mainBytes[0xB8]=1;host_producer::PumpNative();
    Require(producer.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&
        batch.count==2&&batch.records[1].movement.parts==1&&producer.Commit(packet.token),"generic Move admission was lost");
    requests[0][3]=3;host_producer::PumpNative();
    while(producer.Peek(packet))Require(producer.Commit(packet.token),"generic terminal drain failed");
    Require(gNativeRelease==3&&host_producer::NativeIdle(),"generic Move leaked its independent Ref");
    // The same Stop(3) primitive is also a standalone replacement, not always
    // a Move companion. It must keep its own primary identity and factory.
    requests[1][3]=1;requests[1][6]=0;
    host_producer::BeforeNativeAdmission(companion);host_producer::ObserveNativeAdmission(companion,true);
    requests[1][6]=1;host_producer::PumpNative();
    Require(producer.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&
        batch.records[1].nativeKind==3&&batch.records[1].movement.parts==1&&batch.records[1].movement.primaryFlags==2&&
        batch.records[1].movement.before.retained==0&&batch.records[1].movement.after.retained==1&&producer.Commit(packet.token),
        "standalone Stop was conflated with a companion");
    requests[1][3]=3;host_producer::PumpNative();
    while(producer.Peek(packet))Require(producer.Commit(packet.token),"standalone Stop terminal drain failed");
    Require(gNativeRelease==4&&host_producer::NativeIdle(),"standalone Stop observer Ref leaked");
    // Scalar Front preserves parameters and retention0->1 across admission.
    auto& turn=requests[0];turn[0]=base+0xEB19CC;turn[3]=1;turn[4]=4;turn[6]=0;
    const std::array<float,3> turnPoint{1.25F,-2.5F,3.75F};std::memcpy(mainBytes+0x1C,turnPoint.data(),12);
    *reinterpret_cast<float*>(mainBytes+0x2C)=.5F;turn[0x30/4]=0;turn[0x34/4]=1;mainBytes[0x38]=1;
    host_producer::BeforeNativeAdmission(primary,2);host_producer::PumpNative();
    Require(!producer.Peek(packet),"scalar admission finalized inside callback");
    host_producer::ObserveNativeAdmission(primary,true);turn[6]=1;host_producer::PumpNative();
    Require(producer.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&batch.count==2&&
        batch.records[1].nativeKind==4&&batch.records[1].admission==2&&batch.records[1].scalarRetention==2&&batch.records[1].mode==3&&
        producer.Commit(packet.token),"scalar Front retention or parameters were flattened");
    turn[3]=3;host_producer::PumpNative();
    while(producer.Peek(packet))Require(producer.Commit(packet.token),"scalar Front terminal drain failed");
    Require(gNativeRelease==5&&host_producer::NativeIdle(),"scalar observer Ref leaked");
    const auto savedTurn=turn;
    alignas(4)std::array<std::uint8_t,0x40> cycle{};
    const auto cycleAddress=reinterpret_cast<std::uintptr_t>(cycle.data());
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x36C)=cycleAddress;
    *reinterpret_cast<std::uintptr_t*>(cycle.data())=base+0xEADB58;
    *reinterpret_cast<std::uintptr_t*>(cycle.data()+4)=character;cycle[0x19]=7;cycle[0x1A]=0x3B;
    const auto makeAction=[&](std::size_t i) {
        auto& r=requests[i];r={};const auto address=reinterpret_cast<std::uintptr_t>(r.data());
        r[0]=base+0xEB198C;r[1]=address;r[2]=reinterpret_cast<std::uintptr_t>(controls[i].data());
        r[3]=1;r[4]=5;r[5]=character+0x2CD0;r[6]=1;r[7]=0x3B;
    };
    makeAction(0);host_producer::BeforeNativeAdmission(primary);host_producer::ObserveNativeAdmission(primary,true);
    action_correlation::Reference reference{};
    Require(!host_producer::IdentifyNativeAction(character,cycleAddress,0xEADB58,0x3B,7,9,reference),"unpublished command acquired action reference");
    host_producer::PumpNative();
    Require(!host_producer::IdentifyNativeAction(character,cycleAddress,0xEADB58,0x3B,7,9,reference),"admission alone acquired action reference");
    // Association happens only when the scheduler executes its head request.
    std::array<std::uintptr_t,3> schedulerHead{},schedulerFirst{};
    schedulerHead[0]=reinterpret_cast<std::uintptr_t>(schedulerFirst.data());schedulerFirst[2]=reinterpret_cast<std::uintptr_t>(requests[0].data());
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x2CD4)=reinterpret_cast<std::uintptr_t>(schedulerHead.data());
    *reinterpret_cast<std::uint32_t*>(actor.data()+0x2CD8)=1;
    host_producer::ObserveNativeActionExecution(character+0x2CD0);
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x2CD4)=0;*reinterpret_cast<std::uint32_t*>(actor.data()+0x2CD8)=0;
    Require(host_producer::IdentifyNativeAction(character,cycleAddress,0xEADB58,0x3B,7,9,reference)&&reference.nativeKind==5&&reference.executionRevision==9,
        "exact pre-admission action observation did not correlate");
    const auto retained=reference;
    Require(!host_producer::IdentifyNativeAction(character,cycleAddress,0xEADB58,0x42,7,9,reference)&&reference==retained,"wrong cycle state acquired reference");
    cycle[0x19]=8;Require(!host_producer::IdentifyNativeAction(character,cycleAddress,0xEADB58,0x3B,8,9,reference),"changed cycle incarnation acquired old reference");
    cycle[0x19]=7;Require(!host_producer::IdentifyNativeAction(character,cycleAddress,0xEADB58,0x3B,7,9,reference),"revoked cycle reference resurrected");
    requests[0][3]=3;host_producer::PumpNative();while(producer.Peek(packet))Require(producer.Commit(packet.token),"action observation drain failed");
    makeAction(0);makeAction(1);
    for(const auto request:{primary,companion}) {host_producer::BeforeNativeAdmission(request);host_producer::ObserveNativeAdmission(request,true);}
    host_producer::PumpNative();
    Require(!host_producer::IdentifyNativeAction(character,cycleAddress,0xEADB58,0x3B,7,10,reference)&&reference==retained,"ambiguous reservations selected a command");
    requests[0][3]=3;host_producer::PumpNative();
    Require(!host_producer::IdentifyNativeAction(character,cycleAddress,0xEADB58,0x3B,7,10,reference),"ambiguous binding became authoritative after unrelated cleanup");
    requests[1][3]=3;host_producer::PumpNative();while(producer.Peek(packet))Require(producer.Commit(packet.token),"ambiguous action drain failed");
    Require(gNativeRelease==8&&host_producer::NativeIdle()&&producer.Bound(),"correlation changed native ownership or source coverage");
    turn=savedTurn;
    // A first safe destruction boundary releases only our observer Ref. It
    // closes coverage, never invents native Host Cancel/Complete/Release.
    turn[3]=1;host_producer::BeforeNativeAdmission(primary,2);host_producer::ObserveNativeAdmission(primary,true);
    host_producer::PumpNative();
    Require(producer.Peek(packet)&&!packet.control&&producer.Commit(packet.token),"pre-destruction admission missing");
    Require(host_producer::BeforeNativeActorDestruction(character,20)&&gNativeRelease==8,"wrong runtime consumed observer Ref");
    host_producer::BeginNativeMutation();
    Require(!host_producer::BeforeNativeActorDestruction(character,19)&&gNativeRelease==8,"setter callback released its own observer");
    host_producer::EndNativeMutation();
    Require(host_producer::BeforeNativeActorDestruction(character,19)&&gNativeRelease==9&&host_producer::NativeIdle(),
        "safe Host pre-destruction boundary leaked observer Ref");
    execution::Control closed{};
    Require(producer.Peek(packet)&&packet.control&&execution::DecodeControl(std::span(packet.bytes).first(packet.size),closed)&&
        closed.kind==execution::ControlKind::Recovery&&producer.Commit(packet.token)&&!producer.Peek(packet),
        "observer cleanup fabricated an authoritative terminal operation");
    Require(movementControl[1]==1&&controls[0][1]==1,"pre-destruction resource pin was not balanced");
    Require(hooks.UninstallAll(error)&&hooks.ReleaseRetiredTrampolines(true,error),"Move source hook cleanup failed");
}
void TestTypedControls(const char* path) {
    using namespace aimp::ai_sync;using namespace transactions;
    DataImage image(path);const auto base=image.Base();
    TestThunk(base+0x6D8F0,reinterpret_cast<std::uintptr_t>(&TestStrong));
    TestThunk(base+0xD770,reinterpret_cast<std::uintptr_t>(&TestRelease));
    std::vector<std::uint8_t> actor(0x42B0);
    const auto character=reinterpret_cast<std::uintptr_t>(actor.data()),scheduler=character+0x2CD0;
    *reinterpret_cast<std::uint32_t*>(actor.data()+0x50)=19;
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x2CD0)=character;
    aimp::HookManager hooks(true,true);std::string error;
    const auto context=[](std::uintptr_t at,OwnerKey& owner)noexcept {
        if(*reinterpret_cast<const std::uint32_t*>(at+0x50)!=19)return false;
        owner=ContextValue().owner;return true;
    };
    Require(host_producer::InstallNative(base,hooks,context,[]()noexcept{return true;},
        [](const Scope&,const OwnerKey&,std::uintptr_t,std::uint32_t)noexcept{return true;},error),"control source install failed");
    auto& producer=host_producer::EngineProducer();const auto scope=ContextValue().scope;
    Require(producer.Configure({scope,execution::ControlKind::Offer,execution::kProfileSchedulerControl|execution::kProfilePlacement},1),"control source configure failed");
    host_producer::HostPacket packet{};Require(producer.Peek(packet)&&producer.Commit(packet.token),"control Ready missing");
    unsigned runs{};
    // A configured profile alone never enables native kind8 capture.
    Require(!execution::PlacementDedupReady(),"placement gate enabled before integration proof");
    {
        std::array<std::uint32_t,16> request{};
        request[0]=static_cast<std::uint32_t>(base+scheduler_control::kTypeRva);request[4]=8;request[5]=static_cast<std::uint32_t>(scheduler);
        const auto address=reinterpret_cast<std::uintptr_t>(request.data());
        host_producer::BeforeNativeAdmission(address,0);host_producer::ObserveNativeAdmission(address,true);host_producer::PumpNative();
        Require(host_producer::NativeIdle()&&!producer.Peek(packet)&&host_producer::UncoveredKind(8)>0,"placement captured before dedup proof");
    }
    execution::SetPlacementDedupReady(true); // Test process only; no game is executed.
    for(const auto branch:{2U,255U}) {
        std::array<std::uint32_t,16> request{};
        request[0]=static_cast<std::uint32_t>(base+scheduler_control::kTypeRva);request[4]=8;
        request[5]=static_cast<std::uint32_t>(scheduler);
        reinterpret_cast<std::uint8_t*>(request.data())[0x2C]=static_cast<std::uint8_t>(branch);
        const auto address=reinterpret_cast<std::uintptr_t>(request.data());
        const auto uncovered=host_producer::UncoveredKind(8),losses=host_producer::Coverage().losses;
        host_producer::BeforeNativeAdmission(address,0);host_producer::ObserveNativeAdmission(address,true);
        host_producer::PumpNative();
        Require(producer.Bound()&&host_producer::Coverage().losses==losses&&host_producer::NativeIdle()&&
            host_producer::UncoveredKind(8)>uncovered&&!producer.Peek(packet),
            "unsupported placement branch closed source or acquired a native Ref");
    }
    for(const auto kind:{6U,8U,11U})for(const auto admission:{0U,1U,2U})for(const auto status:{3U,5U})
        for(const auto before:{0U,1U})for(const auto after:{0U,1U}) {
        std::array<std::uint32_t,16> request{};std::array<LONG,3> control{0,1,1};
        const auto address=reinterpret_cast<std::uintptr_t>(request.data());
        request[0]=static_cast<std::uint32_t>(base+scheduler_control::kTypeRva);
        request[1]=static_cast<std::uint32_t>(address);request[2]=reinterpret_cast<std::uint32_t>(control.data());
        request[3]=0;request[4]=kind;request[5]=static_cast<std::uint32_t>(scheduler);request[6]=before;
        if(kind==execution::kPlacementKind) {
            const float pose[]{1.25F,-2.5F,3.75F,-1.25F};std::memcpy(request.data()+7,pose,sizeof(pose));
            reinterpret_cast<std::uint8_t*>(request.data())[0x2C]=static_cast<std::uint8_t>(admission&1U);
        }
        host_producer::BeforeNativeAdmission(address,admission);
        request[3]=2;request[6]=after;host_producer::ObserveNativeAdmission(address,true);
        // Real00683470 receives a strong Ref and turns3/5 into6 when not
        // retained. Model that write only AFTER the production capture seam.
        request[3]=status;
        host_producer::BeforeNativeRetention(scheduler,address);
        request[3]=6;host_producer::PumpNative();
        std::vector<Op> observed;std::uint32_t retention{};
        while(producer.Peek(packet)) {
            execution::Batch batch{};
            Require(!packet.control&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch),"control batch invalid");
            for(std::size_t i=0;i<batch.count;++i) {
                const auto& operation=batch.records[i];Require(operation.nativeKind==kind,"control kind converted");
                observed.push_back(operation.opcode);
                if(operation.opcode==Op::Submit){
                    Require(operation.admission==admission,"control queue order changed");retention=operation.scalarRetention;
                    if(kind==execution::kPlacementKind)Require(operation.flags==HasGoal&&operation.mode==(admission&1U)&&
                        operation.goalX==1.25F&&operation.goalY==-2.5F&&operation.goalZ==3.75F&&operation.aux==-1.25F,
                        "kind8 capture changed authored pose/yaw/branch");
                }
            }
            Require(producer.Commit(packet.token),"control commit failed");
        }
        Require(observed==std::vector<Op>{Op::Begin,Op::Submit,status==3U?Op::Complete:Op::Cancel,Op::Release},"short control lost or terminal6 misclassified");
        Require(retention==(before|(after<<1))&&control[1]==1&&host_producer::NativeIdle(),"control retention/pin mismatch");
        ++runs;
    }
    Require(gNativeRelease==runs&&producer.Bound(),"control source leaked or closed coverage");
    execution::SetPlacementDedupReady(false);
    Require(hooks.UninstallAll(error)&&hooks.ReleaseRetiredTrampolines(true,error),"control hook rollback failed");
}
void TestTypedMarkers(const char* path) {
    DataImage image(path);const auto base=image.Base();
    TestThunk(base+0x6D8F0,reinterpret_cast<std::uintptr_t>(&TestStrong));
    TestThunk(base+0xD770,reinterpret_cast<std::uintptr_t>(&TestRelease));
    gNativeStrong=gNativeRelease=0;
    std::vector<std::uint8_t> actor(0x42B0);std::array<std::uint32_t,0x198/4> motion{};
    std::array<std::uint32_t,64> request{};std::array<LONG,3> control{0,1,1},motionControl{0,1,1};
    const auto character=reinterpret_cast<std::uintptr_t>(actor.data()),at=reinterpret_cast<std::uintptr_t>(request.data());
    *reinterpret_cast<std::uint32_t*>(actor.data()+0x50)=19;
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x2CD0)=character;
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x42A8)=reinterpret_cast<std::uintptr_t>(motion.data());
    *reinterpret_cast<std::uintptr_t*>(actor.data()+0x42AC)=reinterpret_cast<std::uintptr_t>(motionControl.data());
    request[0]=base+0xEB1A60;request[1]=at;request[2]=reinterpret_cast<std::uintptr_t>(control.data());
    request[3]=1;request[4]=2;request[5]=character+0x2CD0;request[0xB0/4]=17;
    reinterpret_cast<std::uint8_t*>(request.data())[0xB4]=1;
    aimp::HookManager hooks(true,true);std::string error;
    Require(host_producer::InstallNative(base,hooks,[](std::uintptr_t c,OwnerKey& owner)noexcept{
        if(*reinterpret_cast<std::uint32_t*>(c+0x50)!=19)return false;owner=ContextValue().owner;return true;
    },[]()noexcept{return true;},[](const Scope&,const OwnerKey&,std::uintptr_t,std::uint32_t)noexcept{return true;},error),"marker capture hooks failed");
    host_producer::SetNativeMarkerCapture([](std::uint32_t local,native_intent::Payload& p)noexcept{
        if(local!=17)return false;p={};p.words[0]=24;p.words[1]=1;p.words[2]=7;p.words[10]=1;p.words[11]=3;return true;
    });
    auto& source=host_producer::EngineProducer();const auto scope=ContextValue().scope;
    Require(source.Configure({scope,execution::ControlKind::Offer,execution::kProfileMarker|execution::kProfileRoutes},1),"marker source bind failed");
    host_producer::HostPacket packet{};Require(source.Peek(packet)&&source.Commit(packet.token),"marker Ready missing");
    host_producer::BeforeNativeAdmission(at,1);host_producer::ObserveNativeAdmission(at,true);
    host_producer::PumpNative();Require(!source.Peek(packet),"marker published uninitialized local geometry");
    auto* bytes=reinterpret_cast<std::uint8_t*>(request.data());
    const std::array<float,3> goal{12,3,-4};std::memcpy(bytes+0x1C,goal.data(),12);
    *reinterpret_cast<float*>(bytes+0x48)=1.75F;bytes[0xAC]=0x1C;
    *reinterpret_cast<float*>(bytes+0x74)=8.25F;
    host_producer::BeforeNativeMarkerInsertion(reinterpret_cast<std::uintptr_t>(motion.data())+0xC4,at+0x1C);
    request[6]=1;host_producer::PumpNative();execution::Batch batch{};
    Require(source.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&batch.count==2,
        "marker Begin/Submit missing");
    const auto initial=batch.records[1];
    Require(initial.nativeKind==2&&initial.intent.words[10]==1&&initial.intent.words[11]==3&&initial.intent.words[12]==1&&
        initial.movement.before.goal==goal&&initial.movement.before.query[0]==8.25F&&initial.movement.before.retained==0&&
        initial.movement.after.retained==1&&source.Commit(packet.token),"marker identity/authoring/retention flattened");
    host_producer::CommandIdentity identity{};std::uint32_t part{};
    Require(host_producer::IdentifyNativeNavigation(reinterpret_cast<std::uintptr_t>(motion.data())+0xC4,at+0x1C,identity,part)&&
        identity.nativeKind==2&&part==0,"marker route correlation missing");
    // A later native cursor edit must NOT become a new authoritative goal.
    *reinterpret_cast<float*>(bytes+0x1C)=999;bytes[0xB4]=0;
    host_producer::PumpNative();
    Require(source.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&batch.count==1&&
        batch.records[0].opcode==Op::Update&&batch.records[0].intent.words[13]==0&&batch.records[0].movement.after.goal==goal&&
        source.Commit(packet.token),"marker byte update replaced authored geometry");
    request[3]=3;host_producer::PumpNative();
    Require(source.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&
        batch.records[0].opcode==Op::Complete&&source.Commit(packet.token),"marker Complete missing");
    Require(source.Peek(packet)&&execution::DecodeBatch(std::span(packet.bytes).first(packet.size),scope,batch)&&
        batch.records[0].opcode==Op::Release&&source.Commit(packet.token),"marker Release missing");
    Require(gNativeStrong==1&&gNativeRelease==1&&control[1]==1&&host_producer::NativeIdle(),"marker observer Ref leaked");
    host_producer::SetNativeMarkerCapture(nullptr);
    Require(hooks.UninstallAll(error)&&hooks.ReleaseRetiredTrampolines(true,error),"marker hook rollback failed");
}
void TestTypedInstallation(const char* path, bool corrupt, bool shared = false) {
    using namespace aimp::ai_sync;
    DataImage image(path); const auto base = image.Base();
    auto registry = std::make_unique<route_identity::Registry>(); route::NativeRouteResult nativeRoute;
    Require(nativeRoute.Initialize(base,true,*registry),"native route ABI checks rejected the rebased original PE");
    aimp::HookManager hooks(true,true); std::string error;
    constexpr std::array<std::uintptr_t,11> sites{0x281810,0x28192F,0x2777E0,0x283470,0x288510,0x30A940,0x283C10,0x283550,0x27D0D0,0x30AAEA,0x362417};
    constexpr std::array<std::size_t,11> lengths{6,5,5,5,6,7,6,6,5,5,5};
    std::array<std::array<std::uint8_t,7>,11> before{};
    for (std::size_t i = 0; i < sites.size(); ++i)
        std::memcpy(before[i].data(),reinterpret_cast<const void*>(base+sites[i]),lengths[i]);
    if (corrupt) *reinterpret_cast<std::uint8_t*>(base+sites.back()) = 0x90;
    const auto context = [](std::uintptr_t,transactions::OwnerKey&) noexcept { return false; };
    const auto guard = []() noexcept { return false; };
    if (shared) {
        SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_SHADOW","1");
        Require(Install(base,hooks,[](std::uintptr_t,CaptureContext&) noexcept { return false; },guard,error),
            "shared shadow admission hook installation failed");
        SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_SHADOW",nullptr);
        Require(HasAdmissionHooks(base) && hooks.InstalledCount() == 9,"shared shadow suffix missing");
    }
    const auto retirement = [](const transactions::Scope&,const transactions::OwnerKey&,std::uintptr_t,std::uint32_t) noexcept { return false; };
    const auto installed = host_producer::InstallNative(base,hooks,context,guard,retirement,error);
    if (corrupt) {
        Require(!installed && !error.empty() && hooks.InstalledCount() == 0,"typed hook suffix did not roll back atomically");
        *reinterpret_cast<std::uint8_t*>(base+sites.back()) = before.back()[0];
    } else {
        Require(installed && hooks.InstalledCount() == (shared ? 14U : sites.size()),"typed native hook prefixes/ABI rejected");
        Require(hooks.UninstallAll(error),"typed hook uninstall failed");
    }
    for (std::size_t i = 0; i < sites.size(); ++i)
        Require(std::memcmp(before[i].data(),reinterpret_cast<const void*>(base+sites[i]),lengths[i]) == 0,
            "typed hook original bytes were not restored");
    Require(hooks.ReleaseRetiredTrampolines(true,error),"typed trampoline reclamation failed");
    Require(host_producer::NativeIdle(),"hook installation created native request ownership");
}
bool gGuardAllows{};
bool gInvalidContext{};
bool GuardFalse() noexcept { return gGuardAllows; }
bool ContextFalse(std::uintptr_t, CaptureContext& context) noexcept {
    if (!gGuardAllows) return false;
    context = ContextValue();
    if (gInvalidContext) context.scope.session = {};
    return true;
}
std::size_t DrainTransport() {
    TransportEnvelope bytes{}; std::size_t count{};
    while (PeekTransport(bytes)) { static_cast<void>(TransportRecord(bytes)); CommitTransport(); ++count; }
    return count;
}
void TestTransportQueue(const std::uintptr_t request) {
    TransportEnvelope first{}, retry{}, next{};
    Require(PeekTransport(first) && PeekTransport(retry) && first == retry,
        "peek lost or changed head under IPC backpressure");
    const auto oldEpoch = LE(first, 32, 8);
    CommitTransport();
    Require(PeekTransport(next) && LE(next, 8, 4) == 1 && LE(next, 32, 8) == oldEpoch,
        "commit did not advance exactly one observation");
    CommitTransport();
    Require(PeekTransport(next) && LE(next, 8, 4) == 0 && LE(next, 32, 8) > oldEpoch &&
        LE(next, 24, 8) > LE(first, 24, 8), "Reset relabelled old transport epochs or rewound consumer index");
    Require(DrainTransport() == 2, "diagnostic drain consumed IPC observations");

    const auto before = transaction_capture::DiagnosticsSnapshot();
    for (std::size_t i = 0; i < 520; ++i) ObserveSubmit(request, true);
    const auto full = transaction_capture::DiagnosticsSnapshot();
    Require(full.transportDropped == before.transportDropped + 16 &&
        full.queueDropped == before.queueDropped + 16, "bounded queue overflow was not counted exactly");
    Require(PeekTransport(first) && LE(first, 48, 8) == full.lossCount &&
        LE(first, 48, 8) >= 32, "envelope omitted cumulative queue loss snapshot");
    while (!DrainJSONL(128).empty()) {}
    Require(PeekTransport(retry) && retry == first, "diagnostic drain advanced IPC head");
    ObserveSubmit(request, true); // Diagnostic queue has room while transport remains full.
    Require(!DrainJSONL(128).empty() && transaction_capture::DiagnosticsSnapshot().transportDropped == full.transportDropped + 2,
        "transport saturation suppressed independent diagnostics");
    Require(DrainTransport() == 1024, "transport exceeded capacity or lost retained head");

    const auto independentlyDrained = transaction_capture::DiagnosticsSnapshot();
    for (std::size_t i = 0; i < 513; ++i) {
        ObserveSubmit(request, true);
        Require(DrainTransport() == 2, "diagnostic saturation suppressed transport capture");
    }
    const auto diagnosticFull = transaction_capture::DiagnosticsSnapshot();
    Require(diagnosticFull.queueDropped == independentlyDrained.queueDropped + 2 &&
        diagnosticFull.transportDropped == independentlyDrained.transportDropped,
        "diagnostic and IPC loss counters were coupled");
    while (!DrainJSONL(128).empty()) {}

    // A barrier discards only copied transport values, including a peeked head.
    ObserveSubmit(request, true); Require(PeekTransport(first), "barrier test has no head");
    const auto preDiscard = transaction_capture::DiagnosticsSnapshot();
    DiscardTransport(); Reset();
    Require(!PeekTransport(next) && transaction_capture::DiagnosticsSnapshot().transportDiscarded == preDiscard.transportDiscarded + 2 &&
        transaction_capture::DiagnosticsSnapshot().lossCount == preDiscard.lossCount + 2, "session barrier discard was not counted");
    ObserveSubmit(request, true); CommitTransport(); // A discarded peek cannot commit a new head.
    Require(PeekTransport(next) && LE(next, 8, 4) == 0 && LE(next, 32, 8) > LE(first, 32, 8),
        "discarded peek consumed a subsequent session head");
    Require(DrainTransport() == 2, "post-barrier observations did not resume");
    const auto diagnostic = DrainJSONL(128);
    Require(diagnostic.find("\"captureEpoch\":" + std::to_string(LE(first, 32, 8)) + ",") != std::string::npos,
        "IPC discard cleared diagnostic queue");

    // Exercise capture-map overflow through the real sink with synthetic data.
    Reset();
    std::vector<std::array<std::uint32_t, 64>> requests(CaptureState::kRequests + 4);
    for (auto& item : requests) {
        std::memcpy(item.data(), reinterpret_cast<const void*>(request), sizeof(item));
        ObserveSubmit(reinterpret_cast<std::uintptr_t>(item.data()), true);
    }
    const auto captureFull = transaction_capture::DiagnosticsSnapshot();
    Require(captureFull.overflow == diagnosticFull.overflow + 4 && PeekTransport(next) &&
        LE(next, 48, 8) == captureFull.lossCount, "capture overflow omitted from export loss");
    Require(DrainTransport() == 2 * requests.size(), "capture capacity silently suppressed oracle export");
    while (!DrainJSONL(128).empty()) {}

    Reset();
    const auto beforeInvalid = transaction_capture::DiagnosticsSnapshot();
    gInvalidContext = true; ObserveSubmit(request, true); gInvalidContext = false;
    Require(!PeekTransport(next) && transaction_capture::DiagnosticsSnapshot().unbound == beforeInvalid.unbound + 1 &&
        transaction_capture::DiagnosticsSnapshot().exportDropped == beforeInvalid.exportDropped,
        "invalid context bypassed pre-capture validation");
    ObserveSubmit(request, true);
    Require(PeekTransport(next) && LE(next, 48, 8) == beforeInvalid.lossCount,
        "subsequent envelope lost cumulative loss snapshot");
    Require(DrainTransport() == 2, "export did not recover after invalid context");
    while (!DrainJSONL(128).empty()) {}
}
void TestConcurrentDiagnosticDrains(const std::uintptr_t request) {
    while (!DrainJSONL(128).empty()) {}
    Reset();
    const auto priorLoss = transaction_capture::DiagnosticsSnapshot().queueDropped;
    std::atomic<bool> finished{};
    std::array<std::string, 2> outputs;
    const auto drain = [&](std::size_t index) {
        for (;;) {
            auto lines = DrainJSONL(8);
            outputs[index] += lines;
            if (finished.load(std::memory_order_acquire) && lines.empty()) break;
            std::this_thread::yield();
        }
    };
    std::thread first(drain, 0), second(drain, 1);
    constexpr std::size_t operations = 1000;
    for (std::size_t i = 0; i < operations; ++i) ObserveSubmit(request, true);
    finished.store(true, std::memory_order_release);
    first.join(); second.join();
    std::set<std::string> records;
    std::size_t count{};
    constexpr std::string_view key = "\"canonical\":\"";
    for (const auto& output : outputs) {
        std::size_t cursor{};
        while ((cursor = output.find(key, cursor)) != std::string::npos) {
            cursor += key.size();
            Require(output.size() - cursor >= 320, "truncated concurrent diagnostic record");
            Require(records.insert(output.substr(cursor, 320)).second, "concurrent diagnostic duplicate");
            ++count; cursor += 320;
        }
    }
    Require(count + transaction_capture::DiagnosticsSnapshot().queueDropped - priorLoss == 2 * operations,
        "concurrent drain silently lost observations or rewound the ring");
    DiscardTransport();
}

void TestInstallation(const std::filesystem::path& path, bool corrupt, const char* transportOptIn) {
    DataImage image(path); aimp::HookManager hooks(true, true); std::string error;
    const auto base = image.Base();
    constexpr std::array<std::uintptr_t, 10> sites{0x30A940,0x30B140,0x283C10,0x283550,
        0x283DB0,0x283A90,0x27D0D0,0x30AAEA,0x362417,0x283CE0};
    std::array<std::array<std::byte, 16>, sites.size()> original{};
    for (std::size_t i = 0; i < sites.size(); ++i)
        std::memcpy(original[i].data(), reinterpret_cast<void*>(base + sites[i]), original[i].size());
    // A stand-in mandatory prefix proves optional rollback leaves older hooks
    // installed. Neither this buffer nor any mapped game bytes are executed.
    auto* mandatory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Require(mandatory != nullptr, "mandatory fake hook allocation failed");
    std::memset(mandatory, 0x90, 16);
    void* trampoline{}; std::atomic<void*> published{};
    aimp::HookRequest old{"mandatory stand-in", mandatory, reinterpret_cast<void*>(&GuardFalse),
        5, {{0x90,0x90,0x90,0x90,0x90}, "xxxxx"}, true, &trampoline, &published};
    Require(hooks.Install(old, error), "mandatory stand-in install failed");
    const auto checkpoint = hooks.InstalledCount();
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_SHADOW", nullptr);
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_TRANSPORT_SHADOW", "1");
    Require(Install(base, hooks, &ContextFalse, &GuardFalse, error) && hooks.InstalledCount() == checkpoint,
        "shadow is not default-off");
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_SHADOW", "01");
    Require(Install(base, hooks, &ContextFalse, &GuardFalse, error) && hooks.InstalledCount() == checkpoint,
        "shadow accepted an inexact opt-in value");
    if (corrupt) *reinterpret_cast<std::uint8_t*>(base + sites[8]) = 0x90;
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_TRANSPORT_SHADOW", transportOptIn);
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_SHADOW", "1");
    const auto success = Install(base, hooks, &ContextFalse, &GuardFalse, error);
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_SHADOW", nullptr);
    SetEnvironmentVariableA("AIMP_AISYNC_TRANSACTION_TRANSPORT_SHADOW", nullptr);
    if (corrupt) {
        Require(!success && !error.empty() && hooks.InstalledCount() == checkpoint,
            "partial optional install did not roll back its suffix");
        *reinterpret_cast<std::uint8_t*>(base + sites[8]) = std::to_integer<std::uint8_t>(original[8][0]);
        Require(!transaction_capture::DiagnosticsSnapshot().enabled, "failed shadow install stayed enabled");
        Require(!transaction_capture::DiagnosticsSnapshot().transportEnabled, "failed shadow install enabled transport");
    } else {
        Require(success && hooks.InstalledCount() == checkpoint + 9,
            "certified shadow hooks/call redirects were not installed");
        Require(transaction_capture::DiagnosticsSnapshot().enabled && transaction_capture::DiagnosticsSnapshot().coverageIncomplete, "shadow diagnostics state incorrect");
        const bool transportEnabled = transportOptIn != nullptr && std::string(transportOptIn) == "1";
        Require(transaction_capture::DiagnosticsSnapshot().transportEnabled == transportEnabled, "transport accepted missing or inexact opt-in");
        const auto skipped = transaction_capture::DiagnosticsSnapshot().skippedGuard;
        ObserveSubmit(1, true); // Guard rejects BEFORE any native pointer read.
        Require(transaction_capture::DiagnosticsSnapshot().skippedGuard > skipped, "off-thread guard did not fail closed");
        // Synthetic local objects only. No mapped game code is executed.
        // Copied scoped values must survive Reset before diagnostics drain.
        std::vector<std::uint8_t> character(0x2D00);
        std::array<std::uint32_t, 64> request{};
        const auto characterAddress = reinterpret_cast<std::uintptr_t>(character.data());
        const auto scheduler = characterAddress + 0x2CD0;
        std::memcpy(character.data() + 0x2CD0, &characterAddress, sizeof(characterAddress));
        request[0] = static_cast<std::uint32_t>(base + 0xEB1A70);
        request[3] = 1; request[4] = 1; request[5] = static_cast<std::uint32_t>(scheduler);
        const float goal[3]{10,20,30};
        std::memcpy(reinterpret_cast<std::uint8_t*>(request.data()) + 0x28, goal, sizeof(goal));
        gGuardAllows = true;
        const auto priorEpoch = transaction_capture::DiagnosticsSnapshot().captureEpoch;
        ObserveSubmit(reinterpret_cast<std::uintptr_t>(request.data()), true);
        Reset();
        const auto newEpoch = transaction_capture::DiagnosticsSnapshot().captureEpoch;
        ObserveSubmit(reinterpret_cast<std::uintptr_t>(request.data()), true);
        const auto json = DrainJSONL(128);
        Require(priorEpoch != newEpoch &&
            json.find("\"captureEpoch\":" + std::to_string(priorEpoch) + ",") != std::string::npos &&
            json.find("\"captureEpoch\":" + std::to_string(newEpoch) + ",") != std::string::npos,
            "reset discarded copied observations before diagnostics drained them");
        Require(DrainJSONL(128).empty(), "JSONL drain duplicated observations");
        if (transportEnabled) TestTransportQueue(reinterpret_cast<std::uintptr_t>(request.data()));
        else {
            TransportEnvelope bytes{};
            Require(!PeekTransport(bytes), "P1-only capture populated IPC transport");
        }
        TestConcurrentDiagnosticDrains(reinterpret_cast<std::uintptr_t>(request.data()));
        gGuardAllows = false;
        Require(hooks.UninstallFrom(checkpoint, error), "optional suffix uninstall failed");
    }
    Require(*mandatory == 0xE9 && hooks.InstalledCount() == checkpoint,
        "shadow rollback disabled an older hook");
    for (std::size_t i = 0; i < sites.size(); ++i)
        Require(std::memcmp(original[i].data(), reinterpret_cast<void*>(base + sites[i]), original[i].size()) == 0,
            "native original bytes not restored (or duplicate 683CE0 hook installed)");
    Require(hooks.UninstallAll(error), "test mandatory hook cleanup failed");
    Require(hooks.ReleaseRetiredTrampolines(true, error), "test quiescent trampoline cleanup failed");
    VirtualFree(mandatory, 0, MEM_RELEASE);
}
}

int main(int argc, char** argv) {
    try {
        if (argc >= 3) {
            const std::string mode = argv[2];
            if(mode=="mesh-storage") {
                TestMeshStorage(argv[1]);std::cout<<"Native mesh owned storage, reload pin and hook rollback PASS (offline)\n";return 0;
            }
            if(mode=="typed-markers") {
                TestTypedMarkers(argv[1]);std::cout<<"Marker capture and retained native route identity PASS (offline)\n";return 0;
            }
            if(mode=="typed-movement") {
                TestTypedMovement(argv[1]);std::cout<<"Move source phases, pending goal, companion and observer lifetime PASS (offline)\n";return 0;
            }
            if(mode=="typed-controls") {
                TestTypedControls(argv[1]);std::cout<<"Scheduler control source admission, retention and terminal provenance PASS (offline)\n";return 0;
            }
            if (mode == "typed" || mode == "typed-rollback" || mode == "typed-shared") {
                TestTypedInstallation(argv[1],mode == "typed-rollback",mode == "typed-shared");
                std::cout << "Typed native hook installation PASS (private PE data; no game execution)\n";
                return 0;
            }
            TestInstallation(argv[1], mode == "rollback",
                mode == "transport" || mode == "rollback" ? "1" :
                (mode == "transport-inexact" ? "01" : nullptr));
        } else {
            TestTransportEncoding(); TestTransportDefaultOff();
            TestFinalizationAndCompanion(); TestNestedOrderAndAddressReuse();
            TestBriefActionsMutationsAndUnknowns(); TestResetAndBounds();
        }
        std::cout << "AISync transaction shadow capture tests passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
