#include "aimp/BuildProfile.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

namespace aimp {
namespace {

MaskedBytes Bytes(
    std::initializer_list<std::uint8_t> values,
    const char* mask) {
    return {std::vector<std::uint8_t>(values), mask};
}

SymbolSpec Code(
    const SymbolId id,
    const std::string_view name,
    const std::uint32_t rva,
    MaskedBytes expected,
    const std::size_t patchLength = 0,
    const bool relocationSafe = false) {
    return {id, name, rva, std::move(expected), patchLength, relocationSafe};
}

std::uint8_t HexNibble(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    return 0;
}

std::array<std::uint8_t, 32> Sha256(const std::string_view hex) noexcept {
    std::array<std::uint8_t, 32> result{};
    if (hex.size() != result.size() * 2) {
        return result;
    }
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(
            (HexNibble(hex[index * 2]) << 4U) |
            HexNibble(hex[index * 2 + 1]));
    }
    return result;
}

ProfileSectionIdentity ProfileSection(
    const std::string_view name,
    const std::uint32_t virtualAddress,
    const std::uint32_t virtualSize,
    const std::uint32_t rawOffset,
    const std::uint32_t rawSize,
    const std::uint32_t characteristics,
    const std::string_view sha256,
    std::vector<ProfileMutableRange> normalizedRanges = {}) noexcept {
    return {
        name,
        virtualAddress,
        virtualSize,
        rawOffset,
        rawSize,
        characteristics,
        Sha256(sha256),
        std::move(normalizedRanges)};
}

struct SectionCompatibility {
    bool layoutMatch{};
    bool protectedContentMatch{};
};

bool IsAsciiPathCharacter(const std::uint8_t value) noexcept {
    return (value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') ||
        value == '_' || value == '-' || value == '/';
}

bool ValidateMutableRange(
    const std::span<const std::byte> bytes,
    const ProfileMutableRangeKind kind) noexcept {
    if (kind != ProfileMutableRangeKind::ProductionPath) {
        return false;
    }
    constexpr std::string_view prefix = "PRODUCTION/";
    if (bytes.size() <= prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::to_integer<std::uint8_t>(bytes[index]) !=
            static_cast<std::uint8_t>(prefix[index])) {
            return false;
        }
    }
    bool terminated = false;
    for (std::size_t index = prefix.size(); index < bytes.size(); ++index) {
        const auto value = std::to_integer<std::uint8_t>(bytes[index]);
        if (value == 0) {
            terminated = true;
        } else if (!IsAsciiPathCharacter(value)) {
            return false;
        }
    }
    return terminated;
}

bool HashSha256(
    std::span<const std::byte> bytes,
    std::array<std::uint8_t, 32>& output);

SectionCompatibility EvaluateSectionCompatibility(
    const std::span<const std::byte> fileBytes,
    const PeIdentity& identity,
    const BuildProfile& profile) noexcept {
    try {
        SectionCompatibility result{};
        if (profile.sections.empty() ||
            identity.sections.size() != profile.sections.size()) {
            return result;
        }
        result.layoutMatch = true;
        result.protectedContentMatch = true;
        for (std::size_t index = 0;
             index < profile.sections.size();
             ++index) {
            const auto& expected = profile.sections[index];
            const auto& actual = identity.sections[index];
            if (actual.name != expected.name ||
                actual.virtualAddress != expected.virtualAddress ||
                actual.virtualSize != expected.virtualSize ||
                actual.rawOffset != expected.rawOffset ||
                actual.rawSize != expected.rawSize ||
                actual.characteristics != expected.characteristics) {
                result.layoutMatch = false;
                result.protectedContentMatch = false;
                return result;
            }
            if (actual.rawOffset > fileBytes.size() ||
                actual.rawSize > fileBytes.size() - actual.rawOffset) {
                result.protectedContentMatch = false;
                continue;
            }
            auto content = fileBytes.subspan(
                actual.rawOffset,
                actual.rawSize);
            std::vector<std::byte> normalized;
            if (!expected.normalizedRanges.empty()) {
                if ((expected.characteristics &
                     (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) != 0) {
                    result.protectedContentMatch = false;
                    continue;
                }
                normalized.assign(content.begin(), content.end());
                for (const auto& range : expected.normalizedRanges) {
                    if (range.size == 0 ||
                        range.offset > normalized.size() ||
                        range.size > normalized.size() - range.offset) {
                        result.protectedContentMatch = false;
                        break;
                    }
                    if (!ValidateMutableRange(
                            content.subspan(range.offset, range.size),
                            range.kind)) {
                        result.protectedContentMatch = false;
                        break;
                    }
                    std::fill_n(
                        normalized.begin() + range.offset,
                        range.size,
                        std::byte{0});
                }
                content = normalized;
            }
            std::array<std::uint8_t, 32> actualHash{};
            if (!HashSha256(content, actualHash) ||
                actualHash != expected.sha256) {
                result.protectedContentMatch = false;
            }
        }
        return result;
    } catch (...) {
        return {};
    }
}

bool NormalizedFileMatches(
    const std::span<const std::byte> fileBytes,
    const PeIdentity& identity,
    const BuildProfile& profile) noexcept {
    try {
        std::vector<std::byte> normalized(
            fileBytes.begin(),
            fileBytes.end());
        if (identity.sections.size() != profile.sections.size()) {
            return false;
        }
        for (std::size_t index = 0;
             index < profile.sections.size();
             ++index) {
            const auto& actual = identity.sections[index];
            const auto& expected = profile.sections[index];
            for (const auto& range : expected.normalizedRanges) {
                const auto offset =
                    static_cast<std::uint64_t>(actual.rawOffset) +
                    range.offset;
                if (range.size == 0 ||
                    offset > normalized.size() ||
                    range.size > normalized.size() - offset) {
                    return false;
                }
                std::fill_n(
                    normalized.begin() +
                        static_cast<std::size_t>(offset),
                    range.size,
                    std::byte{0});
            }
        }
        std::array<std::uint8_t, 32> hash{};
        return HashSha256(normalized, hash) &&
            hash == profile.normalizedSha256;
    } catch (...) {
        return false;
    }
}

bool ReadWholeFile(const std::wstring& path, std::vector<std::byte>& output) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    constexpr std::uint64_t kMaximumProfileInput = 512ULL * 1024ULL * 1024ULL;
    if (!GetFileSizeEx(file, &size) ||
        size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaximumProfileInput ||
        static_cast<std::uint64_t>(size.QuadPart) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        CloseHandle(file);
        return false;
    }

    output.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < output.size()) {
        const auto remaining = output.size() - offset;
        const auto request = static_cast<DWORD>(
            (std::min<std::size_t>)(remaining, 1024U * 1024U));
        DWORD read = 0;
        if (!ReadFile(file, output.data() + offset, request, &read, nullptr) ||
            read == 0) {
            CloseHandle(file);
            output.clear();
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

bool HashSha256(
    const std::span<const std::byte> bytes,
    std::array<std::uint8_t, 32>& output) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD resultBytes = 0;
    std::vector<std::uint8_t> object;

    if (BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0) < 0 ||
        BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes),
            sizeof(objectBytes),
            &resultBytes,
            0) < 0) {
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        return false;
    }

    object.resize(objectBytes);
    if (BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            static_cast<ULONG>(object.size()),
            nullptr,
            0,
            0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<ULONG>(
            (std::min<std::size_t>)(bytes.size() - offset, 1024U * 1024U));
        if (BCryptHashData(
                hash,
                reinterpret_cast<PUCHAR>(
                    const_cast<std::byte*>(bytes.data() + offset)),
                chunk,
                0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return false;
        }
        offset += chunk;
    }

    const bool success =
        BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

std::wstring CanonicalPath(const std::wstring& path) {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetFullPathNameW(
        path.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    if (length == 0 || length >= buffer.size()) {
        return path;
    }
    return std::wstring(buffer.data(), length);
}

std::optional<std::size_t> RvaToFileOffset(
    const std::uint32_t rva,
    const IMAGE_NT_HEADERS32& nt,
    const std::span<const IMAGE_SECTION_HEADER> sections,
    const std::size_t fileSize) noexcept {
    if (rva < nt.OptionalHeader.SizeOfHeaders && rva < fileSize) {
        return static_cast<std::size_t>(rva);
    }
    for (const auto& section : sections) {
        const auto spanSize =
            (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva < section.VirtualAddress ||
            rva - section.VirtualAddress >= spanSize) {
            continue;
        }
        const auto delta = rva - section.VirtualAddress;
        if (delta >= section.SizeOfRawData ||
            section.PointerToRawData > fileSize ||
            delta > fileSize - section.PointerToRawData) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(section.PointerToRawData) + delta;
    }
    return std::nullopt;
}

std::string ReadAsciiZ(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::size_t maximum) {
    if (offset >= bytes.size()) {
        return {};
    }
    std::string output;
    const auto available = (std::min)(maximum, bytes.size() - offset);
    output.reserve(available);
    for (std::size_t index = 0; index < available; ++index) {
        const auto value = std::to_integer<unsigned char>(bytes[offset + index]);
        if (value == 0) {
            return output;
        }
        if (value < 0x20U || value > 0x7EU) {
            return {};
        }
        output.push_back(static_cast<char>(value));
    }
    return {};
}

void ParseImports(
    const std::span<const std::byte> bytes,
    const IMAGE_NT_HEADERS32& nt,
    const std::span<const IMAGE_SECTION_HEADER> sections,
    PeIdentity& identity) {
    const auto& directory =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 ||
        directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        return;
    }
    const auto directoryOffset =
        RvaToFileOffset(directory.VirtualAddress, nt, sections, bytes.size());
    if (!directoryOffset.has_value()) {
        return;
    }

    std::set<std::string, std::less<>> unique;
    constexpr std::size_t kMaximumImportModules = 256;
    for (std::size_t index = 0; index < kMaximumImportModules; ++index) {
        const auto offset =
            *directoryOffset + index * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (offset > bytes.size() ||
            bytes.size() - offset < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            break;
        }
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        std::memcpy(&descriptor, bytes.data() + offset, sizeof(descriptor));
        if (descriptor.Name == 0 &&
            descriptor.FirstThunk == 0 &&
            descriptor.OriginalFirstThunk == 0) {
            break;
        }
        const auto nameOffset =
            RvaToFileOffset(descriptor.Name, nt, sections, bytes.size());
        if (!nameOffset.has_value()) {
            continue;
        }
        auto name = ReadAsciiZ(bytes, *nameOffset, 260);
        if (name.empty()) {
            continue;
        }
        std::transform(name.begin(), name.end(), name.begin(), [](const char value) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(value)));
        });
        unique.insert(std::move(name));
    }
    identity.imports.assign(unique.begin(), unique.end());
}

