#include "aimp/AlienPvPInputIsolation.h"

#include "aimp/CrashGuard.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aimp::alien_pvp_input {
namespace {

using AsyncKeyFn=SHORT(WINAPI*)(int);
using RawInputFn=UINT(WINAPI*)(HRAWINPUT,UINT,LPVOID,PUINT,UINT);
using PeekWFn=BOOL(WINAPI*)(LPMSG,HWND,UINT,UINT,UINT);
using PeekAFn=BOOL(WINAPI*)(LPMSG,HWND,UINT,UINT,UINT);
using GetWFn=BOOL(WINAPI*)(LPMSG,HWND,UINT,UINT);
using GetAFn=BOOL(WINAPI*)(LPMSG,HWND,UINT,UINT);

struct ImportSlot final {
    void** location{};
    void* original{};
    void* replacement{};
};
constexpr std::size_t kSlotCount=6U;
std::array<ImportSlot,kSlotCount> gSlots{};
std::atomic<bool> gActive{},gInstalled{};
std::uintptr_t gExecutableBase{};
AsyncKeyFn gOriginalAsync{};
RawInputFn gOriginalRaw{};
PeekWFn gOriginalPeekW{};
PeekAFn gOriginalPeekA{};
GetWFn gOriginalGetW{};
GetAFn gOriginalGetA{};

template<typename T>
bool Read(const std::uintptr_t at,T& out) noexcept {
    return at && crash_guard::TryCopy(&out,reinterpret_cast<const void*>(at),sizeof(T));
}
bool PassKey(const std::uint32_t key) noexcept {
    return key==VK_ESCAPE || key==VK_MENU || key==VK_F12 ||
        key==VK_SNAPSHOT || key==VK_LWIN || key==VK_RWIN;
}
bool Active() noexcept { return gActive.load(std::memory_order_acquire); }
void FilterMessage(LPMSG message) noexcept {
    if (!Active() || !message) return;
    if (message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST &&
        !PassKey(static_cast<std::uint32_t>(message->wParam))) {
        message->message=WM_NULL;
        return;
    }
    // Keep mouse movement for the retail view orientation, suppress only
    // interactions and attacks that otherwise belong to the technical Amanda.
    if (message->message >= WM_LBUTTONDOWN && message->message <= WM_XBUTTONDBLCLK)
        message->message=WM_NULL;
}
SHORT WINAPI HookAsync(const int key) noexcept {
    const auto original=gOriginalAsync;
    if (!original) return 0;
    return Active() && !PassKey(static_cast<std::uint32_t>(key)) ? 0 : original(key);
}
UINT WINAPI HookRaw(const HRAWINPUT input,const UINT command,
    LPVOID data,PUINT size,const UINT headerSize) noexcept {
    const auto original=gOriginalRaw;
    if (!original) return static_cast<UINT>(-1);
    const auto result=original(input,command,data,size,headerSize);
    if (!Active() || command!=RID_INPUT || !data ||
        result==static_cast<UINT>(-1) || result<sizeof(RAWINPUTHEADER)) return result;
    auto* raw=static_cast<RAWINPUT*>(data);
    if (raw->header.dwType==RIM_TYPEKEYBOARD && result>=sizeof(RAWINPUTHEADER)+sizeof(RAWKEYBOARD)) {
        if (!PassKey(raw->data.keyboard.VKey)) {
            raw->data.keyboard.Flags=RI_KEY_BREAK;
            raw->data.keyboard.VKey=0U;
            raw->data.keyboard.Message=WM_KEYUP;
        }
    } else if (raw->header.dwType==RIM_TYPEMOUSE &&
        result>=sizeof(RAWINPUTHEADER)+sizeof(RAWMOUSE)) {
        raw->data.mouse.usButtonFlags=0U;
        raw->data.mouse.usButtonData=0U;
        raw->data.mouse.ulRawButtons=0U;
        // Preserve lLastX/lLastY: only the look direction remains routed to
        // the retail player controller. The PvP pilot reads the view yaw.
    }
    return result;
}
BOOL WINAPI HookPeekW(LPMSG msg,HWND wnd,UINT first,UINT last,UINT flags) noexcept {
    const auto original=gOriginalPeekW;
    if (!original) return FALSE;
    const auto result=original(msg,wnd,first,last,flags);
    if (result) FilterMessage(msg);
    return result;
}
BOOL WINAPI HookPeekA(LPMSG msg,HWND wnd,UINT first,UINT last,UINT flags) noexcept {
    const auto original=gOriginalPeekA;
    if (!original) return FALSE;
    const auto result=original(msg,wnd,first,last,flags);
    if (result) FilterMessage(msg);
    return result;
}
BOOL WINAPI HookGetW(LPMSG msg,HWND wnd,UINT first,UINT last) noexcept {
    const auto original=gOriginalGetW;
    if (!original) return FALSE;
    const auto result=original(msg,wnd,first,last);
    if (result>0) FilterMessage(msg);
    return result;
}
BOOL WINAPI HookGetA(LPMSG msg,HWND wnd,UINT first,UINT last) noexcept {
    const auto original=gOriginalGetA;
    if (!original) return FALSE;
    const auto result=original(msg,wnd,first,last);
    if (result>0) FilterMessage(msg);
    return result;
}

bool WriteSlot(const ImportSlot& slot,void* expected,void* replacement) noexcept {
    if (!slot.location || !expected || !replacement) return false;
    DWORD previousProtection{};
    if (!VirtualProtect(slot.location,sizeof(void*),PAGE_READWRITE,&previousProtection))
        return false;
    const auto previous=InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot.location),replacement,expected);
    DWORD ignored{};
    static_cast<void>(VirtualProtect(slot.location,sizeof(void*),previousProtection,&ignored));
    return previous==expected;
}
bool Within(const std::uint32_t offset,const std::size_t length,const std::uint32_t size) noexcept {
    return offset!=0U && offset<size && length<=static_cast<std::size_t>(size-offset);
}
bool FindImports(std::uintptr_t base,std::array<ImportSlot,kSlotCount>& output) noexcept {
    IMAGE_DOS_HEADER dos{};
    if (!Read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew<0 || dos.e_lfanew>0x100000) return false;
    IMAGE_NT_HEADERS32 nt{};
    if (!Read(base+static_cast<std::uintptr_t>(dos.e_lfanew),nt) ||
        nt.Signature!=IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    const auto imageSize=nt.OptionalHeader.SizeOfImage;
    const auto imports=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!Within(imports.VirtualAddress,sizeof(IMAGE_IMPORT_DESCRIPTOR),imageSize)) return false;
    const auto user32=GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    constexpr std::array<const char*,kSlotCount> names{
        "GetAsyncKeyState","GetRawInputData","PeekMessageW",
        "PeekMessageA","GetMessageW","GetMessageA"};
    const std::array<void*,kSlotCount> callbacks{
        reinterpret_cast<void*>(&HookAsync),reinterpret_cast<void*>(&HookRaw),
        reinterpret_cast<void*>(&HookPeekW),reinterpret_cast<void*>(&HookPeekA),
        reinterpret_cast<void*>(&HookGetW),reinterpret_cast<void*>(&HookGetA)};
    // The executable is an exact-build AIMP target. Every pointer and RVA
    // remains bounded to its live PE image before reading an import name.
    for (std::size_t d=0U;d<256U;++d) {
        const auto rva=imports.VirtualAddress+
            static_cast<std::uint32_t>(d*sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (!Within(rva,sizeof(IMAGE_IMPORT_DESCRIPTOR),imageSize)) return false;
        IMAGE_IMPORT_DESCRIPTOR desc{};
        if (!Read(base+rva,desc)) return false;
        if (!desc.Name) break;
        std::array<char,16> library{};
        if (!Within(desc.Name,library.size(),imageSize)) continue;
        if (!crash_guard::TryCopy(library.data(),reinterpret_cast<const void*>(base+desc.Name),library.size()))
            continue;
        if (_stricmp(library.data(),"USER32.dll")!=0) continue;
        if (!desc.OriginalFirstThunk || !desc.FirstThunk) return false;
        for (std::size_t i=0U;i<4096U;++i) {
            const auto named=desc.OriginalFirstThunk+
                static_cast<std::uint32_t>(i*sizeof(IMAGE_THUNK_DATA32));
            const auto slot=desc.FirstThunk+
                static_cast<std::uint32_t>(i*sizeof(IMAGE_THUNK_DATA32));
            if (!Within(named,sizeof(IMAGE_THUNK_DATA32),imageSize) ||
                !Within(slot,sizeof(IMAGE_THUNK_DATA32),imageSize)) return false;
            IMAGE_THUNK_DATA32 nameThunk{},addressThunk{};
            if (!Read(base+named,nameThunk) || !Read(base+slot,addressThunk)) return false;
            if (!nameThunk.u1.AddressOfData) break;
            if (IMAGE_SNAP_BY_ORDINAL32(nameThunk.u1.Ordinal) ||
                !Within(nameThunk.u1.AddressOfData,sizeof(IMAGE_IMPORT_BY_NAME)+24U,imageSize))
                continue;
            std::array<char,64> name{};
            if (!crash_guard::TryCopy(name.data(),reinterpret_cast<const void*>(
                base+nameThunk.u1.AddressOfData+offsetof(IMAGE_IMPORT_BY_NAME,Name)),name.size()))
                continue;
            name.back()='\0';
            for (std::size_t n=0U;n<names.size();++n) {
                if (std::strcmp(name.data(),names[n])!=0) continue;
                auto* slotPointer=reinterpret_cast<void**>(base+slot);
                const auto original=reinterpret_cast<void*>(GetProcAddress(user32,names[n]));
                if (!original || addressThunk.u1.Function!=reinterpret_cast<std::uintptr_t>(original))
                    return false; // Never overwrite a different mod's hook.
                output[n]={slotPointer,original,callbacks[n]};
            }
        }
        break;
    }
    // Critical keyboard and RawInput paths must BOTH be controllable.
    // Message APIs are optional, because some executable builds omit them.
    return output[0].location!=nullptr && output[1].location!=nullptr;
}
void ResetPublished() noexcept {
    gOriginalAsync=nullptr;gOriginalRaw=nullptr;
    gOriginalPeekW=nullptr;gOriginalPeekA=nullptr;
    gOriginalGetW=nullptr;gOriginalGetA=nullptr;
}
} // namespace

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }
void SetActive(bool enabled) noexcept {
    gActive.store(enabled && Installed(),std::memory_order_release);
}
bool EnsureInstalled(const std::uintptr_t gameModule) noexcept {
    if (Installed()) return gExecutableBase==gameModule;
    if (!gameModule) return false;
    std::array<ImportSlot,kSlotCount> discovered{};
    if (!FindImports(gameModule,discovered)) return false;
    gOriginalAsync=reinterpret_cast<AsyncKeyFn>(discovered[0].original);
    gOriginalRaw=reinterpret_cast<RawInputFn>(discovered[1].original);
    gOriginalPeekW=reinterpret_cast<PeekWFn>(discovered[2].original);
    gOriginalPeekA=reinterpret_cast<PeekAFn>(discovered[3].original);
    gOriginalGetW=reinterpret_cast<GetWFn>(discovered[4].original);
    gOriginalGetA=reinterpret_cast<GetAFn>(discovered[5].original);
    for (std::size_t n=0U;n<discovered.size();++n) {
        const auto& slot=discovered[n];
        if (!slot.location) continue;
        if (!WriteSlot(slot,slot.original,slot.replacement)) {
            for (std::size_t i=0U;i<n;++i)
                if (discovered[i].location)
                    static_cast<void>(WriteSlot(discovered[i],discovered[i].replacement,discovered[i].original));
            ResetPublished();return false;
        }
    }
    gSlots=discovered;
    gExecutableBase=gameModule;
    gInstalled.store(true,std::memory_order_release);
    return true;
}
void Uninstall() noexcept {
    SetActive(false);
    if (!Installed()) return;
    for (const auto& slot:gSlots)
        if (slot.location)
            static_cast<void>(WriteSlot(slot,slot.replacement,slot.original));
    gSlots={};
    gExecutableBase=0U;
    gInstalled.store(false,std::memory_order_release);
    // Keep original API targets available for any wrapper call still in flight
    // while the game completes a previous callback.
}
} // namespace aimp::alien_pvp_input
