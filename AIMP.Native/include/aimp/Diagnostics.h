#pragma once

#include <atomic>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>

#include <Windows.h>

namespace aimp {

struct DiagnosticField {
    std::string_view key;
    std::string value;
    bool rawJson{};
};

class Diagnostics final {
public:
    Diagnostics() = default;
    ~Diagnostics();

    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    bool Initialize();
    void Shutdown() noexcept;

    void Log(
        std::string_view level,
        std::string_view event,
        std::string_view message,
        std::initializer_list<DiagnosticField> fields = {});

    [[nodiscard]] std::wstring Path() const;
    [[nodiscard]] bool IsOpen() const noexcept;

    static std::string Pointer(const void* value);
    static std::string Unsigned(std::uint64_t value);
    static std::string Boolean(bool value);

private:
    static std::string EscapeJson(std::string_view value);
    static std::string UtcTimestamp();

    mutable std::mutex mutex_;
    HANDLE file_{INVALID_HANDLE_VALUE};
    std::wstring path_;
    std::atomic<std::uint64_t> sequence_{0};
};

} // namespace aimp
