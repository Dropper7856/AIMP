#include "aimp/AISyncMarkerIdentity.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <cstring>
#include "aimp/HookManager.h"
#include <fstream>
#include <iterator>

using namespace aimp::ai_sync::marker_identity;
namespace {
std::size_t checks{};
void Check(bool condition, int line) {
    ++checks;
    if (!condition) {
        std::cerr << "Marker identity check failed at " << line << '\n';
        std::abort();
    }
}
#define CHECK(x) Check((x), __LINE__)
void Append(std::vector<std::byte>& b, std::uint32_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        b.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
}
std::array<char, kMaxLevelBytes> Level() {
    std::array<char, kMaxLevelBytes> level{};
    constexpr char name[] = "TECH_COMMS";
    std::copy(std::begin(name), std::end(name), level.begin());
    return level;
}
std::vector<std::byte> Cover() {
    std::vector<std::byte> b;
    Append(b, 0x32727663, 4); Append(b, 7, 4); Append(b, 3, 2);
    b.resize(b.size() + 3 * 64); // Three deliberately identical covers.
    Append(b, 3, 4);
    Append(b, 2, 2); b.resize(b.size() + 2 * 18); // Two identical markers.
    Append(b, 0, 2); // Empty cover preserves the serialized cover ordinal.
    Append(b, 1, 2); b.resize(b.size() + 18);
    Append(b, 1, 2); Append(b, 1, 2); b.resize(b.size() + 12);
    Append(b, 1, 2); Append(b, 1, 2); // One grid cell, one entry.
    return b;
}
void DigestVectors() {
    Digest digest{};
    const std::array<std::byte, 3> abc{std::byte{0x61}, std::byte{0x62}, std::byte{0x63}};
    constexpr Digest expected{0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,
        0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    CHECK(HashImmutableContent(abc, digest) == Result::Ok);
    CHECK(digest == expected);
    CHECK(HashImmutableContent({}, digest) == Result::Ok);
    constexpr Digest empty{0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
        0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,0x27,0xae,0x41,0xe4,
        0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
    CHECK(digest == empty);
}
void SerializedIdentity() {
    auto bytes = Cover();
    Key first{}, second{}, otherCover{};
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 1, 0, first) == Result::Ok);
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 1, 1, second) == Result::Ok);
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 3, 0, otherCover) == Result::Ok);
    CHECK(first != second && first != otherCover && second != otherCover);
    CHECK(first.contentSha256 == otherCover.contentSha256);
    std::uint32_t ordinal{};
    CHECK(FindSerializedMarker(bytes, Level(), 0, second, ordinal) == Result::Ok && ordinal == 1);
    CHECK(FindSerializedMarker(bytes, Level(), 0, otherCover, ordinal) == Result::Ok && ordinal == 2);
    Key state{};
    CHECK(DescribeSerializedMarker(bytes, Level(), 9, 1, 0, state) == Result::Ok);
    CHECK(state != first && state.contentSha256 != first.contentSha256);
    // Changing grid bytes changes identity even though marker bytes still match.
    bytes.back() = std::byte{1};
    CHECK(FindSerializedMarker(bytes, Level(), 0, first, ordinal) == Result::ContentMismatch);
    CHECK(ordinal == 0xffffffffU);
    bytes = Cover();
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 2, 0, state) == Result::InvalidOrdinal);
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 1, 2, state) == Result::InvalidOrdinal);
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 4, 0, state) == Result::InvalidOrdinal);
    // Every truncation, including after the chosen marker, must fail.
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        CHECK(DescribeSerializedMarker(std::span(bytes).first(n), Level(), 0, 1, 0, state) != Result::Ok);
        CHECK(state.contentSha256 == Digest{});
    }
    bytes.push_back(std::byte{});
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 1, 0, state) == Result::InvalidContent);
    bytes = Cover(); bytes[4] = std::byte{8};
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 1, 0, state) == Result::UnsupportedVersion);
    bytes[4] = std::byte{6};
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 1, 0, state) == Result::UnsupportedVersion);
    bytes = Cover(); bytes[202] = std::byte{2}; // Marker total contradicts per-cover counts.
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 1, 0, state) == Result::InvalidContent);
    bytes = Cover(); bytes[202] = std::byte{4};
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 1, 0, state) == Result::InvalidContent);
    auto badLevel = Level(); badLevel[0] = 't';
    CHECK(DescribeSerializedMarker(bytes, badLevel, 0, 0, 0, state) == Result::InvalidKey);
    CHECK(Validate(Key{}) == Result::InvalidKey);
}
void NativeReadiness() {
    const auto bytes = Cover();
    Key key{};
    CHECK(DescribeSerializedMarker(bytes, Level(), 0, 1, 0, key) == Result::Ok);
    for (const auto generation : {0ULL, 1ULL, 2ULL}) {
        // Invalid addresses are intentional: readiness must reject before any
        // native access. A new generation cannot turn a digest into a binding.
        std::int32_t marker = 123;
        CHECK(Resolve(1, 1, generation, key, marker) == Result::NativeLoaderBindingRequired);
        CHECK(marker == -1);
        Key output = key;
        CHECK(Capture(1, 1, generation, 0, output) == Result::NativeLoaderBindingRequired);
        CHECK(output.contentSha256 == Digest{});
    }
}
#ifdef AIMP_MARKER_IDENTITY_TESTING
bool onEngine = true;
bool Engine() noexcept { return onEngine; }
template<class T> void Put(std::uintptr_t at, T value) { std::memcpy(reinterpret_cast<void*>(at), &value, sizeof(value)); }
template<class T> T At(std::span<const std::byte> data, std::size_t at) {
    CHECK(at <= data.size() && sizeof(T) <= data.size() - at);
    T value{}; std::memcpy(&value, data.data() + at, sizeof(value)); return value;
}
// Private, rebased PE DATA image only. No imports, entrypoint, TLS callbacks,
// or original game instruction is invoked by these installation tests.
class DataImage {
public:
    void* allocation{};
    explicit DataImage(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<char> file{std::istreambuf_iterator<char>{input}, {}};
        const auto raw = std::as_bytes(std::span(file));
        Digest digest{};
        BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
        CHECK(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0);
        CHECK(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0);
        CHECK(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(raw.data())), static_cast<ULONG>(raw.size()), 0) >= 0);
        CHECK(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0);
        BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
        constexpr Digest expected{0x00,0xd6,0xbb,0x84,0x6a,0xc5,0xa7,0x89,0x16,0xba,0xf3,0x63,0xeb,0x14,0x8a,0xa2,
            0x55,0xa2,0x3b,0xf2,0x16,0xa2,0x71,0xcf,0xd2,0x3a,0x0e,0xce,0x3c,0x36,0x83,0x64};
        CHECK(digest == expected);
        const auto dos = At<IMAGE_DOS_HEADER>(raw, 0);
        CHECK(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0);
        const auto nt = At<IMAGE_NT_HEADERS32>(raw, static_cast<std::size_t>(dos.e_lfanew));
        CHECK(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386);
        CHECK(nt.OptionalHeader.ImageBase == 0x00400000 && nt.OptionalHeader.SizeOfImage <= 0x10000000);
        std::vector<std::byte> image(nt.OptionalHeader.SizeOfImage);
        CHECK(nt.OptionalHeader.SizeOfHeaders <= raw.size() && nt.OptionalHeader.SizeOfHeaders <= image.size());
        std::memcpy(image.data(), raw.data(), nt.OptionalHeader.SizeOfHeaders);
        const auto sectionTable = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        for (std::size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            const auto section = At<IMAGE_SECTION_HEADER>(raw, sectionTable + i * sizeof(IMAGE_SECTION_HEADER));
            CHECK(section.PointerToRawData <= raw.size() && section.SizeOfRawData <= raw.size() - section.PointerToRawData);
            CHECK(section.VirtualAddress <= image.size() && section.SizeOfRawData <= image.size() - section.VirtualAddress);
            if (section.SizeOfRawData) std::memcpy(image.data() + section.VirtualAddress, raw.data() + section.PointerToRawData, section.SizeOfRawData);
        }
        allocation = VirtualAlloc(nullptr, image.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        CHECK(allocation != nullptr);
        const auto delta = static_cast<DWORD>(Base()) - nt.OptionalHeader.ImageBase;
        CHECK(delta != 0);
        const auto reloc = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        CHECK(reloc.VirtualAddress && reloc.VirtualAddress <= image.size() && reloc.Size <= image.size() - reloc.VirtualAddress);
        std::size_t at = reloc.VirtualAddress, end = at + reloc.Size;
        while (at < end) {
            const auto block = At<IMAGE_BASE_RELOCATION>(image, at);
            CHECK(block.SizeOfBlock >= sizeof(block) && block.SizeOfBlock <= end - at);
            for (std::size_t i = sizeof(block); i < block.SizeOfBlock; i += 2) {
                const auto entry = At<WORD>(image, at + i);
                if ((entry >> 12) == IMAGE_REL_BASED_ABSOLUTE) continue;
                CHECK((entry >> 12) == IMAGE_REL_BASED_HIGHLOW);
                const auto target = std::size_t(block.VirtualAddress) + (entry & 0xFFF);
                const auto value = At<DWORD>(image, target) + delta;
                std::memcpy(image.data() + target, &value, sizeof(value));
            }
            at += block.SizeOfBlock;
        }
        std::memcpy(allocation, image.data(), image.size());
    }
    ~DataImage() { VirtualFree(allocation, 0, MEM_RELEASE); }
    std::uintptr_t Base() const { return reinterpret_cast<std::uintptr_t>(allocation); }
};
void Installation(const char* path) {
    DataImage image(path);
    const auto base = image.Base();
    constexpr std::array<std::uint32_t, 7> sites{0x341C10,0x33E350,0x33E2E0,0x343CA0,0x343D1B,0x343D5A,0x3444A2};
    std::array<std::array<std::byte, 12>, 7> original{};
    for (std::size_t i = 0; i < sites.size(); ++i)
        std::memcpy(original[i].data(), reinterpret_cast<void*>(base + sites[i]), original[i].size());
    std::string error;
    {
        aimp::HookManager denied(false, true);
        CHECK(!Install(base, denied, &Engine, error) && denied.InstalledCount() == 0);
    }
    // Corrupt the LAST redirect, so all earlier hooks must roll back.
    Put(base + sites.back(), std::uint8_t{0x90});
    {
        aimp::HookManager hooks(true, true);
        CHECK(!Install(base, hooks, &Engine, error));
        CHECK(hooks.InstalledCount() == 0);
        for (std::size_t i = 0; i + 1 < sites.size(); ++i)
            CHECK(std::memcmp(original[i].data(), reinterpret_cast<void*>(base + sites[i]), original[i].size()) == 0);
        CHECK(hooks.ReleaseRetiredTrampolines(true, error));
    }
    Put(base + sites.back(), std::uint8_t{0xE8});
    testing::Configure(0, nullptr);
    {
        aimp::HookManager hooks(true, true);
        CHECK(Install(base, hooks, &Engine, error));
        CHECK(hooks.InstalledCount() == 7);
        for (std::size_t i = 0; i < sites.size(); ++i)
            CHECK(std::memcmp(original[i].data(), reinterpret_cast<void*>(base + sites[i]), 5) != 0);
        Retire(); CHECK(hooks.UninstallAll(error));
        for (std::size_t i = 0; i < sites.size(); ++i)
            CHECK(std::memcmp(original[i].data(), reinterpret_cast<void*>(base + sites[i]), original[i].size()) == 0);
        CHECK(hooks.ReleaseRetiredTrampolines(true, error));
    }
    testing::Configure(0, nullptr);
}
struct NativeFixture {
    void* image = VirtualAlloc(nullptr, 0x1300000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    std::vector<std::byte> worldBytes = std::vector<std::byte>(0x2400);
    std::vector<std::byte> coverBytes = std::vector<std::byte>(3 * 0x60);
    std::vector<std::byte> markerBytes = std::vector<std::byte>(3 * 0x24);
    std::array<std::uint32_t, 16> root{};
    std::uintptr_t Base() const { return reinterpret_cast<std::uintptr_t>(image); }
    std::uintptr_t World() const { return reinterpret_cast<std::uintptr_t>(worldBytes.data()); }
    std::uintptr_t Markers() const { return reinterpret_cast<std::uintptr_t>(markerBytes.data()); }
    std::uintptr_t Covers() const { return reinterpret_cast<std::uintptr_t>(coverBytes.data()); }
    NativeFixture() {
        CHECK(image != nullptr);
        Put(Base() + 0x12F0C88, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(root.data())));
        root[8] = static_cast<std::uint32_t>(World());
        Put(World() + 0x23D8, static_cast<std::uint32_t>(Markers()));
        Put(World() + 0x23DC, 3U); Put(World() + 0x23E0, static_cast<std::uint32_t>(World()));
        Put(World() + 0x23E4, static_cast<std::uint32_t>(Covers())); Put(World() + 0x23E8, 3U);
        Put(World() + 0x23F4, std::uint8_t{1}); Put(World() + 0x23F8, 1U); Put(World() + 0x23FC, 1U);
        Put(Covers() + 0x58, std::uint16_t{2}); Put(Covers() + 0x5C, 0U);
        Put(Covers() + 0x60 + 0x58, std::uint16_t{0}); Put(Covers() + 0x60 + 0x5C, 2U);
        Put(Covers() + 0xC0 + 0x58, std::uint16_t{1}); Put(Covers() + 0xC0 + 0x5C, 2U);
        Put(Markers() + 0x18, std::uint16_t{1});
        Put(Markers() + 0x24 + 0x18, std::uint16_t{1});
        Put(Markers() + 0x48 + 0x18, std::uint16_t{3});
        testing::Configure(Base(), &Engine);
    }
    ~NativeFixture() { Retire(); VirtualFree(image, 0, MEM_RELEASE); }
};
void NativeIdentity() {
    NativeFixture native;
    auto bytes = Cover();
    const auto base = native.Base(), world = native.World();
    Key key{};
    CHECK(Capture(base, world, 1, 0, key) == Result::MissingResource); // No late reconstruction.
    // Loading can precede dispatcher certification or occur on its own worker.
    onEngine = false;
    CHECK(testing::Load(world, 0, Level(), bytes));
    onEngine = true;
    CHECK(Capture(base, 0, 1, 2, key) == Result::Ok);
    CHECK(key.coverOrdinal == 3 && key.markerOrdinal == 0);
    Mapping mapping{};
    CHECK(Resolve(base, world, 1, key, mapping) == Result::Ok && mapping.marker == 2);
    CHECK(Revalidate(base, mapping) == Result::Ok);
    bool used = false;
    CHECK(UseResolved(base, world, 1, key, +[](const Mapping& value, void* context) noexcept {
        *static_cast<bool*>(context) = value.marker == 2 && value.load != 0; return true;
    }, &used) == Result::Ok && used);
    CHECK(UseResolved(base, world, 1, key, nullptr, nullptr) == Result::NativeUseRejected);
    std::int32_t marker{};
    CHECK(Resolve(base, world + 4, 1, key, marker) == Result::WrongWorld && marker == -1);
    CHECK(Resolve(base, world, 0, key, marker) == Result::InvalidGeneration);
    CHECK(Capture(base, world, 1, -1, key) == Result::InvalidOrdinal);
    CHECK(Capture(base, world, 1, 3, key) == Result::InvalidOrdinal);
    key = mapping.key;
    // Mutable ownership and resource availability do not alter content identity.
    Put(native.Markers() + 0x48 + 0x0C, 0xDEADBEEFU);
    Put(native.Markers() + 0x48 + 0x10, 19U); Put(native.Markers() + 0x48 + 0x14, 2U);
    Put(native.Covers() + 0xC0 + 0x54, 0U);
    CHECK(Revalidate(base, mapping) == Result::Ok);
    Put(native.Markers() + 0x48 + 0x18, std::uint16_t{1});
    CHECK(Revalidate(base, mapping) == Result::NativeMismatch);
    Put(native.Markers() + 0x48 + 0x18, std::uint16_t{3});
    Put(native.Markers() + 0x48, 9U);
    CHECK(Revalidate(base, mapping) == Result::NativeMismatch);
    Put(native.Markers() + 0x48, 0U);
    Put(native.World() + 0x23D8, static_cast<std::uint32_t>(native.Markers() + 4));
    CHECK(Revalidate(base, mapping) == Result::NativeMismatch);
    Put(native.World() + 0x23D8, static_cast<std::uint32_t>(native.Markers()));
    onEngine = false;
    CHECK(Resolve(base, world, 1, key, marker) == Result::WrongThread);
    onEngine = true;
    testing::Unload(world);
    CHECK(Revalidate(base, mapping) == Result::MissingResource);
    // Native allocation address/count/content ABA cannot resurrect old ticket.
    CHECK(testing::Load(world, 0, Level(), bytes));
    CHECK(Revalidate(base, mapping) == Result::StaleBinding);
    Mapping reloaded{};
    CHECK(Resolve(base, world, 1, key, reloaded) == Result::Ok);
    CHECK(reloaded.load != mapping.load);
    CHECK(testing::Load(world, 9, Level(), bytes));
    CHECK(Resolve(base, world, 1, key, marker) == Result::ContentMismatch);
    CHECK(Capture(base, world, 2, 2, key) == Result::Ok);
    CHECK(Resolve(base, world, 1, key, marker) == Result::InvalidGeneration);
    CHECK(Resolve(base, world, 2, key, marker) == Result::Ok && marker == 2);
    // Authored marker membership mismatch refuses binding before publication.
    Put(native.Covers() + 0xC0 + 0x5C, 1U);
    CHECK(!testing::Load(world, 9, Level(), bytes));
    CHECK(Resolve(base, world, 2, key, marker) == Result::MissingResource);
    Put(native.Covers() + 0xC0 + 0x5C, 2U);
    bytes.back() = std::byte{1}; // Same geometry, distinct complete content.
    CHECK(testing::Load(world, 9, Level(), bytes));
    CHECK(Resolve(base, world, 2, key, marker) == Result::ContentMismatch);
    CHECK(!testing::Load(world, 9, Level(), bytes, static_cast<std::uint32_t>(bytes.size() - 1)));
    CHECK(Capture(base, world, 2, 2, key) == Result::MissingResource);
    CHECK(!testing::Load(world, 9, Level(), bytes, UINT32_MAX, 1));
    CHECK(Capture(base, world, 2, 2, key) == Result::MissingResource);
    Retire();
    CHECK(Resolve(base, world, 2, key, marker) == Result::NativeLoaderBindingRequired);
}
#endif
} // namespace
int main(int argc, char** argv) {
    DigestVectors(); SerializedIdentity(); NativeReadiness();
#ifdef AIMP_MARKER_IDENTITY_TESTING
    if (argc > 1) Installation(argv[1]);
    NativeIdentity();
#else
    static_cast<void>(argc); static_cast<void>(argv);
#endif
    std::cout << "AISync marker identity: " << checks << " checks passed\n";
}
