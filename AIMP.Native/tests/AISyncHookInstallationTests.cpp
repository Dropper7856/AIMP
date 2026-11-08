#include "aimp/AISyncAnimation.h"
#include "aimp/AISync.h"
#include "aimp/AlienPvPNativeActionAbi.h"
#include "aimp/AlienPvPVentNode.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/AISyncExecutionPolicy.h"
#include "aimp/AISyncCombat.h"
#include "aimp/AISyncHealth.h"
#include "aimp/AISyncNavigation.h"
#include "aimp/AISyncPairRetirement.h"
#include "aimp/AISyncRouteRuntime.h"
#include "aimp/HookManager.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void Require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
T Read(const std::span<const std::byte> bytes, const std::size_t offset) {
    Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "Truncated PE data");
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

class DataImage final {
public:
    explicit DataImage(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        const std::vector<char> file{std::istreambuf_iterator<char>{stream}, {}};
        const auto raw = std::as_bytes(std::span(file));
        const auto dos = Read<IMAGE_DOS_HEADER>(raw, 0U);
        Require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0, "Invalid DOS header");
        const auto offset = static_cast<std::size_t>(dos.e_lfanew);
        const auto nt = Read<IMAGE_NT_HEADERS32>(raw, offset);
        Require(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
            nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC, "Expected x86 PE32");
        Require(nt.OptionalHeader.SizeOfImage != 0U && nt.OptionalHeader.SizeOfImage <= 0x10000000U,
            "Invalid PE image size");
        const auto sectionTable = offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
            nt.FileHeader.SizeOfOptionalHeader;
        std::vector<std::byte> mapped(nt.OptionalHeader.SizeOfImage);
        Require(nt.OptionalHeader.SizeOfHeaders <= raw.size() &&
            nt.OptionalHeader.SizeOfHeaders <= mapped.size(), "Invalid PE headers size");
        std::memcpy(mapped.data(), raw.data(), nt.OptionalHeader.SizeOfHeaders);
        for (unsigned i = 0U; i < nt.FileHeader.NumberOfSections; ++i) {
            const auto section = Read<IMAGE_SECTION_HEADER>(raw, sectionTable + i * sizeof(IMAGE_SECTION_HEADER));
            Require(section.PointerToRawData <= raw.size() && section.SizeOfRawData <= raw.size() - section.PointerToRawData &&
                section.VirtualAddress <= mapped.size() && section.SizeOfRawData <= mapped.size() - section.VirtualAddress,
                "Invalid PE section bounds");
            if (section.SizeOfRawData != 0U)
                std::memcpy(mapped.data() + section.VirtualAddress,
                    raw.data() + section.PointerToRawData, section.SizeOfRawData);
        }
        // Data only: no LoadLibrary, imports, entry point, TLS callbacks or
        // game instructions are executed. HookManager patches this private copy.
        allocation_ = VirtualAlloc(nullptr, mapped.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        Require(allocation_ != nullptr, "Unable to allocate the private PE data image");
        try {
            const auto delta = static_cast<DWORD>(Base()) - nt.OptionalHeader.ImageBase;
            Require(delta != 0U, "Test requires a rebased image to cover ASLR");
            const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            Require(directory.VirtualAddress <= mapped.size() && directory.Size <= mapped.size() - directory.VirtualAddress,
                "Invalid PE relocation directory");
            std::size_t cursor = directory.VirtualAddress;
            const auto end = cursor + directory.Size;
            Require(cursor != 0U && cursor != end, "Expected native HIGHLOW relocations");
            while (cursor < end) {
                const auto block = Read<IMAGE_BASE_RELOCATION>(mapped, cursor);
                Require(block.SizeOfBlock >= sizeof(block) && block.SizeOfBlock <= end - cursor &&
                    (block.SizeOfBlock - sizeof(block)) % sizeof(WORD) == 0U, "Invalid relocation block");
                for (std::size_t j = sizeof(block); j < block.SizeOfBlock; j += sizeof(WORD)) {
                    const auto entry = Read<WORD>(mapped, cursor + j);
                    if ((entry >> 12U) == IMAGE_REL_BASED_ABSOLUTE) continue;
                    Require((entry >> 12U) == IMAGE_REL_BASED_HIGHLOW, "Unsupported PE relocation type");
                    const auto target = static_cast<std::size_t>(block.VirtualAddress) + (entry & 0xFFFU);
                    const auto value = Read<DWORD>(mapped, target) + delta;
                    std::memcpy(mapped.data() + target, &value, sizeof(value));
                }
                cursor += block.SizeOfBlock;
            }
            std::memcpy(allocation_, mapped.data(), mapped.size());
        } catch (...) {
            VirtualFree(allocation_, 0U, MEM_RELEASE);
            throw;
        }
    }
    ~DataImage() { VirtualFree(allocation_, 0U, MEM_RELEASE); }
    DataImage(const DataImage&) = delete;
    DataImage& operator=(const DataImage&) = delete;
    std::uintptr_t Base() const noexcept { return reinterpret_cast<std::uintptr_t>(allocation_); }
private:
    void* allocation_{};
};

