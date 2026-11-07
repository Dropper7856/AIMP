#pragma once
#include "aimp/AlienPvPWire.h"
#include <cstdint>
namespace aimp::alien_pvp {
struct Context;
// Local callbacks installed only by the certified AISync engine-thread owner.
// No pointer is serialized. A simulated host can exercise the real controller
// through the same seam without claiming that the retail ABI was executed.
struct ActionHooks final {
    bool (*begin)(const Context&,const Frame&,Frame& result) noexcept{};
    bool (*poll)(const Context&,Frame& result) noexcept{};
    bool (*busy)(const Context&) noexcept{};
    void (*cancel)(const Context&,bool living) noexcept{};
    // Authenticated pose recovery uses a native navigation request, not a
    // position write. A null goal asks to retire it and returns while busy.
    bool (*recover)(const Context&,const Frame* goal) noexcept{};
    bool (*cancelRecovery)(const Context&,bool living) noexcept{};
};
} // namespace aimp::alien_pvp
