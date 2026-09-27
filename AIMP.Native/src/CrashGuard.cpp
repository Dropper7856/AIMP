#include "aimp/CrashGuard.h"

#include <Windows.h>

#include <cstring>
#include <cwchar>
#include <cstdio>
#include <limits>

namespace aimp::crash_guard {
namespace {

bool HasReadAccess(const DWORD protection) noexcept {
    if ((protection & PAGE_GUARD) != 0 || protection == PAGE_NOACCESS) {
        return false;
    }
    const DWORD basic = protection & 0xFFU;
    return basic == PAGE_READONLY ||
        basic == PAGE_READWRITE ||
        basic == PAGE_WRITECOPY ||
        basic == PAGE_EXECUTE_READ ||
        basic == PAGE_EXECUTE_READWRITE ||
        basic == PAGE_EXECUTE_WRITECOPY;
}

bool HasWriteAccess(const DWORD protection) noexcept {
    if ((protection & PAGE_GUARD) != 0 || protection == PAGE_NOACCESS) {
        return false;
    }
    const DWORD basic = protection & 0xFFU;
    return basic == PAGE_READWRITE ||
        basic == PAGE_WRITECOPY ||
        basic == PAGE_EXECUTE_READWRITE ||
        basic == PAGE_EXECUTE_WRITECOPY;
}

bool CheckPages(
    const void* address,
    const std::size_t bytes,
    const bool writable) noexcept {
    if (address == nullptr) {
        return bytes == 0;
    }
    if (bytes == 0) {
        return true;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin > std::numeric_limits<std::uintptr_t>::max() - bytes) {
        return false;
    }
    const auto end = begin + bytes;
    auto cursor = begin;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(cursor),
                &info,
                sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT ||
            !(writable ? HasWriteAccess(info.Protect) : HasReadAccess(info.Protect))) {
            return false;
        }

        const auto regionBegin =
            reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (regionBegin > std::numeric_limits<std::uintptr_t>::max() - info.RegionSize) {
            return false;
        }
        const auto regionEnd = regionBegin + info.RegionSize;
        if (regionEnd <= cursor) {
            return false;
        }
        cursor = (regionEnd < end) ? regionEnd : end;
    }
    return true;
}



wchar_t gCrashReportPath[32768]{};
LPTOP_LEVEL_EXCEPTION_FILTER gPreviousUnhandledFilter{};
volatile LONG gExceptionRecorderInstalled{};

LONG WINAPI RecordUnhandledException(EXCEPTION_POINTERS* pointers) noexcept {
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr) {
        HANDLE file = CreateFileW(
            gCrashReportPath,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char text[4096]{};
            const auto* record = pointers->ExceptionRecord;
            const auto* context = pointers->ContextRecord;
            const auto aiBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const auto aimpBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"AIMP.NetZ.dll"));
            unsigned long eip = 0U, esp = 0U, ebp = 0U, eax = 0U, ebx = 0U,
                ecx = 0U, edx = 0U, esi = 0U, edi = 0U, eflags = 0U;
#if defined(_M_IX86)
            if (context != nullptr) {
                eip = context->Eip; esp = context->Esp; ebp = context->Ebp;
                eax = context->Eax; ebx = context->Ebx; ecx = context->Ecx;
                edx = context->Edx; esi = context->Esi; edi = context->Edi;
                eflags = context->EFlags;
            }
#endif
            const auto info0 = record->NumberParameters > 0U
                ? static_cast<unsigned long long>(record->ExceptionInformation[0]) : 0ULL;
            const auto info1 = record->NumberParameters > 1U
                ? static_cast<unsigned long long>(record->ExceptionInformation[1]) : 0ULL;
            const auto writtenChars = _snprintf_s(
                text, sizeof(text), _TRUNCATE,
                "version=1\r\n"
                "pid=%lu\r\n"
                "tid=%lu\r\n"
                "tickMs=%llu\r\n"
                "exceptionCode=0x%08lX\r\n"
                "exceptionFlags=0x%08lX\r\n"
                "exceptionAddress=0x%p\r\n"
                "exceptionParameters=%lu\r\n"
                "exceptionInfo0=0x%llX\r\n"
                "exceptionInfo1=0x%llX\r\n"
                "aiModuleBase=0x%08llX\r\n"
                "aimpModuleBase=0x%08llX\r\n"
                "eip=0x%08lX\r\n"
                "esp=0x%08lX\r\n"
                "ebp=0x%08lX\r\n"
                "eax=0x%08lX\r\n"
                "ebx=0x%08lX\r\n"
                "ecx=0x%08lX\r\n"
                "edx=0x%08lX\r\n"
                "esi=0x%08lX\r\n"
                "edi=0x%08lX\r\n"
                "eflags=0x%08lX\r\n",
                GetCurrentProcessId(), GetCurrentThreadId(),
                static_cast<unsigned long long>(GetTickCount64()),
                record->ExceptionCode, record->ExceptionFlags,
                record->ExceptionAddress, record->NumberParameters,
                info0, info1,
                static_cast<unsigned long long>(aiBase),
                static_cast<unsigned long long>(aimpBase),
                eip, esp, ebp, eax, ebx, ecx, edx, esi, edi, eflags);
            if (writtenChars > 0) {
                DWORD bytesWritten = 0U;
                static_cast<void>(WriteFile(
                    file, text, static_cast<DWORD>(writtenChars), &bytesWritten, nullptr));
                static_cast<void>(FlushFileBuffers(file));
            }
            CloseHandle(file);
        }
    }
    const auto previous = gPreviousUnhandledFilter;
    if (previous != nullptr && previous != &RecordUnhandledException)
        return previous(pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}


} // namespace

