#include "aimp/CampaignSaveRuntime.h"

#include "aimp/CrashGuard.h"

#include <Windows.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aimp::campaign_save {
namespace {

// Retail save storage object (manager + 0x2238), vtable 0x013E9AA8:
//   slot 1 (0x013E9AAC) FUN_00A593E0 requests an enumeration (state 3, kind 1);
//   slot 7 (0x013E9AC4) FUN_00A637C0 is the per-frame poll that hands queued
//   operations to the storage worker and consumes its result.
// +0x0C operation state (0 idle), +0x2F4 worker operation (0 idle).
constexpr std::uintptr_t kStorageEnumerateSlotRva = 0x00FE9AACU;
constexpr std::uintptr_t kStorageEnumerateRva = 0x006593E0U;
constexpr std::uintptr_t kStoragePollSlotRva = 0x00FE9AC4U;
constexpr std::uintptr_t kStoragePollRva = 0x006637C0U;
constexpr std::uintptr_t kStorageStateOffset = 0x0CU;
constexpr std::uintptr_t kStorageWorkerOffset = 0x2F4U;

// ISteamRemoteStorage012 slots used by AI.exe (verified at every call site).
enum Slot : std::size_t {
    kFileWrite = 0, kFileRead = 1, kFileForget = 2, kFileDelete = 3,
    kFileExists = 10, kFilePersisted = 11, kGetFileSize = 12, kGetFileTimestamp = 13,
    kGetFileCount = 15, kGetFileNameAndSize = 16, kSlotCount = 17
};

using StorageFn = void(__fastcall*)(void*);
using FileWriteFn = bool(__fastcall*)(void*, void*, const char*, const void*, std::int32_t);
using FileReadFn = std::int32_t(__fastcall*)(void*, void*, const char*, void*, std::int32_t);
using NameBoolFn = bool(__fastcall*)(void*, void*, const char*);
using NameIntFn = std::int32_t(__fastcall*)(void*, void*, const char*);
using NameInt64Fn = std::int64_t(__fastcall*)(void*, void*, const char*);
using CountFn = std::int32_t(__fastcall*)(void*, void*);
using NameAndSizeFn = const char*(__fastcall*)(void*, void*, int, std::int32_t*);

std::atomic<bool> gInstalled{};
StorageFn gOriginalPoll{};
StorageFn gEnumerate{};
std::atomic<void*> gStorage{};

std::mutex gMutex;
bool gSteamPatched{};
void** gSteamVtable{};
void* gOriginal[kSlotCount]{};
std::wstring gDirectory;          // Empty: passthrough.
std::string gDirectoryUtf8;
std::string gSessionId;
std::string gError;
std::uint64_t gWriteRevision{};
std::uint64_t gSaveSequence{};
std::vector<std::pair<std::string, std::int32_t>> gListing;

// 0 idle, 1 enumeration wanted, 2 enumeration issued (wait for idle).
std::atomic<int> gRefresh{};

bool Read32(const std::uintptr_t at, std::uintptr_t& value) noexcept {
    return at && crash_guard::TryCopy(&value, reinterpret_cast<const void*>(at), sizeof(value));
}

bool StorageIdle(void* storage) noexcept {
    std::uintptr_t state{}, worker{};
    const auto base = reinterpret_cast<std::uintptr_t>(storage);
    return Read32(base + kStorageStateOffset, state) && Read32(base + kStorageWorkerOffset, worker)
        && state == 0U && worker == 0U;
}

void __fastcall StoragePoll(void* storage) {
    gStorage.store(storage, std::memory_order_release);
    if (gRefresh.load(std::memory_order_acquire) == 1 && StorageIdle(storage)) {
        gEnumerate(storage);
        gRefresh.store(2, std::memory_order_release);
    }
    gOriginalPoll(storage);
    if (gRefresh.load(std::memory_order_acquire) == 2 && StorageIdle(storage))
        gRefresh.store(0, std::memory_order_release);
}

bool Digits(const char* text, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}

// Only retail campaign/progression slots are isolated, never configuration.
bool SaveName(const char* name) noexcept {
    if (!name) return false;
    const auto length = std::strlen(name);
    if (length == 15U && _stricmp(name, "PROGRESSION.ais") == 0) return true;
    if (length != 10U || _stricmp(name + 6, ".ais") != 0 || !Digits(name + 4, 2)) return false;
    return _strnicmp(name, "CHSV", 4) == 0 || _strnicmp(name, "MSSV", 4) == 0
        || _strnicmp(name, "SPSV", 4) == 0;
}

bool Checkpoint(const char* name) noexcept { return _strnicmp(name, "CHSV", 4) == 0; }

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), length);
    return result;
}

