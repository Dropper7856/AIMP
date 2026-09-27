#pragma once

#include <atomic>
#include <cstdint>

#include <Windows.h>

namespace aimp {

class Bootstrap final {
public:
    static Bootstrap& Instance() noexcept;

    void SetModule(HMODULE module) noexcept;
    DWORD Run() noexcept;
    void RequestStop() noexcept;
    void ReleaseStopEventOnDetach() noexcept;
    [[nodiscard]] std::uint32_t State() const noexcept;

private:
    Bootstrap() = default;
    ~Bootstrap() = default;

    Bootstrap(const Bootstrap&) = delete;
    Bootstrap& operator=(const Bootstrap&) = delete;

    DWORD RunImpl();

    HMODULE module_{};
    std::atomic<HANDLE> stopEvent_{nullptr};
    volatile LONG state_{};
};

} // namespace aimp
