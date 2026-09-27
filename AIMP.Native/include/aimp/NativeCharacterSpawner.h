#pragma once

#include "aimp/Common.h"

namespace aimp {

class NativeCharacterSpawner final {
public:
    [[nodiscard]] static constexpr ModuleReport Report() noexcept {
        return {
            "NativeCharacterSpawner",
            ModuleStatus::BlockedByResearch,
            "No captured native spawn request, resource contract, thread, or runtime destruction proof."};
    }

    NativeCharacterSpawner() = delete;
};

} // namespace aimp