// Caller holds gMutex. Empty when passthrough or the name is not isolated.
std::wstring IsolatedPath(const char* name) {
    if (gDirectory.empty() || !SaveName(name)) return {};
    return gDirectory + L"\\" + Widen(name);
}

template <typename Fn>
Fn Original(Slot slot) noexcept { return reinterpret_cast<Fn>(gOriginal[slot]); }

bool __fastcall FileWrite(void* self, void* edx, const char* name, const void* data, std::int32_t size) {
    std::unique_lock lock(gMutex);
    const auto path = IsolatedPath(name);
    if (path.empty()) {
        lock.unlock();
        return Original<FileWriteFn>(kFileWrite)(self, edx, name, data, size);
    }
    if (size < 0 || (size > 0 && !data)) return false;
    // Atomic replace: a reader never observes a partially written checkpoint.
    const auto temporary = path + L".aimp-tmp";
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { gError = "Campaign profile write failed."; return false; }
    DWORD written{};
    const bool ok = (size == 0 || WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr))
        && written == static_cast<DWORD>(size) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        gError = "Campaign profile write failed.";
        return false;
    }
    ++gWriteRevision;
    if (Checkpoint(name)) ++gSaveSequence;
    return true;
}

std::int32_t __fastcall FileRead(void* self, void* edx, const char* name, void* data, std::int32_t size) {
    std::unique_lock lock(gMutex);
    const auto path = IsolatedPath(name);
    if (path.empty()) {
        lock.unlock();
        return Original<FileReadFn>(kFileRead)(self, edx, name, data, size);
    }
    if (size <= 0 || !data) return 0;
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD read{};
    const bool ok = ReadFile(file, data, static_cast<DWORD>(size), &read, nullptr);
    CloseHandle(file);
    return ok ? static_cast<std::int32_t>(read) : 0;
}

bool __fastcall FileForget(void* self, void* edx, const char* name) {
    std::unique_lock lock(gMutex);
    if (IsolatedPath(name).empty()) {
        lock.unlock();
        return Original<NameBoolFn>(kFileForget)(self, edx, name);
    }
    return true; // Profile files are local; there is no cloud copy to forget.
}

bool __fastcall FileDelete(void* self, void* edx, const char* name) {
    std::unique_lock lock(gMutex);
    const auto path = IsolatedPath(name);
    if (path.empty()) {
        lock.unlock();
        return Original<NameBoolFn>(kFileDelete)(self, edx, name);
    }
    const bool deleted = DeleteFileW(path.c_str()) != FALSE;
    if (deleted) ++gWriteRevision;
    return deleted;
}

bool Attributes(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& data) {
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)
        && (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0
        && data.nFileSizeHigh == 0 && data.nFileSizeLow <= 0x7FFFFFFFU;
}