void ParseDebugDirectory(
    const std::span<const std::byte> bytes,
    const IMAGE_NT_HEADERS32& nt,
    const std::span<const IMAGE_SECTION_HEADER> sections,
    PeIdentity& identity) {
    const auto& directory =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    identity.hasDebugDirectory =
        directory.VirtualAddress != 0 && directory.Size >= sizeof(IMAGE_DEBUG_DIRECTORY);
    if (!identity.hasDebugDirectory) {
        return;
    }
    const auto offset =
        RvaToFileOffset(directory.VirtualAddress, nt, sections, bytes.size());
    if (!offset.has_value()) {
        return;
    }
    const auto count = (std::min<std::size_t>)(
        directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY),
        64);
    for (std::size_t index = 0; index < count; ++index) {
        const auto itemOffset = *offset + index * sizeof(IMAGE_DEBUG_DIRECTORY);
        if (itemOffset > bytes.size() ||
            bytes.size() - itemOffset < sizeof(IMAGE_DEBUG_DIRECTORY)) {
            break;
        }
        IMAGE_DEBUG_DIRECTORY item{};
        std::memcpy(&item, bytes.data() + itemOffset, sizeof(item));
        if (item.Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
            item.PointerToRawData > bytes.size() ||
            item.SizeOfData < 4 ||
            item.SizeOfData > bytes.size() - item.PointerToRawData) {
            continue;
        }
        std::array<char, 4> signature{};
        std::memcpy(signature.data(), bytes.data() + item.PointerToRawData, 4);
        if (signature == std::array<char, 4>{'R', 'S', 'D', 'S'} ||
            signature == std::array<char, 4>{'N', 'B', '1', '0'}) {
            identity.hasCodeView = true;
            return;
        }
    }
}

std::string QueryFileVersion(const std::wstring& path) {
    DWORD ignored = 0;
    const auto bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (bytes == 0 || bytes > 16U * 1024U * 1024U) {
        return {};
    }
    std::vector<std::byte> buffer(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, buffer.data())) {
        return {};
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoBytes = 0;
    if (!VerQueryValueW(
            buffer.data(),
            L"\\",
            reinterpret_cast<void**>(&info),
            &infoBytes) ||
        info == nullptr ||
        infoBytes < sizeof(VS_FIXEDFILEINFO) ||
        info->dwSignature != VS_FFI_SIGNATURE) {
        return {};
    }
    std::ostringstream output;
    output << HIWORD(info->dwFileVersionMS) << '.'
           << LOWORD(info->dwFileVersionMS) << '.'
           << HIWORD(info->dwFileVersionLS) << '.'
           << LOWORD(info->dwFileVersionLS);
    return output.str();
}

