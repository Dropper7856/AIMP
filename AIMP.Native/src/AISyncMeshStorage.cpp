#include "aimp/AISyncMeshStorage.h"
#include "aimp/HookManager.h"
#include "aimp/CrashGuard.h"
#include "aimp/NativeResourceLease.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <vector>

namespace aimp::ai_sync::mesh_storage {
struct Snapshot {
    route_identity::ResourceKey resource{};
    std::uintptr_t manager{}, mesh{}, blob{};
    std::uint64_t load{};
    std::uint32_t bytes{};
    std::unique_ptr<native_resource::StrongLease> native;
    std::shared_ptr<const Snapshot> origin;
    bool current{};
};
namespace {
constexpr std::size_t kCapacity=64, kMaxBlob=16U*1024U*1024U;
std::uintptr_t gBase{};
std::atomic<void*> gLoadOriginal{};
std::atomic<void*> gDestroyOriginal{};
void* gLoadTrampoline{};
void* gDestroyTrampoline{};
std::mutex gGate;
std::array<std::shared_ptr<Snapshot>,kCapacity> gEntries{};
std::uint64_t gLoad{};
std::atomic<std::uint64_t> gOwned{},gRejected{},gCapacity{},gAllocFailed{};
bool gInstalled{};
EngineThread gEngineThread{};
struct Frame { std::uintptr_t manager{}; std::uint32_t state{}; std::array<char,96> level{}; };
thread_local std::array<Frame,8> gFrames{};
thread_local std::size_t gDepth{};
using Initialize=std::uint32_t(__thiscall*)(void*,const void*,std::uint32_t,std::uint32_t);
using Load=void(__thiscall*)(void*,std::uint32_t);
using Path=void(__thiscall*)(void*,char*,std::uint32_t);
using Destroy=void(__thiscall*)(void*);
template<class T> bool Read(std::uintptr_t at,T& value) noexcept {
    return crash_guard::TryCopy(&value,reinterpret_cast<const void*>(at),sizeof(value));
}
std::uint32_t U32(std::span<const std::byte> b,std::size_t at) noexcept {
    std::uint32_t v{};std::memcpy(&v,b.data()+at,4);return v;
}
bool Hash(std::span<const std::byte> bytes,route_identity::ImmutableDigest& digest) noexcept {
    BCRYPT_ALG_HANDLE algorithm{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    BCRYPT_HASH_HANDLE hash{};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0) {
        BCryptCloseAlgorithmProvider(algorithm,0);return false;
    }
    const auto updated=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
        static_cast<ULONG>(bytes.size()),0);
    const auto finished=updated>=0?BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0):updated;
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);return finished>=0;
}
bool Level(const char* text,std::uint32_t capacity,std::array<char,96>& output) noexcept {
    if(capacity==0||capacity>260)return false;
    std::array<char,260> value{};
    if(!crash_guard::TryCopy(value.data(),text,capacity))return false;
    const auto end=std::find(value.begin(),value.begin()+capacity,'\0');
    if(end==value.begin()+capacity)return false;
    std::size_t length=static_cast<std::size_t>(end-value.begin());
    for(std::size_t i=0;i<length;++i) {
        if(value[i]=='\\')value[i]='/';
        if(value[i]>='a'&&value[i]<='z')value[i]=static_cast<char>(value[i]-'a'+'A');
    }
    constexpr char prefix[]="DATA/ENV/PRODUCTION/";
    if(length<=sizeof(prefix)-1||!std::equal(std::begin(prefix),std::end(prefix)-1,value.begin()))return false;
    constexpr char suffix[]="/WORLD";
    if(length<sizeof(suffix)||!std::equal(std::begin(suffix),std::end(suffix)-1,value.begin()+length-(sizeof(suffix)-1)))return false;
    length-=sizeof(suffix)-1;
    auto begin=length;while(begin>0&&value[begin-1]!='/')--begin;
    if(length==begin||length-begin>=output.size())return false;
    std::array<char,96> candidate{};
    for(std::size_t i=begin;i<length;++i) {
        const auto c=value[i];
        if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;
        candidate[i-begin]=c;
    }
    output=candidate;return true;
}
bool Owned(std::uintptr_t mesh,std::uintptr_t blob,std::uint32_t bytes) noexcept {
    std::uintptr_t slots{},actual{},controlTable{};std::uint32_t count{},flags{},size{};
    return mesh>12 && Read(mesh-12,controlTable)&&controlTable==gBase+0xDC711CU&&
        Read(mesh+0x30,count)&&count==1&&Read(mesh+0x44,slots)&&slots&&
        Read(slots+0x30,actual)&&actual==blob&&Read(slots+0x34,size)&&size==bytes&&
        Read(slots+0x38,flags)&&flags==1;
}
void RetireManager(std::uintptr_t manager) {
    // Cache records contain metadata only. Native refs belong to external
    // leases, so static cache teardown never calls the game allocator.
    std::array<std::shared_ptr<Snapshot>,kCapacity> retired{};
    {
        const std::lock_guard lock(gGate);
        for(std::size_t i=0;i<gEntries.size();++i)if(gEntries[i]) {
            if(gEntries[i]->manager==manager)gEntries[i]->current=false;
            if(!gEntries[i]->current&&gEntries[i].use_count()==1)retired[i]=std::move(gEntries[i]);
        }
    }
}
void __fastcall DestroyDetour(void* object,void*) {
    {
        const std::lock_guard lock(gGate);
        for(const auto& item:gEntries)if(item&&item->mesh==reinterpret_cast<std::uintptr_t>(object))item->current=false;
    }
    const auto original=reinterpret_cast<Destroy>(gDestroyOriginal.load(std::memory_order_acquire));
    if(original)original(object);
}
void __fastcall LoadDetour(void* manager,void*,std::uint32_t state) {
    const auto original=reinterpret_cast<Load>(gLoadOriginal.load(std::memory_order_acquire));
    if(!original)return;
    // An unqualified worker load retains its retail ownership contract. Mask
    // a possible outer frame so it cannot accidentally inherit its metadata.
    if(!gEngineThread||!gEngineThread()) {
        const auto depth=gDepth;gDepth=0;
        struct Restore {std::size_t depth;~Restore(){gDepth=depth;}} restore{depth};
        original(manager,state);return;
    }
    RetireManager(reinterpret_cast<std::uintptr_t>(manager));
    const auto at=gDepth++;
    struct Scope {std::size_t index;~Scope(){--gDepth;if(index<gFrames.size())gFrames[index]={};}} scope{at};
    if(at<gFrames.size())gFrames[at]={reinterpret_cast<std::uintptr_t>(manager),state,{}};
    original(manager,state);
}
void __fastcall PathDetour(void* owner,void*,char* output,std::uint32_t bytes) {
    reinterpret_cast<Path>(gBase+0x38BEB0U)(owner,output,bytes);
    if(gDepth&&gDepth<=gFrames.size())static_cast<void>(Level(output,bytes,gFrames[gDepth-1].level));
}
void Publish(const Frame& frame,std::uintptr_t mesh,std::uintptr_t blob,std::uint32_t size,
    const route_identity::ResourceKey& resource) noexcept {
    if(!Owned(mesh,blob,size)) {++gRejected;return;}
    try {
        auto item=std::make_shared<Snapshot>();
        item->manager=frame.manager;item->mesh=mesh;item->blob=blob;item->bytes=size;item->resource=resource;item->current=true;
        {
            const std::lock_guard lock(gGate);
            if(gLoad==UINT64_MAX){++gCapacity;return;}
            for(auto& slot:gEntries)if(!slot){item->load=++gLoad;slot=std::move(item);++gOwned;return;}
        }
        ++gCapacity;
    }catch(...){++gAllocFailed;}
}
std::uint32_t __fastcall InitializeDetour(void* mesh,void*,const void* blob,std::uint32_t size,std::uint32_t flags) {
    const auto original=reinterpret_cast<Initialize>(gBase+0x4B690U);
    if(!gDepth||gDepth>gFrames.size()||flags!=0||size<0x64||size>kMaxBlob)return original(mesh,blob,size,flags);
    const auto frame=gFrames[gDepth-1];std::uintptr_t current{},control{},allocator{},free{};
    if(frame.level[0]==0||!Read(frame.manager+0x18,current)||current!=reinterpret_cast<std::uintptr_t>(mesh)||
        !Read(frame.manager+0x1C,control)||control+12!=current||
        !Read(gBase+0x1236788U,allocator)||allocator!=gBase+0x441B0U||
        !Read(gBase+0x123678CU,free)||free!=gBase+0x2C6100U)return original(mesh,blob,size,flags);
    bool capacity{};
    {const std::lock_guard lock(gGate);capacity=std::any_of(gEntries.begin(),gEntries.end(),[](const auto& item){return !item;});}
    if(!capacity){++gCapacity;return original(mesh,blob,size,flags);}
    std::vector<std::byte> source;
    try{source.resize(size);}catch(...){++gAllocFailed;return original(mesh,blob,size,flags);}
    std::uint32_t elements{};route_identity::ResourceKey resource{};
    if(!crash_guard::TryCopy(source.data(),blob,size)||!BlobLayout(source,elements,resource.headerTuple)) {
        ++gRejected;return original(mesh,blob,size,flags);
    }
    resource.level=frame.level;resource.state=frame.state;
    if(!Hash(source,resource.immutableDigest)){++gRejected;return original(mesh,blob,size,flags);}
    // 004551F0 installs this native allocator/free pair. 00448280 publishes
    // the ownership flag only on its success path; 0043DAA0 frees the blob.
    auto* copy=reinterpret_cast<void*(__cdecl*)(std::uint32_t,std::uint32_t)>(allocator)(size,0);
    if(!copy){++gAllocFailed;return original(mesh,blob,size,flags);}
    std::memcpy(copy,source.data(),size);
    const auto status=original(mesh,copy,size,1);
    if((status&0x80000000U)!=0) {
        // A partially owned buffer is never decremented twice. The checked
        // failure branches precede slot installation; verify before freeing.
        if(!Owned(reinterpret_cast<std::uintptr_t>(mesh),reinterpret_cast<std::uintptr_t>(copy),size))
            reinterpret_cast<void(__cdecl*)(void*)>(free)(copy);
        ++gRejected;return status;
    }
    Publish(frame,reinterpret_cast<std::uintptr_t>(mesh),reinterpret_cast<std::uintptr_t>(copy),size,resource);
    return status;
}
}
bool BlobLayout(std::span<const std::byte> blob,std::uint32_t& elements,std::array<std::uint32_t,3>& header) noexcept {
    if(blob.size()<0x64||blob.size()>kMaxBlob||U32(blob,0)!=0x444E4156U||U32(blob,4)!=7)return false;
    const std::array<std::uint32_t,8> strides{36,12,12,12,12,4,16,60};
    std::uint64_t size=0x64;
    for(std::size_t i=0;i<strides.size();++i)size+=static_cast<std::uint64_t>(U32(blob,0x18+i*4))*strides[i];
    if(size!=blob.size()||U32(blob,0x18)==0||U32(blob,0x18)>(1U<<22)||U32(blob,0x20)==0)return false;
    elements=U32(blob,0x18);header={U32(blob,8),U32(blob,12),U32(blob,16)};return true;
}
bool Acquire(std::uintptr_t manager,std::uintptr_t mesh,Lease& output) noexcept {
    if(!gEngineThread||!gEngineThread()||manager==0||mesh==0)return false;
    // The caller must own the live manager/world. Serialize with the retail
    // loader before acquiring a native strong ref; readability is not a pin.
    EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(manager));
    struct Unlock {std::uintptr_t manager;~Unlock(){LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(manager));}} unlock{manager};
    std::unique_lock lock(gGate);std::uintptr_t current{},control{};
    if(!Read(manager+0x18,current)||current!=mesh||!Read(manager+0x1C,control)||control+12!=mesh)return false;
    for(const auto& item:gEntries)if(item&&item->current&&item->manager==manager&&item->mesh==mesh&&
        Owned(mesh,item->blob,item->bytes)) {
        try {
            struct Pair {std::uintptr_t object,control;} ref{mesh,mesh-12};
            auto pin=std::make_shared<Snapshot>();
            pin->native=std::make_unique<native_resource::StrongLease>(reinterpret_cast<std::uintptr_t>(&ref),0x54);
            if(!*pin->native)return false;
            pin->origin=item;pin->load=item->load;pin->mesh=mesh;
            Lease next{pin,item->resource,mesh,item->load};
            lock.unlock();output=std::move(next);return true;
        }catch(...){return false;}
    }
    return false;
}
bool Current(const Lease& lease) noexcept {
    if(!lease.storage||!lease.storage->origin||!lease.storage->native||!*lease.storage->native||
        lease.storage->native->Object()!=lease.mesh)return false;
    const std::lock_guard lock(gGate);
    for(const auto& item:gEntries)if(item&&item.get()==lease.storage->origin.get()&&item->current&&item->load==lease.load&&
        item->mesh==lease.mesh&&item->resource==lease.resource) {
        std::uintptr_t mesh{};return Read(item->manager+0x18,mesh)&&mesh==item->mesh&&Owned(mesh,item->blob,item->bytes);
    }
    return false;
}
route_identity::Result BindOrdinary(route_identity::Registry& registry,std::uint64_t world,
    const Lease& lease,route_identity::BindingIncarnation& output) noexcept {
    using namespace route_identity;
    if(!gEngineThread||!gEngineThread()||world==0||!Current(lease))return Result::StaleBinding;
    const auto& item=*lease.storage->origin;
    CallerVerifiedBinding binding{};binding.resource=lease.resource;
    binding.owner={world,lease.load,lease.mesh,lease.load};
    auto& layout=binding.layout;
    std::uintptr_t slots{},header{},blob{};std::uint32_t count{},salt{};
    if(!Read(lease.mesh+0x48,layout.saltBits)||!Read(lease.mesh+0x4C,layout.slotBits)||
        !Read(lease.mesh+0x50,layout.elementBits)||!Read(lease.mesh+0x30,layout.slotCapacity)||
        !Read(lease.mesh+0x18,layout.elementCapacity)||!Read(lease.mesh+0x44,slots)||
        !Read(slots,salt)||!Read(slots+8,header)||!Read(slots+0x30,blob)||header!=blob||blob!=item.blob||
        !Read(header+0x18,count)||count==0||count>layout.elementCapacity)return Result::UnverifiedBinding;
    for(std::size_t i=0;i<3;++i) {
        std::uint32_t value{};
        if(!Read(header+8+i*4,value)||value!=binding.resource.headerTuple[i])return Result::UnverifiedBinding;
    }
    binding.localSlot=0;binding.localSalt=salt;binding.elementCount=count;
    binding.resourceAndLocalMeshVerified=true;binding.storageLifetimeVerified=true;
    // All element ordinals are resource-qualified. No global pair-key mapping
    // is asserted here; NativeRoute separately verifies the exact owned link
    // table, special element bit and native inverse resolver for type1 nodes.
    if(!Current(lease))return Result::StaleBinding;
    // W+18 is the configured element capacity (00440DF0). W+34 is
    // the resource hash-table bucket count, often 1 for a single-slot mesh.
    const auto bound=registry.BindCallerVerified(binding,{},output);
    if(bound!=Result::DuplicateBinding)return bound;
    // Repeated captures share this exact load incarnation. DuplicateBinding
    // deliberately returns no ticket; recover only the unique ACTIVE mapping
    // just verified by SameBinding, never mint another storage incarnation.
    ElementMapping existing{};
    const auto mapped=registry.MapElement({binding.resource,0},existing);
    if(mapped!=Result::Ok)return mapped;
    if(existing.binding.owner!=binding.owner)return Result::InvalidIncarnation;
    output=existing.binding;return Result::Ok;
}
bool Retire() noexcept {
    std::array<std::shared_ptr<Snapshot>,kCapacity> retired{};
    bool complete=true;
    {
        const std::lock_guard lock(gGate);
        for(std::size_t i=0;i<gEntries.size();++i)if(gEntries[i]) {
            gEntries[i]->current=false;
            if(gEntries[i].use_count()==1)retired[i]=std::move(gEntries[i]);
            else complete=false;
        }
    }
    return complete;
}
Counters Diagnostics() noexcept{return {gOwned.load(),gRejected.load(),gCapacity.load(),gAllocFailed.load()};}
bool Install(std::uintptr_t base,HookManager& hooks,EngineThread engineThread,std::string& error) {
    if(gInstalled||!base||!engineThread||!hooks.ExactBuildValidated())return false;
    gBase=base;gEngineThread=engineThread;const auto before=hooks.InstalledCount();
    HookRequest load{};load.name="AISync nav owned storage loader";load.target=reinterpret_cast<void*>(base+0x58CA0U);
    load.detour=reinterpret_cast<void*>(&LoadDetour);load.patchLength=7;load.relocationSafePrefix=true;
    load.trampolineOut=&gLoadTrampoline;load.publishedTrampolineOut=&gLoadOriginal;
    const auto handler=static_cast<std::uint32_t>(base+0xD1FD41U);
    load.expected.bytes={0x6A,0xFF,0x68,static_cast<std::uint8_t>(handler),static_cast<std::uint8_t>(handler>>8),
        static_cast<std::uint8_t>(handler>>16),static_cast<std::uint8_t>(handler>>24)};load.expected.mask="xxxxxxx";
    HookRequest destroy{};destroy.name="AISync nav owned storage destructor";
    destroy.target=reinterpret_cast<void*>(base+0x3DAA0U);destroy.detour=reinterpret_cast<void*>(&DestroyDetour);
    destroy.patchLength=5;destroy.relocationSafePrefix=true;destroy.trampolineOut=&gDestroyTrampoline;
    destroy.publishedTrampolineOut=&gDestroyOriginal;destroy.expected.bytes={0x53,0x55,0x56,0x8B,0xF1};destroy.expected.mask="xxxxx";
    const auto redirect=[&](const char* name,std::uintptr_t site,std::uintptr_t target,void* replacement) {
        return hooks.InstallCallRedirect({name,reinterpret_cast<void*>(base+site),reinterpret_cast<void*>(base+target),replacement},error);
    };
    if(!hooks.Install(destroy,error)||!hooks.Install(load,error)||!redirect("AISync nav immutable path",0x58D5FU,0x38BEB0U,reinterpret_cast<void*>(&PathDetour))||
        !redirect("AISync nav owned blob",0x58E4FU,0x4B690U,reinterpret_cast<void*>(&InitializeDetour))) {
        std::string rollback;if(!hooks.UninstallFrom(before,rollback))error+="; "+rollback;return false;
    }
    gInstalled=true;return true;
}
}