bool IsReadable(const void* address, const std::size_t bytes) noexcept {
    return CheckPages(address, bytes, false);
}

bool IsWritable(void* address, const std::size_t bytes) noexcept {
    return CheckPages(address, bytes, true);
}

bool TryCopy(
    void* destination,
    const void* source,
    const std::size_t bytes) noexcept {
    if (bytes == 0) {
        return true;
    }
    if (destination == nullptr || !IsReadable(source, bytes)) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, source, bytes);
    return true;
#endif
}

bool TryWrite(
    void* destination,
    const void* source,
    const std::size_t bytes) noexcept {
    if (bytes == 0) {
        return true;
    }
    if (source == nullptr || !IsWritable(destination, bytes)) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, source, bytes);
    return true;
#endif
}


bool InstallUnhandledExceptionRecorder() noexcept {
    if (InterlockedCompareExchange(&gExceptionRecorderInstalled, 1, 0) != 0)
        return true;

    wchar_t root[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", root, _countof(root));
    if (length == 0U || length >= _countof(root)) {
        if (GetTempPathW(_countof(root), root) == 0U) {
            InterlockedExchange(&gExceptionRecorderInstalled, 0);
            return false;
        }
    }
    std::size_t used = wcslen(root);
    if (used != 0U && root[used - 1U] != L'\\') {
        if (used + 1U >= _countof(root)) {
            InterlockedExchange(&gExceptionRecorderInstalled, 0);
            return false;
        }
        root[used++] = L'\\'; root[used] = L'\0';
    }
    if (wcscat_s(root, _countof(root), L"AIMP") != 0) {
        InterlockedExchange(&gExceptionRecorderInstalled, 0);
        return false;
    }
    CreateDirectoryW(root, nullptr);
    if (wcscat_s(root, _countof(root), L"\\logs") != 0) {
        InterlockedExchange(&gExceptionRecorderInstalled, 0);
        return false;
    }
    CreateDirectoryW(root, nullptr);
    wchar_t suffix[96]{};
    if (_snwprintf_s(suffix, _countof(suffix), _TRUNCATE,
            L"\\AIMP.Crash-%lu.txt", GetCurrentProcessId()) < 0 ||
        wcscat_s(root, _countof(root), suffix) != 0 ||
        wcscpy_s(gCrashReportPath, _countof(gCrashReportPath), root) != 0) {
        InterlockedExchange(&gExceptionRecorderInstalled, 0);
        return false;
    }
    gPreviousUnhandledFilter = SetUnhandledExceptionFilter(&RecordUnhandledException);
    return true;
}

void UninstallUnhandledExceptionRecorder() noexcept {
    if (InterlockedExchange(&gExceptionRecorderInstalled, 0) == 0) return;
    static_cast<void>(SetUnhandledExceptionFilter(gPreviousUnhandledFilter));
    gPreviousUnhandledFilter = nullptr;
}

bool IsInRange(
    const void* address,
    const std::size_t bytes,
    const void* rangeBase,
    const std::size_t rangeBytes) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(rangeBase);
    if (value < base || base > std::numeric_limits<std::uintptr_t>::max() - rangeBytes) {
        return false;
    }
    const auto rangeEnd = base + rangeBytes;
    if (value > std::numeric_limits<std::uintptr_t>::max() - bytes) {
        return false;
    }
    return value + bytes <= rangeEnd;
}

} // namespace aimp::crash_guard