bool NoHumanPair(std::uintptr_t, std::uintptr_t) noexcept { return false; }
bool NoManagedCharacter(std::uintptr_t) noexcept { return false; }
bool PairRetirementScope(const aimp::ai_sync::interactions::Key&,
    const aimp::ai_sync::interactions::Binding&) noexcept { return false; }

bool RouteEngine() noexcept {return true;}
bool RouteScope(const aimp::ai_sync::transactions::Scope&) noexcept {return false;}
void TestThePlayerNativeAbi(const std::uintptr_t base) {
    // Reproduce the exact crash caller on an isolated mapped image with private
    // fake objects. Only the getter and the small 0078EAF0 store helper execute;
    // no game process, managers, resources or scripts are started.
    std::array<std::uintptr_t,64> root{},players{};
    std::array<std::uintptr_t,0x1200U/4U> player{};
    std::array<std::uintptr_t,0x1520U/4U> controller{};
    alignas(16) std::array<std::byte,0x3C00U> receiver{};
    auto localPlayer=reinterpret_cast<std::uintptr_t>(player.data());
    root[2]=reinterpret_cast<std::uintptr_t>(players.data());
    players[0x80U/4U]=reinterpret_cast<std::uintptr_t>(&localPlayer);
    players[0x84U/4U]=1U;
    player[0x1100U/4U]=reinterpret_cast<std::uintptr_t>(controller.data());
    controller[0x1514U/4U]=4U;
    auto* rootSlot=reinterpret_cast<std::uintptr_t*>(base+0x012F0C88U);
    const auto oldRoot=*rootSlot;
    *rootSlot=reinterpret_cast<std::uintptr_t>(root.data());
    DWORD oldGetter{},oldCaller{};
    Require(VirtualProtect(reinterpret_cast<void*>(base+0x38BAF0U),0x30U,PAGE_EXECUTE_READWRITE,&oldGetter)!=0,
        "Private getter executable permission");
    Require(VirtualProtect(reinterpret_cast<void*>(base+0x38EAF0U),0x60U,PAGE_EXECUTE_READ,&oldCaller)!=0,
        "Private crash caller executable permission");
    using Store=void(__thiscall*)(void*,std::uint32_t);
    const auto store=reinterpret_cast<Store>(base+0x38EAF0U);
    store(receiver.data(),6U); // fallback getter must preserve EDX=receiver
    Require(*reinterpret_cast<const std::uint32_t*>(receiver.data()+0x3BC4U)==4U,
        "THE_PLAYER hook corrupted the live native attack receiver");
    players[0x84U/4U]=0U;
    store(receiver.data(),6U);
    Require(*reinterpret_cast<const std::uint32_t*>(receiver.data()+0x3BC4U)==4U,
        "Empty player getter must not write the receiver");
    players[0x84U/4U]=1U;
    const auto getter=base+0x38BAF0U;
    std::uintptr_t returned{},preserved{};
    alignas(16) const std::array<std::uint32_t,4> simd{0x12345678U,0x87654321U,0x11223344U,0x55667788U};
    alignas(16) std::array<std::uint32_t,4> result{};
    const auto* simdInput=simd.data();auto* simdOutput=result.data();
    __asm {
        mov eax,simdInput
        movups xmm2,[eax]
        mov edx,13572468h
        mov eax,getter
        call eax
        mov returned,eax
        mov preserved,edx
        mov eax,simdOutput
        movups [eax],xmm2
    }
    Require(returned==localPlayer&&preserved==0x13572468U&&result==simd,
        "THE_PLAYER hook changed native return, EDX or untouched SIMD state");
    DWORD ignored{};
    Require(VirtualProtect(reinterpret_cast<void*>(base+0x38BAF0U),0x30U,oldGetter,&ignored)!=0&&
        VirtualProtect(reinterpret_cast<void*>(base+0x38EAF0U),0x60U,oldCaller,&ignored)!=0,
        "Private ABI page restoration");
    *rootSlot=oldRoot;
}
void TestPassageFallbackBytes(const std::uintptr_t base) {
    // Both seams are after native M+8=5, before the cycle interruption. The
    // first continuation keeps kind3 completion; neither synthesizes a return.
    for(const auto site:{0x287396U,0x2873CAU})
        Require(std::memcmp(reinterpret_cast<const void*>(base+site),"\x8B\x06\x8B\x88\x6C\x03\x00\x00",8)==0,
            "Passage scheduler seam no longer has its complete eight-byte prefix");
    Require(std::memcmp(reinterpret_cast<const void*>(base+0x28722DU),"\xC7\x40\x08\x05\x00\x00\x00",7)==0&&
        std::memcmp(reinterpret_cast<const void*>(base+0x28739EU),"\x8B\x01\x8B\x50\x34\xFF\xD2",7)==0&&
        std::memcmp(reinterpret_cast<const void*>(base+0x2873D2U),"\x8B\x01\x8B\x50\x34\xFF\xD2",7)==0&&
        std::memcmp(reinterpret_cast<const void*>(base+0x2873ADU),"\xE8\x0E\x36\xFC\xFF",5)==0&&
        std::memcmp(reinterpret_cast<const void*>(base+0x2873E1U),"\xE8\xDA\x35\xFC\xFF",5)==0&&
        std::memcmp(reinterpret_cast<const void*>(base+0x2873B2U),"\x8B\xCE\xE8\x47\xF9\xFF\xFF",7)==0&&
        std::memcmp(reinterpret_cast<const void*>(base+0x2873E6U),"\x8B\x74\x24\x2C\x85\xF6",6)==0,
        "Passage native mode, interruption, constructor or continuation contract changed");
    Require(*reinterpret_cast<const std::uint32_t*>(base+0xDBB2A0U+0x34U)==base+0x16E0U&&
        *reinterpret_cast<const std::uint32_t*>(base+0xDBB360U+0x34U)==base+0x16E0U&&
        std::memcmp(reinterpret_cast<const void*>(base+0x16E0U),"\x8A\x41\x08\xA8\x06\x75\x05\x0C\x10\x88\x41\x08\x80\x49\x08\x26\xC3",17)==0,
        "Passage interruption no longer has the exact relocated flags6/flags26 contract");
    Require(std::memcmp(reinterpret_cast<const void*>(base+0x24A9B6U),"\xC2\x08\x00",3)==0&&
        std::memcmp(reinterpret_cast<const void*>(base+0x1853U),"\xC2\x08\x00",3)==0&&
        std::memcmp(reinterpret_cast<const void*>(base+0x184AU),"\x89\x4E\x50",3)==0,
        "Passage76 wrapper/constructor lost RET8 or its copied speed selector");
}
void TestProductionInstallation(DataImage& image) {
    aimp::HookManager hooks(true, true);
    aimp::LevelGenerationTracker generations;
    aimp::GameThreadDispatcher dispatcher;
    std::uintptr_t inactiveRoot{};
    std::string error;
    const auto base = image.Base();
    aimp::alien_pvp::vent_node::Methods pilotMethods{},retailMethods{};
    const auto readMethods=+[](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
        std::memcpy(out,reinterpret_cast<const void*>(at),size);return true;
    };
    Require(aimp::alien_pvp::vent_node::PilotEntryMethods(base,nullptr,readMethods,pilotMethods),
        "Pilot entry callback contract rejected the ASLR-rebased executable");
    std::memcpy(retailMethods.data(),reinterpret_cast<const void*>(base+0xDBC498U),sizeof(retailMethods));
    for(std::size_t i=0;i<pilotMethods.size();++i)
        Require(pilotMethods[i]==(i==0x40CU/4U?base+0x9931C0U:retailMethods[i]),
            "Pilot entry changed a native method beyond THE_PLAYER exclusion geometry");
    constexpr std::array<std::uintptr_t, 8> sites{0x2579F0U, 0x2556F0U, 0x33433EU,0x287396U,0x2873CAU,0x24A930U,
        0x032300U,0x608C50U}; // noclip render-room and post-blend seams, including ASLR
    std::array<std::array<std::byte, 16>, sites.size()> original{};
    for (std::size_t i = 0; i < sites.size(); ++i)
        std::memcpy(original[i].data(), reinterpret_cast<const void*>(base + sites[i]), original[i].size());
    Require(aimp::character_direct_step_runtime::Install(base, &inactiveRoot, hooks, error),
        "Production Character installation: " + error);
    const auto characterHooks = hooks.InstalledCount();
    Require(characterHooks != 0U, "Production Character hooks missing");
    const bool installed = aimp::ai_sync::Install(base, hooks, generations, dispatcher, error);
    Require(installed, "Production AISync installation: " + error);
    for(const auto& signature:aimp::alien_pvp::action_abi::kSignatures) {
        std::array<std::uint8_t,8> actual{};
        std::memcpy(actual.data(),reinterpret_cast<const void*>(base+signature.rva),actual.size());
        if(actual!=aimp::alien_pvp::action_abi::Expected(signature,base))
            std::cerr<<"Alien ABI mismatch RVA "<<std::hex<<signature.rva<<std::dec<<'\n';
    }
    Require(aimp::alien_pvp::action_abi::Matches(base,nullptr,
        [](void*,std::uintptr_t at,void* out,std::size_t size) noexcept {
            std::memcpy(out,reinterpret_cast<const void*>(at),size);return true;
        }),"Alien native action ABI rejected the ASLR-rebased installed executable");
    Require(aimp::ai_sync::Installed() && hooks.InstalledCount() > characterHooks,
        "Production AISync did not publish readiness");
    for (const auto site : sites)
        Require(*reinterpret_cast<const std::uint8_t*>(base + site) == 0xE9U,
            "Production intention, animation, brain or passage hook missing");
    TestThePlayerNativeAbi(base);
    // Cover the Host path too: the Retail fast-return can happen to preserve
    // EDX in one optimized build and would conceal the reported defect.
    Require(dispatcher.BindEngineThread(GetCurrentThreadId(),aimp::EngineThreadSource::CharacterManagerUpdate,1U)==
        aimp::EngineThreadBindResult::Bound,"Private Host engine thread binding");
    aimp::ai_sync::Configuration host{};
    host.role=aimp::ai_sync::Role::Host;host.nativeGeneration=host.worldGeneration=1U;
    host.sessionCookie=host.localPeer=1U;
    Require(aimp::ai_sync::Configure(std::as_bytes(std::span{&host,1U})),"Private Host configuration");
    aimp::ai_sync::PumpOnEngineThread(); // no stable world, no actors/native work
    Require(aimp::ai_sync::DiagnosticsSnapshot().role==1U,"Private Host role not applied");
    const aimp::EngineRootObservation privateRoots{1U,2U,3U,4U,5U};
    for(unsigned i=0;i<aimp::LevelGenerationTracker::kStablePollsRequired;++i)
        static_cast<void>(generations.Observe(privateRoots));
    Require(generations.IsStableGeneration(1U),"Private Host generation not ready");
    TestThePlayerNativeAbi(base); // no manager runs against these synthetic roots
    Require(hooks.UninstallAll(error), "Production rollback: " + error);
    for (std::size_t i = 0; i < sites.size(); ++i)
        Require(std::memcmp(original[i].data(), reinterpret_cast<const void*>(base + sites[i]), original[i].size()) == 0,
            "Production rollback did not restore retail instructions");
    // The isolated ABI probe returned; no game thread can retain a trampoline.
    Require(hooks.ReleaseRetiredTrampolines(true, error), "Production trampoline release: " + error);
}
void TestInstallation(DataImage& image) {
    using namespace aimp::ai_sync;
    aimp::HookManager hooks(true, true);
    std::string error;
    const auto CheckCall = [&](const bool success, const char* stage) {
        Require(success, std::string(stage) + ": " + error);
    };
    const auto base = image.Base();
    std::array<std::byte, 5U> originalTimingCall{};
    std::memcpy(originalTimingCall.data(), reinterpret_cast<void*>(base + 0x0026D517U), 5U);
    constexpr std::array<std::uintptr_t, 9U> sites{
        0x003CAAC0U, 0x003C3B10U,
        0x002556F0U, 0x0024E70FU, 0x00263E00U, 0x00291220U, 0x00291000U,
        0x0026C06FU, 0x00276080U};
    std::array<std::array<std::byte, 32U>, sites.size()> original{};
    for (std::size_t i = 0U; i < sites.size(); ++i)
        std::memcpy(original[i].data(), reinterpret_cast<const void*>(base + sites[i]), original[i].size());

    auto* shotEntry = reinterpret_cast<std::uint8_t*>(base + sites[1]);
    const auto firstByte = *shotEntry;
    *shotEntry = 0x90U;
    Require(!combat::Initialize(base, error) && hooks.InstalledCount() == 0U,
        "A mismatched combat ABI must still be refused");
    *shotEntry = firstByte;
    error.clear();
    CheckCall(health::Initialize(base, error), "Health ABI");
    CheckCall(combat::Initialize(base, error), "Combat ABI");
    CheckCall(navigation::Initialize(base, error), "Navigation ABI");
    CheckCall(health::InstallDamageObserver(hooks, error), "Health installation");
    Require(hooks.InstalledCount() == 1U, "Missing health hook");
    CheckCall(combat::Install(hooks, error), "Combat installation");
    // Combat owns a call redirect (0054A3AB) plus the shot detour; only the
    // detour site is a JMP-patched prefix listed in `sites`.
    Require(hooks.InstalledCount() == 3U, "Missing shot hook");
    Require(std::memcmp(originalTimingCall.data(), reinterpret_cast<void*>(base + 0x0026D517U), 5U) == 0,
        "AISync must not install a locomotion speed correction");
    // The player graph continuation writes through EDI. Reject a changed
    // retail branch before installing any animation hook, then verify that
    // the independently checked classification site also patches/rolls back.
    auto* playerGraphBranch = reinterpret_cast<std::uint8_t*>(base + 0x0026C07EU);
    const auto graphBranchFirstByte = *playerGraphBranch;
    *playerGraphBranch = 0x90U;
    Require(!animation::Install(base, hooks,
        {&NoHumanPair, &NoManagedCharacter, nullptr, &NoManagedCharacter}, error) &&
        hooks.InstalledCount() == 3U, "A mismatched player graph branch must be refused atomically");
    *playerGraphBranch = graphBranchFirstByte;
    error.clear();
    auto* victimGraphBranch = reinterpret_cast<std::uint8_t*>(base + 0x00276090U);
    const auto victimGraphBranchFirstByte = *victimGraphBranch;
    *victimGraphBranch = 0x90U;
    Require(!animation::Install(base, hooks,
        {&NoHumanPair, &NoManagedCharacter, nullptr, &NoManagedCharacter}, error) &&
        hooks.InstalledCount() == 3U, "A mismatched victim graph branch must be refused atomically");
    *victimGraphBranch = victimGraphBranchFirstByte;
    error.clear();
    auto* scriptedEntry = reinterpret_cast<std::uint8_t*>(base + 0x2579F0U);
    const auto scriptedFirstByte = *scriptedEntry;
    *scriptedEntry = 0xE9U;
    Require(!animation::Install(base, hooks,
        {&NoHumanPair, &NoManagedCharacter, nullptr, &NoManagedCharacter}, error) &&
        hooks.InstalledCount() == 3U, "A patched scripted constructor must still be refused atomically");
    Require(error.find("RVA 0x002579F0 expected 0x56, actual 0xE9") != std::string::npos,
        "Scripted constructor refusal must identify the conflicting hook bytes");
    *scriptedEntry = scriptedFirstByte;
    error.clear();
    CheckCall(animation::Install(base, hooks,
        {&NoHumanPair, &NoManagedCharacter, nullptr, &NoManagedCharacter}, error), "Animation installation");
    Require(hooks.InstalledCount() == sites.size() + 1U, "Missing animation hooks");
    for (const auto site : sites)
        Require(*reinterpret_cast<const std::uint8_t*>(base + site) == 0xE9U, "Hook did not patch the private image");

    CheckCall(hooks.UninstallAll(error), "Hook rollback");
    for (std::size_t i = 0U; i < sites.size(); ++i)
        Require(std::memcmp(original[i].data(), reinterpret_cast<const void*>(base + sites[i]), original[i].size()) == 0,
            "Rollback did not restore the original prefix");
    Require(std::memcmp(originalTimingCall.data(), reinterpret_cast<void*>(base + 0x0026D517U), 5U) == 0,
        "Timing call rollback failed");
    Require(hooks.InstalledCount() == 0U && hooks.RetiredCount() == sites.size() + 1U, "Unexpected rollback state");
    // No detour/trampoline has been invoked; this test owns their quiescence.
    CheckCall(hooks.ReleaseRetiredTrampolines(true, error), "Trampoline release");
    Require(hooks.RetiredCount() == 0U, "Trampolines were not released");
    // Exercise the actual retirement installation, including rebased SEH
    // immediates and published originals. No detour/game code is invoked.
    constexpr std::array<std::uintptr_t,3> pairSites{0x25B190U,0x25B630U,0x2A5850U};
    std::array<std::array<std::byte,7>,3> pairOriginal{};
    for(std::size_t i=0;i<pairSites.size();++i)
        std::memcpy(pairOriginal[i].data(),reinterpret_cast<const void*>(base+pairSites[i]),7);
    CheckCall(pair_retirement::Install(base,hooks,&PairRetirementScope,error),"Pair retirement installation");
    Require(hooks.InstalledCount()==3U,"Missing pair retirement hooks");
    for(const auto site:pairSites)
        Require(*reinterpret_cast<const std::uint8_t*>(base+site)==0xE9U,"Pair retirement hook not patched");
    CheckCall(hooks.UninstallAll(error),"Pair retirement rollback");
    for(std::size_t i=0;i<pairSites.size();++i)
        Require(std::memcmp(pairOriginal[i].data(),reinterpret_cast<const void*>(base+pairSites[i]),7)==0,
            "Pair retirement rollback changed original instructions");
    CheckCall(hooks.ReleaseRetiredTrampolines(true,error),"Pair retirement trampoline release");
    // Verify the complete production native route group, including refusal
    // at EACH call boundary and rollback on a rebased data-only retail image.
    constexpr std::array<std::uintptr_t,12> routeSites{
        0x66638U,0x4D320U,0x65E35U,0x659A1U,0x607E3U,
        0x65AADU,0x65ABBU,0x65AC9U,0x65AD7U,0x65CCAU,0x65B00U,0x65C46U};
    std::array<std::array<std::byte,8>,routeSites.size()> routeOriginal{};
    for(std::size_t i=0;i<routeSites.size();++i)
        std::memcpy(routeOriginal[i].data(),reinterpret_cast<const void*>(base+routeSites[i]),8);
    for(std::size_t changed=0;changed<routeSites.size();++changed) {
        auto* entry=reinterpret_cast<std::uint8_t*>(base+routeSites[changed]);
        const auto originalOpcode=*entry;*entry=static_cast<std::uint8_t>(*entry^0xFFU);
        Require(!route_runtime::Install(base,hooks,&RouteEngine,&RouteScope,&RouteScope,error),"Changed Host path gate must refuse installation");
        Require(hooks.InstalledCount()==0,"Changed Host path gate left partial hooks");
        *entry=originalOpcode;error.clear();
        CheckCall(hooks.ReleaseRetiredTrampolines(true,error),"Rejected Host path gate trampoline release");
    }
    CheckCall(route_runtime::Install(base,hooks,&RouteEngine,&RouteScope,&RouteScope,error),"Host path observation installation");
    Require(hooks.InstalledCount()==routeSites.size(),"Missing native route capture/execution hooks");
    Require(*reinterpret_cast<const std::uint8_t*>(base+routeSites[1])==0xE9U,"Missing Host endpoint observation");
    for(std::size_t i=0;i<routeSites.size();++i)
        Require(std::memcmp(routeOriginal[i].data(),reinterpret_cast<const void*>(base+routeSites[i]),5)!=0,
            "Native route boundary was not patched");
    CheckCall(hooks.UninstallAll(error),"Route query rollback");
    for(std::size_t i=0;i<routeSites.size();++i)
        Require(std::memcmp(routeOriginal[i].data(),reinterpret_cast<const void*>(base+routeSites[i]),8)==0,"Route rollback changed retail instructions");
    CheckCall(hooks.ReleaseRetiredTrampolines(true,error),"Route query trampoline release");
}
} // namespace