bool __fastcall FileExists(void* self, void* edx, const char* name) {
    std::unique_lock lock(gMutex);
    const auto path = IsolatedPath(name);
    if (path.empty()) {
        lock.unlock();
        return Original<NameBoolFn>(kFileExists)(self, edx, name);
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return Attributes(path, data);
}

bool __fastcall FilePersisted(void* self, void* edx, const char* name) {
    std::unique_lock lock(gMutex);
    const auto path = IsolatedPath(name);
    if (path.empty()) {
        lock.unlock();
        return Original<NameBoolFn>(kFilePersisted)(self, edx, name);
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return Attributes(path, data);
}

std::int32_t __fastcall GetFileSize(void* self, void* edx, const char* name) {
    std::unique_lock lock(gMutex);
    const auto path = IsolatedPath(name);
    if (path.empty()) {
        lock.unlock();
        return Original<NameIntFn>(kGetFileSize)(self, edx, name);
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return Attributes(path, data) ? static_cast<std::int32_t>(data.nFileSizeLow) : 0;
}

std::int64_t __fastcall GetFileTimestamp(void* self, void* edx, const char* name) {
    std::unique_lock lock(gMutex);
    const auto path = IsolatedPath(name);
    if (path.empty()) {
        lock.unlock();
        return Original<NameInt64Fn>(kGetFileTimestamp)(self, edx, name);
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!Attributes(path, data)) return 0;
    ULARGE_INTEGER time{};
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return static_cast<std::int64_t>((time.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

// The listing is rebuilt by GetFileCount (the retail enumeration always calls
// it first) so GetFileNameAndSize returns stable pointers for that pass.
std::int32_t __fastcall GetFileCount(void* self, void* edx) {
    std::lock_guard lock(gMutex);
    if (gDirectory.empty()) return Original<CountFn>(kGetFileCount)(self, edx);
    gListing.clear();
    const auto count = Original<CountFn>(kGetFileCount)(self, edx);
    for (int i = 0; i < count && gListing.size() < 1024U; ++i) {
        std::int32_t size{};
        const auto* name = Original<NameAndSizeFn>(kGetFileNameAndSize)(self, edx, i, &size);
        if (name && !SaveName(name)) gListing.emplace_back(name, size);
    }
    WIN32_FIND_DATAW entry{};
    const auto find = FindFirstFileW((gDirectory + L"\\*.ais").c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0
                || entry.nFileSizeHigh != 0 || entry.nFileSizeLow > 0x7FFFFFFFU) continue;
            char narrow[MAX_PATH]{};
            if (WideCharToMultiByte(CP_UTF8, 0, entry.cFileName, -1, narrow, MAX_PATH, nullptr, nullptr) <= 0
                || !SaveName(narrow)) continue;
            gListing.emplace_back(narrow, static_cast<std::int32_t>(entry.nFileSizeLow));
        } while (gListing.size() < 1024U && FindNextFileW(find, &entry));
        FindClose(find);
    }
    return static_cast<std::int32_t>(gListing.size());
}

const char* __fastcall GetFileNameAndSize(void* self, void* edx, int index, std::int32_t* size) {
    std::lock_guard lock(gMutex);
    if (gDirectory.empty()) return Original<NameAndSizeFn>(kGetFileNameAndSize)(self, edx, index, size);
    if (index < 0 || static_cast<std::size_t>(index) >= gListing.size()) {
        if (size) *size = 0;
        return "";
    }
    if (size) *size = gListing[static_cast<std::size_t>(index)].second;
    return gListing[static_cast<std::size_t>(index)].first.c_str();
}

bool WriteSlot(void** slot, void* expected, void* replacement) noexcept {
    DWORD previous{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) return false;
    const auto old = InterlockedCompareExchangePointer(slot, replacement, expected);
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), previous, &ignored);
    return old == expected;
}

// Caller holds gMutex. Patches the live ISteamRemoteStorage012 vtable once.
bool EnsureSteamPatched(std::string& error) {
    if (gSteamPatched) return true;
    const auto steam = GetModuleHandleW(L"steam_api.dll");
    using GetterFn = void*(__cdecl*)();
    const auto getter = steam ? reinterpret_cast<GetterFn>(GetProcAddress(steam, "SteamRemoteStorage")) : nullptr;
    void* storage = getter ? getter() : nullptr;
    std::uintptr_t vtable{};
    if (!storage || !Read32(reinterpret_cast<std::uintptr_t>(storage), vtable) || !vtable) {
        error = "Steam remote storage is unavailable.";
        return false;
    }
    auto** slots = reinterpret_cast<void**>(vtable);
    void* originals[kSlotCount]{};
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        std::uintptr_t value{};
        if (!Read32(vtable + i * sizeof(void*), value) || !value) {
            error = "Steam remote storage vtable is unreadable.";
            return false;
        }
        originals[i] = reinterpret_cast<void*>(value);
    }
    const std::pair<Slot, void*> replacements[]{
        {kFileWrite, reinterpret_cast<void*>(&FileWrite)},
        {kFileRead, reinterpret_cast<void*>(&FileRead)},
        {kFileForget, reinterpret_cast<void*>(&FileForget)},
        {kFileDelete, reinterpret_cast<void*>(&FileDelete)},
        {kFileExists, reinterpret_cast<void*>(&FileExists)},
        {kFilePersisted, reinterpret_cast<void*>(&FilePersisted)},
        {kGetFileSize, reinterpret_cast<void*>(&GetFileSize)},
        {kGetFileTimestamp, reinterpret_cast<void*>(&GetFileTimestamp)},
        {kGetFileCount, reinterpret_cast<void*>(&GetFileCount)},
        {kGetFileNameAndSize, reinterpret_cast<void*>(&GetFileNameAndSize)}};
    // Originals are published before any slot can route through a hook.
    std::memcpy(gOriginal, originals, sizeof(gOriginal));
    std::size_t applied = 0;
    for (; applied < std::size(replacements); ++applied) {
        const auto [slot, hook] = replacements[applied];
        if (!WriteSlot(&slots[slot], originals[slot], hook)) break;
    }
    if (applied != std::size(replacements)) {
        for (std::size_t i = 0; i < applied; ++i)
            WriteSlot(&slots[replacements[i].first], replacements[i].second, originals[replacements[i].first]);
        error = "Steam remote storage is already hooked by another module.";
        return false;
    }
    gSteamVtable = slots;
    gSteamPatched = true;
    return true;
}

std::string Escape(const std::string& text) {
    std::string result;
    for (const char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (static_cast<unsigned char>(c) < 0x20U) result += ' ';
        else result += c;
    }
    return result;
}

bool ValidDirectory(const std::wstring& path) {
    if (path.size() < 3U || path[1] != L':' || path[2] != L'\\') return false;
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

} // namespace

bool Install(const std::uintptr_t moduleBase, std::string& error) noexcept {
    if (gInstalled.load(std::memory_order_acquire)) return true;
    std::uintptr_t poll{}, enumerate{};
    if (!moduleBase || !Read32(moduleBase + kStoragePollSlotRva, poll)
        || !Read32(moduleBase + kStorageEnumerateSlotRva, enumerate)
        || poll != moduleBase + kStoragePollRva || enumerate != moduleBase + kStorageEnumerateRva) {
        error = "Campaign save storage requires the exact executable save-storage vtable.";
        return false;
    }
    gOriginalPoll = reinterpret_cast<StorageFn>(poll);
    gEnumerate = reinterpret_cast<StorageFn>(enumerate);
    if (!WriteSlot(reinterpret_cast<void**>(moduleBase + kStoragePollSlotRva),
            reinterpret_cast<void*>(poll), reinterpret_cast<void*>(&StoragePoll))) {
        error = "Campaign save storage poll slot is already hooked.";
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }

bool Configure(const std::string& sessionId, const std::string& utf8Directory, std::string& error) {
    if (!Installed()) { error = "Campaign save storage is not installed."; return false; }
    auto directory = Widen(utf8Directory);
    while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/')) directory.pop_back();
    if (!ValidDirectory(directory)) { error = "Campaign profile directory is invalid."; return false; }
    std::lock_guard lock(gMutex);
    if (gSessionId == sessionId && gDirectory == directory) return true;
    const auto storage = gStorage.load(std::memory_order_acquire);
    if (!storage || !StorageIdle(storage) || gRefresh.load(std::memory_order_acquire) != 0) {
        error = "Native save storage is busy; retry from the menu.";
        return false;
    }
    if (!EnsureSteamPatched(error)) return false;
    gDirectory = std::move(directory);
    gDirectoryUtf8 = utf8Directory;
    gSessionId = sessionId;
    gError.clear();
    gWriteRevision = gSaveSequence = 0;
    gListing.clear();
    gRefresh.store(1, std::memory_order_release);
    return true;
}

bool Clear(const std::string& sessionId, std::string& error) {
    std::lock_guard lock(gMutex);
    if (gDirectory.empty()) return true;
    if (gSessionId != sessionId) { error = "Campaign profile belongs to another session."; return false; }
    const auto storage = gStorage.load(std::memory_order_acquire);
    if (!storage || !StorageIdle(storage)) { error = "Native save storage is busy."; return false; }
    gDirectory.clear();
    gDirectoryUtf8.clear();
    gSessionId.clear();
    gListing.clear();
    gRefresh.store(1, std::memory_order_release);
    return true;
}

Status Snapshot() {
    std::lock_guard lock(gMutex);
    Status status;
    const auto refresh = gRefresh.load(std::memory_order_acquire);
    const auto storage = gStorage.load(std::memory_order_acquire);
    status.busy = refresh != 0 || !storage || !StorageIdle(storage);
    status.ready = !gDirectory.empty() && refresh == 0;
    status.sessionId = gSessionId;
    status.profilePath = gDirectoryUtf8;
    status.saveSequence = gSaveSequence;
    status.writeRevision = gWriteRevision;
    status.error = gError;
    return status;
}

std::string EncodeStatus(const Status& status) {
    std::ostringstream json;
    json << "{\"Ready\":" << (status.ready ? "true" : "false")
         << ",\"Busy\":" << (status.busy ? "true" : "false")
         << ",\"SessionId\":\"" << Escape(status.sessionId)
         << "\",\"ProfilePath\":\"" << Escape(status.profilePath)
         << "\",\"SaveSequence\":" << status.saveSequence
         << ",\"WriteRevision\":" << status.writeRevision
         << ",\"Error\":\"" << Escape(status.error) << "\"}";
    return json.str();
}

} // namespace aimp::campaign_save
