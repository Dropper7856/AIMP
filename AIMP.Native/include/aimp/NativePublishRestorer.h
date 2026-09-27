#pragma once

#include "aimp/Common.h"

namespace aimp {

class NativePublishRestorer final {
public:
    [[nodiscard]] static constexpr ModuleReport Report() noexcept {
        return {
            "NativePublishRestorer",
            ModuleStatus::BlockedByResearch,
            "The current publisher ignores its descriptor ID; payload source and original writer are unknown."};
    }

    NativePublishRestorer() = delete;
};

} // namespace aimp