bool ParsePe(
    const std::span<const std::byte> bytes,
    PeIdentity& identity) noexcept {
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        return false;
    }
    const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
    if (ntOffset > bytes.size() ||
        bytes.size() - ntOffset < sizeof(IMAGE_NT_HEADERS32)) {
        return false;
    }

    IMAGE_NT_HEADERS32 nt{};
    std::memcpy(&nt, bytes.data() + ntOffset, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 96 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)) {
        return false;
    }

    const auto sectionOffset =
        ntOffset + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    if (sectionOffset > bytes.size() ||
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) >
            (bytes.size() - sectionOffset) / sizeof(IMAGE_SECTION_HEADER)) {
        return false;
    }
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    std::memcpy(
        sections.data(),
        bytes.data() + sectionOffset,
        sections.size() * sizeof(IMAGE_SECTION_HEADER));

    identity.machine = nt.FileHeader.Machine;
    identity.fileCharacteristics = nt.FileHeader.Characteristics;
    identity.timeDateStamp = nt.FileHeader.TimeDateStamp;
    identity.sizeOfImage = nt.OptionalHeader.SizeOfImage;
    identity.optionalHeaderMagic = nt.OptionalHeader.Magic;
    identity.preferredImageBase = nt.OptionalHeader.ImageBase;
    identity.entryPoint = nt.OptionalHeader.AddressOfEntryPoint;
    identity.sectionCount = nt.FileHeader.NumberOfSections;
    identity.hasRelocations =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]
                .VirtualAddress != 0 &&
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size != 0;

    identity.sections.clear();
    identity.sections.reserve(sections.size());
    for (const auto& section : sections) {
        PeSectionIdentity sectionIdentity{};
        const auto* nameBegin =
            reinterpret_cast<const char*>(section.Name);
        const auto nameLength = std::find(
            nameBegin,
            nameBegin + IMAGE_SIZEOF_SHORT_NAME,
            '\0') - nameBegin;
        sectionIdentity.name.assign(nameBegin, nameLength);
        sectionIdentity.virtualAddress = section.VirtualAddress;
        sectionIdentity.virtualSize = section.Misc.VirtualSize;
        sectionIdentity.rawOffset = section.PointerToRawData;
        sectionIdentity.rawSize = section.SizeOfRawData;
        sectionIdentity.characteristics = section.Characteristics;
        if (section.SizeOfRawData != 0) {
            if (section.PointerToRawData > bytes.size() ||
                section.SizeOfRawData > bytes.size() - section.PointerToRawData) {
                return false;
            }
            // Section digests enrich diagnostics but never determine PE validity.
            (void)HashSha256(
                bytes.subspan(
                    section.PointerToRawData,
                    section.SizeOfRawData),
                sectionIdentity.sha256);
        }
        identity.sections.push_back(std::move(sectionIdentity));
    }
    ParseImports(bytes, nt, sections, identity);
    ParseDebugDirectory(bytes, nt, sections, identity);
    return true;
}

} // namespace

bool MaskedBytes::IsValid() const noexcept {
    if (bytes.size() != mask.size()) {
        return false;
    }
    return std::all_of(mask.begin(), mask.end(), [](const char value) {
        return value == 'x' || value == '?';
    });
}

bool MaskedBytes::Matches(
    const std::span<const std::byte> candidate) const noexcept {
    if (!IsValid() || candidate.size() < bytes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (mask[index] == 'x' &&
            std::to_integer<std::uint8_t>(candidate[index]) != bytes[index]) {
            return false;
        }
    }
    return true;
}

const SymbolSpec* BuildProfile::Find(const SymbolId idToFind) const noexcept {
    const auto found = std::find_if(
        symbols.begin(),
        symbols.end(),
        [idToFind](const SymbolSpec& value) { return value.id == idToFind; });
    return found == symbols.end() ? nullptr : &*found;
}

bool IsProfileCompatible(
    const std::span<const std::byte> fileBytes,
    const PeIdentity& identity,
    const BuildProfile& profile) noexcept {
    const bool metadataMatch =
        identity.machine == profile.machine &&
        identity.fileCharacteristics == profile.fileCharacteristics &&
        identity.sectionCount == profile.sectionCount &&
        identity.timeDateStamp == profile.timeDateStamp &&
        identity.sizeOfImage == profile.sizeOfImage &&
        identity.fileSize == profile.fileSize &&
        identity.optionalHeaderMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        identity.preferredImageBase == profile.preferredImageBase &&
        identity.entryPoint == profile.entryPoint;
    if (!metadataMatch || fileBytes.size() != identity.fileSize) {
        return false;
    }
    const auto sections =
        EvaluateSectionCompatibility(fileBytes, identity, profile);
    return sections.layoutMatch &&
        sections.protectedContentMatch &&
        NormalizedFileMatches(fileBytes, identity, profile);
}

