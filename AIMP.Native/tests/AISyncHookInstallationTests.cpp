#include "aimp/AISyncAnimation.h"
#include "aimp/AISyncCombat.h"
#include "aimp/AISyncHealth.h"
#include "aimp/AISyncNavigation.h"
#include "aimp/HookManager.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
void Require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
T Read(const std::span<const std::byte> bytes, const std::size_t offset) {
    Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "Truncated PE data");
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

class DataImage final {
public:
    explicit DataImage(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        const std::vector<char> file{std::istreambuf_iterator<char>{stream}, {}};
        const auto raw = std::as_bytes(std::span(file));
        const auto dos = Read<IMAGE_DOS_HEADER>(raw, 0U);
        Require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0, "Invalid DOS header");
        const auto offset = static_cast<std::size_t>(dos.e_lfanew);
        const auto nt = Read<IMAGE_NT_HEADERS32>(raw, offset);
        Require(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
            nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC, "Expected x86 PE32");
        Require(nt.OptionalHeader.SizeOfImage != 0U && nt.OptionalHeader.SizeOfImage <= 0x10000000U,
            "Invalid PE image size");
        const auto sectionTable = offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
            nt.FileHeader.SizeOfOptionalHeader;
        std::vector<std::byte> mapped(nt.OptionalHeader.SizeOfImage);
        Require(nt.OptionalHeader.SizeOfHeaders <= raw.size() &&
            nt.OptionalHeader.SizeOfHeaders <= mapped.size(), "Invalid PE headers size");
        std::memcpy(mapped.data(), raw.data(), nt.OptionalHeader.SizeOfHeaders);
        for (unsigned i = 0U; i < nt.FileHeader.NumberOfSections; ++i) {
            const auto section = Read<IMAGE_SECTION_HEADER>(raw, sectionTable + i * sizeof(IMAGE_SECTION_HEADER));
            Require(section.PointerToRawData <= raw.size() && section.SizeOfRawData <= raw.size() - section.PointerToRawData &&
                section.VirtualAddress <= mapped.size() && section.SizeOfRawData <= mapped.size() - section.VirtualAddress,
                "Invalid PE section bounds");
            if (section.SizeOfRawData != 0U)
                std::memcpy(mapped.data() + section.VirtualAddress,
                    raw.data() + section.PointerToRawData, section.SizeOfRawData);
        }
        // Data only: no LoadLibrary, imports, entry point, TLS callbacks or
        // game instructions are executed. HookManager patches this private copy.
        allocation_ = VirtualAlloc(nullptr, mapped.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        Require(allocation_ != nullptr, "Unable to allocate the private PE data image");
        try {
            const auto delta = static_cast<DWORD>(Base()) - nt.OptionalHeader.ImageBase;
            Require(delta != 0U, "Test requires a rebased image to cover ASLR");
            const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            Require(directory.VirtualAddress <= mapped.size() && directory.Size <= mapped.size() - directory.VirtualAddress,
                "Invalid PE relocation directory");
            std::size_t cursor = directory.VirtualAddress;
            const auto end = cursor + directory.Size;
            Require(cursor != 0U && cursor != end, "Expected native HIGHLOW relocations");
            while (cursor < end) {
                const auto block = Read<IMAGE_BASE_RELOCATION>(mapped, cursor);
                Require(block.SizeOfBlock >= sizeof(block) && block.SizeOfBlock <= end - cursor &&
                    (block.SizeOfBlock - sizeof(block)) % sizeof(WORD) == 0U, "Invalid relocation block");
                for (std::size_t j = sizeof(block); j < block.SizeOfBlock; j += sizeof(WORD)) {
                    const auto entry = Read<WORD>(mapped, cursor + j);
                    if ((entry >> 12U) == IMAGE_REL_BASED_ABSOLUTE) continue;
                    Require((entry >> 12U) == IMAGE_REL_BASED_HIGHLOW, "Unsupported PE relocation type");
                    const auto target = static_cast<std::size_t>(block.VirtualAddress) + (entry & 0xFFFU);
                    const auto value = Read<DWORD>(mapped, target) + delta;
                    std::memcpy(mapped.data() + target, &value, sizeof(value));
                }
                cursor += block.SizeOfBlock;
            }
            std::memcpy(allocation_, mapped.data(), mapped.size());
        } catch (...) {
            VirtualFree(allocation_, 0U, MEM_RELEASE);
            throw;
        }
    }
    ~DataImage() { VirtualFree(allocation_, 0U, MEM_RELEASE); }
    DataImage(const DataImage&) = delete;
    DataImage& operator=(const DataImage&) = delete;
    std::uintptr_t Base() const noexcept { return reinterpret_cast<std::uintptr_t>(allocation_); }
private:
    void* allocation_{};
};

