#include "aimp/AlienPvPLocalController.h"
#include "aimp/AlienPvPInputIsolation.h"
#include "aimp/AlienPvPMovementMath.h"

#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CrashGuard.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aimp::alien_pvp_local {
namespace {

// Certified retail Steam x86 RVAs; the caller supplies AISync's exact-build
// engine-thread context. Never scan the process for a camera or character.
constexpr std::uintptr_t kGlobalsRva=0x012F0C88U;
constexpr std::uintptr_t kCameraSetEntityRva=0x00029220U;
constexpr std::uintptr_t kCameraActivateRva=0x00032E10U;
constexpr std::uintptr_t kCharacterSkeletonOffset=0x718U;

struct CameraBinding final {
    std::uintptr_t root{}, manager{}, follow{}, head{}, previous{}, technical{}, alien{};
    std::uint32_t generation{};
    bool bound{}, firstPerson{};
};
struct LocalState final {
    CameraBinding camera{};
    std::uintptr_t character{};
    std::uint32_t generation{};
    bool moving{}, viewKeyDown{};
    std::uint64_t lastFrameMs{}, nextCameraAttemptMs{}, nextInputAttemptMs{};
};
LocalState gLocal{}; // certified game thread only

template<typename T>
bool Read(const std::uintptr_t at, T& out) noexcept {
    return at != 0U && crash_guard::TryCopy(&out,reinterpret_cast<const void*>(at),sizeof(T));
}
bool LivingCameraTarget(const std::uintptr_t character) noexcept {
    std::uintptr_t skeleton{};
    return character != 0U && Read(character + kCharacterSkeletonOffset,skeleton) && skeleton != 0U;
}
bool CurrentCameraRoot(const std::uintptr_t base, CameraBinding& binding) noexcept {
    std::uintptr_t root{}, manager{}, follow{}, head{}, current{};
    if (base == 0U || !Read(base+kGlobalsRva,root) || !root ||
        !Read(root+0x58U,manager) || !manager ||
        !Read(manager+0x1DCU,follow) || !follow ||
        !Read(manager+0x1E0U,head) || !head ||
        !Read(manager+0x1D8U,current) || !current) return false;
    binding.root=root;binding.manager=manager;
    binding.follow=follow;binding.head=head;binding.previous=current;
    return true;
}
// Original camera behaviour list uses a deterministic name hash. Only
// touch the BoneAttached target on the currently selected retail head camera.
constexpr std::uint32_t BoneAttachedHash() noexcept {
    constexpr char name[]="BoneAttached";
    std::uint32_t h{};
    for (std::size_t i=0U,j=0U;i<256U;++i,j=(j+1U)%(sizeof(name)-1U))
        h+=static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[j]))<<(i&31U);
    return h;
}
bool AttachHeadBone(const std::uintptr_t camera,const std::uintptr_t alien) noexcept {
    std::uintptr_t array{};
    std::uint32_t count{};
    if (!Read(camera+0xBCU,array) || !array ||
        !Read(camera+0xC0U,count) || count>256U) return false;
    for (std::uint32_t i=0U;i<count;++i) {
        std::uintptr_t behavior{};
        std::uint32_t type{};
        if (!Read(array+i*sizeof(std::uintptr_t),behavior) || !behavior ||
            !Read(behavior+8U,type)) return false;
        if (type!=BoneAttachedHash()) continue;
#if defined(_MSC_VER)
        __try {
            *reinterpret_cast<volatile std::uintptr_t*>(behavior+0xE4U)=alien;
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
        return false;
#endif
    }
    return false;
}
bool RetargetCamera(const std::uintptr_t base,const std::uintptr_t camera,
    const std::uintptr_t character) noexcept {
    if (!base || !camera || !LivingCameraTarget(character)) return false;
#if defined(_MSC_VER)
    __try {
        reinterpret_cast<void(__thiscall*)(void*,void*)>(base+kCameraSetEntityRva)(
            reinterpret_cast<void*>(camera),reinterpret_cast<void*>(character));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
bool CameraCall(const std::uintptr_t base,const std::uintptr_t manager,
    const std::uintptr_t camera,const std::uintptr_t character) noexcept {
    if (!manager || !RetargetCamera(base,camera,character)) return false;
#if defined(_MSC_VER)
    __try {
        static_cast<void>(reinterpret_cast<unsigned(__thiscall*)(void*,void*)>(base+kCameraActivateRva)(
            reinterpret_cast<void*>(manager),reinterpret_cast<void*>(camera)));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
// Camera restores use a fresh global-root proof and exact engine generation.
// Never activate a stored pointer after a loading-screen transition.
std::uintptr_t gCameraBase{};
void RestoreLocalCamera(const bool validWorld) noexcept {
    const auto camera=gLocal.camera;
    gLocal.camera={};
    if (!validWorld || !camera.bound || gCameraBase==0U ||
        !LivingCameraTarget(camera.technical)) { gCameraBase=0U;return; }
    CameraBinding current{};
    std::uintptr_t active{};
    if (CurrentCameraRoot(gCameraBase,current) &&
        current.root==camera.root && current.manager==camera.manager &&
        current.follow==camera.follow && current.head==camera.head) {
        // Retarget without activating: a cinematic may currently own the
        // camera manager, and we must never steal its view on PvP exit.
        static_cast<void>(AttachHeadBone(camera.head,camera.technical));
        static_cast<void>(RetargetCamera(gCameraBase,camera.head,camera.technical));
        static_cast<void>(RetargetCamera(gCameraBase,camera.follow,camera.technical));
        if (Read(camera.manager+0x1D8U,active) &&
            (active==camera.follow || active==camera.head))
            static_cast<void>(CameraCall(gCameraBase,camera.manager,
                camera.previous==camera.head?camera.head:camera.follow,camera.technical));
    }
    gCameraBase=0U;
}
bool BindCamera(const Context& context,const std::uint64_t now) noexcept {
    if (gLocal.camera.bound) return true;
    if (now<gLocal.nextCameraAttemptMs || !LivingCameraTarget(context.alien) ||
        !LivingCameraTarget(context.technicalPlayer)) return false;
    gLocal.nextCameraAttemptMs=now+1000U;
    CameraBinding camera{};
    if (!CurrentCameraRoot(context.moduleBase,camera)) return false;
    // A cinematic, loading camera or a third-party view is never hijacked.
    if (camera.previous!=camera.follow && camera.previous!=camera.head) return false;
    if (!CameraCall(context.moduleBase,camera.manager,camera.follow,context.alien)) return false;
    camera.technical=context.technicalPlayer;
    camera.alien=context.alien;
    camera.generation=context.nativeGeneration;
    camera.bound=true;
    gLocal.camera=camera;
    gCameraBase=context.moduleBase;
    return true;
}
bool GameFocused() noexcept {
    DWORD process{};
    const auto foreground=GetForegroundWindow();
    return foreground!=nullptr && GetWindowThreadProcessId(foreground,&process)!=0U &&
        process==GetCurrentProcessId();
}
bool InputAllowed(const std::uintptr_t base) noexcept {
    if (!GameFocused()) return false;
    std::uintptr_t pauseManager{};
    std::uint32_t pauseReasons{};
    if (Read(base+0x012F194CU,pauseManager) && pauseManager &&
        Read(pauseManager+4U,pauseReasons) &&
        (pauseReasons&0x00012000U)!=0U) return false;
    return true;
}
bool CameraStillOwned(const Context& context) noexcept {
    CameraBinding current{};
    std::uintptr_t active{};
    return gLocal.camera.bound && CurrentCameraRoot(context.moduleBase,current) &&
        current.root==gLocal.camera.root && current.manager==gLocal.camera.manager &&
        Read(current.manager+0x1D8U,active) &&
        (active==gLocal.camera.follow || active==gLocal.camera.head);
}
bool Down(const int key) noexcept { return (GetAsyncKeyState(key)&0x8000)!=0; }
void TickInput(const Context& context,const std::uint64_t now) noexcept {
    if (now==gLocal.lastFrameMs) return;
    gLocal.lastFrameMs=now;
    const bool focused=InputAllowed(context.moduleBase);
    if (focused && gLocal.camera.bound) {
        const bool view=Down('V');
        if (view && !gLocal.viewKeyDown) {
            auto& camera=gLocal.camera;
            const auto wanted=camera.firstPerson?camera.follow:camera.head;
            if ((camera.firstPerson || AttachHeadBone(camera.head,context.alien)) &&
                CameraCall(context.moduleBase,camera.manager,wanted,context.alien))
                camera.firstPerson=!camera.firstPerson;
        }
        gLocal.viewKeyDown=view;
    } else {
        gLocal.viewKeyDown=false;
    }

    int forward=0,side=0;
    if (focused && gLocal.camera.bound) {
        forward=static_cast<int>(Down('W'))-static_cast<int>(Down('S'));
        side=static_cast<int>(Down('D'))-static_cast<int>(Down('A'));
    }
    std::uint32_t gait=1U;
    if (focused && Down(VK_CONTROL)) gait=0U;
    else if (focused && Down(VK_SPACE)) gait=3U;
    else if (focused && Down(VK_SHIFT)) gait=2U;
    std::string error;
    if (forward==0 && side==0) {
        if (gLocal.moving) {
            static_cast<void>(character_direct_step_runtime::HoldAIIntentIdle(
                context.alien,gait,error));
            gLocal.moving=false;
        }
        return;
    }
    float yaw{};
    if (!Read(context.technicalPlayer+0x174U,yaw) || !std::isfinite(yaw))
        if (!Read(context.alien+0x174U,yaw) || !std::isfinite(yaw)) return;
    alien_pvp_movement::Plan plan{};
    if (!alien_pvp_movement::TryPlan(forward,side,yaw,gait,plan)) return;
    gLocal.moving=character_direct_step_runtime::ArmAIIntentReplication(
        context.alien,plan.worldX,plan.worldZ,plan.intensity,plan.gait,error);
}
} // namespace

bool Drives(const std::uintptr_t character,const std::uint32_t generation) noexcept {
    return character!=0U && gLocal.character==character &&
        gLocal.generation==generation && gLocal.camera.bound &&
        alien_pvp_input::Installed();
}
void Reset(const bool clearCurrentWorld) noexcept {
    // The controller does not release AISync's exclusive lease: only the lease
    // owner does that after the certified request boundary is idle.
    alien_pvp_input::SetActive(false);
    RestoreLocalCamera(clearCurrentWorld);
    if (clearCurrentWorld && gLocal.character!=0U)
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(gLocal.character));
    gLocal={};
    alien_pvp_input::Uninstall();
}
void Pump(const Context& context) noexcept {
    const bool current=context.exclusiveLease && context.moduleBase!=0U &&
        context.alien!=0U && context.technicalPlayer!=0U &&
        context.nativeGeneration!=0U && context.localPeer!=0U &&
        context.ownerPeer==context.localPeer;
    if (!current) {
        if (gLocal.character!=0U)
            Reset(context.nativeGeneration!=0U && gLocal.generation==context.nativeGeneration);
        else alien_pvp_input::SetActive(false);
        return;
    }
    if (gLocal.character!=0U && (gLocal.character!=context.alien ||
        gLocal.generation!=context.nativeGeneration))
        Reset(false);
    if (gLocal.character==0U) {
        gLocal.character=context.alien;
        gLocal.generation=context.nativeGeneration;
    }
    const auto now=GetTickCount64();
    if (!alien_pvp_input::Installed() && now>=gLocal.nextInputAttemptMs) {
        gLocal.nextInputAttemptMs=now+3000U;
        static_cast<void>(alien_pvp_input::EnsureInstalled(context.moduleBase));
    }
    if (!alien_pvp_input::Installed()) return; // Amanda input must be isolated first.
    if (gLocal.camera.bound && (gLocal.camera.alien!=context.alien ||
        gLocal.camera.technical!=context.technicalPlayer))
        RestoreLocalCamera(true);
    if (gLocal.camera.bound && !CameraStillOwned(context)) {
        // The engine selected a cinematic or another camera; never hijack it.
        alien_pvp_input::SetActive(false);
        RestoreLocalCamera(true);
    }
    const bool attached=BindCamera(context,now);
    alien_pvp_input::SetActive(attached && InputAllowed(context.moduleBase));
    if (attached) TickInput(context,now);
}
} // namespace aimp::alien_pvp_local
