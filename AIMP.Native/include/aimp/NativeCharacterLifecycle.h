#pragma once

#include "aimp/Common.h"

namespace aimp {

class NativeCharacterLifecycle final {
public:
    [[nodiscard]] static constexpr ModuleReport Report() noexcept {
        return {
            "NativeCharacterLifecycle",
            ModuleStatus::BlockedByResearch,
            "Static cleanup exists, but runtime ordering across level changes has not been measured."};
    }

    NativeCharacterLifecycle() = delete;
};

} // namespace aimp
