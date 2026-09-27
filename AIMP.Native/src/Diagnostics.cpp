#include "aimp/Diagnostics.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

namespace aimp {
namespace {

std::wstring BuildLogDirectory() {
    std::vector<wchar_t> localAppData(32768);
    const auto length = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        localAppData.data(),
        static_cast<DWORD>(localAppData.size()));

    std::wstring root;
    if (length > 0 && length < localAppData.size()) {
        root.assign(localAppData.data(), length);
    } else {
        std::vector<wchar_t> temporary(32768);
        const auto tempLength =
            GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (tempLength == 0 || tempLength >= temporary.size()) {
            return {};
        }
        root.assign(temporary.data(), tempLength);
    }

    if (!root.empty() && root.back() != L'\\') {
        root.push_back(L'\\');
    }
    root += L"AIMP";
    CreateDirectoryW(root.c_str(), nullptr);
    root += L"\\logs";
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

} // namespace

Diagnostics::~Diagnostics() {
    Shutdown();
}

bool Diagnostics::Initialize() {
    std::lock_guard lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        return true;
    }

    const auto directory = BuildLogDirectory();
    if (directory.empty()) {
        return false;
    }

    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t fileName[128]{};
    _snwprintf_s(
        fileName,
        _countof(fileName),
        _TRUNCATE,
        L"\\AIMP.NetZ-%04u%02u%02uT%02u%02u%02u-%03u-%lu.jsonl",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds,
        GetCurrentProcessId());
    path_ = directory + fileName;

    file_ = CreateFileW(
        path_.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    return file_ != INVALID_HANDLE_VALUE;
}

void Diagnostics::Shutdown() noexcept {
    std::lock_guard lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(file_);
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

void Diagnostics::Log(
    const std::string_view level,
    const std::string_view event,
    const std::string_view message,
    const std::initializer_list<DiagnosticField> fields) {
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::ostringstream line;
    line << "{\"timestamp\":\"" << UtcTimestamp()
         << "\",\"sequence\":" << sequence
         << ",\"pid\":" << GetCurrentProcessId()
         << ",\"tid\":" << GetCurrentThreadId()
         << ",\"level\":\"" << EscapeJson(level)
         << "\",\"event\":\"" << EscapeJson(event)
         << "\",\"message\":\"" << EscapeJson(message) << '"';
    for (const auto& field : fields) {
        line << ",\"" << EscapeJson(field.key) << "\":";
        if (field.rawJson) {
            line << field.value;
        } else {
            line << '"' << EscapeJson(field.value) << '"';
        }
    }
    line << "}\r\n";
    const auto text = line.str();

    std::lock_guard lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE) {
        return;
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto remaining = text.size() - offset;
        const auto requested = static_cast<DWORD>(
            (std::min<std::size_t>)(remaining, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(
                file_,
                text.data() + offset,
                requested,
                &written,
                nullptr) ||
            written == 0) {
            break;
        }
        offset += written;
    }
}

std::wstring Diagnostics::Path() const {
    std::lock_guard lock(mutex_);
    return path_;
}

bool Diagnostics::IsOpen() const noexcept {
    std::lock_guard lock(mutex_);
    return file_ != INVALID_HANDLE_VALUE;
}

std::string Diagnostics::Pointer(const void* value) {
    char buffer[32]{};
    sprintf_s(buffer, "0x%08IX", reinterpret_cast<std::uintptr_t>(value));
    return buffer;
}

std::string Diagnostics::Unsigned(const std::uint64_t value) {
    return std::to_string(value);
}

std::string Diagnostics::Boolean(const bool value) {
    return value ? "true" : "false";
}

std::string Diagnostics::EscapeJson(const std::string_view value) {
    std::string output;
    output.reserve(value.size() + 8);
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                output += "\\u00";
                output.push_back(kHex[(byte >> 4U) & 0x0FU]);
                output.push_back(kHex[byte & 0x0FU]);
            } else {
                output.push_back(character);
            }
            break;
        }
    }
    return output;
}

std::string Diagnostics::UtcTimestamp() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[40]{};
    sprintf_s(
        buffer,
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);
    return buffer;
}

} // namespace aimp
