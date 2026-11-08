// Donor-derived movement/parking: AI-Playable-Alien, (c) 2026 pirletik.
// MIT license: third_party/AI-Playable-Alien/LICENSE.
#include "aimp/AlienPvPLocalController.h"
#include "aimp/AlienPvPInputIsolation.h"
#include "aimp/AlienPvPVisualIsolation.h"
#include "aimp/AlienPvPControlRuntime.h"
#include "aimp/AlienPvPControlPolicy.h"
#include "aimp/AlienPvPTechnicalPolicy.h"
#include "aimp/AlienPvPVents.h"
#include "aimp/AlienPvPFreeCamera.h"

#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <atomic>

namespace aimp::alien_pvp_local {
namespace {

// Certified retail Steam x86 RVAs; the caller supplies AISync's exact-build
// engine-thread context. Never scan the process for a camera or character.
constexpr std::uintptr_t kGlobalsRva=0x012F0C88U;
constexpr std::uintptr_t kCameraSetEntityRva=0x00029220U;
constexpr std::uintptr_t kCameraActivateRva=0x00032E10U;
constexpr std::uintptr_t kCharacterSkeletonOffset=0x718U;

struct CameraBinding final {
    std::uintptr_t root{}, manager{}, follow{}, head{}, previous{}, technical{}, alien{}, technicalSkeleton{};
    std::uint32_t generation{}, technicalRuntime{}, alienRuntime{};
    std::uintptr_t alienSkeleton{};
    bool bound{}, touched{}, firstPerson{};
    std::uintptr_t followBehavior{};
    std::array<float,6> originalFollow{};
    std::uint8_t originalFirstPerson{};
    bool followSaved{};
};
struct LocalState final {
    CameraBinding camera{};
    alien_pvp::technical::Parking parking{};
    std::uintptr_t character{};
    std::uint32_t generation{};
    bool moving{},visualReady{},middleDown{};
    bool attackDown{}, enterDown{}, exitDown{}, roarDown{};
    bool ventMapOpen{},wasBackstage{},ventLeft{},ventRight{},ventConfirm{};
    bool ventWasPending{},ventCancelSent{};std::uint64_t ventPendingAt{};
    int nearbyVent{-1},selectedVent{-1};
    float cameraDistance{3.0F},cameraHeight{2.0F},cameraSide{0.5F};
    bool duct{};unsigned ductRun{};std::uint64_t ductAt{};
    float collisionScale{1.0F};
    bool freeCamera{},eyeKnown{};
    std::array<float,3> freeEye{},lastEye{},freeForward{0.0F,0.0F,1.0F};
    std::uint64_t lastFrameMs{}, nextCameraAttemptMs{}, nextInputAttemptMs{};
};
LocalState gLocal{}; // certified game thread only
std::uintptr_t gCameraBase{};
std::atomic<DWORD> gPilotThread{};
void* gFollowTrampoline{};
void* gWorldTrampoline{};
void* gSubmitTrampoline{};
void* gPostBlendTrampoline{};
std::atomic<std::uint32_t> gCameraGeneration{};
struct WorldSample {std::atomic<std::uintptr_t> world{};std::atomic<std::uint64_t> at{};std::atomic<std::uint32_t> generation{};};
std::array<WorldSample,4> gWorlds{};

template<typename T>
bool Read(const std::uintptr_t at, T& out) noexcept {
    return at != 0U && crash_guard::TryCopy(&out,reinterpret_cast<const void*>(at),sizeof(T));
}
alien_pvp::technical::Identity TechnicalTicket(std::uintptr_t character,std::uint32_t generation) noexcept {
    alien_pvp::technical::Identity id{character,0U,0U,generation};std::uint32_t dead{};
    if(!character || !Read(character+0x50U,id.runtime) || !Read(character+0x718U,id.skeleton) ||
        !Read(character+0x2F90U,dead) || dead!=0U)return {};
    return id;
}
bool PrepareTechnicalHome(const Context& context) noexcept {
    const auto id=TechnicalTicket(context.technicalPlayer,context.nativeGeneration);
    std::array<float,3> home{};float yaw{};std::uint8_t hiding{},interaction{};
    if(!id.Valid() || !Read(context.technicalPlayer+0x70U,home) ||
        !Read(context.technicalPlayer+0x174U,yaw) || !Read(context.technicalPlayer+0x4249U,hiding) ||
        !Read(context.technicalPlayer+0x425BU,interaction))return false;
    return gLocal.parking.Prepare(id,home,alien_pvp::WrapYaw(yaw),hiding==0U && interaction==0U);
}
bool ParkTechnical(const Context& context) noexcept {
    const auto id=TechnicalTicket(context.technicalPlayer,context.nativeGeneration);
    std::array<float,3> target{};float yaw{};
    // Preserve mouse look: the technical Character supplies its current yaw.
    if(!gLocal.parking.Target(id,target) || !Read(context.technicalPlayer+0x174U,yaw) ||
        !std::isfinite(yaw))return false;
    if(!character_direct_step_runtime::ApplyAIRecoveryTransform(context.technicalPlayer,target.data(),alien_pvp::WrapYaw(yaw)))return false;
    gLocal.parking.Applied(id);return true;
}
void RestoreTechnicalHome(bool validWorld) noexcept {
    const auto camera=gLocal.camera;std::array<float,3> home{};float yaw{};
    if(validWorld && gLocal.parking.Restore(TechnicalTicket(camera.technical,camera.generation),home,yaw))
        static_cast<void>(character_direct_step_runtime::ApplyAIRecoveryTransform(camera.technical,home.data(),yaw));
    gLocal.parking.Clear();
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
constexpr std::uint32_t NameHash(const char* name,std::size_t size) noexcept {
    std::uint32_t h{};
    for(std::size_t i=0,j=0;i<256U;++i,j=(j+1U)%size)
        h+=static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[j]))<<(i&31U);
    return h;
}
std::uintptr_t FindFollow(const std::uintptr_t camera) noexcept {
    std::uintptr_t array{};std::uint32_t count{};
    if(!Read(camera+0xBCU,array) || !array || !Read(camera+0xC0U,count) || count>256U)return 0U;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t b{};std::uint32_t type{};
        if(Read(array+i*sizeof(std::uintptr_t),b) && b && Read(b+8U,type) &&
            type==NameHash("FollowBehavior",14U))return b;
    }
    return 0U;
}
template<class T> bool Write(std::uintptr_t at,const T& value) noexcept {
    return at && crash_guard::TryCopy(reinterpret_cast<void*>(at),&value,sizeof(value));
}
bool ConfigureFollow(CameraBinding& camera,bool firstPerson) noexcept {
    const auto b=FindFollow(camera.follow);
    if(!b || (camera.followSaved && camera.followBehavior!=b))return false;
    if(!camera.followSaved) {
        if(!Read(b+0x9CU,camera.originalFollow) || !Read(b+0xF6U,camera.originalFirstPerson))return false;
        camera.followBehavior=b;camera.followSaved=true;
    }
    // Donor camera.cpp::FollowFromCfg, DuctUpdate and FpKeeperTick.
    const std::array<float,6> values=gLocal.duct?
        std::array<float,6>{0.0F,0.35F,-0.9F,0.0F,0.3F,1.5F}:
        std::array<float,6>{gLocal.cameraSide,gLocal.cameraHeight,-gLocal.cameraDistance,
            0.0F,gLocal.cameraHeight-0.7F,2.0F};
    return Write(b+0x9CU,values) && Write(b+0xF6U,firstPerson?camera.originalFirstPerson:std::uint8_t{0});
}
bool IsBackstage(std::uintptr_t alien) noexcept {
    std::uint8_t a{},b{};
    return Read(alien+0x2DAU,a) && Read(alien+0x2DBU,b) && alien_pvp::spatial::Backstage(a,b);
}
// Donor Havok ray ABI and camera collision math. All native rays run on the
// current pilot's engine thread, using worlds observed in this generation.
bool Ray(std::uintptr_t world,const std::array<float,3>& from,const std::array<float,3>& to,
    std::array<float,3>& hit) noexcept {
    alignas(16) float a[4]{from[0],from[1],from[2],0},b[4]{to[0],to[1],to[2],0},p[4]{},n[4]{};
    alignas(16) std::uint8_t result[16]{};
#if defined(_MSC_VER)
    __try {
        using Fn=void(__thiscall*)(std::uintptr_t,void*,std::uintptr_t,const void*,const void*,void*,void*);
        reinterpret_cast<Fn>(gCameraBase+0x0028DAB0U)(0U,result,world,a,b,p,n);
        if(!result[0])return false;
        hit={p[0],p[1],p[2]};return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
#else
    return false;
#endif
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
bool TechnicalIdentityCurrent(const CameraBinding& camera) noexcept;
void RestoreLocalCamera(const bool validWorld) noexcept {
    const auto camera=gLocal.camera;
    const bool sameTechnical=validWorld && TechnicalIdentityCurrent(camera);
    std::uintptr_t alienSkeleton{};std::uint32_t alienRuntime{};
    const bool sameAlien=validWorld && camera.generation==gLocal.generation &&
        camera.alien==gLocal.character && camera.alienRuntime!=0U &&
        Read(camera.alien+0x50U,alienRuntime) && alienRuntime==camera.alienRuntime &&
        Read(camera.alien+kCharacterSkeletonOffset,alienSkeleton) && alienSkeleton==camera.alienSkeleton;
    alien_pvp_visual::RestoreAlienHead(camera.alien,camera.generation,sameAlien);
    alien_pvp_visual::RestoreTechnical(camera.technical,camera.generation,sameTechnical);
    RestoreTechnicalHome(sameTechnical);
    gLocal.camera={};
    if (!sameTechnical || !camera.touched || gCameraBase==0U) { gCameraBase=0U;return; }
    CameraBinding current{};
    std::uintptr_t active{};
    if (CurrentCameraRoot(gCameraBase,current) &&
        current.root==camera.root && current.manager==camera.manager &&
        current.follow==camera.follow && current.head==camera.head) {
        if(camera.followSaved && FindFollow(camera.follow)==camera.followBehavior) {
            static_cast<void>(Write(camera.followBehavior+0x9CU,camera.originalFollow));
            static_cast<void>(Write(camera.followBehavior+0xF6U,camera.originalFirstPerson));
        }
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
    camera.technical=context.technicalPlayer;
    camera.alien=context.alien;
    if (!Read(camera.technical+0x50U,camera.technicalRuntime) ||
        !Read(camera.technical+kCharacterSkeletonOffset,camera.technicalSkeleton) ||
        !Read(camera.alien+0x50U,camera.alienRuntime) ||
        !Read(camera.alien+kCharacterSkeletonOffset,camera.alienSkeleton) ||
        !camera.technicalRuntime || !camera.technicalSkeleton || !camera.alienRuntime || !camera.alienSkeleton) return false;
    // Donor's parking requires a living, non-interacting Amanda. Capture home
    // before retargeting the camera; never use the Alien position as her home.
    if (!gLocal.parking.Parked() && camera.previous==camera.follow) {
        float eyes{},feet{};
        if(!Read(camera.follow+0x18U,eyes) || !Read(camera.technical+0x74U,feet) ||
            !std::isfinite(eyes) || !std::isfinite(feet))return false;
        const float height=eyes-feet;
        if(height>0.2F && height<1.3F)return false; // donor: crouched restore can fall through floor
    }
    if (!PrepareTechnicalHome(context)) return false;
    camera.generation=context.nativeGeneration;camera.touched=true;
    gLocal.camera=camera;gCameraBase=context.moduleBase; // record partial retarget for rollback
    if(!ConfigureFollow(gLocal.camera,false)) {RestoreLocalCamera(true);return false;}
    camera=gLocal.camera;
    if (!CameraCall(context.moduleBase,camera.manager,camera.follow,context.alien)) {
        RestoreLocalCamera(true);return false;
    }
    camera.bound=true;
    gLocal.camera=camera;
    gCameraBase=context.moduleBase;
    return true;
}
bool TechnicalIdentityCurrent(const CameraBinding& camera) noexcept {
    std::uintptr_t skeleton{};
    std::uint32_t runtime{};
    return camera.technical!=0U && camera.technicalRuntime!=0U &&
        Read(camera.technical+0x50U,runtime) && runtime==camera.technicalRuntime &&
        Read(camera.technical+kCharacterSkeletonOffset,skeleton) &&
        skeleton==camera.technicalSkeleton;
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
bool VentRead(std::uintptr_t at,void* out,std::size_t size) noexcept {
    return at && crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),size);
}
bool UpdateVentInterface(const Context& context,alien_pvp::vents::Table& table) noexcept {
    const bool backstage=IsBackstage(context.alien);
    gLocal.ventMapOpen=false;
    if(backstage && !gLocal.freeCamera) {
        gLocal.freeEye=gLocal.lastEye;
        if(!gLocal.eyeKnown && !Read(context.alien+0x70U,gLocal.freeEye))return false;
        gLocal.freeCamera=true;
    }
    if(!backstage && !alien_pvp::VentPending())gLocal.freeCamera=false;
    gLocal.wasBackstage=backstage;gLocal.nearbyVent=-1;
    if(!alien_pvp::vents::ReadTable(context.alien,&VentRead,table))return false;
    alien_pvp::vents::Point body{};float best=100000.0F,nearest=100000.0F;int closest=-1;
    if(!Read(context.alien+0x70U,body))return false;
    if(gLocal.freeCamera)body=gLocal.freeEye;
    for(std::uint32_t i=0;i<table.count;++i) {
        alien_pvp::vents::Point point{};
        if(!alien_pvp::vents::ReadPoint(table,i,&VentRead,point))continue;
        const float distance=std::hypot(point[0]-body[0],point[2]-body[2])+std::abs(point[1]-body[1])*0.7F;
        if(distance<nearest){nearest=distance;closest=static_cast<int>(i);}
        const bool reachable=gLocal.freeCamera?
            std::hypot(std::hypot(point[0]-body[0],point[2]-body[2]),point[1]-body[1])<=3.0F:
            alien_pvp::vents::WithinReach(body,point);
        if(reachable && distance<best){best=distance;gLocal.nearbyVent=static_cast<int>(i);}
    }
    alien_pvp::vents::Point selected{};
    if(gLocal.selectedVent<0 || !alien_pvp::vents::ReadPoint(table,static_cast<std::uint32_t>(gLocal.selectedVent),&VentRead,selected))
        gLocal.selectedVent=closest;
    return true;
}
void TickInput(const Context& context,const std::uint64_t now) noexcept {
    if (now==gLocal.lastFrameMs) return;
    const float dt=gLocal.lastFrameMs && now>=gLocal.lastFrameMs?
        std::min(0.05F,static_cast<float>(now-gLocal.lastFrameMs)/1000.0F):0.016F;
    gLocal.lastFrameMs=now;
    const bool focused=InputAllowed(context.moduleBase);
    const auto authority=alien_pvp::VentAuthority();
    const bool pendingVent=(authority.action==alien_pvp::Action::EnterVent || authority.action==alien_pvp::Action::ExitVent) &&
        (authority.phase==alien_pvp::ActionPhase::Preparing || authority.phase==alien_pvp::ActionPhase::Started);
    if(pendingVent && !gLocal.ventWasPending){gLocal.ventPendingAt=now;gLocal.ventCancelSent=false;}
    if(!pendingVent)gLocal.ventCancelSent=false;
    gLocal.ventWasPending=pendingVent;
    alien_pvp::vents::Table vents{};
    const bool haveVents=UpdateVentInterface(context,vents);
    static_cast<void>(haveVents);
    const auto wheel=alien_pvp_input::TakeCameraWheel();
    const bool middle=focused && Down(VK_MBUTTON);
    if(focused && gLocal.camera.bound && !gLocal.camera.firstPerson && !gLocal.freeCamera) {
        if(middle && !gLocal.middleDown) {
            gLocal.cameraDistance=3.0F;gLocal.cameraHeight=2.0F;gLocal.cameraSide=0.5F;
        }
        const float steps=static_cast<float>(wheel)/120.0F;
        if(Down(VK_SHIFT))gLocal.cameraHeight=std::clamp(gLocal.cameraHeight+steps*0.1F,0.3F,4.5F);
        else if(Down(VK_CONTROL))gLocal.cameraSide=std::clamp(gLocal.cameraSide+steps*0.1F,-2.0F,2.0F);
        else gLocal.cameraDistance=std::clamp(gLocal.cameraDistance-steps*0.25F,1.0F,8.0F);
    }
    gLocal.middleDown=middle;
    int forward=0,side=0;
    if (focused && gLocal.camera.bound && !gLocal.freeCamera && !pendingVent) {
        forward=static_cast<int>(Down('W'))-static_cast<int>(Down('S'));
        side=static_cast<int>(Down('D'))-static_cast<int>(Down('A'));
    }
    std::uint32_t gait=1U;
    if (focused && Down(VK_SPACE)) gait=3U;
    else if (focused && Down(VK_SHIFT)) gait=2U;
    else if (focused && Down(VK_CONTROL)) gait=0U;
    float yaw{};
    if (!Read(context.technicalPlayer+0x174U,yaw) || !std::isfinite(yaw))
        if (!Read(context.alien+0x174U,yaw) || !std::isfinite(yaw)) yaw=0.0F;
    alien_pvp::LocalInput input{};
    input.forward=static_cast<float>(forward);input.strafe=static_cast<float>(side);
    input.yaw=alien_pvp::WrapYaw(yaw);input.gait=gait;input.focused=focused;
    if(gLocal.freeCamera && focused && alien_pvp::ControlActive()) {
        const float f=static_cast<float>(Down('W'))-static_cast<float>(Down('S'));
        const float s=static_cast<float>(Down('D'))-static_cast<float>(Down('A'));
        const float up=static_cast<float>(Down(VK_SPACE))-static_cast<float>(Down(VK_CONTROL));
        const auto direction=FlightDirection(gLocal.freeForward,yaw,f,s,up);
        for(unsigned i=0;i<3;++i)
            gLocal.freeEye[i]=std::clamp(gLocal.freeEye[i]+direction[i]*(Down(VK_SHIFT)?12.0F:5.0F)*dt,-99900.0F,99900.0F);
        // This is a local camera, never an Alien movement/teleport command.
        input.forward=input.strafe=0.0F;
        static_cast<void>(UpdateVentInterface(context,vents));
    }
    // Edges are queued independently of movement; holding a button does not
    // repeat a vent/attack command every frame. Native adapters explicitly
    // execute host-authorized native actions. No fabricated health effect.
    const auto edge=[&](int key,bool& previous,alien_pvp::Action action) noexcept {
        const bool pressed=focused && Down(key);
        if(pressed && !previous) {input.action=action;alien_pvp::CaptureLocal(input);}
        previous=pressed;
    };
    const bool interact=focused && Down('E'),exit=focused && Down('Q');
    const bool cancelKey=(interact && !gLocal.enterDown) || (exit && !gLocal.exitDown);
    const bool cancelMove=focused && !gLocal.freeCamera && pendingVent && now-gLocal.ventPendingAt>=400U &&
        (Down('W')||Down('A')||Down('S')||Down('D'));
    if(pendingVent && !gLocal.ventCancelSent && (cancelKey||cancelMove)) {
        input.action=alien_pvp::Action::CancelVent;alien_pvp::CaptureLocal(input);gLocal.ventCancelSent=true;
    }
    if((interact && !gLocal.enterDown) || (exit && !gLocal.exitDown)) {
        if(pendingVent) { /* Host resolves cancellation of the original traversal. */ }
        else if(gLocal.wasBackstage && interact && gLocal.nearbyVent>=0)
            static_cast<void>(GoToVent(static_cast<std::uint32_t>(gLocal.nearbyVent)));
        else if(interact && gLocal.nearbyVent>=0 && !alien_pvp::VentPending()) {
            alien_pvp::vents::Point portal{};
            if(alien_pvp::vents::ReadPoint(vents,static_cast<std::uint32_t>(gLocal.nearbyVent),&VentRead,portal)) {
                input.action=alien_pvp::Action::EnterVent;input.selectedVent=true;
                input.ventX=portal[0];input.ventY=portal[1];input.ventZ=portal[2];
                alien_pvp::CaptureLocal(input);
            }
        }
    }
    gLocal.enterDown=interact;gLocal.exitDown=exit;
    alien_pvp_input::SetMapMode(false);
    if(!gLocal.freeCamera && !pendingVent) {
        edge(VK_LBUTTON,gLocal.attackDown,alien_pvp::Action::Attack);
        edge('G',gLocal.roarDown,alien_pvp::Action::Roar);
    } else {gLocal.attackDown=Down(VK_LBUTTON);gLocal.roarDown=Down('G');}
    input.action=alien_pvp::Action::None;
    alien_pvp::CaptureLocal(input);
    gLocal.moving=alien_pvp::MovingNow();
}
bool CameraHookCurrent(std::uintptr_t behavior) noexcept {
    if(GetCurrentThreadId()!=gPilotThread.load(std::memory_order_acquire) ||
        !gCameraGeneration.load(std::memory_order_acquire))return false;
    CameraBinding current{};std::uint32_t runtime{};std::uintptr_t skeleton{};
    return gLocal.camera.bound && !gLocal.camera.firstPerson && gCameraBase &&
        CurrentCameraRoot(gCameraBase,current) && current.manager==gLocal.camera.manager &&
        current.previous==gLocal.camera.follow && FindFollow(current.follow)==behavior &&
        TechnicalIdentityCurrent(gLocal.camera) &&
        Read(gLocal.character+0x50U,runtime) && runtime==gLocal.camera.alienRuntime &&
        Read(gLocal.character+kCharacterSkeletonOffset,skeleton) && skeleton && skeleton==gLocal.camera.alienSkeleton;
}
void FollowBefore(std::uintptr_t behavior) noexcept {
    if(!CameraHookCurrent(behavior))return;
    // Donor OnFollowUpdateEnter: backstage actors do not tick the engine's
    // follow camera. Use the living technical actor to compute the orbit.
    const auto target=gLocal.freeCamera?gLocal.camera.technical:gLocal.character;
    std::uintptr_t current{},child{};
    if(Read(gLocal.camera.follow+0x148U,current) && current!=target)
        static_cast<void>(RetargetCamera(gCameraBase,gLocal.camera.follow,target));
    if(Read(behavior+0x34U,child) && child && child!=gLocal.camera.follow &&
        Read(child+0x148U,current) && current!=target)
        static_cast<void>(RetargetCamera(gCameraBase,child,target));
}
void FollowAfter(std::uintptr_t behavior) noexcept {
    if(!CameraHookCurrent(behavior))return;
    std::array<float,3> body{},base{},position{};
    if(!Read(gLocal.character+0x70U,body) || !Read(behavior+0x1C0U,base) || !Read(behavior+0x1CU,position))return;
    if(gLocal.freeCamera) {
        const float length=std::hypot(std::hypot(base[0]-position[0],base[2]-position[2]),base[1]-position[1]);
        if(length>0.01F)for(unsigned i=0;i<3;++i)gLocal.freeForward[i]=(base[i]-position[i])/length;
        const auto out=gLocal.freeEye;
        std::array<float,3> target{};
        for(unsigned i=0;i<3;++i)target[i]=out[i]+gLocal.freeForward[i]*5.0F;
        static_cast<void>(Write(behavior+0x1C0U,target));static_cast<void>(Write(behavior+0x1CU,out));
        std::uintptr_t camera{};
        if(Read(behavior+0x34U,camera) && camera) {
            for(auto offset:{0x14U,0x3CU,0x64U})static_cast<void>(Write(camera+offset,out));
            static_cast<void>(Write(camera+0x10CU,target));
            static_cast<void>(Write(camera+0x153U,std::uint8_t{1}));
        }
        return;
    }
    gLocal.lastEye=position;gLocal.eyeKnown=true;
    const auto now=GetTickCount64();const auto generation=gCameraGeneration.load(std::memory_order_acquire);
    std::uintptr_t world{};
    // Select the donor's world containing the actor's floor; samples from a
    // previous generation are never candidates after a checkpoint reload.
    for(auto& sample:gWorlds) {
        if(sample.generation.load(std::memory_order_acquire)!=generation || now-sample.at.load()>1000U)continue;
        const auto candidate=sample.world.load();std::array<float,3> hit{};
        const std::array<float,3> from{body[0],body[1]+1.0F,body[2]},to{body[0],body[1]-5.0F,body[2]};
        if(candidate && Ray(candidate,from,to,hit)){world=candidate;break;}
    }
    if(!world)return;
    if(now-gLocal.ductAt>=150U) {
        gLocal.ductAt=now;std::array<float,3> hit{};
        const bool low=Ray(world,{body[0],body[1]+0.4F,body[2]},{body[0],body[1]+1.9F,body[2]},hit);
        if(low==gLocal.duct)gLocal.ductRun=0U;
        else if(++gLocal.ductRun>=5U){gLocal.duct=low;gLocal.ductRun=0U;}
        static_cast<void>(ConfigureFollow(gLocal.camera,false));
    }
    // Donor Collide soft mode: median of three rays, smooth inward/outward.
    const std::array<float,3> from{base[0],base[1]+1.2F,base[2]};
    const float dx=position[0]-from[0],dy=position[1]-from[1],dz=position[2]-from[2];
    const float length=std::max(0.05F,std::sqrt(dx*dx+dy*dy+dz*dz));
    const float ux=dx/length,uy=dy/length,uz=dz/length,hl=std::hypot(ux,uz);
    const float sx=hl>0.01F?uz/hl*0.3F:0.3F,sz=hl>0.01F?-ux/hl*0.3F:0.0F;
    const auto fraction=[&](float ox,float oz) noexcept {
        std::array<float,3> hit{};
        if(!Ray(world,from,{position[0]+ox+ux*0.3F,position[1]+uy*0.3F,position[2]+oz+uz*0.3F},hit))return 1.0F;
        const float x=hit[0]-from[0],y=hit[1]-from[1],z=hit[2]-from[2];
        return std::min(1.0F,(std::sqrt(x*x+y*y+z*z)-0.3F)/length);
    };
    const float t0=fraction(0,0),t1=fraction(sx,sz),t2=fraction(-sx,-sz);
    const float median=std::max(std::min(t0,t1),std::min(std::max(t0,t1),t2));
    const float target=std::min(1.0F,std::max(std::max(0.3F,1.0F/length),median));
    gLocal.collisionScale+=(target-gLocal.collisionScale)*(target<gLocal.collisionScale?0.25F:0.07F);
    if(std::abs(target-gLocal.collisionScale)<0.02F)gLocal.collisionScale=target;
    if(gLocal.collisionScale<0.999F) {
        const std::array<float,3> out{from[0]+dx*gLocal.collisionScale,from[1]+dy*gLocal.collisionScale,from[2]+dz*gLocal.collisionScale};
        static_cast<void>(Write(behavior+0x1CU,out));
    }
}
void __fastcall FollowDetour(void* self,void*,std::uint32_t a,std::uint32_t b) noexcept {
    FollowBefore(reinterpret_cast<std::uintptr_t>(self));
    reinterpret_cast<void(__thiscall*)(void*,std::uint32_t,std::uint32_t)>(gFollowTrampoline)(self,a,b);
    FollowAfter(reinterpret_cast<std::uintptr_t>(self));
}
bool NoclipViewCurrent() noexcept {
    return GetCurrentThreadId()==gPilotThread.load(std::memory_order_acquire) &&
        gLocal.freeCamera && CameraHookCurrent(gLocal.camera.followBehavior);
}
void __fastcall SubmitDetour(void* self,void*) noexcept {
    reinterpret_cast<void(__thiscall*)(void*)>(gSubmitTrampoline)(self);
    if(!NoclipViewCurrent() || reinterpret_cast<std::uintptr_t>(self)!=gLocal.camera.manager)return;
    // Donor photo.cpp::OnCamSubmitLeave. Retail otherwise selects the room
    // of the parked technical actor. Let the renderer match the camera's
    // position instead. Outside the level, retain its existing room fallback.
    std::uintptr_t manager{},pov{};std::uint32_t zone{};
    if(!Read(gCameraBase+0x01366A00U,manager) || !manager ||
        !Read(manager+0x8CU,pov) || !pov || !Read(pov+0x330U,zone) || zone==0xFFFFFFFFU)return;
    static_cast<void>(Write(gCameraBase+0x01358144U,std::uint8_t{0}));
    static_cast<void>(Write(gCameraBase+0x01358145U,std::uint8_t{0}));
}
void __fastcall PostBlendDetour(void* self,void*) noexcept {
    reinterpret_cast<void(__thiscall*)(void*)>(gPostBlendTrampoline)(self);
    if(!NoclipViewCurrent())return;
    // Donor photo.cpp::OnPostBlendLeave: change only this frame's blended
    // output, never level settings. Retail recalculates these on every call,
    // so normal lighting resumes on the first frame outside noclip.
    const auto manager=reinterpret_cast<std::uintptr_t>(self);
    float brightness{};
    if(!Read(manager+0x1914U,brightness) || !std::isfinite(brightness))return;
    // Preserve the level's RGB/exposure. A forced minimum RGB multiplier of
    // four plus .25 brightness clipped the whole image to white in build 444.
    // Only lift very dark output slightly; never multiply the color channels.
    brightness=std::max(brightness,0.015F);
    static_cast<void>(Write(manager+0x1914U,brightness));
    // Clean, readable navigation: no vignette, grain, or motion blur.
    for(const auto offset:{0x1988U,0x19E8U,0x19ECU,0x19B0U,0x19B4U,0x19B8U,0x19D0U,0x19D4U,0x19D8U})
        static_cast<void>(Write(manager+offset,0.0F));
}
void __fastcall WorldDetour(void* self,void*,std::uint32_t a,std::uint32_t b) noexcept {
    const auto generation=gCameraGeneration.load(std::memory_order_acquire);
    if(generation) {
        const auto world=reinterpret_cast<std::uintptr_t>(self);const auto now=GetTickCount64();
        for(auto& sample:gWorlds) {
            if(sample.world.load()==world || sample.generation.load()!=generation || now-sample.at.load()>3000U) {
                sample.world.store(world);sample.at.store(now);sample.generation.store(generation,std::memory_order_release);break;
            }
        }
    }
    reinterpret_cast<void(__thiscall*)(void*,std::uint32_t,std::uint32_t)>(gWorldTrampoline)(self,a,b);
}
} // namespace

bool InstallCameraHooks(std::uintptr_t base,HookManager& hooks,std::string& error) {
    // Both supplied x86 entries start with push ebp; mov ebp,esp;
    // and esp,-16. Six complete relocation-free instructions bytes.
    for(const auto world:{false,true}) {
        HookRequest request;
        request.name=world?"alien_donor_camera_world":"alien_donor_follow_camera";
        request.target=reinterpret_cast<void*>(base+(world?0x00748890U:0x0002E970U));
        request.detour=world?reinterpret_cast<void*>(&WorldDetour):reinterpret_cast<void*>(&FollowDetour);
        request.patchLength=6U;request.relocationSafePrefix=true;
        request.expected=MaskedBytes{{0x55,0x8B,0xEC,0x83,0xE4,0xF0},"xxxxxx"};
        request.trampolineOut=world?&gWorldTrampoline:&gFollowTrampoline;
        if(!hooks.Install(request,error))return false;
    }
    // Exact retail entries; neither prefix contains a relative instruction.
    HookRequest submit;
    submit.name="alien_noclip_render_room";
    submit.target=reinterpret_cast<void*>(base+0x00032300U);
    submit.detour=reinterpret_cast<void*>(&SubmitDetour);
    submit.patchLength=6U;submit.relocationSafePrefix=true;
    submit.expected=MaskedBytes{{0x55,0x8B,0xEC,0x83,0xE4,0xF0},"xxxxxx"};
    submit.trampolineOut=&gSubmitTrampoline;
    if(!hooks.Install(submit,error))return false;
    HookRequest post;
    post.name="alien_noclip_bright_view";
    post.target=reinterpret_cast<void*>(base+0x00608C50U);
    post.detour=reinterpret_cast<void*>(&PostBlendDetour);
    // push ecx; movq xmm0,[absolute address]. The loader relocates that
    // address, which remains valid in the trampoline (no EIP-relative data).
    post.patchLength=9U;post.relocationSafePrefix=true;
    post.expected=MaskedBytes{{0x51,0xF3,0x0F,0x7E,0x05,0,0,0,0},"xxxxx????"};
    post.trampolineOut=&gPostBlendTrampoline;
    if(!hooks.Install(post,error))return false;
    return true;
}

Readiness Snapshot() noexcept {
    return {alien_pvp_input::Installed(),gLocal.camera.bound,
        gLocal.visualReady,gLocal.moving};
}
VentInterface VentSnapshot() noexcept {
    return {false,gLocal.wasBackstage,gLocal.nearbyVent,gLocal.selectedVent,gLocal.freeCamera};
}
bool GoToVent(std::uint32_t index) noexcept {
    if(!gLocal.freeCamera || !gLocal.wasBackstage || alien_pvp::VentPending() || !alien_pvp::ControlActive() ||
        !gLocal.camera.bound || !InputAllowed(gCameraBase))return false;
    alien_pvp::vents::Table table{};alien_pvp::vents::Point point{};
    if(!alien_pvp::vents::ReadTable(gLocal.character,&VentRead,table) ||
        !alien_pvp::vents::ReadPoint(table,index,&VentRead,point))return false;
    if(std::hypot(std::hypot(point[0]-gLocal.freeEye[0],point[2]-gLocal.freeEye[2]),point[1]-gLocal.freeEye[1])>3.0F)return false;
    gLocal.selectedVent=static_cast<int>(index);
    alien_pvp::LocalInput input{};input.focused=true;
    input.action=alien_pvp::Action::ExitVent;input.selectedVent=true;
    input.ventX=point[0];input.ventY=point[1];input.ventZ=point[2];
    alien_pvp::CaptureLocal(input);return true;
}
bool Drives(const std::uintptr_t character,const std::uint32_t generation) noexcept {
    return character!=0U && gLocal.character==character &&
        gLocal.generation==generation && gLocal.camera.bound &&
        gLocal.visualReady && alien_pvp_input::Installed();
}
void Reset(const bool sameCameraWorld,const bool certifiedLivingCharacter) noexcept {
    gCameraGeneration.store(0U,std::memory_order_release);
    // The controller does not release AISync's exclusive lease: only the lease
    // owner does that after the certified request boundary is idle.
    alien_pvp_input::SetActive(false);
    const auto camera=gLocal.camera;
    alien_pvp_visual::RestoreAlienHead(gLocal.character,gLocal.generation,
        certifiedLivingCharacter && sameCameraWorld);
    alien_pvp_visual::RestoreTechnical(camera.technical,camera.generation,
        sameCameraWorld && TechnicalIdentityCurrent(camera));
    RestoreLocalCamera(sameCameraWorld);
    if (certifiedLivingCharacter && gLocal.character!=0U)
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(gLocal.character));
    gLocal={};
    alien_pvp_input::Uninstall();
}
void Pump(const Context& context) noexcept {
    gPilotThread.store(GetCurrentThreadId(),std::memory_order_release);
    const bool current=context.exclusiveLease && context.moduleBase!=0U &&
        context.alien!=0U && context.technicalPlayer!=0U &&
        context.nativeGeneration!=0U && context.localPeer!=0U &&
        context.ownerPeer==context.localPeer;
    if (!current) {
        const bool waiting=context.possessionRequested && context.moduleBase && context.technicalPlayer &&
            context.nativeGeneration && context.ownerPeer==context.localPeer;
        if (waiting) {
            if (gLocal.generation && gLocal.generation!=context.nativeGeneration) Reset(false,false);
            gLocal.generation=context.nativeGeneration;
            const auto waitingNow=GetTickCount64();
            if (!alien_pvp_input::Installed() && waitingNow>=gLocal.nextInputAttemptMs) {
                gLocal.nextInputAttemptMs=waitingNow+3000U;
                static_cast<void>(alien_pvp_input::EnsureInstalled(context.moduleBase));
            }
            if (!alien_pvp_input::Installed()) return;
            alien_pvp_input::SetActive(InputAllowed(context.moduleBase));
            // Keep the original technical identity/home so cancellation also
            // restores a role which never obtained an Alien camera.
            if (!gLocal.camera.technical) {
                CameraBinding camera{};
                if (!CurrentCameraRoot(context.moduleBase,camera)) return;
                camera.technical=context.technicalPlayer;camera.generation=context.nativeGeneration;
                if (!Read(camera.technical+0x50U,camera.technicalRuntime) ||
                    !Read(camera.technical+kCharacterSkeletonOffset,camera.technicalSkeleton) ||
                    !TechnicalIdentityCurrent(camera) || !PrepareTechnicalHome(context)) return;
                gLocal.camera=camera;gCameraBase=context.moduleBase;
            }
            // Like the donor, park only after an Alien exists and its camera
            // can be attached. Moving THE_PLAYER earlier can disturb the map's
            // initial spawn triggers. The waiting overlay covers the camera.
            gLocal.visualReady=TechnicalIdentityCurrent(gLocal.camera) &&
                alien_pvp_visual::HideTechnical(context.technicalPlayer,context.nativeGeneration);
            alien_pvp::CaptureLocal({});
            return;
        }
        if (gLocal.character!=0U || gLocal.camera.technical!=0U)
            Reset(context.nativeGeneration!=0U && gLocal.generation==context.nativeGeneration,false);
        else alien_pvp_input::SetActive(false);
        return;
    }
    if (gLocal.character!=0U && (gLocal.character!=context.alien ||
        gLocal.generation!=context.nativeGeneration))
        Reset(false,false);
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
    gCameraGeneration.store(attached?context.nativeGeneration:0U,std::memory_order_release);
    if(attached)static_cast<void>(ConfigureFollow(gLocal.camera,gLocal.camera.firstPerson));
    // Retail can re-enable a submodel after a camera/animation transition.
    // Reassert head visibility only while we own the exact current Alien.
    if (attached && gLocal.camera.firstPerson &&
        !alien_pvp_visual::HideAlienHead(context.alien,context.nativeGeneration)) {
        const auto camera=gLocal.camera;
        if (CameraCall(context.moduleBase,camera.manager,camera.follow,context.alien)) {
            gLocal.camera.firstPerson=false;
            alien_pvp_visual::RestoreAlienHead(context.alien,context.nativeGeneration,true);
        }
    }
    gLocal.visualReady=attached && TechnicalIdentityCurrent(gLocal.camera) &&
        alien_pvp_visual::HideTechnical(context.technicalPlayer,context.nativeGeneration) &&
        ParkTechnical(context);
    if (!gLocal.visualReady && attached)
        alien_pvp_visual::RestoreTechnical(context.technicalPlayer,context.nativeGeneration,
            TechnicalIdentityCurrent(gLocal.camera));
    alien_pvp_input::SetActive(gLocal.visualReady && InputAllowed(context.moduleBase));
    if (gLocal.visualReady) TickInput(context,now);
    else alien_pvp::CaptureLocal({});
}
} // namespace aimp::alien_pvp_local
