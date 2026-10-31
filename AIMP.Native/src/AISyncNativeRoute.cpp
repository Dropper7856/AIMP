#include "aimp/AISyncNativeRoute.h"
#include <Windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>

namespace aimp::ai_sync::route {
namespace {
constexpr std::uintptr_t kResultType = 0x00DC7288, kNodeType = 0x00DC730C;
constexpr std::uintptr_t kNodeFactory = 0x00062260, kAppend = 0x00059CE0;
constexpr std::uintptr_t kRelease = 0xD770, kResolve = 0x0003DC70;
constexpr std::uintptr_t kPortal = 0x00043050;
struct Busy {
    bool& flag;
    explicit Busy(bool& value) noexcept : flag(value) { flag = true; }
    ~Busy() { flag = false; }
};
template<class T> bool Read(std::uintptr_t at, T& value) noexcept {
    __try { value = *reinterpret_cast<const T*>(at); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Match(std::uintptr_t at, std::initializer_list<std::uint8_t> bytes) noexcept {
    for (auto byte : bytes) { std::uint8_t actual{}; if (!Read(at++,actual) || actual != byte) return false; }
    return true;
}
bool Pair(std::uintptr_t object, std::uintptr_t control) noexcept {
    LONG count{};
    return object != 0 && control != 0 && object == control + 0xC && Read(control+4,count) && count > 0;
}
template<std::size_t Size> bool SameFloatBits(const std::array<float,Size>& a,const std::array<float,Size>& b) noexcept {
    for(std::size_t i=0;i<Size;++i)if(std::bit_cast<std::uint32_t>(a[i])!=std::bit_cast<std::uint32_t>(b[i]))return false;
    return true;
}
bool SameGraphBits(const Graph& a,const Graph& b) noexcept {
    if(a!=b||!SameFloatBits(a.auxiliary,b.auxiliary)||!SameFloatBits(a.goal,b.goal))return false;
    for(std::size_t i=0;i<a.nodeCount;++i)if(!SameFloatBits(a.nodes[i].vectors,b.nodes[i].vectors))return false;
    return true;
}
bool RetiredBlock(std::uintptr_t navigation,std::uintptr_t block) noexcept {
    std::uintptr_t head{},at{},last{},prior{};std::uint32_t count{},matches{};
    if(!Read(navigation+0x34,head)||!head||!Read(navigation+0x38,count)||!count||count>128||
        !Read(head,at)||!Read(head+4,last))return false;
    prior=head;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t before{},next{},candidate{};
        if(!at||at==head||!Read(at+4,before)||before!=prior||!Read(at,next)||!Read(at+8,candidate))return false;
        if(candidate==block) {std::uint32_t reason{};if(!Read(block+0x44,reason)||reason!=3)return false;++matches;}
        prior=at;at=next;
    }
    return matches==1&&at==head&&prior==last;
}
}
bool ReplacementAffectsOnlyBlock(std::uintptr_t navigation,std::uintptr_t block) noexcept {
    return ReplacementAffectsOnlyBlocks(navigation,std::span(&block,1));
}
bool ReplacementAffectsOnlyBlocks(std::uintptr_t navigation,std::span<const std::uintptr_t> owned) noexcept {
    std::uintptr_t head{},at{},prior{},last{};std::uint32_t count{},matches{};
    if(!navigation||owned.empty()||owned.size()>128||!Read(navigation+0x28,head)||!head||!Read(head,at)||
        !Read(head+4,last)||!Read(navigation+0x2C,count)||!count||count>128)return false;
    for(std::size_t i=0;i<owned.size();++i)
        if(!owned[i]||std::find(owned.begin(),owned.begin()+i,owned[i])!=owned.begin()+i)return false;
    std::array<std::uintptr_t,128> seen{};
    prior=head;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t previous{},next{},candidate{};std::uint32_t reason{},mode{},state{};
        if(!at||at==head||!Read(at+4,previous)||previous!=prior||!Read(at,next)||
            !Read(at+8,candidate)||!candidate||!Read(candidate+0x44,reason)||
            !Read(candidate+0x48,mode)||mode||!Read(candidate+0x40,state))return false;
        if(std::find(seen.begin(),seen.begin()+i,candidate)!=seen.begin()+i)return false;
        seen[i]=candidate;
        if(std::find(owned.begin(),owned.end(),candidate)!=owned.end()) {if(reason==3||state!=3)return false;++matches;}
        else if(reason!=3)return false;
        prior=at;at=next;
    }
    return matches==owned.size()&&at==head&&prior==last;
}
bool NativeRouteResult::Initialize(std::uintptr_t base, bool exact, route_identity::Registry& registry) noexcept {
    if (busy_ || entered_ || !exact || base == 0 || base > UINTPTR_MAX - 0x00EC0000U) return false;
    base_ = base;
    if (!Image()) { base_ = 0; return false; }
    registry_ = &registry; return true;
}
bool NativeRouteResult::Image() const noexcept {
    // SHA00d6... base00400000; fresh instruction exports in
    // aisync_v260_20261024/native plus Phase2/native-path/0043dc70.txt.
    std::uint32_t exceptionHandler{};
    return Match(base_+kNodeFactory,{0x6A,0xFF,0x68}) &&
        Read(base_+kNodeFactory+3,exceptionHandler) && exceptionHandler == base_+0x00D206B9U &&
        Match(base_+kNodeFactory+0x82,{0xC3}) &&
        Match(base_+kAppend,{0x8B,0x44,0x24,0x04,0x56,0x57,0x8B,0xF1}) &&
        Match(base_+kAppend+0x3E,{0xC2,0x04,0x00}) &&
        Match(base_+kResolve,{0x56,0x8B,0x74,0x24,0x08,0x8B,0xD1}) &&
        Match(base_+kResolve+0x11,{0xC2,0x10,0x00}) &&
        Match(base_+kPortal+0x2DD,{0xC3}) &&
        Match(base_+kRelease,{0x56,0x8B,0x71,0x04}) &&
        Match(base_+0x417C0U,{0x56,0x8B,0x74,0x24,0x08,0x57,0x8B,0xF9}) &&
        Match(base_+0x417D0U,{0xC2,0x04,0x00})&&
        Match(base_+0x528B6U,{0xC7,0x47,0x0C,7,0,0,0})&&
        Match(base_+0x52986U,{0xC7,0x47,0x0C,8,0,0,0})&&
        Match(base_+0x524A6U,{0xC7,0x47,0x0C,9,0,0,0})&&
        Match(base_+0x52576U,{0xC7,0x47,0x0C,11,0,0,0})&&
        Match(base_+0x4F3E6U,{0xC7,0x47,0x0C,2,0,0,0})&&
        Match(base_+0x52646U,{0xC7,0x47,0x0C,3,0,0,0})&&
        Match(base_+0x527E6U,{0xC7,0x47,0x0C,4,0,0,0})&&
        Match(base_+0x617D0U,{0x6A,0xFF,0x68})&&Match(base_+0x618A4U,{0xC2,4,0});
}
bool NativeRouteResult::Context(const LocalContext& local, bool capturing, std::size_t count, ImportPoint point,bool allowAppend) const noexcept {
    std::uintptr_t type{}, sentinel{}, next{}, previous{}, bSentinel{}, bFirst{}, bLast{}, b{};
    std::uintptr_t currentB{}, previousB{}, currentR{}, currentControl{};
    std::uint32_t nCount{}, blockCount{}, rCount{}, ready{}, status{}, stop{}, mode{}, blockState{};
    contextCheck_=0;
    if (local.navigation == 0) {contextCheck_=1;return false;}
    if (local.block == 0) {contextCheck_=2;return false;}
    if (!Pair(local.result,local.resultControl)) {contextCheck_=3;return false;}
    if (!Read(local.result,type)) {contextCheck_=4;return false;}
    if (type != base_+kResultType) {contextCheck_=5;return false;}
    if (!Read(local.navigation,sentinel)) {contextCheck_=6;return false;}
    if (sentinel == 0) {contextCheck_=7;return false;}
    if (!Read(local.navigation+4,nCount)) {contextCheck_=8;return false;}
    const bool replacement=!capturing&&point==ImportPoint::ReplacementNoWorker;
    if (nCount != 0 && !replacement) {contextCheck_=9;return false;}
    if (nCount > kMaxNodes*2) {contextCheck_=9;return false;}
    if (!Read(sentinel,next)) {contextCheck_=10;return false;}
    if (!next || (!nCount && next != sentinel)) {contextCheck_=11;return false;}
    if (!Read(sentinel+4,previous)) {contextCheck_=12;return false;}
    if (!previous || (!nCount && previous != sentinel)) {contextCheck_=13;return false;}
    if(replacement&&nCount) {
        // The existing route stays alive until 004659D0's native prefix.
        // Validate the old ring before considering the installer's replacement
        // predicates below. Neither the importer nor this check erases it.
        auto at=next;auto before=sentinel;
        for(std::uint32_t i=0;i<nCount;++i) {
            std::uintptr_t prior{},following{};
            if(!at||at==sentinel||!Read(at+4,prior)||prior!=before||!Read(at,following)) {contextCheck_=62;return false;}
            before=at;at=following;
        }
        if(at!=sentinel||before!=previous) {contextCheck_=63;return false;}
    }
    if (!Read(local.navigation+0x28,bSentinel)) {contextCheck_=14;return false;}
    if (bSentinel == 0) {contextCheck_=15;return false;}
    if (!Read(local.navigation+0x2C,blockCount)) {contextCheck_=16;return false;}
    if (!blockCount || blockCount > 128) {contextCheck_=17;return false;}
    if (!Read(bSentinel,bFirst)) {contextCheck_=18;return false;}
    if (bFirst == bSentinel) {contextCheck_=19;return false;}
    if (!Read(bSentinel+4,bLast)) {contextCheck_=20;return false;}
    std::array<std::uintptr_t,128> registeredBlocks{};
    std::uint32_t matches{};auto at=bFirst;previous=bSentinel;
    for(std::uint32_t i=0;i<blockCount;++i) {
        std::uintptr_t prior{};
        if(!at||at==bSentinel||!Read(at,next)) {contextCheck_=22;return false;}
        if(!Read(at+4,prior)||prior!=previous) {contextCheck_=24;return false;}
        if(!Read(at+8,b)) {contextCheck_=26;return false;}
        registeredBlocks[i]=b;
        if(b==local.block)++matches;
        previous=at;at=next;
    }
    if(at!=bSentinel||previous!=bLast) {contextCheck_=23;return false;}
    if(matches!=1) {contextCheck_=27;return false;}
    if (!Read(local.block+0x30,stop)) {contextCheck_=28;return false;}
    if (!StopTrimDisabled(stop)&&(!replacement||!std::isfinite(std::bit_cast<float>(stop))||std::bit_cast<float>(stop)<0)) {contextCheck_=29;return false;}
    if (!Read(local.block+0x48,mode)) {contextCheck_=30;return false;}
    if (mode != 0) {contextCheck_=31;return false;}
    if (!Read(local.block+0x40,blockState)) {contextCheck_=32;return false;}
    if (((capturing || point != ImportPoint::QueryDispatch) ? (blockState!=1U&&blockState!=2U) : blockState!=1U)) {contextCheck_=33;return false;}
    if (!Read(local.navigation+0x40,currentB)) {contextCheck_=34;return false;}
    if (!Read(local.navigation+0x44,previousB)) {contextCheck_=35;return false;}
    if (previousB != 0 && (!replacement||
        (std::find(registeredBlocks.begin(),registeredBlocks.begin()+blockCount,previousB)==registeredBlocks.begin()+blockCount&&
         !RetiredBlock(local.navigation,previousB)))) {contextCheck_=36;return false;}
    if(replacement&&nCount) {
        // 004659D0 has THREE erase predicates: N.reset, current B bit0,
        // or previous B bit7 (00465A7A). Both 00456BD0 and 00456C30 call
        // 00741570 before installing R. The third case is the native handoff
        // from an interrupted/retired block; it must not be mistaken for an
        // append. Dereference previousB only after membership validation.
        std::uint8_t reset{},flags{},previousFlags{};
        if(!Read(local.navigation+0x48,reset)||!Read(local.block+0x90,flags)||
            (previousB&&!Read(previousB+0x90,previousFlags))||
            (!allowAppend&&!reset&&!(flags&1U)&&!(previousFlags&0x80U))) {contextCheck_=61;return false;}
    }
    if (!Read(local.navigation+0x5C,currentR)) {contextCheck_=37;return false;}
    if (!Read(local.navigation+0x60,currentControl)) {contextCheck_=38;return false;}
    if (!Read(local.result+4,ready)) {contextCheck_=39;return false;}
    if (!Read(local.result+0xC,status)) {contextCheck_=40;return false;}
    if (!Read(local.result+0x38,sentinel)) {contextCheck_=41;return false;}
    if (sentinel == 0) {contextCheck_=42;return false;}
    if (!Read(local.result+0x3C,rCount)) {contextCheck_=43;return false;}
    if (capturing) { contextCheck_=50;return currentB == local.block && currentR == local.result && currentControl == local.resultControl &&
        ready == 1 && status == 1 && rCount != 0 && rCount <= kMaxNodes; }
    // 00461280 installs N.currentB/R and B.state2 after the dispatch wrapper
    // returns. The embedding owner must prove that it suppressed that exact
    // dispatch and retained R; no worker can concurrently publish to it.
    if (point != ImportPoint::QueryDispatch) {
        if(currentB!=local.block){contextCheck_=51;return false;}
        if(currentR!=local.result){contextCheck_=52;return false;}
        if(currentControl!=local.resultControl){contextCheck_=53;return false;}
        if(ready!=0){contextCheck_=54;return false;}
        if(status!=0){contextCheck_=55;return false;}
        if(rCount!=count){contextCheck_=56;return false;}
        contextCheck_=0;return true;
    }
    contextCheck_=60;return currentB == 0 && currentR == 0 && currentControl == 0 && ready == 0 && status == 0 && rCount == count;
}
bool NativeRouteResult::Protected(const Key& key, const LocalContext& local, const Protection& protection,
    std::span<const route_identity::ElementMapping> mappings, GuardPhase phase) const noexcept {
    if (!protection.storage || protection.mesh == 0 || !protection.guard ||
        !protection.guard(protection.context,key,local,mappings,phase)) return false;
    for (const auto& mapping : mappings) if (registry_->Revalidate(mapping) != route_identity::Result::Ok) return false;
    return true;
}
bool NativeRouteResult::ResolveOrdinary(const Protection& protection, std::uint32_t reference) noexcept {
    __try {
        if (resolve_) return resolve_(callsContext_,protection.mesh,reference);
        std::uintptr_t slot{}, element{};
        const auto result = reinterpret_cast<std::uint32_t(__thiscall*)(void*,std::uint32_t,std::uintptr_t*,std::uintptr_t*,void*)>(base_+kResolve)(
            reinterpret_cast<void*>(protection.mesh),reference,&slot,&element,nullptr);
        return result == 0x40000000U && slot != 0 && element != 0 &&
            (*reinterpret_cast<const std::uint32_t*>(element+0x20) & 1U) == 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeRouteResult::ResolveSpecial(const Protection& protection, std::uint32_t reference, std::uintptr_t& output) noexcept {
    // Resource-qualified element identity already includes the immutable blob
    // digest. Derive its unique link ordinal from H+38; no global pair lookup
    // (last-insertion-wins) and no spatial/direction heuristic is involved.
    __try {
        const auto w=protection.mesh;
        std::uint32_t saltBits{},slotBits{},elementBits{},capacity{},elementCapacity{};
        if(!Read(w+0x48,saltBits)||!Read(w+0x4C,slotBits)||!Read(w+0x50,elementBits)||
            !Read(w+0x30,capacity)||!Read(w+0x18,elementCapacity)||
            route_identity::Validate(route_identity::HandleLayout{elementBits,slotBits,saltBits,capacity,elementCapacity})!=route_identity::Result::Ok)return false;
        const auto index=(reference>>elementBits)&((1U<<slotBits)-1U);
        const auto ordinal=reference&((1U<<elementBits)-1U);
        const auto salt=reference>>(elementBits+slotBits);
        std::uintptr_t slots{},header{},elements{},links{},blob{};
        std::uint32_t actualSalt{},count{},first{},linkCount{},bytes{},flags{};
        if(!reference||index>=capacity||!Read(w+0x44,slots)||!slots||index>(UINTPTR_MAX-slots)/0x40)return false;
        const auto slot=slots+index*0x40;
        if(!Read(slot,actualSalt)||salt!=actualSalt||!Read(slot+8,header)||!header||
            !Read(slot+0xC,elements)||!Read(slot+0x2C,links)||!Read(slot+0x30,blob)||header!=blob||
            !Read(slot+0x34,bytes)||bytes<0x64||bytes>16U*1024U*1024U||blob>UINTPTR_MAX-bytes||
            !Read(slot+0x38,flags)||flags!=1||!Read(header+0x18,count)||count>elementCapacity||count>(1U<<22)||
            !Read(header+0x34,linkCount)||!Read(header+0x38,first)||!linkCount||first>=count||linkCount>count-first||
            ordinal<first||ordinal-first>=linkCount)return false;
        const auto inside=[&](std::uintptr_t at,std::uint64_t length) noexcept {
            return at>=blob&&at<=blob+bytes&&length<=blob+bytes-at;
        };
        // Exact v7 array order from00448280, not merely an in-blob pointer.
        // H+1C vertices precede H+18 elements; remaining arrays follow them.
        std::uint64_t elementOffset=0x64,linkOffset{};
        std::uint32_t arrayCount{};
        if(!Read(header+0x1C,arrayCount))return false;
        elementOffset+=static_cast<std::uint64_t>(arrayCount)*12;
        linkOffset=elementOffset+static_cast<std::uint64_t>(count)*36;
        constexpr std::array<std::uint32_t,5> strides{12,12,12,4,16};
        for(std::size_t i=0;i<strides.size();++i) {
            if(!Read(header+0x20+i*4,arrayCount))return false;
            linkOffset+=static_cast<std::uint64_t>(arrayCount)*strides[i];
        }
        if(elementOffset>bytes||linkOffset>bytes||
            linkOffset+static_cast<std::uint64_t>(linkCount)*60!=bytes||
            elements!=blob+static_cast<std::uintptr_t>(elementOffset)||links!=blob+static_cast<std::uintptr_t>(linkOffset))return false;
        if(!inside(elements,static_cast<std::uint64_t>(count)*0x24)||
            !inside(links,static_cast<std::uint64_t>(linkCount)*0x3C))return false;
        const auto link=links+(ordinal-first)*0x3C;
        std::uint32_t elementFlags{},localReference{};std::uint16_t linkElement{};
        if(!Read(elements+ordinal*0x24+0x20,elementFlags)||(elementFlags&1U)==0||
            !Read(link+0x1C,linkElement)||linkElement!=ordinal||!Read(link+0x20,localReference)||localReference!=reference)return false;
        const auto resolved=special_?special_(callsContext_,w,reference):
            reinterpret_cast<std::uintptr_t(__thiscall*)(void*,std::uint32_t)>(base_+0x417C0U)(reinterpret_cast<void*>(w),reference);
        if(resolved!=link)return false;
        output=link;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeRouteResult::MatchesNode(const Protection& protection,const Node& node,
    const route_identity::ElementMapping& mapping,std::uintptr_t borrowed) noexcept {
    if(!registry_||mapping.binding.owner.localMesh!=protection.mesh||
        registry_->Revalidate(mapping)!=route_identity::Result::Ok)return false;
    if(node.type==0||node.type==2)return borrowed==0&&ResolveOrdinary(protection,mapping.localReference);
    std::uintptr_t link{};
    return node.type==1&&ResolveSpecial(protection,mapping.localReference,link)&&borrowed==link;
}
bool NativeRouteResult::Create(Ref& ref) noexcept {
    __try {
        if (create_) return create_(callsContext_,ref.object,ref.control);
        std::uint8_t tag{};
        reinterpret_cast<Ref*(__cdecl*)(Ref*,std::uint8_t*)>(base_+kNodeFactory)(&ref,&tag);
        return Pair(ref.object,ref.control);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeRouteResult::ReadMarkers(std::uintptr_t node,std::uint32_t& output) noexcept {
    markerIndex_=markerKind_=markerTypeRva_=UINT32_MAX;
    std::uintptr_t begin{},end{};
    contextCheck_=1050;
    if(!Read(node+0x34,begin)||!Read(node+0x38,end))return false;
    contextCheck_=1051;
    if(end<begin||(end-begin)%8||(end-begin)>32||(!begin&&end))return false;
    std::uint32_t markers{};unsigned shift{};
    for(auto at=begin;at<end;at+=8,shift+=8) {
        markerIndex_=shift/8;
        std::uintptr_t object{},control{},type{};std::uint32_t kind{};
        contextCheck_=1052;
        if(!Read(at,object)||!Read(at+4,control))return false;
        contextCheck_=1053;
        if(!Pair(object,control))return false;
        contextCheck_=1054;
        if(!Read(object,type))return false;
        if(type>=base_&&type-base_<=UINT32_MAX)markerTypeRva_=static_cast<std::uint32_t>(type-base_);
        contextCheck_=1055;
        if(type!=base_+0xDC7224U)return false;
        contextCheck_=1056;
        if(!Read(object+0xC,kind))return false;
        markerKind_=kind;
        contextCheck_=1057;
        if(kind!=2&&kind!=3&&kind!=4&&kind!=7&&kind!=8&&kind!=9&&kind!=11)return false;
        markers|=kind<<shift;
    }
    contextCheck_=0;output=markers;return true;
}
bool NativeRouteResult::Marker(std::uint32_t kind,Ref& ref) noexcept {
    __try {
        if(marker_)return marker_(callsContext_,kind,ref.object,ref.control);
        std::uintptr_t factory{};
        switch(kind) {
        case 2:factory=0x4F370U;break;case 3:factory=0x525D0U;break;case 4:factory=0x52770U;break;
        case 7:factory=0x52840U;break;case 8:factory=0x52910U;break;case 9:factory=0x52430U;break;case 11:factory=0x52500U;break;
        default:return false;
        }
        std::uint8_t tag{};
        reinterpret_cast<Ref*(__cdecl*)(Ref*,std::uint8_t*)>(base_+factory)(&ref,&tag);
        std::uintptr_t type{};std::uint32_t actual{};
        return Pair(ref.object,ref.control)&&Read(ref.object,type)&&type==base_+0xDC7224U&&Read(ref.object+0xC,actual)&&actual==kind;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRouteResult::AttachMarker(std::uintptr_t node,const Ref& ref) noexcept {
    __try {
        if(attachMarker_)return attachMarker_(callsContext_,node,ref.object,ref.control);
        reinterpret_cast<void(__thiscall*)(void*,const Ref*)>(base_+0x617D0U)(reinterpret_cast<void*>(node+0x34),&ref);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRouteResult::Adjacent(const Protection& protection, std::uint32_t from, std::uint32_t to) noexcept {
    __try {
        if (adjacent_) return adjacent_(callsContext_,from,to);
        if (from == to) return true; // More than one metric node in one polygon.
        using Resolve = std::uint32_t(__thiscall*)(void*,std::uint32_t,std::uintptr_t*,std::uintptr_t*,void*);
        const auto resolve = reinterpret_cast<Resolve>(base_+kResolve);
        std::uintptr_t fromSlot{}, fromElement{}, toSlot{}, toElement{};
        if (resolve(reinterpret_cast<void*>(protection.mesh),from,&fromSlot,&fromElement,nullptr) != 0x40000000U ||
            resolve(reinterpret_cast<void*>(protection.mesh),to,&toSlot,&toElement,nullptr) != 0x40000000U ||
            fromSlot == 0 || toSlot == 0 || fromElement == 0 || toElement == 0) return false;
        float first[3]{}, second[3]{};
        // 00443050 is cdecl with eight stack arguments. It searches the FIRST
        // polygon's adjacency chain for exactly `to`; it performs no route
        // search, proximity selection or optimization. Outputs are temporary
        // local validation only; Host portal vectors remain intact.
        return reinterpret_cast<std::uint32_t(__cdecl*)(std::uint32_t,void*,void*,std::uint32_t,void*,void*,float*,float*)>(base_+kPortal)(
            from,reinterpret_cast<void*>(fromElement),reinterpret_cast<void*>(fromSlot),
            to,reinterpret_cast<void*>(toElement),reinterpret_cast<void*>(toSlot),first,second) == 0x40000000U;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeRouteResult::Append(std::uintptr_t result, const Ref& ref) noexcept {
    __try {
        if (append_) return append_(callsContext_,result,ref.object,ref.control);
        reinterpret_cast<void(__thiscall*)(void*,const Ref*)>(base_+kAppend)(reinterpret_cast<void*>(result+0x38),&ref);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeRouteResult::Release(Ref& ref) noexcept {
    __try {
        if (release_) return release_(callsContext_,ref.object,ref.control);
        reinterpret_cast<void(__thiscall*)(Ref*)>(base_+kRelease)(&ref); return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeRouteResult::NodeValues(const Ref& ref, const Node& node, std::uint32_t reference, std::uintptr_t link) noexcept {
    __try {
        const auto at = ref.object;
        if (!Pair(at,ref.control) || *reinterpret_cast<const std::uintptr_t*>(at) != base_+kNodeType ||
            *reinterpret_cast<const std::uint32_t*>(at+4) != 3 ||
            *reinterpret_cast<const std::uintptr_t*>(at+0x30) != 0 ||
            *reinterpret_cast<const std::uintptr_t*>(at+0x34) != *reinterpret_cast<const std::uintptr_t*>(at+0x38)) return false;
        *reinterpret_cast<std::uint32_t*>(at+4) = node.type;
        *reinterpret_cast<std::uintptr_t*>(at+0x30) = link;
        *reinterpret_cast<std::uint32_t*>(at+8) = reference;
        for (std::size_t i = 0; i < 9; ++i) *reinterpret_cast<float*>(at+0xC+i*4) = node.vectors[i];
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeRouteResult::Publish(const Graph& graph, const Protection& protection, const LocalContext& local) noexcept {
    __try {
        for (std::size_t i = 0; i < 6; ++i) *reinterpret_cast<float*>(local.result+0x10+i*4) = graph.auxiliary[i];
        for (std::size_t i = 0; i < 4; ++i) *reinterpret_cast<std::uint32_t*>(local.result+0x28+i*4) = graph.statuses[i];
        *reinterpret_cast<std::uint32_t*>(local.result+0xC) = 1;
        *reinterpret_cast<std::uint32_t*>(local.result+8) = protection.localWorldRevision;
        // Exactly the scheduler's token-before-ready publication00455433..42.
        InterlockedExchange(reinterpret_cast<volatile LONG*>(local.result+4),1);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
Result NativeRouteResult::Capture(const Key& key, const LocalContext& local, const Protection& supplied,
    std::span<const route_identity::BindingIncarnation> bindings, Graph& output) noexcept {
    if (busy_) return Result::Reentrant;
    if (base_ == 0 || registry_ == nullptr) return Result::NotInitialized;
    if (!ValidKey(key)) return Result::InvalidKey;
    const Busy lock(busy_);
    // Own protection even when a callback releases the caller's last owner.
    const Protection protection = supplied;
    if (!Protected(key,local,protection,{},GuardPhase::Read)) return Result::LifetimeLost;
    if (bindings.empty() || bindings.size() > route_identity::kMaxBindings || !Context(local,true)) return Result::InvalidNativeState;
    Graph graph{}; graph.key = key;
    for(std::size_t i=0;i<3;++i)if(!Read(local.block+i*4,graph.goal[i]))return Result::NativeFault;
    std::array<route_identity::ElementMapping,kMaxNodes> mappings{};
    std::array<std::uintptr_t,kMaxNodes> seenNodes{};
    std::uintptr_t sentinel{}, link{}, previous{};
    std::uint32_t count{};
    if (!Read(local.result+0x38,sentinel) || !Read(sentinel,link) || !Read(local.result+0x3C,count)) return Result::NativeFault;
    previous = sentinel;
    for (std::size_t i = 0; i < count; ++i) {
        if (!Protected(key,local,protection,std::span(mappings).first(i),GuardPhase::Read)) return Result::LifetimeLost;
        std::uintptr_t before{}, next{}, object{}, control{}, type{}, borrowed{}, begin{}, end{};
        std::uint32_t kind{}, reference{};
        if (link == 0 || link == sentinel || !Read(link+4,before) || before != previous || !Read(link,next) ||
            !Read(link+8,object) || !Read(link+0xC,control) || !Pair(object,control) ||
            !Read(object,type) || type != base_+kNodeType || !Read(object+4,kind) || (kind != 1 && kind != 2) ||
            !Read(object+0x30,borrowed) || !Read(object+0x34,begin) || !Read(object+0x38,end) || begin != end ||
            !Read(object+8,reference)) return Result::UnsupportedDomain;
        for (std::size_t j = 0; j < i; ++j) if (seenNodes[j] == object) return Result::UnsupportedDomain;
        seenNodes[i] = object;
        route_identity::ElementKey portable{};
        std::size_t found = 0;
        for (const auto& binding : bindings) {
            route_identity::ElementKey candidate{};
            if (registry_->ExportElement(binding,reference,candidate) == route_identity::Result::Ok) { portable = candidate; ++found; }
        }
        if (found != 1 || registry_->MapElement(portable,mappings[i]) != route_identity::Result::Ok) return Result::MissingResource;
        Node capturedNode{};capturedNode.type=kind;
        if (!MatchesNode(protection,capturedNode,mappings[i],borrowed)) return Result::LocalInvalidity;
        if (i != 0 && !Adjacent(protection,mappings[i-1].localReference,reference)) return Result::LocalInvalidity;
        if (!Protected(key,local,protection,std::span(mappings).first(i+1),GuardPhase::Read)) return Result::LifetimeLost;
        auto& node = graph.nodes[i]; node.type=kind; node.ordinal = portable.ordinal;
        std::size_t resource = 0;
        for (; resource < graph.resourceCount; ++resource) if (graph.resources[resource] == portable.resource) break;
        if (resource == graph.resourceCount) {
            if (resource == kMaxResources) return Result::UnsupportedDomain;
            graph.resources[graph.resourceCount++] = portable.resource;
        }
        node.resourceIndex = static_cast<std::uint32_t>(resource);
        for (std::size_t j = 0; j < 9; ++j) if (!Read(object+0xC+j*4,node.vectors[j])) return Result::NativeFault;
        previous = link; link = next;
    }
    std::uintptr_t last{};
    if (link != sentinel || !Read(sentinel+4,last) || last != previous) return Result::InvalidNativeState;
    graph.nodeCount = count;
    for (std::size_t i = 0; i < 6; ++i) if (!Read(local.result+0x10+i*4,graph.auxiliary[i])) return Result::NativeFault;
    for (std::size_t i = 0; i < 4; ++i) if (!Read(local.result+0x28+i*4,graph.statuses[i])) return Result::NativeFault;
    const auto valid = Validate(graph);
    if (valid != Result::Ok) return valid;
    if (!Protected(key,local,protection,std::span(mappings).first(count),GuardPhase::Read) || !Context(local,true)) return Result::LifetimeLost;
    output = graph; return Result::Ok;
}
Result NativeRouteResult::CaptureInstalled(const Key& key,const LocalContext& local,const Protection& supplied,
    const route_identity::BindingIncarnation& binding,const std::array<float,6>& auxiliary,Graph& output,bool requireComplete) noexcept {
    contextCheck_=0;captureNode_=markerIndex_=markerKind_=markerTypeRva_=UINT32_MAX;
    if(busy_)return Result::Reentrant;
    if(!base_||!registry_)return Result::NotInitialized;
    if(!ValidKey(key))return Result::InvalidKey;
    const Busy lock(busy_);
    const Protection protection=supplied;
    if(!Protected(key,local,protection,{},GuardPhase::Read))return Result::LifetimeLost;
    contextCheck_=100;
    std::uintptr_t sentinel{},cursor{},link{},previous{},result{},current{},resultControl{},prior{},blockHead{},blockLink{},registered{};std::uint32_t count{},state{},blocks{},mode{},stop{};
    const auto n=local.navigation,b=local.block;
    if(local.result||local.resultControl||!Read(n+0x5C,result)||!Read(n+0x60,resultControl)||
        !Read(n+0x40,current)||!Read(n+0x44,prior)||!Read(b+0x40,state))return Result::InvalidNativeState;
    // Query/result ownership is a transient admission state, not an unknown
    // route format. In particular 0044D320 can call 0044D200 before returning.
    if(result||resultControl||current||prior||state!=3)return Result::Deferred;
    std::uint8_t reset{},dirty{};
    if(!Read(n+0x48,reset)||!Read(b+0x90,dirty))return Result::NativeFault;
    if(reset||(dirty&1U))return Result::Deferred;
    contextCheck_=101;
    // N+24 is execution progress, cleared by 0045A320 when advancing a node.
    // It is not a solver reference. A read-only export must not require it
    // to be zero (the import/retirement gates remain independently strict).
    if(!Read(n+0x2C,blocks)||!blocks||blocks>128||
        !Read(n+0x28,blockHead)||!blockHead||!Read(blockHead,blockLink)||blockLink==blockHead||
        !Read(b+0x48,mode)||mode||!Read(b+0x30,stop)||!std::isfinite(std::bit_cast<float>(stop))||
        (std::bit_cast<float>(stop)<0&&!StopTrimDisabled(stop))||!Read(n,sentinel)||!sentinel||
        !Read(n+4,count)||count>kMaxNodes*2||!Read(n+0xC,cursor)||!Read(sentinel,link))return Result::UnsupportedDomain;
    if(!count||cursor==sentinel)return Result::Deferred;
    contextCheck_=102;
    std::uint32_t matches{};
    for(std::uint32_t i=0;i<blocks;++i) {
        std::uintptr_t next{};
        if(!blockLink||blockLink==blockHead||!Read(blockLink+8,registered)||!Read(blockLink,next))return Result::InvalidNativeState;
        if(registered==b)++matches;blockLink=next;
    }
    if(blockLink!=blockHead||matches!=1)return Result::InvalidNativeState;
    std::uintptr_t start{},finish{};
    if(!Read(b+0x38,start)||!Read(b+0x3C,finish)||!start||!finish)return Result::InvalidNativeState;
    if(start==sentinel||finish==sentinel)return Result::Deferred;
    contextCheck_=103;
    std::uint32_t startIndex=UINT32_MAX,endIndex=UINT32_MAX,cursorIndex=UINT32_MAX;
    auto scan=link;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t next{};
        if(!scan||scan==sentinel||!Read(scan,next))return Result::InvalidNativeState;
        if(scan==start)startIndex=i;
        if(scan==finish)endIndex=i;
        if(scan==cursor)cursorIndex=i;
        scan=next;
    }
    if(scan!=sentinel||startIndex==UINT32_MAX||endIndex==UINT32_MAX||cursorIndex==UINT32_MAX||
        startIndex>endIndex)return Result::InvalidNativeState;
    if(cursorIndex>endIndex)return Result::Deferred;
    // Only the immediate post-install seam can certify the first full range.
    // Later endpoint exports may observe a suffix, joined to the retained graph.
    if(requireComplete&&cursorIndex>startIndex){contextCheck_=109;return Result::LocalInvalidity;}
    startIndex=(std::max)(startIndex,cursorIndex);
    Graph graph{};graph.key=key;graph.auxiliary=auxiliary;graph.statuses.fill(0x40000000U);
    for(std::size_t i=0;i<3;++i)if(!Read(b+i*4,graph.goal[i])||!Read(n+0x50+i*4,graph.auxiliary[i+3]))return Result::NativeFault;
    std::array<route_identity::ElementMapping,kMaxNodes> mappings{};std::array<std::uintptr_t,kMaxNodes> seen{};
    previous=sentinel;
    for(std::size_t i=0;i<count;++i) {
        if(!Protected(key,local,protection,std::span(mappings).first(graph.nodeCount),GuardPhase::Read))return Result::LifetimeLost;
        std::uintptr_t before{},next{};
        if(!link||link==sentinel||!Read(link+4,before)||before!=previous||!Read(link,next))return Result::InvalidNativeState;
        if(i>=startIndex&&i<=endIndex) {
            captureNode_=static_cast<std::uint32_t>(graph.nodeCount);
            contextCheck_=104;
            if(graph.nodeCount>=kMaxNodes)return Result::UnsupportedDomain;
            std::uintptr_t object{},control{},type{},borrowed{},begin{},end{};std::uint32_t kind{},reference{};
            contextCheck_=1040;
            if(!Read(link+8,object)||!Read(link+12,control)||!Pair(object,control))return Result::UnsupportedDomain;
            contextCheck_=1041;
            if(!Read(object,type)||type!=base_+kNodeType)return Result::UnsupportedDomain;
            contextCheck_=1042;
            if(!Read(object+4,kind)||kind>2)return Result::UnsupportedDomain;
            contextCheck_=1043;
            if(!Read(object+0x30,borrowed)||!Read(object+0x34,begin)||!Read(object+0x38,end)||
                end<begin||(end-begin)%8||(end-begin)>32||!Read(object+8,reference))return Result::UnsupportedDomain;
            contextCheck_=105;
            std::uint32_t markers{};if(!ReadMarkers(object,markers))return Result::UnsupportedDomain;
            contextCheck_=106;
            for(std::size_t j=0;j<graph.nodeCount;++j)if(seen[j]==object)return Result::UnsupportedDomain;
            seen[graph.nodeCount]=object;
            route_identity::ElementKey portable{};auto& mapping=mappings[graph.nodeCount];
            contextCheck_=107;
            if(registry_->ExportElement(binding,reference,portable)!=route_identity::Result::Ok||
                registry_->MapElement(portable,mapping)!=route_identity::Result::Ok)return Result::MissingResource;
            Node capturedNode{};capturedNode.type=kind;
            contextCheck_=108;
            if(!MatchesNode(protection,capturedNode,mapping,borrowed)||(graph.nodeCount&&!Adjacent(protection,mappings[graph.nodeCount-1].localReference,reference)))return Result::LocalInvalidity;
            if(!graph.resourceCount){graph.resources[0]=portable.resource;graph.resourceCount=1;}
            contextCheck_=110;
            if(graph.resources[0]!=portable.resource)return Result::UnsupportedDomain;
            auto& node=graph.nodes[graph.nodeCount++];node.type=kind;node.ordinal=portable.ordinal;node.markers=markers;
            for(std::size_t j=0;j<9;++j)if(!Read(object+0xC+j*4,node.vectors[j]))return Result::NativeFault;
        }
        previous=link;link=next;
    }
    std::uintptr_t last{};
    if(!graph.nodeCount||link!=sentinel||!Read(sentinel+4,last)||last!=previous)return Result::InvalidNativeState;
    contextCheck_=111;
    if(graph.failure!=Failure::None)return Result::UnsupportedDomain;
    const auto valid=Validate(graph);if(valid!=Result::Ok)return valid;
    std::uintptr_t endCursor{};std::uint32_t endState{};
    if(!Protected(key,local,protection,std::span(mappings).first(graph.nodeCount),GuardPhase::Read)||
        !Read(n+0xC,endCursor)||endCursor!=cursor||!Read(b+0x40,endState)||endState!=3)return Result::LifetimeLost;
    contextCheck_=0;output=graph;return Result::Ok;
}
Result NativeRouteResult::Preflight(const Graph& graph,const LocalContext& local,const Protection& protection) noexcept {
    importStage_=40;importNode_=UINT32_MAX;contextCheck_=0;
    if(busy_)return Result::Reentrant;
    if(!base_||!registry_)return Result::NotInitialized;
    if(graph.failure!=Failure::None)return Result::UnsupportedDomain;
    const auto valid=Validate(graph);if(valid!=Result::Ok)return valid;
    const Busy lock(busy_);
    std::array<route_identity::ElementMapping,kMaxNodes> mappings{};
    for(std::size_t i=0;i<graph.nodeCount;++i) {
        importNode_=static_cast<std::uint32_t>(i);importStage_=41;
        if(!Protected(graph.key,local,protection,std::span(mappings).first(i),GuardPhase::Preflight))return Result::LifetimeLost;
        const auto& node=graph.nodes[i];
        importStage_=42;
        if(registry_->MapElement({graph.resources[node.resourceIndex],node.ordinal},mappings[i])!=route_identity::Result::Ok)return Result::MissingResource;
        std::uintptr_t borrowed{};
        importStage_=43;
        if((node.type==1&&!ResolveSpecial(protection,mappings[i].localReference,borrowed))||
            !MatchesNode(protection,node,mappings[i],borrowed))return Result::LocalInvalidity;
        importStage_=44;
        if(i&&!Adjacent(protection,mappings[i-1].localReference,mappings[i].localReference))return Result::LocalInvalidity;
    }
    importStage_=45;
    return Protected(graph.key,local,protection,std::span(mappings).first(graph.nodeCount),GuardPhase::Preflight)?Result::Ok:Result::LifetimeLost;
}
Result NativeRouteResult::CertifyAdmission(const Graph& graph,const LocalContext& local,const Protection& supplied,
    AdmissionPlan& output,ImportPoint point) noexcept {
    if(busy_)return Result::Reentrant;
    const auto valid=Preflight(graph,local,supplied);if(valid!=Result::Ok)return valid;
    const Busy lock(busy_);const Protection protection=supplied;
    AdmissionPlan plan{};const auto result=PlanAdmission(graph,local,protection,plan,point,0);
    if(result==Result::Ok)output=plan;
    return result;
}
Result NativeRouteResult::PlanAdmission(const Graph& graph,const LocalContext& local,const Protection& protection,
    AdmissionPlan& output,ImportPoint point,std::size_t resultCount) noexcept {
    if(!Protected(graph.key,local,protection,{},GuardPhase::Read))return Result::LifetimeLost;
    if(!Context(local,false,resultCount,point,true))return Result::InvalidNativeState;
    AdmissionPlan plan{};const auto n=local.navigation;
    std::uint8_t reset{},flags{},previousFlags{};
    if(!Read(n,plan.sentinel)||!Read(n+4,plan.existingNodeCount)||!Read(plan.sentinel,plan.first)||
        !Read(plan.sentinel+4,plan.tail)||!Read(n+0xC,plan.cursor)||!Read(n+0x44,plan.previousBlock)||
        !Read(n+0x48,reset)||!Read(local.block+0x90,flags)||
        (plan.previousBlock&&!Read(plan.previousBlock+0x90,previousFlags)))return Result::NativeFault;
    plan.incomingCount=graph.nodeCount;
    if(reset||(flags&1U)||(previousFlags&0x80U)) {output=plan;return Result::Ok;}
    plan.mode=plan.existingNodeCount?AdmissionMode::Append:AdmissionMode::AppendEmpty;
    if(!plan.existingNodeCount) {
        if(plan.cursor!=plan.sentinel)return Result::InvalidNativeState;
        output=plan;return Result::Ok;
    }
    // 004659D0 shares precisely the OLD TAIL, not an arbitrary overlapping
    // prefix. Earlier nodes/ranges, history and cursor remain native-owned.
    std::array<std::uintptr_t,kMaxNodes*2> links{};
    auto link=plan.first;
    for(std::uint32_t i=0;i<plan.existingNodeCount;++i) {
        links[i]=link;if(link==plan.cursor)plan.cursorIndex=i;
        if(!Read(link,link))return Result::NativeFault;
    }
    if(plan.cursor!=plan.sentinel&&plan.cursorIndex==UINT32_MAX)return Result::InvalidNativeState;
    // Validate every live B range before allowing a shared-list append. A
    // retired previous B can still be registered; its ranges are not reused.
    std::uintptr_t head{},at{};std::uint32_t count{};
    if(!Read(n+0x28,head)||!Read(head,at)||!Read(n+0x2C,count))return Result::NativeFault;
    std::array<std::uintptr_t,128> blocks{};
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t b{},begin{},end{};std::uint32_t reason{},state{},mode{};
        if(!Read(at+8,b)||!b||std::find(blocks.begin(),blocks.begin()+i,b)!=blocks.begin()+i||
            !Read(b+0x44,reason)||!Read(b+0x40,state)||!Read(b+0x48,mode)||mode)return Result::InvalidNativeState;
        blocks[i]=b;
        if(b!=local.block&&reason!=3&&state==3) {
            if(!Read(b+0x38,begin)||!Read(b+0x3C,end))return Result::NativeFault;
            const auto first=std::find(links.begin(),links.begin()+plan.existingNodeCount,begin);
            const auto last=std::find(links.begin(),links.begin()+plan.existingNodeCount,end);
            if(first==links.begin()+plan.existingNodeCount||last==links.begin()+plan.existingNodeCount||first>last)return Result::InvalidNativeState;
        }
        if(!Read(at,at))return Result::NativeFault;
    }
    std::uintptr_t object{},control{},type{},borrowed{};std::uint32_t kind{},reference{};
    if(!Read(plan.tail+8,object)||!Read(plan.tail+12,control)||!Pair(object,control)||
        !Read(object,type)||type!=base_+kNodeType||!Read(object+4,kind)||!Read(object+8,reference)||
        !Read(object+0x30,borrowed)||!ReadMarkers(object,plan.tailMarkers))return Result::UnsupportedDomain;
    const auto& node=graph.nodes[0];route_identity::ElementMapping mapping{};
    if(registry_->MapElement({graph.resources[node.resourceIndex],node.ordinal},mapping)!=route_identity::Result::Ok)return Result::MissingResource;
    if(kind!=node.type||reference!=mapping.localReference||!MatchesNode(protection,node,mapping,borrowed))return Result::LocalInvalidity;
    for(std::size_t i=0;i<9;++i) {
        std::uint32_t bits{};
        if(!Read(object+0xC+i*4,bits)||bits!=std::bit_cast<std::uint32_t>(node.vectors[i]))return Result::LocalInvalidity;
    }
    plan.prefixCount=plan.incomingOffset=1;plan.incomingCount=graph.nodeCount-1;
    auto expected=plan.tailMarkers;
    if(!plan.incomingCount) {
        unsigned shift{};for(auto markers=expected;markers;markers>>=8)shift+=8;
        if(shift>=32)return Result::UnsupportedDomain;
        expected|=11U<<shift;plan.tailMarkerDelta=11;
    }
    if(expected!=node.markers)return Result::LocalInvalidity;
    if(!Protected(graph.key,local,protection,std::span(&mapping,1),GuardPhase::Read))return Result::LifetimeLost;
    output=plan;return Result::Ok;
}
Result NativeRouteResult::Rearm(const Key& previous,const Graph& graph,const LocalContext& local,const Protection& protection) noexcept {
    if(busy_)return Result::Reentrant;
    if(!entered_||!published_||retired_||previous!=key_||pending_.object||pending_.control||pendingMarker_.object||pendingMarker_.control)return Result::InvalidNativeState;
    const auto& next=graph.key;
    if(graph.failure!=Failure::None||Validate(graph)!=Result::Ok||next.scope!=key_.scope||next.owner!=key_.owner||next.commandId!=key_.commandId||next.part!=key_.part||
        !((next.parameterRevision==key_.parameterRevision&&next.routeRevision>key_.routeRevision)||
          next.parameterRevision>key_.parameterRevision))return Result::WrongKey;
    const Busy lock(busy_);
    if(local.navigation!=local_.navigation||local.block!=local_.block||local.result==local_.result||
        protection.mesh!=protection_.mesh||
        protection.storage!=protection_.storage||protection.storage.owner_before(protection_.storage)||protection_.storage.owner_before(protection.storage)||
        !Protected(key_,local_,protection_,{},GuardPhase::Rebind))return Result::LifetimeLost;
    AdmissionPlan plan{};
    if(PlanAdmission(graph,local,protection,plan,ImportPoint::ReplacementNoWorker,0)!=Result::Ok)return Result::LifetimeLost;
    // Never drop the backing-storage owner between old history and new nodes.
    protection_=protection;key_=next;local_=local;entered_=published_=false;
    return Result::Ok;
}
Result NativeRouteResult::RetainContinuation(const Graph& authoritative,const LocalContext& installed,
    const Protection& protection,Graph& output) noexcept {
    if(busy_)return Result::Reentrant;
    continuation_.reset();
    if(!entered_||!published_||retired_||authoritative.key!=key_||Validate(authoritative)!=Result::Ok||
        authoritative.failure!=Failure::None||installed.navigation!=local_.navigation||installed.block!=local_.block||
        installed.result||installed.resultControl||protection.storage!=protection_.storage||
        protection.storage.owner_before(protection_.storage)||protection_.storage.owner_before(protection.storage)||
        protection.mesh!=protection_.mesh)return Result::InvalidNativeState;
    route_identity::ElementMapping mapping{};
    const auto& first=authoritative.nodes[0];
    if(registry_->MapElement({authoritative.resources[first.resourceIndex],first.ordinal},mapping)!=route_identity::Result::Ok)return Result::MissingResource;
    Graph captured{};
    const auto read=CaptureInstalled(key_,installed,protection,mapping.binding,authoritative.auxiliary,captured);
    if(read!=Result::Ok)return read;
    if(captured.nodeCount>authoritative.nodeCount||!SameFloatBits(captured.goal,authoritative.goal))return Result::LocalInvalidity;
    const auto offset=authoritative.nodeCount-captured.nodeCount;
    for(std::size_t i=0;i<captured.nodeCount;++i) {
        auto expected=authoritative.nodes[offset+i];const auto actual=captured.nodes[i];
        if(authoritative.resources[expected.resourceIndex]!=captured.resources[actual.resourceIndex])return Result::LocalInvalidity;
        expected.resourceIndex=actual.resourceIndex;
        if(expected!=actual||!SameFloatBits(expected.vectors,actual.vectors))return Result::LocalInvalidity;
    }
    Graph remaining=authoritative;remaining.nodeCount=captured.nodeCount;remaining.nodes={};
    std::copy_n(authoritative.nodes.begin()+offset,remaining.nodeCount,remaining.nodes.begin());
    try {continuation_=std::make_unique<Graph>(remaining);}catch(...) {return Result::NativeFault;}
    output=remaining;return Result::Ok;
}
Result NativeRouteResult::RearmContinuation(const Graph& graph,const LocalContext& local,const Protection& protection) noexcept {
    if(busy_)return Result::Reentrant;
    if(!continuation_||!SameGraphBits(graph,*continuation_)||!entered_||!published_||retired_||graph.key!=key_||
        pending_.object||pending_.control||pendingMarker_.object||pendingMarker_.control)return Result::InvalidNativeState;
    const Busy lock(busy_);
    if(local.navigation!=local_.navigation||local.block!=local_.block||local.result==local_.result||
        protection.mesh!=protection_.mesh||protection.storage!=protection_.storage||
        protection.storage.owner_before(protection_.storage)||protection_.storage.owner_before(protection.storage)||
        !Protected(key_,local_,protection_,{},GuardPhase::Rebind))return Result::LifetimeLost;
    AdmissionPlan plan{};
    if(PlanAdmission(graph,local,protection,plan,ImportPoint::ReplacementNoWorker,0)!=Result::Ok)return Result::LifetimeLost;
    protection_=protection;local_=local;entered_=published_=false;continuation_.reset();
    return Result::Ok;
}
Result NativeRouteResult::Populate(const Graph& graph, const LocalContext& local, const Protection& supplied, ImportPoint point) noexcept {
    if (busy_) return Result::Reentrant;
    importNode_=UINT32_MAX;
    importStage_=2;
    if (entered_) return Result::AlreadyEntered;
    importStage_=3;
    if (base_ == 0 || registry_ == nullptr) return Result::NotInitialized;
    importStage_=4;
    if(graph.failure!=Failure::None)return Result::UnsupportedDomain;
    const auto valid = Validate(graph);
    importStage_=5;
    if (valid != Result::Ok) return valid;
    const Busy lock(busy_);
    const Protection protection = supplied;
    importStage_=6;
    if (!Protected(graph.key,local,protection,{},GuardPhase::Read)) return Result::LifetimeLost;
    importStage_=7;
    AdmissionPlan admission{};
    const auto admitted=PlanAdmission(graph,local,protection,admission,point,0);
    if(admitted!=Result::Ok)return admitted;
    std::array<route_identity::ElementMapping,kMaxNodes> mappings{};
    std::array<std::uintptr_t,kMaxNodes> borrowedLinks{};
    for (std::size_t i = 0; i < graph.nodeCount; ++i) {
        importNode_=static_cast<std::uint32_t>(i);
        importStage_=8;
        if (!Protected(graph.key,local,protection,std::span(mappings).first(i),GuardPhase::Read)) return Result::LifetimeLost;
        const auto& node = graph.nodes[i];
        importStage_=9;
        if (registry_->MapElement({graph.resources[node.resourceIndex],node.ordinal},mappings[i]) != route_identity::Result::Ok) return Result::MissingResource;
        importStage_=10;
        if ((node.type==1&&!ResolveSpecial(protection,mappings[i].localReference,borrowedLinks[i]))||
            !MatchesNode(protection,node,mappings[i],borrowedLinks[i])) return Result::LocalInvalidity;
        importStage_=11;
        if (i != 0 && !Adjacent(protection,mappings[i-1].localReference,mappings[i].localReference)) return Result::LocalInvalidity;
    }
    const auto mapped = std::span(mappings).first(graph.nodeCount);
    importStage_=12;
    if (!Protected(graph.key,local,protection,mapped,GuardPhase::Create)) return Result::LifetimeLost;
    // Fence before the FIRST native allocation. Partial faults cannot retry.
    protection_ = protection; key_ = graph.key; local_ = local; importPoint_ = point; entered_ = true;
    const auto stable=[&](std::size_t count) noexcept {
        AdmissionPlan current{};
        return PlanAdmission(graph,local_,protection_,current,importPoint_,count)==Result::Ok&&current==admission;
    };
    for (std::size_t i = admission.incomingOffset; i < graph.nodeCount; ++i) {
        importNode_=static_cast<std::uint32_t>(i);
        importStage_=13;
        if (!Protected(key_,local_,protection_,mapped,GuardPhase::Create) || !stable(i-admission.incomingOffset)) return Result::LifetimeLost;
        importStage_=14;
        if (!Create(pending_)) return Result::NativeFault;
        importStage_=15;
        if (!Protected(key_,local_,protection_,mapped,GuardPhase::Create)) return Result::LifetimeLost;
        importStage_=16;
        if (!MatchesNode(protection_,graph.nodes[i],mappings[i],borrowedLinks[i])) return Result::LocalInvalidity;
        importStage_=17;
        if (!NodeValues(pending_,graph.nodes[i],mappings[i].localReference,borrowedLinks[i])) return Result::NativeFault;
        for(auto markers=graph.nodes[i].markers;markers;markers>>=8) {
            importStage_=18;
            if(!Protected(key_,local_,protection_,mapped,GuardPhase::Create))return Result::LifetimeLost;
            importStage_=19;
            if(!Marker(markers&255U,pendingMarker_))return Result::NativeFault;
            importStage_=20;
            if(!Protected(key_,local_,protection_,mapped,GuardPhase::Create))return Result::LifetimeLost;
            importStage_=21;
            if(!AttachMarker(pending_.object,pendingMarker_))return Result::NativeFault;
            importStage_=22;
            if(!Protected(key_,local_,protection_,mapped,GuardPhase::Create))return Result::LifetimeLost;
            importStage_=23;
            if(!Release(pendingMarker_))return Result::NativeFault;
            pendingMarker_={};
        }
        std::uint32_t restoredMarkers{};
        importStage_=24;
        if(!ReadMarkers(pending_.object,restoredMarkers)||restoredMarkers!=graph.nodes[i].markers)return Result::NativeFault;
        importStage_=25;
        if (!Append(local_.result,pending_)) return Result::NativeFault;
        importStage_=26;
        if (!Protected(key_,local_,protection_,mapped,GuardPhase::Create) || !stable(i+1-admission.incomingOffset)) return Result::LifetimeLost;
        importStage_=27;
        if (!Release(pending_)) return Result::NativeFault;
        pending_ = {};
    }
    importStage_=28;
    if (!Protected(key_,local_,protection_,mapped,GuardPhase::Publish) || !stable(admission.incomingCount)) return Result::LifetimeLost;
    for(std::size_t i=0;i<graph.nodeCount;++i) {
        importNode_=static_cast<std::uint32_t>(i);
        importStage_=29;
        if(!MatchesNode(protection_,graph.nodes[i],mappings[i],borrowedLinks[i]))return Result::LocalInvalidity;
    }
    importStage_=30;
    if (!Publish(graph,protection_,local_)) return Result::NativeFault;
    importStage_=31;
    published_ = true; return Result::Ok;
}
Result NativeRouteResult::Retire(const Key& key) noexcept {
    if (busy_) return Result::Reentrant;
    if (!entered_ || key != key_) return Result::WrongKey;
    if (retired_) return Result::Ok;
    const Busy lock(busy_);
    // Retire is a local quiescence proof. Never dereference potentially dead
    // N/B/R after the guard; it must prove native nodes/workers no longer use
    // this storage. A partial temporary Release is not automatically repeated.
    if (!Protected(key_,local_,protection_,{},GuardPhase::Retire)) return Result::LifetimeLost;
    if (pending_.object != 0 || pending_.control != 0 || pendingMarker_.object || pendingMarker_.control) return Result::NativeFault;
    retired_ = true; protection_ = {}; return Result::Ok;
}
} // namespace aimp::ai_sync::route