int main(int argc, char** argv) {
    if ((argc != 2 && argc != 3) || !std::filesystem::is_regular_file(argv[1])) {
        std::cout << "SKIP: supply the certified AI.exe as offline PE data.\n";
        return 77;
    }
    try {
        DataImage image(argv[1]);
        TestPassageFallbackBytes(image.Base());
        if (argc == 3 && std::string_view(argv[2]) == "production") {
            TestProductionInstallation(image);
            std::cout << "PASS: production installation/rollback plus exact THE_PLAYER crash-caller ABI on private synthetic objects; no game process.\n";
            return 0;
        }
        // End_Action sets native flags|6, then skips its lethal helper when
        // E5==0. Deferred observer markers may enter only this exact branch.
        Require(std::memcmp(reinterpret_cast<const void*>(image.Base()+0x264AB2U),
            "\x80\x4E\x08\x06\x80\xBE\xE5\x00\x00\x00\x00\x74\x07",13)==0&&
            std::memcmp(reinterpret_cast<const void*>(image.Base()+0x264AC8U),"\xC2\x04\x00",3)==0,
            "Retail End_Action no longer matches the nonlethal flags6/RET4 contract");
        // 00657DA0/57 consumes a stack float and copies it to cycle+44.
        // Check the binary contract rather than trusting decompiler types.
        Require(std::memcmp(reinterpret_cast<const void*>(image.Base()+0x257DD7U),"\xF3\x0F\x10\x44\x24\x08",6)==0&&
            std::memcmp(reinterpret_cast<const void*>(image.Base()+0x257E16U),"\xF3\x0F\x11\x46\x44",5)==0&&
            std::memcmp(reinterpret_cast<const void*>(image.Base()+0x257E1EU),"\xC2\x04\x00",3)==0,
            "Retail cycle 57 no longer matches the float argument/RET4 contract");
        // Every newly intercepted zero-argument constructor and scripted
        // entry uses this complete, relocation-free six-byte prefix.
        for(std::size_t i=0;i<6U;++i)
            Require(std::memcmp(reinterpret_cast<const void*>(image.Base()+aimp::ai_sync::kExecutionTransitions[i].entryRva),
                "\x56\x8B\xF1\x8A\x46\x1A",6)==0,"Native intention constructor prefix mismatch");
        Require(std::memcmp(reinterpret_cast<const void*>(image.Base()+0x2579F0U),"\x56\x8B\xF1\x8A\x46\x1A",6)==0&&
            std::memcmp(reinterpret_cast<const void*>(image.Base()+0x257A38U),"\xC2\x08\x00",3)==0,
            "Scripted intention no longer matches the two-stack-argument RET8 contract");
        // 00440DF0 copies the configured maximum element count to W+18.
        // W+34 stores a different, rounded resource-hash bucket count.
        Require(std::memcmp(reinterpret_cast<const void*>(image.Base()+0x40E14U),"\x8B\x47\x18\x89\x46\x18",6)==0&&
            std::memcmp(reinterpret_cast<const void*>(image.Base()+0x40E6DU),"\x89\x46\x34",3)==0&&
            std::memcmp(reinterpret_cast<const void*>(image.Base()+0x40F9AU),"\x8B\x4F\x18",3)==0,
            "Retail navigation capacities no longer match the resource binding contract");
        // Data-only evidence for the neutral cycle observed in the v262 Client
        // diagnostics. 00656430 constructs precisely state1/RVA EAD858; this
        // prevents silently broadening the drain policy to arbitrary state1s.
        const auto neutral=image.Base()+0x256430U;
        Require(*reinterpret_cast<const std::uint8_t*>(neutral+0x14)==0xB2U&&
            *reinterpret_cast<const std::uint8_t*>(neutral+0x15)==1U&&
            std::memcmp(reinterpret_cast<const void*>(neutral+0x1F),"\x88\x50\x1A",3)==0&&
            std::memcmp(reinterpret_cast<const void*>(neutral+0x2E),"\xC7\x00",2)==0&&
            *reinterpret_cast<const std::uint32_t*>(neutral+0x30)==image.Base()+0xEAD858U,
            "Retail neutral cycle constructor does not match the drain contract");
        TestInstallation(image);
        std::cout << "PASS: AISync and three pair retirement hooks installed and rolled back on rebased PE data; no game code executed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
