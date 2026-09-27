#pragma once

#include "aimp/Common.h"

namespace aimp {

class IdentityMapper final {
public:
    [[nodiscard]] static constexpr ModuleReport Report() noexcept {
        return {
            "IdentityMapper",
            ModuleStatus::BlockedByResearch,
            "Portable session identities are designed; native participant/channel lifetime mappings are unmeasured."};
    }

    IdentityMapper() = delete;
};

} // namespace aimp
