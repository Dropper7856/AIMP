#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace aimp::alien_pvp::vent_node {
using Methods=std::array<std::uint32_t,0x448U/4U>;
using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t) noexcept;
// The retail entry node's +40C callback authors an exclusion sphere around
// THE_PLAYER (0040F6B0). The donor hides that technical player below the map.
// On a multiplayer Host THE_PLAYER is a real survivor, so that sphere can
// exclude the very opening we are approaching. Reuse the exit node's native
// "no exclusion geometry" callback for this private manually controlled node.
// Start, Update, Shutdown, passage and all other native methods stay retail.
inline bool PilotEntryMethods(std::uintptr_t base,void* context,Reader read,Methods& out) noexcept {
    std::uint32_t noExclusion{};std::array<std::uint8_t,5> code{};
    if(!base || !read || !read(context,base+0xDBC498U,out.data(),sizeof(out)) ||
        out[0x40CU/4U]!=base+0xF6B0U ||
        !read(context,base+0xDBC8E0U+0x40CU,&noExclusion,sizeof(noExclusion)) ||
        noExclusion!=base+0x9931C0U || !read(context,noExclusion,code.data(),code.size()) ||
        code!=std::array<std::uint8_t,5>{0x32U,0xC0U,0xC2U,0x14U,0U})return false;
    out[0x40CU/4U]=noExclusion;return true;
}
}
