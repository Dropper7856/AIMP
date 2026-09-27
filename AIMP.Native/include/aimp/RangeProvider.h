#pragma once

#include "aimp/Common.h"

namespace aimp {

class RangeProvider final {
public:
    [[nodiscard]] static constexpr ModuleReport Report() noexcept {
        return {
            "RangeProvider",
            ModuleStatus::BlockedByResearch,
            "The native function always returns a null range; buffer ownership, flush, and locking are unknown."};
    }

    RangeProvider() = delete;
};

} // namespace aimp