const BuildProfile& Retail2015Profile() {
    static const BuildProfile profile{
        "ai-retail-2015-02-17-sha256-00d6",
        14,
        {0x00, 0xD6, 0xBB, 0x84, 0x6A, 0xC5, 0xA7, 0x89,
         0x16, 0xBA, 0xF3, 0x63, 0xEB, 0x14, 0x8A, 0xA2,
         0x55, 0xA2, 0x3B, 0xF2, 0x16, 0xA2, 0x71, 0xCF,
         0xD2, 0x3A, 0x0E, 0xCE, 0x3C, 0x36, 0x83, 0x64},
        Sha256(
            "65CFD45C98E342A5C5348C700DF6B2B151891501FF4E1E2F5DBD62AC0A56F80B"),
        IMAGE_FILE_MACHINE_I386,
        0x0122,
        5,
        0x54E3199D,
        0x01F81000,
        0x00400000,
        0x00671E61,
        0x00001000,
        0x00D9D13B,
        21822464,
        {
            ProfileSection(
                ".text",
                0x00001000,
                0x00D9D13B,
                0x00000400,
                0x00D9D200,
                0x60000020,
                "97838C36709A8501CEF78EEC772F657345B0BE39AA88896C7EEF44A999CB32FD"),
            ProfileSection(
                ".rdata",
                0x00D9F000,
                0x004965CC,
                0x00D9D600,
                0x00496600,
                0x40000040,
                "C92979ED0B516D687E47DE5FE8BF002A84480A15CB3FFAE1F1FD00C2F2C7AE1D",
                {{0x00155D68, 32}}),
            ProfileSection(
                ".data",
                0x01236000,
                0x00B1DD28,
                0x01233C00,
                0x00070600,
                0xC0000040,
                "82739DE56065FFA82A57D7DAEF708E4448AF186525C87053C10989D1ED897CED"),
            ProfileSection(
                ".rsrc",
                0x01D54000,
                0x0001171C,
                0x012A4200,
                0x00011800,
                0x40000040,
                "AC722DD8B2951A9A490A4C133C694447D992FB12CC8C25D6196610CD27E87185"),
            ProfileSection(
                ".reloc",
                0x01D66000,
                0x0021A0CA,
                0x012B5A00,
                0x0021A200,
                0x42000040,
                "26B74C81F46A4384A923F6ED2DAA35D981DB5686E175DF797843F9C035780058"),
        },
        {
            Code(SymbolId::GlobalStateRoot, "global_state_root", 0x012F0C88, {}),
            Code(SymbolId::DescriptorListHead, "descriptor_list_head", 0x012F2DAC, {}),
            Code(SymbolId::NetzVtable, "netz_vtable", 0x00EF74FC, {}),
            Code(
                SymbolId::NetzConstructor,
                "netz_constructor",
                0x003E1CB0,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0xF8, 0x05, 0x13, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89},
                    "xxx????xxxxxxxxx"),
                7,
                true),
            Code(
                SymbolId::NetzDestructor,
                "netz_destructor",
                0x003E1D40,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0xF8, 0x05, 0x13, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89},
                    "xxx????xxxxxxxxx"),
                7,
                true),
            Code(
                SymbolId::NetzDispatch,
                "netz_dispatch",
                0x003DDE20,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC,
                     0x20, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x8B},
                    "xxxxxxxxxxxxxxxx"),
                6,
                true),
            Code(
                SymbolId::AllocateChannel,
                "allocate_channel",
                0x003DDC60,
                Bytes(
                    {0x51, 0x8B, 0xC1, 0x8B, 0x48, 0x08, 0x8D, 0x51,
                     0xFF, 0x33, 0xD1, 0x23, 0xD1, 0x0F, 0xBD, 0xCA},
                    "xxxxxxxxxxxxxxxx"),
                6,
                true),
            Code(
                SymbolId::ReactivateChannel,
                "reactivate_channel",
                0x003DDD10,
                Bytes(
                    {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24,
                     0x10, 0x2B, 0xCE, 0x83, 0xE9, 0x18, 0xB8, 0xAB},
                    "xxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::ReleaseChannel,
                "release_channel",
                0x003DDDB0,
                Bytes(
                    {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1,
                     0x8B, 0xCF, 0xE8, 0x11, 0xCE, 0x24, 0x00, 0x2B},
                    "xxxxxxxxxxxxxxxx"),
                6,
                true),
            Code(
                SymbolId::BuildDescriptorRegistry,
                "build_descriptor_registry",
                0x00389E20,
                Bytes(
                    {0xA1, 0xAC, 0x2D, 0x6F, 0x01, 0x33, 0xC9, 0x85,
                     0xC0, 0x74, 0x0C, 0xEB, 0x03, 0x8D, 0x49, 0x00},
                    "x????xxxxxxxxxxx")),
            Code(
                SymbolId::ParticipantBind,
                "participant_bind",
                0x003A37E0,
                Bytes(
                    {0x8B, 0x09, 0x8B, 0x91, 0xC4, 0x0A, 0x00, 0x00,
                     0x56, 0x33, 0xC0, 0x57, 0x85, 0xD2, 0x74, 0x30},
                    "xxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::ParticipantFindIdentity,
                "participant_find_identity",
                0x003A3890,
                Bytes(
                    {0x8B, 0x01, 0x53, 0x56, 0x8B, 0xB0, 0xC4, 0x0A,
                     0x00, 0x00, 0x33, 0xC9, 0x57, 0x85, 0xF6, 0x74},
                    "xxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::ParticipantRecordConstruct,
                "participant_record_construct",
                0x003A3990,
                Bytes(
                    {0x56, 0x8B, 0xF1, 0xE8, 0xF8, 0x4B, 0x2B, 0x00,
                     0x33, 0xD2, 0xC7, 0x86, 0xC8, 0x00, 0x00, 0x00,
                     0x01, 0x00, 0x00, 0x00},
                    "xxxx????xxxxxxxxxxxx")),
            Code(
                SymbolId::ParticipantRecordCopy,
                "participant_record_copy",
                0x00385540,
                Bytes(
                    {0x8B, 0x54, 0x24, 0x04, 0x8B, 0xC1, 0x53, 0x55,
                     0x56, 0x57, 0xB9, 0x32, 0x00, 0x00, 0x00, 0x8B,
                     0xF2, 0x8B, 0xF8, 0xF3, 0xA5},
                    "xxxxxxxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::ParticipantAppend,
                "participant_append",
                0x003941D0,
                Bytes(
                    {0x56, 0x8B, 0x74, 0x24, 0x08, 0xF6, 0x86, 0xC8,
                     0x00, 0x00, 0x00, 0x01, 0x74, 0x19, 0x8B, 0x86,
                     0xC8, 0x00, 0x00, 0x00, 0xD1, 0xE8, 0xF6, 0xD0},
                    "xxxxxxxxxxxxxxxxxxxxxxxx"),
                5,
                true),
            Code(
                SymbolId::ParticipantDetach,
                "participant_detach",
                0x003B0610,
                Bytes(
                    {0x8B, 0x11, 0x8B, 0x8A, 0xC4, 0x0A, 0x00, 0x00,
                     0x33, 0xC0, 0x85, 0xC9, 0x74, 0x5F, 0x56, 0x8B,
                     0xB2, 0xC0, 0x0A, 0x00, 0x00},
                    "xxxxxxxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::CharacterCreate,
                "character_create",
                0x003D0470,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0x73, 0xFC, 0x12, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89},
                    "xxx????xxxxxxxxx"),
                7,
                true),
            Code(
                SymbolId::CharacterDestroy,
                "character_destroy",
                0x003D0860,
                Bytes(
                    {0x6A, 0xFF, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,
                     0x68, 0xC1, 0x3F, 0x12, 0x01, 0x50, 0x64, 0x89},
                    "xxxxxxxxx????xxx"),
                8,
                true),
            Code(
                SymbolId::CharacterDestroyFull,
                "character_destroy_full",
                0x003D3D10,
                Bytes(
                    {0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x8B, 0x46,
                     0x64, 0xC6, 0x86, 0x03, 0x03, 0x00, 0x00, 0x00,
                     0x80, 0x78, 0x1C, 0x00, 0x57, 0x8B, 0xD9, 0x75},
                    "xxxxxxxxxxxxxxxxxxxxxxxx"),
                6,
                true),
            Code(
                SymbolId::MainThreadCharacterProcessing,
                "main_thread_character_processing",
                0x003CD750,
                Bytes(
                    {0xA1, 0x88, 0x0C, 0x6F, 0x01, 0x8B, 0x48, 0x08,
                     0x83, 0xEC, 0x48, 0x56, 0x57, 0x8D, 0x54, 0x24,
                     0x08, 0x52, 0xE8, 0x69, 0x8A, 0xFF, 0xFF, 0x8D,
                     0x7C, 0x84, 0x08},
                    "x????xxxxxxxxxxxxxx????xxxx"),
                5,
                true),
            Code(
                SymbolId::CharacterManagerUpdate,
                "character_manager_update",
                0x003CC080,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0x28, 0xDE, 0x12, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89,
                     0x25, 0x00, 0x00, 0x00, 0x00, 0x83, 0xEC, 0x0C,
                     0xF2, 0x0F, 0x10, 0x05, 0xC0, 0x10, 0x64, 0x01,
                     0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x87,
                     0x70, 0x43, 0x00, 0x00},
                    "xxx????xxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxx"),
                7,
                true),
            Code(
                SymbolId::CharacterReadinessProducer,
                "character_readiness_producer",
                0x003A76E0,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0x2B, 0xE3, 0x12, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89,
                     0x25, 0x00, 0x00, 0x00, 0x00, 0x83, 0xEC, 0x24,
                     0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x44, 0x57,
                     0x33, 0xFF, 0x89, 0x7C, 0x24, 0x14},
                    "xxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"),
                7,
                true),
            Code(
                SymbolId::CharacterReadinessPublisher,
                "character_readiness_publisher",
                0x00395780,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0x8B, 0xF9,
                     0xE8, 0x03, 0x4D, 0x29, 0x00, 0x33, 0xDB, 0x83},
                    "xxxxxxxxx????xxx"),
                5,
                true),
            Code(
                SymbolId::CharacterReadinessBarrier,
                "character_readiness_barrier",
                0x003D3A80,
                Bytes(
                    {0x51, 0x8B, 0x44, 0x24, 0x08, 0x53, 0x8B, 0x18,
                     0x56, 0x57, 0x8B, 0xF1, 0x8B, 0x8E, 0x54, 0x43},
                    "xxxxxxxxxxxxxxxx"),
                5,
                true),
            Code(
                SymbolId::CharacterOrchestrator,
                "character_orchestrator",
                0x003D3B50,
                Bytes(
                    {0x51, 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24,
                     0x18, 0x8D, 0x47, 0x28, 0x8B, 0xD9, 0x8B, 0x0D},
                    "xxxxxxxxxxxxxxxx"),
                5,
                true),
            Code(
                SymbolId::ReplicationContextConstruct,
                "replication_context_construct",
                0x003DE3A0,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0xD8, 0x04, 0x13, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89},
                    "xxx????xxxxxxxxx")),
            Code(
                SymbolId::ReplicationContextAllocate,
                "replication_context_allocate",
                0x003E1040,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0xA1, 0x27, 0x13, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89},
                    "xxx????xxxxxxxxx"),
                7,
                true),
            Code(
                SymbolId::ReplicationContextRelease,
                "replication_context_release",
                0x003DE700,
                Bytes(
                    {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x4C, 0x24, 0x0C,
                     0x8B, 0xD1, 0x2B, 0x57, 0x08, 0xB8, 0x79, 0x78},
                    "xxxxxxxxxxxxxxxx"),
                8,
                true),
            Code(
                SymbolId::EmptyRangeProvider,
                "empty_range_provider",
                0x0062AB80,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x33, 0xC9,
                     0x8D, 0x55, 0xF8, 0x83, 0xEC, 0x08, 0x89, 0x08},
                    "xxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::NeutralizedPublisher,
                "neutralized_publisher",
                0x0062ABB0,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x80, 0x7D, 0x0C, 0x00, 0x8D,
                     0x41, 0x2A, 0x75, 0x03, 0x8D, 0x41, 0x2B, 0x5D},
                    "xxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::ReceiveSpawnRequestF0,
                "receive_spawn_request_f0",
                0x00014A70,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x56, 0x57, 0xB8, 0x00, 0x01,
                     0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00},
                    "xxxxxxxxxxx????"),
                5,
                true),
            Code(
                SymbolId::ReceiveSpawnRequest130,
                "receive_spawn_request_130",
                0x00014C90,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x56, 0x57, 0xB8, 0x40, 0x01,
                     0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00},
                    "xxxxxxxxxxx????"),
                5,
                true),
            Code(
                SymbolId::CharacterRuntimeIdCounter,
                "character_runtime_id_counter",
                0x012AE64C,
                {}),
            Code(
                SymbolId::UiRegistryRoot,
                "ui_registry_root",
                0x0134A78C,
                {}),
            Code(
                SymbolId::UiFindNamedElement,
                "ui_find_named_element",
                0x00410420,
                Bytes(
                    {0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x8B, 0xF1,
                     0x57, 0x8D, 0x5E, 0x04, 0x53, 0x89, 0x5C, 0x24},
                    "xxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::PopupMessageEnqueue,
                "popup_message_enqueue",
                0x0040B1A0,
                Bytes(
                    {0x81, 0xEC, 0x28, 0x02, 0x00, 0x00, 0x55, 0x6A,
                     0xFF, 0x68, 0x00, 0x00, 0x00, 0x00, 0x8D, 0x84},
                    "xxxxxxxxxx????xx")),
            Code(
                SymbolId::CharacterDirectMove,
                "character_direct_move",
                0x0028D6D0,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45,
                     0x08, 0x83, 0xEC, 0x1C, 0x56, 0x8B, 0xF1, 0x50},
                    "xxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::CathodeFindNamedComposite,
                "cathode_find_named_composite",
                0x00522520,
                {}),
            Code(
                SymbolId::CathodeCreateEntity,
                "cathode_create_entity",
                0x00530950,
                {}),
            Code(
                SymbolId::CathodeFinalizeEntity,
                "cathode_finalize_entity",
                0x00524B60,
                {}),
            Code(
                SymbolId::CathodeSetEntityKind,
                "cathode_set_entity_kind",
                0x0062A900,
                {}),
            Code(
                SymbolId::CharacterEquipmentSystemRoot,
                "character_equipment_system_root",
                0x012F0DE8,
                {}),
            // Cheat Engine reports this as AI.exe.text+3A2040. BuildProfile
            // stores module RVAs, and retail .text starts at RVA 0x1000, so the
            // corresponding module RVA is 0x003A3040.
            Code(
                SymbolId::CharacterGiveWeaponToCharacter,
                "character_give_weapon_to_character",
                0x003A3040,
                {}),
            // Retail typed equipment conversion used by the downstream Give
            // chain. AI.exe VA 008B4B80 => module RVA 0x004B4B80.
            Code(
                SymbolId::CharacterResolveEquipmentInstance,
                "character_resolve_equipment_instance",
                0x004B4B80,
                {}),
            // Higher Character assignment primitive from AI.exe VA 007D7B70
            // (text+3D6B70). It takes CharacterBase directly and owns the
            // normal EquipmentManager assignment path.
            Code(
                SymbolId::CharacterAssignEquipmentInstance,
                "character_assign_equipment_instance",
                0x003D7B70,
                {}),
            // Retail NPC equipment presentation command recovered from the
            // Character equipment path. This is FUN_00654E60 (module RVA
            // 0x00254E60) and owns the normal select/holster animation path.
            Code(
                SymbolId::CathodeNpcEquipmentCommand,
                "cathode_npc_equipment_command",
                0x00254E60,
                Bytes(
                    {0x56, 0x8B, 0xF1, 0x57, 0x85, 0xF6, 0x74, 0x08,
                     0x8B, 0x06, 0x8B, 0x10, 0x6A, 0x01, 0xFF, 0xD2},
                    "xxxxxxxxxxxxxxxx")),
            // Shared Character equipment-controller intents recovered from retail
            // AI.exe. These are the highest confirmed action setters currently
            // used by both player/NPC paths before item-specific weapon logic.
            Code(
                SymbolId::CharacterPrimaryActionStart,
                "character_primary_action_start",
                0x00374AD0,
                {}),
            Code(
                SymbolId::CharacterPrimaryActionStop,
                "character_primary_action_stop",
                0x00374AE0,
                {}),
            Code(
                SymbolId::CharacterAimStart,
                "character_aim_start",
                0x00374AF0,
                {}),
            Code(
                SymbolId::CharacterAimStop,
                "character_aim_stop",
                0x00374B00,
                {}),
            // CharacterActionRuntime validates these code bytes independently
            // against the SHA-256-certified on-disk image (including PE relocation).
            // Only its functional hook owns the before/after equipment boundary.
            Code(SymbolId::CharacterEquipmentControllerUpdate,
                "character_equipment_controller_update", 0x003CCFB0, {}),
            Code(SymbolId::CharacterCurrentEquipmentItem,
                "character_current_equipment_item", 0x003829E0, {}),
            Code(SymbolId::CharacterActionFrameDelta,
                "character_action_frame_delta", 0x012410C0, {}),
            // FUN_007AD7E0: common NPC magazine/resource initialization, used
            // ONCE by Give after attachment. No weapon name/slot dispatch.
            // First 17 bytes from the supplied SHA-256-certified AI.exe;
            // this prefix has no image-base relocation.
            Code(SymbolId::CharacterItemInitializeNpcResources,
                "character_item_initialize_npc_resources", 0x003AD7E0,
                Bytes({0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x8B, 0x46,
                       0x28, 0x85, 0xC0, 0x0F, 0x84, 0x44, 0x01, 0x00, 0x00},
                      "xxxxxxxxxxxxxxxxx")),
            // Legacy ShootingRequest symbols remain for owned-reference cleanup.
            // ShootingRequest path used by the Character action probe.  The
            // request factory creates the native type-1 shooting request, the
            // activation helper inserts it into Character+0x34C0's manager. The
            // diagnostic trace may retain the local shared-ref temporarily;
            // FUN_0040D770 releases it and FUN_00471E90 cancels only on an
            // explicit post-trace recovery action.
            Code(
                SymbolId::CharacterShootingRequestCreate,
                "character_shooting_request_create",
                0x000707A0,
                {}),
            Code(
                SymbolId::CharacterShootingRequestActivate,
                "character_shooting_request_activate",
                0x00071E00,
                {}),
            Code(
                SymbolId::CharacterShootingRequestRelease,
                "character_shooting_request_release",
                0x0000D770,
                {}),
            Code(
                SymbolId::CharacterShootingRequestCancel,
                "character_shooting_request_cancel",
                0x00071E90,
                {}),
            Code(
                SymbolId::CathodeEntityManagerRoot,
                "cathode_entity_manager_root",
                0x0134EF40,
                {}),
            Code(
                SymbolId::CathodeResolveEntity,
                "cathode_resolve_entity",
                0x005236A0,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x81, 0x64,
                     0x13, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00},
                    "xxxxxx????xx????")),
            // Static C decompilation: AI.exe VA 008C3260 / module RVA
            // 004C3260 computes the exact hierarchy/context hash used as the
            // second DWORD of direct-child CATHODE RuntimeKeys by walking the
            // execution object's retained parent chain. PlayerSync uses this
            // instead of synthesizing CompositeInstanceId values.
            Code(
                SymbolId::CathodeExtractHierarchyContext,
                "cathode_extract_hierarchy_context",
                0x004C3260,
                {}),
            // Generic CATHODE runtime-key extractor at AI.exe VA 008C3360.
            // Static decompilation shows it emits {entityShortGuid, hierarchyHash}
            // from the object's +0x0C identity and retained +0x08 parent context.
            // PlayerSync still uses the equipment-specific semantic alias below
            // when this helper is applied to the validated WeaponPayload.
            Code(
                SymbolId::CathodeExtractRuntimeKey,
                "cathode_extract_runtime_key",
                0x004C3360,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1, 0x8B,
                     0x46, 0x08, 0x85, 0xC0, 0x74, 0x20, 0x8D, 0x4D},
                    "xxxxxxxxxxxxxxxx")),
            // PlayerSync-specific semantic alias for the same generic retail
            // RVA. In the proven WEAPON_GiveToCharacter path ECX is the real
            // WeaponPayload, whose +0x08 parent context and +0x0C AIMED_ITEM
            // identity produce the exact 8-byte ObjectRef consumed by 3A2040.
            Code(
                SymbolId::CharacterWeaponPayloadToObjectRef,
                "character_weapon_payload_to_object_ref",
                0x004C3360,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1, 0x8B,
                     0x46, 0x08, 0x85, 0xC0, 0x74, 0x20, 0x8D, 0x4D},
                    "xxxxxxxxxxxxxxxx")),
            Code(
                SymbolId::CathodeAssignEntityRef,
                "cathode_assign_entity_ref",
                0x000673C0,
                Bytes(
                    {0x55, 0x8B, 0x6C, 0x24, 0x08, 0x56, 0x8B, 0xF1,
                     0x3B, 0xEE, 0x74, 0x7D, 0x8B, 0x06, 0x57, 0x85},
                    "xxxxxxxxxxxxxxxx")),
            // Historical Fix3.2 helper addresses. Static RE confirms these are
            // retail work-item construction/retain/release helpers, but NOT that
            // any observed flags value is a portable gameplay semantic. Fix3.12
            // makes only FUN_00926FF0 hookable for a bounded read-only origin
            // probe; callback replay and retain/release synthesis remain dormant.
            Code(
                SymbolId::CathodeBuildCallbackValue,
                "cathode_build_callback_value",
                0x00526FF0,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x2F, 0x6A,
                     0x13, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00},
                    "xxxxxx????xx????"),
                5,
                true),
            Code(
                SymbolId::CathodeBuildRetainedWorkItem,
                "cathode_build_retained_work_item",
                0x00526D20,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x91, 0x69,
                     0x13, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00},
                    "xxxxxx????xx????")),
            Code(
                SymbolId::CathodeEnterWorkItemCriticalSection,
                "cathode_enter_work_item_critical_section",
                0x0051E1B0,
                Bytes(
                    {0x56, 0x68, 0x00, 0x00, 0x00, 0x00, 0x8B, 0xF1,
                     0xFF, 0x15, 0x00, 0x00, 0x00, 0x00},
                    "xx????xxxx????")),
            Code(
                SymbolId::CathodeLeaveWorkItemCriticalSection,
                "cathode_leave_work_item_critical_section",
                0x0051E1D0,
                Bytes(
                    {0x68, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x15,
                     0x00, 0x00, 0x00, 0x00, 0xC3},
                    "x????xx????x")),
            Code(
                SymbolId::CathodeRetainCallbackValue,
                "cathode_retain_callback_value",
                0x0051E1E0,
                {}),
            Code(
                SymbolId::CathodeReleaseCallbackValue,
                "cathode_release_callback_value",
                0x005252F0,
                {}),
            Code(
                SymbolId::CathodeProviderRegistryRoot,
                "cathode_provider_registry_root",
                0x01354678,
                {}),
            Code(
                SymbolId::CathodeDispatchInput,
                "cathode_dispatch_input",
                0x0051D9F0,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0x75, 0x10,
                     0x8B, 0x06, 0x57, 0x85, 0xC0, 0x74, 0x05, 0x8B},
                    "xxxxxxxxxxxxxxxx")),
            // Fix3.27: the existing STATIC CONFIRMED FUN_00921F70 ABI is
            // now also hookable. The first six bytes are three complete,
            // position-independent instructions (push ebp; mov ebp,esp;
            // mov eax,[ebp+0C]); do not shorten this prefix to five bytes.
            Code(
                SymbolId::CathodeBuildInputTarget,
                "cathode_build_input_target",
                0x00521F70,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0xDD, 0x45,
                     0x10, 0x8B, 0x55, 0x08, 0x56, 0x8B, 0xF1, 0x8B},
                    "xxxxxxxxxxxxxxxx"),
                6,
                true),
            Code(
                SymbolId::CathodeQueueInput,
                "cathode_queue_input",
                0x005271C0,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x68, 0x6A,
                     0x13, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00},
                    "xxxxxx????xx????"),
                5,
                true),
            Code(
                SymbolId::CathodeQueueTypedMessage,
                "cathode_queue_typed_message",
                0x00530640,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x10, 0x77,
                     0x13, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00},
                    "xxxxxx????xx????"),
                5,
                true),
            // WS3: image RVAs, NOT offsets relative to .text. All four
            // prefixes are push ebp; mov ebp,esp; push -1 (5 full bytes).
            Code(SymbolId::CathodeOutputPropagation, "cathode_output_propagation",
                0x004BE0D0, Bytes({0x55,0x8B,0xEC,0x6A,0xFF,0x68,0xA0,0x4F,0x13,0x01,
                    0x64,0xA1,0,0,0,0}, "xxxxxx????xxxxxx"), 5, true),
            Code(SymbolId::CathodeSchedulerDrain, "cathode_scheduler_drain",
                0x0052CE20, Bytes({0x55,0x8B,0xEC,0x6A,0xFF,0x68,0xB0,0x71,0x13,0x01,
                    0x64,0xA1,0,0,0,0}, "xxxxxx????xxxxxx"), 5, true),
            Code(SymbolId::CathodePublicCompositeAction, "cathode_public_composite_action",
                0x00565920, Bytes({0x55,0x8B,0xEC,0x6A,0xFF,0x68,0xF8,0x9D,0x13,0x01,
                    0x64,0xA1,0,0,0,0}, "xxxxxx????xxxxxx"), 5, true),
            Code(SymbolId::CathodeWorkItemBuild, "cathode_work_item_build",
                0x005265C0, Bytes({0x55,0x8B,0xEC,0x6A,0xFF,0x68,0x03,0x69,0x13,0x01,
                    0x64,0xA1,0,0,0,0}, "xxxxxx????xxxxxx"), 5, true),
            Code(SymbolId::CathodeWorkItemExecute, "cathode_work_item_execute",
                0x00526680, Bytes({0x55,0x8B,0xEC,0x6A,0xFF,0x68,0x30,0x69,0x13,0x01,
                    0x64,0xA1,0,0,0,0}, "xxxxxx????xxxxxx"), 5, true),
            Code(SymbolId::CathodeWorkItemDestroy, "cathode_work_item_destroy",
                0x00522800, Bytes({0x55,0x8B,0xEC,0x6A,0xFF,0x68,0x43,0x63,0x13,0x01,
                    0x64,0xA1,0,0,0,0}, "xxxxxx????xxxxxx"), 5, true),
            // STATIC CONFIRMED by the dormant targeted WorldSync path against
            // the same retail executable. v13.3 promotes this constructor to
            // the strict build profile so generic LogicSwitch WorldState can
            // retain the real raw runtime object without a Door-specific
            // ShortGuid filter.
            Code(
                SymbolId::CathodeLogicSwitchConstructor,
                "cathode_logic_switch_constructor",
                0x001B4BB0,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0xA0, 0x21, 0x12, 0x01},
                    "xxx????"),
                7,
                true),
            Code(SymbolId::CathodeLogicSwitchVtable, "cathode_logic_switch_vtable", 0x00E62880, {}),
            Code(
                SymbolId::CathodeLogicSwitchSet,
                "cathode_logic_switch_set",
                0x001B4D20,
                Bytes(
                    {0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x0C, 0x00},
                    "xxxxxxx"),
                7,
                true),
            Code(
                SymbolId::CathodeLogicSwitchClear,
                "cathode_logic_switch_clear",
                0x001B4D60,
                Bytes(
                    {0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x0C, 0x01},
                    "xxxxxxx"),
                7,
                true),
            Code(
                SymbolId::CathodeReleaseInputTarget,
                "cathode_release_input_target",
                0x000BF890,
                Bytes(
                    {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x08, 0x85, 0xC0,
                     0x74, 0x51, 0x8B, 0x40, 0x04, 0x83, 0xF8, 0x01},
                    "xxxxxxxxxxxxxxxx")),
            // v12.7 repairs the v12.6 class-vtable evidence. These RVAs are the
            // exact vtables assigned by the corresponding retail constructors
            // in AI.exe.c; the previous v12.6 values pointed at unrelated .rdata.
            Code(SymbolId::CathodeInteractionVtable, "cathode_interaction_vtable", 0x00E4268C, {}),
            Code(SymbolId::CathodeWorldPickupVtable, "cathode_world_pickup_vtable", 0x00E4263C, {}),
            Code(SymbolId::CathodeRewireSystemVtable, "cathode_rewire_system_vtable", 0x00DF9428, {}),
            Code(SymbolId::CathodeRewireLocationVtable, "cathode_rewire_location_vtable", 0x00DF9450, {}),
            Code(SymbolId::CathodeRewireAccessPointVtable, "cathode_rewire_access_point_vtable", 0x00DF94A0, {}),
            Code(SymbolId::CathodeRewireTotalPowerVtable, "cathode_rewire_total_power_vtable", 0x00DF9478, {}),
            Code(SymbolId::CathodeRewireVtable, "cathode_rewire_vtable", 0x00DF94C8, {}),
            Code(SymbolId::CathodeSplinePathVtable, "cathode_spline_path_vtable", 0x00DC8A48, {}),
            Code(SymbolId::CathodeEnvironmentModelReferenceVtable, "cathode_environment_model_reference_vtable", 0x00DC8A70, {}),
            Code(SymbolId::CathodeAnimatedModelAttachmentNodeVtable, "cathode_animated_model_attachment_node_vtable", 0x00DC8A98, {}),
            Code(SymbolId::CathodePlayEnvironmentAnimationVtable, "cathode_play_environment_animation_vtable", 0x00DC8AC0, {}),
            Code(SymbolId::CathodeDoorStatusVtable, "cathode_door_status_vtable", 0x00DC8B10, {}),
            Code(SymbolId::CathodeDoorNativeVtable, "cathode_door_native_vtable", 0x00DE0138, {}),
            Code(
                SymbolId::CathodeDoorStartedOpening,
                "cathode_door_started_opening",
                0x004CF7C0,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0xDD, 0x45, 0x10, 0x8B, 0x45,
                     0x0C, 0x8B, 0x55, 0x08, 0x83, 0xEC, 0x08, 0xDD},
                    "xxxxxxxxxxxxxxxx"),
                6,
                true),
            Code(
                SymbolId::CathodeDoorStartedClosing,
                "cathode_door_started_closing",
                0x004CF7F0,
                Bytes(
                    {0x55, 0x8B, 0xEC, 0xDD, 0x45, 0x10, 0x8B, 0x45,
                     0x0C, 0x8B, 0x55, 0x08, 0x83, 0xEC, 0x08, 0xDD},
                    "xxxxxxxxxxxxxxxx"),
                6,
                true),
            Code(
                SymbolId::CathodeDoorRequestLock,
                "cathode_door_request_lock",
                0x000E8D10,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0x18, 0x23, 0x12, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89},
                    "xxx????xx????xxx"),
                7,
                true),
            Code(
                SymbolId::CathodeDoorRequestUnlock,
                "cathode_door_request_unlock",
                0x000E8E10,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0x18, 0x23, 0x12, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89},
                    "xxx????xx????xxx"),
                7,
                true),
            Code(
                SymbolId::CathodeDoorForceOpen,
                "cathode_door_force_open",
                0x000E8F20,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0x18, 0x23, 0x12, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89},
                    "xxx????xx????xxx"),
                7,
                true),
            Code(
                SymbolId::CathodeDoorForceClose,
                "cathode_door_force_close",
                0x000E9040,
                Bytes(
                    {0x6A, 0xFF, 0x68, 0xC0, 0x20, 0x12, 0x01, 0x64,
                     0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89},
                    "xxx????xx????xxx"),
                7,
                true),
        }};
    return profile;
}

