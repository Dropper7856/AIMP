#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
namespace aimp::alien_pvp::action_abi {
// First eight unhooked bytes from the supplied exact-build AI.exe, not a
// signature scan. Existing AISync pair entries and Move(0070A940) are
// validated before installation by their mandatory hooks, so their rewritten
// entry bytes must not be compared here. HIGHLOW operands follow ASLR.
struct Signature {std::uintptr_t rva;std::array<std::uint8_t,8> bytes;unsigned relocationOffset{};};
inline constexpr std::array<Signature,16> kSignatures{{
    {0x0000F5C0U,{0x53U,0x8BU,0x5CU,0x24U,0x0CU,0x83U,0x7BU,0x04U}}, // donor entry Update
    {0x0000FB10U,{0x83U,0xECU,0x0CU,0x53U,0x8BU,0x5CU,0x24U,0x18U}}, // donor exit Update
    {0x00318810U,{0xE9U,0x6BU,0x27U,0xFFU,0xFFU,0xCCU,0xCCU,0xCCU}}, // ordinary Move shutdown
    {0x0000F680U,{0x8BU,0xC1U,0x33U,0xC9U,0x89U,0x48U,0x04U,0xC7U}}, // entry constructor
    {0x0000FC50U,{0x8BU,0xC1U,0x33U,0xC9U,0x89U,0x48U,0x04U,0xC7U}}, // exit constructor
    {0x002F15C0U,{0x56U,0x8BU,0x35U,0x20U,0x1CU,0x6BU,0x01U,0x33U},3U}, // pooled frame
    {0x002D0260U,{0x56U,0x8BU,0x74U,0x24U,0x0CU,0x80U,0x7EU,0x1FU}}, // node start dispatch
    {0x0030A7C0U,{0x6AU,0xFFU,0x68U,0x40U,0xEEU,0x11U,0x01U,0x64U},3U}, // cancel request
    {0x0030A5E0U,{0x6AU,0xFFU,0x68U,0xB0U,0x95U,0x12U,0x01U,0x64U},3U}, // cancel completion
    {0x0000FBC0U,{0x8BU,0x44U,0x24U,0x08U,0x56U,0x57U,0x8BU,0x7CU}}, // shutdown
    {0x002FA8D0U,{0x56U,0x8BU,0xF1U,0xE8U,0x78U,0x3BU,0xD1U,0xFFU}}, // pooled destructor
    {0x0021CCC0U,{0x6AU,0xFFU,0x68U,0x88U,0x51U,0x12U,0x01U,0x64U},3U}, // portal exit
    {0x002F3A30U,{0x55U,0x8BU,0xECU,0x83U,0xE4U,0xF0U,0x6AU,0xFFU}}, // Alien explicit target query
    {0x00310C60U,{0x8BU,0x81U,0xC8U,0x01U,0x00U,0x00U,0x83U,0xF8U}}, // Alien attack selector
    {0x003EB940U,{0x8BU,0x01U,0xC1U,0xE8U,0x1CU,0x50U,0x51U,0xE8U}}, // native ray handle valid
    {0x003EB960U,{0x8BU,0x01U,0xC1U,0xE8U,0x1CU,0x50U,0x51U,0xE8U}}, // native ray handle status
}};
using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t) noexcept;
inline std::array<std::uint8_t,8> Expected(const Signature& signature,std::uintptr_t base) noexcept {
    auto expected=signature.bytes;
    if(signature.relocationOffset) {
        const auto at=signature.relocationOffset;std::uint32_t address{};
        for(unsigned i=0;i<4U;++i)address|=std::uint32_t(expected[at+i])<<(i*8U);
        address+=static_cast<std::uint32_t>(base)-0x00400000U;
        for(unsigned i=0;i<4U;++i)expected[at+i]=static_cast<std::uint8_t>(address>>(i*8U));
    }
    return expected;
}
inline bool Matches(std::uintptr_t base,void* context,Reader read) noexcept {
    if(!base || !read)return false;
    for(const auto& signature:kSignatures) {
        std::array<std::uint8_t,8> observed{};
        if(!read(context,base+signature.rva,observed.data(),observed.size()) || observed!=Expected(signature,base))return false;
    }
    return true;
}
} // namespace aimp::alien_pvp::action_abi