bool NoHumanPair(std::uintptr_t, std::uintptr_t) noexcept { return false; }
bool NoManagedCharacter(std::uintptr_t) noexcept { return false; }

void TestInstallation(DataImage& image) {
    using namespace aimp::ai_sync;
    aimp::HookManager hooks(true, true);
    std::string error;
    const auto CheckCall = [&](const bool success, const char* stage) {
        Require(success, std::string(stage) + ": " + error);
    };
    const auto base = image.Base();
    constexpr std::array<std::uintptr_t, 8U> sites{
        0x003CAAC0U, 0x003C3B10U, 0x00286570U, 0x00286860U,
        0x002556F0U, 0x00263E00U, 0x00291220U, 0x00291000U};
    std::array<std::array<std::byte, 32U>, sites.size()> original{};
    for (std::size_t i = 0U; i < sites.size(); ++i)
        std::memcpy(original[i].data(), reinterpret_cast<const void*>(base + sites[i]), original[i].size());

    auto* shotEntry = reinterpret_cast<std::uint8_t*>(base + sites[1]);
    const auto firstByte = *shotEntry;
    *shotEntry = 0x90U;
    Require(!combat::Initialize(base, error) && hooks.InstalledCount() == 0U,
        "A mismatched combat ABI must still be refused");
    *shotEntry = firstByte;
    error.clear();
    CheckCall(health::Initialize(base, error), "Health ABI");
    CheckCall(combat::Initialize(base, error), "Combat ABI");
    CheckCall(navigation::Initialize(base, error), "Navigation ABI");
    CheckCall(health::InstallDamageObserver(hooks, error), "Health installation");
    Require(hooks.InstalledCount() == 1U, "Missing health hook");
    CheckCall(combat::Install(hooks, error), "Combat installation");
    Require(hooks.InstalledCount() == 2U, "Missing shot hook");
    CheckCall(navigation::InstallGoalObservers(hooks, error), "Navigation installation");
    Require(hooks.InstalledCount() == 4U, "Missing directed goal hooks");
    CheckCall(animation::Install(base, hooks,
        {&NoHumanPair, &NoManagedCharacter, nullptr, &NoManagedCharacter}, error), "Animation installation");
    Require(hooks.InstalledCount() == sites.size(), "Missing animation hooks");
    for (const auto site : sites)
        Require(*reinterpret_cast<const std::uint8_t*>(base + site) == 0xE9U, "Hook did not patch the private image");

    CheckCall(hooks.UninstallAll(error), "Hook rollback");
    for (std::size_t i = 0U; i < sites.size(); ++i)
        Require(std::memcmp(original[i].data(), reinterpret_cast<const void*>(base + sites[i]), original[i].size()) == 0,
            "Rollback did not restore the original prefix");
    Require(hooks.InstalledCount() == 0U && hooks.RetiredCount() == sites.size(), "Unexpected rollback state");
    // No detour/trampoline has been invoked; this test owns their quiescence.
    CheckCall(hooks.ReleaseRetiredTrampolines(true, error), "Trampoline release");
    Require(hooks.RetiredCount() == 0U, "Trampolines were not released");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !std::filesystem::is_regular_file(argv[1])) {
        std::cout << "SKIP: supply the certified AI.exe as offline PE data.\n";
        return 77;
    }
    try {
        DataImage image(argv[1]);
        TestInstallation(image);
        std::cout << "PASS: eight AISync hooks installed and rolled back on rebased PE data; no game code executed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