BuildValidation ValidateExecutable(
    const std::wstring& path,
    const BuildProfile& profile) {
    BuildValidation result{};
    std::vector<std::byte> file;
    if (!ReadWholeFile(path, file)) {
        result.reason = "unable_to_read_executable";
        return result;
    }

    result.fileRead = true;
    result.actual.canonicalPath = CanonicalPath(path);
    result.actual.fileSize = file.size();
    // Hashing is best-effort provenance data and cannot reject an AI.exe.
    result.hashAvailable = HashSha256(file, result.actual.sha256);
    if (!ParsePe(file, result.actual)) {
        result.reason = "invalid_pe32";
        return result;
    }

    result.peValid = true;
    auto fileName = result.actual.canonicalPath;
    const auto separator = fileName.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
        fileName.erase(0, separator + 1);
    }
    std::transform(
        fileName.begin(),
        fileName.end(),
        fileName.begin(),
        [](const wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    const auto entrySection = std::find_if(
        result.actual.sections.begin(),
        result.actual.sections.end(),
        [&](const PeSectionIdentity& section) {
            const auto sectionSize =
                (std::max)(section.virtualSize, section.rawSize);
            return sectionSize != 0 &&
                result.actual.entryPoint >= section.virtualAddress &&
                result.actual.entryPoint - section.virtualAddress <
                    sectionSize &&
                (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        });
    const auto hasImport = [&](const std::string_view name) {
        return std::find(
            result.actual.imports.begin(),
            result.actual.imports.end(),
            name) != result.actual.imports.end();
    };
    const bool alienImports =
        hasImport("kernel32.dll") &&
        (hasImport("d3d11.dll") || hasImport("d3d9.dll"));
    result.validCandidate =
        fileName == L"ai.exe" &&
        result.actual.machine == IMAGE_FILE_MACHINE_I386 &&
        result.actual.optionalHeaderMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        (result.actual.fileCharacteristics & IMAGE_FILE_DLL) == 0 &&
        result.actual.sizeOfImage != 0 &&
        result.actual.entryPoint != 0 &&
        result.actual.entryPoint < result.actual.sizeOfImage &&
        entrySection != result.actual.sections.end() &&
        alienImports;
    result.actual.fileVersion = QueryFileVersion(path);
    result.hashMatch =
        result.hashAvailable && result.actual.sha256 == profile.sha256;
    result.metadataMatch =
        result.actual.machine == profile.machine &&
        result.actual.fileCharacteristics == profile.fileCharacteristics &&
        result.actual.sectionCount == profile.sectionCount &&
        result.actual.timeDateStamp == profile.timeDateStamp &&
        result.actual.sizeOfImage == profile.sizeOfImage &&
        result.actual.fileSize == profile.fileSize &&
        result.actual.optionalHeaderMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        result.actual.preferredImageBase == profile.preferredImageBase &&
        result.actual.entryPoint == profile.entryPoint;
    const auto sectionCompatibility =
        EvaluateSectionCompatibility(file, result.actual, profile);
    result.sectionLayoutMatch = sectionCompatibility.layoutMatch;
    result.protectedSectionsMatch =
        sectionCompatibility.protectedContentMatch;
    result.normalizedHashMatch =
        result.sectionLayoutMatch &&
        result.protectedSectionsMatch &&
        NormalizedFileMatches(file, result.actual, profile);
    result.profileCompatible =
        result.metadataMatch &&
        result.sectionLayoutMatch &&
        result.protectedSectionsMatch &&
        result.normalizedHashMatch;
    result.recognized = result.hashMatch && result.profileCompatible;
    if (result.recognized) {
        result.reason = "exact_profile_match";
    } else if (result.profileCompatible) {
        result.reason = "local_profile_compatible_launch_slot_change";
    } else if (result.validCandidate) {
        result.reason = "ai_executable_accepted_hash_is_diagnostic_only";
    } else if (!result.hashAvailable) {
        result.reason = "invalid_ai_executable_sha256_unavailable";
    } else if (!result.hashMatch && !result.metadataMatch) {
        result.reason = "hash_and_pe_metadata_mismatch";
    } else if (!result.hashMatch) {
        result.reason = "sha256_mismatch";
    } else {
        result.reason = "pe_metadata_mismatch";
    }
    return result;
}

bool IsFixedRvaEligible(const BuildValidation& validation) noexcept {
    return validation.validCandidate &&
        validation.metadataMatch &&
        validation.sectionLayoutMatch;
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::string Sha256Hex(const std::array<std::uint8_t, 32>& bytes) {
    std::string output(bytes.size() * 2, '0');
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        output[index * 2] = kHex[(bytes[index] >> 4U) & 0x0FU];
        output[index * 2 + 1] = kHex[bytes[index] & 0x0FU];
    }
    return output;
}

} // namespace aimp
